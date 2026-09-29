#include "debug/crash_dump.hpp"

#include <cstdlib>
#include <cstring>

#ifdef _WIN32

#include "debug/crash_dump_internal.hpp"

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
bool g_installed = false;
// First-chance reporting: one write at a time (a fault that faults again must not
// recurse), and a hard cap, so a process that throws access violations around for
// its own reasons cannot fill the disk.
volatile LONG g_first_chance_busy = 0;
int g_first_chance_writes = 0;
constexpr int kMaxFirstChanceWrites = 8;

// A synthetic crash is only allowed when asked for by name: it writes a report
// into the player's crash folder and an entry into the Windows application log,
// and both get read as evidence later.
//
// Read once at install time, like everything else here: getenv takes a lock inside
// the C runtime, so asking from a fault path could hang against another thread rather
// than answer.
bool g_armed_for_test = false;

[[nodiscard]] bool armed_for_test() noexcept { return g_armed_for_test; }

bool to_wide(const std::string& text, wchar_t* out, size_t out_size) {
    if (text.empty()) return false;
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, out, static_cast<int>(out_size));
    return n > 0;
}

}  // namespace

// The hooks that fire, and the two values they read. The report they hand the
// fault to lives in debug/crash_dump_report.cpp.
namespace crash_detail {

HANDLE g_stderr = INVALID_HANDLE_VALUE;
DWORD g_main_thread = 0;

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

}  // namespace crash_detail

std::string install_crash_dump_handler(const std::string& directory) {
    // Not thread safe in the standard's eyes, and there is nothing to replace it
    // with: the rule is about another thread calling setenv underneath this one, and
    // this read happens once, from the loader, before any thread exists - in a
    // process that never calls setenv. That is why it is here rather than on the
    // fault path as well, and why the suppression is the honest answer instead of a
    // lock around it.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* armed = std::getenv("FARLANDS_CRASH_TEST");
    g_armed_for_test = armed != nullptr && std::strcmp(armed, "1") == 0;
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

    crash_detail::g_stderr = GetStdHandle(STD_ERROR_HANDLE);
    if (crash_detail::g_stderr == nullptr) crash_detail::g_stderr = INVALID_HANDLE_VALUE;
    crash_detail::g_main_thread = GetCurrentThreadId();

    g_installed = SetUnhandledExceptionFilter(crash_detail::crash_filter) != nullptr;
    if (!g_installed) return std::string();
    AddVectoredExceptionHandler(1, crash_detail::first_chance_filter);
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

// Set at install time rather than read from the fault path: getenv is not thread safe.
bool g_armed_for_test = false;

[[nodiscard]] bool armed_for_test() noexcept { return g_armed_for_test; }

}  // namespace

std::string install_crash_dump_handler(const std::string& directory) {
    // Not thread safe in the standard's eyes, and there is nothing to replace it
    // with: the rule is about another thread calling setenv underneath this one, and
    // this read happens once, from the loader, before any thread exists - in a
    // process that never calls setenv. The Windows branch above reads the same
    // variable the same way and for the same reason.
    // NOLINTNEXTLINE(concurrency-mt-unsafe)
    const char* armed = std::getenv("FARLANDS_CRASH_TEST");
    g_armed_for_test = armed != nullptr && std::strcmp(armed, "1") == 0;
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
