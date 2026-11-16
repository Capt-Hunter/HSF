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

// The game's compute state, put back after HSF's dispatches. Only the first VIEWS views, OUTPUTS outputs, BUFFERS constant buffers and
// SAMPLERS samplers are saved: a dispatch that binds more would leave the game's state changed. Callers check their counts against these.
class SavedComputeState {
 public:
  static constexpr UINT VIEWS = 3;
  static constexpr UINT OUTPUTS = 4;
  static constexpr UINT BUFFERS = 1;
  static constexpr UINT SAMPLERS = 2;

  explicit SavedComputeState(ID3D11DeviceContext* context) : context_(context) {
    context_->CSGetShader(&shader_, instances_, &instance_count_);
    context_->CSGetShaderResources(0, VIEWS, srv_);
    context_->CSGetUnorderedAccessViews(0, OUTPUTS, uav_);
    context_->CSGetConstantBuffers(0, BUFFERS, constants_);
    context_->CSGetSamplers(0, SAMPLERS, samplers_);
  }
  ~SavedComputeState() {
    context_->CSSetShader(shader_, instances_, instance_count_);
    context_->CSSetShaderResources(0, VIEWS, srv_);
    UINT keep[OUTPUTS];
    for (UINT& k : keep) k = static_cast<UINT>(-1);
    context_->CSSetUnorderedAccessViews(0, OUTPUTS, uav_, keep);
    context_->CSSetConstantBuffers(0, BUFFERS, constants_);
    context_->CSSetSamplers(0, SAMPLERS, samplers_);
    if (shader_ != nullptr) shader_->Release();
    for (UINT i = 0; i < instance_count_; ++i)
      if (instances_[i] != nullptr) instances_[i]->Release();
    for (auto* view : srv_)
      if (view != nullptr) view->Release();
    for (auto* view : uav_)
      if (view != nullptr) view->Release();
    for (auto* buffer : constants_)
      if (buffer != nullptr) buffer->Release();
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
  ID3D11ShaderResourceView* srv_[VIEWS] = {};
  ID3D11UnorderedAccessView* uav_[OUTPUTS] = {};
  ID3D11Buffer* constants_[BUFFERS] = {};
  ID3D11SamplerState* samplers_[SAMPLERS] = {};
};

}  // namespace hsf
