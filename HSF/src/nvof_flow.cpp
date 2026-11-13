// See nvof_flow.h. Ported from DLSS5 ReShade AIO's nvof-motion-provider.cpp (Copyright 2026 kibblerz, Apache-2.0) and changed for HSF.

#include "nvof_flow.h"

#include <windows.h>

#include <d3dcompiler.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <vector>

#include "nvOpticalFlowD3D12.h"

namespace hsf::nvof {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT DESCRIPTORS_PER_SLOT = 8;  // the picture, the grey image; forward, backward, their two costs, depth; the motion
constexpr UINT CONVERT_CONSTANTS = 16;
// AIO's defaults. The two flows must agree to 3 pixels of a 1080-line picture, a cost over 0.35 of the scale starts to count as a bad match,
// and a bad pixel takes all of the trusted motion found around it on its own surface.
constexpr float AGREE_PIXELS_AT_1080 = 3.0f;
constexpr float COST_THRESHOLD = 0.35f;
constexpr float DEPTH_SENSITIVITY = 0.02f;
constexpr float REPAIR = 1.0f;
constexpr DWORD STALL_MS = 500;

// The picture to a small grey image: four taps a pixel, bright (HDR) pictures brought into range first.
const char PREP_SHADER[] = R"(
Texture2D<float4> Source : register(t0);
RWTexture2D<float> Grey : register(u0);
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  uint w, h, sw, sh;
  Grey.GetDimensions(w, h);
  Source.GetDimensions(sw, sh);
  if (id.x >= w || id.y >= h) return;
  float4 c;
  if (w == sw && h == sh) {
    c = Source.Load(int3(id.xy, 0));
  } else {
    const float2 scale = float2(sw, sh) / float2(w, h);
    const uint2 last = uint2(sw - 1, sh - 1);
    const uint2 p00 = min(uint2((float2(id.xy) + float2(0.25, 0.25)) * scale), last);
    const uint2 p10 = min(uint2((float2(id.xy) + float2(0.75, 0.25)) * scale), last);
    const uint2 p01 = min(uint2((float2(id.xy) + float2(0.25, 0.75)) * scale), last);
    const uint2 p11 = min(uint2((float2(id.xy) + float2(0.75, 0.75)) * scale), last);
    c = (Source.Load(int3(p00, 0)) + Source.Load(int3(p10, 0)) + Source.Load(int3(p01, 0)) + Source.Load(int3(p11, 0))) * 0.25;
  }
  const float peak = max(max(c.r, c.g), max(c.b, 1.0));
  c.rgb = saturate(c.rgb / peak);
  Grey[id.xy] = dot(c.rgb, float3(0.2126, 0.7152, 0.0722));
}
)";

