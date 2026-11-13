// A look at what DLSS-G is given (Direct3D 12 games HSF brought Streamline into). Replaces the picture with this frame's depth or motion, exactly as tagged for DLSS-G,
// so they can be checked by eye. Depth: near is bright, far is dark, whichever way round the game keeps it. Motion: still is grey, moving is coloured,
// red for right and green for down, full colour at 2.5% of the picture per frame.
#pragma once

#include <d3d12.h>
#include <wrl/client.h>

namespace hsf::debugview {

enum class Kind : unsigned { game = 0, depth = 1, motion = 2 };

class Pass {
 public:
  // `back_format` is the back buffer's format. False if the view cannot run on this device: `what` says which step and `hr` why.
  bool Init(ID3D12Device* device, DXGI_FORMAT back_format, UINT width, UINT height, const char*& what, HRESULT& hr);

  // Records the view on `list` and copies it over `back_buffer`, which is left in `back_state`. `frame` below compose::FRAMES and finished on the GPU since it was last used.
  // `motion` and `depth` readable as shader resources, as they are when tagged for DLSS-G. The part in use is x, y, width, height of them (width 0: all of them).
  // `scale` is DLSS-G's mvecScale. Leaves the list's compute state and descriptor heap set.
  void Record(ID3D12GraphicsCommandList* list, UINT frame, Kind kind, ID3D12Resource* motion, ID3D12Resource* depth, UINT x, UINT y, UINT width, UINT height,
              bool depth_inverted, const float scale[2], ID3D12Resource* back_buffer, D3D12_RESOURCE_STATES back_state);

 private:
  Microsoft::WRL::ComPtr<ID3D12Device> device_;
  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
  Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
  Microsoft::WRL::ComPtr<ID3D12Resource> out_;
  DXGI_FORMAT view_format_ = DXGI_FORMAT_UNKNOWN;
  UINT width_ = 0, height_ = 0;
  UINT descriptor_size_ = 0;
};

}  // namespace hsf::debugview
