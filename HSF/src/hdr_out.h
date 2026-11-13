// DLSS-G takes no FP16 (scRGB) frames: NVIDIA's DLSS-G guide, section 11.0, asks for HDR10 (R10G10B10A2, ST.2084 with BT.2020 primaries). A game
// whose back buffer is FP16 gets an R10G10B10A2 DLSS-G swap chain, and this pass writes each frame and HUD-less image into that format.
// The colour maths is RenoDX's (src/shaders/color/rgb.hlsl, pq.hlsl, srgb.hlsl; MIT, Carlos Lopez Jr., commit 216fc64): scRGB 1.0 is 80 nits.
//   pq on:  BT.709 to BT.2020, then the ST.2084 curve (the display is in HDR mode)
//   pq off: the sRGB curve, clipped to 0..1 (the display is in SDR mode, where scRGB 1.0 is SDR white)
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

namespace hsf::hdr_out {

class Pass {
 public:
  // `source_format` is how the FP16 textures are read, `frames` the frames in flight (one set of descriptors each).
  bool Init(ID3D12Device* device, DXGI_FORMAT source_format, UINT width, UINT height, UINT frames, const char*& what, HRESULT& hr);
  // `source` must be readable as a shader resource (COMMON promotes), `out` (R10G10B10A2_UNORM) in UNORDERED_ACCESS. `which` is 0 or 1: two passes
  // a frame (the frame and its HUD-less image) each have their own descriptors. Leaves the list's compute state set.
  void Record(ID3D12GraphicsCommandList* list, UINT frame, UINT which, ID3D12Resource* source, ID3D12Resource* out, bool pq);

 private:
  static constexpr UINT DESCRIPTORS_PER_PASS = 2, PASSES_PER_FRAME = 2;
  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
  UINT descriptor_size_ = 0, frames_ = 0;
  DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
  UINT width_ = 0, height_ = 0;
};

}  // namespace hsf::hdr_out
