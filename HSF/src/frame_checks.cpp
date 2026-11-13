// See frame_checks.h.

#include "frame_checks.h"

#include "camera.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iterator>

namespace hsf::frame_checks {
namespace {

using Microsoft::WRL::ComPtr;

// The mean of 16 points in each grid cell. Points, not every pixel, because this runs on every frame and the presenter waits for it.
const char GRID_SHADER[] = R"(
Texture2D<float4> Colour : register(t0);
RWByteAddressBuffer Grid : register(u0);
cbuffer Constants : register(b0) { int2 Size; int2 Cells; };

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const int2 cell = int2(id.xy);
  if (any(cell >= Cells)) return;
  float sum = 0.0;
  for (int y = 0; y < 4; ++y)
    for (int x = 0; x < 4; ++x) {
      const float2 place = (float2(cell) + (float2(x, y) + 0.5) / 4.0) * float2(Size) / float2(Cells);
      sum += dot(saturate(Colour.Load(int3(int2(place), 0)).rgb), float3(0.299, 0.587, 0.114));
    }
  Grid.Store((cell.y * Cells.x + cell.x) * 4, asuint(sum / 16.0));
}
)";

// One point per cell, in the cell's middle. The flow is a value here, not an average: an average across an object's edge is a motion nothing has.
const char SAMPLES_SHADER[] = R"(
Texture2D<float4> Motion : register(t0);
Texture2D<float> Depth : register(t1);
RWByteAddressBuffer Grid : register(u0);
cbuffer Constants : register(b0) { int2 Size; int2 Cells; };

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const int2 cell = int2(id.xy);
  if (any(cell >= Cells)) return;
  const int2 place = int2((float2(cell) + 0.5) * float2(Size) / float2(Cells));
  const uint at = (cell.y * Cells.x + cell.x) * 16;
  const float2 motion = Motion.Load(int3(place, 0)).xy;
  Grid.Store(at, asuint(motion.x));
  Grid.Store(at + 4, asuint(motion.y));
  Grid.Store(at + 8, asuint(Depth.Load(int3(place, 0))));
  Grid.Store(at + 12, 0u);
}
)";

// CHANGED: last frame's colour here differs by this much, so this is not a pixel staying put. DETAIL: the difference to a neighbour that makes a pixel
// worth judging. BETTER: how much worse the colour at the motion's far end must match. MOVING: the flow's offset, in frame pixels, that counts as saying it moves.
// ATTACK and RELEASE are steps of 0-1 per frame. The mask starts at 0.5, so about 8 frames in, and a pixel is free about 4 frames after it starts changing.
const char MASK_SHADER[] = R"(
Texture2D<float4> Colour : register(t0);
Texture2D<float4> LastColour : register(t1);
Texture2D<float4> Motion : register(t2);
Texture2D<float> LastStill : register(t3);
RWTexture2D<float4> Output : register(u0);
RWTexture2D<float> Still : register(u1);
cbuffer Constants : register(b0) { int2 FrameSize; int2 MotionSize; float2 ToFramePixels; uint HaveLast; uint AllStill; };

static const float CHANGED = 0.03;
static const float DETAIL = 0.06;
static const float BETTER = 0.03;
static const float MOVING = 0.5;
static const float ATTACK = 1.0 / 16.0;
static const float RELEASE = 1.0 / 8.0;
static const int REACH = 2;

float Luma(Texture2D<float4> t, int2 p) { return dot(saturate(t.Load(int3(clamp(p, int2(0, 0), FrameSize - 1), 0)).rgb), float3(0.299, 0.587, 0.114)); }
float StillAt(int2 p) { return LastStill.Load(int3(clamp(p, int2(0, 0), MotionSize - 1), 0)); }

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const int2 p = int2(id.xy);
  if (any(p >= MotionSize)) return;
  const float4 motion = Motion.Load(int3(p, 0));
  float still = 0.0;
  float strongest = 0.0;
  if (HaveLast != 0) {
    still = StillAt(p);
    const int2 place = int2((float2(p) + 0.5) * float2(FrameSize) / float2(MotionSize));
    const float2 offset = motion.xy * ToFramePixels;
    const float now = Luma(Colour, place);
    const float in_place = abs(now - Luma(LastColour, place));
    if (in_place > CHANGED) {
      still = max(still - RELEASE, 0.0);
    } else if (length(offset) > MOVING) {
      const float detail = max(abs(now - Luma(Colour, place + int2(2, 0))), abs(now - Luma(Colour, place + int2(0, 2))));
      const float along = abs(now - Luma(LastColour, place + int2(floor(offset + 0.5))));
      if (detail > DETAIL && along > in_place + BETTER) still = min(still + ATTACK, 1.0);
    }
    // Reaches past thin strokes: flow has no clean edge around text. Neighbours are last frame's, a frame late at worst.
    strongest = still;
    for (int y = -1; y <= 1; ++y)
      for (int x = -1; x <= 1; ++x) strongest = max(strongest, StillAt(p + int2(x, y) * REACH));
  }
  Still[p] = still;
  const float mask = AllStill != 0 ? 1.0 : smoothstep(0.5, 1.0, strongest);
  Output[p] = float4(motion.xy * (1.0 - mask), motion.zw);
}
)";

// NOISE: smaller differences than this are not overlay (about 2 levels of 255). GAIN: a see-through panel differs little from what is behind it, and still
// has to count as overlay.
const char UI_SHADER[] = R"(
Texture2D<float4> Finished : register(t0);
Texture2D<float4> Clean : register(t1);
RWTexture2D<float> Alpha : register(u0);
cbuffer Constants : register(b0) { int2 Size; };

