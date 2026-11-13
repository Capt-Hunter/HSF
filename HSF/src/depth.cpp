#include "depth.h"

#include "compute_state.h"

#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace hsf::depth {
namespace {

// The read-back looks at this many points across the picture.
constexpr UINT SAMPLES_X = 64, SAMPLES_Y = 36;
constexpr UINT VIEW_WIDTH = 240;
// Less range than this is one value with noise. 24-bit depth steps are 6e-8, so a real scene's range is far above it even with all of it near far.
constexpr float FLAT_RANGE = 1e-5f;
// Read-backs in a row (4 a second in a game) before a buffer that has only ever been flat is called out in Status::flat_for_long.
constexpr uint32_t FLAT_FOR_LONG_READS = 40;
// The median has to be clearly on the other side before a reversed / normal decision changes.
constexpr float MEDIAN_REVERSED = 0.4f, MEDIAN_NORMAL = 0.6f;
// Upside down: the depth's edges have to line up this much better mirrored, smoothed over about 10 read-backs and after at least this many. A scene
// that looks the same both ways (a level horizon) decides nothing.
constexpr float MIRRORED_BETTER = 1.25f, EDGES_KEEP = 0.9f, EDGES_LEAST = 1e-5f;
constexpr uint32_t EDGES_MIN_READS = 8;
// Upside down, second test: the ground is near at the bottom of nearly every 3D picture. The copy's top quarter has to be nearer than its bottom
// quarter by this share of the depth's range, read after read, FLOOR_VOTES more times than the other way round (about 2 s), before it flips.
// Edges alone miss it where the picture has edges everywhere (grass, foliage), which line up about as well mirrored.
constexpr float FLOOR_GAP = 0.15f;
constexpr int FLOOR_VOTES = 8;

// DEPTH_MS is the only difference: Load takes the sample as a second argument on a multisampled texture. Sample 0 is a real surface; an average of
// samples at an object's edge would be a depth where nothing is.
constexpr char COPY_SHADER[] = R"(
#ifdef DEPTH_MS
Texture2DMS<float> game_depth : register(t0);
#else
Texture2D<float> game_depth : register(t0);
#endif
RWTexture2D<float> copy : register(u0);
cbuffer Copy : register(b0) { float2 to_source; float2 size; float source_height; float flip; float2 offset; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= (uint)size.x || id.y >= (uint)size.y) return;
  // offset: where the depth's picture starts in the copy (0 unless fitted). Outside it, the nearest edge of the depth.
  int2 at = clamp(int2(floor((id.xy + 0.5 - offset) * to_source)), int2(0, 0), int2(round((size - 2.0 * offset) * to_source)) - 1);
  if (flip != 0) at.y = (int)source_height - 1 - at.y;
#ifdef DEPTH_MS
  copy[id.xy] = game_depth.Load(at, 0);
#else
  copy[id.xy] = game_depth.Load(int3(at, 0));
#endif
}
)";

// A game's vectors into the middle of the copy (offset: where they start; outside them, no motion), turned into UV of the whole copy.
constexpr char FIT_MOTION_SHADER[] = R"(
Texture2D<float2> game_motion : register(t0);
RWTexture2D<float2> fitted : register(u0);
cbuffer Fit : register(b0) { float2 to_source; float2 size; float2 offset; float2 to_uv; float2 base; float smooth; float unused; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= (uint)size.x || id.y >= (uint)size.y) return;
  const float2 at_cover = id.xy + 0.5 - offset;
  const float2 cover = size - 2.0 * offset;
  if (any(at_cover < 0.0) || any(at_cover >= cover)) {
    fitted[id.xy] = float2(0.0, 0.0);
    return;
  }
  const int2 last = int2(round(cover * to_source)) - 1;
  if (smooth == 0.0) {
    fitted[id.xy] = game_motion.Load(int3(int2(base) + min(int2(floor(at_cover * to_source)), last), 0)) * to_uv;
    return;
  }
  // A small source scaled up (the flow): blended between its four nearest texels.
  const float2 at = clamp(at_cover * to_source - 0.5, 0.0, float2(last));
  const int2 a = int2(floor(at));
  const int2 b = min(a + 1, last);
  const float2 t = at - float2(a);
  const float2 top = lerp(game_motion.Load(int3(int2(base) + a, 0)), game_motion.Load(int3(int2(base) + int2(b.x, a.y), 0)), t.x);
  const float2 bottom = lerp(game_motion.Load(int3(int2(base) + int2(a.x, b.y), 0)), game_motion.Load(int3(int2(base) + b, 0)), t.x);
  fitted[id.xy] = lerp(top, bottom, t.y) * to_uv;
}
)";

