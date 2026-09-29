#pragma once

// The pieces of the Windows crash handler that debug/crash_dump.cpp (the hooks,
// and their installation) and debug/crash_dump_report.cpp (the report those hooks
// write) both need: the two globals the process settles once, at install time,
// and the five helpers that cross between the two files.
//
// Private to src/debug: nothing outside it includes this, and nothing in it is
// part of the engine's interface.

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>

namespace VoxelEngine {
namespace debug {
namespace crash_detail {

// Where a report's lines also go (so a console run or the editor's Output panel
// shows the whole thing without anyone hunting for a file), and which thread is
// the main one. Neither can be looked up from a fault path.
extern HANDLE g_stderr;
extern DWORD g_main_thread;

void write_text(HANDLE file, const wchar_t* text);
void report(HANDLE file, const wchar_t* format, ...);

// The heap manager's "this block is corrupt" status, in either of the two shapes
// it arrives in (see the definition).
[[nodiscard]] bool is_heap_corruption(const EXCEPTION_RECORD* record);

// Answers the bytes written, or 0, with the last error in `error_out`.
DWORD64 write_dump(EXCEPTION_POINTERS* pointers, const wchar_t* path, DWORD* error_out);

// The body of a report, shared by both handlers. `kind` says which one caught it.
void write_report(HANDLE file, EXCEPTION_POINTERS* pointers, const wchar_t* kind);

}  // namespace crash_detail
}  // namespace debug
}  // namespace VoxelEngine

#endif  // _WIN32