// The engine's answer to motion. Forward is this picture to the last one, in 1/32 pixels of the grey image, one value per Grid x Grid block; backward is the
// last one to this. A pixel's motion is trusted less when following it forward and then backward does not come back (Consistency, in pixels of the output),
// when the engine's own cost for the match is high, or when a depth edge lies at the pixel or between it and where it came from. A pixel that is not
// trusted takes the trusted motion of 20 points around it that lie on its own surface, and a foreground edge takes the foreground's.
const char CONVERT_SHADER[] = R"(
Texture2D<int2> Forward : register(t0);
Texture2D<int2> Backward : register(t1);
Texture2D<uint> ForwardCost : register(t2);
Texture2D<uint> BackwardCost : register(t3);
Texture2D<float> SceneDepth : register(t4);
RWTexture2D<float4> Motion : register(u0);
cbuffer Constants : register(b0) {
  uint Width; uint Height; uint FlowWidth; uint FlowHeight;
  uint Grid; float Consistency; float CostThreshold; float CostScale;
  uint UseDepth; uint DepthWidth; uint DepthHeight; uint DepthReversed;
  float DepthSensitivity; float Repair; float2 ToTexel;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= Width || id.y >= Height) return;
  const float2 guideToFlow = float2(FlowWidth, FlowHeight) / float2(Width, Height);
  const float2 flowToGuide = float2(Width, Height) / float2(FlowWidth, FlowHeight);
  const float2 flowMax = float2(FlowWidth - 1, FlowHeight - 1);
  const float2 flowPixel = (float2(id.xy) + 0.5) * guideToFlow - 0.5;
  const uint2 cell = uint2(clamp(flowPixel, float2(0, 0), flowMax)) / Grid;
  const float2 fFlow = float2(Forward.Load(int3(cell, 0))) / 32.0;
  const float2 f = fFlow * flowToGuide;
  const float2 previous = float2(id.xy) + f;
  const float2 previousFlow = flowPixel + fFlow;
  const uint2 priorCell = uint2(clamp(previousFlow, float2(0, 0), flowMax)) / Grid;
  const float2 b = float2(Backward.Load(int3(priorCell, 0))) / 32.0 * flowToGuide;
  const float cf = ForwardCost.Load(int3(cell, 0)) * CostScale;
  const float cb = BackwardCost.Load(int3(priorCell, 0)) * CostScale;
  const float badFlow = smoothstep(Consistency, Consistency * 2.0, length(f + b));
  const float badCost = smoothstep(CostThreshold, 1.0, max(cf, cb));
  float depthReject = 0.0;
  float sceneDepth = DepthReversed != 0 ? 0.0 : 1.0;
  const float2 depthScale = float2(DepthWidth, DepthHeight) / float2(Width, Height);
  const float clearDepth = DepthReversed != 0 ? 0.0 : 1.0;
  float valid = 0.0;
  if (UseDepth != 0) {
    const uint2 dp = min(uint2((float2(id.xy) + 0.5) * depthScale), uint2(DepthWidth - 1, DepthHeight - 1));
    sceneDepth = SceneDepth.Load(int3(dp, 0));
    const uint2 dx0 = uint2(dp.x > 0 ? dp.x - 1 : dp.x, dp.y);
    const uint2 dx1 = uint2(min(dp.x + 1, DepthWidth - 1), dp.y);
    const uint2 dy0 = uint2(dp.x, dp.y > 0 ? dp.y - 1 : dp.y);
    const uint2 dy1 = uint2(dp.x, min(dp.y + 1, DepthHeight - 1));
    const float localDelta = max(max(abs(sceneDepth - SceneDepth.Load(int3(dx0, 0))), abs(sceneDepth - SceneDepth.Load(int3(dx1, 0)))),
                                 max(abs(sceneDepth - SceneDepth.Load(int3(dy0, 0))), abs(sceneDepth - SceneDepth.Load(int3(dy1, 0)))));
    const float2 endpoint = clamp(previous, float2(0, 0), float2(Width - 1, Height - 1));
    const uint2 ep = min(uint2((endpoint + 0.5) * depthScale), uint2(DepthWidth - 1, DepthHeight - 1));
    const float endpointDelta = abs(sceneDepth - SceneDepth.Load(int3(ep, 0)));
    valid = abs(sceneDepth - clearDepth) > 1e-7 ? 1.0 : 0.0;
    depthReject = valid * saturate(max(smoothstep(DepthSensitivity * 0.25, DepthSensitivity, localDelta), smoothstep(DepthSensitivity * 0.5, DepthSensitivity * 2.0, endpointDelta)));
  }
  const float rawRejection = saturate(max(max(badFlow, badCost), depthReject));
  float2 repaired = f;
  float coverage = 0.0;
  if (UseDepth != 0 && valid > 0.5 && Repair > 0.0) {
    float2 bestFlow = f;
    float bestScore = 1000.0;
    float2 motionSum = float2(0, 0);
    float motionWeight = 0.0, motionEnergy = 0.0;
    float foregroundEvidence = 0.0, foregroundMotionWeight = 0.0, foregroundRejectionSum = 0.0, foregroundSurfaceWeight = 0.0;
    float2 foregroundMotionSum = float2(0, 0);
    static const int2 offsets[20] = {int2(-4, 0), int2(4, 0), int2(0, -4), int2(0, 4), int2(-8, 0), int2(8, 0), int2(0, -8), int2(0, 8),
                                     int2(-16, 0), int2(16, 0), int2(0, -16), int2(0, 16), int2(-32, 0), int2(32, 0), int2(0, -32), int2(0, 32),
                                     int2(-8, -8), int2(8, -8), int2(-8, 8), int2(8, 8)};
    [unroll] for (int i = 0; i < 20; i++) {
      const int2 candidate = int2(id.xy) + offsets[i];
      if (any(candidate < 0) || candidate.x >= int(Width) || candidate.y >= int(Height)) continue;
      const uint2 cp = uint2(candidate);
      const uint2 cdp = min(uint2((float2(cp) + 0.5) * depthScale), uint2(DepthWidth - 1, DepthHeight - 1));
      const float cd = SceneDepth.Load(int3(cdp, 0));
      if (abs(cd - clearDepth) <= 1e-7) continue;
      const float depthGap = abs(cd - sceneDepth) / max(max(abs(cd), abs(sceneDepth)), 1e-4);
      const float signedDepthGap = (DepthReversed != 0 ? sceneDepth - cd : cd - sceneDepth) / max(max(abs(cd), abs(sceneDepth)), 1e-4);
      const float2 candidateFlowPixel = (float2(cp) + 0.5) * guideToFlow - 0.5;
      const uint2 ccell = uint2(clamp(candidateFlowPixel, float2(0, 0), flowMax)) / Grid;
      const float2 candidateFlowRaw = float2(Forward.Load(int3(ccell, 0))) / 32.0;
      const float2 candidateFlow = candidateFlowRaw * flowToGuide;
      const float2 candidatePrevious = float2(cp) + candidateFlow;
      const bool candidateOutside = any(candidatePrevious < 0.0) || candidatePrevious.x >= Width || candidatePrevious.y >= Height;
      const float2 candidatePreviousFlow = candidateFlowPixel + candidateFlowRaw;
      const uint2 cbcell = uint2(clamp(candidatePreviousFlow, float2(0, 0), flowMax)) / Grid;
      const float2 candidateBackward = float2(Backward.Load(int3(cbcell, 0))) / 32.0 * flowToGuide;
      const float candidateFlowBad = smoothstep(Consistency, Consistency * 2.0, length(candidateFlow + candidateBackward));
      const float candidateCost = max(ForwardCost.Load(int3(ccell, 0)) * CostScale, BackwardCost.Load(int3(cbcell, 0)) * CostScale);
      const float candidateBad = max(candidateFlowBad, smoothstep(CostThreshold, 1.0, candidateCost));
      const float surfaceMismatch = smoothstep(DepthSensitivity, DepthSensitivity * 4.0, depthGap);
      const float distanceWeight = i < 4 ? 1.0 : (i < 8 ? 0.8 : (i < 12 ? 0.6 : (i < 16 ? 0.4 : 0.65)));
      const float inside = candidateOutside ? 0.0 : 1.0;
      const float reliability = inside * saturate(1.0 - candidateBad);
      const float surfaceWeight = inside * (1.0 - surfaceMismatch) * distanceWeight;
      const float weight = reliability * surfaceWeight;
      motionSum += candidateFlow * weight;
      motionWeight += weight;
      motionEnergy += length(candidateFlow) * weight;
      const float fartherBackground = smoothstep(DepthSensitivity, DepthSensitivity * 4.0, signedDepthGap);
      foregroundEvidence += fartherBackground * distanceWeight;
      const float relaxedSurface = 1.0 - smoothstep(DepthSensitivity * 2.0, DepthSensitivity * 12.0, depthGap);
      const float notFarBackground = 1.0 - smoothstep(DepthSensitivity, DepthSensitivity * 3.0, signedDepthGap);
      const float foregroundWeight = inside * relaxedSurface * notFarBackground * distanceWeight;
      foregroundRejectionSum += candidateBad * foregroundWeight;
      foregroundSurfaceWeight += foregroundWeight;
      const float trustedForegroundWeight = foregroundWeight * reliability;
      foregroundMotionSum += candidateFlow * trustedForegroundWeight;
      foregroundMotionWeight += trustedForegroundWeight;
      const float score = (candidateOutside ? 1.0 : candidateBad) + surfaceMismatch * 2.0 + (1.0 - distanceWeight) * 0.1;
      if (score < bestScore) {
        bestScore = score;
        bestFlow = candidateFlow;
      }
    }
    if (bestScore < rawRejection) repaired = bestFlow;
    const float2 consensus = motionWeight > 0.5 ? motionSum / motionWeight : f;
    const float coherence = motionEnergy > 0.01 ? length(motionSum) / motionEnergy : 0.0;
    const float coverageGap = length(consensus) - length(f);
    coverage = smoothstep(0.5, 2.0, coverageGap) * smoothstep(0.55, 0.82, coherence) * smoothstep(0.5, 2.0, motionWeight);
    repaired = lerp(repaired, consensus, coverage);
    const float foregroundRejection = foregroundSurfaceWeight > 0.5 ? foregroundRejectionSum / foregroundSurfaceWeight : rawRejection;
    const float foregroundTrust = saturate(1.0 - foregroundRejection);
    const float foregroundHalo = smoothstep(0.5, 1.5, foregroundEvidence) * smoothstep(0.5, 1.5, foregroundMotionWeight) * smoothstep(0.45, 0.85, foregroundTrust);
    const float2 foregroundMotion = foregroundMotionWeight > 0.5 ? foregroundMotionSum / foregroundMotionWeight : repaired;
    repaired = lerp(repaired, foregroundMotion, foregroundHalo);
    coverage = max(coverage, foregroundHalo);
  }
  const float2 pixels = lerp(f, repaired, saturate(Repair * max(rawRejection, coverage)));
  Motion[id.xy] = float4(pixels * ToTexel, 0.0, 0.0);
}
)";

