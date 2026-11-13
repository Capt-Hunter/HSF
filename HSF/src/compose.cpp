// See compose.h.

#include "compose.h"

#include <d3dcompiler.h>

#include <cstring>
#include <iterator>

namespace hsf::compose {
namespace {

using Microsoft::WRL::ComPtr;

// Closeness is 1 - depth, or depth when reversed. Both grow like 1 / distance, so the relative difference of two closeness values is about the
// relative difference of their distances, whatever the scene's scale. SIGMA: 5% apart counts for little. STRIDE and RADIUS reach 6 pixels, which is
// how far optical flow tends to spill; 25 samples of motion, 49 of depth.
// HISTORY_WEIGHT: the most last frame's motion can count for. AGREE_PIXELS + AGREE_SHARE x speed: how far apart, in motion texture pixels, the two motions
// may be before last frame's stops counting. Flow's shake is about a pixel, more when fast. Together these limit the lag: motion that changes steadily by
// d pixels a frame trails by about 1.5 d, and a change far beyond the limit does not trail at all.
// The camera: its motion is exact for what does not move, so a pixel takes it, with no averaging over time to lag it, where the flow says about the same. Where they differ
// either the flow is wrong or the thing moves by itself, and last frame's picture settles which: the pixel takes the camera's only if that finds a clearly better match there.
// Smooth skin matches either way and keeps the flow: a character on screen moves with the camera, and taking the world's motion broke Lara's face in Tomb Raider.
// CAMERA_PIXELS + AGREE_SHARE x speed: about the same. CLEARLY: better by about 4 levels of 255, on average over 5 points.
const char SHADER[] = R"(
Texture2D<float4> Motion : register(t0);
Texture2D<float> Depth : register(t1);
Texture2D<float4> LastMotion : register(t2);
Texture2D<float> LastDepth : register(t3);
Texture2D<float4> Colour : register(t4);
Texture2D<float4> LastColour : register(t5);
RWTexture2D<float4> Output : register(u0);
cbuffer Constants : register(b0) {
  int2 Size; uint Inverted; uint UseHistory;
  float2 ToPixels; uint UseCamera; uint MotionSize;
  int2 FrameSize; float2 Scale;
  float4 Row0; float4 Row1; float4 Row2; float4 Row3;
};

static const float SIGMA = 0.05;
static const int STRIDE = 3;
static const int RADIUS = 2;
static const float HISTORY_WEIGHT = 0.6;
static const float AGREE_PIXELS = 1.0;
static const float AGREE_SHARE = 0.1;
static const float CAMERA_PIXELS = 0.5;
static const float CLEARLY = 0.015;

float Closeness(int2 p) {
  const float d = Depth.Load(int3(clamp(p, int2(0, 0), Size - 1), 0));
  return Inverted != 0 ? d : 1.0 - d;
}

// The flow may be smaller than the depth (LumeniteFX Kernel's is 1/8 size): MotionSize is its width | height << 16, 0 when it is the depth's size. The pass then
// works at the depth's size and takes each flow texel for the pixels it covers; the depth-aware average below smooths the blocks along surfaces.
float4 MotionAt(int2 p) {
  p = clamp(p, int2(0, 0), Size - 1);
  if (MotionSize != 0) {
    const int2 m = int2(MotionSize & 0xffff, MotionSize >> 16);
    p = clamp(int2((float2(p) + 0.5) * float2(m) / float2(Size)), int2(0, 0), m - 1);
  }
  return Motion.Load(int3(p, 0));
}

// Near 1 for two points on one surface, near 0 for two surfaces.
float Alike(float a, float b) {
  const float apart = abs(a - b) / max(max(a, b), 0.0001) / SIGMA;
  return exp(-apart * apart);
}

int At(int x, int y) { return (y + RADIUS + 1) * (2 * RADIUS + 3) + x + RADIUS + 1; }

float Luma(Texture2D<float4> t, int2 p) { return dot(saturate(t.Load(int3(clamp(p, int2(0, 0), FrameSize - 1), 0)).rgb), float3(0.299, 0.587, 0.114)); }

// How badly this motion lines the pixel up with last frame's picture.
float Mismatch(int2 place, float2 motion) {
  const int2 offset = int2(floor(motion * Scale * float2(FrameSize) + 0.5));
  static const int2 POINTS[5] = {int2(0, 0), int2(2, 0), int2(-2, 0), int2(0, 2), int2(0, -2)};
  float sum = 0.0;
  for (int i = 0; i < 5; ++i) sum += abs(Luma(Colour, place + POINTS[i]) - Luma(LastColour, place + POINTS[i] + offset));
  return sum / 5.0;
}

// The camera's motion at a pixel, in motion texture units. False if it was behind the camera last frame.
bool CameraMotion(int2 q, out float2 motion) {
  motion = float2(0.0, 0.0);
  const float2 uv = (float2(q) + 0.5) / float2(Size);
  const float4 clip = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, Depth.Load(int3(q, 0)), 1.0);
  const float w = dot(Row3, clip);
  if (w < 0.000001) return false;
  const float2 last_uv = float2(dot(Row0, clip) / w * 0.5 + 0.5, 0.5 - dot(Row1, clip) / w * 0.5);
  motion = (last_uv - uv) / Scale;
  return true;
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const int2 p = int2(id.xy);
  if (any(p >= Size)) return;
  // The nearest surface in the 3x3.
  int2 nearest = p;
  float surface = Closeness(p);
  for (int y = -1; y <= 1; ++y)
    for (int x = -1; x <= 1; ++x) {
      const float c = Closeness(p + int2(x, y));
      if (c > surface) {
        surface = c;
        nearest = p + int2(x, y);
      }
    }
  // Closeness on the sample grid, with one more ring: a sample's neighbours say whether it sits on a depth edge.
  float grid[(2 * RADIUS + 3) * (2 * RADIUS + 3)];
  for (int y = -RADIUS - 1; y <= RADIUS + 1; ++y)
    for (int x = -RADIUS - 1; x <= RADIUS + 1; ++x) grid[At(x, y)] = Closeness(p + int2(x, y) * STRIDE);
  // Motion around the pixel from samples on that surface, away from edges, because flow is wrong at edges: a sample left there would carry the spill
  // further out. The surface's own sample counts a little, so the sum is never zero and a thin object with no clean sample keeps its own motion.
  float2 sum = 0.1 * MotionAt(nearest).xy;
  float total = 0.1;
  for (int y = -RADIUS; y <= RADIUS; ++y)
    for (int x = -RADIUS; x <= RADIUS; ++x) {
      const float c = grid[At(x, y)];
      const float spatial = exp(-0.5 * float(x * x + y * y) / float(RADIUS * RADIUS));
      const float w = Alike(c, surface) * Alike(c, grid[At(x - 1, y)]) * Alike(c, grid[At(x + 1, y)]) * Alike(c, grid[At(x, y - 1)]) * Alike(c, grid[At(x, y + 1)]) * spatial;
      sum += w * MotionAt(p + int2(x, y) * STRIDE).xy;
      total += w;
    }
  float2 result = sum / total;
  // How much of the flow to keep over the camera's motion: none where they agree.
  float2 camera_motion = float2(0.0, 0.0);
  float keep_flow = 1.0;
  if (UseCamera != 0 && CameraMotion(clamp(nearest, int2(0, 0), Size - 1), camera_motion)) {
    const float apart = length((camera_motion - result) * ToPixels) / (CAMERA_PIXELS + AGREE_SHARE * length(camera_motion * ToPixels));
    if (apart < 1.0) {
      keep_flow = 0.0;
    } else {
      const int2 place = int2((float2(p) + 0.5) * float2(FrameSize) / float2(Size));
      keep_flow = 1.0 - saturate((Mismatch(place, result) - Mismatch(place, camera_motion) - CLEARLY) / CLEARLY);
    }
  }
  // Where this pixel was last frame, and what it was doing there. The nearest surface in that 3x3, as above, so an edge compares like with like.
  const float2 offset = result * ToPixels;
  const int2 last = int2(floor(float2(p) + offset + 0.5));
  if (keep_flow > 0.0 && UseHistory != 0 && all(last >= int2(0, 0)) && all(last < Size)) {
    float last_surface = 0.0;
    for (int y = -1; y <= 1; ++y)
      for (int x = -1; x <= 1; ++x) {
        const float d = LastDepth.Load(int3(clamp(last + int2(x, y), int2(0, 0), Size - 1), 0));
        last_surface = max(last_surface, Inverted != 0 ? d : 1.0 - d);
      }
    const float2 history = LastMotion.Load(int3(last, 0)).xy;
    const float apart = length((history - result) * ToPixels) / (AGREE_PIXELS + AGREE_SHARE * length(offset));
    result = lerp(result, history, HISTORY_WEIGHT * Alike(last_surface, surface) * exp(-apart * apart));
  }
  Output[p] = float4(lerp(camera_motion, result, keep_flow), MotionAt(p).zw);
}
)";

