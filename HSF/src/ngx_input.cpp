// See ngx_input.h.
#include "ngx_input.h"

#include <windows.h>
#include <d3d11.h>
#include <intrin.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdarg>
#include <cwchar>
#include <iterator>
#include <cstdio>
#include <cstring>
#include <vector>

#include "hook_patch.h"
#include "ngx_call.h"

using Microsoft::WRL::ComPtr;

namespace hsf::ngx {
namespace {

using call::NGX_SUCCESS;
using call::ReadFloat;
using call::ReadUint;
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

}  // namespace

// Each DLSS feature made through the hooks, with the module that called (for the menu's MOD STACK). Its own lock: it is written from the
// create hooks only, which are rare.
SRWLOCK g_made_lock = SRWLOCK_INIT;
MadeFeature g_made[8];
int g_made_count = 0;
void NoteMade(int feature, int api, void* caller) {
  wchar_t module[64] = L"?";
  wchar_t path[MAX_PATH] = {};
  HMODULE handle = nullptr;
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(caller), &handle)) {
    if (GetModuleFileNameW(handle, path, MAX_PATH) != 0) {
      const wchar_t* const slash = wcsrchr(path, L'\\');
      wcsncpy_s(module, slash != nullptr ? slash + 1 : path, _TRUNCATE);
    }
  }
  AcquireSRWLockExclusive(&g_made_lock);
  int i = 0;
  while (i < g_made_count && !(g_made[i].feature == feature && g_made[i].api == api && _wcsicmp(g_made[i].path, path) == 0)) ++i;
  if (i == g_made_count && g_made_count < static_cast<int>(std::size(g_made))) {
    g_made[i] = MadeFeature{feature, api, 0, {}, {}};
    wcsncpy_s(g_made[i].caller, module, _TRUNCATE);
    wcsncpy_s(g_made[i].path, path, _TRUNCATE);
    ++g_made_count;
  }
  if (i < g_made_count) ++g_made[i].count;
  ReleaseSRWLockExclusive(&g_made_lock);
}

namespace {

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
call::Created g_created;  // from the last CreateFeature for DLSS
call::MainCall g_main;
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

int __cdecl HookCreate(ID3D12GraphicsCommandList* list, int feature, const Parameter* parameters, void** handle) {
  const int result = g_real_create(list, feature, parameters, handle);
  if (result == NGX_SUCCESS) NoteMade(feature, MADE_D3D12, _ReturnAddress());
  if (result == NGX_SUCCESS) {
    Lock lock;
    if (call::ReadCreate(parameters, feature, g_created))
      Line("NGX: DLSS made, feature %d, flags 0x%x, render %ux%u, output %ux%u", feature, g_created.flags, g_created.width, g_created.height, g_created.out_width, g_created.out_height);
  }
  return result;
}

bool Record(ID3D12GraphicsCommandList* list, const Parameter* parameters, Frame& out);
void Publish(const Frame& f);

// The copies are recorded BEFORE the call goes on. Another mod's DLSS can sit under this hook (RenoDX's DLSS runs its own work inside the
// call), and after it the game's vectors and depth are no longer sure to be in NGX's state, or to hold what the game gave. Before the call they
// are, as NGX's rule says. The copy is handed over only if the call worked.
int __cdecl HookEvaluate(ID3D12GraphicsCommandList* list, const void* handle, const Parameter* parameters, void* callback) {
  Frame f;
  const bool taken = Record(list, parameters, f);
  const int result = g_real_evaluate(list, handle, parameters, callback);
  if (taken && result == NGX_SUCCESS) Publish(f);
  return result;
}
int __cdecl HookEvaluateC(ID3D12GraphicsCommandList* list, const void* handle, const Parameter* parameters, void* callback) {
  Frame f;
  const bool taken = Record(list, parameters, f);
  const int result = g_real_evaluate_c(list, handle, parameters, callback);
  if (taken && result == NGX_SUCCESS) Publish(f);
  return result;
}

}  // namespace

int MadeFeatures(MadeFeature* out, int max) {
  AcquireSRWLockShared(&g_made_lock);
  const int n = std::min(g_made_count, max);
  for (int i = 0; i < n; ++i) out[i] = g_made[i];
  ReleaseSRWLockShared(&g_made_lock);
  return n;
}