NV_OF_D3D12_API_FUNCTION_LIST& Api(void* api) { return *static_cast<NV_OF_D3D12_API_FUNCTION_LIST*>(api); }

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.StateBefore = before;
  barrier.Transition.StateAfter = after;
  barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  return barrier;
}

HRESULT MakeTexture(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& resource) {
  const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = flags;
  return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr, IID_PPV_ARGS(&resource));
}

D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap* heap, UINT size, UINT index) {
  D3D12_CPU_DESCRIPTOR_HANDLE handle = heap->GetCPUDescriptorHandleForHeapStart();
  handle.ptr += static_cast<SIZE_T>(size) * index;
  return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE Gpu(ID3D12DescriptorHeap* heap, UINT size, UINT index) {
  D3D12_GPU_DESCRIPTOR_HANDLE handle = heap->GetGPUDescriptorHandleForHeapStart();
  handle.ptr += static_cast<UINT64>(size) * index;
  return handle;
}

D3D12_SHADER_RESOURCE_VIEW_DESC Srv(DXGI_FORMAT format) {
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.Format = format;
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  return srv;
}

D3D12_UNORDERED_ACCESS_VIEW_DESC Uav(DXGI_FORMAT format) {
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.Format = format;
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  return uav;
}

// A root signature of one table per range, then `constants` 32-bit values at b0. From the loaded d3d12.dll: the add-on must not import it.
HRESULT MakeRoot(ID3D12Device* device, UINT srvs, UINT constants, ComPtr<ID3D12RootSignature>& root) {
  const D3D12_DESCRIPTOR_RANGE ranges[2] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, srvs, 0, 0, 0}, {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0}};
  D3D12_ROOT_PARAMETER params[3] = {};
  for (UINT i = 0; i < 2; ++i) {
    params[i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[i].DescriptorTable.NumDescriptorRanges = 1;
    params[i].DescriptorTable.pDescriptorRanges = &ranges[i];
  }
  params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[2].Constants.Num32BitValues = constants;
  const D3D12_ROOT_SIGNATURE_DESC desc{constants != 0 ? 3u : 2u, params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  const auto serialize = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
  if (serialize == nullptr) return E_NOINTERFACE;
  ComPtr<ID3DBlob> code, messages;
  const HRESULT hr = serialize(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &code, &messages);
  if (FAILED(hr)) return hr;
  return device->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&root));
}

