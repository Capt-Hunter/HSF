// See ngx_input.h.
#include "ngx_input.h"

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#include "sl_game.h"

using Microsoft::WRL::ComPtr;

namespace hsf::ngx {
namespace {

constexpr int NGX_SUCCESS = 1;
constexpr int FLAG_MV_LOW_RES = 1 << 1, FLAG_MV_JITTERED = 1 << 2, FLAG_DEPTH_INVERTED = 1 << 3;
constexpr D3D12_RESOURCE_STATES READABLE = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
// NGX's rule for what it reads, which is the state the game's textures are in around the call.
constexpr D3D12_RESOURCE_STATES GAME_STATE = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

LogFunction g_log = nullptr;
void Line(const char* format, ...) {
  if (g_log == nullptr) return;
  char text[400];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  g_log(text);
}

using EvaluateFn = int(__cdecl*)(ID3D12GraphicsCommandList*, const void*, const Parameter*, void*);
using CreateFn = int(__cdecl*)(ID3D12GraphicsCommandList*, int, const Parameter*, void**);
EvaluateFn g_real_evaluate = nullptr, g_real_evaluate_c = nullptr;
CreateFn g_real_create = nullptr;

struct Copies {
  ComPtr<ID3D12Resource> motion, depth;
  UINT64 width = 0;
  UINT height = 0;
  DXGI_FORMAT motion_format = DXGI_FORMAT_UNKNOWN;
};

SRWLOCK g_lock = SRWLOCK_INIT;  // guards everything from here to the hooks
Copies g_copies;
Frame g_frame;
bool g_have_frame = false;
uint64_t g_serial = 0;
struct OldCopies {
  Copies copies;
  uint64_t serial;
};
std::vector<OldCopies> g_old;  // still used by frames on the GPU when replaced
// From the last CreateFeature for DLSS, which is the only sure place for these. -1: not seen.
int g_create_flags = -1;
uint32_t g_create_width = 0, g_create_height = 0, g_create_out_width = 0, g_create_out_height = 0;
// A game can call DLSS for something small too, such as a mirror: the largest recent call is the main one.
uint64_t g_largest_area = 0, g_largest_serial = 0;
uint32_t g_refused_format = 0;

struct Lock {
  Lock() { AcquireSRWLockExclusive(&g_lock); }
  ~Lock() { ReleaseSRWLockExclusive(&g_lock); }
};

// The format HSF's copy of the vectors has: Streamline needs to be told a real one, and a copy within the same family works.
DXGI_FORMAT TypedMotion(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
    default: return format;
  }
}

// Depth whose first plane can be copied straight to R32_FLOAT. D24 cannot: that game stays on flow.
bool DepthCopies(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
      return true;
    default:
      return false;
  }
}

bool MakeCopies(ID3D12Device* device, const D3D12_RESOURCE_DESC& motion, const D3D12_RESOURCE_DESC& depth, Copies& out) {
  const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = motion.Width;
  desc.Height = motion.Height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Format = TypedMotion(motion.Format);
  HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, READABLE, nullptr, IID_PPV_ARGS(&out.motion));
  if (SUCCEEDED(hr)) {
    desc.Width = depth.Width;
    desc.Height = depth.Height;
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, READABLE, nullptr, IID_PPV_ARGS(&out.depth));
  }
  if (FAILED(hr)) {
    Line("NGX: copies of %llux%u and %llux%u not made: 0x%08lx", motion.Width, motion.Height, depth.Width, depth.Height, hr);
    return false;
  }
  out.width = motion.Width;
  out.height = motion.Height;
  out.motion_format = motion.Format;
  Line("NGX: vectors %llux%u format %u, depth %llux%u format %u", motion.Width, motion.Height, static_cast<unsigned>(motion.Format), depth.Width, depth.Height, static_cast<unsigned>(depth.Format));
  return true;
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = 0;  // for depth with stencil, its depth plane: the only one read
  barrier.Transition.StateBefore = from;
  barrier.Transition.StateAfter = to;
  list->ResourceBarrier(1, &barrier);
}

