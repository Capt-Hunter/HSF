// The game side of DLSS-G's inputs (dlssg_remote.cpp): shared textures on the game's
// Direct3D 11 device, made on the first frame that has the input, in its size and (typed) format. What happens to each texture's NT handle
// is up to the caller (dlssg_remote.cpp duplicates it into the helper).
#pragma once

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace hsf::dlssg_slots {

inline DXGI_FORMAT TypedFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return format;
  }
}

// A new shared texture and its NT handle (the caller owns the handle); nullptr, and why in `hr`, if not.
inline HANDLE SharedTexture(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& desc, Microsoft::WRL::ComPtr<ID3D11Texture2D>& texture, HRESULT& hr) {
  Microsoft::WRL::ComPtr<IDXGIResource1> shared;
  HANDLE handle = nullptr;
  if (FAILED(hr = device->CreateTexture2D(&desc, nullptr, &texture)) || FAILED(hr = texture.As(&shared)) ||
      FAILED(hr = shared->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle)))
    return nullptr;
  return handle;
}

// One input's ring of shared textures. Game thread only.
template <UINT COUNT>
class InputSlots {
 public:
  using LogFn = void (*)(const char* format, ...);

  // Makes the slots on the first call, in the size and format of `source`; tried once per Start. `name` is for the log. For each new texture
  // share(i, handle) takes the handle and returns an HRESULT; once all are shared, ready(width, height, typed format) runs.
  template <typename Share, typename Ready>
  bool Prepare(ID3D11Device* device, ID3D11Texture2D* source, const char* name, LogFn log, Share&& share, Ready&& ready) {
    if (made_ || failed_) return made_;
    failed_ = true;
    D3D11_TEXTURE2D_DESC d{};
    source->GetDesc(&d);
    if (d.SampleDesc.Count != 1 || d.ArraySize != 1) {
      log("%s are multisampled or an array (%u samples, %u layers): not used", name, d.SampleDesc.Count, d.ArraySize);
      return false;
    }
    const D3D11_TEXTURE2D_DESC slot_desc{d.Width, d.Height, 1, 1, TypedFormat(d.Format), {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                                         D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
    for (UINT i = 0; i < COUNT; ++i) {
      HRESULT hr = S_OK;
      const HANDLE handle = SharedTexture(device, slot_desc, slots_[i], hr);
      if (handle != nullptr) hr = share(i, handle);
      if (FAILED(hr)) {
        log("sharing %s (%ux%u format %u) failed (0x%08lx)", name, d.Width, d.Height, static_cast<unsigned>(d.Format), hr);
        return false;
      }
    }
    desc_ = slot_desc;
    ready(d.Width, d.Height, slot_desc.Format);
    log("%s shared: %ux%u format %u", name, d.Width, d.Height, static_cast<unsigned>(d.Format));
    failed_ = false;
    made_ = true;
    return true;
  }

  // Copies `source` into a slot; false if it no longer matches the slots (the game changed its size or format).
  bool Copy(ID3D11DeviceContext* context, UINT slot, ID3D11Texture2D* source) {
    D3D11_TEXTURE2D_DESC d{};
    source->GetDesc(&d);
    if (d.Width != desc_.Width || d.Height != desc_.Height || TypedFormat(d.Format) != desc_.Format) return false;
    context->CopyResource(slots_[slot].Get(), source);
    return true;
  }

  UINT Width() const { return desc_.Width; }
  UINT Height() const { return desc_.Height; }
  DXGI_FORMAT Format() const { return desc_.Format; }

 private:
  Microsoft::WRL::ComPtr<ID3D11Texture2D> slots_[COUNT];
  D3D11_TEXTURE2D_DESC desc_{};
  bool made_ = false, failed_ = false;
};

}  // namespace hsf::dlssg_slots
