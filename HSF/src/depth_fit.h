// Brings a game's depth up to the size of its motion vectors. DLSS-G takes one size for both (sl::DLSSGOptions::mvecDepthWidth), and a game's DLSS can
// give full size vectors (dilated, without DLSS's low resolution flag) with depth at the render size, as Unreal Engine 4 does. Each texel of the result
// takes the nearest depth texel, so edges stay sharp and no depth is made up between surfaces.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

namespace hsf::depthfit {

class Pass {
 public:
  // The result is width x height, R32_FLOAT. `frames` sets of descriptors, one per frame in flight. False if it cannot run here: `what` says which step and `hr` why.
  bool Init(ID3D12Device* device, UINT width, UINT height, UINT frames, const char*& what, HRESULT& hr);

  // Records the pass. `frame` (below Init's `frames`) must have finished on the GPU since it was last used. `depth` readable as a shader resource where it
  // is. Leaves the list's compute state and descriptor heap set, and the result in PIXEL_SHADER_RESOURCE | NON_PIXEL_SHADER_RESOURCE.
  void Record(ID3D12GraphicsCommandList* list, UINT frame, ID3D12Resource* depth);

  ID3D12Resource* Result() const { return out_.Get(); }
  UINT Width() const { return width_; }
  UINT Height() const { return height_; }

 private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> out_;
  UINT width_ = 0, height_ = 0, frames_ = 0;
  UINT descriptor_size_ = 0;
  bool readable_ = false;  // the result is in the read state Record leaves it in
};

}  // namespace hsf::depthfit