HRESULT MakePipeline(ID3D12Device* device, const char* source, size_t length, const char* name, ID3D12RootSignature* root, ComPtr<ID3D12PipelineState>& pso) {
  ComPtr<ID3DBlob> code, messages;
  const HRESULT hr = D3DCompile(source, length, name, nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);
  if (FAILED(hr)) return hr;
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
  desc.pRootSignature = root;
  desc.CS = {code->GetBufferPointer(), code->GetBufferSize()};
  return device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pso));
}

}  // namespace

void Flow::Say(const char* format, ...) {
  if (log_ == nullptr) return;
  char text[400];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  log_(text);
}

void Flow::Fail(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(status_, sizeof(status_), format, args);
  va_end(args);
  state_ = State::failed;
  Say("NVOF not available: %s", status_);
}

bool Flow::Register(ID3D12Resource* resource, void** handle) {
  NV_OF_REGISTER_RESOURCE_PARAMS_D3D12 params{};
  params.resource = resource;
  params.inputFencePoint.fence = register_fence_.Get();
  params.inputFencePoint.value = register_value_;
  params.hOFGpuBuffer = reinterpret_cast<NvOFGPUBufferHandle*>(handle);
  params.outputFencePoint.fence = register_fence_.Get();
  params.outputFencePoint.value = ++register_value_;
  const NV_OF_STATUS status = Api(api_).nvOFRegisterResourceD3D12(static_cast<NvOFHandle>(session_), &params);
  if (status != NV_OF_SUCCESS) {
    Fail("the engine refused a texture (%d)", static_cast<int>(status));
    return false;
  }
  if (FAILED(register_fence_->SetEventOnCompletion(register_value_, event_)) || WaitForSingleObject(event_, 5000) != WAIT_OBJECT_0) {
    Fail("the engine took over 5 s to take a texture");
    return false;
  }
  return *handle != nullptr;
}

bool Flow::MakePipelines() {
  const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, DESCRIPTORS_PER_SLOT * SLOTS, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  HRESULT hr = device_->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_));
  descriptor_size_ = device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  if (SUCCEEDED(hr)) hr = MakeRoot(device_, 1, 0, prep_root_);
  if (SUCCEEDED(hr)) hr = MakePipeline(device_, PREP_SHADER, sizeof(PREP_SHADER) - 1, "hsf_nvof_prep", prep_root_.Get(), prep_pso_);
  if (SUCCEEDED(hr)) hr = MakeRoot(device_, 5, CONVERT_CONSTANTS, convert_root_);
  if (SUCCEEDED(hr)) hr = MakePipeline(device_, CONVERT_SHADER, sizeof(CONVERT_SHADER) - 1, "hsf_nvof_convert", convert_root_.Get(), convert_pso_);
  if (FAILED(hr)) {
    Fail("HSF's shaders for it (0x%08lx)", hr);
    return false;
  }
  return true;
}