// The first mip and plane of source into the whole of copy, with source back as it was.
void CopyOn(ID3D12GraphicsCommandList* list, ID3D12Resource* source, ID3D12Resource* copy) {
  Barrier(list, source, GAME_STATE, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Barrier(list, copy, READABLE, D3D12_RESOURCE_STATE_COPY_DEST);
  D3D12_TEXTURE_COPY_LOCATION to{}, from{};
  to.pResource = copy;
  to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  from.pResource = source;
  from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
  list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
  Barrier(list, copy, D3D12_RESOURCE_STATE_COPY_DEST, READABLE);
  Barrier(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, GAME_STATE);
}

float ReadFloat(const Parameter* parameters, const char* name, float otherwise) {
  float value = 0.f;
  return parameters->Get(name, &value) == NGX_SUCCESS ? value : otherwise;
}
uint32_t ReadUint(const Parameter* parameters, const char* name) {
  unsigned int value = 0;
  return parameters->Get(name, &value) == NGX_SUCCESS ? value : 0;
}

int __cdecl HookCreate(ID3D12GraphicsCommandList* list, int feature, const Parameter* parameters, void** handle) {
  const int result = g_real_create(list, feature, parameters, handle);
  constexpr int FEATURE_SUPER_SAMPLING = 1, FEATURE_RAY_RECONSTRUCTION = 13;
  if (result == NGX_SUCCESS && parameters != nullptr && (feature == FEATURE_SUPER_SAMPLING || feature == FEATURE_RAY_RECONSTRUCTION)) {
    int flags = 0;
    const bool have_flags = parameters->Get("DLSS.Feature.Create.Flags", &flags) == NGX_SUCCESS;
    Lock lock;
    if (have_flags) g_create_flags = flags;
    g_create_width = ReadUint(parameters, "Width");
    g_create_height = ReadUint(parameters, "Height");
    g_create_out_width = ReadUint(parameters, "OutWidth");
    g_create_out_height = ReadUint(parameters, "OutHeight");
    Line("NGX: DLSS made, feature %d, flags 0x%x, render %ux%u, output %ux%u", feature, have_flags ? flags : -1, g_create_width, g_create_height, g_create_out_width, g_create_out_height);
  }
  return result;
}

int __cdecl HookEvaluate(ID3D12GraphicsCommandList* list, const void* handle, const Parameter* parameters, void* callback) {
  const int result = g_real_evaluate(list, handle, parameters, callback);
  if (result == NGX_SUCCESS) Capture(list, parameters);
  return result;
}
int __cdecl HookEvaluateC(ID3D12GraphicsCommandList* list, const void* handle, const Parameter* parameters, void* callback) {
  const int result = g_real_evaluate_c(list, handle, parameters, callback);
  if (result == NGX_SUCCESS) Capture(list, parameters);
  return result;
}

int g_hooked = 0;
bool g_tried = false;

}  // namespace