constexpr char VIEW_SHADER[] = R"(
Texture2D<float> depth : register(t0);
RWTexture2D<float4> view : register(u0);
cbuffer View : register(b0) { float2 to_source; float2 size; float low; float high; float near_is_high; float unused; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= (uint)size.x || id.y >= (uint)size.y) return;
  float t = saturate((depth.Load(int3(int2(floor((id.xy + 0.5) * to_source)), 0)) - low) / max(high - low, 1e-7));
  if (near_is_high == 0) t = 1 - t;
  view[id.xy] = float4(t, t, t, 1);
}
)";

// Per cell of a coarse grid: how much the depth and the picture both change here, with the depth as it is and mirrored top to bottom. Mirroring only moves
// the depth's edges around, so the two sums compare fairly: whichever way the edges land on the picture's edges is the right way up.
constexpr char EDGE_SHADER[] = R"(
Texture2D<float> depth : register(t0);
Texture2D<float4> picture : register(t1);
RWTexture2D<float2> cells : register(u0);
cbuffer Edges : register(b0) { float2 depth_size; float2 picture_size; float2 grid; float to_edge; float unused; };
float D(float2 uv) { return depth.Load(int3(int2(saturate(uv) * (depth_size - 1)), 0)); }
float L(float2 uv) { return dot(saturate(picture.Load(int3(int2(saturate(uv) * (picture_size - 1)), 0)).rgb), float3(0.299, 0.587, 0.114)); }
float DepthEdge(float2 uv, float2 step) { return saturate((abs(D(uv + float2(step.x, 0)) - D(uv - float2(step.x, 0))) + abs(D(uv + float2(0, step.y)) - D(uv - float2(0, step.y)))) * to_edge); }
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= (uint)grid.x || id.y >= (uint)grid.y) return;
  const float2 uv = (id.xy + 0.5) / grid, step = 0.5 / grid;
  const float light = saturate((abs(L(uv + float2(step.x, 0)) - L(uv - float2(step.x, 0))) + abs(L(uv + float2(0, step.y)) - L(uv - float2(0, step.y)))) * 4);
  cells[id.xy] = float2(light * DepthEdge(uv, step), light * DepthEdge(float2(uv.x, 1 - uv.y), step));
}
)";

struct CopyConstants {
  float to_source[2];
  float size[2];
  float source_height, flip, offset[2];
};

struct EdgeConstants {
  float depth_size[2];
  float picture_size[2];
  float grid[2];
  float to_edge, unused;
};

struct FitConstants {
  float to_source[2];
  float size[2];
  float offset[2];
  float to_uv[2];
  float base[2];
  float smooth, unused;
};

struct ViewConstants {
  float to_source[2];
  float size[2];
  float low, high, near_is_high, unused;
};

struct State {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11ComputeShader> copy_shader, copy_ms_shader, view_shader, edge_shader;
  ComPtr<ID3D11Buffer> copy_constants, view_constants, edge_constants;
  ComPtr<ID3D11Texture2D> picture, cells, cells_staging;
  ComPtr<ID3D11ShaderResourceView> picture_srv;
  ComPtr<ID3D11UnorderedAccessView> cells_uav;
  D3D11_TEXTURE2D_DESC picture_desc{};
  bool edges_pending = false;  // this read-back's edges are on the way
  float low = 0, high = 0;     // the depth's range at the last read-back, without outliers
  double edges_same = 0, edges_mirrored = 0;
  uint32_t edge_reads = 0;
  int floor_votes = 0;  // FloorVote: + the top of the copy is nearer than its bottom, - the other way
  bool floor_decided = false;  // the floor test reached FLOOR_VOTES either way: the edge test no longer flips
  bool found_upside_down = false;
  ComPtr<ID3D11Texture2D> copy, staging, view_tex;
  ComPtr<ID3D11ShaderResourceView> copy_srv, view_srv;
  ComPtr<ID3D11UnorderedAccessView> copy_uav, view_uav;
  UINT width = 0, height = 0, view_height = 0;
  ComPtr<ID3D11ComputeShader> fit_shader;  // FitMotion, made on its first call
  ComPtr<ID3D11Buffer> fit_constants;
  ComPtr<ID3D11Texture2D> fitted;
  ComPtr<ID3D11UnorderedAccessView> fitted_uav;
  UINT fitted_width = 0, fitted_height = 0;
  bool reading = false;
  ULONGLONG next_read = 0;
  uint32_t flat_reads = 0;   // read-backs in a row that were flat
  bool seen_depth = false;   // some read-back on this device was not flat
  Status status;
};

