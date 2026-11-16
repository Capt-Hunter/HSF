// See hook_patch.h. Moved from sl_game.cpp unchanged, except that log lines go to the caller's log function.
#include "hook_patch.h"

#include "safe_try.h"

#include <windows.h>
#include <intrin.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

// MinHook's HDE64 length decoder (BSD-2-Clause), built as C in hsf_minhook.
extern "C" {
#include "../third_party/minhook/src/hde/hde64.h"
}

namespace hsf::hookpatch {
namespace {

// No C++ objects in this: it uses structured exceptions.
bool SafeCopy(void* to, const void* from, size_t bytes) {
  __try {
    std::memcpy(to, from, bytes);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

void Say(Log log, const char* format, ...) {
  if (log == nullptr) return;
  char text[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  log(text);
}

}  // namespace

// One relay and one trampoline for each hook, in a page within 2 GB of the patched code, as the patch is a 5-byte relative jump.
struct CodePage {
  uint8_t* base;
  size_t used;
};
static CodePage g_pages[8];

static uint8_t* AllocateNear(const uint8_t* near_address) {
  SYSTEM_INFO info;
  GetSystemInfo(&info);
  const uintptr_t step = info.dwAllocationGranularity;
  const uintptr_t origin = reinterpret_cast<uintptr_t>(near_address) & ~(step - 1);
  constexpr uintptr_t RANGE = 0x70000000;
  for (uintptr_t distance = step; distance < RANGE; distance += step) {
    for (const uintptr_t address : {origin + distance, origin > distance ? origin - distance : uintptr_t{0}}) {
      if (address == 0) continue;
      MEMORY_BASIC_INFORMATION mbi;
      if (VirtualQuery(reinterpret_cast<void*>(address), &mbi, sizeof(mbi)) == 0 || mbi.State != MEM_FREE) continue;
      void* const page = VirtualAlloc(reinterpret_cast<void*>(address), 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
      if (page != nullptr) return static_cast<uint8_t*>(page);
    }
  }
  return nullptr;
}

// A slot in a page near target. Pages stay for good: a thread can still be inside a trampoline when hooks come off, and add-on reloads are few.
uint8_t* NearSlot(const uint8_t* target) {
  for (CodePage& page : g_pages) {
    if (page.base == nullptr) {
      page.base = AllocateNear(target);
      if (page.base == nullptr) return nullptr;
    }
    const intptr_t distance = page.base - target;
    if (distance > -0x7FF00000LL && distance < 0x7FF00000LL && page.used + SLOT_BYTES <= 4096) {
      uint8_t* const slot = page.base + page.used;
      page.used += SLOT_BYTES;
      return slot;
    }
  }
  return nullptr;
}

void WriteAbsoluteJump(uint8_t* at, const void* to) {
  at[0] = 0xFF;
  at[1] = 0x25;
  std::memset(at + 2, 0, 4);
  std::memcpy(at + 6, &to, sizeof(to));
}

// Code still mapped at this address. A module that has been unloaded leaves free memory behind.
bool CodeStillThere(const void* address) {
  MEMORY_BASIC_INFORMATION mbi;
  return VirtualQuery(address, &mbi, sizeof(mbi)) != 0 && mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}

// All 8 bytes in one write, so a thread entering the function sees the old start or the new one, never half of each. A function that is not 8-byte aligned (only with the
// full decoder, H3) cannot be written in one go: the bytes are copied, as MinHook does, and a thread entering at that very moment could see half of each.
static bool WriteFirstBytes(uint8_t* target, const uint8_t bytes[8]) {
  DWORD old = 0;
  if (!VirtualProtect(target, 8, PAGE_EXECUTE_READWRITE, &old)) return false;
  LONG64 value;
  std::memcpy(&value, bytes, 8);
  if ((reinterpret_cast<uintptr_t>(target) & 7) == 0) _InterlockedExchange64(reinterpret_cast<volatile LONG64*>(target), value);
  else std::memcpy(target, bytes, 8);
  VirtualProtect(target, 8, old, &old);
  FlushInstructionCache(GetCurrentProcess(), target, 8);
  return true;
}

// ReShade loads its add-ons, unloads them and loads them again. Another add-on that hooks the same function after HSF (RenoDX DLSS does, in Witcher 3) copies HSF's jump into its own
// trampoline, so after the unload its calls still go through HSF's relay, into wherever HSF's code was. The relay and trampoline sit in a code page that stays, and the process environment
// survives the reload, so each hook leaves there where its relay is. Remove points the relay at the trampoline; the next load finds it and points it at the new hook again.
static void RelayName(const Hook& hook, wchar_t* name, size_t size) { std::swprintf(name, size, L"HSF_HOOK_%hs", hook.name); }

static void PublishRelay(const Hook& hook) {
  wchar_t name[64], value[96];
  RelayName(hook, name, 64);
  std::swprintf(value, 96, L"%p %p %p", static_cast<void*>(hook.target), static_cast<void*>(hook.relay), static_cast<void*>(hook.trampoline));
  SetEnvironmentVariableW(name, value);
}

// The 8-byte address in the relay's absolute jump, written in one go.
void PointRelay(uint8_t* relay, const void* to) {
  LONG64 value;
  std::memcpy(&value, &to, 8);
  _InterlockedExchange64(reinterpret_cast<volatile LONG64*>(relay + 6), value);
  FlushInstructionCache(GetCurrentProcess(), relay, 16);
}

// target starts with someone else's jump. If an earlier load of HSF hooked it and left its relay, take that relay back: their jump still leads through it.
static bool Adopt(Hook& hook, uint8_t* target, Log log) {
  wchar_t name[64], value[96] = L"";
  RelayName(hook, name, 64);
  if (GetEnvironmentVariableW(name, value, 96) == 0) return false;
  void *old_target = nullptr, *relay = nullptr, *trampoline = nullptr;
  if (swscanf_s(value, L"%p %p %p", &old_target, &relay, &trampoline) != 3 || old_target != target) return false;
  uint8_t head[6];
  if (!CodeStillThere(relay) || !CodeStillThere(trampoline) || !SafeCopy(head, relay, sizeof(head))) return false;
  if (head[0] != 0xFF || head[1] != 0x25 || head[2] != 0 || head[3] != 0 || head[4] != 0 || head[5] != 0) return false;
  hook.target = target;
  hook.relay = static_cast<uint8_t*>(relay);
  hook.trampoline = static_cast<uint8_t*>(trampoline);
  *hook.real = trampoline;  // ready before any call can arrive
  std::atomic_thread_fence(std::memory_order_seq_cst);
  PointRelay(hook.relay, hook.hook);
  hook.installed = true;
  hook.adopted = true;
  Say(log, "hook %s taken back at %p: someone else's jump is on top, and it leads through HSF's relay from the last load", hook.name, target);
  return true;
}

// target starts with "eb f9", a 2-byte jump back to a 5-byte jump in the padding before the function: another tool's hot-patch hook. Moving their short jump would break the way
// back into the function their hook uses, so HSF leaves the function alone and changes only where the 5-byte jump leads: to HSF's relay, whose trampoline jumps on to their hook.
// HSF runs first, then their hook, then the function. The displacement is 4 bytes, written in one go when it is 4-byte aligned.
static bool ChainHotPatch(Hook& hook, uint8_t* target, const char* bytes_text, Log log) {
  uint8_t* const stub = target - 5;
  uint8_t head[5];
  if (!SafeCopy(head, stub, sizeof(head)) || head[0] != 0xE9) {
    Say(log, "hook %s NOT installed at %p: starts %s, a short jump back, but no jump before the function", hook.name, target, bytes_text);
    return false;
  }
  if ((reinterpret_cast<uintptr_t>(stub + 1) & 3) != 0) {
    Say(log, "hook %s NOT installed at %p: starts %s, a hot-patch jump that cannot be changed in one write", hook.name, target, bytes_text);
    return false;
  }
  int32_t old_displacement;
  std::memcpy(&old_displacement, head + 1, 4);
  const uint8_t* const theirs = stub + 5 + old_displacement;
  uint8_t* const slot = NearSlot(stub);
  if (slot == nullptr) {
    Say(log, "hook %s NOT installed: no memory near %p", hook.name, stub);
    return false;
  }
  uint8_t* const trampoline = slot + 16;
  WriteAbsoluteJump(slot, hook.hook);
  WriteAbsoluteJump(trampoline, theirs);
  FlushInstructionCache(GetCurrentProcess(), slot, SLOT_BYTES);
  const int32_t new_displacement = static_cast<int32_t>(slot - (stub + 5));
  *hook.real = trampoline;  // ready before any call can arrive
  std::atomic_thread_fence(std::memory_order_seq_cst);
  DWORD old = 0;
  if (!VirtualProtect(stub, 5, PAGE_EXECUTE_READWRITE, &old)) {
    Say(log, "hook %s NOT installed: could not write at %p", hook.name, stub);
    return false;
  }
  _InterlockedExchange(reinterpret_cast<volatile long*>(stub + 1), new_displacement);
  VirtualProtect(stub, 5, old, &old);
  FlushInstructionCache(GetCurrentProcess(), stub, 5);
  hook.target = target;
  hook.relay = slot;
  hook.trampoline = trampoline;
  hook.adopted = false;
  hook.chained_at = stub;
  hook.chained_old = old_displacement;
  hook.chained_new = new_displacement;
  hook.installed = true;
  Say(log, "hook %s installed at %p behind another tool's hot-patch jump (to %p): starts %s", hook.name, target, theirs, bytes_text);
  return true;
}

// Call with the caller's lock held. A function already hooked by someone else starts with a jump. If that jump leads through HSF's own relay (an earlier load of HSF, under someone
// else's hook), the relay is taken back. Otherwise HSF hooks on top: the jump is moved into the trampoline, so HSF runs first and then their hook (RenoDX DLSS on NGX in Jusant).
bool Install(Hook& hook, uint8_t* target, Log log) {
  if (hook.installed && hook.target == target) return true;
  char bytes_text[64] = "";
  uint8_t first[32] = {};
  if (!SafeCopy(first, target, sizeof(first))) return false;
  if (first[0] == 0xE9 && Adopt(hook, target, log)) return true;
  for (int i = 0; i < 12; ++i) std::snprintf(bytes_text + i * 3, 4, "%02x ", first[i]);
  if (first[0] == 0xEB && first[1] == 0xF9) return ChainHotPatch(hook, target, bytes_text, log);
  MovedFix fixes[MAX_RIP_LOADS];
  size_t fix_count = 0;
  // Someone else's absolute jump (ff 25, the address in memory): HSF goes on top, and its trampoline makes the same jump.
  const void* absolute_to = nullptr;
  if (first[0] == 0xFF && first[1] == 0x25) {
    int32_t displacement;
    std::memcpy(&displacement, first + 2, 4);
    if (!SafeCopy(&absolute_to, target + 6 + displacement, sizeof(absolute_to)) || !CodeStillThere(absolute_to)) absolute_to = nullptr;
  }
  size_t length = 0;
  if (absolute_to != nullptr) {
    length = 6;
  } else {
    size_t rip_at[MAX_RIP_LOADS];
    length = PrologueLength(first, 5, rip_at, &fix_count);
    for (size_t i = 0; i < fix_count; ++i) {
      const bool is_jump = first[rip_at[i]] == 0xE9;
      fixes[i] = {static_cast<uint8_t>(rip_at[i]), static_cast<uint8_t>(rip_at[i] + (is_jump ? 1 : 3)), static_cast<uint8_t>(rip_at[i] + (is_jump ? 5 : 7)), is_jump};
    }
  }
  const bool aligned = (reinterpret_cast<uintptr_t>(target) & 7) == 0;
  // HSF's short list does not know this start, or the function is not 8-byte aligned: a full length decoder (H3), unless it was the way in use when this game last went down.
  bool decoded = false;
  if ((length == 0 || !aligned) && safetry::Allowed("H3_decoder")) {
    if (length == 0) {
      length = PrologueLengthDecoded(first, 5, fixes, &fix_count);
      decoded = length != 0;
    }
    if (length != 0) safetry::Begin("H3_decoder");
  } else if (!aligned) {
    length = 0;
  }
  if (length == 0 || length > 24) {
    if (Adopt(hook, target, log)) return true;
    Say(log, "hook %s NOT installed at %p: starts %s(left alone)", hook.name, target, bytes_text);
    return false;
  }
  uint8_t* const slot = NearSlot(target);
  if (slot == nullptr) {
    Say(log, "hook %s NOT installed: no memory near %p", hook.name, target);
    return false;
  }
  // [relay: jump to the hook][trampoline: the moved instructions, jump back]
  uint8_t* const trampoline = slot + 16;
  WriteAbsoluteJump(slot, hook.hook);
  std::memcpy(trampoline, first, length);
  // A moved load relative to rip must still reach the same address. The page is near the target and what it loads usually is too, but check.
  bool ends_in_jump = false;
  if (absolute_to != nullptr) {
    WriteAbsoluteJump(trampoline, absolute_to);
    ends_in_jump = true;
  }
  for (size_t i = 0; i < fix_count; ++i) {
    const MovedFix& fix = fixes[i];
    int32_t displacement;
    std::memcpy(&displacement, first + fix.disp_at, sizeof(displacement));
    const uint8_t* const address = target + fix.end + displacement;
    const intptr_t moved = address - (trampoline + fix.end);
    // Someone else's jump as the last moved instruction (MFG-Unlocker on slDLSSGSetOptions, its module more than 2 GB from the trampoline in PBZ): an absolute jump to the
    // same place does the same and reaches anywhere. Nothing runs after a jump, so no jump back follows it.
    if (fix.is_jump && fix.end == length && fix.at + 14u <= SLOT_BYTES - 16) {
      WriteAbsoluteJump(trampoline + fix.at, address);
      ends_in_jump = true;
      continue;
    }
    if (moved > INT32_MAX || moved < INT32_MIN) {
      Say(log, "hook %s NOT installed at %p: its load cannot reach from the trampoline. Starts %s", hook.name, target, bytes_text);
      return false;
    }
    const int32_t new_displacement = static_cast<int32_t>(moved);
    std::memcpy(trampoline + fix.disp_at, &new_displacement, sizeof(new_displacement));
  }
  if (!ends_in_jump) WriteAbsoluteJump(trampoline + length, target + length);
  FlushInstructionCache(GetCurrentProcess(), slot, SLOT_BYTES);

  std::memcpy(hook.saved, first, 8);
  std::memcpy(hook.patch, hook.saved, 8);
  hook.patch[0] = 0xE9;
  const int32_t relative = static_cast<int32_t>(slot - (target + 5));
  std::memcpy(hook.patch + 1, &relative, 4);
  *hook.real = trampoline;  // ready before any call can arrive
  std::atomic_thread_fence(std::memory_order_seq_cst);
  if (!WriteFirstBytes(target, hook.patch)) {
    Say(log, "hook %s NOT installed: could not write at %p", hook.name, target);
    return false;
  }
  hook.target = target;
  hook.relay = slot;
  hook.trampoline = trampoline;
  hook.adopted = false;
  hook.installed = true;
  PublishRelay(hook);
  Say(log, "hook %s installed at %p, moved %zu bytes, %zu loads relative to rip%s%s: %s", hook.name, target, length, fix_count, decoded ? ", measured by the full decoder" : "",
       aligned ? "" : ", not 8-byte aligned (written in two parts)", bytes_text);
  return true;
}

void Remove(Hook& hook, Log log) {
  if (!hook.installed) return;
  hook.installed = false;
  if (!CodeStillThere(hook.target)) return;
  if (hook.chained_at != nullptr) {
    // Put their hot-patch jump back only if it still leads to HSF's relay; otherwise the relay skips HSF from now on.
    int32_t now_displacement;
    DWORD old = 0;
    if (SafeCopy(&now_displacement, hook.chained_at + 1, 4) && now_displacement == hook.chained_new && VirtualProtect(hook.chained_at, 5, PAGE_EXECUTE_READWRITE, &old)) {
      _InterlockedExchange(reinterpret_cast<volatile long*>(hook.chained_at + 1), hook.chained_old);
      VirtualProtect(hook.chained_at, 5, old, &old);
      FlushInstructionCache(GetCurrentProcess(), hook.chained_at, 5);
    } else if (hook.relay != nullptr && CodeStillThere(hook.relay)) {
      PointRelay(hook.relay, hook.trampoline);
      Say(log, "hook %s changed by someone else, not restored; its calls now skip HSF", hook.name);
    }
    hook.chained_at = nullptr;
    return;
  }
  uint8_t now[8];
  if (!SafeCopy(now, hook.target, 8)) return;
  // Someone patched on top of us (RenoDX DLSS, for one): leave their jump in place rather than cut it out, and point HSF's relay straight at the trampoline, so their calls reach the real
  // function and never code of HSF's that is about to go. The next load of HSF takes the relay back (Adopt).
  if (hook.adopted || std::memcmp(now, hook.patch, 5) != 0) {
    if (hook.relay != nullptr && hook.trampoline != nullptr && CodeStillThere(hook.relay)) PointRelay(hook.relay, hook.trampoline);
    Say(log, "hook %s changed by someone else, not restored; its calls now skip HSF", hook.name);
    return;
  }
  WriteFirstBytes(hook.target, hook.saved);
  // Nothing leads through the relay now, so the next load must not take it back.
  wchar_t name[64];
  RelayName(hook, name, 64);
  SetEnvironmentVariableW(name, nullptr);
}

size_t PrologueLength(const uint8_t* code, size_t min_bytes, size_t* rip_at, size_t* rip_count) {
  size_t at = 0;
  size_t rips = 0;
  while (at < min_bytes) {
    const uint8_t* const p = code + at;
    size_t length = 0;
    if (p[0] >= 0x50 && p[0] <= 0x57) {
      length = 1;  // push r
    } else if ((p[0] == 0x40 || p[0] == 0x41) && p[1] >= 0x50 && p[1] <= 0x57) {
      length = 2;  // push r, with a prefix
    } else if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x89 && (p[2] & 0xC7) == 0x44 && p[3] == 0x24) {
      length = 5;  // mov [rsp+disp8], r
    } else if ((p[0] == 0x48 || p[0] == 0x4C || p[0] == 0x49 || p[0] == 0x4D) && (p[1] == 0x8B || p[1] == 0x89) && p[2] >= 0xC0) {
      length = 3;  // mov r, r
    } else if (p[0] == 0x48 && p[1] == 0x83 && p[2] == 0xEC) {
      length = 4;  // sub rsp, imm8
    } else if (p[0] == 0x48 && p[1] == 0x81 && p[2] == 0xEC) {
      length = 7;  // sub rsp, imm32
    } else if ((p[0] == 0x48 || p[0] == 0x4C) && (p[1] == 0x8B || p[1] == 0x8D) && (p[2] & 0xC7) == 0x05) {
      if (rips == MAX_RIP_LOADS) return 0;
      if (rip_at != nullptr) rip_at[rips] = at;
      ++rips;
      length = 7;  // mov r, [rip+disp32] or lea r, [rip+disp32]
    } else if (p[0] == 0x33 && p[1] >= 0xC0) {
      length = 2;  // xor r, r
    } else if (p[0] == 0xE9) {
      // jmp rel32, as a small forwarding function ends: slReflexSetOptions is xor edx, edx then this. At the very start it is someone else's hook, and HSF goes on top of it.
      if (rips == MAX_RIP_LOADS) return 0;
      if (rip_at != nullptr) rip_at[rips] = at;
      ++rips;
      at += 5;
      break;
    } else {
      return 0;
    }
    at += length;
  }
  if (at < min_bytes) return 0;  // ended by a jump before there was room for the patch
  if (rip_count != nullptr) *rip_count = rips;
  return at;
}

size_t PrologueLengthDecoded(const uint8_t* code, size_t min_bytes, MovedFix* fixes, size_t* fix_count) {
  size_t at = 0;
  size_t count = 0;
  while (at < min_bytes) {
    hde64s hs;
    const unsigned int length = hde64_disasm(code + at, &hs);
    if (length == 0 || (hs.flags & F_ERROR) != 0) return 0;
    const size_t end = at + length;
    const uint8_t op = hs.opcode;
    // A return or a stop before there is room for the patch: the function is too short.
    if (op == 0xC3 || op == 0xC2 || op == 0xCC) return 0;
    const bool rip_relative = (hs.flags & F_MODRM) != 0 && hs.modrm_mod == 0 && hs.modrm_rm == 5;
    if (rip_relative || (hs.flags & F_RELATIVE) != 0) {
      const bool jump = op == 0xE9;
      const bool call = op == 0xE8;
      const bool far_branch = op == 0x0F && hs.opcode2 >= 0x80 && hs.opcode2 <= 0x8F;  // jcc rel32
      if ((hs.flags & F_RELATIVE) != 0 && !jump && !call && !far_branch) return 0;  // a short branch (rel8) cannot be moved
      if (count == MAX_RIP_LOADS) return 0;
      size_t immediate = 0;
      if (rip_relative) {
        if (hs.flags & F_IMM8) immediate = 1;
        else if (hs.flags & F_IMM16) immediate = 2;
        else if (hs.flags & F_IMM32) immediate = 4;
        else if (hs.flags & F_IMM64) immediate = 8;
      }
      const size_t disp_at = end - 4 - immediate;
      int32_t displacement;
      std::memcpy(&displacement, code + disp_at, 4);
      // A branch back into the bytes being moved would land in the patch.
      const intptr_t lands = static_cast<intptr_t>(end) + displacement;
      if (!rip_relative && lands >= 0 && lands < static_cast<intptr_t>(std::max<size_t>(end, min_bytes))) return 0;
      fixes[count++] = {static_cast<uint8_t>(at), static_cast<uint8_t>(disp_at), static_cast<uint8_t>(end), jump};
      at = end;
      if (jump) break;  // nothing after a jump belongs to this path
      continue;
    }
    if (op == 0xEB || (op >= 0x70 && op <= 0x7F) || (op >= 0xE0 && op <= 0xE3)) return 0;
    at = end;
  }
  if (at < min_bytes) return 0;
  if (fix_count != nullptr) *fix_count = count;
  return at;
}

}  // namespace hsf::hookpatch
