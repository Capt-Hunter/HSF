#pragma once
// HSF.log: every part of HSF writes its lines to this one file next to HSF.ini. That covers the add-on, the Streamline routes,
// the DLSS-G helper and the Vulkan layer. Each line carries the time and the part that wrote it:
//   21:04:05.123 [HSF] route: in game (DLSS-G inside the game's own Streamline)
// The first part of HSF to open the file in a game process starts it fresh, and the last run moves to HSF.previous.log. It appends instead
// when another process still has the file open: the game, for the DLSS-G helper it starts, or a game the launcher has started. A part of
// HSF that loads again in the same process (ReShade reloads add-ons at start-up) also appends.
// Every line is one WriteFile on a file opened for appending, so lines from several threads and processes never mix inside a line.
#include <windows.h>

#include <string>

namespace hsf::hsflog {

// The parts of HSF, as each line names them.
inline constexpr const char* HSF = "HSF";
inline constexpr const char* STREAMLINE = "STREAMLINE";
inline constexpr const char* HELPER = "HELPER";
inline constexpr const char* VULKAN = "VULKAN";

// Lines past this many bytes from one part in one process are dropped, after one line that says so.
inline constexpr unsigned long long MAX_BYTES = 64ull * 1024 * 1024;

// Opens HSF.log at `path` for this module. Again after Close (a reload) opens it again; while open, it does nothing.
void Open(const wchar_t* path);
// The HSF.log that the first part of HSF opened in this process (or, in the helper, in the game that started it), or nullptr. The Vulkan
// layer and the helper use it so that every part writes to the same file.
const wchar_t* PathFromEnvironment(bool this_process_only);
// Where this module writes, or an empty string before Open.
const wchar_t* Path();
void Write(const char* part, const char* text);
void Writef(const char* part, _Printf_format_string_ const char* format, ...);
void Close();

// For the tests and the menu.
// True when another process has `path` open for writing (a game, a launcher, or the helper).
bool OpenElsewhere(const wchar_t* path);
// HSF.previous.log next to `path`.
std::wstring PreviousPath(const wchar_t* path);
// One line as written: time, part, text, CRLF. Trailing line breaks in `text` are dropped.
std::string FormatLine(const SYSTEMTIME& time, const char* part, const char* text);

}  // namespace hsf::hsflog