// Above 1920x1200 the pass works at a smaller size (WorkDivisor, compose.h), as its cost grows with the pixel count: 8 to 11 ms at 4K in FFXV. DOWN
// first takes each block's nearest depth, the pass runs on that, and UP brings its motion back to full size by depth: each pixel averages its four
// nearest small results, weighing each by how close its depth is to the pixel's own, so an edge keeps the motion of its own side. A pixel whose four all
// lie on other surfaces takes the closest one's.

const char DOWN_SHADER[] = R"(
Texture2D<float> Depth : register(t1);
RWTexture2D<float> Output : register(u0);
cbuffer Constants : register(b0) {
  int2 Size; uint Inverted; uint Unused;
  int2 FullSize;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const int2 p = int2(id.xy);
  if (any(p >= Size)) return;
  const int2 from = p * FullSize / Size;
  const int2 to = min(max(from + 1, (p + 1) * FullSize / Size), FullSize);
  float nearest = Depth.Load(int3(from, 0));
  for (int y = from.y; y < to.y; ++y)
    for (int x = from.x; x < to.x; ++x) {
      const float d = Depth.Load(int3(x, y, 0));
      nearest = Inverted != 0 ? max(nearest, d) : min(nearest, d);
    }
  Output[p] = nearest;
}
)";

