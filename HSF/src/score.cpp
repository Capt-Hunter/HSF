#include "score.h"

#include "compute_state.h"

#include <d3dcompiler.h>
#include <windows.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace hsf::score {
namespace {

// Grid width in cells; the height follows the frame's shape. 240 x 135 patches of 9 samples, a few times a second, is a very small GPU cost.
constexpr UINT GRID_WIDTH = 240;
constexpr size_t HISTORY = 128;
// A patch counts as changed from about 2 levels in 255: below that it is noise and dither.
constexpr float MOVING_ERROR = 2.f / 255.f;
// "Clearly" better or worse: 10% either way.
constexpr float CLEARLY = 0.1f;
// Fewer changed patches than this: the frames are the same picture, nothing to rate.
constexpr float STILL_SHARE = 0.01f;
// A cut: the two frames' brightness histograms differ by over this share of the picture, and the motion explains little of the change. A pan or bad
// motion vectors leave the histogram about the same, so they are rated and not excused as cuts. Both are first guesses, to tune in play.
constexpr int HISTOGRAM_BINS = 32;
constexpr float CUT_HISTOGRAM = 0.35f;
constexpr float CUT_GAIN = 0.2f;
// Fewer pairs or less of the picture changing than this in a window: shown as too little to rate.
constexpr uint32_t MIN_PAIRS = 3;
constexpr float MIN_MOVING = 0.1f;

constexpr char SCORE_SHADER[] = R"(
Texture2D<float4> before_frame : register(t0);
Texture2D<float4> after_frame : register(t1);
Texture2D<float4> motion : register(t2);
SamplerState linear_clamp : register(s0);
SamplerState point_clamp : register(s1);
RWTexture2D<float4> cells : register(u0);
RWTexture2D<float4> view_motion : register(u1);
RWTexture2D<float4> view_error : register(u2);
RWTexture2D<float4> view_better : register(u3);
cbuffer Score : register(b0) { float2 to_pixels; float2 cell_size; float2 frame_size; float2 grid_size; };
float Luma(float3 c) { return dot(saturate(c), float3(0.299, 0.587, 0.114)); }
float3 Hue(float h) { return saturate(abs(frac(h + float3(0, 2.0 / 3.0, 1.0 / 3.0)) * 6 - 3) - 1); }
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID) {
  if (id.x >= (uint)grid_size.x || id.y >= (uint)grid_size.y) return;
  // The patch's centre on a real pixel of the frame, so the frames themselves are not blurred by the look-up.
  const float2 q = floor((id.xy + 0.5) * cell_size) + 0.5;
  float2 v = motion.SampleLevel(point_clamp, q / frame_size, 0).xy * to_pixels;
  if (!all(abs(v) < 1e5)) v = 0;
  float warp = 0, none = 0;
  [unroll] for (int y = -1; y <= 1; ++y)
    [unroll] for (int x = -1; x <= 1; ++x) {
      const float2 p = q + float2(x, y) * 2;
      const float after_luma = Luma(after_frame.SampleLevel(point_clamp, p / frame_size, 0).rgb);
      warp += abs(after_luma - Luma(before_frame.SampleLevel(linear_clamp, (p + v) / frame_size, 0).rgb));
      none += abs(after_luma - Luma(before_frame.SampleLevel(point_clamp, p / frame_size, 0).rgb));
    }
  warp /= 9;
  none /= 9;
  cells[id.xy] = float4(warp, none, Luma(after_frame.SampleLevel(point_clamp, q / frame_size, 0).rgb), Luma(before_frame.SampleLevel(point_clamp, q / frame_size, 0).rgb));
  // Motion: colour = direction, brightness = speed (full at 32 pixels per frame).
  const float speed = length(v);
  view_motion[id.xy] = float4(Hue(atan2(v.y, v.x) / 6.2831853 + 0.5) * saturate(speed / 32), 1);
  // Error left after the warp: black, red, yellow, white (full at 64 levels in 255).
  const float heat = saturate(warp * 4) * 3;
  view_error[id.xy] = float4(saturate(heat), saturate(heat - 1), saturate(heat - 2), 1);
  // Green = the motion helped this patch, red = it hurt, dark = nothing changed here or about the same. Brighter = more.
  const float change = (none - warp) * 8;
  view_better[id.xy] = float4(saturate(-change), saturate(change), 0, 1);
}
)";

