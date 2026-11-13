// GPU fences between the game's D3D9Ex device and HSF's Direct3D 11 device (dx9_bridge.h), so neither side waits on the CPU for the other.
// Ported from RenoDX's src/utils/gpu_fence.hpp (MIT, Copyright (C) 2026 Carlos Lopez, https://github.com/clshortfuse/renodx, commit d97ad70;
// licence in third_party/renodx/LICENSE): only the D3D9Ex and Direct3D 11 parts, without its callbacks, OpenGL, Vulkan and D3D12.
// How it works:
// - D3D9 has no fence of its own. The kernel has one (a "monitored fence", D3DKMT calls in gdi32.dll), but it needs the kernel handles of the
//   device and its context, which D3D9 does not hand out. They are read from three kernel calls D3D9 makes for CheckDeviceState and
//   Get/SetGPUThreadPriority (hooked with MinHook in win32u.dll, kept for the life of the process).
// - Game to bridge: a kernel fence made on the game's device, signalled from the game's GPU queue after its copies; shared as an NT handle and
//   opened by Direct3D 11 (ID3D11Device5::OpenSharedFence), whose context waits for it on the GPU (ID3D11DeviceContext4::Wait).
// - Bridge to game: a Direct3D 11 fence (ID3D11Device5::CreateFence), signalled by HSF's context; opened on the game's kernel device, whose
//   GPU queue waits for it before the game writes HSF's copies again.
// All calls on the game thread. The kernel handles die with a device Reset: Stop before it, Start after.
#pragma once

#include <d3d11.h>
#include <d3d9.h>

#include <cstddef>

namespace hsf::fence9 {

// Makes both fences. False, and why in `why`, when this Windows, driver or device cannot (the bridge then keeps its event queries).
bool Start(IDirect3DDevice9Ex* game, ID3D11Device* device, ID3D11DeviceContext* context, char* why, size_t why_size);
// Waits a moment for what is still queued, then frees the fences. Safe when not started.
void Stop();
bool Ready();
// After the game's device has issued and flushed this frame's copies: the game's queue signals, HSF's Direct3D 11 context waits for that
// before anything issued on it from now on. False = failed (nothing HSF issued waits on a signal that will not come).
bool GameDone();
// Before the game's device writes HSF's copies again: everything HSF's Direct3D 11 context was given so far is flushed and signalled, and the
// game's queue waits for it.
bool BridgeDone();
// Once a frame. A GPU wait has no time limit: if a signal one side waits for never comes, both GPU queues stand still (a blank screen, a
// game at one frame a second). nullptr while the fences move; otherwise what stood still (half a second, or a dozen signals behind), after both fences were
// signalled from the CPU so every wait ends. The caller then stops using the fences.
const char* Stalled();
int KernelContexts();  // the kernel contexts of the game's device (for the log)
void TestLoseNextSignal();  // tests only: the next GameDone leaves the game's signal out

}  // namespace hsf::fence9