bool Flow::MakeResources(const Frame& frame) {
  const NvOFHandle session = static_cast<NvOFHandle>(session_);
  const auto formats = [&](NV_OF_BUFFER_USAGE usage) {
    std::vector<DXGI_FORMAT> result;
    uint32_t count = 0;
    if (Api(api_).nvOFGetSurfaceFormatCountD3D12(session, usage, NV_OF_MODE_OPTICALFLOW, &count) != NV_OF_SUCCESS || count == 0) return result;
    result.resize(count);
    if (Api(api_).nvOFGetSurfaceFormatD3D12(session, usage, NV_OF_MODE_OPTICALFLOW, result.data()) != NV_OF_SUCCESS) result.clear();
    return result;
  };
  const auto has = [](const std::vector<DXGI_FORMAT>& list, DXGI_FORMAT format) { return std::find(list.begin(), list.end(), format) != list.end(); };
  const std::vector<DXGI_FORMAT> inputs = formats(NV_OF_BUFFER_USAGE_INPUT), outputs = formats(NV_OF_BUFFER_USAGE_OUTPUT), costs = formats(NV_OF_BUFFER_USAGE_COST);
  cost_format_ = has(costs, DXGI_FORMAT_R8_UINT) ? DXGI_FORMAT_R8_UINT : has(costs, DXGI_FORMAT_R32_UINT) ? DXGI_FORMAT_R32_UINT : DXGI_FORMAT_UNKNOWN;
  if (!has(inputs, DXGI_FORMAT_R8_UNORM) || !has(outputs, DXGI_FORMAT_R16G16_SINT) || cost_format_ == DXGI_FORMAT_UNKNOWN) {
    Fail("this driver's engine lacks the grey input, 16-bit flow or cost formats");
    return false;
  }
  const UINT cells_x = (flow_width_ + grid_ - 1) / grid_, cells_y = (flow_height_ + grid_ - 1) / grid_;
  const D3D12_COMMAND_LIST_TYPE type = queue_->GetDesc().Type;
  HRESULT hr = S_OK;
  for (Slot& slot : slots_) {
    if (SUCCEEDED(hr)) hr = device_->CreateCommandAllocator(type, IID_PPV_ARGS(&slot.alloc));
    if (SUCCEEDED(hr)) hr = MakeTexture(device_, flow_width_, flow_height_, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON, slot.input);
  }
  if (SUCCEEDED(hr)) hr = device_->CreateCommandList(0, type, slots_[0].alloc.Get(), nullptr, IID_PPV_ARGS(&prep_list_));
  if (SUCCEEDED(hr)) hr = prep_list_->Close();
  if (SUCCEEDED(hr)) hr = MakeTexture(device_, cells_x, cells_y, DXGI_FORMAT_R16G16_SINT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, forward_);
  if (SUCCEEDED(hr)) hr = MakeTexture(device_, cells_x, cells_y, DXGI_FORMAT_R16G16_SINT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, backward_);
  if (SUCCEEDED(hr)) hr = MakeTexture(device_, cells_x, cells_y, cost_format_, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, forward_cost_);
  if (SUCCEEDED(hr)) hr = MakeTexture(device_, cells_x, cells_y, cost_format_, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, backward_cost_);
  if (SUCCEEDED(hr)) hr = MakeTexture(device_, out_width_, out_height_, out_format_, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, out_);
  if (SUCCEEDED(hr) && copy_colour_) {
    D3D12_RESOURCE_DESC desc = frame.colour->GetDesc();
    desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Alignment = 0;
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    hr = device_->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&copy_));
  }
  if (FAILED(hr)) {
    Fail("HSF's textures for it (0x%08lx)", hr);
    return false;
  }
  for (Slot& slot : slots_)
    if (!Register(slot.input.Get(), &slot.input_handle)) return false;
  if (!Register(forward_.Get(), &forward_handle_) || !Register(backward_.Get(), &backward_handle_) || !Register(forward_cost_.Get(), &forward_cost_handle_) ||
      !Register(backward_cost_.Get(), &backward_cost_handle_))
    return false;
  // The views that never change. The picture's and the depth's are written each frame.
  for (UINT i = 0; i < SLOTS; ++i) {
    const UINT base = i * DESCRIPTORS_PER_SLOT;
    const D3D12_UNORDERED_ACCESS_VIEW_DESC grey = Uav(DXGI_FORMAT_R8_UNORM);
    device_->CreateUnorderedAccessView(slots_[i].input.Get(), nullptr, &grey, Cpu(heap_.Get(), descriptor_size_, base + 1));
    const D3D12_SHADER_RESOURCE_VIEW_DESC flow = Srv(DXGI_FORMAT_R16G16_SINT), cost = Srv(cost_format_);
    device_->CreateShaderResourceView(forward_.Get(), &flow, Cpu(heap_.Get(), descriptor_size_, base + 2));
    device_->CreateShaderResourceView(backward_.Get(), &flow, Cpu(heap_.Get(), descriptor_size_, base + 3));
    device_->CreateShaderResourceView(forward_cost_.Get(), &cost, Cpu(heap_.Get(), descriptor_size_, base + 4));
    device_->CreateShaderResourceView(backward_cost_.Get(), &cost, Cpu(heap_.Get(), descriptor_size_, base + 5));
    const D3D12_UNORDERED_ACCESS_VIEW_DESC motion = Uav(out_format_);
    device_->CreateUnorderedAccessView(out_.Get(), nullptr, &motion, Cpu(heap_.Get(), descriptor_size_, base + 7));
  }
  return true;
}