static const float NOISE = 0.007;
static const float GAIN = 8.0;

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const int2 p = int2(id.xy);
  if (any(p >= Size)) return;
  const float3 d = abs(Finished.Load(int3(p, 0)).rgb - Clean.Load(int3(p, 0)).rgb);
  const float difference = max(d.r, max(d.g, d.b));
  Alpha[p] = difference < NOISE ? 0.0 : saturate(difference * GAIN);
}
)";

constexpr UINT MASK_DESCRIPTORS_PER_FRAME = 6;
constexpr UINT UI_DESCRIPTORS_PER_FRAME = 3;

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b{};
  b.Transition.pResource = r;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  return b;
}

D3D12_RESOURCE_DESC BufferDesc(UINT64 size, D3D12_RESOURCE_FLAGS flags) {
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
  d.Width = size;
  d.Height = 1;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
  d.Flags = flags;
  return d;
}

// One table (SRVs then UAVs) and root constants.
HRESULT MakeRoot(ID3D12Device* device, UINT srvs, UINT uavs, UINT constants, ComPtr<ID3D12RootSignature>& root) {
  const D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, srvs, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
                                           {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, uavs, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND}};
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = static_cast<UINT>(std::size(ranges));
  params[0].DescriptorTable.pDescriptorRanges = ranges;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.Num32BitValues = constants;
  const D3D12_ROOT_SIGNATURE_DESC desc{static_cast<UINT>(std::size(params)), params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  // From the loaded d3d12.dll: the add-on must not import it, as it only loads in games that get DLSS-G.
  const auto serialize = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
  if (serialize == nullptr) return E_NOINTERFACE;
  ComPtr<ID3DBlob> code, messages;
  HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &code, &messages);
  if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&root));
  return hr;
}

HRESULT MakePipeline(ID3D12Device* device, const char* source, size_t size, const char* name, ID3D12RootSignature* root, ComPtr<ID3D12PipelineState>& pso) {
  ComPtr<ID3DBlob> code, messages;
  HRESULT hr = D3DCompile(source, size, name, nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);
  if (FAILED(hr)) return hr;
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root;
  desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
  return device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
}

}  // namespace

// ---- Grid ----
bool Grid::Init(ID3D12Device* device, DXGI_FORMAT colour_format, const char*& what, HRESULT& hr) {
  device_ = device;
  format_ = colour_format;
  what = "the compute queue";
  D3D12_COMMAND_QUEUE_DESC queue{};
  queue.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
  queue.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
  if (FAILED(hr = device->CreateCommandQueue(&queue, IID_PPV_ARGS(&queue_)))) return false;
  if (FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&alloc_)))) return false;
  if (FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, alloc_.Get(), nullptr, IID_PPV_ARGS(&list_)))) return false;
  list_->Close();
  if (FAILED(hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return false;
  event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  what = "the grid's shader";
  if (FAILED(hr = MakeRoot(device, 1, 1, 4, root_))) return false;
  if (FAILED(hr = MakePipeline(device, GRID_SHADER, sizeof(GRID_SHADER) - 1, "hsf_grid", root_.Get(), pso_))) return false;

  what = "the grid's buffers";
  const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (FAILED(hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_)))) return false;
  descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const UINT64 bytes = GRID_CELLS * sizeof(float);
  const D3D12_HEAP_PROPERTIES gpu{D3D12_HEAP_TYPE_DEFAULT}, back{D3D12_HEAP_TYPE_READBACK};
  const D3D12_RESOURCE_DESC buffer = BufferDesc(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), plain = BufferDesc(bytes, D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(hr = device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer_)))) return false;
  if (FAILED(hr = device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &plain, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_)))) return false;
  void* mapped = nullptr;
  if (FAILED(hr = readback_->Map(0, nullptr, &mapped))) return false;
  mapped_ = static_cast<const float*>(mapped);
  LARGE_INTEGER frequency{};
  QueryPerformanceFrequency(&frequency);
  ticks_per_second_ = frequency.QuadPart;
  what = nullptr;
  return true;
}

