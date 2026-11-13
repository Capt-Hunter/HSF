// NVIDIA Optical Flow (NVOFA) as a motion source: the flow comes from the GPU's own optical flow engine instead of a ReShade effect, so it takes no
// time out of the game's frame the way a shader does. The engine is in the display driver (nvofapi64.dll, RTX 20 and newer; HSF never ships it).
// Each frame: the picture is shrunk to a small grey image, the engine matches it against the last frame's in both directions and says how good each match
// is, and a compute shader turns that into motion in the same texture size, format and scale the effect's flow has. So everything after it (the compose
// pass, the HUD mask, DLSS-G) takes it without knowing which of the two it got. It runs on HSF's own Direct3D 12 work, in the helper and in game.
// No CPU waits: the queue waits for the engine on the GPU, and a signal that never comes is let go after half a second (the DX9 fence lesson).
//
// Ported from DLSS5 ReShade AIO's nvof-motion-provider.cpp (Copyright 2026 kibblerz, Apache-2.0; see third_party/kibblerz-aio). Changed for HSF: one
// call per frame on the caller's queue and command list, flow for this frame (not a queued one), output in the caller's motion format and scale,
// no history mask or visualisation, a watchdog on the engine's fence.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

namespace hsf::nvof {

using LogFn = void (*)(const char* text);

constexpr UINT SLOTS = 3;         // frames in flight, as compose::FRAMES
constexpr UINT DEFAULT_SIZE = 360;  // the grey image's height. AIO measured 720 against 180: the small one was three times quicker and looked no worse. In Village 360 looked better than 180 by eye, so 360 is the default

// One frame for the engine.
struct Frame {
  ID3D12Resource* colour = nullptr;  // the finished picture
  DXGI_FORMAT colour_view = DXGI_FORMAT_UNKNOWN;  // how to read it
  D3D12_RESOURCE_STATES colour_state = D3D12_RESOURCE_STATE_COMMON;  // the state it is in, and is left in
  bool copy_colour = false;  // the picture cannot be read by a shader (a swap chain's back buffer): copy it first
  ID3D12Resource* depth = nullptr;  // R32_FLOAT, readable by a compute shader, any size; or nullptr: no depth checks
  bool depth_inverted = false;
  UINT width = 0, height = 0;  // the motion texture to make
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  float scale[2] = {};  // DLSS-G's mvecScale for it: a texel times this is the offset, as a share of the picture, to where the pixel was a frame earlier
  bool reset = false;   // the picture does not follow from the last one
  UINT size = DEFAULT_SIZE;  // the grey image's height, 0 = the picture's own
};

enum class State { off, waiting, active, failed };

class Flow {
 public:
  ~Flow() { Shutdown(); }
  // On the thread that owns `queue`, with everything recorded for it so far already submitted. Sends the picture to the engine, makes the queue wait for the
  // answer on the GPU, and records the conversion on `list`, which the caller must execute on `queue` next. Returns this frame's motion, readable as a shader
  // resource (NON_PIXEL_SHADER_RESOURCE) once `list` has run that far, or nullptr: no flow this frame (the first frame, a frame after a failure, or the engine
  // is not there). Leaves the list's compute state set.
  ID3D12Resource* Run(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list, const Frame& frame, LogFn log);
  // The next Run has no last picture to match against.
  void Forget() { previous_ = -1; }
  void Shutdown();
  State GetState() const { return state_; }
  // "320x180, grid 1" once running, or why it is not.
  const char* Status() const { return status_; }

 private:
  bool Start(ID3D12Device* device, ID3D12CommandQueue* queue, const Frame& frame);
  bool MakePipelines();
  bool MakeResources(const Frame& frame);
  bool Register(ID3D12Resource* resource, void** handle);
  void Fail(const char* format, ...);
  void Say(const char* format, ...);

  struct Slot {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
    Microsoft::WRL::ComPtr<ID3D12Resource> input;  // the grey image
    void* input_handle = nullptr;
    UINT64 free_at = 0;  // done_ value after which nothing on the GPU uses this slot
  };

  LogFn log_ = nullptr;
  State state_ = State::off;
  char status_[200] = "not started";
  bool tried_ = false;
  // What the session was made for: a change starts it again.
  ID3D12Device* device_ = nullptr;
  ID3D12CommandQueue* queue_ = nullptr;
  UINT colour_width_ = 0, colour_height_ = 0, out_width_ = 0, out_height_ = 0, size_ = 0;
  DXGI_FORMAT colour_format_ = DXGI_FORMAT_UNKNOWN, colour_view_ = DXGI_FORMAT_UNKNOWN, out_format_ = DXGI_FORMAT_UNKNOWN;
  bool copy_colour_ = false;

  void* api_ = nullptr;      // NV_OF_D3D12_API_FUNCTION_LIST
  void* session_ = nullptr;  // NvOFHandle
  UINT flow_width_ = 0, flow_height_ = 0, grid_ = 1;
  DXGI_FORMAT cost_format_ = DXGI_FORMAT_UNKNOWN;
  Microsoft::WRL::ComPtr<ID3D12Fence> prep_fence_, flow_fence_, register_fence_, done_;
  UINT64 prep_value_ = 0, flow_value_ = 0, register_value_ = 0, done_value_ = 0;
  HANDLE event_ = nullptr;
  ULONGLONG flow_sent_ms_ = 0;  // when the newest engine call went out, for the watchdog
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> prep_list_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> prep_root_, convert_root_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> prep_pso_, convert_pso_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
  UINT descriptor_size_ = 0;
  Slot slots_[SLOTS];
  Microsoft::WRL::ComPtr<ID3D12Resource> copy_;  // the picture's copy, when it cannot be read where it is
  Microsoft::WRL::ComPtr<ID3D12Resource> forward_, backward_, forward_cost_, backward_cost_, out_;
  void* forward_handle_ = nullptr;
  void* backward_handle_ = nullptr;
  void* forward_cost_handle_ = nullptr;
  void* backward_cost_handle_ = nullptr;
  UINT64 count_ = 0;
  int previous_ = -1;  // the slot with the last picture, or -1
  uint32_t made_ = 0, skipped_ = 0;
  ULONGLONG last_report_ms_ = 0;
};

}  // namespace hsf::nvof