namespace {

int g_hooked = 0;
int g_found = 0;  // of the three calls, how many this driver's _nvngx.dll has
bool g_tried = false;
hookpatch::HookSet g_hooks;  // NGX's D3D12 calls: removed by Stop only

// The vectors and depth of one call copied on its list, and what HSF reads of the call in `out`. False when the call is not taken (not the
// upscaler, not the main call, or a format HSF cannot copy): nothing is recorded then.
bool Record(ID3D12GraphicsCommandList* list, const Parameter* parameters, Frame& out) {
  if (list == nullptr || parameters == nullptr) return false;
  ID3D12Resource* motion = nullptr;
  ID3D12Resource* depth = nullptr;
  // Only the upscalers have these under these names, so DLSS's own frame generation or anything else on this call never gets here.
  if (parameters->Get("MotionVectors", &motion) != NGX_SUCCESS || parameters->Get("Depth", &depth) != NGX_SUCCESS || motion == nullptr || depth == nullptr) return false;
  const D3D12_RESOURCE_DESC motion_desc = motion->GetDesc();
  const D3D12_RESOURCE_DESC depth_desc = depth->GetDesc();
  if (motion_desc.SampleDesc.Count > 1 || depth_desc.SampleDesc.Count > 1) return false;

  Lock lock;
  if (!DepthCopies(depth_desc.Format)) {
    if (g_refused_format != static_cast<uint32_t>(depth_desc.Format)) Line("NGX: depth format %u cannot be copied, so this game stays on flow", static_cast<unsigned>(depth_desc.Format));
    g_refused_format = static_cast<uint32_t>(depth_desc.Format);
    return false;
  }

  const call::DlssCall c = call::Read(parameters, g_created, static_cast<uint32_t>(motion_desc.Width), motion_desc.Height);
  if (!g_main.Take(c.Area(), g_serial)) return false;

  if (g_copies.motion == nullptr || g_copies.width != motion_desc.Width || g_copies.height != motion_desc.Height || g_copies.motion_format != motion_desc.Format ||
      g_copies.depth->GetDesc().Width != depth_desc.Width || g_copies.depth->GetDesc().Height != depth_desc.Height) {
    ComPtr<ID3D12Device> device;
    if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) return false;
    Copies made;
    if (!MakeCopies(device.Get(), motion_desc, depth_desc, made)) return false;
    if (g_copies.motion != nullptr) g_old.push_back({std::move(g_copies), g_serial});
    g_copies = std::move(made);
  }
  while (!g_old.empty() && g_serial - g_old.front().serial > 16) g_old.erase(g_old.begin());

  CopyOn(list, motion, g_copies.motion.Get());
  CopyOn(list, depth, g_copies.depth.Get());

  Frame f;
  f.motion = g_copies.motion.Get();
  f.depth = g_copies.depth.Get();
  f.x = c.x;
  f.y = c.y;
  f.width = c.width;
  f.height = c.height;
  // NGX: multiply by its scale for pixels of the part in use. DLSS-G: multiply by its scale for the fraction of the picture.
  f.scale[0] = c.to_pixels[0] / static_cast<float>(c.width);
  f.scale[1] = c.to_pixels[1] / static_cast<float>(c.height);
  f.jitter[0] = c.jitter[0];
  f.jitter[1] = c.jitter[1];
  f.jittered = c.jittered;
  f.full_res = !c.low_res;
  f.depth_inverted = c.depth_inverted != 0;  // not known: reversed, as most games with DLSS have it
  f.reset = c.reset;
  f.flags = c.flags;
  f.serial = ++g_serial;
  out = f;
  return true;
}

void Publish(const Frame& f) {
  Lock lock;
  if (g_have_frame && f.serial < g_frame.serial) return;  // a newer call on another thread was handed over first
  g_frame = f;
  if (!g_have_frame) {
    g_have_frame = true;
    const D3D12_RESOURCE_DESC motion_desc = f.motion->GetDesc();
    Line("NGX: first DLSS call taken: in use %ux%u at %u,%u of %llux%u, scale %.6f %.6f, jitter %.3f %.3f, flags %s0x%x, copied before the call goes on", f.width, f.height, f.x, f.y,
         motion_desc.Width, motion_desc.Height, f.scale[0], f.scale[1], f.jitter[0], f.jitter[1], f.flags >= 0 ? "" : "not known, taken as low resolution and reversed depth ",
         f.flags >= 0 ? f.flags : 0);
  }
}

}  // namespace

void Capture(ID3D12GraphicsCommandList* list, const Parameter* parameters) {
  Frame f;
  if (Record(list, parameters, f)) Publish(f);
}

