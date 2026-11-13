// Offline test of NVIDIA Optical Flow as a motion source (nvof_flow.h), no game needed: a textured picture that moves by a known number of pixels
// each frame goes in, and the motion that comes out must say so, in direction and size. Needs an NVIDIA RTX card.
//   nvof_test.exe [size] [copy]     size: the grey image's height (default 360, 0 = the picture's own); copy: the picture is copied first, as a back buffer is
#include "../src/nvof_flow.h"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT W = 1920, H = 1080;
constexpr int FRAMES = 40;

float Hash(int x, int y) {
  uint32_t h = static_cast<uint32_t>(x) * 374761393u + static_cast<uint32_t>(y) * 668265263u;
  h = (h ^ (h >> 13)) * 1274126177u;
  return static_cast<float>((h ^ (h >> 16)) & 0xffff) / 65535.f;
}

// Smooth noise with cells of `cell` pixels.
float Noise(float x, float y, float cell) {
  const float fx = x / cell, fy = y / cell;
  const int ix = static_cast<int>(std::floor(fx)), iy = static_cast<int>(std::floor(fy));
  const float tx = fx - ix, ty = fy - iy;
  const float a = Hash(ix, iy), b = Hash(ix + 1, iy), c = Hash(ix, iy + 1), d = Hash(ix + 1, iy + 1);
  return (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
}

void Fill(std::vector<uint8_t>& pixels, float shift_x, float shift_y) {
  for (UINT y = 0; y < H; ++y)
    for (UINT x = 0; x < W; ++x) {
      const float px = static_cast<float>(x) - shift_x, py = static_cast<float>(y) - shift_y;
      const float v = 0.5f * Noise(px, py, 96.f) + 0.3f * Noise(px, py, 28.f) + 0.2f * Noise(px, py, 9.f);
      const uint8_t g = static_cast<uint8_t>(std::clamp(v, 0.f, 1.f) * 255.f);
      uint8_t* const p = &pixels[(static_cast<size_t>(y) * W + x) * 4];
      p[0] = p[1] = p[2] = g;
      p[3] = 255;
    }
}

void Line(const char* text) { std::printf("  log: %s\n", text); }

double NowMs() {
  LARGE_INTEGER now, frequency;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&frequency);
  return static_cast<double>(now.QuadPart) * 1000.0 / static_cast<double>(frequency.QuadPart);
}

}  // namespace

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  UINT size = hsf::nvof::DEFAULT_SIZE;
  bool copy = false;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "copy") == 0) copy = true;
    else size = static_cast<UINT>(std::atoi(argv[i]));
  }
  std::printf("nvof_test: picture %ux%u, grey image height %u%s\n", W, H, size, copy ? ", picture copied first" : "");

  ComPtr<IDXGIFactory6> factory;
  ComPtr<IDXGIAdapter1> adapter;
  ComPtr<ID3D12Device> device;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || FAILED(factory->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter))) ||
      FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
    std::printf("FAIL: no Direct3D 12 device\n");
    return 1;
  }
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> alloc;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
  device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
  device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
  list->Close();
  device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
  const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  UINT64 value = 0;
  const auto flush = [&] {
    queue->Signal(fence.Get(), ++value);
    fence->SetEventOnCompletion(value, event);
    return WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
  };

  const D3D12_HEAP_PROPERTIES default_heap{D3D12_HEAP_TYPE_DEFAULT}, upload_heap{D3D12_HEAP_TYPE_UPLOAD}, readback_heap{D3D12_HEAP_TYPE_READBACK};
  D3D12_RESOURCE_DESC colour_desc{};
  colour_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  colour_desc.Width = W;
  colour_desc.Height = H;
  colour_desc.DepthOrArraySize = 1;
  colour_desc.MipLevels = 1;
  colour_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  colour_desc.SampleDesc.Count = 1;
  ComPtr<ID3D12Resource> colour, upload, readback;
  device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE, &colour_desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&colour));
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT colour_print{}, motion_print{};
  UINT64 upload_bytes = 0, readback_bytes = 0;
  device->GetCopyableFootprints(&colour_desc, 0, 1, 0, &colour_print, nullptr, nullptr, &upload_bytes);
  D3D12_RESOURCE_DESC motion_desc = colour_desc;
  motion_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  device->GetCopyableFootprints(&motion_desc, 0, 1, 0, &motion_print, nullptr, nullptr, &readback_bytes);
  D3D12_RESOURCE_DESC buffer{};
  buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buffer.Width = upload_bytes;
  buffer.Height = 1;
  buffer.DepthOrArraySize = 1;
  buffer.MipLevels = 1;
  buffer.SampleDesc.Count = 1;
  buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
  buffer.Width = readback_bytes;
  device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
  if (!colour || !upload || !readback) {
    std::printf("FAIL: the test's textures\n");
    return 1;
  }

  // The picture moves right and down: a pixel was further left and up a frame ago, so its motion must be minus the shift.
  const float shift[2] = {7.f, -4.f};
  std::vector<uint8_t> pixels(static_cast<size_t>(W) * H * 4);
  hsf::nvof::Flow flow;
  hsf::nvof::Frame frame;
  frame.colour = colour.Get();
  frame.colour_view = DXGI_FORMAT_R8G8B8A8_UNORM;
  frame.colour_state = D3D12_RESOURCE_STATE_COMMON;
  frame.copy_colour = copy;
  frame.width = W;
  frame.height = H;
  frame.format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  frame.scale[0] = 1.f / W;  // a texel is pixels of the picture
  frame.scale[1] = 1.f / H;
  frame.size = size;
  bool pass = true;
  int with_flow = 0;
  double ms_sum = 0, ms_worst = 0, worst_error = 0;
  for (int k = 0; k < FRAMES && pass; ++k) {
    Fill(pixels, shift[0] * k, shift[1] * k);
    void* mapped = nullptr;
    upload->Map(0, nullptr, &mapped);
    for (UINT y = 0; y < H; ++y) std::memcpy(static_cast<uint8_t*>(mapped) + colour_print.Offset + static_cast<size_t>(y) * colour_print.Footprint.RowPitch, &pixels[static_cast<size_t>(y) * W * 4], static_cast<size_t>(W) * 4);
    upload->Unmap(0, nullptr);
    alloc->Reset();
    list->Reset(alloc.Get(), nullptr);
    D3D12_TEXTURE_COPY_LOCATION to{colour.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX}, from{upload.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    from.PlacedFootprint = colour_print;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    list->Close();
    ID3D12CommandList* const upload_lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, upload_lists);
    if (!flush()) {
      std::printf("FAIL: the GPU did not finish the upload\n");
      return 1;
    }

    alloc->Reset();
    list->Reset(alloc.Get(), nullptr);
    const double before = NowMs();
    ID3D12Resource* const motion = flow.Run(device.Get(), queue.Get(), list.Get(), frame, &Line);
    if (motion != nullptr) {
      D3D12_RESOURCE_BARRIER b{};
      b.Transition.pResource = motion;
      b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      b.Transition.StateBefore = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
      b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
      list->ResourceBarrier(1, &b);
      D3D12_TEXTURE_COPY_LOCATION out{readback.Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT}, in{motion, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      out.PlacedFootprint = motion_print;
      list->CopyTextureRegion(&out, 0, 0, 0, &in, nullptr);
      std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
      list->ResourceBarrier(1, &b);
    }
    list->Close();
    ID3D12CommandList* const lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (!flush()) {
      std::printf("FAIL: the GPU did not finish frame %d within 5 s\n", k);
      return 1;
    }
    const double ms = NowMs() - before;
    if (motion == nullptr) {
      if (k > 0) {
        std::printf("FAIL: no flow on frame %d (%s)\n", k, flow.Status());
        pass = false;
      }
      continue;
    }
    ++with_flow;
    if (k >= 2) {  // the first call compiles and warms up
      ms_sum += ms;
      ms_worst = std::max(ms_worst, ms);
    }
    // The middle of the picture: the edges have nowhere to have come from.
    const D3D12_RANGE all{0, static_cast<SIZE_T>(readback_bytes)};
    readback->Map(0, &all, &mapped);
    std::vector<float> xs, ys;
    for (UINT y = H / 4; y < 3 * H / 4; y += 8)
      for (UINT x = W / 4; x < 3 * W / 4; x += 8) {
        const float* const t = reinterpret_cast<const float*>(static_cast<const uint8_t*>(mapped) + motion_print.Offset + static_cast<size_t>(y) * motion_print.Footprint.RowPitch) + static_cast<size_t>(x) * 4;
        xs.push_back(t[0]);
        ys.push_back(t[1]);
      }
    readback->Unmap(0, nullptr);
    std::nth_element(xs.begin(), xs.begin() + xs.size() / 2, xs.end());
    std::nth_element(ys.begin(), ys.begin() + ys.size() / 2, ys.end());
    const float mx = xs[xs.size() / 2], my = ys[ys.size() / 2];
    const double error = std::max(std::fabs(mx + shift[0]), std::fabs(my + shift[1]));
    worst_error = std::max(worst_error, error);
    if (k < 3 || k == FRAMES - 1) std::printf("  frame %d: median motion %.2f, %.2f pixels (want %.2f, %.2f); %.2f ms from call to GPU done\n", k, mx, my, -shift[0], -shift[1], ms);
  }
  // Half a pixel of the grey image is the best a block match can promise.
  const double allowed = std::max(0.5, 0.5 * static_cast<double>(H) / static_cast<double>(size == 0 ? H : size) / 2.0);
  if (pass && with_flow != FRAMES - 1) {
    std::printf("FAIL: flow on %d of %d frames\n", with_flow, FRAMES - 1);
    pass = false;
  }
  if (pass && worst_error > allowed) {
    std::printf("FAIL: worst error %.2f pixels, allowed %.2f\n", worst_error, allowed);
    pass = false;
  }
  std::printf("engine: %s; worst error %.2f pixels (allowed %.2f); call to GPU done: average %.2f ms, worst %.2f\n", flow.Status(), worst_error, allowed, with_flow > 1 ? ms_sum / std::max(1, with_flow - 1) : 0.0, ms_worst);
  flow.Shutdown();
  std::printf(pass ? "PASS\n" : "FAIL\n");
  return pass ? 0 : 1;
}