const char UP_SHADER[] = R"(
Texture2D<float> Depth : register(t1);
Texture2D<float> HalfDepth : register(t2);
Texture2D<float4> HalfMotion : register(t3);
RWTexture2D<float4> Output : register(u0);
cbuffer Constants : register(b0) {
  int2 Size; uint Inverted; uint Unused;
  int2 FullSize;
};

static const float SIGMA = 0.05;

float Closeness(float d) { return Inverted != 0 ? d : 1.0 - d; }

float Alike(float a, float b) {
  const float apart = abs(a - b) / max(max(a, b), 0.0001) / SIGMA;
  return exp(-apart * apart);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  const int2 p = int2(id.xy);
  if (any(p >= FullSize)) return;
  const float2 h = (float2(p) + 0.5) * float2(Size) / float2(FullSize) - 0.5;
  const int2 base = int2(floor(h));
  const float2 f = h - float2(base);
  const float surface = Closeness(Depth.Load(int3(p, 0)));
  float4 sum = float4(0.0, 0.0, 0.0, 0.0);
  float total = 0.0;
  float4 closest = float4(0.0, 0.0, 0.0, 0.0);
  float closest_apart = 1e30;
  for (int j = 0; j <= 1; ++j)
    for (int i = 0; i <= 1; ++i) {
      const int2 q = clamp(base + int2(i, j), int2(0, 0), Size - 1);
      const float c = Closeness(HalfDepth.Load(int3(q, 0)));
      const float4 m = HalfMotion.Load(int3(q, 0));
      const float w = (i != 0 ? f.x : 1.0 - f.x) * (j != 0 ? f.y : 1.0 - f.y) * Alike(c, surface);
      sum += w * m;
      total += w;
      if (abs(c - surface) < closest_apart) {
        closest_apart = abs(c - surface);
        closest = m;
      }
    }
  Output[p] = total > 0.001 ? sum / total : closest;
}
)";

