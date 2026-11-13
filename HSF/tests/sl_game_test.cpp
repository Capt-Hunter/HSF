// Offline test of src/sl_game.cpp, no game needed: hooks a real sl.interposer.dll, sends it calls with a real D3D12 texture, and checks that results do not change, the log says the
// right things, and the bytes go back.   sl_game_test <path to sl.interposer.dll> <log path>   (exit code 0 = all passed)
#include "../src/sl_game.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_dlss.h>
#include <sl_reflex.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

int g_failures = 0;

void Check(bool ok, const char* what) {
  std::printf("%s %s\n", ok ? "  ok  " : "  FAIL", what);
  if (!ok) ++g_failures;
}

struct Token : sl::FrameToken {
  uint32_t value = 0;
  operator uint32_t() const override { return value; }
};

using SetTagFn = sl::Result (*)(const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
using SetTagForFrameFn = sl::Result (*)(const sl::FrameToken&, const sl::ViewportHandle&, const sl::ResourceTag*, uint32_t, sl::CommandBuffer*);
using SetConstantsFn = sl::Result (*)(const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&);
using EvaluateFn = sl::Result (*)(sl::Feature, const sl::FrameToken&, const sl::BaseStructure**, uint32_t, sl::CommandBuffer*);
using SetDlssgOptionsFn = sl::Result (*)(const sl::ViewportHandle&, const sl::DLSSGOptions&);

bool Has(const std::string& log, const char* text) { return log.find(text) != std::string::npos; }

// Witcher 3 with RenoDX DLSS: HSF hooks, RenoDX hooks on top, ReShade unloads and loads HSF again. RenoDX's copy of HSF's jump must never reach HSF's old code, and HSF must get back in.
using IntFn = int (*)();
IntFn g_real_int = nullptr;
int HookPlusTen() { return g_real_int() + 10; }

void TestHookUnderSomeoneElse() {
  auto* const page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
  uint8_t* const target = page;        // mov [rsp+8], rbx; mov eax, 1; ret
  const uint8_t code[] = {0x48, 0x89, 0x5c, 0x24, 0x08, 0xb8, 0x01, 0x00, 0x00, 0x00, 0xc3};
  std::memcpy(target, code, sizeof(code));
  const auto call = reinterpret_cast<IntFn>(target);
  Check(call() == 1, "reload: the function alone gives 1");
  Check(hsf::slgame::HookFunction("reload_test", target, reinterpret_cast<void*>(&HookPlusTen), reinterpret_cast<void**>(&g_real_int), nullptr), "reload: HSF hooks it");
  Check(call() == 11, "reload: through HSF's hook");
  // Someone else hooks on top: their trampoline is HSF's jump, moved, and the function now jumps to it.
  uint8_t* const theirs = page + 256;
  int32_t relative;
  std::memcpy(&relative, target + 1, 4);
  const uint8_t* const relay = target + 5 + relative;
  theirs[0] = 0xE9;
  const int32_t to_relay = static_cast<int32_t>(relay - (theirs + 5));
  std::memcpy(theirs + 1, &to_relay, 4);
  const int32_t to_theirs = static_cast<int32_t>(theirs - (target + 5));
  std::memcpy(target + 1, &to_theirs, 4);
  FlushInstructionCache(GetCurrentProcess(), page, 4096);
  Check(call() == 11, "reload: someone on top, still through HSF");
  hsf::slgame::UnhookFunctions();
  Check(target[0] == 0xE9 && std::memcmp(target + 1, &to_theirs, 4) == 0, "reload: their jump left in place");
  g_real_int = nullptr;  // what a fresh load of HSF has
  Check(call() == 1, "reload: with HSF gone, their call skips HSF and reaches the function");
  Check(hsf::slgame::HookFunction("reload_test", target, reinterpret_cast<void*>(&HookPlusTen), reinterpret_cast<void**>(&g_real_int), nullptr), "reload: HSF loaded again takes its relay back");
  Check(call() == 11, "reload: through HSF's hook again, under theirs");
  hsf::slgame::UnhookFunctions();
  Check(call() == 1, "reload: unhooked again, skips HSF");
  wchar_t value[8];
  Check(GetEnvironmentVariableW(L"HSF_HOOK_reload_test", value, 8) != 0, "reload: the relay stays known for the next load");
  VirtualFree(page, 0, MEM_RELEASE);
  SetEnvironmentVariableW(L"HSF_HOOK_reload_test", nullptr);
}

// Jusant with RenoDX DLSS: its hook on NGX is there before HSF's. HSF goes on top, runs first, then theirs.
void TestHookOnTopOfSomeoneElse() {
  auto* const page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
  uint8_t* const target = page;
  uint8_t* const theirs = page + 512;  // their hook: mov eax, 100; ret
  const uint8_t their_code[] = {0xb8, 0x64, 0x00, 0x00, 0x00, 0xc3};
  std::memcpy(theirs, their_code, sizeof(their_code));
  target[0] = 0xE9;
  const int32_t to_theirs = static_cast<int32_t>(theirs - (target + 5));
  std::memcpy(target + 1, &to_theirs, 4);
  target[5] = 0xCC;
  FlushInstructionCache(GetCurrentProcess(), page, 4096);
  const auto call = reinterpret_cast<IntFn>(target);
  Check(call() == 100, "on top: their hook alone gives 100");
  g_real_int = nullptr;
  Check(hsf::slgame::HookFunction("on_top_test", target, reinterpret_cast<void*>(&HookPlusTen), reinterpret_cast<void**>(&g_real_int), nullptr), "on top: HSF hooks over their jump");
  Check(call() == 110, "on top: HSF first, then theirs");
  hsf::slgame::UnhookFunctions();
  Check(target[0] == 0xE9 && std::memcmp(target + 1, &to_theirs, 4) == 0, "on top: their jump back as it was");
  Check(call() == 100, "on top: unhooked, their hook alone again");
  VirtualFree(page, 0, MEM_RELEASE);
  SetEnvironmentVariableW(L"HSF_HOOK_on_top_test", nullptr);
}

void TestPrologues() {
  const uint8_t seen_set_tag[] = {0x40, 0x55, 0x56, 0x57, 0x41, 0x56, 0x48, 0x81, 0xEC};
  Check(hsf::slgame::PrologueLength(seen_set_tag, 5) == 6, "prologue: pushes stop at the first whole instruction past 5 bytes");
  const uint8_t seen_mov[] = {0x48, 0x89, 0x5C, 0x24, 0x08, 0x57};
  Check(hsf::slgame::PrologueLength(seen_mov, 5) == 5, "prologue: mov [rsp+8], rbx");
  const uint8_t seen_sub[] = {0x48, 0x83, 0xEC, 0x28, 0x4C, 0x8B, 0xC2, 0x8B};
  Check(hsf::slgame::PrologueLength(seen_sub, 5) == 7, "prologue: sub rsp then mov r8, rdx");
  const uint8_t relative[] = {0x48, 0x83, 0xEC, 0x58, 0x48, 0x8B, 0x05, 0x15, 0x5E, 0x03, 0x00, 0x48};
  size_t rip_at[hsf::slgame::MAX_RIP_LOADS];
  size_t rip_count = 0;
  Check(hsf::slgame::PrologueLength(relative, 5, rip_at, &rip_count) == 11 && rip_count == 1 && rip_at[0] == 4, "prologue: the start of slDLSSGSetOptions, with its load relative to rip");
  const uint8_t forwarder[] = {0x33, 0xD2, 0xE9, 0x09, 0xDC, 0xFF, 0xFF, 0xCC};
  rip_count = 0;
  Check(hsf::slgame::PrologueLength(forwarder, 5, rip_at, &rip_count) == 7 && rip_count == 1 && rip_at[0] == 2, "prologue: the start of slReflexSetOptions, a forwarding jump");
  const uint8_t short_jump[] = {0x50, 0xE9, 0x00, 0x00, 0x00, 0x00, 0xCC};
  Check(hsf::slgame::PrologueLength(short_jump, 7) == 0, "prologue: a jump that ends too soon for the patch refused");
  const uint8_t bad_call[] = {0xE8, 0x00, 0x10, 0x00, 0x00, 0x90};
  Check(hsf::slgame::PrologueLength(bad_call, 5) == 0, "prologue: a call refused");
  const uint8_t hooked[] = {0xE9, 0x00, 0x10, 0x00, 0x00, 0x90};
  rip_count = 0;
  Check(hsf::slgame::PrologueLength(hooked, 5, rip_at, &rip_count) == 5 && rip_count == 1 && rip_at[0] == 0, "prologue: a function already hooked by someone else, its jump moved");
}

// ---- Driving, against a stand-in for DLSS-G's slDLSSGSetOptions ----

sl::DLSSGMode g_fake_mode = sl::DLSSGMode::eCount;
uint32_t g_fake_frames = 0, g_fake_flags = 0, g_fake_calls = 0, g_fake_optimal_width = 0, g_fake_optimal_height = 0;
size_t g_fake_version = 0;

sl::Result FakeSetOptions(const sl::ViewportHandle&, const sl::DLSSGOptions& options) {
  g_fake_mode = options.mode;
  g_fake_frames = options.numFramesToGenerate;
  g_fake_flags = static_cast<uint32_t>(options.flags);
  g_fake_optimal_width = options.dynamicResWidth;
  g_fake_optimal_height = options.dynamicResHeight;
  g_fake_version = options.structVersion;
  ++g_fake_calls;
  return sl::Result::eOk;
}

// Machine code that starts as slDLSSGSetOptions does in the game: sub rsp, 0x58; mov rax, [rip+x]; add rsp, 0x58; jmp rax. It only reaches FakeSetOptions if the moved load still finds x.
sl::Result FakeSetDlss(const sl::ViewportHandle&, const sl::DLSSOptions&) { return sl::Result::eOk; }

uint8_t* MakeStub(const void* fake = reinterpret_cast<const void*>(&FakeSetOptions)) {
  uint8_t* const page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
  const uint8_t code[] = {0x48, 0x83, 0xEC, 0x58, 0x48, 0x8B, 0x05, 0, 0, 0, 0, 0x48, 0x83, 0xC4, 0x58, 0xFF, 0xE0};
  std::memcpy(page, code, sizeof(code));
  const int32_t displacement = 0x100 - 11;
  std::memcpy(page + 7, &displacement, 4);
  std::memcpy(page + 0x100, &fake, sizeof(fake));
  FlushInstructionCache(GetCurrentProcess(), page, 4096);
  return page;
}

sl::ReflexMode g_fake_reflex = sl::ReflexMode::eLowLatencyWithBoost;
uint64_t g_fake_rdx = 1;

sl::Result FakeReflexOptions(const sl::ReflexOptions& options, uint64_t rdx) {
  g_fake_reflex = options.mode;
  g_fake_rdx = rdx;
  return sl::Result::eOk;
}

// Machine code that starts as slReflexSetOptions does in the game: xor edx, edx; jmp rel32. The jump lands on code that goes on to FakeReflexOptions.
uint8_t* MakeReflexStub() {
  uint8_t* const page = static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
  const uint8_t code[] = {0x33, 0xD2, 0xE9, 0, 0, 0, 0, 0xCC};
  std::memcpy(page, code, sizeof(code));
  const int32_t displacement = 0x100 - 7;
  std::memcpy(page + 3, &displacement, 4);
  const uint8_t jump[] = {0xFF, 0x25, 0, 0, 0, 0};  // jmp [rip+0]
  std::memcpy(page + 0x100, jump, sizeof(jump));
  const void* const fake = reinterpret_cast<const void*>(&FakeReflexOptions);
  std::memcpy(page + 0x106, &fake, sizeof(fake));
  FlushInstructionCache(GetCurrentProcess(), page, 4096);
  return page;
}

// A point seen by two cameras: the rebuilt matrix must take where the first sees it to where the second does.
void Project(const float world[3], const hsf::slgame::CameraPose& pose, const float view_to_clip[4][4], float out[4]) {
  float d[3], view[4];
  for (int i = 0; i < 3; ++i) d[i] = world[i] - pose.pos[i];
  view[0] = d[0] * pose.right[0] + d[1] * pose.right[1] + d[2] * pose.right[2];
  view[1] = d[0] * pose.up[0] + d[1] * pose.up[1] + d[2] * pose.up[2];
  view[2] = -(d[0] * pose.fwd[0] + d[1] * pose.fwd[1] + d[2] * pose.fwd[2]);
  view[3] = 1.0f;
  for (int j = 0; j < 4; ++j) {
    out[j] = 0.0f;
    for (int i = 0; i < 4; ++i) out[j] += view[i] * view_to_clip[i][j];
  }
}

void TestCameraRebuild() {
  // Shadows' own projection and inverse, as logged: depth inverted, near 0.1.
  const float p[4][4] = {{1.1517f, 0, 0, 0}, {0, 2.7514f, 0, 0}, {0, 0, -1.00008f, -1}, {0, 0, -0.100008f, 0}};
  const float pi[4][4] = {{1 / 1.1517f, 0, 0, 0}, {0, 1 / 2.7514f, 0, 0}, {0, 0, 0, -1 / 0.100008f}, {0, 0, -1, 1.00008f / 0.100008f}};
  // World positions of the size seen in the game, z up, and a heading turned a little between the frames.
  const hsf::slgame::CameraPose cur = {{0.8f, 0.6f, 0}, {0, 0, 1}, {-0.6f, 0.8f, 0}, {-5592.1f, -2558.8f, 6.05f}};
  const float c = std::cos(0.02f), s = std::sin(0.02f);
  const hsf::slgame::CameraPose prev = {{0.8f * c - 0.6f * s, 0.6f * c + 0.8f * s, 0}, {0, 0, 1}, {-0.6f * c - 0.8f * s, 0.8f * c - 0.6f * s, 0}, {-5592.4f, -2558.5f, 6.05f}};

  float same[4][4];
  Check(hsf::slgame::RebuildClipToPrev(pi, cur, p, cur, same), "camera: rebuilt with no move");
  bool identity = true;
  for (int i = 0; i < 4; ++i)
    for (int j = 0; j < 4; ++j) identity = identity && std::fabs(same[i][j] - (i == j ? 1.0f : 0.0f)) < 2e-3f;
  Check(identity, "camera: no move gives identity");

  float to_prev[4][4], from_prev[4][4];
  Check(hsf::slgame::RebuildClipToPrev(pi, cur, p, prev, to_prev) && hsf::slgame::RebuildClipToPrev(pi, prev, p, cur, from_prev), "camera: rebuilt both ways with a move");
  const float world[3] = {-5592.1f - 0.6f * 40 + 0.8f * 5, -2558.8f + 0.8f * 40 + 0.6f * 5, 8.0f};  // 40 ahead, 5 right, 2 up
  float now[4], before[4], moved[4] = {}, back[4] = {};
  Project(world, cur, p, now);
  Project(world, prev, p, before);
  for (int j = 0; j < 4; ++j)
    for (int i = 0; i < 4; ++i) {
      moved[j] += now[i] * to_prev[i][j];
      back[j] += before[i] * from_prev[i][j];
    }
  Check(std::fabs(moved[0] / moved[3] - before[0] / before[3]) < 1e-3f && std::fabs(moved[1] / moved[3] - before[1] / before[3]) < 1e-3f, "camera: a point goes to where the previous camera saw it");
  Check(std::fabs(back[0] / back[3] - now[0] / now[3]) < 1e-3f && std::fabs(back[1] / back[3] - now[1] / now[3]) < 1e-3f, "camera: and back, with prevClipToClip");
  Check(std::fabs(moved[0] / moved[3] - now[0] / now[3]) > 0.01f, "camera: the test's move is a real one");

  hsf::slgame::CameraPose bad = cur;
  bad.right[0] = 3.0f;
  float untouched[4][4] = {};
  Check(!hsf::slgame::RebuildClipToPrev(pi, bad, p, prev, untouched) && untouched[0][0] == 0.0f, "camera: axes that are not sound are refused");
  hsf::slgame::CameraPose mirrored = cur;
  for (float& v : mirrored.fwd) v = -v;
  Check(!hsf::slgame::RebuildClipToPrev(pi, mirrored, p, prev, untouched), "camera: a mirrored camera is refused");
}

void TestDrive(sl::Result (*set_loaded)(sl::Feature, bool), EvaluateFn evaluate, ID3D12Device* device, ID3D12Resource* texture, ID3D12Resource* other_size, const wchar_t* log) {
  ComPtr<ID3D12CommandAllocator> allocator;
  device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator));
  ComPtr<ID3D12GraphicsCommandList> list;
  device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list));

  hsf::slgame::Start(log, true);
  hsf::slgame::SetGeneration(true, 2);
  set_loaded(sl::kFeatureDLSS_G, false);
  uint8_t* const stub = MakeStub();
  const auto set_options = reinterpret_cast<SetDlssgOptionsFn>(stub);
  Check(hsf::slgame::HookFeatureFunctionForTest("slDLSSGSetOptions", stub), "drive: hook on a start like slDLSSGSetOptions");
  Check(stub[0] == 0xE9, "drive: it now starts with the jump");

  uint8_t* const reflex_stub = MakeReflexStub();
  const auto set_reflex = reinterpret_cast<sl::Result (*)(const sl::ReflexOptions&)>(reflex_stub);
  Check(hsf::slgame::HookFeatureFunctionForTest("slReflexSetOptions", reflex_stub), "drive: hook on a start like slReflexSetOptions");
  sl::ReflexOptions game_reflex{};
  game_reflex.mode = sl::ReflexMode::eOff;
  set_reflex(game_reflex);
  Check(g_fake_reflex == sl::ReflexMode::eLowLatency, "drive: Reflex off in the game becomes low latency");
  Check(g_fake_rdx == 0, "drive: the moved xor and jump ran");
  game_reflex.mode = sl::ReflexMode::eLowLatencyWithBoost;
  set_reflex(game_reflex);
  Check(g_fake_reflex == sl::ReflexMode::eLowLatencyWithBoost, "drive: the game's own Reflex choice stands when it is on");

  sl::ViewportHandle viewport(0);
  uint8_t* const dlss_stub = MakeStub(reinterpret_cast<const void*>(&FakeSetDlss));
  const auto set_dlss = reinterpret_cast<sl::Result (*)(const sl::ViewportHandle&, const sl::DLSSOptions&)>(dlss_stub);
  Check(hsf::slgame::HookFeatureFunctionForTest("slDLSSSetOptions", dlss_stub), "drive: hook on a start like slDLSSSetOptions");
  sl::DLSSOptions game_dlss{};
  game_dlss.outputWidth = 3440;
  game_dlss.outputHeight = 1440;
  set_dlss(viewport, game_dlss);
  game_dlss.outputWidth = game_dlss.outputHeight = 4294967295u;  // Shadows sends this with mode 0
  set_dlss(viewport, game_dlss);

  sl::DLSSGOptions game{};
  game.structVersion = sl::kStructVersion3;
  game.mode = sl::DLSSGMode::eOff;
  game.numFramesToGenerate = 1;
  set_options(viewport, game);
  Check(g_fake_calls == 1 && g_fake_mode == sl::DLSSGMode::eOff, "drive: moved load works, and no vectors yet means off");
  Check(g_fake_version == sl::kStructVersion3, "drive: the game's struct version is kept");
  hsf::slgame::Poll();
  Check(g_fake_calls == 1, "drive: nothing sent while there are no vectors");

  Token token;
  sl::Resource mv(sl::ResourceType::eTex2d, texture, static_cast<uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
  sl::Resource depth(sl::ResourceType::eTex2d, texture, static_cast<uint32_t>(D3D12_RESOURCE_STATE_GENERIC_READ));
  sl::ResourceTag mv_tag(&mv, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent);
  sl::ResourceTag depth_tag(&depth, sl::kBufferTypeDepth, sl::eValidUntilPresent);
  const sl::BaseStructure* inputs[] = {&viewport, &depth_tag, &mv_tag};
  for (uint32_t i = 0; i < 12; ++i) {
    token.value = 100 + i;
    evaluate(sl::kFeatureDLSS, token, inputs, 3, reinterpret_cast<sl::CommandBuffer*>(list.Get()));
  }
  Check(hsf::slgame::GetStats().tagged == 0, "drive: Streamline not started, so its tag call failed and nothing counts as tagged");
  hsf::slgame::Poll();
  Check(g_fake_mode == sl::DLSSGMode::eOn && g_fake_frames == 2, "drive: after a run of evaluates, on with HSF's frame count");
  Check(g_fake_version == sl::kStructVersion3, "drive: HSF's own call has the game's struct version");
  Check((g_fake_flags & static_cast<uint32_t>(sl::DLSSGFlags::eRetainResourcesWhenOff)) != 0, "drive: resources kept when off");
  Check((g_fake_flags & static_cast<uint32_t>(sl::DLSSGFlags::eDynamicResolutionEnabled)) == 0, "drive: no dynamic resolution flag while the size holds");
  Check(hsf::slgame::GetStats().generating, "drive: says it is generating");

  set_options(viewport, game);
  Check(g_fake_mode == sl::DLSSGMode::eOn, "drive: the game saying off does not turn it off");

  sl::Resource mv_small(sl::ResourceType::eTex2d, other_size, static_cast<uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
  sl::ResourceTag mv_small_tag(&mv_small, sl::kBufferTypeMotionVectors, sl::eValidUntilPresent);
  const sl::BaseStructure* small_inputs[] = {&viewport, &depth_tag, &mv_small_tag};
  evaluate(sl::kFeatureDLSS, token, small_inputs, 3, reinterpret_cast<sl::CommandBuffer*>(list.Get()));
  hsf::slgame::Poll();
  Check((g_fake_flags & static_cast<uint32_t>(sl::DLSSGFlags::eDynamicResolutionEnabled)) != 0, "drive: vectors changing size turn the dynamic resolution flag on");
  Check(g_fake_optimal_width == 2580 && g_fake_optimal_height == 1080, "drive: optimal size is 3/4 of the real output, and a nonsense output is ignored");

  hsf::slgame::SetGeneration(false, 2);
  hsf::slgame::Poll();
  Check(g_fake_mode == sl::DLSSGMode::eOff, "drive: not allowed means off");
  hsf::slgame::SetGeneration(true, 3);
  hsf::slgame::Poll();
  Check(g_fake_mode == sl::DLSSGMode::eOn && g_fake_frames == 3, "drive: allowed again, with a new frame count");

  hsf::slgame::SetGeneration(true, 2);
  hsf::slgame::Poll();
  Check(g_fake_mode == sl::DLSSGMode::eOff && g_fake_frames == 3, "drive: a new frame count while generating goes off first");
  hsf::slgame::Poll();
  Check(g_fake_mode == sl::DLSSGMode::eOn && g_fake_frames == 2, "drive: then on with the new count, on the next frame");

  // Quick off: a gap much shorter than EVALUATE_GONE_MS stops generation, only with the option on.
  for (uint32_t i = 0; i < 12; ++i) evaluate(sl::kFeatureDLSS, token, inputs, 3, reinterpret_cast<sl::CommandBuffer*>(list.Get()));
  Sleep(100);
  hsf::slgame::Poll();
  Check(g_fake_mode == sl::DLSSGMode::eOn, "drive: without quick off a 100 ms gap leaves generation on");
  hsf::slgame::SetQuickOff(true);
  hsf::slgame::Poll();
  Check(g_fake_mode == sl::DLSSGMode::eOff, "drive: with quick off a 100 ms gap stops generation");
  evaluate(sl::kFeatureDLSS, token, inputs, 3, reinterpret_cast<sl::CommandBuffer*>(list.Get()));
  hsf::slgame::Poll();
  Check(g_fake_mode == sl::DLSSGMode::eOn, "drive: after a short gap it comes straight back, with no new run of evaluates");
  hsf::slgame::SetQuickOff(false);

  // The HUD-free picture goes through the same tag calls.
  hsf::slgame::SetGeneration(false, 1);
  hsf::slgame::Poll();
  Check(!hsf::slgame::TagHudless(list.Get(), texture, 64, 64), "hud: nothing is tagged while not generating");
  hsf::slgame::SetGeneration(true, 2);
  for (int i = 0; i < 12; ++i) evaluate(sl::kFeatureDLSS, token, inputs, 3, reinterpret_cast<sl::CommandBuffer*>(list.Get()));
  hsf::slgame::Poll();
  Check(hsf::slgame::GetStats().generating, "hud: generating again");
  Check(!hsf::slgame::TagHudless(list.Get(), texture, 64, 64), "hud: Streamline not started, so its tag call failed and the picture does not count as handed over");
  hsf::slgame::ClearHudless();

  // The score: only logs until the guard is on.
  for (int i = 0; i < 7; ++i) hsf::slgame::NoteScore(0.03f, 0.01f, 0.1f, 0.8f, 0.5f, 0);
  Check(!hsf::slgame::GetStats().vectors_doubted, "score: too few pairs doubt nothing");
  hsf::slgame::NoteScore(0.03f, 0.01f, 0.1f, 0.8f, 0.5f, 1);
  hsf::slgame::NoteScore(0.03f, 0.01f, 0.1f, 0.8f, 0.5f, 2);
  Check(hsf::slgame::GetStats().score_pairs == 7, "score: still pairs and cuts do not count");
  hsf::slgame::NoteScore(0.03f, 0.01f, 0.1f, 0.8f, 0.5f, 0);
  Check(hsf::slgame::GetStats().vectors_doubted, "score: a full window of bad pairs doubts the vectors");
  evaluate(sl::kFeatureDLSS, token, inputs, 3, reinterpret_cast<sl::CommandBuffer*>(list.Get()));
  hsf::slgame::Poll();
  Check(g_fake_mode == sl::DLSSGMode::eOn, "score: with the guard off, doubt changes nothing");
  hsf::slgame::SetScoreGuard(true);
  evaluate(sl::kFeatureDLSS, token, inputs, 3, reinterpret_cast<sl::CommandBuffer*>(list.Get()));
  hsf::slgame::Poll();
  Check(g_fake_mode == sl::DLSSGMode::eOff, "score: with the guard on, doubt stops generation");
  for (int i = 0; i < 8; ++i) hsf::slgame::NoteScore(0.002f, 0.01f, 0.8f, 0.1f, 0.5f, 0);
  evaluate(sl::kFeatureDLSS, token, inputs, 3, reinterpret_cast<sl::CommandBuffer*>(list.Get()));
  hsf::slgame::Poll();
  Check(!hsf::slgame::GetStats().vectors_doubted && g_fake_mode == sl::DLSSGMode::eOn, "score: vectors that help again start it again");
  for (int i = 0; i < 8; ++i) hsf::slgame::NoteScore(0.002f, 0.01f, 0.8f, 0.1f, 0.01f, 0);  // the window holds only pairs with little moving before the bad ones arrive
  for (int i = 0; i < 8; ++i) hsf::slgame::NoteScore(0.03f, 0.01f, 0.1f, 0.8f, 0.01f, 0);
  Check(!hsf::slgame::GetStats().vectors_doubted, "score: a bad ratio with almost nothing moving doubts nothing");
  hsf::slgame::SetScoreGuard(false);

  // The camera: HSF's reset, and the picture check against it.
  {
    const auto set_constants = reinterpret_cast<SetConstantsFn>(GetProcAddress(GetModuleHandleW(L"sl.interposer.dll"), "slSetConstants"));
    sl::Constants c{};
    c.cameraFwd = {0.f, 1.f, 0.f};
    c.cameraRight = {1.f, 0.f, 0.f};
    c.cameraUp = {0.f, 0.f, 1.f};
    c.cameraPos = {0.f, 0.f, 0.f};
    c.reset = sl::Boolean::eFalse;
    const auto frame_with = [&](uint32_t value) {
      token.value = value;
      set_constants(c, token, viewport);
    };
    frame_with(500);
    c.cameraPos.y = 0.3f;
    frame_with(501);
    Check(hsf::slgame::GetStats().cuts_by_hsf == 0, "cut: an ordinary step is not a cut");
    hsf::slgame::NotePictureCut(0.9f, 1.0f);
    Check(hsf::slgame::GetStats().picture_cuts == 1 && hsf::slgame::GetStats().picture_cuts_no_reset == 1, "cut: the picture cutting with no reset near it is counted");
    c.cameraFwd = {1.f, 0.f, 0.f};
    c.cameraRight = {0.f, -1.f, 0.f};
    frame_with(502);
    Check(hsf::slgame::GetStats().cuts_by_hsf == 1, "cut: a turn of 90 degrees in a frame is a cut");
    frame_with(502);
    Check(hsf::slgame::GetStats().cuts_by_hsf == 1, "cut: the same frame sent again counts once");
    hsf::slgame::NotePictureCut(0.9f, 1.0f);
    Check(hsf::slgame::GetStats().picture_cuts_no_reset == 1, "cut: a picture cut near a reset is not a miss");
    c.cameraPos.y = 5000.f;
    frame_with(503);
    Check(hsf::slgame::GetStats().cuts_by_hsf == 2, "cut: a long jump is a cut");
    c.cameraPos.y = 9000.f;
    c.reset = sl::Boolean::eTrue;
    frame_with(504);
    Check(hsf::slgame::GetStats().cuts_by_hsf == 2, "cut: a jump with the game's own reset needs nothing from HSF");
    c.reset = sl::Boolean::eFalse;
    hsf::slgame::SetCutCheck(false);
    c.cameraPos.y = 20000.f;
    frame_with(505);
    Check(hsf::slgame::GetStats().cuts_by_hsf == 2, "cut: off means off");
    hsf::slgame::SetCutCheck(true);
  }

  // The vectors reach the checks, with their state and the extent filled in.
  {
    static uint32_t s_seen = 0, s_state = 0, s_extent[4] = {};
    static void* s_motion = nullptr;
    hsf::slgame::SetVectorsHook([](void*, void* motion, uint32_t state, const uint32_t extent[4], const float*) {
      ++s_seen;
      s_motion = motion;
      s_state = state;
      std::memcpy(s_extent, extent, sizeof(s_extent));
    });
    evaluate(sl::kFeatureDLSS, token, inputs, 3, reinterpret_cast<sl::CommandBuffer*>(list.Get()));
    hsf::slgame::SetVectorsHook(nullptr);
    const D3D12_RESOURCE_DESC desc = texture->GetDesc();
    Check(s_seen == 1 && s_motion == texture && s_state == static_cast<uint32_t>(D3D12_RESOURCE_STATE_UNORDERED_ACCESS) && s_extent[2] == desc.Width && s_extent[3] == desc.Height, "drive: the checks get each frame's vectors once, with state and size");
  }

  Sleep(400);  // past EVALUATE_GONE_MS: a menu
  hsf::slgame::Poll();
  Check(g_fake_mode == sl::DLSSGMode::eOff, "drive: evaluates stopping means off");
  evaluate(sl::kFeatureDLSS, token, inputs, 3, reinterpret_cast<sl::CommandBuffer*>(list.Get()));
  hsf::slgame::Poll();
  Check(g_fake_mode == sl::DLSSGMode::eOff, "drive: one evaluate is not enough to start again");
  for (uint32_t i = 0; i < 12; ++i) evaluate(sl::kFeatureDLSS, token, inputs, 3, reinterpret_cast<sl::CommandBuffer*>(list.Get()));
  hsf::slgame::Poll();
  Check(g_fake_mode == sl::DLSSGMode::eOn, "drive: a run of evaluates starts it again");

  hsf::slgame::Stop(false);
  Check(g_fake_mode == sl::DLSSGMode::eOff, "drive: stopping takes generation off");
  Check(reflex_stub[0] == 0x33, "drive: the Reflex stub's bytes are back");
  {
    std::ifstream file(log, std::ios::binary);
    std::stringstream text;
    text << file.rdbuf();
    Check(Has(text.str(), "this game does not tag by frame"), "drive: a refused frame tag moves to slSetTag");
    Check(Has(text.str(), "generating 1"), "drive: a size change is logged with whether generation is on");
    Check(Has(text.str(), "frame generation is off in HSF"), "drive: off is logged with HSF's switch as its reason");
    Check(Has(text.str(), "no vectors for"), "drive: off is logged with missing vectors as its reason");
    Check(Has(text.str(), "for a new frame count"), "drive: off is logged with the frame count as its reason");
    Check(Has(text.str(), "evaluates back after"), "drive: the length of a gap in evaluates is logged");
    Check(Has(text.str(), "HSF sets the reset"), "cut: logged");
    Check(Has(text.str(), "with NO reset near it"), "cut: a picture cut with no reset is logged");
    Check(Has(text.str(), "the score doubts the vectors"), "score: off is logged with the score as its reason");
    Check(Has(text.str(), "slSetFeatureLoaded DLSS-G(1000) false, HSF keeps it loaded"), "drive: the game cannot unload DLSS-G's plugin");
  }
  Check(stub[0] == 0x48, "drive: the stub's bytes are back");
  game.mode = sl::DLSSGMode::eOn;
  game.numFramesToGenerate = 1;
  set_options(viewport, game);
  Check(g_fake_mode == sl::DLSSGMode::eOn && g_fake_frames == 1, "drive: after Stop the game's call goes through unchanged");

  // A game an earlier run saw changing size: the flag is there the first time generation goes on, with no change of size in this run.
  hsf::slgame::Start(log, true, true);
  hsf::slgame::SetGeneration(true, 2);
  Check(hsf::slgame::HookFeatureFunctionForTest("slDLSSGSetOptions", stub), "drive: hook again for a run that knows");
  set_options(viewport, game);
  for (uint32_t i = 0; i < 12; ++i) {
    token.value = 300 + i;
    evaluate(sl::kFeatureDLSS, token, inputs, 3, reinterpret_cast<sl::CommandBuffer*>(list.Get()));
  }
  hsf::slgame::Poll();
  Check(g_fake_mode == sl::DLSSGMode::eOn && (g_fake_flags & static_cast<uint32_t>(sl::DLSSGFlags::eDynamicResolutionEnabled)) != 0, "drive: known dynamic resolution has the flag from the first time on");
  hsf::slgame::Stop(false);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  TestCameraRebuild();
  if (argc < 3) {
    std::printf("usage: sl_game_test <sl.interposer.dll> <log path>\n");
    return 2;
  }
  TestPrologues();
  TestHookUnderSomeoneElse();
  TestHookOnTopOfSomeoneElse();
  DeleteFileW(argv[2]);

  const HMODULE module = LoadLibraryW(argv[1]);
  Check(module != nullptr, "interposer loads");
  if (module == nullptr) return 1;
  const auto set_tag = reinterpret_cast<SetTagFn>(GetProcAddress(module, "slSetTag"));
  const auto tag_for_frame = reinterpret_cast<SetTagForFrameFn>(GetProcAddress(module, "slSetTagForFrame"));
  const auto constants = reinterpret_cast<SetConstantsFn>(GetProcAddress(module, "slSetConstants"));
  const auto evaluate = reinterpret_cast<EvaluateFn>(GetProcAddress(module, "slEvaluateFeature"));
  Check(set_tag && tag_for_frame && constants && evaluate, "exports found");

  ComPtr<IDXGIFactory4> factory;
  CreateDXGIFactory1(IID_PPV_ARGS(&factory));
  ComPtr<IDXGIAdapter> warp;
  factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
  ComPtr<ID3D12Device> device;
  Check(SUCCEEDED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "WARP device");
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = 320;
  desc.Height = 180;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = DXGI_FORMAT_R16G16_FLOAT;
  desc.SampleDesc.Count = 1;
  ComPtr<ID3D12Resource> texture;
  Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&texture))), "texture");

  Token token;
  token.value = 41;
  sl::ViewportHandle viewport(3);
  sl::Resource resource(sl::ResourceType::eTex2d, texture.Get(), static_cast<uint32_t>(D3D12_RESOURCE_STATE_COMMON));
  sl::Extent extent{};
  extent.left = 2;
  extent.top = 4;
  extent.width = 300;
  extent.height = 170;
  sl::ResourceTag tags[] = {sl::ResourceTag(&resource, sl::kBufferTypeMotionVectors, sl::eOnlyValidNow, &extent), sl::ResourceTag(nullptr, sl::kBufferTypeUIColorAndAlpha, sl::eValidUntilPresent)};
  sl::Constants values{};
  values.jitterOffset = sl::float2(0.25f, -0.25f);
  values.mvecScale = sl::float2(1.f / 320.f, 1.f / 180.f);
  values.cameraNear = 0.5f;
  values.cameraFar = 5000.f;
  values.reset = sl::Boolean::eTrue;
  const sl::BaseStructure* inputs[] = {&viewport, &tags[0]};

  const auto call_all = [&](sl::Result out[4]) {
    out[0] = set_tag(viewport, tags, 2, nullptr);
    out[1] = tag_for_frame(token, viewport, tags, 2, nullptr);
    out[2] = constants(values, token, viewport);
    out[3] = evaluate(sl::kFeatureDLSS, token, inputs, 2, nullptr);
  };

  sl::Result before[4], during[4], after[4];
  uint8_t bytes_before[8];
  std::memcpy(bytes_before, reinterpret_cast<const void*>(constants), 8);
  call_all(before);
  std::printf("  results without hooks: %d %d %d %d\n", static_cast<int>(before[0]), static_cast<int>(before[1]), static_cast<int>(before[2]), static_cast<int>(before[3]));

  hsf::slgame::Start(argv[2], false);
  std::printf("  hooks in place: %d\n", hsf::slgame::HooksInstalled());
  Check(hsf::slgame::HooksInstalled() == 7, "all seven export hooks in place");
  Check(*reinterpret_cast<const uint8_t*>(constants) == 0xE9, "slSetConstants now starts with the jump");
  call_all(during);
  // Still alive with a texture that is not a texture.
  sl::Resource bad(sl::ResourceType::eTex2d, reinterpret_cast<void*>(0x10), 0u);
  sl::ResourceTag bad_tag(&bad, sl::kBufferTypeDepth, sl::eValidUntilPresent);
  set_tag(viewport, &bad_tag, 1, nullptr);
  hsf::slgame::Poll();
  for (int i = 0; i < 4; ++i) Check(during[i] == before[i], "same result with hooks on");
  hsf::slgame::Stop(false);
  Check(hsf::slgame::HooksInstalled() == 0, "no hooks after Stop");
  Check(std::memcmp(bytes_before, reinterpret_cast<const void*>(constants), 8) == 0, "game's bytes back after Stop");
  call_all(after);
  for (int i = 0; i < 4; ++i) Check(after[i] == before[i], "same result after Stop");

  // A reload, as ReShade does during startup.
  hsf::slgame::Start(argv[2], false);
  Check(hsf::slgame::HooksInstalled() == 7, "hooks go back on after a reload");
  call_all(during);
  for (int i = 0; i < 4; ++i) Check(during[i] == before[i], "same result after a reload");
  hsf::slgame::Stop(false);
  Check(std::memcmp(bytes_before, reinterpret_cast<const void*>(constants), 8) == 0, "game's bytes back again");

  std::ifstream file(argv[2], std::ios::binary);
  std::stringstream text;
  text << file.rdbuf();
  const std::string log = text.str();
  file.close();  // the next Start writes to this file
  Check(Has(log, "slSetTag vp3 MotionVectors(1): 320x180 dxgi 34"), "log: tag with the texture's real size and format");
  Check(Has(log, "OnlyValidNow extent 2,4 300x170"), "log: lifecycle and render rectangle");
  Check(Has(log, "slSetTagForFrame vp3 MotionVectors(1)"), "log: frame tag");
  Check(Has(log, "UIColorAndAlpha(23): no resource"), "log: a tag with no resource");
  Check(Has(log, "slSetConstants #1 vp3 token 41 struct v2"), "log: constants with token and version");
  Check(Has(log, "jitter 0.25000 -0.25000"), "log: jitter");
  Check(Has(log, "near 0.5 far 5000"), "log: near and far");
  Check(Has(log, "slEvaluateFeature DLSS(0) token 41 inputs 2"), "log: evaluate");
  Check(Has(log, "slEvaluateFeature input vp3 MotionVectors(1)"), "log: a tag given with evaluate");
  Check(Has(log, "Depth(0)") && Has(log, "(no desc)"), "log: a bad texture is survived and marked");
  Check(Has(log, "game's bytes put back"), "log: stop");

  desc.Width = 160;
  desc.Height = 90;
  ComPtr<ID3D12Resource> small_texture;
  Check(SUCCEEDED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&small_texture))), "smaller texture");
  const auto set_loaded = reinterpret_cast<sl::Result (*)(sl::Feature, bool)>(GetProcAddress(module, "slSetFeatureLoaded"));
  Check(set_loaded != nullptr, "slSetFeatureLoaded found");
  TestDrive(set_loaded, evaluate, device.Get(), texture.Get(), small_texture.Get(), argv[2]);

  std::printf(g_failures == 0 ? "all passed\n" : "%d FAILED\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
