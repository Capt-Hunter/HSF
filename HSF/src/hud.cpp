#include "hud.h"

#include "compute_state.h"

#include <d3dcompiler.h>

#include <cstring>

namespace hsf::hud {

namespace {

// NOISE: as frame_checks.cpp's UI alpha: smaller differences are not HUD, and leave the finished frame exactly as it is. The picture is counted on every 8th pixel each way.
const char SHADER[] = R"(
Texture2D<float4> Finished : register(t0);
Texture2D<float4> AtEffects : register(t1);
Texture2D<float4> BeforeHud : register(t2);
RWTexture2D<float4> Out : register(u0);
RWByteAddressBuffer Count : register(u1);

static const float NOISE = 0.007;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID, uint3 thread : SV_GroupThreadID) {
  const int3 p = int3(id.xy, 0);
  const float4 finished = Finished.Load(p);
  const float3 hud = BeforeHud.Load(p).rgb - AtEffects.Load(p).rgb;
  const bool is_hud = max(abs(hud.r), max(abs(hud.g), abs(hud.b))) >= NOISE;
  Out[id.xy] = is_hud ? float4(finished.rgb + hud, finished.a) : finished;
  if (is_hud && thread.x == 0 && thread.y == 0) {
    uint before;
    Count.InterlockedAdd(0, 1, before);
  }
}
)";

}  // namespace

bool Remover::Same(ID3D11Texture2D* back) const {
  D3D11_TEXTURE2D_DESC d{};
  back->GetDesc(&d);
  return made_ && d.Width == desc_.Width && d.Height == desc_.Height && d.Format == desc_.Format;
}

void Remover::Reset() {
  *this = Remover{};
}

bool Remover::Prepare(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& back, const char*& why) {
  if (made_ && back.Width == desc_.Width && back.Height == desc_.Height && back.Format == desc_.Format) return true;
  if (failed_ && back.Width == desc_.Width && back.Height == desc_.Height && back.Format == desc_.Format) {
    why = nullptr;
    return false;
  }
  Reset();
  desc_ = back;
  failed_ = true;
  if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
    why = "the game's device is below feature level 11_0";
    return false;
  }
  if (back.SampleDesc.Count != 1 || back.ArraySize != 1) {
    why = "the back buffer is multisampled or an array";
    return false;
  }
  if (ReadFormat(back.Format) != back.Format) {
    why = "the back buffer's format cannot be written by a shader as it is";
    return false;
  }
  UINT support = 0;
  if (FAILED(device->CheckFormatSupport(back.Format, &support)) || (support & D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW) == 0) {
    why = "the back buffer's format cannot be written by a shader on this machine";
    return false;
  }
  why = "out of resources making its textures";
  Microsoft::WRL::ComPtr<ID3DBlob> code;
  if (FAILED(D3DCompile(SHADER, sizeof(SHADER) - 1, "hsf_hud", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, nullptr))) {
    why = "its shader did not compile";
    return false;
  }
  if (FAILED(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader_))) return false;
  D3D11_TEXTURE2D_DESC d{back.Width, back.Height, 1, 1, back.Format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
  D3D11_TEXTURE2D_DESC target = d;
  target.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
  if (FAILED(device->CreateTexture2D(&target, nullptr, &before_)) || FAILED(device->CreateRenderTargetView(before_.Get(), nullptr, &before_rtv_))) return false;
  if (FAILED(device->CreateTexture2D(&d, nullptr, &at_effects_)) || FAILED(device->CreateTexture2D(&d, nullptr, &finished_)) ||
      FAILED(device->CreateShaderResourceView(before_.Get(), nullptr, &before_srv_)) || FAILED(device->CreateShaderResourceView(at_effects_.Get(), nullptr, &at_effects_srv_)) ||
      FAILED(device->CreateShaderResourceView(finished_.Get(), nullptr, &finished_srv_)))
    return false;
  d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  if (FAILED(device->CreateTexture2D(&d, nullptr, &out_)) || FAILED(device->CreateUnorderedAccessView(out_.Get(), nullptr, &out_uav_))) return false;
  const D3D11_BUFFER_DESC count{16, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS, 0};
  if (FAILED(device->CreateBuffer(&count, nullptr, &count_))) return false;
  D3D11_UNORDERED_ACCESS_VIEW_DESC view{};
  view.Format = DXGI_FORMAT_R32_TYPELESS;
  view.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
  view.Buffer.NumElements = 4;
  view.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_RAW;
  if (FAILED(device->CreateUnorderedAccessView(count_.Get(), &view, &count_uav_))) return false;
  const D3D11_BUFFER_DESC staging{16, D3D11_USAGE_STAGING, 0, D3D11_CPU_ACCESS_READ, 0, 0};
  for (auto& s : staging_)
    if (FAILED(device->CreateBuffer(&staging, nullptr, &s))) return false;
  failed_ = false;
  made_ = true;
  why = nullptr;
  return true;
}