bool Latest(Frame* out) {
  Lock lock;
  if (!g_have_frame) return false;
  *out = g_frame;
  return true;
}

int HookedCalls() { return g_hooked; }
int FoundCalls() { return g_found; }
bool Loaded() { return g_tried; }

void Rescan() {
  // Hooks already in stay in; the next Poll tries the rest again, and finds _nvngx.dll if the game loaded it since.
  if (g_tried && g_hooked >= g_found && g_found > 0) return;
  g_tried = false;
  g_hooked = 0;
}

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
  int found = 0;
  const auto hook = [&](const char* name, void* detour, void** real) {
    void* const target = reinterpret_cast<void*>(GetProcAddress(module, name));
    if (target == nullptr) return;
    ++found;
    if (g_hooks.Add(name, target, detour, real, g_log)) ++g_hooked;
  };
  hook("NVSDK_NGX_D3D12_CreateFeature", reinterpret_cast<void*>(&HookCreate), reinterpret_cast<void**>(&g_real_create));
  hook("NVSDK_NGX_D3D12_EvaluateFeature", reinterpret_cast<void*>(&HookEvaluate), reinterpret_cast<void**>(&g_real_evaluate));
  hook("NVSDK_NGX_D3D12_EvaluateFeature_C", reinterpret_cast<void*>(&HookEvaluateC), reinterpret_cast<void**>(&g_real_evaluate_c));
  g_found = found;
  Line("NGX: %ls found, %d of the %d calls this driver has hooked (older drivers have a third, _C call)", path, g_hooked, found);
}

