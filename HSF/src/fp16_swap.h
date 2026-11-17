// D3D12 games whose swap chain is FP16 (scRGB), on the route where HSF brings Streamline. DLSS-G takes no FP16 frames (NVIDIA's DLSS-G guide,
// section 11.0) and presents on the game's own swap chain, so the swap chain is made through Streamline as R10G10B10A2 instead. Streamline hands
// out back buffers of its own (tied to the game's queue; DXGI's are on DLSS-G's pacer queue). The game is handed FP16 textures of HSF's own in
// place of those and keeps drawing scRGB into them; at each Present, before Streamline takes the frame, HSF converts the one drawn into
// Streamline's back buffer (hdr_out.h: HDR10 when the display is in HDR mode, sRGB otherwise).
// What the game reads back is what it asked for: GetDesc says FP16, the scRGB colour space reads as supported and setting it sets HSF's.
#pragma once

#include <windows.h>
#include <dxgi1_6.h>

namespace hsf::fp16swap {

using LineFn = void (*)(const char* format, ...);

// For the swap chain made through Streamline's factory: an FP16 format becomes R10G10B10A2_UNORM. True when it changed it.
bool Rewrite(DXGI_FORMAT& format);

// After the swap chain is made. `game` is the one the game holds, `made` Streamline's (the same with no wrapper), `queue` the queue the game
// passed and `real_queue` the same under any wrapper. The swap chains are not held.
void Start(IUnknown* game, IUnknown* made, IUnknown* queue, IUnknown* real_queue, LineFn line);

// The game presents `game`: the FP16 frame goes into the real back buffer first.
void BeforePresent(IUnknown* game);

// DXGI's own swap chain, made under Streamline's: HSF's colour space goes on it straight as well. Not held.
void SetNative(IUnknown* native);

// HSF is converting this swap chain's frames.
bool Active();

}  // namespace hsf::fp16swap