bool Grid::Run(ID3D12Resource* colour, UINT width, UINT height, Verdict& verdict) {
  // A run that timed out may still be going, and its list is about to be reset.
  if (fence_->GetCompletedValue() < value_) {
    fence_->SetEventOnCompletion(value_, event_);
    if (WaitForSingleObject(event_, 100) != WAIT_OBJECT_0) return false;
  }
  LARGE_INTEGER begin{};
  QueryPerformanceCounter(&begin);

  alloc_->Reset();
  list_->Reset(alloc_.Get(), pso_.Get());
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  srv.Format = format_;
  device_->CreateShaderResourceView(colour, &srv, cpu);
  cpu.ptr += descriptor_size_;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_R32_TYPELESS;
  uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav.Buffer.NumElements = GRID_CELLS;
  uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  device_->CreateUnorderedAccessView(buffer_.Get(), nullptr, &uav, cpu);

  ID3D12DescriptorHeap* const heaps[] = {heap_.Get()};
  list_->SetDescriptorHeaps(1, heaps);
  list_->SetComputeRootSignature(root_.Get());
  list_->SetComputeRootDescriptorTable(0, heap_->GetGPUDescriptorHandleForHeapStart());
  const UINT constants[4] = {width, height, GRID_WIDTH, GRID_HEIGHT};
  list_->SetComputeRoot32BitConstants(1, 4, constants, 0);
  const D3D12_RESOURCE_BARRIER write = Transition(buffer_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list_->ResourceBarrier(1, &write);
  list_->Dispatch((GRID_WIDTH + 7) / 8, (GRID_HEIGHT + 7) / 8, 1);
  const D3D12_RESOURCE_BARRIER read = Transition(buffer_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list_->ResourceBarrier(1, &read);
  list_->CopyResource(readback_.Get(), buffer_.Get());
  const D3D12_RESOURCE_BARRIER rest = Transition(buffer_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
  list_->ResourceBarrier(1, &rest);
  list_->Close();
  ID3D12CommandList* const lists[] = {list_.Get()};
  queue_->ExecuteCommandLists(1, lists);
  queue_->Signal(fence_.Get(), ++value_);

  bool answered = fence_->GetCompletedValue() >= value_;
  if (!answered) {
    fence_->SetEventOnCompletion(value_, event_);
    answered = WaitForSingleObject(event_, 3) == WAIT_OBJECT_0;
  }
  LARGE_INTEGER end{};
  QueryPerformanceCounter(&end);
  sum_ms_ += 1000.0 * static_cast<double>(end.QuadPart - begin.QuadPart) / static_cast<double>(ticks_per_second_);
  ++count_;
  if (!answered) return false;
  verdict = classifier_.Frame(mapped_);
  return true;
}

bool Grid::TakeAverageWaitMs(UINT every, double& ms) {
  if (count_ < every) return false;
  ms = sum_ms_ / count_;
  sum_ms_ = 0;
  count_ = 0;
  return true;
}

// ---- Mask ----
bool Mask::Init(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT motion_format, DXGI_FORMAT colour_resource_format, DXGI_FORMAT colour_format,
                UINT frame_width, UINT frame_height, UINT motion_width, UINT motion_height, const char*& what, HRESULT& hr) {
  device_ = device;
  motion_format_ = motion_format;
  colour_format_ = colour_format;
  frame_width_ = frame_width;
  frame_height_ = frame_height;
  motion_width_ = motion_width;
  motion_height_ = motion_height;

  what = "the motion vectors' format has no typed UAV store";
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{motion_format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
  if (FAILED(hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))) return false;
  if ((support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) == 0 || (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0) {
    hr = E_NOTIMPL;
    return false;
  }

  what = "the mask's shader";
  if (FAILED(hr = MakeRoot(device, 4, 2, 8, root_))) return false;
  if (FAILED(hr = MakePipeline(device, MASK_SHADER, sizeof(MASK_SHADER) - 1, "hsf_mask", root_.Get(), pso_))) return false;

  what = "the mask's textures";
  const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, MASK_DESCRIPTORS_PER_FRAME * FRAMES, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (FAILED(hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_)))) return false;
  descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const D3D12_HEAP_PROPERTIES gpu{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC texture{};
  texture.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  texture.Width = frame_width;
  texture.Height = frame_height;
  texture.DepthOrArraySize = 1;
  texture.MipLevels = 1;
  texture.SampleDesc.Count = 1;
  texture.Format = colour_resource_format;
  if (FAILED(hr = device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&last_colour_)))) return false;
  texture.Width = motion_width;
  texture.Height = motion_height;
  texture.Format = DXGI_FORMAT_R8_UNORM;
  texture.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (FAILED(hr = device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&still_[0])))) return false;
  if (FAILED(hr = device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &texture, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&still_[1])))) return false;
  current_ = 1;  // written first

  // Timing is a report only: without it the pass still runs.
  UINT64 frequency = 0;
  if (SUCCEEDED(queue->GetTimestampFrequency(&frequency)) && frequency != 0) {
    const D3D12_QUERY_HEAP_DESC query{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * FRAMES, 0};
    const D3D12_HEAP_PROPERTIES back{D3D12_HEAP_TYPE_READBACK};
    const D3D12_RESOURCE_DESC buffer = BufferDesc(2 * FRAMES * sizeof(UINT64), D3D12_RESOURCE_FLAG_NONE);
    void* mapped = nullptr;
    if (SUCCEEDED(device->CreateQueryHeap(&query, IID_PPV_ARGS(&queries_))) &&
        SUCCEEDED(device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&times_buffer_))) &&
        SUCCEEDED(times_buffer_->Map(0, nullptr, &mapped))) {
      times_ = static_cast<const UINT64*>(mapped);
      ticks_per_ms_ = static_cast<double>(frequency) / 1000.0;
    }
  }
  what = nullptr;
  return true;
}

