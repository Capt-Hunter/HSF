// Apex ANR FG's direct D3D12 provider. HSF owns the native swap chain and its pacing.
#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <memory>

namespace hsf::slbring {
struct FrameFeed;
}

namespace hsf::apex {

class Provider {
public:
  Provider();
  ~Provider();
  bool Init(const wchar_t* path, ID3D12Device* device, bool debug);
  unsigned MaxGenerated() const;

private:
  struct Api;
  std::unique_ptr<Api> api_;
  friend class Session;
};

enum class FrameResult { failed, real_only, generated };

class Session {
public:
  Session();
  ~Session();
  bool Init(Provider& provider, const slbring::FrameFeed& feed, unsigned input_width, unsigned input_height);
  FrameResult Record(ID3D12GraphicsCommandList* list, const slbring::FrameFeed& feed, ID3D12Resource* motion,
                     D3D12_RESOURCE_STATES motion_state, ID3D12Resource* depth, bool dilated, bool reset, unsigned frames = 1);
  bool BeginPresent(ID3D12Resource* back);
  HRESULT CopySecondGenerated(IDXGISwapChain3* swap);
  HRESULT RestoreReal(IDXGISwapChain3* swap);
  void EndPresent();

private:
  struct State;
  std::unique_ptr<State> state_;
};

}  // namespace hsf::apex