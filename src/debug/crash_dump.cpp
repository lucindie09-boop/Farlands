#include "debug/crash_dump.hpp"

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <dbghelp.h>
#include <tlhelp32.h>
#pragma comment(lib, "dbghelp.lib")

namespace VoxelEngine {
namespace debug {
namespace {

// Where the report and the dump go, and the prefix to give them. Resolved once,
// at install time: after a fault there is no safe place left to look anything up
// (and the process is about to die, so the less work the better).
wchar_t g_report_path[MAX_PATH] = {};
wchar_t g_dump_path[MAX_PATH] = {};
wchar_t g_first_chance_path[MAX_PATH] = {};
HANDLE g_stderr = INVALID_HANDLE_VALUE;
DWORD g_main_thread = 0;
bool g_installed = false;
// First-chance reporting: one write at a time (a fault that faults again must not
// recurse), and a hard cap, so a process that throws access violations around for
// its own reasons cannot fill the disk.
volatile LONG g_first_chance_busy = 0;
int g_first_chance_writes = 0;
constexpr int kMaxFirstChanceWrites = 8;

constexpr int kMaxFrames = 80;
// Raw stack words to scan when the unwinder has nothing to offer, and how many
// lines of it to print.
constexpr size_t kStackWords = 512;
constexpr int kMaxRawLines = 48;
// Big enough for a readable name, small enough to live on the stack: the handler
// runs on a stack that is suspect by definition.
constexpr int kBuffer = 1024;

void write_text(HANDLE file, const wchar_t* text) {
    if (file == INVALID_HANDLE_VALUE || text == nullptr) return;
    const int length = lstrlenW(text);
    if (length <= 0) return;
    // UTF-8, so the report opens in any editor and pastes into a terminal.
    char utf8[kBuffer * 2];
    const int n = WideCharToMultiByte(CP_UTF8, 0, text, length, utf8, sizeof(utf8) - 1, nullptr, nullptr);
    if (n <= 0) return;
    utf8[n] = '\0';
    DWORD written = 0;
    WriteFile(file, utf8, static_cast<DWORD>(n), &written, nullptr);
}

// Every line goes to the report AND to stderr, so a console run or the editor's
// Output panel shows the whole thing without anyone hunting for a file.
void report(HANDLE file, const wchar_t* format, ...) {
    wchar_t buffer[kBuffer];
    va_list args;
    va_start(args, format);
    _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, format, args);
    va_end(args);
    write_text(file, buffer);
    write_text(file, L"\r\n");
    if (g_stderr != INVALID_HANDLE_VALUE) {
        write_text(g_stderr, buffer);
        write_text(g_stderr, L"\r\n");
    }
}

// The name of the module an address lives in, and its offset, which is what
// identifies a frame when no symbols are around.
void describe_address(DWORD64 address, wchar_t* module, size_t module_size, DWORD64* offset) {
    lstrcpynW(module, L"?", static_cast<int>(module_size));
    *offset = address;
    if (address == 0) return;
    HMODULE base = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &base) == FALSE) {
        return;
    }
    wchar_t path[MAX_PATH] = {};
    if (GetModuleFileNameW(base, path, MAX_PATH) == 0) return;
    const wchar_t* name = path;
    for (const wchar_t* at = path; *at != L'\0'; ++at) {
        if (*at == L'\\' || *at == L'/') name = at + 1;
    }
    lstrcpynW(module, name, static_cast<int>(module_size));
    *offset = address - reinterpret_cast<DWORD64>(base);
}

// Names a symbol for an address, or leaves the buffer empty.
void describe_symbol(DWORD64 address, wchar_t* out, size_t out_size, DWORD64* displacement) {
    out[0] = L'\0';
    *displacement = 0;
    HANDLE process = GetCurrentProcess();
    constexpr int kNameChars = 512;
    BYTE storage[sizeof(SYMBOL_INFOW) + kNameChars * sizeof(wchar_t)] = {};
    SYMBOL_INFOW* symbol = reinterpret_cast<SYMBOL_INFOW*>(storage);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFOW);
    symbol->MaxNameLen = kNameChars;
    if (SymFromAddrW(process, address, displacement, symbol) == FALSE) return;
    lstrcpynW(out, symbol->Name, static_cast<int>(out_size));

    IMAGEHLP_LINEW64 line = {};
    line.SizeOfStruct = sizeof(line);
    DWORD line_displacement = 0;
    if (SymGetLineFromAddrW64(process, address, &line_displacement, &line) != FALSE) {
        wchar_t file[MAX_PATH] = {};
        const wchar_t* bare = line.FileName;
        for (const wchar_t* at = line.FileName; *at != L'\0'; ++at) {
            if (*at == L'\\' || *at == L'/') bare = at + 1;
        }
        _snwprintf_s(file, _countof(file), _TRUNCATE, L" (%s:%lu)", bare,
                     static_cast<unsigned long>(line.LineNumber));
        const size_t used = lstrlenW(out);
        lstrcpynW(out + used, file, static_cast<int>(out_size - used));
    }
}

