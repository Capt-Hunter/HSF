// The compose pass: uses the frame's depth to clean up the optical flow before DLSS-G gets it. Optical flow is soft at the
// edges of objects and spills across them; depth has sharp edges. For each pixel the shader takes the nearest surface in its 3x3 (so a foreground
// edge keeps the foreground's motion, FSR 3's dilation) and averages the motion around it, weighing each sample by how close its depth is to that
// surface's (a joint bilateral filter). It then steadies the result over time: optical flow shakes from frame to frame, so each pixel follows its own
// motion back to where it was last frame and blends in the motion it had there, if that place was the same surface and the two motions nearly agree.
// A real change of motion falls outside the agreement and goes through at once. It runs on HSF's own Direct3D 12 device, so one shader serves every
// game API and both bitnesses.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

#include <cmath>

namespace hsf::compose {

constexpr UINT FRAMES = 3;  // frames in flight: one set of descriptors and timestamps each

// The still world's motion from the camera (camera.h), for one frame.
struct Camera {
  ID3D12Resource* colour = nullptr;      // this frame's picture, readable as a shader resource (COMMON promotes). Next frame's "last picture".
  const float* clip_to_prev = nullptr;   // this frame's clip position to last frame's, m[4 * row + column], or nullptr: flow only this frame
};

// How much smaller than the picture the pass works, on each side: the pass costs about the same per pixel, so it gets about 1920x1200's worth of
// pixels at most. 1 up to 1920x1200, 2 at 1440p, 3440x1440 and 4K, 3 above that (5K, 8K). Never more than 3, as the edge band that takes the near
// surface's motion grows with it.
inline UINT WorkDivisor(UINT width, UINT height) {
  const double share = static_cast<double>(width) * height / (1920.0 * 1200.0);
  if (share <= 1.0) return 1;
  const UINT d = static_cast<UINT>(std::ceil(std::sqrt(share) - 1e-9));
  return d < 2 ? 2 : d > 3 ? 3 : d;
}

class Pass {
 public:
  // False if this device or motion format cannot run the pass: `what` says which step and `hr` why. The caller then copies the motion as it is.
  bool Init(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT motion_format, UINT width, UINT height, const char*& what, HRESULT& hr);
  // For Record's `camera`. `resource_format` is the picture's texture's, `view_format` how to read it.
  bool InitCamera(DXGI_FORMAT resource_format, DXGI_FORMAT view_format, UINT frame_width, UINT frame_height, const char*& what, HRESULT& hr);

  // Records the pass. `frame` (below FRAMES) must have finished on the GPU since it was last used. `motion` must be readable as a shader resource
  // (COMMON promotes), `depth` (R32_FLOAT) in NON_PIXEL_SHADER_RESOURCE, `out` in UNORDERED_ACCESS; all three the same size. Leaves the list's
  // compute state set, and `depth` and `out` in the states they came in. `scale` is DLSS-G's mvecScale: a motion texel times it is the offset, as a share
  // of the picture, to where the pixel was one frame earlier. `reset`: this frame does not follow from the last one, so last frame's motion is not used.
  // `camera`, after InitCamera: a pixel takes the camera's motion where it agrees with the flow, or where last frame's picture says the camera explains it clearly better.
  void Record(ID3D12GraphicsCommandList* list, UINT frame, ID3D12Resource* motion, ID3D12Resource* depth, ID3D12Resource* out, UINT width, UINT height,
              bool depth_inverted, const float scale[2], bool reset, const Camera* camera = nullptr);

  // The next Record must not use last frame's motion: a frame went by without this pass.
  void ForgetHistory() {
    have_history_ = false;
    have_last_colour_ = false;
  }

  // True once `every` finished passes have been timed: `ms` is their mean on the GPU, and the count starts again.
  bool TakeAverageMs(UINT every, double& ms);

  // The size the pass works at: Init's divided by WorkDivisor. `out` is always Init's size.
  UINT WorkWidth() const { return work_width_; }
  UINT WorkHeight() const { return work_height_; }

 private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_, pso_down_, pso_up_;
  Microsoft::WRL::ComPtr<ID3D12Resource> half_depth_, half_out_;  // at a smaller size: DOWN's depth and the pass's result, kept in UNORDERED_ACCESS between frames
  bool reduced_ = false;
  UINT work_width_ = 0, work_height_ = 0;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
  Microsoft::WRL::ComPtr<ID3D12QueryHeap> queries_;
  Microsoft::WRL::ComPtr<ID3D12Resource> readback_;
  Microsoft::WRL::ComPtr<ID3D12Resource> history_motion_, history_depth_;  // last frame's result and depth; one queue, so its order keeps them right
  Microsoft::WRL::ComPtr<ID3D12Resource> last_colour_;  // last frame's picture, for the camera
  bool have_last_colour_ = false;
  DXGI_FORMAT colour_format_ = DXGI_FORMAT_UNKNOWN;
  UINT frame_width_ = 0, frame_height_ = 0;
  bool have_history_ = false;
  bool last_inverted_ = false;
  float last_scale_[2] = {};
  const UINT64* times_ = nullptr;
  bool timed_[FRAMES] = {};
  UINT descriptor_size_ = 0;
  DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
  double ticks_per_ms_ = 0;
  double sum_ms_ = 0;
  UINT count_ = 0;
};

}  // namespace hsf::compose
