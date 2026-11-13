// FP16 (scRGB) frames into DLSS-G's R10G10B10A2 swap chain (hdr_out.h).
#include "hdr_out.h"

#include <d3dcompiler.h>

#include <iterator>

namespace hsf::hdr_out {

namespace {

using Microsoft::WRL::ComPtr;

// The matrices and curves are RenoDX's (MIT, Carlos Lopez Jr., commit 216fc64): BT709_TO_XYZ_MAT and XYZ_TO_BT2020_MAT from
// src/shaders/color/rgb.hlsl, pq::Encode from color/pq.hlsl, srgb::Encode from color/srgb.hlsl.
const char SHADER[] = R"(
Texture2D<float4> Source : register(t0);
RWTexture2D<float4> Out : register(u0);
cbuffer Constants : register(b0) { uint2 Size; uint Pq; uint Unused; };

static const float3x3 BT709_TO_XYZ_MAT = float3x3(
    0.4123907993f, 0.3575843394f, 0.1804807884f,
    0.2126390059f, 0.7151686788f, 0.0721923154f,
    0.0193308187f, 0.1191947798f, 0.9505321522f);
static const float3x3 XYZ_TO_BT2020_MAT = float3x3(
    1.7166511880f, -0.3556707838f, -0.2533662814f,
    -0.6666843518f, 1.6164812366f, 0.0157685458f,
    0.0176398574f, -0.0427706133f, 0.9421031212f);
static const float3x3 BT709_TO_BT2020_MAT = mul(XYZ_TO_BT2020_MAT, BT709_TO_XYZ_MAT);

static const float M1 = 2610.f / 16384.f;
static const float M2 = 128.f * (2523.f / 4096.f);
static const float C1 = 3424.f / 4096.f;
static const float C2 = 32.f * (2413.f / 4096.f);
static const float C3 = 32.f * (2392.f / 4096.f);

float3 PqEncode(float3 color, float scaling) {
  color *= (scaling / 10000.f);
  float3 y_m1 = pow(color, M1);
  return pow((C1 + C2 * y_m1) / (1.f + C3 * y_m1), M2);
}

float3 SrgbEncode(float3 c) {
  return (c <= 0.0031308f) ? c * 12.92f : mad(1.055f, pow(c, 1.f / 2.4f), -0.055f);
}

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (any(id.xy >= Size)) return;
  float3 c = Source.Load(int3(id.xy, 0)).rgb;
  // NaN or infinity from the game would light whole blocks after DLSS-G's filtering.
  c = (c == c && abs(c) < 65504.f) ? c : 0.f;
  float3 o = Pq != 0 ? PqEncode(max(0.f, mul(BT709_TO_BT2020_MAT, c)), 80.f) : SrgbEncode(saturate(c));
  Out[id.xy] = float4(saturate(o), 1.f);
}
)";

}  // namespace

bool Pass::Init(ID3D12Device* device, DXGI_FORMAT source_format, UINT width, UINT height, UINT frames, const char*& what, HRESULT& hr) {
  device_ = device;
  format_ = source_format;
  width_ = width;
  height_ = height;
  frames_ = frames;
  what = "the HDR10 shader's root signature";
  const D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
                                           {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND}};
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = static_cast<UINT>(std::size(ranges));
  params[0].DescriptorTable.pDescriptorRanges = ranges;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.Num32BitValues = 4;
  const D3D12_ROOT_SIGNATURE_DESC root{static_cast<UINT>(std::size(params)), params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  // From the loaded d3d12.dll, as frame_checks.cpp does: the add-on must not import it.
  const auto serialize = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE>(GetProcAddress(GetModuleHandleW(L"d3d12.dll"), "D3D12SerializeRootSignature"));
  if (serialize == nullptr) {
    hr = E_NOINTERFACE;
    return false;
  }
  ComPtr<ID3DBlob> code, messages;
  if (FAILED(hr = serialize(&root, D3D_ROOT_SIGNATURE_VERSION_1, &code, &messages))) return false;
  if (FAILED(hr = device->CreateRootSignature(0, code->GetBufferPointer(), code->GetBufferSize(), IID_PPV_ARGS(&root_)))) return false;
  what = "the HDR10 shader";
  code.Reset();
  messages.Reset();
  if (FAILED(hr = D3DCompile(SHADER, sizeof(SHADER) - 1, "hsf_hdr_out", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages))) return false;
  D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
  pso.pRootSignature = root_.Get();
  pso.CS = {code->GetBufferPointer(), code->GetBufferSize()};
  if (FAILED(hr = device->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pso_)))) return false;
  what = "the HDR10 shader's descriptors";
  const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, DESCRIPTORS_PER_PASS * PASSES_PER_FRAME * frames, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (FAILED(hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_)))) return false;
  descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
  what = nullptr;
  return true;
}

void Pass::Record(ID3D12GraphicsCommandList* list, UINT frame, UINT which, ID3D12Resource* source, ID3D12Resource* out, bool pq) {
  const SIZE_T first = (static_cast<SIZE_T>(frame % frames_) * PASSES_PER_FRAME + (which % PASSES_PER_FRAME)) * DESCRIPTORS_PER_PASS;
  D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += first * descriptor_size_;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  srv.Format = format_;
  device_->CreateShaderResourceView(source, &srv, cpu);
  cpu.ptr += descriptor_size_;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  uav.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  device_->CreateUnorderedAccessView(out, nullptr, &uav, cpu);

  ID3D12DescriptorHeap* const heaps[] = {heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(root_.Get());
  list->SetPipelineState(pso_.Get());
  D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += static_cast<UINT64>(first) * descriptor_size_;
  list->SetComputeRootDescriptorTable(0, gpu);
  const UINT constants[4] = {width_, height_, pq ? 1u : 0u, 0u};
  list->SetComputeRoot32BitConstants(1, 4, constants, 0);
  list->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);
}

}  // namespace hsf::hdr_out