// Per frame: the pass's table, then DOWN's and UP's (each 6 shader resources and 1 output, like the root signature).
constexpr UINT TABLE = 7;
constexpr UINT DESCRIPTORS_PER_FRAME = 3 * TABLE;
constexpr UINT ROOT_CONSTANTS = 28;

}  // namespace

bool Pass::Init(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT motion_format, UINT width, UINT height, const char*& what, HRESULT& hr) {
  hr = S_OK;
  device_ = device;
  format_ = motion_format;

  what = "the motion vectors' format has no typed UAV store";
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{motion_format, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
  if (FAILED(hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))) return false;
  if ((support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) == 0 || (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0) {
    hr = E_NOTIMPL;
    return false;
  }

  const UINT divisor = WorkDivisor(width, height);
  reduced_ = divisor > 1;
  work_width_ = (width + divisor - 1) / divisor;
  work_height_ = (height + divisor - 1) / divisor;

  what = "compiling the shader";
  ComPtr<ID3DBlob> code, down_code, up_code, messages;
  if (FAILED(hr = D3DCompile(SHADER, sizeof(SHADER) - 1, "hsf_compose", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages))) return false;
  if (reduced_) {
    what = "compiling the small size shaders";
    if (FAILED(hr = D3DCompile(DOWN_SHADER, sizeof(DOWN_SHADER) - 1, "hsf_compose_down", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &down_code, &messages))) return false;
    if (FAILED(hr = D3DCompile(UP_SHADER, sizeof(UP_SHADER) - 1, "hsf_compose_up", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &up_code, &messages))) return false;
  }

  what = "the root signature";
  const D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 6, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
                                           {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND}};
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = static_cast<UINT>(std::size(ranges));
  params[0].DescriptorTable.pDescriptorRanges = ranges;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.Num32BitValues = ROOT_CONSTANTS;
  const D3D12_ROOT_SIGNATURE_DESC root_desc{static_cast<UINT>(std::size(params)), params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  // From the loaded d3d12.dll: the add-on must not import it, as it only loads in games that get DLSS-G.
  const auto serialize = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
  ComPtr<ID3DBlob> root_code;
  if (serialize == nullptr) hr = E_NOINTERFACE;
  else hr = serialize(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &root_code, &messages);
  if (FAILED(hr) || FAILED(hr = device->CreateRootSignature(0, root_code->GetBufferPointer(), root_code->GetBufferSize(), IID_PPV_ARGS(&root_)))) return false;

  what = "the pipeline";
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = root_.Get();
  pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
  if (FAILED(hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pso_)))) return false;
  if (reduced_) {
    pso.CS = {down_code->GetBufferPointer(), down_code->GetBufferSize()};
    if (FAILED(hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pso_down_)))) return false;
    pso.CS = {up_code->GetBufferPointer(), up_code->GetBufferSize()};
    if (FAILED(hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pso_up_)))) return false;
  }

  what = "the descriptor heap";
  const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, DESCRIPTORS_PER_FRAME * FRAMES, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (FAILED(hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_)))) return false;
  descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  what = "the history textures";
  const D3D12_HEAP_PROPERTIES history_heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC history{};
  history.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  history.Width = work_width_;
  history.Height = work_height_;
  history.DepthOrArraySize = 1;
  history.MipLevels = 1;
  history.SampleDesc.Count = 1;
  history.Format = motion_format;
  if (FAILED(hr = device->CreateCommittedResource(&history_heap, D3D12_HEAP_FLAG_NONE, &history, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&history_motion_)))) return false;
  history.Format = DXGI_FORMAT_R32_FLOAT;
  if (FAILED(hr = device->CreateCommittedResource(&history_heap, D3D12_HEAP_FLAG_NONE, &history, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&history_depth_)))) return false;
  if (reduced_) {
    what = "the small size textures";
    history.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(hr = device->CreateCommittedResource(&history_heap, D3D12_HEAP_FLAG_NONE, &history, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&half_depth_)))) return false;
    history.Format = motion_format;
    if (FAILED(hr = device->CreateCommittedResource(&history_heap, D3D12_HEAP_FLAG_NONE, &history, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&half_out_)))) return false;
  }

  // Timing is a report only: without it the pass still runs.
  UINT64 frequency = 0;
  if (SUCCEEDED(queue->GetTimestampFrequency(&frequency)) && frequency != 0) {
    const D3D12_QUERY_HEAP_DESC query{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 2 * FRAMES, 0};
    const D3D12_HEAP_PROPERTIES readback{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 2 * FRAMES * sizeof(UINT64);
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* mapped = nullptr;
    if (SUCCEEDED(device->CreateQueryHeap(&query, IID_PPV_ARGS(&queries_))) &&
        SUCCEEDED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback_))) &&
        SUCCEEDED(readback_->Map(0, nullptr, &mapped))) {
      times_ = static_cast<const UINT64*>(mapped);
      ticks_per_ms_ = static_cast<double>(frequency) / 1000.0;
    }
  }
  what = nullptr;
  return true;
}