struct Constants {
  float to_pixels[2];
  float cell_size[2];
  float frame_size[2];
  float grid_size[2];
};

struct Sample {
  ULONGLONG time = 0;
  float warp = 0, none = 0, better = 0, worse = 0, moving = 0;
  uint32_t kind = 0;  // 0 = counts, 1 = still, 2 = cut
};

// One candidate's copy of its motion vectors, results and views.
struct Slot {
  ComPtr<ID3D11Texture2D> motion, cells, staging, view_tex[3];
  ComPtr<ID3D11ShaderResourceView> motion_srv, view_srv[3];
  ComPtr<ID3D11UnorderedAccessView> cells_uav, view_uav[3];
  D3D11_TEXTURE2D_DESC motion_desc{};
  bool measured = false;  // this pair's dispatch ran, its result is on the way
  bool has_view = false;
  Sample history[HISTORY];
  size_t written = 0;
};

enum class Stage { wait, before_taken, reading };

struct State {
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11ComputeShader> shader;
  ComPtr<ID3D11Buffer> constants;
  ComPtr<ID3D11SamplerState> linear, point;
  ComPtr<ID3D11Query> done;
  ComPtr<ID3D11Texture2D> before, after;
  ComPtr<ID3D11ShaderResourceView> before_srv, after_srv;
  UINT width = 0, height = 0, grid_width = 0, grid_height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  Stage stage = Stage::wait;
  ULONGLONG next_pair = 0;
  Slot slots[MAX_INPUTS];
  // ReadView: one copy of a view on its way to the CPU, and the last picture that arrived.
  uint32_t view_serial = 0;  // goes up with each measurement's new views
  ComPtr<ID3D11Texture2D> view_staging;
  bool staged = false;
  int staged_input = -1, staged_view = 0;
  uint32_t staged_serial = 0;
  int shown_input = -1, shown_view = 0;
  uint32_t shown_serial = 0, pixels_serial = 0;
  std::vector<uint32_t> pixels;
  UINT pixels_width = 0, pixels_height = 0;
};

State g_s;
bool g_failed_device = false;  // this device cannot run it; do not try again each frame
ID3D11Device* g_failed_on = nullptr;  // only compared, never used
uint32_t g_period_ms = 250;
char g_problem[160] = "";

void SetProblem(const char* text, HRESULT hr = S_OK) {
  if (hr != S_OK)
    std::snprintf(g_problem, sizeof(g_problem), "%s (0x%08lx)", text, static_cast<unsigned long>(hr));
  else
    std::snprintf(g_problem, sizeof(g_problem), "%s", text);
}

bool MakeDevice(ID3D11Device* device) {
  if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
    SetProblem("the game's device is below feature level 11_0: no compute shader with these views");
    return false;
  }
  ComPtr<ID3DBlob> code, messages;
  HRESULT hr = D3DCompile(SCORE_SHADER, sizeof(SCORE_SHADER) - 1, "hsf_score", nullptr, nullptr, "main", "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &messages);
  if (FAILED(hr)) {
    SetProblem(messages != nullptr ? static_cast<const char*>(messages->GetBufferPointer()) : "the score's shader did not compile", hr);
    return false;
  }
  if (FAILED(hr = device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &g_s.shader))) {
    SetProblem("the score's shader", hr);
    return false;
  }
  const D3D11_BUFFER_DESC buffer{sizeof(Constants), D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
  D3D11_SAMPLER_DESC sampler{};
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  sampler.MaxLOD = D3D11_FLOAT32_MAX;
  hr = device->CreateBuffer(&buffer, nullptr, &g_s.constants);
  if (SUCCEEDED(hr)) hr = device->CreateSamplerState(&sampler, &g_s.linear);
  sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  if (SUCCEEDED(hr)) hr = device->CreateSamplerState(&sampler, &g_s.point);
  const D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
  if (SUCCEEDED(hr)) hr = device->CreateQuery(&query, &g_s.done);
  if (FAILED(hr)) {
    SetProblem("the score's objects", hr);
    return false;
  }
  g_s.device = device;
  device->GetImmediateContext(&g_s.context);
  return true;
}