State g_s;
bool g_failed_device = false;
ID3D11Device* g_failed_on = nullptr;  // only compared, never used
uint32_t g_period_ms = 250;
int g_upside_down_setting = -1;
bool g_fit_picture = false;
// Shapes closer than this are the same shape: a depth buffer a few rows taller than the picture (1088 for 1080) is stretched, not fitted.
constexpr float SAME_SHAPE = 0.03f;
char g_problem[200] = "";

void SetProblem(const char* text, HRESULT hr = S_OK) {
  if (hr != S_OK)
    std::snprintf(g_problem, sizeof(g_problem), "%s (0x%08lx)", text, static_cast<unsigned long>(hr));
  else
    std::snprintf(g_problem, sizeof(g_problem), "%s", text);
}

HRESULT MakeShader(ID3D11Device* device, const char* source, size_t size, const D3D_SHADER_MACRO* defines, ComPtr<ID3D11ComputeShader>& shader) {
  ComPtr<ID3DBlob> code, messages;
  HRESULT hr = D3DCompile(source, size, "hsf_depth", defines, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);
  if (FAILED(hr)) {
    SetProblem(messages != nullptr ? static_cast<const char*>(messages->GetBufferPointer()) : "the depth shader did not compile", hr);
    return hr;
  }
  hr = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader);
  if (FAILED(hr)) SetProblem("the depth shader", hr);
  return hr;
}

