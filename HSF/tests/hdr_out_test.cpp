// Offline test of the FP16 to R10G10B10A2 pass (src/hdr_out.cpp), no game needed. scRGB pixels of known brightness go in; the HDR10 and sRGB codes that come
// out are checked against the ST.2084 and sRGB curves worked out here on the CPU. hdr_out_test.exe; exit code 0 = passed.

#include "../src/hdr_out.h"

#include <DirectXPackedVector.h>
#include <dxgi1_6.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
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

// The CPU's own ST.2084 curve, from the standard's constants, for luminance in nits.
double Pq(double nits) {
  const double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 4096.0 * 128.0, c1 = 3424.0 / 4096.0, c2 = 2413.0 / 4096.0 * 32.0, c3 = 2392.0 / 4096.0 * 32.0;
  const double y = std::pow(std::fmax(nits, 0.0) / 10000.0, m1);
  return std::pow((c1 + c2 * y) / (1.0 + c3 * y), m2);
}

double Srgb(double c) {
  c = std::fmin(std::fmax(c, 0.0), 1.0);
  return c <= 0.0031308 ? c * 12.92 : 1.055 * std::pow(c, 1.0 / 2.4) - 0.055;
}

void Check(const char* what, unsigned code, double want) {
  const unsigned expected = static_cast<unsigned>(std::lround(want * 1023.0));
  const bool ok = (code > expected ? code - expected : expected - code) <= 2;
  std::printf("%s %s: %u (want %u)\n", ok ? "ok  " : "FAIL", what, code, expected);
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

  // scRGB pixels: black, 1.0 (80 nits, SDR white), 2.5375 (203 nits, HDR reference white), 125 (10000 nits), pure red at 1.0, 0.5 grey, NaN, negative.
  constexpr UINT W = 8, H = 1;
  const float nan = std::numeric_limits<float>::quiet_NaN();
  const float in[W][3] = {{0, 0, 0}, {1, 1, 1}, {2.5375f, 2.5375f, 2.5375f}, {125, 125, 125}, {1, 0, 0}, {0.5f, 0.5f, 0.5f}, {nan, nan, nan}, {-1, -1, -1}};
  uint16_t half[W][4];
  for (UINT x = 0; x < W; ++x) {
    for (int c = 0; c < 3; ++c) half[x][c] = DirectX::PackedVector::XMConvertFloatToHalf(in[x][c]);
    half[x][3] = DirectX::PackedVector::XMConvertFloatToHalf(1.f);
  }

  // The game's slot is typeless, as a shared back buffer copy may be; the pass reads it as R16G16B16A16_FLOAT.
  const auto source = Texture(W, H, DXGI_FORMAT_R16G16B16A16_TYPELESS, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
  {
    D3D12_RESOURCE_DESC d = source->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT place{};
    UINT64 total = 0;
    g_device->GetCopyableFootprints(&d, 0, 1, 0, &place, nullptr, nullptr, &total);
    const auto up = Buffer(total, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    char* mapped = nullptr;
    up->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    std::memcpy(mapped, half, sizeof(half));
    up->Unmap(0, nullptr);
    D3D12_TEXTURE_COPY_LOCATION dst{source.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    D3D12_TEXTURE_COPY_LOCATION src{up.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    src.PlacedFootprint = place;
    g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(source.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Run();
  }

  hsf::hdr_out::Pass pass;
  const char* what = nullptr;
  HRESULT hr = S_OK;
  if (!pass.Init(g_device.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, W, H, 3, what, hr)) {
    std::printf("FAIL Init: %s (0x%08lx)\n", what, hr);
    return 1;
  }
  const auto out = Texture(W, H, DXGI_FORMAT_R10G10B10A2_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

  const auto convert = [&](bool pq, UINT frame, UINT which) {
    pass.Record(g_list.Get(), frame, which, source.Get(), out.Get(), pq);
    Barrier(out.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_RESOURCE_DESC d = out->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT place{};
    UINT64 total = 0;
    g_device->GetCopyableFootprints(&d, 0, 1, 0, &place, nullptr, nullptr, &total);
    const auto down = Buffer(total, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst{down.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    dst.PlacedFootprint = place;
    D3D12_TEXTURE_COPY_LOCATION src{out.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    g_list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Barrier(out.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Run();
    std::vector<uint32_t> codes(W);
    char* mapped = nullptr;
    down->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
    std::memcpy(codes.data(), mapped, W * sizeof(uint32_t));
    down->Unmap(0, nullptr);
    return codes;
  };
  const auto r = [](uint32_t v) { return v & 1023u; };
  const auto g = [](uint32_t v) { return (v >> 10) & 1023u; };
  const auto b = [](uint32_t v) { return (v >> 20) & 1023u; };

  // HDR10: grey stays grey through BT.709 to BT.2020; pure BT.709 red becomes BT.2020 (0.6274, 0.0691, 0.0164).
  std::vector<uint32_t> hdr = convert(true, 0, 0);
  Check("HDR10 black", r(hdr[0]), Pq(0));
  Check("HDR10 scRGB 1.0 = 80 nits", g(hdr[1]), Pq(80));
  Check("HDR10 scRGB 2.5375 = 203 nits", g(hdr[2]), Pq(203));
  Check("HDR10 scRGB 125 = 10000 nits", b(hdr[3]), Pq(10000));
  Check("HDR10 red: red", r(hdr[4]), Pq(80 * 0.6274));
  Check("HDR10 red: green", g(hdr[4]), Pq(80 * 0.0691));
  Check("HDR10 red: blue", b(hdr[4]), Pq(80 * 0.0164));
  Check("HDR10 NaN is black", r(hdr[6]), Pq(0));
  Check("HDR10 negative is black", r(hdr[7]), Pq(0));
  const bool opaque = (hdr[1] >> 30) == 3u;
  std::printf("%s HDR10 alpha opaque\n", opaque ? "ok  " : "FAIL");
  if (!opaque) ++g_failed;

  // sRGB, the display in SDR mode: scRGB 1.0 is white, over 1.0 clips.
  std::vector<uint32_t> sdr = convert(false, 2, 1);
  Check("sRGB black", r(sdr[0]), 0);
  Check("sRGB white", g(sdr[1]), 1);
  Check("sRGB over white clips", g(sdr[3]), 1);
  Check("sRGB 0.5 grey", g(sdr[5]), Srgb(0.5));
  Check("sRGB NaN is black", r(sdr[6]), 0);

  std::printf(g_failed == 0 ? "passed\n" : "%d FAILED\n", g_failed);
  return g_failed == 0 ? 0 : 1;
}
