// Offline test of the depth and motion view (src/debug_view.cpp), no game needed. Inputs half the picture's size, near and still on the left, far and moving right on
// the right. Checks the picture shows near bright and far dark, still grey and moving red, and that only the part in use is shown. debug_view_test.exe; exit code 0 = passed.

#include "../src/debug_view.h"

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
int g_failed = 0;

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

// The back buffer's B8G8R8A8 pixels, read in RENDER_TARGET and left there.
std::vector<unsigned char> ReadBack(ID3D12Resource* back, UINT w, UINT h) {
  Barrier(back, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_RESOURCE_DESC d = back->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT place{};
  UINT64 total = 0;
  g_device->GetCopyableFootprints(&d, 0, 1, 0, &place, nullptr, nullptr, &total);
  const auto down = Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION dst{down.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
  dst.PlacedFootprint = place;
  D3D12_TEXTURE_COPY_LOCATION src{back, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
  g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
  Barrier(back, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
  Run();
  std::vector<unsigned char> pixels(4ull * w * h);
  char* mapped = nullptr;
  down->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
  for (UINT y = 0; y < h; ++y) std::memcpy(&pixels[4ull * y * w], mapped + y * place.Footprint.RowPitch, 4ull * w);
  down->Unmap(0, nullptr);
  return pixels;
}

void Check(bool ok, const char* what, const unsigned char* bgra) {
  std::printf("%s %s: r %u g %u b %u\n", ok ? "ok  " : "FAIL", what, bgra[2], bgra[1], bgra[0]);
  if (!ok) ++g_failed;
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

  constexpr UINT W = 64, H = 32, IW = 32, IH = 16;
  // Left half: near (reversed depth, 1) and still. Right half: far (0.001) and moving 2 texels right.
  std::vector<float> motion(2ull * IW * IH), depth(1ull * IW * IH);
  for (UINT y = 0; y < IH; ++y)
    for (UINT x = 0; x < IW; ++x) {
      const bool right = x >= IW / 2;
      depth[y * IW + x] = right ? 0.001f : 1.f;
      motion[2ull * (y * IW + x)] = right ? 2.f : 0.f;
      motion[2ull * (y * IW + x) + 1] = 0.f;
    }
  const auto m = Texture(IW, IH, DXGI_FORMAT_R32G32_FLOAT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  const auto z = Texture(IW, IH, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  Upload(m.Get(), motion.data(), IW * 8, IH, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  Upload(z.Get(), depth.data(), IW * 4, IH, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  const auto back = Texture(W, H, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);

  hsf::debugview::Pass pass;
  const char* what = nullptr;
  HRESULT hr = S_OK;
  if (!pass.Init(g_device.Get(), DXGI_FORMAT_B8G8R8A8_UNORM, W, H, what, hr)) {
    std::printf("FAIL Init: %s (0x%08lx)\n", what, hr);
    return 1;
  }
  const float scale[2] = {1.f / IW, 1.f / IH};
  const auto at = [](const std::vector<unsigned char>& p, UINT x, UINT y) { return &p[4ull * (y * W + x)]; };

  pass.Record(g_list.Get(), 0, hsf::debugview::Kind::depth, m.Get(), z.Get(), 0, 0, 0, 0, true, scale, back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
  Run();
  auto p = ReadBack(back.Get(), W, H);
  Check(at(p, 5, 5)[2] > 240, "depth: near is bright", at(p, 5, 5));
  Check(at(p, 60, 20)[2] > 20 && at(p, 60, 20)[2] < 50, "depth: far is dark but not black", at(p, 60, 20));

  // The same depth read the other way round: 1 is far.
  pass.Record(g_list.Get(), 1, hsf::debugview::Kind::depth, m.Get(), z.Get(), 0, 0, 0, 0, false, scale, back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
  Run();
  p = ReadBack(back.Get(), W, H);
  Check(at(p, 5, 5)[2] < 5, "depth not inverted: 1 is far, black", at(p, 5, 5));

  pass.Record(g_list.Get(), 2, hsf::debugview::Kind::motion, m.Get(), z.Get(), 0, 0, 0, 0, true, scale, back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
  Run();
  p = ReadBack(back.Get(), W, H);
  const unsigned char* still = at(p, 5, 5);
  Check(still[2] > 120 && still[2] < 136 && still[1] > 120 && still[1] < 136, "motion: still is grey", still);
  Check(at(p, 60, 20)[2] > 250 && at(p, 60, 20)[1] < 136, "motion: moving right is red", at(p, 60, 20));

  // Only the right half in use: the whole picture shows it.
  pass.Record(g_list.Get(), 0, hsf::debugview::Kind::depth, m.Get(), z.Get(), IW / 2, 0, IW / 2, IH, true, scale, back.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
  Run();
  p = ReadBack(back.Get(), W, H);
  Check(at(p, 1, 1)[2] < 50, "part in use: left edge shows the far half", at(p, 1, 1));

  std::printf(g_failed == 0 ? "passed\n" : "%d FAILED\n", g_failed);
  return g_failed == 0 ? 0 : 1;
}
