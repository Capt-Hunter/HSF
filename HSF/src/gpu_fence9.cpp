// See gpu_fence9.h. Ported from RenoDX's src/utils/gpu_fence.hpp (MIT, Copyright (C) 2026 Carlos Lopez).

#include "gpu_fence9.h"

#include <d3d11_4.h>
#include <emmintrin.h>
#include <winternl.h>
#include <wrl/client.h>

// The D3DKMT declarations need NTSTATUS from winternl.h first.
#include <d3dkmthk.h>

#include <MinHook.h>

#include <cstdint>
#include <algorithm>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace hsf::fence9 {
namespace {

// ---- The kernel handles of a D3D9Ex device, read from the kernel calls it makes ----
struct Kernel {
  D3DKMT_HANDLE device = 0;
  D3DKMT_HANDLE context = 0;       // the one D3D9 submits on
  D3DKMT_HANDLE contexts[16]{};    // every context of the device seen (the driver may have more than one)
  size_t count = 0;
  bool ambiguous = false;
};
thread_local Kernel* t_capture = nullptr;  // set only inside Discover, on the game thread

using GetStateFn = NTSTATUS(APIENTRY*)(D3DKMT_GETDEVICESTATE*);
using GetPriorityFn = NTSTATUS(APIENTRY*)(D3DKMT_GETCONTEXTSCHEDULINGPRIORITY*);
using SetPriorityFn = NTSTATUS(APIENTRY*)(const D3DKMT_SETCONTEXTSCHEDULINGPRIORITY*);
GetStateFn g_get_state = nullptr;
GetPriorityFn g_get_priority = nullptr;
SetPriorityFn g_set_priority = nullptr;
int g_hooks = 0;  // 0 not tried, 1 in place, -1 failed (not tried again: MinHook may have left a patch behind)

NTSTATUS APIENTRY OnGetState(D3DKMT_GETDEVICESTATE* args) {
  Kernel* const capture = t_capture;
  if (capture == nullptr) return g_get_state(args);
  const D3DKMT_HANDLE device = args->hDevice;
  const NTSTATUS status = g_get_state(args);
  if (status >= 0) {
    capture->ambiguous |= device == 0 || (capture->device != 0 && capture->device != device);
    capture->device = device;
  }
  return status;
}

NTSTATUS APIENTRY OnGetPriority(D3DKMT_GETCONTEXTSCHEDULINGPRIORITY* args) {
  Kernel* const capture = t_capture;
  if (capture == nullptr) return g_get_priority(args);
  const D3DKMT_HANDLE context = args->hContext;
  const NTSTATUS status = g_get_priority(args);
  if (status >= 0) {
    capture->ambiguous |= context == 0 || (capture->context != 0 && capture->context != context);
    capture->context = context;
  }
  return status;
}

NTSTATUS APIENTRY OnSetPriority(const D3DKMT_SETCONTEXTSCHEDULINGPRIORITY* args) {
  Kernel* const capture = t_capture;
  if (capture == nullptr) return g_set_priority(args);
  const D3DKMT_HANDLE context = args->hContext;
  const NTSTATUS status = g_set_priority(args);
  if (status >= 0) {
    for (size_t i = 0; i < capture->count; ++i)
      if (capture->contexts[i] == context) return status;
    if (capture->count == std::size(capture->contexts) || context == 0)
      capture->ambiguous = true;
    else
      capture->contexts[capture->count++] = context;
  }
  return status;
}

const char* InstallHooks() {
  if (g_hooks > 0) return nullptr;
  if (g_hooks < 0) return "the kernel hooks failed before";
  g_hooks = -1;
  const HMODULE native = GetModuleHandleW(L"win32u.dll");
  if (native == nullptr) return "win32u.dll is not loaded";
  void* const get_state = reinterpret_cast<void*>(GetProcAddress(native, "NtGdiDdDDIGetDeviceState"));
  void* const get_priority = reinterpret_cast<void*>(GetProcAddress(native, "NtGdiDdDDIGetContextSchedulingPriority"));
  void* const set_priority = reinterpret_cast<void*>(GetProcAddress(native, "NtGdiDdDDISetContextSchedulingPriority"));
  if (get_state == nullptr || get_priority == nullptr || set_priority == nullptr) return "win32u.dll lacks the kernel calls";
  // The hooks stay for the life of the process, so this module must too.
  HMODULE self = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&OnGetState), &self))
    return "could not pin HSF in memory";
  const MH_STATUS init = MH_Initialize();
  if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return "MinHook did not start";
  if (MH_CreateHook(get_state, reinterpret_cast<void*>(&OnGetState), reinterpret_cast<void**>(&g_get_state)) != MH_OK ||
      MH_CreateHook(get_priority, reinterpret_cast<void*>(&OnGetPriority), reinterpret_cast<void**>(&g_get_priority)) != MH_OK ||
      MH_CreateHook(set_priority, reinterpret_cast<void*>(&OnSetPriority), reinterpret_cast<void**>(&g_set_priority)) != MH_OK)
    return "MinHook could not hook the kernel calls";
  if (MH_QueueEnableHook(get_state) != MH_OK || MH_QueueEnableHook(get_priority) != MH_OK || MH_QueueEnableHook(set_priority) != MH_OK ||
      MH_ApplyQueued() != MH_OK)
    return "MinHook could not switch the hooks on";
  g_hooks = 1;
  return nullptr;
}