// The heap manager's "this block is corrupt" status, and the two shapes it
// arrives in: as itself (the fatal path), or as a breakpoint whose first
// parameter is that status (the shape a debugger reports as first chance).
constexpr DWORD kStatusHeapCorruption = 0xC0000374u;

[[nodiscard]] bool is_heap_corruption(const EXCEPTION_RECORD* record) {
    if (record->ExceptionCode == kStatusHeapCorruption) return true;
    return record->ExceptionCode == EXCEPTION_BREAKPOINT && record->NumberParameters >= 1 &&
           record->ExceptionInformation[0] == kStatusHeapCorruption;
}

// Names a module and when it was linked. A folder of reports from several builds
// cannot be read without this: an offset in one build means nothing in another.
void report_build(HANDLE file, const wchar_t* label, HMODULE module) {
    wchar_t path[MAX_PATH] = {};
    if (module == nullptr || GetModuleFileNameW(module, path, MAX_PATH) == 0) {
        report(file, L"%s: (unknown)", label);
        return;
    }
    const wchar_t* name = path;
    for (const wchar_t* at = path; *at != L'\0'; ++at) {
        if (*at == L'\\' || *at == L'/') name = at + 1;
    }
    DWORD stamp = 0;
    const auto* base = reinterpret_cast<const BYTE*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic == IMAGE_DOS_SIGNATURE) {
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        if (nt->Signature == IMAGE_NT_SIGNATURE) stamp = nt->FileHeader.TimeDateStamp;
    }
    const time_t seconds = static_cast<time_t>(stamp);
    tm utc = {};
    if (stamp != 0 && gmtime_s(&utc, &seconds) == 0) {
        report(file, L"%s: %s  built %04d-%02d-%02d %02d:%02d:%02d UTC", label, name,
               utc.tm_year + 1900, utc.tm_mon + 1, utc.tm_mday, utc.tm_hour, utc.tm_min, utc.tm_sec);
    } else {
        report(file, L"%s: %s  (no build stamp)", label, name);
    }
}

// Which binary is reporting, and which run of it.
void write_provenance(HANDLE file) {
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&install_crash_dump_handler), &self) != FALSE) {
        report_build(file, L"our build ", self);
    }
    report_build(file, L"host exe  ", GetModuleHandleW(nullptr));
    FILETIME created = {}, exited = {}, kernel = {}, user = {};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) != FALSE) {
        SYSTEMTIME start = {};
        if (FileTimeToSystemTime(&created, &start) != FALSE) {
            report(file, L"started   %04d-%02d-%02d %02d:%02d:%02d UTC", start.wYear, start.wMonth,
                   start.wDay, start.wHour, start.wMinute, start.wSecond);
        }
    }
}

// Walked stack, via the unwinder. Returns how many frames it managed to name —
// which can be zero: a fault inside an ntdll routine (a memcpy, say) has no
// unwind information we can use, and that is exactly when the raw scan below is
// what saves the report.
int write_stack(HANDLE file, CONTEXT* context) {
    HANDLE process = GetCurrentProcess();
    HANDLE thread = GetCurrentThread();
    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = context->Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context->Rsp;
    frame.AddrStack.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context->Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;

    int frames = 0;
    for (int i = 0; i < kMaxFrames; ++i) {
        if (StackWalk64(IMAGE_FILE_MACHINE_AMD64, process, thread, &frame, context, nullptr,
                        SymFunctionTableAccess64, SymGetModuleBase64, nullptr) == FALSE) {
            break;
        }
        const DWORD64 address = frame.AddrPC.Offset;
        if (address == 0) break;
        wchar_t module[MAX_PATH] = {};
        DWORD64 offset = 0;
        describe_address(address, module, _countof(module), &offset);
        wchar_t symbol[kBuffer] = {};
        DWORD64 displacement = 0;
        describe_symbol(address, symbol, _countof(symbol), &displacement);
        report(file, L"  %02d  %s+0x%llx  %s", i, module, static_cast<unsigned long long>(offset),
               symbol[0] != L'\0' ? symbol : L"(no symbol)");
        ++frames;
    }
    return frames;
}