bool Flow::Start(ID3D12Device* device, ID3D12CommandQueue* queue, const Frame& frame) {
  const D3D12_RESOURCE_DESC colour = frame.colour->GetDesc();
  device_ = device;
  queue_ = queue;
  colour_width_ = static_cast<UINT>(colour.Width);
  colour_height_ = colour.Height;
  colour_format_ = colour.Format;
  colour_view_ = frame.colour_view;
  copy_colour_ = frame.copy_colour;
  out_width_ = frame.width;
  out_height_ = frame.height;
  out_format_ = frame.format;
  size_ = frame.size;
  // Keeps the picture's shape; even sizes.
  flow_height_ = std::max(2u, (size_ == 0 ? colour_height_ : std::min(size_, colour_height_)) & ~1u);
  flow_width_ = std::max(2u, static_cast<UINT>(static_cast<UINT64>(colour_width_) * flow_height_ / colour_height_) & ~1u);

  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{out_format_, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
  if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) || (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0) {
    Fail("the motion format %u cannot be written by a compute shader", static_cast<unsigned>(out_format_));
    return false;
  }
  // The driver's own library, never HSF's. It stays loaded for the life of the process.
  static const HMODULE module = LoadLibraryW(L"nvofapi64.dll");
  if (module == nullptr) {
    Fail("nvofapi64.dll is not there (an NVIDIA RTX card and its driver are needed)");
    return false;
  }
  using CreateFn = NV_OF_STATUS(NVOFAPI*)(uint32_t, NV_OF_D3D12_API_FUNCTION_LIST*);
  const auto create = reinterpret_cast<CreateFn>(GetProcAddress(module, "NvOFAPICreateInstanceD3D12"));
  if (create == nullptr) {
    Fail("this driver's nvofapi64.dll has no Direct3D 12 entry (driver too old)");
    return false;
  }
  if (api_ == nullptr) api_ = new NV_OF_D3D12_API_FUNCTION_LIST{};
  NV_OF_STATUS status = create(NV_OF_API_VERSION, &Api(api_));
  if (status != NV_OF_SUCCESS) {
    Fail("the engine's API did not start (%d)", static_cast<int>(status));
    return false;
  }
  NvOFHandle session = nullptr;
  status = Api(api_).nvCreateOpticalFlowD3D12(device, &session);
  if (status != NV_OF_SUCCESS || session == nullptr) {
    Fail("no optical flow session on this device (%d): the card has no engine, or it is in use", static_cast<int>(status));
    return false;
  }
  session_ = session;
  // The finest grid the card has: 1 on newer cards, 4 on RTX 20.
  uint32_t grid_count = 0;
  std::vector<uint32_t> grids;
  if (Api(api_).nvOFGetCaps(session, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, nullptr, &grid_count) == NV_OF_SUCCESS && grid_count != 0) {
    grids.resize(grid_count);
    if (Api(api_).nvOFGetCaps(session, NV_OF_CAPS_SUPPORTED_OUTPUT_GRID_SIZES, grids.data(), &grid_count) != NV_OF_SUCCESS) grids.clear();
  }
  grid_ = std::find(grids.begin(), grids.end(), 1u) != grids.end() ? 1u : std::find(grids.begin(), grids.end(), 2u) != grids.end() ? 2u : 4u;
  NV_OF_INIT_PARAMS init{};
  init.width = flow_width_;
  init.height = flow_height_;
  init.outGridSize = static_cast<NV_OF_OUTPUT_VECTOR_GRID_SIZE>(grid_);
  init.hintGridSize = NV_OF_HINT_VECTOR_GRID_SIZE_UNDEFINED;
  init.mode = NV_OF_MODE_OPTICALFLOW;
  init.perfLevel = NV_OF_PERF_LEVEL_MEDIUM;
  init.enableExternalHints = NV_OF_FALSE;
  init.enableOutputCost = NV_OF_TRUE;
  init.disparityRange = NV_OF_STEREO_DISPARITY_RANGE_UNDEFINED;
  init.enableRoi = NV_OF_FALSE;
  init.predDirection = NV_OF_PRED_DIRECTION_BOTH;
  init.enableGlobalFlow = NV_OF_FALSE;
  init.inputBufferFormat = NV_OF_BUFFER_FORMAT_GRAYSCALE8;
  status = Api(api_).nvOFInit(session, &init);
  if (status != NV_OF_SUCCESS) {
    Fail("the engine refused %ux%u with grid %u (%d)", flow_width_, flow_height_, grid_, static_cast<int>(status));
    return false;
  }
  event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  HRESULT hr = event_ != nullptr ? S_OK : E_OUTOFMEMORY;
  if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&prep_fence_));
  if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&flow_fence_));
  if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&register_fence_));
  if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done_));
  if (FAILED(hr)) {
    Fail("HSF's fences for it (0x%08lx)", hr);
    return false;
  }
  if (!MakePipelines() || !MakeResources(frame)) return false;
  state_ = State::waiting;
  snprintf(status_, sizeof(status_), "%ux%u, grid %u", flow_width_, flow_height_, grid_);
  Say("NVOF ready: flow %ux%u, grid %u, both directions with cost; motion %ux%u format %u from a %ux%u picture", flow_width_, flow_height_, grid_, out_width_, out_height_,
      static_cast<unsigned>(out_format_), colour_width_, colour_height_);
  return true;
}

void Flow::Shutdown() {
  // Nothing goes away while the GPU may still use it.
  if (flow_fence_ && flow_fence_->GetCompletedValue() < flow_value_ && GetTickCount64() - flow_sent_ms_ > STALL_MS) flow_fence_->Signal(flow_value_);
  if (queue_ != nullptr && done_ && event_ != nullptr && SUCCEEDED(queue_->Signal(done_.Get(), ++done_value_)) && SUCCEEDED(done_->SetEventOnCompletion(done_value_, event_)))
    WaitForSingleObject(event_, 2000);
  // AIO's order: the engine lets go of each texture, the textures go, then the session.
  if (session_ != nullptr && api_ != nullptr) {
    const auto unregister = [&](void*& handle) {
      if (handle == nullptr) return;
      NV_OF_UNREGISTER_RESOURCE_PARAMS_D3D12 params{};
      params.hOFGpuBuffer = static_cast<NvOFGPUBufferHandle>(handle);
      Api(api_).nvOFUnregisterResourceD3D12(&params);
      handle = nullptr;
    };
    unregister(backward_cost_handle_);
    unregister(forward_cost_handle_);
    unregister(backward_handle_);
    unregister(forward_handle_);
    for (Slot& slot : slots_) unregister(slot.input_handle);
  }
  for (Slot& slot : slots_) slot = {};
  forward_.Reset();
  backward_.Reset();
  forward_cost_.Reset();
  backward_cost_.Reset();
  if (session_ != nullptr && api_ != nullptr) Api(api_).nvOFDestroy(static_cast<NvOFHandle>(session_));
  session_ = nullptr;
  delete static_cast<NV_OF_D3D12_API_FUNCTION_LIST*>(api_);
  api_ = nullptr;
  prep_list_.Reset();
  copy_.Reset();
  out_.Reset();
  heap_.Reset();
  prep_pso_.Reset();
  convert_pso_.Reset();
  prep_root_.Reset();
  convert_root_.Reset();
  prep_fence_.Reset();
  flow_fence_.Reset();
  register_fence_.Reset();
  done_.Reset();
  if (event_ != nullptr) CloseHandle(event_);
  event_ = nullptr;
  prep_value_ = flow_value_ = register_value_ = done_value_ = 0;
  device_ = nullptr;
  queue_ = nullptr;
  count_ = 0;
  previous_ = -1;
  made_ = skipped_ = 0;
  tried_ = false;
  state_ = State::off;
}