// The frame copies for this frame's size and format.
bool MakeFrames(const D3D11_TEXTURE2D_DESC& source) {
  const DXGI_FORMAT format = ReadFormat(source.Format);
  if (g_s.before != nullptr && g_s.width == source.Width && g_s.height == source.Height && g_s.format == format) return true;
  g_s.before.Reset();
  g_s.after.Reset();
  g_s.before_srv.Reset();
  g_s.after_srv.Reset();
  for (Slot& slot : g_s.slots) {
    slot.cells.Reset();
    slot.staging.Reset();
    slot.cells_uav.Reset();
    slot.has_view = false;
    for (int i = 0; i < 3; ++i) {
      slot.view_tex[i].Reset();
      slot.view_srv[i].Reset();
      slot.view_uav[i].Reset();
    }
  }
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = source.Width;
  desc.Height = source.Height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  HRESULT hr = g_s.device->CreateTexture2D(&desc, nullptr, &g_s.before);
  if (SUCCEEDED(hr)) hr = g_s.device->CreateTexture2D(&desc, nullptr, &g_s.after);
  if (SUCCEEDED(hr)) hr = g_s.device->CreateShaderResourceView(g_s.before.Get(), nullptr, &g_s.before_srv);
  if (SUCCEEDED(hr)) hr = g_s.device->CreateShaderResourceView(g_s.after.Get(), nullptr, &g_s.after_srv);
  if (FAILED(hr)) {
    g_s.before.Reset();
    SetProblem("the score's frame copies", hr);
    return false;
  }
  g_s.width = source.Width;
  g_s.height = source.Height;
  g_s.format = format;
  g_s.grid_width = std::min(GRID_WIDTH, source.Width);
  g_s.grid_height = std::max(1u, static_cast<UINT>(static_cast<UINT64>(g_s.grid_width) * source.Height / source.Width));
  return true;
}

// The slot's copy of the motion vectors (for this source's size and format) and its results and views at the grid's size.
bool MakeSlot(Slot& slot, ID3D11Texture2D* motion) {
  D3D11_TEXTURE2D_DESC source{};
  motion->GetDesc(&source);
  if (source.SampleDesc.Count != 1 || source.ArraySize != 1) return false;
  HRESULT hr = S_OK;
  const DXGI_FORMAT format = ReadFormat(source.Format);
  if (slot.motion == nullptr || slot.motion_desc.Width != source.Width || slot.motion_desc.Height != source.Height || slot.motion_desc.Format != format) {
    slot.motion.Reset();
    slot.motion_srv.Reset();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = source.Width;
    desc.Height = source.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    hr = g_s.device->CreateTexture2D(&desc, nullptr, &slot.motion);
    if (SUCCEEDED(hr)) hr = g_s.device->CreateShaderResourceView(slot.motion.Get(), nullptr, &slot.motion_srv);
    if (FAILED(hr)) {
      slot.motion.Reset();
      SetProblem("the score's motion vector copy", hr);
      return false;
    }
    slot.motion_desc = desc;
  }
  if (slot.cells != nullptr) return true;
  D3D11_TEXTURE2D_DESC grid{};
  grid.Width = g_s.grid_width;
  grid.Height = g_s.grid_height;
  grid.MipLevels = 1;
  grid.ArraySize = 1;
  grid.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
  grid.SampleDesc.Count = 1;
  grid.Usage = D3D11_USAGE_DEFAULT;
  grid.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
  hr = g_s.device->CreateTexture2D(&grid, nullptr, &slot.cells);
  if (SUCCEEDED(hr)) hr = g_s.device->CreateUnorderedAccessView(slot.cells.Get(), nullptr, &slot.cells_uav);
  D3D11_TEXTURE2D_DESC staging = grid;
  staging.Usage = D3D11_USAGE_STAGING;
  staging.BindFlags = 0;
  staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
  if (SUCCEEDED(hr)) hr = g_s.device->CreateTexture2D(&staging, nullptr, &slot.staging);
  D3D11_TEXTURE2D_DESC view = grid;
  view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  view.BindFlags = D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE;
  for (int i = 0; i < 3 && SUCCEEDED(hr); ++i) {
    hr = g_s.device->CreateTexture2D(&view, nullptr, &slot.view_tex[i]);
    if (SUCCEEDED(hr)) hr = g_s.device->CreateUnorderedAccessView(slot.view_tex[i].Get(), nullptr, &slot.view_uav[i]);
    if (SUCCEEDED(hr)) hr = g_s.device->CreateShaderResourceView(slot.view_tex[i].Get(), nullptr, &slot.view_srv[i]);
  }
  if (FAILED(hr)) {
    slot.cells.Reset();
    SetProblem("the score's results", hr);
    return false;
  }
  return true;
}

