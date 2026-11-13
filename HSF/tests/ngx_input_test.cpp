// The DLSS hook on the bench: hooks the real _nvngx.dll of this machine's driver, and takes a stand-in DLSS call on a real device, reading back what was copied.
// Nothing here renders DLSS: the real call is made once with nothing set up, only to see it comes back through the hook. Run: ngx_input_test
#include <windows.h>
#include <d3d12.h>
#include <wrl/client.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include "../src/ngx_input.h"

using Microsoft::WRL::ComPtr;

namespace {
int g_failures = 0;
void Check(bool ok, const char* what) {
  printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) ++g_failures;
}
void Print(const char* text) { printf("     %s\n", text); }

// The stand-in for NGX's block. Each name asked for that is set answers success, the rest fail, as NGX's does.
struct Block : hsf::ngx::Parameter {
  std::map<std::string, float> floats;
  std::map<std::string, unsigned> uints;
  std::map<std::string, ID3D12Resource*> resources;
  void Set(const char*, unsigned long long) override {}
  void Set(const char*, float) override {}
  void Set(const char*, double) override {}
  void Set(const char*, unsigned int) override {}
  void Set(const char*, int) override {}
  void Set(const char*, ID3D11Resource*) override {}
  void Set(const char*, ID3D12Resource*) override {}
  void Set(const char*, void*) override {}
  int Get(const char*, unsigned long long*) const override { return 0; }
  int Get(const char* name, float* value) const override {
    const auto it = floats.find(name);
    if (it == floats.end()) return 0;
    *value = it->second;
    return 1;
  }
  int Get(const char*, double*) const override { return 0; }
  int Get(const char* name, unsigned int* value) const override {
    const auto it = uints.find(name);
    if (it == uints.end()) return 0;
    *value = it->second;
    return 1;
  }
  int Get(const char* name, int* value) const override {
    const auto it = uints.find(name);
    if (it == uints.end()) return 0;
    *value = static_cast<int>(it->second);
    return 1;
  }
  int Get(const char*, ID3D11Resource**) const override { return 0; }
  int Get(const char* name, ID3D12Resource** value) const override {
    const auto it = resources.find(name);
    if (it == resources.end()) return 0;
    *value = it->second;
    return 1;
  }
  int Get(const char*, void**) const override { return 0; }
  void Reset() override {}
};

struct Gpu {
  ComPtr<ID3D12Device> device;
  ComPtr<ID3D12CommandQueue> queue;
  ComPtr<ID3D12CommandAllocator> allocator;
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  UINT64 value = 0;
  void Run() {
    list->Close();
    ID3D12CommandList* const lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence.Get(), ++value);
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence->SetEventOnCompletion(value, event);
    WaitForSingleObject(event, 5000);
    CloseHandle(event);
    allocator->Reset();
    list->Reset(allocator.Get(), nullptr);
  }
};

void BarrierOf(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier.Transition.StateBefore = from;
  barrier.Transition.StateAfter = to;
  list->ResourceBarrier(1, &barrier);
}

ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, D3D12_HEAP_TYPE type, UINT64 bytes, D3D12_RESOURCE_STATES state) {
  const D3D12_HEAP_PROPERTIES heap{type};
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  desc.Width = bytes;
  desc.Height = 1;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  ComPtr<ID3D12Resource> buffer;
  device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&buffer));
  return buffer;
}

// The first texel of a texture in the readable state.
void ReadFirst(Gpu& gpu, ID3D12Resource* texture, void* out, size_t bytes) {
  const D3D12_RESOURCE_DESC desc = texture->GetDesc();
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
  UINT64 total = 0;
  gpu.device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &total);
  ComPtr<ID3D12Resource> readback = Buffer(gpu.device.Get(), D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST);
  const D3D12_RESOURCE_STATES readable = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  BarrierOf(gpu.list.Get(), texture, readable, D3D12_RESOURCE_STATE_COPY_SOURCE);
  D3D12_TEXTURE_COPY_LOCATION to{}, from{};
  to.pResource = readback.Get();
  to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
  to.PlacedFootprint = footprint;
  from.pResource = texture;
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  gpu.list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  BarrierOf(gpu.list.Get(), texture, D3D12_RESOURCE_STATE_COPY_SOURCE, readable);
  gpu.Run();
  void* mapped = nullptr;
  readback->Map(0, nullptr, &mapped);
  std::memcpy(out, mapped, bytes);
  readback->Unmap(0, nullptr);
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-5f; }
}  // namespace