bool Remover::CopyBeforeHud(ID3D11DeviceContext* context, ID3D11Texture2D* back) {
  if (!Same(back)) return false;
  context->CopyResource(before_.Get(), back);
  return true;
}

bool Remover::CopySource(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source, const D3D11_SHADER_RESOURCE_VIEW_DESC& view) {
  if (!made_) return false;
  D3D11_TEXTURE2D_DESC want{};
  source->GetDesc(&want);
  if (want.SampleDesc.Count != 1 || want.ArraySize != 1) return false;
  want.Usage = D3D11_USAGE_DEFAULT;
  want.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  want.CPUAccessFlags = 0;
  want.MiscFlags = 0;
  bool same = false;
  if (source_ != nullptr) {
    D3D11_TEXTURE2D_DESC have{};
    source_->GetDesc(&have);
    same = have.Width == want.Width && have.Height == want.Height && have.Format == want.Format && have.MipLevels == want.MipLevels && std::memcmp(&source_view_, &view, sizeof(view)) == 0;
  }
  if (!same) {
    source_.Reset();
    source_srv_.Reset();
    if (FAILED(device->CreateTexture2D(&want, nullptr, &source_)) || FAILED(device->CreateShaderResourceView(source_.Get(), &view, &source_srv_))) {
      source_.Reset();
      source_srv_.Reset();
      return false;
    }
    source_view_ = view;
  }
  context->CopyResource(source_.Get(), source);
  return true;
}

bool Remover::CopyAtEffects(ID3D11DeviceContext* context, ID3D11Texture2D* back) {
  if (!Same(back)) return false;
  context->CopyResource(at_effects_.Get(), back);
  return true;
}

ID3D11Texture2D* Remover::Make(ID3D11DeviceContext* context, ID3D11Texture2D* finished) {
  if (!Same(finished)) return nullptr;
  context->CopyResource(finished_.Get(), finished);
  const UINT zero[4] = {};
  context->ClearUnorderedAccessViewUint(count_uav_.Get(), zero);
  {
    SavedComputeState saved(context);
    ID3D11ShaderResourceView* const srv[3] = {finished_srv_.Get(), at_effects_srv_.Get(), before_srv_.Get()};
    ID3D11UnorderedAccessView* const uav[2] = {out_uav_.Get(), count_uav_.Get()};
    context->CSSetShader(shader_.Get(), nullptr, 0);
    context->CSSetShaderResources(0, 3, srv);
    context->CSSetUnorderedAccessViews(0, 2, uav, nullptr);
    context->Dispatch((desc_.Width + 7) / 8, (desc_.Height + 7) / 8, 1);
  }
  if (pending_ < STAGING) {
    context->CopyResource(staging_[write_].Get(), count_.Get());
    write_ = (write_ + 1) % STAGING;
    ++pending_;
  }
  return out_.Get();
}

bool Remover::ReadShare(ID3D11DeviceContext* context, float& share) {
  if (pending_ == 0) return false;
  D3D11_MAPPED_SUBRESOURCE map{};
  if (context->Map(staging_[read_].Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map) != S_OK) return false;
  const UINT counted = *static_cast<const UINT*>(map.pData);
  context->Unmap(staging_[read_].Get(), 0);
  read_ = (read_ + 1) % STAGING;
  --pending_;
  const UINT total = ((desc_.Width + 7) / 8) * ((desc_.Height + 7) / 8);
  share = static_cast<float>(counted) / static_cast<float>(total);
  return true;
}

}  // namespace hsf::hud