void Measure(const Input* inputs) {
  SavedComputeState saved(g_s.context.Get());
  ID3D11SamplerState* const samplers[] = {g_s.linear.Get(), g_s.point.Get()};
  g_s.context->CSSetShader(g_s.shader.Get(), nullptr, 0);
  g_s.context->CSSetSamplers(0, 2, samplers);
  ID3D11Buffer* const constants = g_s.constants.Get();
  g_s.context->CSSetConstantBuffers(0, 1, &constants);
  for (int i = 0; i < MAX_INPUTS; ++i) {
    Slot& slot = g_s.slots[i];
    slot.measured = false;
    const Input& input = inputs[i];
    if (input.motion == nullptr || (input.to_pixels[0] == 0.f && input.to_pixels[1] == 0.f) || !MakeSlot(slot, input.motion)) continue;
    g_s.context->CopySubresourceRegion(slot.motion.Get(), 0, 0, 0, 0, input.motion, 0, nullptr);
    const Constants values{{input.to_pixels[0], input.to_pixels[1]},
                           {static_cast<float>(g_s.width) / static_cast<float>(g_s.grid_width), static_cast<float>(g_s.height) / static_cast<float>(g_s.grid_height)},
                           {static_cast<float>(g_s.width), static_cast<float>(g_s.height)},
                           {static_cast<float>(g_s.grid_width), static_cast<float>(g_s.grid_height)}};
    g_s.context->UpdateSubresource(g_s.constants.Get(), 0, nullptr, &values, 0, 0);
    ID3D11ShaderResourceView* const views[] = {g_s.before_srv.Get(), g_s.after_srv.Get(), slot.motion_srv.Get()};
    ID3D11UnorderedAccessView* const targets[] = {slot.cells_uav.Get(), slot.view_uav[0].Get(), slot.view_uav[1].Get(), slot.view_uav[2].Get()};
    g_s.context->CSSetShaderResources(0, 3, views);
    g_s.context->CSSetUnorderedAccessViews(0, 4, targets, nullptr);
    g_s.context->Dispatch((g_s.grid_width + 7) / 8, (g_s.grid_height + 7) / 8, 1);
    ID3D11ShaderResourceView* const no_views[3] = {};
    ID3D11UnorderedAccessView* const no_targets[4] = {};
    g_s.context->CSSetShaderResources(0, 3, no_views);
    g_s.context->CSSetUnorderedAccessViews(0, 4, no_targets, nullptr);
    g_s.context->CopyResource(slot.staging.Get(), slot.cells.Get());
    slot.measured = true;
    slot.has_view = true;
    ++g_s.view_serial;
  }
  g_s.context->End(g_s.done.Get());
}

