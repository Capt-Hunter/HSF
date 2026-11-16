// Offline test of HSF's patcher (src/sl_game.cpp) on made-up functions, no game needed: the full length decoder for starts HSF's short list does not know (H3), a function
// that is not 8-byte aligned, and the safe-try switch that turns the decoder off for a game.   hook_test   (exit code 0 = all passed)
#include "../src/safe_try.h"
#include "../src/sl_game.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
  if (!ok) ++g_failures;
}

using IntFn = int (*)();
IntFn g_real_a = nullptr, g_real_b = nullptr, g_real_c = nullptr;
int HookA() { return g_real_a() + 10; }
int HookB() { return g_real_b() + 10; }
int HookC() { return g_real_c() + 10; }

size_t Decode(std::initializer_list<uint8_t> bytes, hsf::slgame::MovedFix* fixes, size_t* count) {
  uint8_t code[32] = {};
  std::memcpy(code, bytes.begin(), bytes.size());
  *count = 99;
  return hsf::slgame::PrologueLengthDecoded(code, 5, fixes, count);
}

void TestDecoder() {
  hsf::slgame::MovedFix f[hsf::slgame::MAX_RIP_LOADS];
  size_t n = 0;
  Check(Decode({0xEB, 0x05}, f, &n) == 0, "decoder: a short jump at the start is refused");
  Check(Decode({0xC3}, f, &n) == 0, "decoder: a return before 5 bytes is refused");
  Check(Decode({0x48, 0x89, 0x5C, 0x24, 0x08}, f, &n) == 5 && n == 0, "decoder: mov [rsp+8], rbx is 5 bytes, nothing to fix");
  Check(Decode({0xE8, 0x10, 0, 0, 0}, f, &n) == 5 && n == 1 && f[0].disp_at == 1 && f[0].end == 5 && !f[0].is_jump, "decoder: a call rel32 is moved with a new displacement");
  Check(Decode({0xE9, 0x10, 0, 0, 0}, f, &n) == 5 && n == 1 && f[0].is_jump, "decoder: a jmp rel32 ends the moved bytes");
  Check(Decode({0x8B, 0x05, 0x10, 0, 0, 0}, f, &n) == 6 && n == 1 && f[0].disp_at == 2 && f[0].end == 6, "decoder: mov eax, [rip+d] (not on the short list)");
  Check(Decode({0x80, 0x3D, 0x10, 0, 0, 0, 0x01}, f, &n) == 7 && n == 1 && f[0].disp_at == 2 && f[0].end == 7, "decoder: cmp byte [rip+d], 1 has its displacement before the immediate");
  Check(Decode({0x0F, 0x84, 0xFA, 0xFF, 0xFF, 0xFF}, f, &n) == 0, "decoder: a branch back into the moved bytes is refused");
  Check(hsf::slgame::PrologueLength(std::initializer_list<uint8_t>{0x8B, 0x05, 0x10, 0, 0, 0, 0, 0}.begin(), 5) == 0, "short list: does not know mov eax, [rip+d]");
}

void TestHooks() {
  auto* const page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
  // A: mov eax, [rip+d] (the 41 at page+0x100); add eax, 1; ret
  uint8_t* const a = page;
  const int32_t to_data = 0x100 - 6;
  a[0] = 0x8B;
  a[1] = 0x05;
  std::memcpy(a + 2, &to_data, 4);
  const uint8_t a_rest[] = {0x83, 0xC0, 0x01, 0xC3};
  std::memcpy(a + 6, a_rest, sizeof(a_rest));
  const int forty_one = 41;
  std::memcpy(page + 0x100, &forty_one, 4);
  // B, not 8-byte aligned: push rbx; mov eax, 7; pop rbx; ret
  uint8_t* const b = page + 0x203;
  const uint8_t b_code[] = {0x53, 0xB8, 0x07, 0x00, 0x00, 0x00, 0x5B, 0xC3};
  std::memcpy(b, b_code, sizeof(b_code));
  // C: as A, for the safe-try switch
  uint8_t* const c = page + 0x300;
  const int32_t c_to_data = 0x100 - 0x300 - 6;
  c[0] = 0x8B;
  c[1] = 0x05;
  std::memcpy(c + 2, &c_to_data, 4);
  std::memcpy(c + 6, a_rest, sizeof(a_rest));

  const auto call_a = reinterpret_cast<IntFn>(a), call_b = reinterpret_cast<IntFn>(b), call_c = reinterpret_cast<IntFn>(c);
  Check(call_a() == 42 && call_b() == 7 && call_c() == 42, "the functions alone give 42, 7, 42");
  Check(hsf::slgame::HookFunction("decoder_a", a, reinterpret_cast<void*>(&HookA), reinterpret_cast<void**>(&g_real_a), nullptr), "A hooked through the decoder");
  Check(call_a() == 52, "A: the moved load relative to rip still reads 41 (52 through HSF's hook)");
  Check(hsf::slgame::HookFunction("unaligned_b", b, reinterpret_cast<void*>(&HookB), reinterpret_cast<void**>(&g_real_b), nullptr), "B (not 8-byte aligned) hooked");
  Check(call_b() == 17, "B: 17 through HSF's hook");
  hsf::slgame::UnhookFunctions();
  Check(call_a() == 42 && call_b() == 7, "unhooked: the bytes are back");

  // A game where the decoder was in use when it went down: the decoder is off, and C is left alone as before.
  wchar_t temp[MAX_PATH], exe[MAX_PATH];
  GetTempPathW(MAX_PATH, temp);
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  const std::wstring file = std::wstring(temp) + L"hsf_hook_test_safe_try.ini";
  WritePrivateProfileStringW(std::wcsrchr(exe, L'\\') + 1, L"H3_decoder_off", L"1", file.c_str());
  hsf::safetry::Init(file.c_str(), nullptr);
  Check(!hsf::slgame::HookFunction("decoder_off_c", c, reinterpret_cast<void*>(&HookC), reinterpret_cast<void**>(&g_real_c), nullptr), "decoder off for this game: C left alone");
  Check(call_c() == 42, "C unchanged");
  DeleteFileW(file.c_str());
  hsf::slgame::UnhookFunctions();
}

}  // namespace

int main() {
  TestDecoder();
  TestHooks();
  std::printf(g_failures == 0 ? "all passed\n" : "%d FAILED\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