const char* Discover(IDirect3DDevice9Ex* game, Kernel& out) {
  D3DDEVICE_CREATION_PARAMETERS creation{};
  if (FAILED(game->GetCreationParameters(&creation))) return "the device gave no creation parameters";
  Kernel k;
  t_capture = &k;
  const HRESULT state = game->CheckDeviceState(creation.hFocusWindow);
  INT priority = 0;
  HRESULT priority_result = game->GetGPUThreadPriority(&priority);
  if (SUCCEEDED(priority_result)) priority_result = game->SetGPUThreadPriority(priority);
  t_capture = nullptr;
  if (FAILED(state) || FAILED(priority_result)) return "the device refused the calls that show its kernel handles";
  if (k.ambiguous || k.device == 0 || k.context == 0 || k.count == 0) return "the device's kernel handles were not seen (or more than one device answered)";
  bool primary_seen = false;
  for (size_t i = 0; i < k.count; ++i) primary_seen |= k.contexts[i] == k.context;
  if (!primary_seen) return "the device's kernel contexts do not agree";
  out = k;
  return nullptr;
}

// ---- The fences ----
struct KmtFence {
  D3DKMT_HANDLE object = 0;
  D3DKMT_HANDLE context = 0;                // the context that signals it
  const volatile UINT64* completed = nullptr;  // the kernel's read-only mapping of the value reached
  UINT64 value = 0;                         // the last value queued
};

struct State {
  bool ready = false;
  Kernel k;
  decltype(&D3DKMTCreateSynchronizationObject2) create = nullptr;
  decltype(&D3DKMTDestroySynchronizationObject) destroy = nullptr;
  decltype(&D3DKMTSignalSynchronizationObjectFromGpu2) signal_gpu = nullptr;
  decltype(&D3DKMTWaitForSynchronizationObjectFromGpu) wait_gpu = nullptr;
  decltype(&D3DKMTShareObjects) share = nullptr;
  decltype(&D3DKMTOpenSyncObjectFromNtHandle2) open = nullptr;
  decltype(&D3DKMTSignalSynchronizationObjectFromCpu) signal_cpu = nullptr;
  // Stalled(): the values the two fences had reached when last looked at, and since when they have not moved.
  UINT64 seen_game = 0, seen_bridge = 0;
  LONGLONG game_since = 0, bridge_since = 0;
  bool lose_next = false;
  // Game to bridge.
  KmtFence game;                 // on the context D3D9 submits on
  std::vector<KmtFence> joined;  // one per other context of the device: each signals its own, the first context waits for them all
  HANDLE game_share = nullptr;
  ComPtr<ID3D11Fence> game_on11;
  // Bridge to game.
  ComPtr<ID3D11DeviceContext4> context4;
  ComPtr<ID3D11Fence> bridge;
  UINT64 bridge_value = 0;
  HANDLE bridge_share = nullptr;
  D3DKMT_HANDLE bridge_on9 = 0;
};
State g;