namespace {

using Evaluate11Fn = int(__cdecl*)(ID3D11DeviceContext*, const void*, const Parameter*, void*);
using Create11Fn = int(__cdecl*)(ID3D11DeviceContext*, int, const Parameter*, void**);
Evaluate11Fn g_real_evaluate11 = nullptr, g_real_evaluate11_c = nullptr;
Create11Fn g_real_create11 = nullptr;
LogFunction g_log11 = nullptr;
bool g_tried11 = false;
hookpatch::HookSet g_hooks11;  // NGX's D3D11 calls: removed by Stop11 only
LONG g_calls11 = 0;
char g_last_shape11[400] = "";
call::Created g_created11;  // from the last CreateFeature for DLSS
call::MainCall g_main11;     // counted in calls, taken or not

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

// What one call gave, read before it goes on, for the log line after it.
struct Call11 {
  LONG call = 0;
  char shape[300] = "";
  call::DlssCall read;
};

// Before the call goes on (see HookEvaluate): the vectors and depth copied on the caller's context into `out`. False when the call is not taken.
bool Copy11(ID3D11DeviceContext* context, const Parameter* parameters, Call11& c, Frame11& out) {
  c.call = InterlockedIncrement(&g_calls11);
  if (parameters == nullptr || context == nullptr) return false;
  D3D11_TEXTURE2D_DESC motion_desc{}, depth_desc{}, color_desc{};
  ID3D11Texture2D* const motion = Texture11(parameters, "MotionVectors", motion_desc);
  ID3D11Texture2D* const depth = Texture11(parameters, "Depth", depth_desc);
  Texture11(parameters, "Color", color_desc);
  snprintf(c.shape, sizeof(c.shape), "vectors %ux%u fmt %u, depth %ux%u fmt %u, color %ux%u fmt %u", motion_desc.Width, motion_desc.Height,
           static_cast<unsigned>(motion_desc.Format), depth_desc.Width, depth_desc.Height, static_cast<unsigned>(depth_desc.Format), color_desc.Width,
           color_desc.Height, static_cast<unsigned>(color_desc.Format));
  Lock lock;
  c.read = call::Read(parameters, g_created11, motion_desc.Width, motion_desc.Height);
  if (motion == nullptr || depth == nullptr || motion_desc.SampleDesc.Count > 1 || depth_desc.SampleDesc.Count > 1) return false;
  if (!g_main11.Take(c.read.Area(), static_cast<uint64_t>(c.call))) return false;
  if (g_copies11.motion == nullptr || !SameTexture(g_copies11.motion_desc, motion_desc) || !SameTexture(g_copies11.depth_desc, depth_desc)) {
    if (!MakeCopies11(context, motion_desc, depth_desc)) {
      if (g_refused_format != static_cast<uint32_t>(depth_desc.Format)) Line11("D3D11 DLSS: depth format %u is not read, so this game stays on flow", static_cast<unsigned>(depth_desc.Format));
      g_refused_format = static_cast<uint32_t>(depth_desc.Format);
      return false;
    }
    Line11("D3D11 DLSS: copying vectors %ux%u and depth %ux%u before each call goes on", motion_desc.Width, motion_desc.Height, depth_desc.Width, depth_desc.Height);
  }
  context->CopyResource(g_copies11.motion.Get(), motion);
  context->CopyResource(g_copies11.depth.Get(), depth);
  Frame11 f;
  f.motion = g_copies11.motion_srv.Get();
  f.depth = g_copies11.depth_srv.Get();
  f.x = c.read.x;
  f.y = c.read.y;
  f.width = c.read.width;
  f.height = c.read.height;
  f.to_pixels[0] = c.read.to_pixels[0];
  f.to_pixels[1] = c.read.to_pixels[1];
  f.jitter[0] = c.read.jitter[0];
  f.jitter[1] = c.read.jitter[1];
  f.jittered = c.read.jittered;
  f.reset = c.read.reset;
  out = f;
  return true;
}

// After the call: logs the first calls, every change of the textures handed over and every 1000th call, and hands the copy over if the call worked.
void After11(const Call11& c, int result, bool taken, Frame11& f) {
  if (c.shape[0] != 0 && (strcmp(c.shape, g_last_shape11) != 0 || c.call <= 3 || c.call % 1000 == 0)) {
    strncpy_s(g_last_shape11, c.shape, _TRUNCATE);
    Line11("D3D11 DLSS: call %ld on thread %lu, result %d: %s; in use %ux%u at %u,%u, MV scale %.3f %.3f, jitter %.3f %.3f, flags %d, reset %d", c.call,
           GetCurrentThreadId(), result, c.shape, c.read.width, c.read.height, c.read.x, c.read.y, c.read.to_pixels[0], c.read.to_pixels[1], c.read.jitter[0],
           c.read.jitter[1], c.read.flags, c.read.reset ? 1 : 0);
  }
  if (!taken || result != NGX_SUCCESS) return;
  Lock lock;
  f.serial = g_frame11.serial + 1;
  g_frame11 = f;
  g_have_frame11 = true;
}

int __cdecl HookCreate11(ID3D11DeviceContext* context, int feature, const Parameter* parameters, void** handle) {
  const int result = g_real_create11(context, feature, parameters, handle);
  if (result == NGX_SUCCESS) NoteMade(feature, MADE_D3D11, _ReturnAddress());
  if (parameters != nullptr) {
    int flags = -1;
    if (parameters->Get("DLSS.Feature.Create.Flags", &flags) != NGX_SUCCESS) flags = -1;
    if (result == NGX_SUCCESS) {
      Lock lock;
      call::ReadCreate(parameters, feature, g_created11);
    }
    Line11("D3D11 DLSS: feature %d made, result %d, flags 0x%x, render %ux%u, output %ux%u, quality %u", feature, result, flags, ReadUint(parameters, "Width"),
           ReadUint(parameters, "Height"), ReadUint(parameters, "OutWidth"), ReadUint(parameters, "OutHeight"), ReadUint(parameters, "PerfQualityValue"));
  }
  return result;
}
int __cdecl HookEvaluate11(ID3D11DeviceContext* context, const void* handle, const Parameter* parameters, void* callback) {
  Call11 c;
  Frame11 f;
  const bool taken = Copy11(context, parameters, c, f);
  const int result = g_real_evaluate11(context, handle, parameters, callback);
  After11(c, result, taken, f);
  return result;
}
int __cdecl HookEvaluate11C(ID3D11DeviceContext* context, const void* handle, const Parameter* parameters, void* callback) {
  Call11 c;
  Frame11 f;
  const bool taken = Copy11(context, parameters, c, f);
  const int result = g_real_evaluate11_c(context, handle, parameters, callback);
  After11(c, result, taken, f);
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
    if (target != nullptr && g_hooks11.Add(name, target, detour, real, g_log11)) ++hooked;
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
  g_hooks11.RemoveAll(nullptr);
  Lock lock;
  g_have_frame11 = false;
  g_frame11 = {};
  g_copies11 = {};
}

void Stop(bool process_exiting) {
  if (process_exiting) return;
  g_hooks.RemoveAll(nullptr);
  Lock lock;
  g_have_frame = false;
  g_frame = {};
  g_copies = {};
  g_old.clear();
}

}  // namespace hsf::ngx
