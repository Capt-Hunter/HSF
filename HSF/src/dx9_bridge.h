// Direct3D 9 games: the bridge from the game's D3D9 device to a Direct3D 11 device of HSF's own on the same GPU.
// DLSS-G's hand-off (dlssg_remote.cpp, the helper) only takes Direct3D 11 textures. So each frame the game's finished frame, LaunchPad's flow and
// HSF.fx's depth copy are copied from D3D9 into textures of HSF's D3D11 device, and those go to dlssg::Submit exactly like a D3D11 game's.
// Two routes, picked once per device:
// - GPU: the game's device is D3D9Ex, so D3D9 can make shared textures that D3D11 opens. No CPU copy.
// - CPU: a plain D3D9 device (most older games) cannot share. The frame is copied to the CPU and up into D3D11.
// The GPU route hands D3D11 this frame's copies in the same frame: the game's GPU queue signals a fence behind its copies and D3D11 waits for
// it on the GPU (gpu_fence9.h, HSF.ini Dx9GpuFence=1, the default), so the game thread never waits. Where the fences cannot be made it runs
// one frame behind: frame N is copied into a D3D9 texture of HSF's own at N and taken across at N+1, behind event queries. The CPU route runs two
// behind: frame N's read-back to system memory is queued at N (with an event query) and taken across at N+2, when the GPU has long finished it,
// so the game thread does not wait for the GPU. The CPU copy into D3D11 is split over the Windows thread pool.
// With HSF.ini Dx9HalfInputs=1 (the default) flow and depth are copied at half width and height (a quarter of the bytes); DLSS-G takes them
// smaller than the frame, as it does with upscaling.
// D3D9 resources in the default pool stop a device Reset: ReleaseGameResources must run before every Reset (ReShade's destroy_swapchain).
#pragma once

#include <d3d11.h>
#include <d3d9.h>

#include <cstddef>
#include <cstdint>

namespace hsf::dx9 {

enum class Route : int { none = 0, gpu = 1, cpu = 2 };
inline const char* const ROUTE_NAMES[] = {"none", "GPU (shared textures)", "CPU copy"};

// Game thread. Makes HSF's D3D11 device on the GPU of `game` (the native D3D9 device, not ReShade's proxy). True if it is there (also when it
// already was for this device); false, and why in `error`, if not.
bool Start(IDirect3DDevice9* game, char* error, size_t error_size);
// Game thread. Frees everything, the D3D11 device too.
void Stop();
// Game thread. Frees the D3D9 resources HSF made on the game's device; they are made again on the next Bridge. Before a Reset of the device.
void ReleaseGameResources();
void SetHalfInputs(bool half);  // HSF.ini Dx9HalfInputs; takes effect on the next Bridge
void SetGpuFence(bool on);      // HSF.ini Dx9GpuFence; before Start
bool GpuFence();                // the GPU route runs on GPU fences (this frame's copies, no CPU wait)
void TestLoseFenceSignal();     // tests only: the game's next fence signal is left out, as if it got lost
ID3D11Device* Device();  // HSF's D3D11 device, nullptr before Start
Route CurrentRoute();

// The textures on HSF's D3D11 device that hold the copies (this frame's with GPU fences, an earlier frame's without); nullptr for an input not there (yet).
struct Frame {
  ID3D11Texture2D* color = nullptr;
  ID3D11Texture2D* motion = nullptr;
  ID3D11Texture2D* depth = nullptr;
  ID3D11Texture2D* hudless = nullptr;  // the frame from before the overlays (BridgeHudless), or nullptr
  UINT motion_width = 0, motion_height = 0;  // `motion`'s size (half the flow's with Dx9HalfInputs=1)
};
// Game thread, at ReShade's finish_effects: the back buffer before ReShade's menu and the overlays are drawn. The next Bridge hands it over as
// Frame::hudless (as far behind as the rest), so DLSS-G keeps the overlays out of the generated frames while the real ones show them.
void BridgeHudless(IDirect3DSurface9* back_buffer);
// Game thread, in the native Present before it runs. Copies this frame's `back_buffer`, `motion` and `depth` (each may be nullptr) into HSF's
// D3D9 textures and hands back the D3D11 textures that hold them (or an earlier frame's, see above). A size or format change starts that input again.
Frame Bridge(IDirect3DSurface9* back_buffer, IDirect3DTexture9* motion, IDirect3DTexture9* depth);

// For the log: the time the game thread spent in Bridge, per frame, averaged since the last call, and the DXGI format the color goes as.
struct Stats {
  float bridge_ms = 0, worst_ms = 0;
  uint32_t frames = 0, failed = 0;
  const char* problem = "";  // the last thing that went wrong, or ""
  const char* lanes = "";    // game thread ms per frame in each copy, and their sizes
  uint32_t depth_checks = 0;              // times the depth copy's range was read (both routes)
  float depth_min = 0, depth_max = 0;     // its smallest and largest value over those reads
};
Stats TakeStats();

// The depth copy on a grid of DEPTH_GRID_W x DEPTH_GRID_H points (rows top to bottom), read every 30th frame (GPU route: shrunk first). Returns a
// number that changes with each new read (0 = none yet); *values stays valid until the next Bridge.
constexpr unsigned DEPTH_GRID_W = 160, DEPTH_GRID_H = 90;
uint32_t DepthGrid(const float** values);

// The DXGI format a D3D9 format is copied into (same bytes per texel, same channel order), or DXGI_FORMAT_UNKNOWN if there is none.
DXGI_FORMAT ToDxgi(D3DFORMAT format);

}  // namespace hsf::dx9
