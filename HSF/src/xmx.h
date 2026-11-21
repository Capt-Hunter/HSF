// XeMFG: HSF's second frame generation engine (HSF.ini FgEngine=xemfg, default dlssg). Intel's XeSS frame generation model makes the
// generated frames inside DLSS-G's own EvaluateFeature, so DLSS-G still presents, paces and runs Reflex, and every HSF route that feeds
// DLSS-G (in game, bring-in, the helper for D3D11, DX9, 32-bit and Vulkan games) gets the same engine through one hook.
//
// Ported from MFGAdaUnlock-RenoDx by ImDreamt (MIT, Copyright (c) 2026 ImDreamt), commit 8b854ff: src/addons/mfgunlock/xmx_hook.hpp,
// xmx_generator.hpp, xmx_policy.hpp and tag_states.hpp. The licence is in licenses/ and THIRD_PARTY_NOTICES.md.
//
// HSF ships nothing from Intel. The user puts libxess_fg.dll 1.3.1.78 and libxell.dll next to HSF.ini (the game's folder). Anything
// missing or of another build: no hook, plain DLSS-G, and Snapshot says why. Hooking nvngx_dlssg.dll ends NVIDIA's own generation in that
// process, so the hook goes in only after the preflight, and from then on a frame XeFG cannot make repeats the real one: never black.
#pragma once

#include <windows.h>

#include <array>
#include <cstdint>

namespace hsf::xmx {

using LogFunction = void (*)(const char* text);

// Once per process; later calls change nothing (the engine changes on a restart). wanted: HSF.ini FgEngine=xemfg. intel_folder: where
// libxess_fg.dll and libxell.dll are looked for (the game's folder, next to HSF.ini).
void Configure(bool wanted, const wchar_t* intel_folder, LogFunction log);
// From a frame loop of the process that runs DLSS-G (the add-on's present, the helper's presenter). Looks for nvngx_dlssg.dll copies at
// most once a second and hooks each new one. Does nothing unless Configure asked for XeMFG and the preflight passed.
void Poll();

// HSF.ini FgEngine's value: true for xemfg (any case), false for anything else (dlssg, empty, unknown).
bool WantedFromIni(const wchar_t* ini_path);

struct Status {
  bool wanted = false;      // FgEngine=xemfg
  bool preflight = false;   // the Intel files are there and of the supported build
  int hooked = 0;           // nvngx_dlssg.dll copies hooked: NVIDIA's generation is gone in this process once this is above 0
  bool generating = false;  // XeFG made a frame in the last 2 s
  uint32_t max_frames = 1;  // generated frames per real frame this model makes (1 = 2x, Intel's own model)
  bool published = false;   // the model was set up (at XeFG's first context): `model` says how
  char failure[160] = "";   // why XeMFG is not running, or empty
  char model[160] = "";     // once published: where the 3x-6x programs came from, or why it stays at 2x
};
Status Snapshot();

// The D3D12 state each resource had when it was tagged for Streamline (from MFGAdaUnlock's tag_states.hpp). DLSS-G hands NGX a tag valid
// until present in that state; a resource Streamline copied (a tag valid only now) arrives in COPY_DEST. Filled by HSF's own tags and the
// game's (sl_game.cpp), read by XeMFG's copies. Header-only, so every target that tags can record without linking xmx.cpp.
namespace tagstates {
struct Entry {
  const void* resource = nullptr;
  uint32_t state = 0;
};
inline SRWLOCK g_lock = SRWLOCK_INIT;
inline std::array<Entry, 32> g_entries{};
inline uint32_t g_next = 0;
inline void Record(const void* resource, uint32_t state) {
  if (resource == nullptr) return;
  AcquireSRWLockExclusive(&g_lock);
  Entry* slot = nullptr;
  for (auto& entry : g_entries)
    if (entry.resource == resource) slot = &entry;
  if (slot == nullptr) slot = &g_entries[g_next++ % g_entries.size()];
  *slot = {resource, state};
  ReleaseSRWLockExclusive(&g_lock);
}
inline uint32_t StateOf(const void* resource, uint32_t fallback) {
  uint32_t state = fallback;
  AcquireSRWLockShared(&g_lock);
  for (const auto& entry : g_entries)
    if (resource != nullptr && entry.resource == resource) state = entry.state;
  ReleaseSRWLockShared(&g_lock);
  return state;
}
}  // namespace tagstates

}  // namespace hsf::xmx