void Capture(ID3D12GraphicsCommandList* list, const Parameter* parameters) {
  if (list == nullptr || parameters == nullptr) return;
  ID3D12Resource* motion = nullptr;
  ID3D12Resource* depth = nullptr;
  // Only the upscalers have these under these names, so DLSS's own frame generation or anything else on this call never gets here.
  if (parameters->Get("MotionVectors", &motion) != NGX_SUCCESS || parameters->Get("Depth", &depth) != NGX_SUCCESS || motion == nullptr || depth == nullptr) return;
  const D3D12_RESOURCE_DESC motion_desc = motion->GetDesc();
  const D3D12_RESOURCE_DESC depth_desc = depth->GetDesc();
  if (motion_desc.SampleDesc.Count > 1 || depth_desc.SampleDesc.Count > 1) return;

  Lock lock;
  if (!DepthCopies(depth_desc.Format)) {
    if (g_refused_format != static_cast<uint32_t>(depth_desc.Format)) Line("NGX: depth format %u cannot be copied, so this game stays on flow", static_cast<unsigned>(depth_desc.Format));
    g_refused_format = static_cast<uint32_t>(depth_desc.Format);
    return;
  }

  int flags = g_create_flags;
  if (flags < 0 && parameters->Get("DLSS.Feature.Create.Flags", &flags) != NGX_SUCCESS) flags = -1;
  const bool flags_known = flags >= 0;
  const bool low_res = !flags_known || (flags & FLAG_MV_LOW_RES) != 0;
  // The part in use: the render size for low-resolution vectors, otherwise the output.
  uint32_t width = ReadUint(parameters, "DLSS.Render.Subrect.Dimensions.Width");
  uint32_t height = ReadUint(parameters, "DLSS.Render.Subrect.Dimensions.Height");
  if (width == 0 || height == 0) {
    width = g_create_width;
    height = g_create_height;
  }
  if (!low_res && g_create_out_width != 0 && g_create_out_height != 0) {
    width = g_create_out_width;
    height = g_create_out_height;
  }
  if (width == 0 || height == 0 || width > motion_desc.Width || height > motion_desc.Height) {
    width = static_cast<uint32_t>(motion_desc.Width);
    height = motion_desc.Height;
  }

  // The main call: not less than half the largest of the last 240 calls.
  const uint64_t area = static_cast<uint64_t>(width) * height;
  if (g_serial - g_largest_serial > 240) g_largest_area = 0;
  if (area * 2 < g_largest_area) return;
  if (area >= g_largest_area) {
    g_largest_area = area;
    g_largest_serial = g_serial;
  }

  if (g_copies.motion == nullptr || g_copies.width != motion_desc.Width || g_copies.height != motion_desc.Height || g_copies.motion_format != motion_desc.Format ||
      g_copies.depth->GetDesc().Width != depth_desc.Width || g_copies.depth->GetDesc().Height != depth_desc.Height) {
    ComPtr<ID3D12Device> device;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) return;
    Copies made;
    if (!MakeCopies(device.Get(), motion_desc, depth_desc, made)) return;
    if (g_copies.motion != nullptr) g_old.push_back({std::move(g_copies), g_serial});
    g_copies = std::move(made);
  }
  while (!g_old.empty() && g_serial - g_old.front().serial > 16) g_old.erase(g_old.begin());

  CopyOn(list, motion, g_copies.motion.Get());
  CopyOn(list, depth, g_copies.depth.Get());

  Frame f;
  f.motion = g_copies.motion.Get();
  f.depth = g_copies.depth.Get();
  f.x = ReadUint(parameters, "DLSS.Input.MV.Subrect.Base.X");
  f.y = ReadUint(parameters, "DLSS.Input.MV.Subrect.Base.Y");
  if (f.x + width > motion_desc.Width || f.y + height > motion_desc.Height) f.x = f.y = 0;
  f.width = width;
  f.height = height;
  // NGX: multiply by its scale for pixels of the render size. DLSS-G: multiply by its scale for the fraction of the picture.
  f.scale[0] = ReadFloat(parameters, "MV.Scale.X", 1.f) / static_cast<float>(width);
  f.scale[1] = ReadFloat(parameters, "MV.Scale.Y", 1.f) / static_cast<float>(height);
  f.jitter[0] = ReadFloat(parameters, "Jitter.Offset.X", 0.f);
  f.jitter[1] = ReadFloat(parameters, "Jitter.Offset.Y", 0.f);
  f.jittered = flags_known && (flags & FLAG_MV_JITTERED) != 0;
  f.full_res = !low_res;
  f.depth_inverted = !flags_known || (flags & FLAG_DEPTH_INVERTED) != 0;
  int reset = 0;
  f.reset = parameters->Get("Reset", &reset) == NGX_SUCCESS && reset != 0;
  f.serial = ++g_serial;
  g_frame = f;
  if (!g_have_frame) {
    g_have_frame = true;
    Line("NGX: first DLSS call taken: in use %ux%u at %u,%u of %llux%u, scale %.6f %.6f, jitter %.3f %.3f, flags %s0x%x", width, height, f.x, f.y, motion_desc.Width, motion_desc.Height, f.scale[0], f.scale[1], f.jitter[0], f.jitter[1], flags_known ? "" : "not known, taken as low resolution and reversed depth ", flags_known ? flags : 0);
  }
}