// Never waits: tells the driver not to flush and not to block, and tries again next frame.
bool ReadResults() {
  if (g_s.context->GetData(g_s.done.Get(), nullptr, 0, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return false;
  const ULONGLONG now = GetTickCount64();
  for (Slot& slot : g_s.slots) {
    if (!slot.measured) continue;
    slot.measured = false;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(g_s.context->Map(slot.staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped))) continue;
    double warp = 0, none = 0;
    uint32_t moving = 0, better = 0, worse = 0;
    uint32_t histogram_after[HISTOGRAM_BINS] = {}, histogram_before[HISTOGRAM_BINS] = {};
    for (UINT y = 0; y < g_s.grid_height; ++y) {
      const float* const row = reinterpret_cast<const float*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch);
      for (UINT x = 0; x < g_s.grid_width; ++x) {
        const float cell_warp = row[x * 4], cell_none = row[x * 4 + 1];
        ++histogram_after[std::clamp(static_cast<int>(row[x * 4 + 2] * HISTOGRAM_BINS), 0, HISTOGRAM_BINS - 1)];
        ++histogram_before[std::clamp(static_cast<int>(row[x * 4 + 3] * HISTOGRAM_BINS), 0, HISTOGRAM_BINS - 1)];
        warp += cell_warp;
        none += cell_none;
        if (cell_none >= MOVING_ERROR) {
          ++moving;
          if (cell_warp < cell_none * (1.f - CLEARLY)) ++better;
          if (cell_warp > cell_none * (1.f + CLEARLY)) ++worse;
        }
      }
    }
    g_s.context->Unmap(slot.staging.Get(), 0);
    const double cells = static_cast<double>(g_s.grid_width) * g_s.grid_height;
    Sample sample;
    sample.time = now;
    sample.warp = static_cast<float>(warp / cells);
    sample.none = static_cast<float>(none / cells);
    sample.moving = static_cast<float>(moving / cells);
    sample.better = moving != 0 ? static_cast<float>(better) / static_cast<float>(moving) : 0.f;
    sample.worse = moving != 0 ? static_cast<float>(worse) / static_cast<float>(moving) : 0.f;
    const float gain = sample.none > 0.f ? 1.f - sample.warp / sample.none : 0.f;
    double histogram_difference = 0;
    for (int bin = 0; bin < HISTOGRAM_BINS; ++bin) histogram_difference += std::abs(static_cast<double>(histogram_after[bin]) - histogram_before[bin]);
    const float histogram_change = static_cast<float>(histogram_difference / 2.0 / cells);
    if (sample.moving < STILL_SHARE)
      sample.kind = 1;
    else if (histogram_change > CUT_HISTOGRAM && gain < CUT_GAIN)
      sample.kind = 2;
    slot.history[slot.written++ % HISTORY] = sample;
  }
  return true;
}

}  // namespace

void Frame(ID3D11Device* device, ID3D11Texture2D* frame, const Input* inputs) {
  if (device == nullptr || frame == nullptr) return;
  if (g_s.device.Get() != device) {
    if (g_failed_device && g_failed_on == device) return;
    Shutdown();
    if (!MakeDevice(device)) {
      g_failed_device = true;
      g_failed_on = device;
      g_s = State{};
      return;
    }
    g_failed_device = false;
    g_problem[0] = '\0';
  }
  D3D11_TEXTURE2D_DESC desc{};
  frame->GetDesc(&desc);
  if (desc.SampleDesc.Count != 1 || desc.ArraySize != 1) {
    SetProblem("the frame is multisampled or an array");
    g_s.stage = Stage::wait;
    return;
  }
  if (g_s.stage == Stage::reading) {
    if (!ReadResults()) return;
    g_s.stage = Stage::wait;
    g_s.next_pair = GetTickCount64() + g_period_ms;
  }
  const bool size_changed = g_s.width != desc.Width || g_s.height != desc.Height || g_s.format != ReadFormat(desc.Format);
  if (size_changed) g_s.stage = Stage::wait;
  if (g_s.stage == Stage::wait) {
    if (GetTickCount64() < g_s.next_pair) return;
    if (!MakeFrames(desc)) return;
    g_s.context->CopySubresourceRegion(g_s.before.Get(), 0, 0, 0, 0, frame, 0, nullptr);
    g_s.stage = Stage::before_taken;
    return;
  }
  // The very next finished frame: its motion vectors lead back to the frame copied last time.
  g_s.context->CopySubresourceRegion(g_s.after.Get(), 0, 0, 0, 0, frame, 0, nullptr);
  Measure(inputs);
  g_s.stage = Stage::reading;
}

