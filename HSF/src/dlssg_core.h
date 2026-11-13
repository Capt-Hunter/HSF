// The presenter side of DLSS-G, the one path every frame takes (64-bit only: Streamline has no 32-bit build). dlssg_helper.cpp runs it in
// hsf-dlssg-helper64.exe for every D3D11 and DX9 game, 32-bit and 64-bit, and supplies a Host for what differs: where a frame's token and
// inputs come from and where the counters go. Streamline set-up, the swap chain, DLSS-G's options, constants, tags and Present live here only.
//   presenter thread: wait (CPU) until `ready` reaches a new frame -> copy the frame into the swap chain's buffer and the inputs into DLSS-G's ->
//                     tag, Present (DLSS-G adds its frames and paces them) -> signal `consumed` = M
#pragma once

#include "camera_record.h"
#include "compose.h"
#include "frame_checks.h"
#include "dlssg_link.h"
#include "hdr_out.h"
#include "nvof_flow.h"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <sl.h>
#include <sl_dlss_g.h>

#include <atomic>
#include <string>

namespace hsf::dlssg_core {

using Microsoft::WRL::ComPtr;

constexpr UINT RING = dlssg_link::RING;  // shared frame slots
constexpr UINT BUFFERS = 3;              // swap chain buffers

// ---- Log (Streamline's own warnings and errors go there too). Opened once; `mode` and `share` as _wfsopen. ----
void OpenLog(const wchar_t* path, const wchar_t* mode, int share);
void Log(const char* format, ...);

// ---- Process-wide: Streamline and the Direct3D 12 device (Streamline is initialised once and told about one device) ----
struct Streamline {
  HMODULE interposer = nullptr;
  std::wstring folder;
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  uint32_t max_frames = 1;
  char error[240] = "";
};
Streamline& Sl();
// Writes Sl().error and the log; returns false.
bool Fail(const char* format, ...);
// Loads Streamline from `folder`, slInit, checks DLSS-G on the adapter. Then the caller makes its device and calls UseDevice.
bool InitStreamline(const wchar_t* folder, LUID adapter, const wchar_t* adapter_name);
bool UseDevice(ID3D12Device* device);

// ---- Per Start ----
DXGI_FORMAT SwapFormat(DXGI_FORMAT format);

// One frame's inputs on the presenter side.
struct FrameInputs {
  ID3D12Resource* motion = nullptr;  // this frame's vectors, or nullptr: real frames only
  UINT motion_width = 0, motion_height = 0;
  DXGI_FORMAT motion_format = DXGI_FORMAT_UNKNOWN;
  float motion_scale[2] = {};
  bool reset = false;  // the picture does not follow from the last frame: DLSS-G must not use the history
  ID3D12Resource* depth = nullptr;  // this frame's depth (R32_FLOAT), or nullptr: a constant goes to DLSS-G
  UINT depth_width = 0, depth_height = 0;
  bool depth_inverted = false;
  bool compose = false;  // clean up the motion with the depth (compose.h), when there is real depth
  bool checks = false;   // scene cuts, still pictures and the HUD (frame_checks.h)
  bool nvof = false;     // the motion's contents come from NVIDIA Optical Flow (nvof_flow.h); `motion` still gives size, format and scale.
                         // With `motion` nullptr (no flow effect), NVOF makes the flow alone at the depth's size (or the frame's), R16G16_FLOAT, scale 1
  UINT nvof_size = nvof::DEFAULT_SIZE;
  ID3D12Resource* hudless = nullptr;  // this frame from before the overlays were drawn, the frame's size and format, or nullptr: overlays stay in the picture
  bool camera_motion = false;  // the still world's motion from the camera, when compose runs (camera.h)
  bool camera_fit = false;     // with no matrix, find it from the flow and depth
  const wchar_t* camera_record_folder = nullptr;  // not nullptr: write this frame's camera data to a file in this folder (camera_record.h)
  const camera::Candidate* camera = nullptr;
  int camera_count = 0;
  int64_t camera_frame = 0;
};

// The caller's part of the presenter thread. Called on that thread.
class Host {
 public:
  virtual bool Stopping() = 0;
  virtual void SwapChainFailed() = 0;
  virtual void CountSkipped(uint32_t frames) = 0;
  // The game frame's Streamline token for frame `id`, or nullptr: the presenter makes its own token from OwnFrameIndex, so the frame still goes out.
  virtual sl::FrameToken* GameToken(UINT64 id) = 0;
  virtual uint32_t OwnFrameIndex() = 0;
  virtual FrameInputs Inputs(UINT64 id) = 0;
  virtual int64_t CameraHint() = 0;
  virtual void CameraResult(const camera::Decision& decision, int64_t locked) = 0;
  virtual bool On() = 0;
  // True while the child window should take the mouse (ReShade's menu open in the game, so a mod's menu on this swap chain can be clicked).
  virtual bool TakeMouse() { return false; }
  // NVIDIA Optical Flow this frame, as nvof::State.
  virtual void NvofState(int) {}
  virtual uint32_t Frames() = 0;
  virtual void AfterPresent(UINT64 id) = 0;
  virtual void CountFrame(const sl::DLSSGState& state, bool motion, bool on) = 0;

