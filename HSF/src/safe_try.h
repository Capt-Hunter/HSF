// New ways of doing something risky (a new kind of hook, say) are tried with the old way kept as the backup. Before a new way is first used in a launch, HSF marks it in
// HSF_safe_try.ini next to HSF.ini, under the game's exe name. The mark goes once the game has run ~30 s since, or when HSF unloads or the game exits normally. A mark still
// there at the next launch means that launch ended (a crash, most likely) while the new way was in use: from then on that way is off for this game and the old one is used,
// which the log says once. HSF writes the file itself; nobody needs to edit it.
#pragma once

#include <cstdint>

namespace hsf::safetry {

using LogFunction = void (*)(const char* text);

// file: full path of HSF_safe_try.ini. log: where decisions go (may be null). Safe under the loader lock.
void Init(const wchar_t* file, LogFunction log);
// Whether the new way named path may be used in this game. Reads the file once per path.
bool Allowed(const char* path);
// The new way is about to be used: marks it, once per launch. Safe under the loader lock.
void Begin(const char* path);
// Often (each frame is fine): marks older than STABLE_MS are cleared.
constexpr uint64_t STABLE_MS = 30000;
void Tick(uint64_t now_ms = 0);
// A normal unload or exit: this launch's marks are cleared.
void ClearAll();

// For the test: as Init, with the process id the marks carry, and the clock Begin uses.
void InitForTest(const wchar_t* file, LogFunction log, uint32_t process_id, uint64_t now_ms);

}  // namespace hsf::safetry