bool Latest(Frame* out) {
  Lock lock;
  if (!g_have_frame) return false;
  *out = g_frame;
  return true;
}

int HookedCalls() { return g_hooked; }

void Start(LogFunction log) { g_log = log; }

void Poll() {
  if (g_tried) return;
  HMODULE module = GetModuleHandleW(L"_nvngx.dll");
  if (module == nullptr) module = GetModuleHandleW(L"nvngx.dll");
  if (module == nullptr) return;
  g_tried = true;
  HMODULE pinned = nullptr;
  wchar_t path[MAX_PATH] = L"";
  GetModuleFileNameW(module, path, MAX_PATH);
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(module), &pinned);
  const auto hook = [&](const char* name, void* detour, void** real) {
    void* const target = reinterpret_cast<void*>(GetProcAddress(module, name));
    if (target != nullptr && slgame::HookFunction(name, target, detour, real, g_log)) ++g_hooked;
  };
  hook("NVSDK_NGX_D3D12_CreateFeature", reinterpret_cast<void*>(&HookCreate), reinterpret_cast<void**>(&g_real_create));
  hook("NVSDK_NGX_D3D12_EvaluateFeature", reinterpret_cast<void*>(&HookEvaluate), reinterpret_cast<void**>(&g_real_evaluate));
  hook("NVSDK_NGX_D3D12_EvaluateFeature_C", reinterpret_cast<void*>(&HookEvaluateC), reinterpret_cast<void**>(&g_real_evaluate_c));
  Line("NGX: %ls found, %d of 3 calls hooked", path, g_hooked);
}