UINT64 Completed(const KmtFence& f) {
  if (f.completed == nullptr) return UINT64_MAX;
#if defined(_M_IX86)
  // One aligned 64-bit read (MOVQ), so a 32-bit build never sees half an update.
  UINT64 value = 0;
  _mm_storel_epi64(reinterpret_cast<__m128i*>(&value), _mm_loadl_epi64(reinterpret_cast<const __m128i*>(const_cast<const UINT64*>(f.completed))));
  return value;
#else
  return *f.completed;
#endif
}

bool MakeKmt(KmtFence& f, D3DKMT_HANDLE context) {
  D3DKMT_CREATESYNCHRONIZATIONOBJECT2 args{};
  args.hDevice = g.k.device;
  args.Info.Type = D3DDDI_MONITORED_FENCE;
  args.Info.Flags.Shared = 1;
  args.Info.Flags.NtSecuritySharing = 1;
  args.Info.Flags.NoGPUAccess = 1;
  if (g.create(&args) < 0) return false;
  f.object = args.hSyncObject;
  f.context = context;
  f.completed = static_cast<const volatile UINT64*>(args.Info.MonitoredFence.FenceValueCPUVirtualAddress);
  f.value = 0;
  return f.completed != nullptr && reinterpret_cast<uintptr_t>(f.completed) % alignof(UINT64) == 0;
}

void DestroyKmt(D3DKMT_HANDLE& object) {
  if (object == 0) return;
  D3DKMT_DESTROYSYNCHRONIZATIONOBJECT args{};
  args.hSyncObject = object;
  g.destroy(&args);
  object = 0;
}

bool Signal(KmtFence& f, UINT64 target) {
  D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2 args{};
  args.ObjectCount = 1;
  args.ObjectHandleArray = &f.object;
  args.BroadcastContextCount = 1;
  args.BroadcastContextArray = &f.context;
  args.MonitoredFenceValueArray = &target;
  if (g.signal_gpu(&args) < 0) return false;
  f.value = target;
  return true;
}

bool WaitOn(D3DKMT_HANDLE context, D3DKMT_HANDLE object, UINT64 target) {
  D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU args{};
  args.hContext = context;
  args.ObjectCount = 1;
  args.ObjectHandleArray = &object;
  args.MonitoredFenceValueArray = &target;
  return g.wait_gpu(&args) >= 0;
}

// Ends every GPU wait on a fence by signalling it from the CPU (through the game's kernel device, which has both fences open).
void SignalFromCpu(D3DKMT_HANDLE object, UINT64 value) {
  if (g.signal_cpu == nullptr || object == 0 || g.k.device == 0) return;
  D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU args{};
  args.hDevice = g.k.device;
  args.ObjectCount = 1;
  args.ObjectHandleArray = &object;
  args.FenceValueArray = &value;
  g.signal_cpu(&args);
}

LONGLONG Ticks() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}
double MsSince(LONGLONG since) {
  static LARGE_INTEGER freq{};
  if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
  return static_cast<double>(Ticks() - since) * 1000.0 / static_cast<double>(freq.QuadPart);
}

template <typename T>
bool Export(HMODULE gdi, const char* name, T& out) {
  out = reinterpret_cast<T>(GetProcAddress(gdi, name));
  return out != nullptr;
}

bool Fail(char* why, size_t why_size, const char* what, long code = 0) {
  if (code != 0)
    std::snprintf(why, why_size, "%s (0x%08lx)", what, static_cast<unsigned long>(code));
  else
    std::snprintf(why, why_size, "%s", what);
  Stop();
  return false;
}

}  // namespace

