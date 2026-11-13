// HSF's motion quality score: how much better the motion vectors line up two real frames than no motion at all.
// A few times a second two consecutive finished frames are copied; a compute shader warps the earlier one along each input's motion vectors and
// compares it to the later one, on a coarse grid of small patches at full-resolution detail. The result is read back without ever waiting.
// It rates the motion that goes INTO frame generation, not the frame that comes out, so a good score does not promise a clean picture.
// Runs on the game's own Direct3D 11 device on the game thread, so 32-bit and 64-bit games share it. All CS state it touches is put back.
#pragma once

#include <d3d11.h>

#include <cstdint>

namespace hsf::score {

constexpr int MAX_INPUTS = 3;
// Debug views of an input's newest measurement.
// `depth` is depth.h's view, not an input's: it is in this list so one key cycles them all.
enum class View { off = 0, motion = 1, error = 2, better_worse = 3, depth = 4 };
constexpr int VIEW_COUNT = 5;
constexpr const char* VIEW_NAMES[VIEW_COUNT] = {"off", "motion", "error", "better/worse", "depth"};

// One candidate for the motion vectors. `to_pixels` turns a texel into the offset, in frame pixels, from a pixel to where it was one frame earlier
// (motion_convention.h ConventionScale). A null `motion` or a zero scale leaves the input out of this measurement.
struct Input {
  ID3D11Texture2D* motion = nullptr;
  float to_pixels[2] = {};
};

// One input over a time window.
struct Result {
  bool valid = false;      // at least one usable pair in the window
  bool thin = false;       // valid, but too few pairs or too little of the picture changed for the numbers to mean much
  float gain = 0;          // 1 - error with motion / error without: 1 = perfect, 0 = no help, below 0 = worse than no motion
  float error_warp = 0;    // mean luma error with motion, in 8-bit levels (0-255)
  float error_none = 0;    // the same without motion
  float better = 0;        // share (0-1) of the patches that changed where the motion clearly helped
  float worse = 0;         // ... where it clearly hurt
  float moving = 0;        // share (0-1) of all patches that changed between the frames
  uint32_t pairs = 0;      // pairs that counted
  uint32_t still = 0;      // pairs left out: almost nothing changed, so there is nothing to rate
  uint32_t cuts = 0;       // pairs left out: the whole picture changed and the motion did not explain it (scene cut, flash)
};

// Game thread, once per finished frame, with the game's device and the finished frame. `inputs` has MAX_INPUTS entries.
void Frame(ID3D11Device* device, ID3D11Texture2D* frame, const Input* inputs);
// Game thread. The input's results over the last `window_ms`.
Result Recent(int input, uint32_t window_ms);
// Game thread. The input's debug view as a native shader resource view (for ImGui), or nullptr.
ID3D11ShaderResourceView* ViewOf(int input, View view);
// Game thread. The debug view's pixels on the CPU, for a device that cannot draw ViewOf's (a DX9 game's, where the score runs on HSF's own
// D3D11 device): queues a copy of the newest view and takes the one queued before once the GPU has finished it. Never waits. Returns a number
// that changes with each new picture of this input and view (0 = none yet); *pixels (RGBA, *width x *height) stays valid until the next call.
uint32_t ReadView(int input, View view, const uint32_t** pixels, UINT* width, UINT* height);
// Why the score is not running ("" if it is).
const char* Problem();
// How long to wait from one measured pair to the next (default 250 ms; tests use 0).
void SetPeriod(uint32_t ms);
// Releases everything. Call before the game's device goes away; Frame builds it all again.
void Shutdown();

}  // namespace hsf::score