namespace {

using Evaluate11Fn = int(__cdecl*)(ID3D11DeviceContext*, const void*, const Parameter*, void*);
using Create11Fn = int(__cdecl*)(ID3D11DeviceContext*, int, const Parameter*, void**);
Evaluate11Fn g_real_evaluate11 = nullptr, g_real_evaluate11_c = nullptr;
Create11Fn g_real_create11 = nullptr;
LogFunction g_log11 = nullptr;
bool g_tried11 = false;
LONG g_calls11 = 0;
char g_last_shape11[400] = "";
// From the last feature made: its flags (-1 = not known) and output size, for calls that do not repeat them.
int g_create_flags11 = -1;
uint32_t g_create_out_width11 = 0, g_create_out_height11 = 0;
// The main call: the largest in use area of the last 240 calls, and the call that set it. A smaller second DLSS in the same frame is left out.
uint64_t g_largest_area11 = 0;
LONG g_largest_call11 = 0;

struct Copies11 {
  ComPtr<ID3D11Texture2D> motion, depth;
  ComPtr<ID3D11ShaderResourceView> motion_srv, depth_srv;
  D3D11_TEXTURE2D_DESC motion_desc{}, depth_desc{};
};
Copies11 g_copies11;
Frame11 g_frame11;
bool g_have_frame11 = false;

void Line11(const char* format, ...) {
  if (g_log11 == nullptr) return;
  char text[700];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  g_log11(text);
}

// The typeless format a depth buffer is copied into, and the view that reads its depth: -1 for one HSF does not read.
bool DepthFormats(DXGI_FORMAT format, DXGI_FORMAT& copy, DXGI_FORMAT& view) {
  switch (format) {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
      copy = DXGI_FORMAT_R32G8X24_TYPELESS;
      view = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
      return true;
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
      copy = DXGI_FORMAT_R32_TYPELESS;
      view = DXGI_FORMAT_R32_FLOAT;
      return true;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
      copy = DXGI_FORMAT_R24G8_TYPELESS;
      view = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
      return true;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_D16_UNORM:
    case DXGI_FORMAT_R16_UNORM:
      copy = DXGI_FORMAT_R16_TYPELESS;
      view = DXGI_FORMAT_R16_UNORM;
      return true;
    default:
      return false;
  }
}

ID3D11Texture2D* Texture11(const Parameter* parameters, const char* name, D3D11_TEXTURE2D_DESC& desc) {
  ID3D11Resource* resource = nullptr;
  if (parameters->Get(name, &resource) != NGX_SUCCESS || resource == nullptr) return nullptr;
  ComPtr<ID3D11Texture2D> texture;
  if (FAILED(resource->QueryInterface(IID_PPV_ARGS(&texture)))) return nullptr;
  texture->GetDesc(&desc);
  return texture.Get();  // NGX's parameters keep it alive for the call
}

bool MakeCopies11(ID3D11DeviceContext* context, const D3D11_TEXTURE2D_DESC& motion, const D3D11_TEXTURE2D_DESC& depth) {
  DXGI_FORMAT depth_copy = DXGI_FORMAT_UNKNOWN, depth_view = DXGI_FORMAT_UNKNOWN;
  if (!DepthFormats(depth.Format, depth_copy, depth_view)) return false;
  ComPtr<ID3D11Device> device;
  context->GetDevice(&device);
  Copies11 made;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = motion.Width;
  desc.Height = motion.Height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = TypedMotion(motion.Format);
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  HRESULT hr = device->CreateTexture2D(&desc, nullptr, &made.motion);
  if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(made.motion.Get(), nullptr, &made.motion_srv);
  desc.Width = depth.Width;
  desc.Height = depth.Height;
  desc.Format = depth_copy;
  if (SUCCEEDED(hr)) hr = device->CreateTexture2D(&desc, nullptr, &made.depth);
  D3D11_SHADER_RESOURCE_VIEW_DESC view{};
  view.Format = depth_view;
  view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
  view.Texture2D.MipLevels = 1;
  if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(made.depth.Get(), &view, &made.depth_srv);
  if (FAILED(hr)) {
    Line11("D3D11 DLSS: copies of %ux%u and %ux%u not made: 0x%08lx", motion.Width, motion.Height, depth.Width, depth.Height, hr);
    return false;
  }
  made.motion_desc = motion;
  made.depth_desc = depth;
  g_copies11 = std::move(made);
  return true;
}

bool SameTexture(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b) { return a.Width == b.Width && a.Height == b.Height && a.Format == b.Format; }

// After a call that worked: the vectors and depth copied on the caller's context. Logs the first calls, every change of the textures handed over and every
// 1000th call.
void Take11(ID3D11DeviceContext* context, const Parameter* parameters, int result) {
  const LONG call = InterlockedIncrement(&g_calls11);
  if (parameters == nullptr || context == nullptr) return;
  D3D11_TEXTURE2D_DESC motion_desc{}, depth_desc{}, color_desc{};
  ID3D11Texture2D* const motion = Texture11(parameters, "MotionVectors", motion_desc);
  ID3D11Texture2D* const depth = Texture11(parameters, "Depth", depth_desc);
  Texture11(parameters, "Color", color_desc);
  char shape[300];
  snprintf(shape, sizeof(shape), "vectors %ux%u fmt %u, depth %ux%u fmt %u, color %ux%u fmt %u", motion_desc.Width, motion_desc.Height,
           static_cast<unsigned>(motion_desc.Format), depth_desc.Width, depth_desc.Height, static_cast<unsigned>(depth_desc.Format), color_desc.Width,
           color_desc.Height, static_cast<unsigned>(color_desc.Format));
  int flags = -1, reset = 0;
  if (parameters->Get("DLSS.Feature.Create.Flags", &flags) != NGX_SUCCESS) flags = g_create_flags11;
  parameters->Get("Reset", &reset);
  uint32_t width = ReadUint(parameters, "DLSS.Render.Subrect.Dimensions.Width"), height = ReadUint(parameters, "DLSS.Render.Subrect.Dimensions.Height");
  // Vectors at the output's size (no low resolution flag): the part in use is the output, not the render subrect. Flags not known: low resolution, as before.
  if (flags >= 0 && (flags & FLAG_MV_LOW_RES) == 0 && g_create_out_width11 != 0 && g_create_out_height11 != 0) {
    width = g_create_out_width11;
    height = g_create_out_height11;
  }
  if (width == 0 || height == 0 || width > motion_desc.Width || height > motion_desc.Height) {
    width = motion_desc.Width;
    height = motion_desc.Height;
  }
  uint32_t x = ReadUint(parameters, "DLSS.Input.MV.Subrect.Base.X"), y = ReadUint(parameters, "DLSS.Input.MV.Subrect.Base.Y");
  if (x + width > motion_desc.Width || y + height > motion_desc.Height) x = y = 0;
  const float scale_x = ReadFloat(parameters, "MV.Scale.X", 1.f), scale_y = ReadFloat(parameters, "MV.Scale.Y", 1.f);
  if (strcmp(shape, g_last_shape11) != 0 || call <= 3 || call % 1000 == 0) {
    strncpy_s(g_last_shape11, shape, _TRUNCATE);
    Line11("D3D11 DLSS: call %ld on thread %lu, result %d: %s; in use %ux%u at %u,%u, MV scale %.3f %.3f, jitter %.3f %.3f, flags %d, reset %d", call,
           GetCurrentThreadId(), result, shape, width, height, x, y, scale_x, scale_y, ReadFloat(parameters, "Jitter.Offset.X", 0.f),
           ReadFloat(parameters, "Jitter.Offset.Y", 0.f), flags, reset);
  }
  if (result != NGX_SUCCESS || motion == nullptr || depth == nullptr || motion_desc.SampleDesc.Count > 1 || depth_desc.SampleDesc.Count > 1) return;
  Lock lock;
  const uint64_t area = static_cast<uint64_t>(width) * height;
  if (call - g_largest_call11 > 240) g_largest_area11 = 0;
  if (area * 2 < g_largest_area11) return;
  if (area >= g_largest_area11) {
    g_largest_area11 = area;
    g_largest_call11 = call;
  }
  if (g_copies11.motion == nullptr || !SameTexture(g_copies11.motion_desc, motion_desc) || !SameTexture(g_copies11.depth_desc, depth_desc)) {
    if (!MakeCopies11(context, motion_desc, depth_desc)) {
      if (g_refused_format != static_cast<uint32_t>(depth_desc.Format)) Line11("D3D11 DLSS: depth format %u is not read, so this game stays on flow", static_cast<unsigned>(depth_desc.Format));
      g_refused_format = static_cast<uint32_t>(depth_desc.Format);
      return;
    }
    Line11("D3D11 DLSS: copying vectors %ux%u and depth %ux%u after each call", motion_desc.Width, motion_desc.Height, depth_desc.Width, depth_desc.Height);
  }
  context->CopyResource(g_copies11.motion.Get(), motion);
  context->CopyResource(g_copies11.depth.Get(), depth);
  Frame11 f;
  f.motion = g_copies11.motion_srv.Get();
  f.depth = g_copies11.depth_srv.Get();
  f.x = x;
  f.y = y;
  f.width = width;
  f.height = height;
  f.scale[0] = scale_x;
  f.scale[1] = scale_y;
  f.reset = reset != 0;
  f.serial = g_frame11.serial + 1;
  g_frame11 = f;
  g_have_frame11 = true;
}

int __cdecl HookCreate11(ID3D11DeviceContext* context, int feature, const Parameter* parameters, void** handle) {
  const int result = g_real_create11(context, feature, parameters, handle);
  if (parameters != nullptr) {
    int flags = -1;
    if (parameters->Get("DLSS.Feature.Create.Flags", &flags) != NGX_SUCCESS) flags = -1;
    if (result == NGX_SUCCESS) {
      g_create_flags11 = flags;
      g_create_out_width11 = ReadUint(parameters, "OutWidth");
      g_create_out_height11 = ReadUint(parameters, "OutHeight");
    }
    Line11("D3D11 DLSS: feature %d made, result %d, flags 0x%x, render %ux%u, output %ux%u, quality %u", feature, result, flags, ReadUint(parameters, "Width"),
           ReadUint(parameters, "Height"), ReadUint(parameters, "OutWidth"), ReadUint(parameters, "OutHeight"), ReadUint(parameters, "PerfQualityValue"));
  }
  return result;
}
int __cdecl HookEvaluate11(ID3D11DeviceContext* context, const void* handle, const Parameter* parameters, void* callback) {
  const int result = g_real_evaluate11(context, handle, parameters, callback);
  Take11(context, parameters, result);
  return result;
}
int __cdecl HookEvaluate11C(ID3D11DeviceContext* context, const void* handle, const Parameter* parameters, void* callback) {
  const int result = g_real_evaluate11_c(context, handle, parameters, callback);
  Take11(context, parameters, result);
  return result;
}

}  // namespace

