// HSF's real depth for DLSS-G. ReShade's Generic Depth picks the game's depth buffer; a compute shader copies it,
// once per frame, into an R32_FLOAT texture at the motion vectors' size, because a game's depth buffer usually cannot be shared or copied directly
// (R24G8 and multisampled ones). That texture then goes to DLSS-G through the same shared slots as the motion vectors. A few times a second a copy
// is read back without ever waiting, to find out whether the depth is reversed and whether it holds anything at all.
// Runs on the game's own Direct3D 11 device on the game thread, so 32-bit and 64-bit games share it. All CS state it touches is put back.
#pragma once

#include <d3d11.h>

#include <cstdint>

namespace hsf::depth {

struct Status {
  bool source = false;    // the game's depth buffer is there
  UINT width = 0, height = 0, samples = 0;  // the game's depth buffer
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;  // its view's format
  bool measured = false;  // the values below come from this source
  float lowest = 0, median = 0, highest = 0;
  bool flat = false;      // one value everywhere: nothing was drawn into it (a 2D game, a menu), or the wrong buffer
  // Flat for about 10 s with a buffer bound and never anything else since the device came up. The game may clear its depth before the frame ends, which
  // ReShade's DepthCopyBeforeClears gets around; a long menu or a 2D game looks the same, so this is a reason to offer the setting, not to set it.
  bool flat_for_long = false;
  bool inverted = false;  // reversed: far = 0 (most of a picture's depth sits near far, so the median shows it)
  bool upside_down = false;  // the game's buffer is mirrored top to bottom against its picture (a game that renders to a texture and flips it last); the copy is put right
  float edges_same = 0, edges_mirrored = 0;  // how well the depth's edges line up with the picture's, as the copy is and mirrored (smoothed)
  bool fitted = false;  // the depth has another shape than the picture and was put into the picture's middle (SetFitPicture)
};

// Game thread, after ReShade's effects, once per frame. `game_depth` is the view ReShade binds to DEPTH, or nullptr. Returns this frame's depth as
// R32_FLOAT at width x height, or nullptr while there is nothing worth sending: no source, not measured yet, or flat. Valid until Shutdown.
// `picture` is the finished frame, compared with the depth to find out whether it is upside down (nullptr: not compared).
ID3D11Texture2D* Frame(ID3D11Device* device, ID3D11ShaderResourceView* game_depth, UINT width, UINT height, ID3D11Texture2D* picture);
// Any time on the game thread. -1 = found from the picture, 0 = as the game has it, 1 = upside down.
void SetUpsideDown(int setting);
// Any time on the game thread. On: a depth buffer of another shape than the picture (an emulator drawing a 16:9 picture centred on a wider
// screen, with bars at the sides) is put into the middle of the copy at its own shape, and the bars take the depth at the picture's edge.
// Off (the default): the depth is stretched over the whole copy, as before.
void SetFitPicture(bool on);
Status Current();
// Game thread. Vectors of another shape than the picture (a game's own DLSS data, at the game's 16:9 picture centred on a wider screen) as R16G16_FLOAT at
// width x height, in DLSS-G's units (offset towards the previous frame in UV of the whole copy). The part (x, y, used_width x used_height) of `motion` goes
// into the copy's middle at its own shape (the whole copy when the shapes match), with no motion in the bars. `to_pixels` turns a vector into pixels of
// that part. `smooth` blends between texels (a small source scaled up, the flow) in place of taking the nearest. nullptr if it could not be made.
// Valid until Shutdown.
ID3D11Texture2D* FitMotion(ID3D11Device* device, ID3D11ShaderResourceView* motion, UINT x, UINT y, UINT used_width, UINT used_height, const float to_pixels[2],
                           UINT width, UINT height, bool smooth = false);
// Game thread. The debug view (near = white, stretched over the picture's range) as a native view for ImGui, or nullptr.
ID3D11ShaderResourceView* View();
// Why it is not running ("" if it is).
const char* Problem();
// How long to wait from one read-back to the next (default 250 ms; tests use 0).
void SetPeriod(uint32_t ms);
// Releases everything. Call before the game's device goes away; Frame builds it all again.
void Shutdown();

}  // namespace hsf::depth