// The stack the hard way: scan it for anything that points into a module. It
// needs no unwind data and no symbols, so it works on precisely the crashes the
// unwinder gives up on. Data values and stale locals make it noisy, which is why
// it only prints what resolves to a module and stops after a screenful.
void write_raw_stack(HANDLE file, CONTEXT* context) {
    const uintptr_t* sp = reinterpret_cast<const uintptr_t*>(context->Rsp);
    if (sp == nullptr || context->Rsp == 0) return;
    MEMORY_BASIC_INFORMATION info = {};
    if (VirtualQuery(sp, &info, sizeof(info)) == 0) return;
    const uintptr_t start = reinterpret_cast<uintptr_t>(sp);
    const uintptr_t region_end = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
    if (info.State != MEM_COMMIT || region_end <= start) return;
    size_t words = (region_end - start) / sizeof(uintptr_t);
    if (words > kStackWords) words = kStackWords;

    int printed = 0;
    for (size_t i = 0; i < words && printed < kMaxRawLines; ++i) {
        const uintptr_t value = sp[i];
        if (value < 0x10000) continue;
        wchar_t module[MAX_PATH] = {};
        DWORD64 offset = 0;
        describe_address(value, module, _countof(module), &offset);
        if (module[0] == L'?') continue;
        wchar_t symbol[kBuffer] = {};
        DWORD64 displacement = 0;
        describe_symbol(value, symbol, _countof(symbol), &displacement);
        report(file, L"  +0x%04llx  %s+0x%llx  %s",
               static_cast<unsigned long long>(i * sizeof(uintptr_t)), module,
               static_cast<unsigned long long>(offset), symbol[0] != L'\0' ? symbol : L"");
        ++printed;
    }
    if (printed == 0) report(file, L"  (nothing on the stack pointed into a module)");
}

void write_module_list(HANDLE file) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE) return;
    MODULEENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry) != FALSE) {
        do {
            report(file, L"  %-44s 0x%llx +0x%llx", entry.szModule,
                   static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(entry.modBaseAddr)),
                   static_cast<unsigned long long>(entry.modBaseSize));
        } while (Module32NextW(snapshot, &entry) != FALSE);
    }
    CloseHandle(snapshot);
}

// Answers the bytes written, or 0, with the last error in `error_out`.
DWORD64 write_dump(EXCEPTION_POINTERS* pointers, const wchar_t* path, DWORD* error_out) {
    *error_out = 0;
    HANDLE dump = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (dump == INVALID_HANDLE_VALUE) {
        *error_out = GetLastError();
        return 0;
    }
    MINIDUMP_EXCEPTION_INFORMATION info = {};
    info.ThreadId = GetCurrentThreadId();
    info.ExceptionPointers = pointers;
    info.ClientPointers = FALSE;
    const MINIDUMP_TYPE rich = static_cast<MINIDUMP_TYPE>(
        MiniDumpWithThreadInfo | MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory);
    BOOL ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, rich, &info,
                                nullptr, nullptr);
    if (ok == FALSE) {
        // The richer flavours read the process's memory and can fail for exactly
        // the reason it crashed; the plain one needs much less.
        *error_out = GetLastError();
        SetFilePointer(dump, 0, nullptr, FILE_BEGIN);
        SetEndOfFile(dump);
        ok = MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump, MiniDumpNormal,
                               &info, nullptr, nullptr);
        if (ok != FALSE) *error_out = 0;
        else *error_out = GetLastError();
    }
    LARGE_INTEGER size = {};
    GetFileSizeEx(dump, &size);
    CloseHandle(dump);
    return static_cast<DWORD64>(size.QuadPart);
}