bool Start(IDirect3DDevice9Ex* game, ID3D11Device* device, ID3D11DeviceContext* context, char* why, size_t why_size) {
  Stop();
  why[0] = '\0';
  if (game == nullptr || device == nullptr || context == nullptr) return Fail(why, why_size, "no device");
  ComPtr<ID3D11Device5> device5;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device5))) || FAILED(context->QueryInterface(IID_PPV_ARGS(&g.context4))))
    return Fail(why, why_size, "this Windows has no Direct3D 11 fences");
  const HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
  if (gdi == nullptr || !Export(gdi, "D3DKMTCreateSynchronizationObject2", g.create) || !Export(gdi, "D3DKMTDestroySynchronizationObject", g.destroy) ||
      !Export(gdi, "D3DKMTSignalSynchronizationObjectFromGpu2", g.signal_gpu) || !Export(gdi, "D3DKMTWaitForSynchronizationObjectFromGpu", g.wait_gpu) ||
      !Export(gdi, "D3DKMTShareObjects", g.share) || !Export(gdi, "D3DKMTOpenSyncObjectFromNtHandle2", g.open) ||
      !Export(gdi, "D3DKMTSignalSynchronizationObjectFromCpu", g.signal_cpu))
    return Fail(why, why_size, "gdi32.dll lacks the kernel fence calls");
  if (const char* const problem = InstallHooks()) return Fail(why, why_size, problem);
  if (const char* const problem = Discover(game, g.k)) return Fail(why, why_size, problem);

  // Game to bridge: the kernel fence, shared, opened by Direct3D 11.
  if (!MakeKmt(g.game, g.k.context)) return Fail(why, why_size, "the kernel would not make a fence on the game's device");
  for (size_t i = 0; i < g.k.count; ++i) {
    if (g.k.contexts[i] == g.k.context) continue;
    g.joined.emplace_back();
    if (!MakeKmt(g.joined.back(), g.k.contexts[i])) return Fail(why, why_size, "the kernel would not make a fence for the device's second context");
  }
  OBJECT_ATTRIBUTES attributes{};
  attributes.Length = sizeof(attributes);
  const NTSTATUS shared = g.share(1, &g.game.object, &attributes, GENERIC_ALL, &g.game_share);
  if (shared < 0 || g.game_share == nullptr) return Fail(why, why_size, "the kernel would not share the game's fence", shared);
  HRESULT hr = device5->OpenSharedFence(g.game_share, IID_PPV_ARGS(&g.game_on11));
  if (FAILED(hr)) return Fail(why, why_size, "Direct3D 11 would not open the game's fence", hr);

  // Bridge to game: a Direct3D 11 fence, shared, opened on the game's kernel device.
  hr = device5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&g.bridge));
  if (SUCCEEDED(hr)) hr = g.bridge->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &g.bridge_share);
  if (FAILED(hr) || g.bridge_share == nullptr) return Fail(why, why_size, "Direct3D 11 would not make a shared fence", hr);
  D3DKMT_OPENSYNCOBJECTFROMNTHANDLE2 opened{};
  opened.hNtHandle = g.bridge_share;
  opened.hDevice = g.k.device;
  opened.Flags.NoGPUAccess = 1;
  const NTSTATUS open_status = g.open(&opened);
  if (open_status < 0 || opened.hSyncObject == 0) return Fail(why, why_size, "the game's device would not open HSF's fence", open_status);
  g.bridge_on9 = opened.hSyncObject;
  g.bridge_value = 0;
  g.seen_game = g.seen_bridge = 0;
  g.game_since = g.bridge_since = Ticks();
  g.lose_next = false;
  g.ready = true;
  return true;
}

void Stop() {
  // What the game's queue was told to signal must have happened before the fence goes: HSF's Direct3D 11 context may still wait for it.
  if (g.ready && g.game.value != 0) {
    LARGE_INTEGER freq{}, start{}, now{};
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&start);
    while (Completed(g.game) < g.game.value) {
      QueryPerformanceCounter(&now);
      if ((now.QuadPart - start.QuadPart) * 10 > freq.QuadPart) break;  // 100 ms
      YieldProcessor();
    }
    // Still not there: signalled from the CPU, so nothing HSF's Direct3D 11 context was given waits for ever.
    if (Completed(g.game) < g.game.value) SignalFromCpu(g.game.object, g.game.value);
  }
  if (g.ready && g.bridge != nullptr && g.bridge->GetCompletedValue() < g.bridge_value) SignalFromCpu(g.bridge_on9, g.bridge_value);
  g.ready = false;
  if (g.destroy != nullptr) {
    DestroyKmt(g.bridge_on9);
    for (KmtFence& f : g.joined) DestroyKmt(f.object);
    DestroyKmt(g.game.object);
  }
  g.bridge_on9 = 0;
  g.joined.clear();
  g.game = KmtFence{};
  if (g.game_share != nullptr) CloseHandle(g.game_share);
  if (g.bridge_share != nullptr) CloseHandle(g.bridge_share);
  g.game_share = g.bridge_share = nullptr;
  g.game_on11.Reset();
  g.bridge.Reset();
  g.context4.Reset();
  g.bridge_value = 0;
  g.k = Kernel{};
}

