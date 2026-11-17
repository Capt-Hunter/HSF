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

// SafeTry=0 in HSF.ini: every way is allowed whatever the file says, and nothing is marked or turned off (for the developer, to try a way again and again).
void SetEnabled(bool on);
bool Enabled();
// The ways turned off for this game, read from the file: up to `max` names, each under 32 chars. Returns how many.
int ListOff(char (*names)[32], int max);
// Turns a way back on for this game (its off and trying keys go). Ways read at start-up take it at the next launch.
void TurnBackOn(const char* path);
// The player turns a way off for this game from the panel (its off key is 2, where a crash writes 1). Takes effect at the next launch.
void TurnOff(const char* path);
// A way's state in the file: 0 on, 1 off after a crash, 2 turned off by the player.
enum OffState { ON = 0, OFF_AFTER_CRASH = 1, OFF_BY_PLAYER = 2 };
int State(const char* path);
// The ways HSF tries this way, for the panel: a short name and what it does, in plain words.
struct Step {
  const char* path;
  const char* label;
  const char* tip;
};
int Steps(const Step** out);
// A short plain name for the panel ("NGX MOTION"); the path itself when unknown.
const char* Describe(const char* path);

// For the test: as Init, with the process id the marks carry, and the clock Begin uses.
void InitForTest(const wchar_t* file, LogFunction log, uint32_t process_id, uint64_t now_ms);

}  // namespace hsf::safetry