Result Recent(int input, uint32_t window_ms) {
  Result result;
  if (input < 0 || input >= MAX_INPUTS) return result;
  const Slot& slot = g_s.slots[input];
  const ULONGLONG now = GetTickCount64();
  double warp = 0, none = 0, better = 0, worse = 0, moving = 0;
  const size_t count = std::min(slot.written, HISTORY);
  for (size_t i = 0; i < count; ++i) {
    const Sample& sample = slot.history[(slot.written - 1 - i) % HISTORY];
    if (now - sample.time > window_ms) break;
    if (sample.kind == 1) { ++result.still; continue; }
    if (sample.kind == 2) { ++result.cuts; continue; }
    ++result.pairs;
    warp += sample.warp;
    none += sample.none;
    better += sample.better;
    worse += sample.worse;
    moving += sample.moving;
  }
  if (result.pairs == 0) return result;
  const double pairs = result.pairs;
  result.valid = true;
  result.gain = none > 0 ? static_cast<float>(1.0 - warp / none) : 0.f;
  result.error_warp = static_cast<float>(warp / pairs * 255.0);
  result.error_none = static_cast<float>(none / pairs * 255.0);
  result.better = static_cast<float>(better / pairs);
  result.worse = static_cast<float>(worse / pairs);
  result.moving = static_cast<float>(moving / pairs);
  // AC3 logged -279% from one pair with 8% of the picture changing: a ratio of two tiny errors, not a rating.
  result.thin = result.pairs < MIN_PAIRS || result.moving < MIN_MOVING;
  return result;
}

ID3D11ShaderResourceView* ViewOf(int input, View view) {
  if (input < 0 || input >= MAX_INPUTS || view == View::off || view == View::depth) return nullptr;
  const Slot& slot = g_s.slots[input];
  return slot.has_view ? slot.view_srv[static_cast<int>(view) - 1].Get() : nullptr;
}

uint32_t ReadView(int input, View view, const uint32_t** pixels, UINT* width, UINT* height) {
  if (input < 0 || input >= MAX_INPUTS || view == View::off || view == View::depth || g_s.context == nullptr) return 0;
  const int v = static_cast<int>(view);
  if (g_s.staged) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const HRESULT hr = g_s.context->Map(g_s.view_staging.Get(), 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped);
    if (hr != DXGI_ERROR_WAS_STILL_DRAWING) {
      g_s.staged = false;
      if (SUCCEEDED(hr)) {
        D3D11_TEXTURE2D_DESC desc{};
        g_s.view_staging->GetDesc(&desc);
        g_s.pixels.resize(static_cast<size_t>(desc.Width) * desc.Height);
        for (UINT y = 0; y < desc.Height; ++y)
          std::memcpy(g_s.pixels.data() + static_cast<size_t>(y) * desc.Width, static_cast<const char*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                      desc.Width * 4);
        g_s.context->Unmap(g_s.view_staging.Get(), 0);
        g_s.pixels_width = desc.Width;
        g_s.pixels_height = desc.Height;
        g_s.shown_input = g_s.staged_input;
        g_s.shown_view = g_s.staged_view;
        g_s.shown_serial = g_s.staged_serial;
        ++g_s.pixels_serial;
      }
    }
  }
  const Slot& slot = g_s.slots[input];
  const bool newer = g_s.shown_input != input || g_s.shown_view != v || g_s.shown_serial != g_s.view_serial;
  if (!g_s.staged && slot.has_view && newer) {
    ID3D11Texture2D* const source = slot.view_tex[v - 1].Get();
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC have{};
    if (g_s.view_staging != nullptr) g_s.view_staging->GetDesc(&have);
    if (g_s.view_staging == nullptr || have.Width != desc.Width || have.Height != desc.Height) {
      g_s.view_staging.Reset();
      desc.Usage = D3D11_USAGE_STAGING;
      desc.BindFlags = 0;
      desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
      desc.MiscFlags = 0;
      g_s.device->CreateTexture2D(&desc, nullptr, &g_s.view_staging);
    }
    if (g_s.view_staging != nullptr) {
      g_s.context->CopyResource(g_s.view_staging.Get(), source);
      g_s.staged = true;
      g_s.staged_input = input;
      g_s.staged_view = v;
      g_s.staged_serial = g_s.view_serial;
    }
  }
  if (g_s.shown_input != input || g_s.shown_view != v || g_s.pixels.empty()) return 0;
  *pixels = g_s.pixels.data();
  *width = g_s.pixels_width;
  *height = g_s.pixels_height;
  return g_s.pixels_serial;
}

const char* Problem() { return g_problem; }

void SetPeriod(uint32_t ms) { g_period_ms = ms; }

void Shutdown() {
  g_s = State{};
  g_problem[0] = '\0';
}

}  // namespace hsf::score
