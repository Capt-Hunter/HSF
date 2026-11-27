// Read back generated FP16 pixels using the production Apex provider host.
#include "../src/apex_fg.h"
#include "../src/hsf_log.h"
#include "../src/sl_bring.h"

#include <DirectXPackedVector.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdio>

namespace {

using Microsoft::WRL::ComPtr;
constexpr auto READ = D3D12_RESOURCE_STATE_ALL_SHADER_RESOURCE;

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
  list->ResourceBarrier(1, &barrier);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  using namespace hsf;
  if (argc != 3 && argc != 4) {
    std::puts("usage: apex_pixels_test <provider DLL> <log file> [motion|motion-zero-depth|motion-dilated|motion-4k]");
    return 2;
  }
  const bool motion_test = argc == 4;
  const bool zero_depth = motion_test && wcscmp(argv[3], L"motion-zero-depth") == 0;
  const bool dilated = motion_test && wcscmp(argv[3], L"motion-dilated") == 0;
  const bool full_resolution = motion_test && wcscmp(argv[3], L"motion-4k") == 0;
  const UINT WIDTH = full_resolution ? 3840 : 640, HEIGHT = full_resolution ? 2160 : 480;
  hsflog::Open(argv[2]);
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> alloc;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  ComPtr<ID3D12DescriptorHeap> rtv;
  const D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT};
  const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3};
  if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device))) ||
      FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))) ||
      FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
      FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))) ||
      FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))) ||
      FAILED(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv)))) return 2;

  D3D12_RESOURCE_DESC image{};
  image.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  image.Width = WIDTH;
  image.Height = HEIGHT;
  image.DepthOrArraySize = 1;
  image.MipLevels = 1;
  image.SampleDesc.Count = 1;
  image.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  constexpr DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_R16G16_FLOAT, DXGI_FORMAT_R32_FLOAT};
  constexpr D3D12_RESOURCE_STATES states[] = {D3D12_RESOURCE_STATE_PRESENT, READ, READ};
  const D3D12_HEAP_PROPERTIES gpu_heap{D3D12_HEAP_TYPE_DEFAULT}, cpu_heap{D3D12_HEAP_TYPE_READBACK};
  std::array<ComPtr<ID3D12Resource>, 3> textures;
  const auto first = rtv->GetCPUDescriptorHandleForHeapStart();
  const UINT increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  for (size_t i = 0; i < textures.size(); ++i) {
    image.Format = formats[i];
    if (FAILED(device->CreateCommittedResource(&gpu_heap, D3D12_HEAP_FLAG_NONE, &image, states[i], nullptr,
                                               IID_PPV_ARGS(&textures[i])))) return 2;
    device->CreateRenderTargetView(textures[i].Get(), nullptr, {first.ptr + increment * i});
  }
  image.Format = formats[0];
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 bytes = 0;
  device->GetCopyableFootprints(&image, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
  D3D12_RESOURCE_DESC buf{};
  buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  buf.Width = bytes;
  buf.Height = 1;
  buf.DepthOrArraySize = 1;
  buf.MipLevels = 1;
  buf.SampleDesc.Count = 1;
  buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> readback;
  if (FAILED(device->CreateCommittedResource(&cpu_heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST,
                                             nullptr, IID_PPV_ARGS(&readback)))) return 2;
  UINT64 value = 0;
  const auto flush = [&] {
    if (FAILED(list->Close())) return false;
    ID3D12CommandList* const lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    const bool completed = event && SUCCEEDED(queue->Signal(fence.Get(), ++value)) &&
                           SUCCEEDED(fence->SetEventOnCompletion(value, event)) && WaitForSingleObject(event, 10000) == WAIT_OBJECT_0;
    if (event) CloseHandle(event);
    return completed && SUCCEEDED(alloc->Reset()) && SUCCEEDED(list->Reset(alloc.Get(), nullptr));
  };
  apex::Provider provider;
  apex::Session session;
  slbring::FrameFeed feed;
  feed.device = device.Get();
  feed.queue = queue.Get();
  feed.back_buffer = textures[0].Get();
  feed.motion = textures[1].Get();
  feed.depth = textures[2].Get();
  feed.scale[0] = feed.scale[1] = 1.0f;
  feed.colour_space = DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
  if (!provider.Init(argv[1], device.Get(), true) || !session.Init(provider, feed, WIDTH, HEIGHT)) return 1;
  constexpr float colours[3][4] = {{-0.125f, 0.5f, 3.0f, 1.0f}, {0.f, 0.f, 0.f, 0.f}, {0.5f, 0.f, 0.f, 0.f}};
  if (motion_test) {
    feed.depth_inverted = zero_depth;
    ComPtr<IDXGIFactory2> factory;
    ComPtr<IDXGISwapChain1> chain;
    ComPtr<IDXGISwapChain3> swap;
    ComPtr<ID3D12Resource> second;
    const HWND window = CreateWindowExW(0, L"STATIC", L"Apex motion readback", WS_OVERLAPPED, 0, 0, WIDTH, HEIGHT,
                                       nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = WIDTH;
    desc.Height = HEIGHT;
    desc.Format = formats[0];
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    if (!window || FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateSwapChainForHwnd(queue.Get(), window, &desc, nullptr, nullptr, &chain)) ||
        FAILED(chain.As(&swap)) || FAILED(swap->GetBuffer(swap->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&second)))) return 2;
    const auto center = [&](ID3D12Resource* image) {
      Transition(list.Get(), image, states[0], D3D12_RESOURCE_STATE_COPY_SOURCE);
      D3D12_TEXTURE_COPY_LOCATION dst{};
      dst.pResource = readback.Get();
      dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      dst.PlacedFootprint = footprint;
      const D3D12_TEXTURE_COPY_LOCATION src{image, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
      list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
      Transition(list.Get(), image, D3D12_RESOURCE_STATE_COPY_SOURCE, states[0]);
      if (!flush()) return -1.0;
      void* mapped = nullptr;
      const D3D12_RANGE range{0, static_cast<SIZE_T>(bytes)}, no_writes{};
      if (FAILED(readback->Map(0, &range, &mapped))) return -1.0;
      double sum = 0.0, weighted = 0.0;
      for (unsigned y = 160; y < 320; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const char*>(mapped) + y * footprint.Footprint.RowPitch);
        for (unsigned x = 0; x < WIDTH; ++x) {
          const float value = DirectX::PackedVector::XMConvertHalfToFloat(row[4 * x]);
          if (value > 0.10f) { sum += value; weighted += value * x; }
        }
      }
      readback->Unmap(0, &no_writes);
      return sum > 0.0 ? weighted / sum : -1.0;
    };
    const LONG step = full_resolution ? 8 : 24;
    constexpr LONG left = 128, object_width = 160;
    constexpr float background[] = {0.002f, 0.002f, 0.002f, 1.0f}, object[] = {0.8f, 0.6f, 0.4f, 1.0f};
    const float motion[] = {-static_cast<float>(step) / WIDTH, 0.f, 0.f, 0.f};
    const double tolerance = full_resolution ? 2.0 : 4.0;
    bool ordered = true;
    for (unsigned count : {1u, 2u}) {
      for (LONG frame = 0; frame < 6; ++frame) {
        const LONG x = left + frame * step;
        const D3D12_RECT bounds{x, 96, x + object_width, 384};
        for (size_t i = 0; i < textures.size(); ++i) {
          Transition(list.Get(), textures[i].Get(), states[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
          const D3D12_CPU_DESCRIPTOR_HANDLE handle{first.ptr + increment * i};
          list->ClearRenderTargetView(handle, i == 0 ? background : i == 1 ? motion : zero_depth ? colours[1] : colours[2], 0, nullptr);
          if (i == 0) list->ClearRenderTargetView(handle, object, 1, &bounds);
          Transition(list.Get(), textures[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, states[i]);
        }
        const auto result = session.Record(list.Get(), feed, feed.motion, READ, feed.depth, dilated || zero_depth, frame == 0, count);
        if (!flush() || result != (frame == 0 ? apex::FrameResult::real_only : apex::FrameResult::generated)) return 1;
        if (frame == 0) continue;
        if (!session.BeginPresent(feed.back_buffer) || (count == 2 && FAILED(session.CopySecondGenerated(swap.Get())))) return 1;
        session.EndPresent();
        const double first_x = center(feed.back_buffer);
        const double second_x = count == 2 ? center(second.Get()) : 0.0;
        const double real = x + (object_width - 1) / 2.0, prev = real - step;
        const double first_expected = prev + static_cast<double>(step) / (count + 1);
        const double second_expected = prev + 2.0 * step / (count + 1);
        std::printf("%ux %ld: real %.2f -> %.2f, generated %.2f (expected %.2f)", count + 1, frame, prev, real, first_x, first_expected);
        if (count == 2) std::printf(", %.2f (expected %.2f)", second_x, second_expected);
        std::putchar('\n');
        ordered = ordered && std::abs(first_x - first_expected) < tolerance &&
                  (count == 1 || (first_x < second_x && std::abs(second_x - second_expected) < tolerance));
      }
    }
    return ordered && device->GetDeviceRemovedReason() == S_OK ? 0 : 1;
  }
  for (unsigned frame = 0; frame < 3; ++frame) {
    for (size_t i = 0; i < textures.size(); ++i) {
      Transition(list.Get(), textures[i].Get(), states[i], D3D12_RESOURCE_STATE_RENDER_TARGET);
      list->ClearRenderTargetView({first.ptr + increment * i}, colours[i], 0, nullptr);
      Transition(list.Get(), textures[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, states[i]);
    }
    const auto result = session.Record(list.Get(), feed, feed.motion, READ, feed.depth, false, frame == 0);
    const auto expected = frame == 0 ? apex::FrameResult::real_only : apex::FrameResult::generated;
    if (!flush() || result != expected) return 1;
  }
  if (!session.BeginPresent(feed.back_buffer)) return 1;
  session.EndPresent();
  Transition(list.Get(), feed.back_buffer, states[0], D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION dst{};
  dst.pResource = readback.Get();
  dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  dst.PlacedFootprint = footprint;
  const D3D12_TEXTURE_COPY_LOCATION source{feed.back_buffer, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
  list->CopyTextureRegion(&dst, 0, 0, 0, &source, nullptr);
  if (!flush()) return 1;
  void* mapped = nullptr;
  const D3D12_RANGE read_range{0, static_cast<SIZE_T>(bytes)}, no_writes{};
  if (FAILED(readback->Map(0, &read_range, &mapped))) return 1;
  const auto* pixel = reinterpret_cast<const uint16_t*>(static_cast<const char*>(mapped) + 240 * footprint.Footprint.RowPitch + 320 * 8);
  const float rgb[] = {DirectX::PackedVector::XMConvertHalfToFloat(pixel[0]), DirectX::PackedVector::XMConvertHalfToFloat(pixel[1]),
                       DirectX::PackedVector::XMConvertHalfToFloat(pixel[2])};
  readback->Unmap(0, &no_writes);
  std::printf("generated FP16 scRGB sample %.5f %.5f %.5f from %.5f %.5f %.5f\n",
              rgb[0], rgb[1], rgb[2], colours[0][0], colours[0][1], colours[0][2]);
  const bool preserved = std::abs(rgb[0] - colours[0][0]) < 0.035f && std::abs(rgb[1] - colours[0][1]) < 0.035f &&
                         std::abs(rgb[2] - colours[0][2]) < 0.075f;
  return preserved && device->GetDeviceRemovedReason() == S_OK ? 0 : 1;
}