void Mask::Record(ID3D12GraphicsCommandList* list, UINT frame, ID3D12Resource* colour, ID3D12Resource* motion, ID3D12Resource* out, const float scale[2], bool all_still) {
  // This frame's last pass has finished, so its timestamps are there.
  if (times_ != nullptr && timed_[frame]) {
    const UINT64 begin = times_[2 * frame], end = times_[2 * frame + 1];
    if (end > begin) {
      sum_ms_ += static_cast<double>(end - begin) / ticks_per_ms_;
      ++count_;
    }
  }

  ID3D12Resource* const write = still_[current_].Get();
  ID3D12Resource* const read = still_[1 - current_].Get();
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += static_cast<SIZE_T>(frame) * MASK_DESCRIPTORS_PER_FRAME * descriptor_size_;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  srv.Format = colour_format_;
  device_->CreateShaderResourceView(colour, &srv, cpu);
  cpu.ptr += descriptor_size_;
  device_->CreateShaderResourceView(last_colour_.Get(), &srv, cpu);
  cpu.ptr += descriptor_size_;
  srv.Format = motion_format_;
  device_->CreateShaderResourceView(motion, &srv, cpu);
  cpu.ptr += descriptor_size_;
  srv.Format = DXGI_FORMAT_R8_UNORM;
  device_->CreateShaderResourceView(read, &srv, cpu);
  cpu.ptr += descriptor_size_;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  uav.Format = motion_format_;
  device_->CreateUnorderedAccessView(out, nullptr, &uav, cpu);
  cpu.ptr += descriptor_size_;
  uav.Format = DXGI_FORMAT_R8_UNORM;
  device_->CreateUnorderedAccessView(write, nullptr, &uav, cpu);

  ID3D12DescriptorHeap* const heaps[] = {heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(root_.Get());
  list->SetPipelineState(pso_.Get());
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += static_cast<UINT64>(frame) * MASK_DESCRIPTORS_PER_FRAME * descriptor_size_;
  list->SetComputeRootDescriptorTable(0, gpu);
  const float to_pixels[2] = {scale[0] * static_cast<float>(frame_width_), scale[1] * static_cast<float>(frame_height_)};
  UINT constants[8] = {frame_width_, frame_height_, motion_width_, motion_height_, 0, 0, have_last_ ? 1u : 0u, all_still ? 1u : 0u};
  std::memcpy(&constants[4], to_pixels, sizeof(to_pixels));
  list->SetComputeRoot32BitConstants(1, 8, constants, 0);
  if (times_ != nullptr) list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * frame);
  list->Dispatch((motion_width_ + 7) / 8, (motion_height_ + 7) / 8, 1);
  // This frame's colour is next frame's last colour, and what was written is read next frame.
  const D3D12_RESOURCE_BARRIER before[] = {Transition(last_colour_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
                                           Transition(write, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                           Transition(read, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
  list->ResourceBarrier(static_cast<UINT>(std::size(before)), before);
  list->CopyResource(last_colour_.Get(), colour);
  const D3D12_RESOURCE_BARRIER after = Transition(last_colour_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  list->ResourceBarrier(1, &after);
  current_ = 1 - current_;
  have_last_ = true;
  if (times_ != nullptr) {
    list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * frame + 1);
    list->ResolveQueryData(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * frame, 2, times_buffer_.Get(), 2 * frame * sizeof(UINT64));
    timed_[frame] = true;
  }
}

bool Mask::TakeAverageMs(UINT every, double& ms) {
  if (count_ < every) return false;
  ms = sum_ms_ / count_;
  sum_ms_ = 0;
  count_ = 0;
  return true;
}

// ---- Samples ----
bool Samples::Init(ID3D12Device* device, DXGI_FORMAT motion_format, const char*& what, HRESULT& hr) {
  device_ = device;
  format_ = motion_format;
  what = "the camera grid's queue";
  D3D12_COMMAND_QUEUE_DESC queue{};
  queue.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
  queue.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
  if (FAILED(hr = device->CreateCommandQueue(&queue, IID_PPV_ARGS(&queue_)))) return false;
  if (FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&alloc_)))) return false;
  if (FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, alloc_.Get(), nullptr, IID_PPV_ARGS(&list_)))) return false;
  list_->Close();
  if (FAILED(hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return false;
  event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);

  what = "the camera grid's shader";
  if (FAILED(hr = MakeRoot(device, 2, 1, 4, root_))) return false;
  if (FAILED(hr = MakePipeline(device, SAMPLES_SHADER, sizeof(SAMPLES_SHADER) - 1, "hsf_samples", root_.Get(), pso_))) return false;

  what = "the camera grid's buffers";
  const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 3, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (FAILED(hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_)))) return false;
  descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const UINT64 bytes = camera::SAMPLES * 4 * sizeof(float);
  const D3D12_HEAP_PROPERTIES gpu{D3D12_HEAP_TYPE_DEFAULT}, back{D3D12_HEAP_TYPE_READBACK};
  const D3D12_RESOURCE_DESC buffer = BufferDesc(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), plain = BufferDesc(bytes, D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(hr = device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&buffer_)))) return false;
  if (FAILED(hr = device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &plain, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_)))) return false;
  void* mapped = nullptr;
  if (FAILED(hr = readback_->Map(0, nullptr, &mapped))) return false;
  mapped_ = static_cast<const float*>(mapped);
  what = nullptr;
  return true;
}

