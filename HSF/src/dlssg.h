// HSF's DLSS Frame Generation presenter for D3D11 and DX9 games.
// DLSS-G only attaches to a swap chain on a window (not a composition swap chain), and only to Direct3D 12. So this presenter puts a disabled
// child window of its own over the game window's client area, with a Direct3D 12 swap chain (made through Streamline's sl.interposer.dll) on it.
// The game thread hands each finished frame and its motion vectors over through shared textures and fences; the presenter
// thread copies them into the swap chain and DLSS-G's inputs and presents, and DLSS-G adds its generated frames. Depth is real when depth.h has it, otherwise a constant.
// It runs in the 64-bit helper hsf-dlssg-helper64.exe for 32-bit and 64-bit games alike (dlssg_link.h).
#pragma once

#include "camera.h"

#include <d3d11.h>

#include <cstddef>
#include <cstdint>

namespace hsf::dlssg {

// Counters since the last TakeStats, per second.
struct Stats {
  float real_fps = 0;       // game frames handed to DLSS-G
  float shown_fps = 0;      // frames DLSS-G says it presented (real + generated)
  uint32_t skipped = 0;     // game frames never handed over (no free slot)
  uint32_t paced = 0;       // game frames that waited for the helper to take the one before, and their mean wait
  float paced_ms = 0;
  uint32_t early = 0;       // game frames that waited at their start instead (the learned part of that wait), and their mean wait
  float early_ms = 0;
  float copy_ms = 0;        // the game's GPU on the copies to the helper, per frame (start to end, other work in between included)
  uint32_t status = 0;      // DLSS-G status bits seen (sl::DLSSGStatus, 0 = fine)
  uint32_t max_frames = 0;  // the most frames DLSS-G can generate per real one on this machine
  bool generating = false;  // DLSS-G generated frames in the last second
  const char* note = "";    // why it is not running or not generating, if known
  bool gpu_hang = false;    // the helper turned DLSS-G off because the GPU stopped keeping up (note says so)
  int nvof_state = 0;       // NVIDIA Optical Flow in the helper: 0 off, 1 waiting, 2 active, 3 failed (nvof::State)
  bool foreign_dxgi = false;  // the helper's dxgi.dll is not Windows' own: ReShade and its add-ons run in the helper too
  // Reflex on the game's frame (FrameStart): how long the game thread slept per frame, on average, and whether Reflex says it is available.
  float reflex_sleep_ms = 0;
  bool reflex_game_paced = false;  // the game thread slept on Reflex in the last second (FrameStart reached Streamline)
  bool reflex_available = false;   // slReflexGetState: low latency mode available on this GPU and driver
  bool reflex_sleep_on = true;     // SetReflexSleep: off = markers only, for an A/B latency test
  // HSF's own latency timer: from the game frame's start (its previous Present returned and Reflex let it go) to DLSS-G's Present of that
  // frame, on the CPU. Average and worst over the last second; 0 = no frames.
  float latency_ms = 0, latency_max_ms = 0;
  // Reflex's latency report from the driver (reflex_report.h), averaged over the last second's new frames. report_frames = 0: no report, so
  // the driver does not see the markers (Reflex is not really working).
  uint32_t report_frames = 0;
  float report_total_ms = 0;    // simulation start -> GPU render end
  float report_game_ms = 0;     // the game's CPU frame
  float report_submit_ms = 0;   // HSF's hand-off copy
  float report_handoff_ms = 0;  // waiting for HSF's presenter
  float report_present_ms = 0;  // DLSS-G's Present call
  float report_gpu_ms = 0;      // render queue + GPU
  // The still world's motion from the camera (camera.h): which source this frame, how much of the flow it explains (0-100), and the matrix locked.
  hsf::camera::Source camera_source = hsf::camera::Source::none;
  int camera_share = 0;
  int64_t camera_locked = 0;
};

// Game thread. Starts the presenter on `window` for frames of `width` x `height` in `format` from `game_device`. Streamline's DLLs are loaded
// from `sl_folder`. False, and why in `error`, if not.
bool Start(ID3D11Device* game_device, HWND window, UINT width, UINT height, DXGI_FORMAT format, const wchar_t* sl_folder, char* error,
           size_t error_size);
// Game thread. Stops the presenter: waits at most `wait_ms`; returns why it is still stopping, or nullptr once fully stopped.
const char* Stop(DWORD wait_ms = 250);
bool Reap();
bool Stopping();
bool Running();
// Running with frame gen on: the game sends its frames to the helper. With frame gen off the helper stays up but idle (child window hidden),
// and the game shows its own frames with nothing copied, as if the helper were closed.
bool Feeding();
// The size the motion vector slots were made at for the helper running now (they keep it until the next Start); false before they are made.
bool MotionSize(UINT* width, UINT* height);
// One game frame's inputs for DLSS-G.
struct FrameInputs {
  ID3D11Texture2D* motion = nullptr;
  // Turns a motion vector texel into the offset, in UV (0-1 of the frame), from a pixel to where it was one frame earlier (Streamline's
  // convention); {0, 0} = no motion vectors.
  float motion_scale[2] = {};
  bool reset = false;  // the picture does not follow from the last frame (a scene cut): DLSS-G must not use the history
  ID3D11Texture2D* depth = nullptr;  // R32_FLOAT at the motion vectors' size (depth.h), or nullptr: DLSS-G gets a constant
  bool depth_inverted = false;       // far = 0
  bool compose = false;              // clean up the motion with the depth on HSF's device (compose.h), when there is depth
  bool checks = false;               // scene cuts, still pictures and the HUD, on HSF's device (frame_checks.h)
  bool nvof = false;                 // the motion's contents come from NVIDIA Optical Flow in the helper (nvof_flow.h); `motion` still gives size, format and scale
  uint32_t nvof_size = 360;          // the height of the grey image the engine gets, 0 = the picture's own
  ID3D11Texture2D* hudless = nullptr;  // this frame from before the overlays were drawn, or nullptr.
  // The camera (camera.h). `camera_motion` off: flow only.
  bool camera_motion = false;
  bool camera_fit = false;  // with no matrix, find the camera's motion from the flow and depth
  bool camera_record = false;  // write the helper's side of a recording (camera_record.h)
  const hsf::camera::Candidate* camera = nullptr;
  int camera_count = 0;
  int64_t camera_frame = 0;
};

// Game thread, in the native Present before it runs. Hands the finished frame and its inputs over.
// Also Reflex's markers for the game's frame: simulation end and render submit start on entry, render submit end once the frame is handed
// over. The frame is tagged with the game frame's Streamline index, so the presenter's constants, tags and present markers land on it.
void Submit(ID3D11Texture2D* back_buffer, const FrameInputs& inputs);
// Game thread, right after the game's (native) Present returned: the game's next frame starts here. Reflex sleeps the game thread (low
// latency, and the FPS cap of SetReflex), then marks the frame's simulation start. Nothing while the presenter is not running.
void FrameStart();
// Any thread. `frames` generated per real frame (1 = 2x); `on` false turns DLSS-G off (real frames only).
void SetOptions(bool on, uint32_t frames);
// Any thread. true while ReShade's menu is open: the helper's window over the game then takes the mouse (dlssg_link.h, version 10).
void SetMenuOpen(bool open);
// Before Start, for that Start only (the Vulkan route): the helper presents from a click-through window of its own over the game's in place
// of a child window (dlssg_link.h, version 13).
void SetOverlayWindow(bool on);
// Before Start, for that Start only: the game swap chain's colour space (DXGI_COLOR_SPACE_TYPE), which the helper's swap chain takes on
// (dlssg_link.h, version 14). Without it, sRGB.
void SetColorSpace(uint32_t dxgi_color_space);
// Any thread. Reflex's FPS cap (frames on screen per second, real + generated, as DLSS-G counts them; 0 = no cap) and boost. Applied on
// the game thread's next FrameStart.
void SetReflex(uint32_t fps_cap, bool boost);
// Any thread. false = the game thread no longer sleeps on Reflex (markers still go out): to compare latency with and without the sleep.
void SetReflexSleep(bool on);
// Any thread. The matrix that won in this game before (camera.h), or 0.
void SetCameraHint(int64_t key);
Stats TakeStats();
// Game thread. The newest per-frame latencies of HSF's timer (ms, oldest first) into `out`; returns how many (at most `max`).
size_t LatencyHistory(float* out, size_t max);

}  // namespace hsf::dlssg