bool MakeDevice(ID3D11Device* device) {
  if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
    SetProblem("the game's device is below feature level 11_0: no compute shader with these views");
    return false;
  }
  const D3D_SHADER_MACRO multisampled[] = {{"DEPTH_MS", "1"}, {nullptr, nullptr}};
  if (FAILED(MakeShader(device, COPY_SHADER, sizeof(COPY_SHADER) - 1, nullptr, g_s.copy_shader)) ||
      FAILED(MakeShader(device, COPY_SHADER, sizeof(COPY_SHADER) - 1, multisampled, g_s.copy_ms_shader)) ||
      FAILED(MakeShader(device, VIEW_SHADER, sizeof(VIEW_SHADER) - 1, nullptr, g_s.view_shader)) ||
      FAILED(MakeShader(device, EDGE_SHADER, sizeof(EDGE_SHADER) - 1, nullptr, g_s.edge_shader)))
    return false;
  const D3D11_BUFFER_DESC copy{sizeof(CopyConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  const D3D11_BUFFER_DESC view{sizeof(ViewConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  const D3D11_BUFFER_DESC edges{sizeof(EdgeConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  HRESULT hr = device->CreateBuffer(&copy, nullptr, &g_s.copy_constants);
  if (SUCCEEDED(hr)) hr = device->CreateBuffer(&view, nullptr, &g_s.view_constants);
  if (SUCCEEDED(hr)) hr = device->CreateBuffer(&edges, nullptr, &g_s.edge_constants);
  if (FAILED(hr)) {
    SetProblem("the depth constants", hr);
    return false;
  }
  g_s.device = device;
  device->GetImmediateContext(&g_s.context);
  return true;
}

// The copy, its read-back and the debug view at this size.
bool MakeTextures(UINT width, UINT height) {
  if (g_s.copy != nullptr && g_s.width == width && g_s.height == height) return true;
  g_s.copy.Reset();
  g_s.staging.Reset();
  g_s.view_tex.Reset();
  g_s.copy_srv.Reset();
  g_s.view_srv.Reset();
  g_s.copy_uav.Reset();
  g_s.view_uav.Reset();
  g_s.cells.Reset();
  g_s.cells_staging.Reset();
  g_s.cells_uav.Reset();
  g_s.edges_pending = false;
  g_s.reading = false;
  g_s.status.measured = false;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = width;
  desc.Height = height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_R32_FLOAT;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
  HRESULT hr = g_s.device->CreateTexture2D(&desc, nullptr, &g_s.copy);
  if (SUCCEEDED(hr)) hr = g_s.device->CreateShaderResourceView(g_s.copy.Get(), nullptr, &g_s.copy_srv);
  if (SUCCEEDED(hr)) hr = g_s.device->CreateUnorderedAccessView(g_s.copy.Get(), nullptr, &g_s.copy_uav);
  D3D11_TEXTURE2D_DESC staging = desc;
  staging.Usage = D3D11_USAGE_STAGING;
  staging.BindFlags = 0;
  staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (SUCCEEDED(hr)) hr = g_s.device->CreateTexture2D(&staging, nullptr, &g_s.staging);
  D3D11_TEXTURE2D_DESC view = desc;
  view.Width = std::min(VIEW_WIDTH, width);
  view.Height = std::max(1u, static_cast<UINT>(static_cast<UINT64>(view.Width) * height / width));
  view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  if (SUCCEEDED(hr)) hr = g_s.device->CreateTexture2D(&view, nullptr, &g_s.view_tex);
  if (SUCCEEDED(hr)) hr = g_s.device->CreateShaderResourceView(g_s.view_tex.Get(), nullptr, &g_s.view_srv);
  if (SUCCEEDED(hr)) hr = g_s.device->CreateUnorderedAccessView(g_s.view_tex.Get(), nullptr, &g_s.view_uav);
  D3D11_TEXTURE2D_DESC cells = view;
  cells.Format = DXGI_FORMAT_R32G32_FLOAT;
  cells.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  if (SUCCEEDED(hr)) hr = g_s.device->CreateTexture2D(&cells, nullptr, &g_s.cells);
  if (SUCCEEDED(hr)) hr = g_s.device->CreateUnorderedAccessView(g_s.cells.Get(), nullptr, &g_s.cells_uav);
  cells.Usage = D3D11_USAGE_STAGING;
  cells.BindFlags = 0;
  cells.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (SUCCEEDED(hr)) hr = g_s.device->CreateTexture2D(&cells, nullptr, &g_s.cells_staging);
  if (FAILED(hr)) {
    g_s.copy.Reset();
    SetProblem("the depth copy", hr);
    return false;
  }
  g_s.width = width;
  g_s.height = height;
  g_s.view_height = view.Height;
  return true;
}

// What the game's view is. False if it is not something the shaders can read.
bool ReadSource(ID3D11ShaderResourceView* game_depth, bool& multisampled, UINT& width, UINT& height) {
  D3D11_SHADER_RESOURCE_VIEW_DESC view{};
  game_depth->GetDesc(&view);
  if (view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D && view.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2DMS) {
    SetProblem("the game's depth view is not a plain 2D texture");
    return false;
  }
  multisampled = view.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMS;
  ComPtr<ID3D11Resource> resource;
  ComPtr<ID3D11Texture2D> texture;
  game_depth->GetResource(&resource);
  if (resource == nullptr || FAILED(resource.As(&texture))) {
    SetProblem("the game's depth view has no 2D texture");
    return false;
  }
  D3D11_TEXTURE2D_DESC desc{};
  texture->GetDesc(&desc);
  width = desc.Width;
  height = desc.Height;
  g_s.status.width = desc.Width;
  g_s.status.height = desc.Height;
  g_s.status.samples = desc.SampleDesc.Count;
  g_s.status.format = view.Format;
  return true;
}

void DrawView(float low, float high, bool near_is_high) {
  const ViewConstants values{{static_cast<float>(g_s.width) / static_cast<float>(std::min(VIEW_WIDTH, g_s.width)),
                              static_cast<float>(g_s.height) / static_cast<float>(g_s.view_height)},
                             {static_cast<float>(std::min(VIEW_WIDTH, g_s.width)), static_cast<float>(g_s.view_height)},
                             low,
                             high,
                             near_is_high ? 1.f : 0.f,
                             0.f};
  g_s.context->UpdateSubresource(g_s.view_constants.Get(), 0, nullptr, &values, 0, 0);
  ID3D11Buffer* const constants = g_s.view_constants.Get();
  ID3D11ShaderResourceView* const view = g_s.copy_srv.Get();
  ID3D11UnorderedAccessView* const target = g_s.view_uav.Get();
  g_s.context->CSSetShader(g_s.view_shader.Get(), nullptr, 0);
  g_s.context->CSSetConstantBuffers(0, 1, &constants);
  g_s.context->CSSetShaderResources(0, 1, &view);
  g_s.context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
  g_s.context->Dispatch((std::min(VIEW_WIDTH, g_s.width) + 7) / 8, (g_s.view_height + 7) / 8, 1);
  ID3D11ShaderResourceView* const no_view = nullptr;
  ID3D11UnorderedAccessView* const no_target = nullptr;
  g_s.context->CSSetShaderResources(0, 1, &no_view);
  g_s.context->CSSetUnorderedAccessViews(0, 1, &no_target, nullptr);
}

// The picture's copy for this frame's size and format; false if it cannot be compared.
bool MakePicture(ID3D11Texture2D* picture) {
  D3D11_TEXTURE2D_DESC source{};
  picture->GetDesc(&source);
  if (source.SampleDesc.Count != 1 || source.ArraySize != 1) return false;
  const DXGI_FORMAT format = ReadFormat(source.Format);
  if (g_s.picture != nullptr && g_s.picture_desc.Width == source.Width && g_s.picture_desc.Height == source.Height && g_s.picture_desc.Format == format) return true;
  g_s.picture.Reset();
  g_s.picture_srv.Reset();
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = source.Width;
  desc.Height = source.Height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  HRESULT hr = g_s.device->CreateTexture2D(&desc, nullptr, &g_s.picture);
  if (SUCCEEDED(hr)) hr = g_s.device->CreateShaderResourceView(g_s.picture.Get(), nullptr, &g_s.picture_srv);
  if (FAILED(hr)) {
    g_s.picture.Reset();
    return false;
  }
  g_s.picture_desc = desc;
  return true;
}

void MeasureEdges(ID3D11Texture2D* picture) {
  if (picture == nullptr || !g_s.status.measured || g_s.status.flat || !MakePicture(picture)) return;
  g_s.context->CopySubresourceRegion(g_s.picture.Get(), 0, 0, 0, 0, picture, 0, nullptr);
  const UINT grid_width = std::min(VIEW_WIDTH, g_s.width);
  const EdgeConstants values{{static_cast<float>(g_s.width), static_cast<float>(g_s.height)},
                             {static_cast<float>(g_s.picture_desc.Width), static_cast<float>(g_s.picture_desc.Height)},
                             {static_cast<float>(grid_width), static_cast<float>(g_s.view_height)},
                             1.f / std::max(g_s.high - g_s.low, 1e-6f),
                             0.f};
  g_s.context->UpdateSubresource(g_s.edge_constants.Get(), 0, nullptr, &values, 0, 0);
  ID3D11Buffer* const constants = g_s.edge_constants.Get();
  ID3D11ShaderResourceView* const views[] = {g_s.copy_srv.Get(), g_s.picture_srv.Get()};
  ID3D11UnorderedAccessView* const target = g_s.cells_uav.Get();
  g_s.context->CSSetShader(g_s.edge_shader.Get(), nullptr, 0);
  g_s.context->CSSetConstantBuffers(0, 1, &constants);
  g_s.context->CSSetShaderResources(0, 2, views);
  g_s.context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
  g_s.context->Dispatch((grid_width + 7) / 8, (g_s.view_height + 7) / 8, 1);
  ID3D11ShaderResourceView* const no_views[2] = {};
  ID3D11UnorderedAccessView* const no_target = nullptr;
  g_s.context->CSSetShaderResources(0, 2, no_views);
  g_s.context->CSSetUnorderedAccessViews(0, 1, &no_target, nullptr);
  g_s.context->CopyResource(g_s.cells_staging.Get(), g_s.cells.Get());
  g_s.edges_pending = true;
}

// Takes one measurement of the edges in; if the depth lines up clearly better mirrored, the copy flips. False: not ready yet.
bool ReadEdges() {
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(g_s.context->Map(g_s.cells_staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) return false;
  const UINT grid_width = std::min(VIEW_WIDTH, g_s.width);
  double same = 0, mirrored = 0;
  for (UINT y = 0; y < g_s.view_height; ++y) {
    const float* const row = reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
    for (UINT x = 0; x < grid_width; ++x) {
      same += row[x * 2];
      mirrored += row[x * 2 + 1];
    }
  }
  g_s.context->Unmap(g_s.cells_staging.Get(), 0);
  const double cells = static_cast<double>(grid_width) * g_s.view_height;
  g_s.edges_same = g_s.edges_same * EDGES_KEEP + same / cells * (1.0 - EDGES_KEEP);
  g_s.edges_mirrored = g_s.edges_mirrored * EDGES_KEEP + mirrored / cells * (1.0 - EDGES_KEEP);
  ++g_s.edge_reads;
  g_s.status.edges_same = static_cast<float>(g_s.edges_same);
  g_s.status.edges_mirrored = static_cast<float>(g_s.edges_mirrored);
  // Once the floor test has decided, it alone decides: in Valheim the two disagreed and flipped the depth back and forth during play, each flip
  // a LaunchPad recompile (a hitch) and an upside-down depth for DLSS-G.
  if (!g_s.floor_decided && g_s.edge_reads >= EDGES_MIN_READS && g_s.edges_mirrored > EDGES_LEAST && g_s.edges_mirrored > g_s.edges_same * MIRRORED_BETTER) {
    g_s.found_upside_down = !g_s.found_upside_down;
    g_s.edges_same = g_s.edges_mirrored = 0;
    g_s.edge_reads = 0;
  }
  return true;
}

// Never waits: the read-back is tried again next frame. The caller holds the saved compute state.
void ReadBack() {
  if (g_s.edges_pending) {
    if (!ReadEdges()) return;
    g_s.edges_pending = false;
  }
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(g_s.context->Map(g_s.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) return;
  std::vector<float> values, top, bottom;
  values.reserve(SAMPLES_X * SAMPLES_Y);
  for (UINT y = 0; y < SAMPLES_Y; ++y) {
    const UINT row = static_cast<UINT>((y + 0.5) * g_s.height / SAMPLES_Y);
    const float* const texels = reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData) + row * mapped.RowPitch);
    for (UINT x = 0; x < SAMPLES_X; ++x) {
      const float value = texels[static_cast<UINT>((x + 0.5) * g_s.width / SAMPLES_X)];
      if (!std::isfinite(value)) continue;
      values.push_back(value);
      if (x < SAMPLES_X / 8 || x >= SAMPLES_X - SAMPLES_X / 8) continue;  // the middle three quarters: a HUD or a frame often sits at the sides
      if (y < SAMPLES_Y / 4) top.push_back(value);
      else if (y >= SAMPLES_Y - SAMPLES_Y / 4) bottom.push_back(value);
    }
  }
  g_s.context->Unmap(g_s.staging.Get(), 0);
  g_s.reading = false;
  g_s.next_read = GetTickCount64() + g_period_ms;
  if (values.empty()) return;
  std::sort(values.begin(), values.end());
  Status& s = g_s.status;
  s.lowest = values.front();
  s.highest = values.back();
  s.median = values[values.size() / 2];
  s.flat = s.highest - s.lowest < FLAT_RANGE;
  if (s.flat) {
    ++g_s.flat_reads;
  } else {
    g_s.flat_reads = 0;
    g_s.seen_depth = true;
  }
  s.flat_for_long = !g_s.seen_depth && g_s.flat_reads >= FLAT_FOR_LONG_READS;
  // The first decision takes whichever side the median is on; after that it has to cross the gap, so a scene near the middle does not flip it.
  if (!s.flat) {
    if (!s.measured)
      s.inverted = s.median < 0.5f;
    else if (s.median < MEDIAN_REVERSED)
      s.inverted = true;
    else if (s.median > MEDIAN_NORMAL)
      s.inverted = false;
  }
  s.measured = true;
  g_s.low = values[values.size() / 50];
  g_s.high = values[values.size() - 1 - values.size() / 50];
  if (!s.flat && g_upside_down_setting < 0 && !top.empty() && !bottom.empty()) {
    const auto median = [](std::vector<float>& v) {
      std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
      return v[v.size() / 2];
    };
    // Nearer = higher when reversed (far = 0), lower when normal.
    const float gap = (median(top) - median(bottom)) * (s.inverted ? 1.f : -1.f), range = std::max(g_s.high - g_s.low, 1e-6f);
    if (gap > range * FLOOR_GAP) ++g_s.floor_votes;
    else if (gap < -range * FLOOR_GAP) g_s.floor_votes = std::max(g_s.floor_votes - 1, -FLOOR_VOTES);
    if (g_s.floor_votes <= -FLOOR_VOTES) g_s.floor_decided = true;  // the floor agrees with the way up it has now
    if (g_s.floor_votes >= FLOOR_VOTES) {
      g_s.floor_decided = true;
      g_s.found_upside_down = !g_s.found_upside_down;
      g_s.floor_votes = 0;
      g_s.edges_same = g_s.edges_mirrored = 0;
      g_s.edge_reads = 0;
    }
  }
  DrawView(g_s.low, g_s.high, s.inverted);
}

}  // namespace

ID3D11Texture2D* Frame(ID3D11Device* device, ID3D11ShaderResourceView* game_depth, UINT width, UINT height, ID3D11Texture2D* picture) {
  if (device == nullptr || width == 0 || height == 0) return nullptr;
  if (g_s.device.Get() != device) {
    if (g_failed_device && g_failed_on == device) return nullptr;
    Shutdown();
    if (!MakeDevice(device)) {
      g_failed_device = true;
      g_failed_on = device;
      const Status none;
      g_s = State{};
      g_s.status = none;
      return nullptr;
    }
    g_failed_device = false;
    g_problem[0] = '\0';
  }
  if (game_depth == nullptr) {
    g_s.status.source = false;
    g_s.status.measured = false;
    g_s.status.flat_for_long = false;
    g_s.flat_reads = 0;
    return nullptr;
  }
  bool multisampled = false;
  UINT source_width = 0, source_height = 0;
  if (!ReadSource(game_depth, multisampled, source_width, source_height) || !MakeTextures(width, height)) {
    g_s.status.source = false;
    g_s.status.measured = false;
    g_s.status.flat_for_long = false;
    g_s.flat_reads = 0;
    return nullptr;
  }
  g_problem[0] = '\0';
  g_s.status.source = true;
  const bool upside_down = g_upside_down_setting >= 0 ? g_upside_down_setting != 0 : g_s.found_upside_down;
  g_s.status.upside_down = upside_down;

  // The part of the copy the depth covers: all of it, or (fitted) the depth's own shape in the middle.
  float cover_w = static_cast<float>(width), cover_h = static_cast<float>(height);
  const float source_shape = static_cast<float>(source_width) / static_cast<float>(source_height), copy_shape = cover_w / cover_h;
  g_s.status.fitted = g_fit_picture && std::fabs(source_shape / copy_shape - 1.f) > SAME_SHAPE;
  if (g_s.status.fitted) {
    if (source_shape < copy_shape)
      cover_w = cover_h * source_shape;  // bars at the sides
    else
      cover_h = cover_w / source_shape;  // bars above and below
  }
  SavedComputeState saved(g_s.context.Get());
  const CopyConstants values{{static_cast<float>(source_width) / cover_w, static_cast<float>(source_height) / cover_h},
                             {static_cast<float>(width), static_cast<float>(height)},
                             static_cast<float>(source_height),
                             upside_down ? 1.f : 0.f,
                             {(static_cast<float>(width) - cover_w) * 0.5f, (static_cast<float>(height) - cover_h) * 0.5f}};
  g_s.context->UpdateSubresource(g_s.copy_constants.Get(), 0, nullptr, &values, 0, 0);
  ID3D11Buffer* const constants = g_s.copy_constants.Get();
  ID3D11UnorderedAccessView* const target = g_s.copy_uav.Get();
  g_s.context->CSSetShader(multisampled ? g_s.copy_ms_shader.Get() : g_s.copy_shader.Get(), nullptr, 0);
  g_s.context->CSSetConstantBuffers(0, 1, &constants);
  g_s.context->CSSetShaderResources(0, 1, &game_depth);
  g_s.context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
  g_s.context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
  ID3D11ShaderResourceView* const no_view = nullptr;
  ID3D11UnorderedAccessView* const no_target = nullptr;
  g_s.context->CSSetShaderResources(0, 1, &no_view);
  g_s.context->CSSetUnorderedAccessViews(0, 1, &no_target, nullptr);

  if (g_s.reading) {
    ReadBack();
  } else if (GetTickCount64() >= g_s.next_read) {
    g_s.context->CopyResource(g_s.staging.Get(), g_s.copy.Get());
    if (g_upside_down_setting < 0) MeasureEdges(picture);
    g_s.reading = true;
  }
  return g_s.status.measured && !g_s.status.flat ? g_s.copy.Get() : nullptr;
}

Status Current() { return g_s.status; }

ID3D11ShaderResourceView* View() { return g_s.status.measured ? g_s.view_srv.Get() : nullptr; }

const char* Problem() { return g_problem; }

void SetPeriod(uint32_t ms) { g_period_ms = ms; }

void SetUpsideDown(int setting) { g_upside_down_setting = setting; }

void SetFitPicture(bool on) { g_fit_picture = on; }

ID3D11Texture2D* FitMotion(ID3D11Device* device, ID3D11ShaderResourceView* motion, UINT x, UINT y, UINT used_width, UINT used_height, const float to_pixels[2],
                           UINT width, UINT height, bool smooth) {
  if (device == nullptr || motion == nullptr || used_width == 0 || used_height == 0 || width == 0 || height == 0) return nullptr;
  if (g_s.device.Get() != device) {  // Frame makes the device's state; until it has run, nothing here
    if (g_s.device != nullptr || !MakeDevice(device)) return nullptr;
  }
  if (g_s.fit_shader == nullptr) {
    const D3D11_BUFFER_DESC constants{sizeof(FitConstants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    if (FAILED(MakeShader(device, FIT_MOTION_SHADER, sizeof(FIT_MOTION_SHADER) - 1, nullptr, g_s.fit_shader)) ||
        FAILED(device->CreateBuffer(&constants, nullptr, &g_s.fit_constants))) {
      g_s.fit_shader.Reset();
      return nullptr;
    }
  }
  if (g_s.fitted == nullptr || g_s.fitted_width != width || g_s.fitted_height != height) {
    g_s.fitted.Reset();
    g_s.fitted_uav.Reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &g_s.fitted)) || FAILED(device->CreateUnorderedAccessView(g_s.fitted.Get(), nullptr, &g_s.fitted_uav))) {
      g_s.fitted.Reset();
      return nullptr;
    }
    g_s.fitted_width = width;
    g_s.fitted_height = height;
  }
  float cover_w = static_cast<float>(width), cover_h = static_cast<float>(height);
  const float source_shape = static_cast<float>(used_width) / static_cast<float>(used_height), copy_shape = cover_w / cover_h;
  if (std::fabs(source_shape / copy_shape - 1.f) > SAME_SHAPE) {
    if (source_shape < copy_shape)
      cover_w = cover_h * source_shape;
    else
      cover_h = cover_w / source_shape;
  }
  // Pixels of the part in use, over its size, is UV of the cover; the cover's share of the copy makes it UV of the copy.
  const FitConstants values{{static_cast<float>(used_width) / cover_w, static_cast<float>(used_height) / cover_h},
                            {static_cast<float>(width), static_cast<float>(height)},
                            {(static_cast<float>(width) - cover_w) * 0.5f, (static_cast<float>(height) - cover_h) * 0.5f},
                            {to_pixels[0] / static_cast<float>(used_width) * cover_w / static_cast<float>(width),
                             to_pixels[1] / static_cast<float>(used_height) * cover_h / static_cast<float>(height)},
                            {static_cast<float>(x), static_cast<float>(y)},
                            smooth ? 1.f : 0.f,
                            0.f};
  SavedComputeState saved(g_s.context.Get());
  g_s.context->UpdateSubresource(g_s.fit_constants.Get(), 0, nullptr, &values, 0, 0);
  ID3D11Buffer* const constants = g_s.fit_constants.Get();
  ID3D11UnorderedAccessView* const target = g_s.fitted_uav.Get();
  g_s.context->CSSetShader(g_s.fit_shader.Get(), nullptr, 0);
  g_s.context->CSSetConstantBuffers(0, 1, &constants);
  g_s.context->CSSetShaderResources(0, 1, &motion);
  g_s.context->CSSetUnorderedAccessViews(0, 1, &target, nullptr);
  g_s.context->Dispatch((width + 7) / 8, (height + 7) / 8, 1);
  ID3D11ShaderResourceView* const no_view = nullptr;
  ID3D11UnorderedAccessView* const no_target = nullptr;
  g_s.context->CSSetShaderResources(0, 1, &no_view);
  g_s.context->CSSetUnorderedAccessViews(0, 1, &no_target, nullptr);
  return g_s.fitted.Get();
}

void Shutdown() {
  g_s = State{};
  g_problem[0] = '\0';
}

}  // namespace hsf::depth