int main() {
  hsf::ngx::Start(&Print);

  // 1. The real DLL of this driver: does the hook accept how its calls start, and does a call come back through it?
  wchar_t system[MAX_PATH] = L"";
  GetSystemDirectoryW(system, MAX_PATH);
  HMODULE ngx = LoadLibraryW(L"_nvngx.dll");
  if (ngx == nullptr) {
    WIN32_FIND_DATAW found;
    const std::wstring store = std::wstring(system) + L"\\DriverStore\\FileRepository\\nv_dispi.inf_amd64_*";
    const HANDLE search = FindFirstFileW(store.c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
      do {
        const std::wstring path = std::wstring(system) + L"\\DriverStore\\FileRepository\\" + found.cFileName + L"\\_nvngx.dll";
        ngx = LoadLibraryW(path.c_str());
      } while (ngx == nullptr && FindNextFileW(search, &found));
      FindClose(search);
    }
  }
  Check(ngx != nullptr, "this driver's _nvngx.dll loads");
  hsf::ngx::Poll();
  Check(hsf::ngx::HookedCalls() >= 2, "the DLSS calls of the real DLL are hooked");
  if (ngx != nullptr) {
    using Evaluate = int(__cdecl*)(void*, const void*, const void*, void*);
    const auto evaluate = reinterpret_cast<Evaluate>(GetProcAddress(ngx, "NVSDK_NGX_D3D12_EvaluateFeature"));
    if (evaluate != nullptr) {
      const int result = evaluate(nullptr, nullptr, nullptr, nullptr);
      printf("     the real call with nothing set up says 0x%08x\n", result);
      Check(result != 1, "the real call runs through the hook and refuses, with no capture");
    }
  }

  // 2. A stand-in call, with depth that has stencil as Unreal's has.
  Gpu gpu;
  ComPtr<ID3D12Debug> debug;
  if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) debug->EnableDebugLayer();
  Check(SUCCEEDED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&gpu.device))), "device");
  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  gpu.device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&gpu.queue));
  gpu.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&gpu.allocator));
  gpu.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, gpu.allocator.Get(), nullptr, IID_PPV_ARGS(&gpu.list));
  gpu.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.fence));

  const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC tex{};
  tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  tex.Width = 800;
  tex.Height = 450;
  tex.DepthOrArraySize = 1;
  tex.MipLevels = 1;
  tex.SampleDesc.Count = 1;
  tex.Format = DXGI_FORMAT_R16G16_FLOAT;
  tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  const float motion_value[4] = {0.5f, -0.25f, 0, 0};
  D3D12_CLEAR_VALUE motion_clear{};
  motion_clear.Format = tex.Format;
  std::memcpy(motion_clear.Color, motion_value, sizeof(motion_value));
  ComPtr<ID3D12Resource> motion, depth;
  Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_RENDER_TARGET, &motion_clear, IID_PPV_ARGS(&motion))), "game's vectors");
  tex.Format = DXGI_FORMAT_R32G8X24_TYPELESS;
  tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
  D3D12_CLEAR_VALUE depth_clear{};
  depth_clear.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  depth_clear.DepthStencil.Depth = 0.75f;
  Check(SUCCEEDED(gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_DEPTH_WRITE, &depth_clear, IID_PPV_ARGS(&depth))), "game's depth, with stencil");

  D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
  rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_heap_desc.NumDescriptors = 1;
  D3D12_DESCRIPTOR_HEAP_DESC dsv_heap_desc{};
  dsv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  dsv_heap_desc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> rtv_heap, dsv_heap;
  gpu.device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap));
  gpu.device->CreateDescriptorHeap(&dsv_heap_desc, IID_PPV_ARGS(&dsv_heap));
  gpu.device->CreateRenderTargetView(motion.Get(), nullptr, rtv_heap->GetCPUDescriptorHandleForHeapStart());
  D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
  dsv.Format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
  dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  gpu.device->CreateDepthStencilView(depth.Get(), &dsv, dsv_heap->GetCPUDescriptorHandleForHeapStart());
  gpu.list->ClearRenderTargetView(rtv_heap->GetCPUDescriptorHandleForHeapStart(), motion_value, 0, nullptr);
  gpu.list->ClearDepthStencilView(dsv_heap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 0.75f, 0, 0, nullptr);
  BarrierOf(gpu.list.Get(), motion.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  BarrierOf(gpu.list.Get(), depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

  Block block;
  block.resources["MotionVectors"] = motion.Get();
  block.resources["Depth"] = depth.Get();
  block.floats["MV.Scale.X"] = 640.f;
  block.floats["MV.Scale.Y"] = -360.f;
  block.floats["Jitter.Offset.X"] = 0.25f;
  block.floats["Jitter.Offset.Y"] = -0.125f;
  block.uints["DLSS.Feature.Create.Flags"] = (1 << 1) | (1 << 3);  // low-resolution vectors, reversed depth
  block.uints["DLSS.Render.Subrect.Dimensions.Width"] = 640;
  block.uints["DLSS.Render.Subrect.Dimensions.Height"] = 360;
  block.uints["Reset"] = 1;

  hsf::ngx::Frame frame;
  Check(!hsf::ngx::Latest(&frame), "nothing before the first call");
  hsf::ngx::Capture(gpu.list.Get(), &block);
  gpu.Run();
  Check(hsf::ngx::Latest(&frame), "the call is taken");
  Check(frame.width == 640 && frame.height == 360, "the part in use is the render size, not the texture");
  Check(Near(frame.scale[0], 1.f) && Near(frame.scale[1], -1.f), "scale: NGX's over the render size");
  Check(Near(frame.jitter[0], 0.25f) && Near(frame.jitter[1], -0.125f), "jitter");
  Check(frame.depth_inverted && !frame.jittered && !frame.full_res, "flags read");
  Check(frame.reset, "reset read");
  if (frame.motion != nullptr && frame.depth != nullptr) {
    // R16G16_FLOAT: 0.5 and -0.25 as 16-bit floats.
    uint16_t read_motion[2] = {};
    ReadFirst(gpu, frame.motion, read_motion, sizeof(read_motion));
    printf("     copied vectors %04x %04x\n", read_motion[0], read_motion[1]);
    Check(read_motion[0] == 0x3800 && read_motion[1] == 0xB400, "the vectors are copied");
    float read_depth = 0.f;
    ReadFirst(gpu, frame.depth, &read_depth, sizeof(read_depth));
    printf("     copied depth %.3f\n", read_depth);
    Check(Near(read_depth, 0.75f), "depth is copied out from beside its stencil");
    Check(frame.depth->GetDesc().Format == DXGI_FORMAT_R32_FLOAT, "depth is R32_FLOAT");
    Check(frame.motion->GetDesc().Width == frame.depth->GetDesc().Width, "both copies are one size");
  }

  // A small call after the main one, as for a mirror, must not take over.
  block.uints["DLSS.Render.Subrect.Dimensions.Width"] = 100;
  block.uints["DLSS.Render.Subrect.Dimensions.Height"] = 100;
  const uint64_t before = frame.serial;
  hsf::ngx::Capture(gpu.list.Get(), &block);
  gpu.Run();
  hsf::ngx::Latest(&frame);
  Check(frame.serial == before && frame.width == 640, "a small second call does not take over");

  // Depth that cannot be copied leaves the last good frame and no broken one.
  tex.Format = DXGI_FORMAT_R24G8_TYPELESS;
  ComPtr<ID3D12Resource> d24;
  D3D12_CLEAR_VALUE d24_clear{};
  d24_clear.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
  gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_DEPTH_WRITE, &d24_clear, IID_PPV_ARGS(&d24));
  block.resources["Depth"] = d24.Get();
  block.uints["DLSS.Render.Subrect.Dimensions.Width"] = 640;
  block.uints["DLSS.Render.Subrect.Dimensions.Height"] = 360;
  hsf::ngx::Capture(gpu.list.Get(), &block);
  gpu.Run();
  hsf::ngx::Latest(&frame);
  Check(frame.serial == before, "depth in a format that cannot be copied is refused");

  Check(gpu.device->GetDeviceRemovedReason() == S_OK, "the device is fine");
  hsf::ngx::Stop(false);
  printf(g_failures == 0 ? "all passed\n" : "%d FAILED\n", g_failures);
  return g_failures == 0 ? 0 : 1;
}