bool Pass::InitCamera(DXGI_FORMAT resource_format, DXGI_FORMAT view_format, UINT frame_width, UINT frame_height, const char*& what, HRESULT& hr) {
  what = "last frame's picture";
  const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC d{};
  d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  d.Width = frame_width;
  d.Height = frame_height;
  d.DepthOrArraySize = 1;
  d.MipLevels = 1;
  d.SampleDesc.Count = 1;
  d.Format = resource_format;
  if (FAILED(hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &d, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&last_colour_)))) return false;
  colour_format_ = view_format;
  frame_width_ = frame_width;
  frame_height_ = frame_height;
  what = nullptr;
  return true;
}

void Pass::Record(ID3D12GraphicsCommandList* list, UINT frame, ID3D12Resource* motion, ID3D12Resource* depth, ID3D12Resource* out, UINT width, UINT height,
                  bool depth_inverted, const float scale[2], bool reset, const Camera* camera) {
  // This frame's last pass has finished, so its timestamps are there.
  if (times_ != nullptr && timed_[frame]) {
    const UINT64 begin = times_[2 * frame], end = times_[2 * frame + 1];
    if (end > begin) {
      sum_ms_ += static_cast<double>(end - begin) / ticks_per_ms_;
      ++count_;
    }
  }

  // At a smaller size the pass reads DOWN's depth and writes its own texture, and UP writes `out`.
  const UINT full_width = width, full_height = height;
  if (reduced_) {
    width = work_width_;
    height = work_height_;
  }
  ID3D12Resource* const pass_depth = reduced_ ? half_depth_.Get() : depth;
  ID3D12Resource* const pass_out = reduced_ ? half_out_.Get() : out;

  D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += static_cast<SIZE_T>(frame) * DESCRIPTORS_PER_FRAME * descriptor_size_;
  const D3D12_CPU_DESCRIPTOR_HANDLE table_start = cpu;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  srv.Format = format_;
  device_->CreateShaderResourceView(motion, &srv, cpu);
  cpu.ptr += descriptor_size_;
  srv.Format = DXGI_FORMAT_R32_FLOAT;
  device_->CreateShaderResourceView(pass_depth, &srv, cpu);
  cpu.ptr += descriptor_size_;
  srv.Format = format_;
  device_->CreateShaderResourceView(history_motion_.Get(), &srv, cpu);
  cpu.ptr += descriptor_size_;
  srv.Format = DXGI_FORMAT_R32_FLOAT;
  device_->CreateShaderResourceView(history_depth_.Get(), &srv, cpu);
  cpu.ptr += descriptor_size_;
  // Empty views when the camera is not used: the shader does not touch them then.
  const bool have_colour = camera != nullptr && camera->colour != nullptr && last_colour_;
  srv.Format = have_colour ? colour_format_ : DXGI_FORMAT_R8G8B8A8_UNORM;
  device_->CreateShaderResourceView(have_colour ? camera->colour : nullptr, &srv, cpu);
  cpu.ptr += descriptor_size_;
  device_->CreateShaderResourceView(have_colour ? last_colour_.Get() : nullptr, &srv, cpu);
  cpu.ptr += descriptor_size_;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = format_;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  device_->CreateUnorderedAccessView(pass_out, nullptr, &uav, cpu);
  if (reduced_) {
    // DOWN's and UP's tables: t0 to t5, then u0. Slots a shader does not read get empty views.
    const auto table = [&](UINT index, ID3D12Resource* const (&reads)[6], const DXGI_FORMAT (&formats)[6], ID3D12Resource* write, DXGI_FORMAT write_format) {
      D3D12_CPU_DESCRIPTOR_HANDLE at = table_start;
      at.ptr += static_cast<SIZE_T>(index) * TABLE * descriptor_size_;
      for (int i = 0; i < 6; ++i) {
        srv.Format = formats[i];
        device_->CreateShaderResourceView(reads[i], &srv, at);
        at.ptr += descriptor_size_;
      }
      uav.Format = write_format;
      device_->CreateUnorderedAccessView(write, nullptr, &uav, at);
    };
    constexpr DXGI_FORMAT R32 = DXGI_FORMAT_R32_FLOAT;
    ID3D12Resource* const down_reads[6] = {nullptr, depth, nullptr, nullptr, nullptr, nullptr};
    const DXGI_FORMAT down_formats[6] = {format_, R32, R32, format_, format_, format_};
    table(1, down_reads, down_formats, half_depth_.Get(), R32);
    ID3D12Resource* const up_reads[6] = {nullptr, depth, half_depth_.Get(), half_out_.Get(), nullptr, nullptr};
    const DXGI_FORMAT up_formats[6] = {format_, R32, R32, format_, format_, format_};
    table(2, up_reads, up_formats, out, format_);
  }

  ID3D12DescriptorHeap* const heaps[] = {heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(root_.Get());
  list->SetPipelineState(pso_.Get());
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += static_cast<UINT64>(frame) * DESCRIPTORS_PER_FRAME * descriptor_size_;
  const auto transition = [](ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
  };
  const UINT sizes[6] = {width, height, depth_inverted ? 1u : 0u, 0u, full_width, full_height};
  if (times_ != nullptr) list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * frame);
  if (reduced_) {
    D3D12_GPU_DESCRIPTOR_HANDLE down = gpu;
    down.ptr += static_cast<UINT64>(TABLE) * descriptor_size_;
    list->SetPipelineState(pso_down_.Get());
    list->SetComputeRootDescriptorTable(0, down);
    list->SetComputeRoot32BitConstants(1, 6, sizes, 0);
    list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
    const D3D12_RESOURCE_BARRIER read = transition(half_depth_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &read);
    list->SetPipelineState(pso_.Get());
  }
  list->SetComputeRootDescriptorTable(0, gpu);
  // Last frame's values mean something else after a new scale or depth direction.
  const bool use_history = have_history_ && !reset && depth_inverted == last_inverted_ && scale[0] == last_scale_[0] && scale[1] == last_scale_[1];
  const float to_pixels[2] = {scale[0] * static_cast<float>(width), scale[1] * static_cast<float>(height)};
  const bool use_camera = have_colour && have_last_colour_ && !reset && camera->clip_to_prev != nullptr && scale[0] != 0.f && scale[1] != 0.f;
  UINT constants[ROOT_CONSTANTS] = {width, height, depth_inverted ? 1u : 0u, use_history ? 1u : 0u};
  std::memcpy(&constants[4], to_pixels, sizeof(to_pixels));
  constants[6] = use_camera ? 1u : 0u;
  const D3D12_RESOURCE_DESC motion_desc = motion->GetDesc();
  if (motion_desc.Width != width || motion_desc.Height != height) constants[7] = static_cast<UINT>(motion_desc.Width) | (motion_desc.Height << 16);
  constants[8] = frame_width_;
  constants[9] = frame_height_;
  std::memcpy(&constants[10], scale, 2 * sizeof(float));
  if (use_camera) std::memcpy(&constants[12], camera->clip_to_prev, 16 * sizeof(float));
  list->SetComputeRoot32BitConstants(1, ROOT_CONSTANTS, constants, 0);
  list->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
  // At a smaller size UP brings the result to `out`. The history copies then read the small textures, which go back to UNORDERED_ACCESS after.
  const D3D12_RESOURCE_STATES out_state = reduced_ ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
  const D3D12_RESOURCE_STATES depth_end = reduced_ ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  if (reduced_) {
    const D3D12_RESOURCE_BARRIER read = transition(half_out_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &read);
    D3D12_GPU_DESCRIPTOR_HANDLE up = gpu;
    up.ptr += static_cast<UINT64>(2) * TABLE * descriptor_size_;
    list->SetPipelineState(pso_up_.Get());
    list->SetComputeRootDescriptorTable(0, up);
    list->SetComputeRoot32BitConstants(1, 6, sizes, 0);
    list->Dispatch((full_width + 7) / 8, (full_height + 7) / 8, 1);
  }
  // This frame's result and depth are next frame's history.
  const D3D12_RESOURCE_BARRIER before[] = {transition(pass_out, out_state, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                           transition(pass_depth, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                           transition(history_motion_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
                                           transition(history_depth_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
  list->ResourceBarrier(static_cast<UINT>(std::size(before)), before);
  list->CopyResource(history_motion_.Get(), pass_out);
  list->CopyResource(history_depth_.Get(), pass_depth);
  const D3D12_RESOURCE_BARRIER after[] = {transition(pass_out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
                                          transition(pass_depth, D3D12_RESOURCE_STATE_COPY_SOURCE, depth_end),
                                          transition(history_motion_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                          transition(history_depth_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
  list->ResourceBarrier(static_cast<UINT>(std::size(after)), after);
  if (have_colour) {
    const D3D12_RESOURCE_BARRIER to_copy = transition(last_colour_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &to_copy);
    list->CopyResource(last_colour_.Get(), camera->colour);
    const D3D12_RESOURCE_BARRIER to_read = transition(last_colour_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &to_read);
  }
  have_last_colour_ = have_colour;
  have_history_ = true;
  last_inverted_ = depth_inverted;
  last_scale_[0] = scale[0];
  last_scale_[1] = scale[1];
  if (times_ != nullptr) {
    list->EndQuery(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * frame + 1);
    list->ResolveQueryData(queries_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * frame, 2, readback_.Get(), 2 * frame * sizeof(UINT64));
    timed_[frame] = true;
  }
}

bool Pass::TakeAverageMs(UINT every, double& ms) {
  if (count_ < every) return false;
  ms = sum_ms_ / count_;
  sum_ms_ = 0;
  count_ = 0;
  return true;
}

}  // namespace hsf::compose