ID3D12Resource* Flow::Run(ID3D12Device* device, ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list, const Frame& frame, LogFn log) {
  log_ = log;
  if (device == nullptr || queue == nullptr || list == nullptr || frame.colour == nullptr || frame.width == 0 || frame.height == 0 || frame.format == DXGI_FORMAT_UNKNOWN) return nullptr;
  const D3D12_RESOURCE_DESC colour = frame.colour->GetDesc();
  // A new device, picture or motion texture: everything starts again, and one more try if it had failed.
  if (tried_ && (device != device_ || queue != queue_ || colour.Width != colour_width_ || colour.Height != colour_height_ || colour.Format != colour_format_ || frame.colour_view != colour_view_ ||
                 frame.copy_colour != copy_colour_ || frame.width != out_width_ || frame.height != out_height_ || frame.format != out_format_ || frame.size != size_))
    Shutdown();
  if (!tried_) {
    tried_ = true;
    if (!Start(device, queue, frame)) return nullptr;
  }
  if (state_ == State::failed || session_ == nullptr) return nullptr;

  // The engine's signal for the last frame: the queue is waiting for it on the GPU. One that never comes would stop the queue for good, so it is let go from the CPU.
  if (flow_fence_->GetCompletedValue() < flow_value_ && GetTickCount64() - flow_sent_ms_ > STALL_MS) {
    flow_fence_->Signal(flow_value_);
    Fail("the engine's signal never came (stood at %llu of %llu for over %lu ms); the effect's flow is used for the rest of the session", flow_fence_->GetCompletedValue(), flow_value_, STALL_MS);
    return nullptr;
  }
  const ULONGLONG now = GetTickCount64();
  if (now - last_report_ms_ >= 10000) {
    if (last_report_ms_ != 0 && made_ + skipped_ != 0) Say("NVOF: %u frames with its flow, %u without, in the last 10 s", made_, skipped_);
    last_report_ms_ = now;
    made_ = skipped_ = 0;
  }

  // Everything sent to the queue before this call ends here.
  queue->Signal(done_.Get(), ++done_value_);
  const UINT s = static_cast<UINT>(count_ % SLOTS);
  Slot& slot = slots_[s];
  if (done_->GetCompletedValue() < slot.free_at) {
    done_->SetEventOnCompletion(slot.free_at, event_);
    WaitForSingleObject(event_, 4);
    if (done_->GetCompletedValue() < slot.free_at) {
      ++skipped_;
      return nullptr;
    }
  }
  if (frame.reset) previous_ = -1;

  // The grey image, on a list of HSF's own that runs now, so the engine can start before the caller's list is even recorded.
  const UINT base = s * DESCRIPTORS_PER_SLOT;
  ID3D12Resource* const source = copy_colour_ ? copy_.Get() : frame.colour;
  const D3D12_SHADER_RESOURCE_VIEW_DESC source_view = Srv(colour_view_);
  device->CreateShaderResourceView(source, &source_view, Cpu(heap_.Get(), descriptor_size_, base));
  if (FAILED(slot.alloc->Reset()) || FAILED(prep_list_->Reset(slot.alloc.Get(), prep_pso_.Get()))) {
    ++skipped_;
    return nullptr;
  }
  ID3D12GraphicsCommandList* const prep = prep_list_.Get();
  // A picture in COMMON is promoted to a readable state by the list and falls back after it.
  const bool move_colour = copy_colour_ ? frame.colour_state != D3D12_RESOURCE_STATE_COPY_SOURCE
                                        : frame.colour_state != D3D12_RESOURCE_STATE_COMMON && (frame.colour_state & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0;
  const D3D12_RESOURCE_STATES colour_to = copy_colour_ ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  D3D12_RESOURCE_BARRIER barriers[3];
  UINT n = 0;
  if (move_colour) barriers[n++] = Transition(frame.colour, frame.colour_state, colour_to);
  if (copy_colour_) barriers[n++] = Transition(copy_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
  barriers[n++] = Transition(slot.input.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  prep->ResourceBarrier(n, barriers);
  if (copy_colour_) {
    prep->CopyResource(copy_.Get(), frame.colour);
    const D3D12_RESOURCE_BARRIER read = Transition(copy_.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    prep->ResourceBarrier(1, &read);
  }
  ID3D12DescriptorHeap* const heaps[] = {heap_.Get()};
  prep->SetDescriptorHeaps(1, heaps);
  prep->SetComputeRootSignature(prep_root_.Get());
  prep->SetComputeRootDescriptorTable(0, Gpu(heap_.Get(), descriptor_size_, base));
  prep->SetComputeRootDescriptorTable(1, Gpu(heap_.Get(), descriptor_size_, base + 1));
  prep->Dispatch((flow_width_ + 7) / 8, (flow_height_ + 7) / 8, 1);
  n = 0;
  barriers[n++] = Transition(slot.input.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
  if (move_colour) barriers[n++] = Transition(frame.colour, colour_to, frame.colour_state);
  prep->ResourceBarrier(n, barriers);
  if (FAILED(prep->Close())) {
    Fail("HSF's list for the grey image would not close");
    return nullptr;
  }
  ID3D12CommandList* const lists[] = {prep};
  queue->ExecuteCommandLists(1, lists);
  queue->Signal(prep_fence_.Get(), ++prep_value_);
  slot.free_at = done_value_ + 1;
  ++count_;

  const int reference = previous_;
  previous_ = static_cast<int>(s);
  if (reference < 0) {
    // Nothing to match against yet: the flow starts with the next picture.
    ++skipped_;
    return nullptr;
  }

  // Forward is input to reference: this picture to the last one, which is the way DLSS-G's motion points.
  NV_OF_FENCE_POINT after_prep{prep_fence_.Get(), prep_value_};
  NV_OF_EXECUTE_INPUT_PARAMS_D3D12 input{};
  input.inputFrame = static_cast<NvOFGPUBufferHandle>(slot.input_handle);
  input.referenceFrame = static_cast<NvOFGPUBufferHandle>(slots_[reference].input_handle);
  input.disableTemporalHints = NV_OF_FALSE;
  input.numFencePoints = 1;
  input.fencePoint = &after_prep;
  NV_OF_FENCE_POINT when_done{flow_fence_.Get(), flow_value_ + 1};
  NV_OF_EXECUTE_OUTPUT_PARAMS_D3D12 output{};
  output.outputBuffer = static_cast<NvOFGPUBufferHandle>(forward_handle_);
  output.outputCostBuffer = static_cast<NvOFGPUBufferHandle>(forward_cost_handle_);
  output.bwdOutputBuffer = static_cast<NvOFGPUBufferHandle>(backward_handle_);
  output.bwdOutputCostBuffer = static_cast<NvOFGPUBufferHandle>(backward_cost_handle_);
  output.fencePoint = &when_done;
  const NV_OF_STATUS status = Api(api_).nvOFExecuteD3D12(static_cast<NvOFHandle>(session_), &input, &output);
  if (status != NV_OF_SUCCESS) {
    Fail("the engine refused a frame (%d); the effect's flow is used for the rest of the session", static_cast<int>(status));
    return nullptr;
  }
  ++flow_value_;
  flow_sent_ms_ = GetTickCount64();
  slots_[reference].free_at = done_value_ + 1;
  // The caller's list, which reads the answer, runs after it is there. A wait on the GPU: no thread stops.
  queue->Wait(flow_fence_.Get(), flow_value_);

  // The answer to motion, on the caller's list.
  const D3D12_RESOURCE_DESC depth = frame.depth != nullptr ? frame.depth->GetDesc() : D3D12_RESOURCE_DESC{};
  const bool use_depth = frame.depth != nullptr && depth.Format == DXGI_FORMAT_R32_FLOAT && depth.Width != 0 && depth.Height != 0;
  const D3D12_SHADER_RESOURCE_VIEW_DESC depth_view = Srv(DXGI_FORMAT_R32_FLOAT);
  device->CreateShaderResourceView(use_depth ? frame.depth : nullptr, &depth_view, Cpu(heap_.Get(), descriptor_size_, base + 6));
  const D3D12_RESOURCE_BARRIER begin[] = {Transition(forward_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                          Transition(backward_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                          Transition(forward_cost_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                          Transition(backward_cost_.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
                                          Transition(out_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
  list->ResourceBarrier(static_cast<UINT>(std::size(begin)), begin);
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(convert_root_.Get());
  list->SetPipelineState(convert_pso_.Get());
  list->SetComputeRootDescriptorTable(0, Gpu(heap_.Get(), descriptor_size_, base + 2));
  list->SetComputeRootDescriptorTable(1, Gpu(heap_.Get(), descriptor_size_, base + 7));
  struct Constants {
    UINT width, height, flow_width, flow_height;
    UINT grid;
    float consistency, cost_threshold, cost_scale;
    UINT use_depth, depth_width, depth_height, depth_reversed;
    float depth_sensitivity, repair, to_texel[2];
  } constants{};
  static_assert(sizeof(Constants) == CONVERT_CONSTANTS * 4);
  constants.width = out_width_;
  constants.height = out_height_;
  constants.flow_width = flow_width_;
  constants.flow_height = flow_height_;
  constants.grid = grid_;
  constants.consistency = std::max(0.25f, AGREE_PIXELS_AT_1080 * static_cast<float>(out_height_) / 1080.f);
  constants.cost_threshold = COST_THRESHOLD;
  constants.cost_scale = cost_format_ == DXGI_FORMAT_R8_UINT ? 1.f / 255.f : 1.f / 65535.f;
  constants.use_depth = use_depth ? 1u : 0u;
  constants.depth_width = use_depth ? static_cast<UINT>(depth.Width) : 1u;
  constants.depth_height = use_depth ? depth.Height : 1u;
  constants.depth_reversed = frame.depth_inverted ? 1u : 0u;
  constants.depth_sensitivity = DEPTH_SENSITIVITY;
  constants.repair = REPAIR;
  // Pixels of the motion texture to its texel value: pixels / size is the share of the picture, and the caller's scale turns a texel into that share.
  constants.to_texel[0] = frame.scale[0] != 0.f ? 1.f / (static_cast<float>(out_width_) * frame.scale[0]) : 0.f;
  constants.to_texel[1] = frame.scale[1] != 0.f ? 1.f / (static_cast<float>(out_height_) * frame.scale[1]) : 0.f;
  list->SetComputeRoot32BitConstants(2, CONVERT_CONSTANTS, &constants, 0);
  list->Dispatch((out_width_ + 7) / 8, (out_height_ + 7) / 8, 1);
  const D3D12_RESOURCE_BARRIER end[] = {Transition(forward_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                                        Transition(backward_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                                        Transition(forward_cost_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                                        Transition(backward_cost_.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
                                        Transition(out_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
  list->ResourceBarrier(static_cast<UINT>(std::size(end)), end);
  state_ = State::active;
  ++made_;
  return out_.Get();
}

}  // namespace hsf::nvof