bool Samples::Run(ID3D12Resource* motion, ID3D12Resource* depth, UINT width, UINT height, const float*& grid) {
  // A run that timed out may still be going, and its list is about to be reset.
  if (fence_->GetCompletedValue() < value_) {
    fence_->SetEventOnCompletion(value_, event_);
    if (WaitForSingleObject(event_, 100) != WAIT_OBJECT_0) return false;
  }
  alloc_->Reset();
  list_->Reset(alloc_.Get(), pso_.Get());
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  srv.Format = format_;
  device_->CreateShaderResourceView(motion, &srv, cpu);
  cpu.ptr += descriptor_size_;
  srv.Format = DXGI_FORMAT_R32_FLOAT;
  device_->CreateShaderResourceView(depth, &srv, cpu);
  cpu.ptr += descriptor_size_;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_R32_TYPELESS;
  uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav.Buffer.NumElements = camera::SAMPLES * 4;
  uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  device_->CreateUnorderedAccessView(buffer_.Get(), nullptr, &uav, cpu);

  ID3D12DescriptorHeap* const heaps[] = {heap_.Get()};
  list_->SetDescriptorHeaps(1, heaps);
  list_->SetComputeRootSignature(root_.Get());
  list_->SetComputeRootDescriptorTable(0, heap_->GetGPUDescriptorHandleForHeapStart());
  const UINT constants[4] = {width, height, camera::SAMPLE_WIDTH, camera::SAMPLE_HEIGHT};
  list_->SetComputeRoot32BitConstants(1, 4, constants, 0);
  const D3D12_RESOURCE_BARRIER write = Transition(buffer_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list_->ResourceBarrier(1, &write);
  list_->Dispatch((camera::SAMPLE_WIDTH + 7) / 8, (camera::SAMPLE_HEIGHT + 7) / 8, 1);
  const D3D12_RESOURCE_BARRIER read = Transition(buffer_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list_->ResourceBarrier(1, &read);
  list_->CopyResource(readback_.Get(), buffer_.Get());
  const D3D12_RESOURCE_BARRIER rest = Transition(buffer_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
  list_->ResourceBarrier(1, &rest);
  list_->Close();
  ID3D12CommandList* const lists[] = {list_.Get()};
  queue_->ExecuteCommandLists(1, lists);
  queue_->Signal(fence_.Get(), ++value_);
  if (fence_->GetCompletedValue() < value_) {
    fence_->SetEventOnCompletion(value_, event_);
    if (WaitForSingleObject(event_, 3) != WAIT_OBJECT_0) return false;
  }
  grid = mapped_;
  return true;
}

// ---- UiAlpha ----
bool UiAlpha::Init(ID3D12Device* device, DXGI_FORMAT colour_format, UINT width, UINT height, const char*& what, HRESULT& hr) {
  device_ = device;
  format_ = colour_format;
  width_ = width;
  height_ = height;
  what = "the UI alpha shader";
  if (FAILED(hr = MakeRoot(device, 2, 1, 2, root_))) return false;
  if (FAILED(hr = MakePipeline(device, UI_SHADER, sizeof(UI_SHADER) - 1, "hsf_ui_alpha", root_.Get(), pso_))) return false;
  const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, UI_DESCRIPTORS_PER_FRAME * FRAMES, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (FAILED(hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_)))) return false;
  descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  what = nullptr;
  return true;
}

void UiAlpha::Record(ID3D12GraphicsCommandList* list, UINT frame, ID3D12Resource* finished, ID3D12Resource* clean, ID3D12Resource* alpha) {
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += static_cast<SIZE_T>(frame) * UI_DESCRIPTORS_PER_FRAME * descriptor_size_;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  srv.Format = format_;
  device_->CreateShaderResourceView(finished, &srv, cpu);
  cpu.ptr += descriptor_size_;
  device_->CreateShaderResourceView(clean, &srv, cpu);
  cpu.ptr += descriptor_size_;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  uav.Format = DXGI_FORMAT_R8_UNORM;
  device_->CreateUnorderedAccessView(alpha, nullptr, &uav, cpu);

  ID3D12DescriptorHeap* const heaps[] = {heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(root_.Get());
  list->SetPipelineState(pso_.Get());
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += static_cast<UINT64>(frame) * UI_DESCRIPTORS_PER_FRAME * descriptor_size_;
  list->SetComputeRootDescriptorTable(0, gpu);
  const UINT constants[2] = {width_, height_};
  list->SetComputeRoot32BitConstants(1, 2, constants, 0);
  list->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
}

// ---- GameChecks ----
namespace {

// score.cpp's measure: nine small samples around each patch's centre, on real pixels, the earlier frame warped along the vectors and not. The vectors may be smaller than the frame.
const char GAME_SCORE_SHADER[] = R"(
Texture2D<float4> Before : register(t0);
Texture2D<float4> After : register(t1);
Texture2D<float4> Motion : register(t2);
RWByteAddressBuffer Cells : register(u0);
cbuffer Constants : register(b0) { float2 ToPixels; int2 Frame; int2 MotionSize; int2 Grid; };

float Luma(float3 c) { return dot(saturate(c), float3(0.299, 0.587, 0.114)); }
float LumaAt(Texture2D<float4> t, int2 p) { return Luma(t.Load(int3(clamp(p, int2(0, 0), Frame - 1), 0)).rgb); }
float LumaLinear(Texture2D<float4> t, float2 p) {
  p -= 0.5;
  const float2 base = floor(p);
  const float2 f = p - base;
  const int2 i = int2(base);
  return lerp(lerp(LumaAt(t, i), LumaAt(t, i + int2(1, 0)), f.x), lerp(LumaAt(t, i + int2(0, 1)), LumaAt(t, i + int2(1, 1)), f.x), f.y);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const int2 cell = int2(id.xy);
  if (any(cell >= Grid)) return;
  const float2 q = floor((float2(cell) + 0.5) * float2(Frame) / float2(Grid)) + 0.5;
  float2 v = Motion.Load(int3(int2(q * float2(MotionSize) / float2(Frame)), 0)).xy * ToPixels;
  if (!all(abs(v) < 1e5)) v = float2(0, 0);
  float warp = 0.0, none = 0.0;
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
      const float2 p = q + float2(x, y) * 2.0;
      const float later = LumaAt(After, int2(p));
      warp += abs(later - LumaLinear(Before, p + v));
      none += abs(later - LumaAt(Before, int2(p)));
    }
  const uint at = (cell.y * Grid.x + cell.x) * 16;
  Cells.Store(at, asuint(warp / 9.0));
  Cells.Store(at + 4, asuint(none / 9.0));
  Cells.Store(at + 8, asuint(LumaAt(After, int2(q))));
  Cells.Store(at + 12, asuint(LumaAt(Before, int2(q))));
}
)";

// The same thresholds as score.cpp.
constexpr UINT SCORE_GRID_WIDTH = 240;
constexpr float SCORE_MOVING_ERROR = 2.f / 255.f, SCORE_CLEARLY = 0.1f, SCORE_STILL_SHARE = 0.01f, SCORE_CUT_HISTOGRAM = 0.35f, SCORE_CUT_GAIN = 0.2f;
constexpr int SCORE_BINS = 32;

DXGI_FORMAT ReadableFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    default: return format;
  }
}

HRESULT MakeCopyTexture(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format, ComPtr<ID3D12Resource>& out) {
  const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = width;
  d.Height = height;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.Format = format;
  d.SampleDesc.Count = 1;
  return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&out));
}

struct HeldLock {
  explicit HeldLock(SRWLOCK& lock) : lock_(lock) { AcquireSRWLockExclusive(&lock_); }
  ~HeldLock() { ReleaseSRWLockExclusive(&lock_); }
  SRWLOCK& lock_;
};

void SetSrv(ID3D12Device* device, ID3D12Resource* resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE at) {
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  srv.Format = ReadableFormat(format);
  device->CreateShaderResourceView(resource, &srv, at);
}

void SetRawUav(ID3D12Device* device, ID3D12Resource* buffer, UINT elements, D3D12_CPU_DESCRIPTOR_HANDLE at) {
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = DXGI_FORMAT_R32_TYPELESS;
  uav.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  uav.Buffer.NumElements = elements;
  uav.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  device->CreateUnorderedAccessView(buffer, nullptr, &uav, at);
}

}  // namespace