 protected:
  ~Host() = default;
};

struct Presenter {
  UINT width = 0, height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN, swap_format = DXGI_FORMAT_UNKNOWN;
  ComPtr<ID3D12Fence> ready, consumed, frame_fence;
  UINT64 frame_value = 0, alloc_value[BUFFERS] = {};
  HANDLE ready_event = nullptr, frame_event = nullptr;
  ComPtr<ID3D12Resource> slots[RING];
  ComPtr<IDXGISwapChain3> swap;
  ComPtr<ID3D12Resource> back[BUFFERS];
  ComPtr<ID3D12CommandAllocator> alloc[BUFFERS];
  ComPtr<ID3D12GraphicsCommandList> list;
  // Timing, a report only: three timestamps a frame (the frame's list starts, it ends, right after Present on the same queue), and how long the
  // Present call takes on the CPU. Times on the GPU run from start to end, so other work on the GPU in between (the game's) is in them.
  ComPtr<ID3D12QueryHeap> time_queries;
  ComPtr<ID3D12Resource> time_readback;
  const UINT64* times = nullptr;
  double ticks_per_ms = 0;
  ComPtr<ID3D12CommandAllocator> tail_alloc[BUFFERS];
  ComPtr<ID3D12GraphicsCommandList> tail_list;
  bool timed[BUFFERS] = {};
  double own_ms_sum = 0, through_ms_sum = 0, present_ms_sum = 0;
  uint32_t timed_count = 0, present_count = 0;
  bool tearing = false;
  // An FP16 (scRGB) game: DLSS-G takes no FP16, so the swap chain is R10G10B10A2 and hdr_out.h converts the frame and HUD-less image into it, as
  // HDR10 when the display is in HDR mode (`pq`), as sRGB when not. `hdr_frame` holds the converted frame before it is copied into the swap chain.
  bool hdr = false, pq = false;
  // The game swap chain's colour space. An HDR frame (HDR10: R10G10B10A2 in ST.2084, BT.2020) needs the same on this swap chain, or it shows as SDR.
  DXGI_COLOR_SPACE_TYPE game_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  hdr_out::Pass hdr_pass;
  ComPtr<ID3D12Resource> hdr_frame;
  // DLSS-G's inputs, made at the motion vectors' size once the first ones come. Depth holds the frame's real depth, or a constant 0.5 without one.
  ComPtr<ID3D12Resource> mvec, depth;
  // Zero motion, for a frame NVOF has nothing for while it makes the flow alone (its first, or after a failure). Committed resources start as zeros.
  ComPtr<ID3D12Resource> no_motion;
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  UINT input_width = 0, input_height = 0;
  DXGI_FORMAT input_format = DXGI_FORMAT_UNKNOWN;
  bool inputs_failed = false;
  bool depth_constant = false;  // depth holds the constant, so it need not be cleared again
  D3D12_RESOURCE_STATES depth_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
  D3D12_RESOURCE_STATES mvec_state = D3D12_RESOURCE_STATE_COPY_DEST;
  compose::Pass compose;
  bool compose_ready = false;  // the compose pass can run; if not, the motion is copied as it is
  // NVIDIA Optical Flow in place of the effect's flow, when a frame asks for it. `colour_view` is how the frame's texture is read.
  nvof::Flow nvof;
  DXGI_FORMAT colour_view = DXGI_FORMAT_UNKNOWN;
  // The frame checks. With the mask on, compose writes to `composed` and the mask writes DLSS-G's input.
  frame_checks::Grid grid;
  frame_checks::Mask mask;
  bool grid_ready = false, mask_ready = false;
  // The camera: motion and depth on a small grid, read before the frame goes in, like the grid above, because the answer is for this frame.
  frame_checks::Samples samples;
  bool samples_ready = false, camera_ready = false;
  camera::Chooser chooser;
  camera::record::FlowWriter flow_file;
  int64_t last_hint = 0;
  ULONGLONG last_camera_log = 0;
  uint32_t camera_ms_count = 0;
  double camera_ms_sum = 0;
  ComPtr<ID3D12Resource> composed;
  D3D12_RESOURCE_STATES composed_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  bool was_still = false;
  ULONGLONG last_cut_log = 0, last_still_log = 0;
  uint32_t still_changes = 0;  // still/moving changes since the last log line
  // Keeping overlays out of generation: DLSS-G's HUD-less image and UI alpha, at the frame's size.
  frame_checks::UiAlpha ui;
  ComPtr<ID3D12Resource> hudless, ui_alpha;
  bool ui_ready = false;
  D3D12_RESOURCE_STATES hudless_state = D3D12_RESOURCE_STATE_COPY_DEST, ui_alpha_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  uint32_t cuts_unlogged = 0;
  // The DLSS-G options last set.
  bool options_set = false, last_on = false;
  uint32_t last_frames = 0;
  bool last_ui = false;
  // GPU guard: Streamline's GPU fence time-outs seen, how many in the current window, and whether DLSS-G is held off for them.
  uint32_t hang_seen = 0, hang_count = 0;
  ULONGLONG hang_first = 0;
  bool hang_off = false;
  // Window.
  HWND parent = nullptr;
  bool overlay = false;  // Vulkan route, HSF.ini VkOverlayWindow: a click-through window of its own over the game's in place of a child (`child` holds it)
  std::atomic<HWND> child{nullptr};
  std::atomic<bool> window_failed{false};
  std::atomic<bool> take_mouse{false};  // presenter -> window thread: Host::TakeMouse()
  // Frame gen off: the game stops sending frames and shows its own, so the child window is hidden until frames come again.
  bool hidden = false;  // presenter thread
};

// Presenter -> window thread: show (wParam 1) or hide (0) the child window.
constexpr UINT WM_HSF_SHOW = WM_APP + 1;

// The frame fence, events, allocators and command list. On failure returns which, with the reason in `hr`; nullptr if all is well.
const char* MakeCommandObjects(Presenter& p, HRESULT& hr);
// The child window's thread: makes a disabled child of p.parent, pumps its messages and keeps it the size of the parent's client area. Ends on
// WM_CLOSE; if `close_when` is given, also destroys the child by itself on its next timer tick once that is set.
void WindowThread(Presenter& p, const std::atomic<bool>* close_when);
// The presenter thread, until host.Stopping(); then DLSS-G off, the GPU idle, untagged and the swap chain released.
void Run(Presenter& p, Host& host);

}  // namespace hsf::dlssg_core