// The body of a report, shared by both handlers. `kind` says which one caught
// it: "unhandled" is the fatal path, "first chance" is the fault arriving while
// unwinding still exists (and is the only version a stack overflow can produce,
// since running a last-chance filter on a blown stack is not something to count
// on).
void write_report(HANDLE file, EXCEPTION_POINTERS* pointers, const wchar_t* kind) {
    {
        const EXCEPTION_RECORD* record = pointers->ExceptionRecord;
        const CONTEXT* context = pointers->ContextRecord;
        const DWORD code = record->ExceptionCode;
        const DWORD64 instruction = reinterpret_cast<DWORD64>(record->ExceptionAddress);
        const bool is_access_violation = (code == EXCEPTION_ACCESS_VIOLATION);
        const bool heap_corruption = is_heap_corruption(record);
        const DWORD thread_id = GetCurrentThreadId();

        {
            wchar_t module[MAX_PATH] = {};
            DWORD64 offset = 0;
            describe_address(instruction, module, _countof(module), &offset);
            report(file, L"Farlands crash report (%s)", kind);
            report(file, L"exception 0x%08lX%s", static_cast<unsigned long>(code),
                   is_access_violation ? L" (access violation)"
                   : code == EXCEPTION_STACK_OVERFLOW ? L" (stack overflow)"
                   : heap_corruption ? L" (heap corruption)"
                   : code == 0xC0000409u ? L" (fail fast: stack buffer overrun)"
                   : L"");
            if (is_access_violation && record->NumberParameters >= 2) {
                const bool was_write = record->ExceptionInformation[0] != 0;
                report(file, L"tried to %s memory at 0x%llx", was_write ? L"WRITE" : L"READ",
                       static_cast<unsigned long long>(record->ExceptionInformation[1]));
            }
            report(file, L"instruction 0x%llx  in  %s+0x%llx",
                   static_cast<unsigned long long>(instruction), module,
                   static_cast<unsigned long long>(offset));
            report(file, L"thread %lu%s", static_cast<unsigned long>(thread_id),
                   thread_id == g_main_thread ? L" (main)" : L" (worker)");
            report(file, L"");
            write_provenance(file);
            if (heap_corruption) {
                report(file, L"NOTE: the heap manager found a bad block here. The instruction below is");
                report(file, L"      the detector, not the writer - the frame that damaged the block ran");
                report(file, L"      earlier, so the raw scan below is where our own code would show up.");
            }
            report(file, L"");
            // The raw scan and the module table come FIRST, each flushed before the
            // next: both are plain memory reads and toolhelp calls, while the
            // unwinder runs arbitrary unwind code on a stack that is suspect by
            // definition. A report that stops right after a header is the walker
            // having faulted, and everything after it was lost - so the cheap,
            // safe evidence is captured before the walk is attempted.
            report(file, L"raw stack scan (every value that points into a module):");
            write_raw_stack(file, const_cast<CONTEXT*>(context));
            FlushFileBuffers(file);
            report(file, L"");
            report(file, L"loaded modules:");
            write_module_list(file);
            FlushFileBuffers(file);
            report(file, L"");
            report(file, L"walked stack (module+offset, symbol where one is available):");
            const int frames = write_stack(file, const_cast<CONTEXT*>(context));
            if (frames == 0) {
                report(file, L"  (the unwinder could not walk this stack - a fault inside a");
                report(file, L"   system routine has no unwind information; see the raw scan");
                report(file, L"   above, which needs none)");
            }
            FlushFileBuffers(file);
        }
    }
}

LONG WINAPI crash_filter(EXCEPTION_POINTERS* pointers) {
    if (g_installed && pointers != nullptr && pointers->ExceptionRecord != nullptr &&
        pointers->ContextRecord != nullptr) {
        HANDLE file = CreateFileW(g_report_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            write_report(file, pointers, L"unhandled");
            report(file, L"");
            // The dump LAST, with its outcome appended while the report is still
            // open: a zero-byte dump with no explanation would otherwise look
            // like the hook had not run at all.
            DWORD dump_error = 0;
            const DWORD64 dump_bytes = write_dump(pointers, g_dump_path, &dump_error);
            if (dump_bytes > 0) {
                report(file, L"minidump: %s (%llu bytes)", g_dump_path,
                       static_cast<unsigned long long>(dump_bytes));
            } else {
                report(file, L"minidump: FAILED (error %lu) - %s", static_cast<unsigned long>(dump_error),
                       g_dump_path);
            }
            FlushFileBuffers(file);
            CloseHandle(file);
            write_text(g_stderr, L"[crash] wrote ");
            write_text(g_stderr, g_report_path);
            write_text(g_stderr, L"\r\n");
        }
    }
    // Let the engine's own handler have its turn: it prints a trace of its own,
    // and stealing the exception outright would silence it.
    return EXCEPTION_CONTINUE_SEARCH;
}

