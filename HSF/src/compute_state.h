// What score.cpp and depth.cpp share: both run compute shaders on the game's own immediate context and read copies of the game's frame.
#pragma once

#include <d3d11.h>

namespace hsf {

// The format to copy into and read as: typeless made typed, sRGB read as stored, so the score compares what the game's pixels hold.
inline DXGI_FORMAT ReadFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return format;
  }
}

// The game's compute state, put back after HSF's dispatches.
class SavedComputeState {
 public:
  explicit SavedComputeState(ID3D11DeviceContext* context) : context_(context) {
    context_->CSGetShader(&shader_, instances_, &instance_count_);
    context_->CSGetShaderResources(0, 3, srv_);
    context_->CSGetUnorderedAccessViews(0, 4, uav_);
    context_->CSGetConstantBuffers(0, 1, &constants_);
    context_->CSGetSamplers(0, 2, samplers_);
  }
  ~SavedComputeState() {
    context_->CSSetShader(shader_, instances_, instance_count_);
    context_->CSSetShaderResources(0, 3, srv_);
    const UINT keep[4] = {static_cast<UINT>(-1), static_cast<UINT>(-1), static_cast<UINT>(-1), static_cast<UINT>(-1)};
    context_->CSSetUnorderedAccessViews(0, 4, uav_, keep);
    context_->CSSetConstantBuffers(0, 1, &constants_);
    context_->CSSetSamplers(0, 2, samplers_);
    if (shader_ != nullptr) shader_->Release();
    for (UINT i = 0; i < instance_count_; ++i)
      if (instances_[i] != nullptr) instances_[i]->Release();
    for (auto* view : srv_)
      if (view != nullptr) view->Release();
    for (auto* view : uav_)
      if (view != nullptr) view->Release();
    if (constants_ != nullptr) constants_->Release();
    for (auto* sampler : samplers_)
      if (sampler != nullptr) sampler->Release();
  }
  SavedComputeState(const SavedComputeState&) = delete;
  SavedComputeState& operator=(const SavedComputeState&) = delete;

 private:
  ID3D11DeviceContext* context_;
  ID3D11ComputeShader* shader_ = nullptr;
  ID3D11ClassInstance* instances_[256] = {};
  UINT instance_count_ = 256;
  ID3D11ShaderResourceView* srv_[3] = {};
  ID3D11UnorderedAccessView* uav_[4] = {};
  ID3D11Buffer* constants_ = nullptr;
  ID3D11SamplerState* samplers_[2] = {};
};

}  // namespace hsf