GameChecks::GameChecks() = default;

GameChecks::~GameChecks() {
  if (fence_ == nullptr || fence_->GetCompletedValue() >= value_) return;
  const HANDLE done = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (done == nullptr) return;
  fence_->SetEventOnCompletion(value_, done);
  WaitForSingleObject(done, 100);
  CloseHandle(done);
}

void GameChecks::Fail(const char* what, HRESULT hr) {
  failed_ = true;
  std::snprintf(problem_, sizeof(problem_), "%s failed, 0x%08lx", what, static_cast<unsigned long>(hr));
}

bool GameChecks::Init(ID3D12CommandQueue* queue) {
  ComPtr<ID3D12Device> device;
  HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&device));
  if (FAILED(hr)) { Fail("asking the queue for its device", hr); return false; }
  const D3D12_COMMAND_QUEUE_DESC queue_desc = queue->GetDesc();
  if (FAILED(hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) { Fail("the fence", hr); return false; }
  if (FAILED(hr = MakeRoot(device.Get(), 1, 1, 4, grid_root_))) { Fail("the grid's root", hr); return false; }
  if (FAILED(hr = MakePipeline(device.Get(), GRID_SHADER, sizeof(GRID_SHADER) - 1, "hsf_game_grid", grid_root_.Get(), grid_pso_))) { Fail("the grid's shader", hr); return false; }
  if (FAILED(hr = MakeRoot(device.Get(), 3, 1, 8, score_root_))) { Fail("the score's root", hr); return false; }
  if (FAILED(hr = MakePipeline(device.Get(), GAME_SCORE_SHADER, sizeof(GAME_SCORE_SHADER) - 1, "hsf_game_score", score_root_.Get(), score_pso_))) { Fail("the score's shader", hr); return false; }
  const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, RING * DESCRIPTORS, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (FAILED(hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_)))) { Fail("the descriptors", hr); return false; }
  descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  const D3D12_HEAP_PROPERTIES gpu{D3D12_HEAP_TYPE_DEFAULT}, back{D3D12_HEAP_TYPE_READBACK};
  const UINT64 grid_bytes = GRID_CELLS * sizeof(float);
  const D3D12_RESOURCE_DESC writable = BufferDesc(grid_bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), plain = BufferDesc(grid_bytes, D3D12_RESOURCE_FLAG_NONE);
  for (Slot& slot : slots_) {
    if (FAILED(hr = device->CreateCommandAllocator(queue_desc.Type, IID_PPV_ARGS(&slot.alloc)))) { Fail("a command allocator", hr); return false; }
    if (FAILED(hr = device->CreateCommandList(0, queue_desc.Type, slot.alloc.Get(), nullptr, IID_PPV_ARGS(&slot.list)))) { Fail("a command list", hr); return false; }
    slot.list->Close();
    if (FAILED(hr = device->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &writable, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&slot.grid)))) { Fail("a grid buffer", hr); return false; }
    if (FAILED(hr = device->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &plain, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&slot.grid_back)))) { Fail("a grid readback", hr); return false; }
    void* mapped = nullptr;
    if (FAILED(hr = slot.grid_back->Map(0, nullptr, &mapped))) { Fail("mapping a grid readback", hr); return false; }
    slot.grid_mapped = static_cast<const float*>(mapped);
  }
  device_ = device;  // last: a null device_ is what says Init has not run
  return true;
}

bool GameChecks::MakeFrames(const D3D12_RESOURCE_DESC& desc) {
  before_.Reset();
  after_.Reset();
  cells_.Reset();
  cells_back_.Reset();
  cells_mapped_ = nullptr;
  const UINT width = static_cast<UINT>(desc.Width), height = desc.Height;
  HRESULT hr = MakeCopyTexture(device_.Get(), width, height, desc.Format, before_);
  if (SUCCEEDED(hr)) hr = MakeCopyTexture(device_.Get(), width, height, desc.Format, after_);
  if (FAILED(hr)) { Fail("the score's frame copies", hr); return false; }
  grid_width_ = (std::min)(SCORE_GRID_WIDTH, width);
  grid_height_ = (std::max)(1u, static_cast<UINT>(static_cast<UINT64>(grid_width_) * height / width));
  const UINT64 bytes = static_cast<UINT64>(grid_width_) * grid_height_ * 16;
  const D3D12_HEAP_PROPERTIES gpu{D3D12_HEAP_TYPE_DEFAULT}, back{D3D12_HEAP_TYPE_READBACK};
  const D3D12_RESOURCE_DESC writable = BufferDesc(bytes, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), plain = BufferDesc(bytes, D3D12_RESOURCE_FLAG_NONE);
  hr = device_->CreateCommittedResource(&gpu, D3D12_HEAP_FLAG_NONE, &writable, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&cells_));
  if (SUCCEEDED(hr)) hr = device_->CreateCommittedResource(&back, D3D12_HEAP_FLAG_NONE, &plain, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&cells_back_));
  void* mapped = nullptr;
  if (SUCCEEDED(hr)) hr = cells_back_->Map(0, nullptr, &mapped);
  if (FAILED(hr)) { Fail("the score's buffers", hr); return false; }
  cells_mapped_ = static_cast<const float*>(mapped);
  width_ = width;
  height_ = height;
  frame_format_ = desc.Format;
  stage_ = Stage::wait;
  return true;
}