// A first-chance net, for the crashes the last-chance filter cannot report: a
// blown stack, where running a filter on the broken stack is a gamble, and any
// fault that gets swallowed before reaching it. The codes that matter to this
// project are reported — access violation, stack overflow, and heap corruption
// (which is its own status, or a breakpoint carrying it) — and only when there
// is a real faulting instruction: a runtime that RAISES an access violation to
// signal something it handles itself leaves ExceptionAddress null, and those are
// not our crashes. Heap corruption is the exception to that rule, since its
// breakpoint address is the heap manager itself and still worth having.
// It never handles the exception (always EXCEPTION_CONTINUE_SEARCH), so nothing
// about how the process deals with its own faults changes.
LONG CALLBACK first_chance_filter(EXCEPTION_POINTERS* pointers) {
    if (!g_installed || pointers == nullptr || pointers->ExceptionRecord == nullptr ||
        pointers->ContextRecord == nullptr) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const EXCEPTION_RECORD* record = pointers->ExceptionRecord;
    const bool heap_corruption = is_heap_corruption(record);
    if (record->ExceptionCode != EXCEPTION_ACCESS_VIOLATION &&
        record->ExceptionCode != EXCEPTION_STACK_OVERFLOW && !heap_corruption) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (record->ExceptionAddress == nullptr && !heap_corruption) return EXCEPTION_CONTINUE_SEARCH;
    if (InterlockedCompareExchange(&g_first_chance_busy, 1, 0) != 0) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (g_first_chance_writes < kMaxFirstChanceWrites) {
        HANDLE file = CreateFileW(g_first_chance_path, GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                                  CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            ++g_first_chance_writes;
            write_report(file, pointers, L"first chance");
            report(file, L"");
            report(file, L"(this is the fault as it arrived: if the process survived it, nothing");
            report(file, L" was wrong with this one, but the next unhandled report says whether");
            report(file, L" it was the same fault going fatal)");
            FlushFileBuffers(file);
            CloseHandle(file);
        }
    }
    InterlockedExchange(&g_first_chance_busy, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}

// A synthetic crash is only allowed when asked for by name: it writes a report
// into the player's crash folder and an entry into the Windows application log,
// and both get read as evidence later.
[[nodiscard]] bool armed_for_test() {
    const char* armed = std::getenv("FARLANDS_CRASH_TEST");
    return armed != nullptr && std::strcmp(armed, "1") == 0;
}

bool to_wide(const std::string& text, wchar_t* out, size_t out_size) {
    if (text.empty()) return false;
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out, static_cast<int>(out_size));
    return n > 0;
}

}  // namespace

std::string install_crash_dump_handler(const std::string& directory) {
    if (g_installed || directory.empty()) return std::string();
    wchar_t wide[MAX_PATH] = {};
    if (!to_wide(directory, wide, _countof(wide))) return std::string();
    CreateDirectoryW(wide, nullptr);

    SYSTEMTIME now = {};
    GetLocalTime(&now);
    wchar_t stem[MAX_PATH] = {};
    _snwprintf_s(stem, _countof(stem), _TRUNCATE, L"crash-%04d%02d%02d-%02d%02d%02d", now.wYear,
                 now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
    _snwprintf_s(g_report_path, _countof(g_report_path), _TRUNCATE, L"%s\\%s.txt", wide, stem);
    _snwprintf_s(g_dump_path, _countof(g_dump_path), _TRUNCATE, L"%s\\%s.dmp", wide, stem);
    _snwprintf_s(g_first_chance_path, _countof(g_first_chance_path), _TRUNCATE,
                 L"%s\\%s-firstchance.txt", wide, stem);

    // Loaded modules now, while everything is healthy, so the handler only has to
    // look addresses up. Symbols come from whatever is beside the binaries.
    HANDLE process = GetCurrentProcess();
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS | SYMOPT_LOAD_LINES);
    SymInitializeW(process, nullptr, TRUE);

    g_stderr = GetStdHandle(STD_ERROR_HANDLE);
    if (g_stderr == nullptr) g_stderr = INVALID_HANDLE_VALUE;
    g_main_thread = GetCurrentThreadId();

    g_installed = SetUnhandledExceptionFilter(crash_filter) != nullptr;
    if (!g_installed) return std::string();
    AddVectoredExceptionHandler(1, first_chance_filter);
    return directory;
}

bool crash_for_test() {
    if (!armed_for_test()) return false;
    // A write to an address no process has mapped: the same shape of fault as the
    // dialogs the player has been seeing, on purpose.
    volatile int* nowhere = reinterpret_cast<volatile int*>(0x2B0000000000ULL);
    *nowhere = 1;
    // If the fault was somehow handled, do not continue into undefined behaviour.
    std::abort();
}

}  // namespace debug
}  // namespace VoxelEngine

#else  // !_WIN32

namespace VoxelEngine {
namespace debug {
namespace {

[[nodiscard]] bool armed_for_test() {
    const char* armed = std::getenv("FARLANDS_CRASH_TEST");
    return armed != nullptr && std::strcmp(armed, "1") == 0;
}

}  // namespace

std::string install_crash_dump_handler(const std::string& directory) {
    (void)directory;
    return std::string();
}

bool crash_for_test() {
    if (!armed_for_test()) return false;
    std::abort();
}

}  // namespace debug
}  // namespace VoxelEngine

#endif
