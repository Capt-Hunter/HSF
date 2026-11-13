// Offline test of the depth fit pass (src/depth_fit.cpp), no game needed: a 4x2 depth brought up to 8x4 must give each texel the depth texel it falls in.
// depth_fit_test.exe; exit code 0 = passed.

#include "../src/depth_fit.h"

#include <dxgi1_6.h>

#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<ID3D12Device> g_device;
ComPtr<ID3D12CommandQueue> g_queue;
ComPtr<ID3D12CommandAllocator> g_alloc;
ComPtr<ID3D12GraphicsCommandList> g_list;
ComPtr<ID3D12Fence> g_fence;
UINT64 g_value = 0;

void Run() {
  g_list->Close();
  ID3D12CommandList* const lists[] = {g_list.Get()};
  g_queue->ExecuteCommandLists(1, lists);
  g_queue->Signal(g_fence.Get(), ++g_value);
  const HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  g_fence->SetEventOnCompletion(g_value, done);
  WaitForSingleObject(done, INFINITE);
  CloseHandle(done);
  g_alloc->Reset();
  g_list->Reset(g_alloc.Get(), nullptr);
}

void Barrier(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b{};
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  g_list->ResourceBarrier(1, &b);
}

ComPtr<ID3D12Resource> Texture(UINT w, UINT h, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state) {
  const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = w;
  d.Height = h;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = format;
  d.SampleDesc.Count = 1;
  d.Flags = flags;
  ComPtr<ID3D12Resource> r;
  g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r));
  return r;
}

ComPtr<ID3D12Resource> Buffer(UINT64 size, D3D12_HEAP_TYPE type, D3D12_RESOURCE_STATES state) {
  const D3D12_HEAP_PROPERTIES heap{type};
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = size;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> r;
  g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, state, nullptr, IID_PPV_ARGS(&r));
  return r;
}

void Upload(ID3D12Resource* texture, const void* pixels, UINT row_bytes, UINT h, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_DESC d = texture->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT place{};
  UINT64 total = 0;
  g_device->GetCopyableFootprints(&d, 0, 1, 0, &place, nullptr, nullptr, &total);
  ComPtr<ID3D12Resource> up = Buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
  char* mapped = nullptr;
  up->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
  for (UINT y = 0; y < h; ++y) std::memcpy(mapped + y * place.Footprint.RowPitch, static_cast<const char*>(pixels) + y * row_bytes, row_bytes);
  up->Unmap(0, nullptr);
  D3D12_TEXTURE_COPY_LOCATION dst{texture, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
  D3D12_TEXTURE_COPY_LOCATION src{up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
  src.PlacedFootprint = place;
  g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  Barrier(texture, D3D12_RESOURCE_STATE_COPY_DEST, after);
  Run();
}

}  // namespace

int wmain() {
  ComPtr<IDXGIFactory6> factory;
  ComPtr<IDXGIAdapter1> adapter;
  CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
  factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
  if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_device)))) {
    std::printf("no Direct3D 12 device\n");
    return 2;
  }
  D3D12_COMMAND_QUEUE_DESC qdesc{};
  g_device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&g_queue));
  g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc));
  g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc.Get(), nullptr, IID_PPV_ARGS(&g_list));
  g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));

  constexpr UINT IW = 4, IH = 2, W = 8, H = 4;
  const float depth[IW * IH] = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
  const auto z = Texture(IW, IH, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  Upload(z.Get(), depth, IW * 4, IH, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  hsf::depthfit::Pass pass;
  const char* what = nullptr;
  HRESULT hr = S_OK;
  if (!pass.Init(g_device.Get(), W, H, 3, what, hr)) {
    std::printf("FAIL Init: %s (0x%08lx)\n", what, hr);
    return 1;
  }
  // Twice, so the second Record starts from the read state the first left.
  for (UINT round = 0; round < 2; ++round) {
    pass.Record(g_list.Get(), round, z.Get());
    Run();
  }
  ID3D12Resource* const out = pass.Result();
  Barrier(out, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_RESOURCE_DESC d = out->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT place{};
  UINT64 total = 0;
  g_device->GetCopyableFootprints(&d, 0, 1, 0, &place, nullptr, nullptr, &total);
  const auto down = Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION dst{down.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
  dst.PlacedFootprint = place;
  D3D12_TEXTURE_COPY_LOCATION src{out, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
  g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  Run();
  char* mapped = nullptr;
  down->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
  int failed = 0;
  for (UINT y = 0; y < H; ++y)
    for (UINT x = 0; x < W; ++x) {
      float v = 0;
      std::memcpy(&v, mapped + y * place.Footprint.RowPitch + 4 * x, 4);
      const float want = depth[(y / 2) * IW + x / 2];
      if (v != want) {
        std::printf("FAIL texel %u,%u: %f, want %f\n", x, y, v, want);
        ++failed;
      }
    }
  down->Unmap(0, nullptr);
  std::printf(failed == 0 ? "passed\n" : "%d FAILED\n", failed);
  return failed == 0 ? 0 : 1;
}