bool Ready() { return g.ready; }

bool GameDone() {
  if (!g.ready) return false;
  const UINT64 target = g.game.value + 1;
  // Every other context of the device signals its own fence, and the first context waits for each: the first one's signal then stands for
  // the whole device.
  for (KmtFence& f : g.joined) {
    if (!Signal(f, target) || !WaitOn(g.k.context, f.object, target)) return false;
  }
  if (g.lose_next) {
    g.lose_next = false;
    g.game.value = target;
  } else if (!Signal(g.game, target)) {
    return false;
  }
  return SUCCEEDED(g.context4->Wait(g.game_on11.Get(), target));
}

const char* Stalled() {
  if (!g.ready) return nullptr;
  constexpr double LIMIT_MS = 500.0;
  static char text[160];
  const UINT64 game = Completed(g.game);
  const UINT64 bridge = g.bridge->GetCompletedValue();
  if (game != g.seen_game || game >= g.game.value) {
    g.seen_game = game;
    g.game_since = Ticks();
  }
  if (bridge != g.seen_bridge || bridge >= g.bridge_value) {
    g.seen_bridge = bridge;
    g.bridge_since = Ticks();
  }
  // Standing still: no step for half a second, or a dozen signals behind (the GPU is never that many frames behind; and once about thirty
  // waits are queued, the next Direct3D 9 call blocks the game thread for most of a minute, so this must be seen well before).
  constexpr UINT64 LIMIT_BEHIND = 12;
  const bool game_stuck = MsSince(g.game_since) > LIMIT_MS || g.game.value - std::min(game, g.game.value) >= LIMIT_BEHIND;
  const bool bridge_stuck = MsSince(g.bridge_since) > LIMIT_MS || g.bridge_value - std::min(bridge, g.bridge_value) >= LIMIT_BEHIND;
  if (!game_stuck && !bridge_stuck) return nullptr;
  std::snprintf(text, sizeof(text), "%s stood still (the game's fence at %llu of %llu, HSF's at %llu of %llu, %d kernel context%s)",
                game_stuck ? "the game's signal" : "HSF's signal", static_cast<unsigned long long>(game),
                static_cast<unsigned long long>(g.game.value), static_cast<unsigned long long>(bridge), static_cast<unsigned long long>(g.bridge_value),
                static_cast<int>(g.k.count), g.k.count == 1 ? "" : "s");
  // Let everything go: first the game's fence (HSF's Direct3D 11 context waits for it), then HSF's (the game's queue waits for it).
  SignalFromCpu(g.game.object, g.game.value);
  for (const KmtFence& f : g.joined) SignalFromCpu(f.object, f.value);
  SignalFromCpu(g.bridge_on9, g.bridge_value);
  return text;
}

int KernelContexts() { return static_cast<int>(g.k.count); }
void TestLoseNextSignal() { g.lose_next = true; }

bool BridgeDone() {
  if (!g.ready) return false;
  if (FAILED(g.context4->Signal(g.bridge.Get(), g.bridge_value + 1))) return false;
  ++g.bridge_value;
  g.context4->Flush();  // the signal must be on its way before the game's queue waits for it
  for (size_t i = 0; i < g.k.count; ++i) {
    if (!WaitOn(g.k.contexts[i], g.bridge_on9, g.bridge_value)) return false;
  }
  return true;
}

}  // namespace hsf::fence9