bool GameChecks::MakeMotion() {
  motion_.Reset();
  const bool same = want_motion_format_ == motion_format_;
  const UINT width = (std::max)(want_motion_width_, same ? motion_capacity_width_ : 0u), height = (std::max)(want_motion_height_, same ? motion_capacity_height_ : 0u);
  const HRESULT hr = MakeCopyTexture(device_.Get(), width, height, want_motion_format_, motion_);
  if (FAILED(hr)) { Fail("the score's copy of the vectors", hr); return false; }
  motion_format_ = want_motion_format_;
  motion_capacity_width_ = width;
  motion_capacity_height_ = height;
  want_motion_format_ = DXGI_FORMAT_UNKNOWN;
  return true;
}

void GameChecks::NoteVectors(ID3D12GraphicsCommandList* list, ID3D12Resource* motion, UINT state, const UINT extent[4], const float scale[2]) {
  HeldLock lock(lock_);
  if (failed_ || stage_ != Stage::before_taken || vectors_taken_ || list == nullptr || motion == nullptr) return;
  const D3D12_RESOURCE_DESC desc = motion->GetDesc();
  if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1) return;
  const UINT right = extent[0] + extent[2], bottom = extent[1] + extent[3];
  if (extent[2] == 0 || extent[3] == 0 || right > desc.Width || bottom > desc.Height) return;
  // Never makes a texture on this thread: the present thread does, and this pair goes without.
  if (motion_ == nullptr || motion_format_ != desc.Format || motion_capacity_width_ < desc.Width || motion_capacity_height_ < desc.Height) {
    want_motion_format_ = desc.Format;
    want_motion_width_ = static_cast<UINT>(desc.Width);
    want_motion_height_ = desc.Height;
    return;
  }
  const D3D12_RESOURCE_STATES from = static_cast<D3D12_RESOURCE_STATES>(state);
  const bool move = (from & D3D12_RESOURCE_STATE_COPY_SOURCE) == 0;
  if (move) {
    const D3D12_RESOURCE_BARRIER b = Transition(motion, from, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->ResourceBarrier(1, &b);
  }
  D3D12_TEXTURE_COPY_LOCATION to{motion_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}}, source{motion, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
  const D3D12_BOX box{extent[0], extent[1], 0, right, bottom, 1};
  list->CopyTextureRegion(&to, 0, 0, 0, &source, &box);
  if (move) {
    const D3D12_RESOURCE_BARRIER b = Transition(motion, D3D12_RESOURCE_STATE_COPY_SOURCE, from);
    list->ResourceBarrier(1, &b);
  }
  motion_width_ = extent[2];
  motion_height_ = extent[3];
  scale_[0] = scale[0];
  scale_[1] = scale[1];
  vectors_taken_ = true;
}

void GameChecks::Drain(GridFn on_grid, ScoreFn on_score) {
  const UINT64 completed = fence_->GetCompletedValue();
  for (UINT i = 0; i < RING; ++i) {
    Slot& slot = slots_[(next_ + i) % RING];  // oldest first
    if (!slot.in_use || slot.value > completed) continue;
    slot.in_use = false;
    const Verdict verdict = classifier_.Frame(slot.grid_mapped);
    if (on_grid != nullptr) on_grid(verdict);
    if (!slot.has_score) continue;
    slot.has_score = false;
    stage_ = Stage::wait;
    next_pair_ = GetTickCount64() + period_ms_;
    double warp = 0, none = 0;
    UINT moving = 0, better = 0, worse = 0;
    UINT after[SCORE_BINS] = {}, before[SCORE_BINS] = {};
    const UINT cells = grid_width_ * grid_height_;
    for (UINT c = 0; c < cells; ++c) {
      const float* const cell = cells_mapped_ + c * 4;
      warp += cell[0];
      none += cell[1];
      ++after[(std::min)(static_cast<int>(cell[2] * SCORE_BINS), SCORE_BINS - 1)];
      ++before[(std::min)(static_cast<int>(cell[3] * SCORE_BINS), SCORE_BINS - 1)];
      if (cell[1] >= SCORE_MOVING_ERROR) {
        ++moving;
        if (cell[0] < cell[1] * (1.f - SCORE_CLEARLY)) ++better;
        if (cell[0] > cell[1] * (1.f + SCORE_CLEARLY)) ++worse;
      }
    }
    ScoreSample sample;
    sample.warp = static_cast<float>(warp / cells);
    sample.none = static_cast<float>(none / cells);
    sample.moving = static_cast<float>(moving) / static_cast<float>(cells);
    sample.better = moving != 0 ? static_cast<float>(better) / static_cast<float>(moving) : 0.f;
    sample.worse = moving != 0 ? static_cast<float>(worse) / static_cast<float>(moving) : 0.f;
    const float gain = sample.none > 0.f ? 1.f - sample.warp / sample.none : 0.f;
    double difference = 0;
    for (int bin = 0; bin < SCORE_BINS; ++bin) difference += std::fabs(static_cast<double>(after[bin]) - before[bin]);
    const float histogram_change = static_cast<float>(difference / 2.0 / cells);
    if (sample.moving < SCORE_STILL_SHARE) sample.kind = 1;
    else if (histogram_change > SCORE_CUT_HISTOGRAM && gain < SCORE_CUT_GAIN) sample.kind = 2;
    if (on_score != nullptr) on_score(sample);
  }
}

