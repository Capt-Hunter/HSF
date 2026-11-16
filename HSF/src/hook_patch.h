// HSF's own hook patcher, used by sl_game.cpp for the game's Streamline and by the NGX readers through slgame::HookFunction. A 5-byte jump at the start of each function goes to a
// relay in a page HSF allocates near the module, which jumps to the hook; the hook calls a trampoline holding the moved prologue, which jumps back. Anything unexpected at a function's
// start means no hook there, only a log line.
// The patcher has its own lock, a leaf: nothing is called while it is held, and log lines are handed to the caller's log after it is released. So a caller's log may
// take any lock or hook something itself.
#pragma once

#include <cstddef>
#include <cstdint>

namespace hsf::hookpatch {

using Log = void (*)(const char* text);

struct Hook {
  const char* name;
  void* hook;
  void** real;
  uint8_t* target;
  uint8_t saved[8];
  uint8_t patch[8];
  bool installed;
  uint8_t* relay;       // in a code page that outlives HSF: an absolute jump to the hook
  uint8_t* trampoline;  // the moved instructions, then a jump back into the target
  bool adopted;         // took over the relay from an earlier load of HSF, with someone else's jump on top
  uint8_t* chained_at;  // a hot-patch jump before the function that now leads to HSF (see ChainHotPatch), or null
  int32_t chained_old;  // that jump's displacement before HSF changed it
  int32_t chained_new;  // the displacement HSF wrote
};

// One relay and one trampoline for each hook, in a slot of SLOT_BYTES.
constexpr size_t SLOT_BYTES = 64;
// A slot in a page within 2 GB of target, or null. Pages stay for good. Takes the patcher's lock.
uint8_t* NearSlot(const uint8_t* target);
// An absolute jump (ff 25, the address after it): 14 bytes.
void WriteAbsoluteJump(uint8_t* at, const void* to);
// Code still mapped at this address. A module that has been unloaded leaves free memory behind.
bool CodeStillThere(const void* address);
// The 8-byte address in a relay's absolute jump, written in one go.
void PointRelay(uint8_t* relay, const void* to);

// Hooks target with hook.hook; *hook.real gets what calls the original. Messages go to log (may be null). The caller keeps hook; the patcher's lock guards its patching.
bool Install(Hook& hook, uint8_t* target, Log log);
// Puts the function's bytes back, or, if someone hooked on top, points HSF's relay at the trampoline.
void Remove(Hook& hook, Log log);

// One caller's hooks (the NGX readers on D3D12, D3D11 and Vulkan each have one). RemoveAll removes only this set's hooks. Safe from any thread.
class HookSet {
 public:
  static constexpr int MAX_HOOKS = 8;
  // Hooks target as Install does; already hooked under this name at this target is true with nothing done. A full set refuses with a log line.
  bool Add(const char* name, void* target, void* hook_function, void** real, Log log);
  void RemoveAll(Log log);

 private:
  Hook hooks_[MAX_HOOKS] = {};
};

// How many bytes of whole instructions at code cover at least min_bytes, or 0 if an instruction is not on the short list of prologue instructions HSF can move. A function that does
// not start with those is left alone. rip_at, if given, receives the offsets of the instructions relative to rip (loads, and a jump that ends a small forwarding function), which need a
// new displacement when moved; rip_count is how many, at most MAX_RIP_LOADS. Public for the test.
constexpr size_t MAX_RIP_LOADS = 4;
size_t PrologueLength(const uint8_t* code, size_t min_bytes, size_t* rip_at = nullptr, size_t* rip_count = nullptr);
// A moved instruction that needs a new 4-byte displacement: where it starts, where its displacement is and where it ends, and whether it is a jump (rel32).
struct MovedFix {
  uint8_t at, disp_at, end;
  bool is_jump;
};
// For a start PrologueLength does not know: a full x64 length decoder (MinHook's HDE64, BSD) measures whole instructions covering at least min_bytes. Loads relative to rip,
// calls and far jumps (rel32) are listed in fixes (at most MAX_RIP_LOADS); short branches, a return before min_bytes, or a branch back into the moved bytes give 0. Public for the test.
size_t PrologueLengthDecoded(const uint8_t* code, size_t min_bytes, MovedFix* fixes, size_t* fix_count);

}  // namespace hsf::hookpatch