void Start11(LogFunction log) { g_log11 = log; }

void Poll11() {
  if (g_tried11 || g_log11 == nullptr) return;
  HMODULE module = GetModuleHandleW(L"_nvngx.dll");
  if (module == nullptr) module = GetModuleHandleW(L"nvngx.dll");
  if (module == nullptr) return;
  g_tried11 = true;
  HMODULE pinned = nullptr;
  wchar_t path[MAX_PATH] = L"";
  GetModuleFileNameW(module, path, MAX_PATH);
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(module), &pinned);
  int hooked = 0;
  const auto hook = [&](const char* name, void* detour, void** real) {
    void* const target = reinterpret_cast<void*>(GetProcAddress(module, name));
    if (target != nullptr && slgame::HookFunction(name, target, detour, real, g_log11)) ++hooked;
  };
  hook("NVSDK_NGX_D3D11_CreateFeature", reinterpret_cast<void*>(&HookCreate11), reinterpret_cast<void**>(&g_real_create11));
  hook("NVSDK_NGX_D3D11_EvaluateFeature", reinterpret_cast<void*>(&HookEvaluate11), reinterpret_cast<void**>(&g_real_evaluate11));
  hook("NVSDK_NGX_D3D11_EvaluateFeature_C", reinterpret_cast<void*>(&HookEvaluate11C), reinterpret_cast<void**>(&g_real_evaluate11_c));
  Line11("D3D11 DLSS: %ls found, %d calls hooked (older drivers have no _C call)", path, hooked);
}

bool Latest11(Frame11* out) {
  Lock lock;
  if (!g_have_frame11) return false;
  *out = g_frame11;
  return true;
}

void Stop11(bool process_exiting) {
  if (process_exiting || !g_tried11) return;
  slgame::UnhookFunctions();
  Lock lock;
  g_have_frame11 = false;
  g_frame11 = {};
  g_copies11 = {};
}

void Stop(bool process_exiting) {
  if (process_exiting) return;
  slgame::UnhookFunctions();
  Lock lock;
  g_have_frame = false;
  g_frame = {};
  g_copies = {};
  g_old.clear();
}

}  // namespace hsf::ngx
