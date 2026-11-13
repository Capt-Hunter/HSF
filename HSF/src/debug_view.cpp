// See debug_view.h.
#include "debug_view.h"

#include <d3dcompiler.h>

#include <cstring>
#include <iterator>

#include "compose.h"

using Microsoft::WRL::ComPtr;

namespace hsf::debugview {
namespace {

constexpr UINT DESCRIPTORS_PER_FRAME = 3;  // motion, depth, out

// One thread per pixel of the picture. Each reads the matching place in the part of motion and depth in use.
constexpr char SHADER[] = R"(
Texture2D<float2> motion : register(t0);
Texture2D<float> depth : register(t1);
RWTexture2D<float4> picture : register(u0);
cbuffer c : register(b0) {
  uint2 out_size;
  uint2 in_offset;
  uint2 in_size;
  float2 scale;
  uint kind;
  uint inverted;
};
[numthreads(8, 8, 1)]
void main(uint2 id : SV_DispatchThreadID) {
  if (any(id >= out_size)) return;
  const uint2 at = in_offset + min(uint2((float2(id) + 0.5) * float2(in_size) / float2(out_size)), in_size - 1);
  float3 colour;
  if (kind == 1) {
    // Nearness, about near plane over distance either way round; the root spreads it so both near and far are visible.
    const float d = depth[at];
    const float closeness = inverted != 0 ? d : 1.0 - d;
    colour = pow(saturate(closeness), 0.3).xxx;
  } else {
    const float2 m = motion[at] * scale;
    colour = float3(saturate(0.5 + m * 20.0), 0.5);
  }
  picture[id] = float4(colour, 1.0);
}
)";

DXGI_FORMAT Typeless(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_R10G10B10A2_UNORM: return DXGI_FORMAT_R10G10B10A2_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default: return format;
  }
}

DXGI_FORMAT Writable(DXGI_FORMAT format) {
  switch (Typeless(format)) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return format;
  }
}

DXGI_FORMAT ReadMotion(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    default: return format;
  }
}

DXGI_FORMAT ReadDepth(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_UNORM;
    default: return format;
  }
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = resource;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  list->ResourceBarrier(1, &b);
}

}  // namespace

bool Pass::Init(ID3D12Device* device, DXGI_FORMAT back_format, UINT width, UINT height, const char*& what, HRESULT& hr) {
  hr = S_OK;
  device_ = device;
  width_ = width;
  height_ = height;
  view_format_ = Writable(back_format);

  what = "the picture's format has no typed UAV store";
  D3D12_FEATURE_DATA_FORMAT_SUPPORT support{view_format_, D3D12_FORMAT_SUPPORT1_NONE, D3D12_FORMAT_SUPPORT2_NONE};
  if (FAILED(hr = device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support)))) return false;
  if ((support.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW) == 0 || (support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE) == 0) {
    hr = E_NOTIMPL;
    return false;
  }

  what = "compiling the shader";
  ComPtr<ID3DBlob> code, messages;
  if (FAILED(hr = D3DCompile(SHADER, sizeof(SHADER) - 1, "hsf_debug_view", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages))) return false;

  what = "the root signature";
  const D3D12_DESCRIPTOR_RANGE ranges[] = {{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 2, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND},
                                           {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND}};
  D3D12_ROOT_PARAMETER params[2] = {};
  params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  params[0].DescriptorTable.NumDescriptorRanges = static_cast<UINT>(std::size(ranges));
  params[0].DescriptorTable.pDescriptorRanges = ranges;
  params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  params[1].Constants.Num32BitValues = 10;
  const D3D12_ROOT_SIGNATURE_DESC root_desc{static_cast<UINT>(std::size(params)), params, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
  // From the loaded d3d12.dll, as in compose.cpp.
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

  what = "the descriptor heap";
  const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, DESCRIPTORS_PER_FRAME * compose::FRAMES, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
  if (FAILED(hr = device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&heap_)))) return false;
  descriptor_size_ = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

  // Same family as the back buffer, so it copies straight over it.
  what = "the view's texture";
  const D3D12_HEAP_PROPERTIES heap_props{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Format = Typeless(back_format);
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  if (FAILED(hr = device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&out_)))) return false;
  what = nullptr;
  return true;
}

void Pass::Record(ID3D12GraphicsCommandList* list, UINT frame, Kind kind, ID3D12Resource* motion, ID3D12Resource* depth, UINT x, UINT y, UINT width, UINT height,
                  bool depth_inverted, const float scale[2], ID3D12Resource* back_buffer, D3D12_RESOURCE_STATES back_state) {
  const D3D12_RESOURCE_DESC motion_desc = motion->GetDesc();
  const D3D12_RESOURCE_DESC depth_desc = depth->GetDesc();
  if (width == 0 || height == 0) {
    x = y = 0;
    width = static_cast<UINT>(motion_desc.Width);
    height = motion_desc.Height;
  }

  D3D12_CPU_DESCRIPTOR_HANDLE cpu = heap_->GetCPUDescriptorHandleForHeapStart();
  cpu.ptr += static_cast<SIZE_T>(frame) * DESCRIPTORS_PER_FRAME * descriptor_size_;
  D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
  srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
  srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  srv.Texture2D.MipLevels = 1;
  srv.Format = ReadMotion(motion_desc.Format);
  device_->CreateShaderResourceView(motion, &srv, cpu);
  cpu.ptr += descriptor_size_;
  srv.Format = ReadDepth(depth_desc.Format);
  device_->CreateShaderResourceView(depth, &srv, cpu);
  cpu.ptr += descriptor_size_;
  D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};
  uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
  uav.Format = view_format_;
  device_->CreateUnorderedAccessView(out_.Get(), nullptr, &uav, cpu);

  D3D12_GPU_DESCRIPTOR_HANDLE gpu = heap_->GetGPUDescriptorHandleForHeapStart();
  gpu.ptr += static_cast<UINT64>(frame) * DESCRIPTORS_PER_FRAME * descriptor_size_;
  ID3D12DescriptorHeap* const heaps[] = {heap_.Get()};
  list->SetDescriptorHeaps(1, heaps);
  list->SetComputeRootSignature(root_.Get());
  list->SetPipelineState(pso_.Get());
  list->SetComputeRootDescriptorTable(0, gpu);
  UINT constants[10] = {width_, height_, x, y, width, height, 0, 0, static_cast<UINT>(kind), depth_inverted ? 1u : 0u};
  static_assert(sizeof(float) == sizeof(UINT));
  memcpy(&constants[6], &scale[0], sizeof(float));
  memcpy(&constants[7], &scale[1], sizeof(float));
  list->SetComputeRoot32BitConstants(1, 10, constants, 0);
  list->Dispatch((width_ + 7) / 8, (height_ + 7) / 8, 1);

  Barrier(list, out_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Barrier(list, back_buffer, back_state, D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyResource(back_buffer, out_.Get());
  Barrier(list, back_buffer, D3D12_RESOURCE_STATE_COPY_DEST, back_state);
  Barrier(list, out_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
}

}  // namespace hsf::debugview