bool GameChecks::AtPresent(ID3D12CommandQueue* queue, ID3D12Resource* back_buffer, GridFn on_grid, ScoreFn on_score) {
  HeldLock lock(lock_);
  if (failed_) return false;
  if (queue == nullptr || back_buffer == nullptr) return true;
  if (device_ == nullptr && !Init(queue)) return false;
  Drain(on_grid, on_score);
  const D3D12_RESOURCE_DESC desc = back_buffer->GetDesc();
  if (desc.SampleDesc.Count != 1) return true;
  const bool idle = fence_->GetCompletedValue() >= value_;
  // Textures in use by the GPU stay until it is done with them. Nothing here ever waits for it: this frame goes without.
  if (before_ == nullptr || static_cast<UINT>(desc.Width) != width_ || desc.Height != height_ || desc.Format != frame_format_) {
    if (!idle) return true;
    if (!MakeFrames(desc)) return false;
  }
  if (want_motion_format_ != DXGI_FORMAT_UNKNOWN && stage_ == Stage::wait && idle && !MakeMotion()) return false;

  Slot& slot = slots_[next_];
  if (slot.in_use) return true;
  slot.alloc->Reset();
  ID3D12GraphicsCommandList* const list = slot.list.Get();
  list->Reset(slot.alloc.Get(), grid_pso_.Get());
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
  cpu.ptr += static_cast<SIZE_T>(next_) * DESCRIPTORS * descriptor_size_;
  gpu.ptr += static_cast<UINT64>(next_) * DESCRIPTORS * descriptor_size_;

  const D3D12_RESOURCE_STATES READ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_COPY_SOURCE;
  const D3D12_RESOURCE_BARRIER to_read = Transition(back_buffer, D3D12_RESOURCE_STATE_PRESENT, READ);
  list->ResourceBarrier(1, &to_read);

  // The grid, every frame.
  SetSrv(device_.Get(), back_buffer, desc.Format, cpu);
  D3D12_CPU_DESCRIPTOR_HANDLE next_cpu = cpu;
  next_cpu.ptr += descriptor_size_;
  SetRawUav(device_.Get(), slot.grid.Get(), GRID_CELLS, next_cpu);
  ID3D12DescriptorHeap* const heaps[] = {heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(grid_root_.Get());
  list->SetComputeRootDescriptorTable(0, gpu);
  const UINT grid_constants[4] = {width_, height_, GRID_WIDTH, GRID_HEIGHT};
  list->SetComputeRoot32BitConstants(1, 4, grid_constants, 0);
  const D3D12_RESOURCE_BARRIER grid_write = Transition(slot.grid.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  list->ResourceBarrier(1, &grid_write);
  list->Dispatch((GRID_WIDTH + 7) / 8, (GRID_HEIGHT + 7) / 8, 1);
  const D3D12_RESOURCE_BARRIER grid_read = Transition(slot.grid.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  list->ResourceBarrier(1, &grid_read);
  list->CopyResource(slot.grid_back.Get(), slot.grid.Get());
  const D3D12_RESOURCE_BARRIER grid_rest = Transition(slot.grid.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
  list->ResourceBarrier(1, &grid_rest);

  // The score: this frame after the one copied last present, with the vectors the game sent in between.
  if (stage_ == Stage::before_taken) {
    if (!vectors_taken_ || (scale_[0] == 0.f && scale_[1] == 0.f)) {
      stage_ = Stage::wait;  // no vectors this frame: a menu, or the copy was not ready yet
    } else {
      list->CopyResource(after_.Get(), back_buffer);
      const D3D12_RESOURCE_BARRIER in[] = {Transition(before_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                           Transition(after_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                           Transition(motion_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                           Transition(cells_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
      list->ResourceBarrier(static_cast<UINT>(std::size(in)), in);
      D3D12_CPU_DESCRIPTOR_HANDLE at = cpu;
      at.ptr += 2ull * descriptor_size_;
      SetSrv(device_.Get(), before_.Get(), frame_format_, at);
      at.ptr += descriptor_size_;
      SetSrv(device_.Get(), after_.Get(), frame_format_, at);
      at.ptr += descriptor_size_;
      SetSrv(device_.Get(), motion_.Get(), motion_format_, at);
      at.ptr += descriptor_size_;
      SetRawUav(device_.Get(), cells_.Get(), grid_width_ * grid_height_ * 4, at);
      list->SetPipelineState(score_pso_.Get());
      list->SetComputeRootSignature(score_root_.Get());
      D3D12_GPU_DESCRIPTOR_HANDLE score_gpu = gpu;
      score_gpu.ptr += 2ull * descriptor_size_;
      list->SetComputeRootDescriptorTable(0, score_gpu);
      const float to_pixels[2] = {scale_[0] * static_cast<float>(width_), scale_[1] * static_cast<float>(height_)};
      list->SetComputeRoot32BitConstants(1, 2, to_pixels, 0);
      const UINT sizes[6] = {width_, height_, motion_width_, motion_height_, grid_width_, grid_height_};
      list->SetComputeRoot32BitConstants(1, 6, sizes, 2);
      list->Dispatch((grid_width_ + 7) / 8, (grid_height_ + 7) / 8, 1);
      const D3D12_RESOURCE_BARRIER out[] = {Transition(before_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
                                            Transition(after_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
                                            Transition(motion_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
                                            Transition(cells_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)};
      list->ResourceBarrier(static_cast<UINT>(std::size(out)), out);
      list->CopyResource(cells_back_.Get(), cells_.Get());
      const D3D12_RESOURCE_BARRIER cells_rest = Transition(cells_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
      list->ResourceBarrier(1, &cells_rest);
      slot.has_score = true;
      stage_ = Stage::reading;
    }
  } else if (stage_ == Stage::wait && GetTickCount64() >= next_pair_) {
    list->CopyResource(before_.Get(), back_buffer);
    vectors_taken_ = false;
    stage_ = Stage::before_taken;
  }

  const D3D12_RESOURCE_BARRIER to_present = Transition(back_buffer, READ, D3D12_RESOURCE_STATE_PRESENT);
  list->ResourceBarrier(1, &to_present);
  HRESULT hr = list->Close();
  if (FAILED(hr)) { Fail("closing the list", hr); return false; }
  ID3D12CommandList* const lists[] = {list};
  queue->ExecuteCommandLists(1, lists);
  hr = queue->Signal(fence_.Get(), ++value_);
  if (FAILED(hr)) { Fail("signalling the fence", hr); return false; }
  slot.value = value_;
  slot.in_use = true;
  next_ = (next_ + 1) % RING;
  return true;
}

}  // namespace hsf::frame_checks
