#pragma once

#include <string>

namespace VoxelEngine {
namespace debug {

// Installs a handler for an unhandled exception that writes two files next to
// `directory`: a MINIDUMP (`crash-<stamp>.dmp`) and a text report of the same
// event (`crash-<stamp>.txt`) naming the exception code, whether the faulting
// access was a read or a write and at what address, the module the instruction
// pointer is in with its offset, and the faulting thread's stack with a symbol
// per frame where one can be found.
//
// The reason this exists rather than reading the engine's own crash log: the
// failure that prompted it kills the process before the engine's handler can
// print anything (the log simply stops), and a Windows dialog naming a system
// DLL and a garbage address says only that something tried to read or write
// memory it had no business touching. The report is written FIRST and the dump
// second, because the report is what can be read without a debugger, and the
// handler then returns EXCEPTION_CONTINUE_SEARCH so the engine's own handler
// still runs.
//
// Answers the directory it actually used ("" when the platform has no such hook,
// or the directory could not be created). Call once, at startup.
std::string install_crash_dump_handler(const std::string& directory);

// Deliberately faults, so the handler can be proven end to end rather than
// trusted. Bound to a method that only exists in a debug build.
//
// Answers false WITHOUT faulting unless FARLANDS_CRASH_TEST=1 is in the
// environment: a synthetic crash writes a report into the player's crash folder
// and an entry into the Windows application log, where it sits beside the real
// ones and makes both harder to read. Arming it stays an explicit act.
[[nodiscard]] bool crash_for_test();

}  // namespace debug
}  // namespace VoxelEngine
