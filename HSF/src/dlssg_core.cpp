// See dlssg_core.h.

#include "dlssg_core.h"

#include <sl_consts.h>
#include <sl_pcl.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <share.h>
#include <string>
#include <vector>

namespace hsf::dlssg_core {
namespace {

const sl::ViewportHandle VIEWPORT{0};
constexpr float CONSTANT_DEPTH = 0.5f;  // what DLSS-G gets for a frame with no real depth

std::mutex g_log_lock;
FILE* g_log = nullptr;

// Streamline's "Wait on gpu fence timed out after 500ms": the GPU has fallen half a second behind. A few of those in a row end in a driver
// reset (device removed) that can take the whole PC with it, so the presenter turns DLSS-G off when they come close together.
std::atomic<uint32_t> g_fence_timeouts{0};

void SlLog(sl::LogType type, const char* message) {
  if (type == sl::LogType::eInfo) return;
  if (message != nullptr && std::strstr(message, "waitCPUFence") != nullptr && std::strstr(message, "timed out") != nullptr) g_fence_timeouts.fetch_add(1);
  Log("[streamline %s] %s", type == sl::LogType::eWarn ? "warn" : "ERROR", message);
}

sl::float4x4 Identity() {
  sl::float4x4 m{};
  m.setRow(0, {1, 0, 0, 0});
  m.setRow(1, {0, 1, 0, 0});
  m.setRow(2, {0, 0, 1, 0});
  m.setRow(3, {0, 0, 0, 1});
  return m;
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

// ---- The child window ----
LRESULT CALLBACK ChildProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
  if (message == WM_ERASEBKGND) return 1;  // the swap chain covers it; no flash of background
  // Enabled only while ReShade's menu (Host::TakeMouse) or MFG-Unlocker's (ModMenuOpen) is open. Never take the focus or activate: DLSS-G stops when the game loses focus.
  if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  // Windows sends WM_SETCURSOR to the window under the pointer on every mouse move. DefWindowProc would send it on to the game's window and
  // wait for the game's thread, which may be in Reflex's sleep; the game's thread and this one share an input queue (a child window on another
  // thread's window), so the game's mouse input waited behind it. Seen in Valheim: laggy and uneven only with the pointer inside the window.
  // The cursor stays as the game set it.
  if (message == WM_SETCURSOR) return TRUE;
  // A mod that draws a menu on this swap chain (MFG-Unlocker) sees the mouse first, through its own subclass, and keeps what lands on its menu.
  // Everything else goes on to the game's window, where ReShade's menu takes it. The child covers the parent's client area from 0,0, so the
  // client coordinates are the same; wheel messages carry screen coordinates, which are the same for both.
  if (message >= WM_MOUSEFIRST && message <= WM_MOUSELAST) {
    if (const HWND parent = GetParent(hwnd)) PostMessageW(parent, message, wp, lp);
    return 0;
  }
  if (message == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wp, lp);
}

HMODULE ThisModule() {
  HMODULE module = nullptr;
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                     reinterpret_cast<LPCWSTR>(&ChildProc), &module);
  return module;
}

// The swap chain on the child, through Streamline's factory so DLSS-G attaches to it.
bool MakeSwapChain(Presenter& p, Host& host) {
  while (p.child.load() == nullptr) {
    if (p.window_failed.load() || host.Stopping()) return false;
    Sleep(1);
  }
  if (host.Stopping()) return false;
  using CreateFactoryFn = HRESULT(WINAPI*)(UINT, REFIID, void**);
  const auto create_factory = reinterpret_cast<CreateFactoryFn>(GetProcAddress(Sl().interposer, "CreateDXGIFactory2"));
  ComPtr<IDXGIFactory5> factory;
  HRESULT hr = create_factory != nullptr ? create_factory(0, IID_PPV_ARGS(&factory)) : E_NOINTERFACE;
  if (FAILED(hr)) {
    Log("Streamline's CreateDXGIFactory2 failed (0x%08lx)", hr);
    return false;
  }
  // DLSS-G takes no FP16 (NVIDIA's DLSS-G guide, 11.0): an R10G10B10A2 swap chain and the conversion pass, made before the swap chain so that a failure
  // leaves the frame as it was (shown, but DLSS-G generates nothing).
  p.hdr = false;
  if (p.swap_format == DXGI_FORMAT_R16G16B16A16_FLOAT) {
    const D3D12_RESOURCE_DESC colour = p.slots[0]->GetDesc();
    const DXGI_FORMAT read = colour.Format == DXGI_FORMAT_R16G16B16A16_TYPELESS ? DXGI_FORMAT_R16G16B16A16_FLOAT : colour.Format;
    const char* what = nullptr;
    hr = S_OK;
    bool ok = p.hdr_pass.Init(Sl().device.Get(), read, p.width, p.height, BUFFERS, what, hr);
    if (ok) {
      what = "its texture";
      const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
      D3D12_RESOURCE_DESC desc{};
      desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      desc.Width = p.width;
      desc.Height = p.height;
      desc.DepthOrArraySize = 1;
      desc.MipLevels = 1;
      desc.SampleDesc.Count = 1;
      desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
      desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
      ok = SUCCEEDED(hr = Sl().device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&p.hdr_frame)));
    }
    if (ok) {
      p.hdr = true;
      p.swap_format = DXGI_FORMAT_R10G10B10A2_UNORM;
    } else {
      Log("the game's frame is FP16, which DLSS-G does not take, and converting it failed: %s (0x%08lx). DLSS-G will generate nothing", what, hr);
    }
  }
  BOOL tearing = FALSE;
  factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing, sizeof(tearing));
  p.tearing = tearing != FALSE;
  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = p.width;
  desc.Height = p.height;
  desc.Format = p.swap_format;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = BUFFERS;
  desc.Scaling = DXGI_SCALING_STRETCH;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  desc.Flags = p.tearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
  ComPtr<IDXGISwapChain1> swap1;
  Log("making the DLSS-G swap chain on the child window");
  hr = factory->CreateSwapChainForHwnd(Sl().queue.Get(), p.child.load(), &desc, nullptr, nullptr, &swap1);
  if (FAILED(hr) || FAILED(hr = swap1.As(&p.swap))) {
    Log("CreateSwapChainForHwnd on the child window failed (0x%08lx)", hr);
    return false;
  }
  factory->MakeWindowAssociation(p.child.load(), DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER);
  for (UINT i = 0; i < BUFFERS; ++i) p.swap->GetBuffer(i, IID_PPV_ARGS(&p.back[i]));
  Log("swap chain on the child window: %ux%u format %u, tearing %s", p.width, p.height, static_cast<unsigned>(p.swap_format),
      p.tearing ? "allowed" : "not supported");
  if (p.hdr) {
    // HDR10 when the display the window is on is in HDR mode, as RenoDX checks it (src/utils/swapchain.hpp); sRGB otherwise.
    ComPtr<IDXGIOutput> output;
    ComPtr<IDXGIOutput6> output6;
    DXGI_OUTPUT_DESC1 out{};
    p.pq = SUCCEEDED(p.swap->GetContainingOutput(&output)) && SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&out)) &&
           (out.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || out.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
    const DXGI_COLOR_SPACE_TYPE space = p.pq ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    UINT support = 0;
    hr = p.swap->CheckColorSpaceSupport(space, &support);
    if (SUCCEEDED(hr) && (support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) != 0) hr = p.swap->SetColorSpace1(space);
    else if (SUCCEEDED(hr)) hr = E_NOTIMPL;
    Log("the game's frame is FP16 (scRGB), which DLSS-G does not take: converted to %s for DLSS-G (display %s, colour space %s: 0x%08lx)",
        p.pq ? "HDR10" : "sRGB", p.pq ? "in HDR mode" : "in SDR mode", p.pq ? "HDR10" : "sRGB", hr);
  } else if (p.game_space != DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709) {
    // The game's frame is already HDR (RenoDX's HDR10 output, for one): this swap chain takes the game's colour space, set as RenoDX sets it
    // (src/utils/swapchain.hpp, ChangeColorSpace).
    hr = p.swap->SetColorSpace1(p.game_space);
    Log("the game's swap chain is in colour space %u: the same on the DLSS-G swap chain (0x%08lx)", static_cast<unsigned>(p.game_space), hr);
  }
  return true;
}

// DLSS-G's motion vector and depth inputs at the motion vectors' size. Tried once per Start.
bool MakeInputs(Presenter& p, const FrameInputs& in) {
  if (p.mvec) return true;
  if (p.inputs_failed) return false;
  ID3D12Device* const device = Sl().device.Get();
  const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = in.motion_width;
  desc.Height = in.motion_height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Format = in.motion_format;
  const char* compose_what = nullptr;
  HRESULT compose_hr = S_OK;
  p.compose_ready = p.compose.Init(device, Sl().queue.Get(), in.motion_format, in.motion_width, in.motion_height, compose_what, compose_hr);
  if (p.compose_ready) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  else Log("compose pass not available: %s (0x%08lx). The motion goes to DLSS-G as it is.", compose_what, compose_hr);
  // The frame checks read the frame's own texture, whose format may be typeless. Each can fail alone; without them DLSS-G gets what compose gives.
  const D3D12_RESOURCE_DESC colour = p.slots[0]->GetDesc();
  const DXGI_FORMAT colour_view = SwapFormat(colour.Format) != DXGI_FORMAT_UNKNOWN && colour.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && colour.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB ? SwapFormat(colour.Format) : colour.Format;
  p.colour_view = colour_view;
  const char* checks_what = nullptr;
  HRESULT checks_hr = S_OK;
  p.grid_ready = p.grid.Init(device, colour_view, checks_what, checks_hr);
  if (!p.grid_ready) Log("scene cut and still picture checks not available: %s (0x%08lx)", checks_what, checks_hr);
  p.mask_ready = p.mask.Init(device, Sl().queue.Get(), in.motion_format, colour.Format, colour_view, static_cast<UINT>(colour.Width), colour.Height, in.motion_width, in.motion_height, checks_what, checks_hr);
  if (!p.mask_ready) Log("HUD mask not available: %s (0x%08lx)", checks_what, checks_hr);
  p.samples_ready = p.compose_ready && p.samples.Init(device, in.motion_format, checks_what, checks_hr);
  if (p.compose_ready && !p.samples_ready) Log("camera motion not available: %s (0x%08lx)", checks_what, checks_hr);
  p.camera_ready = p.samples_ready && p.compose.InitCamera(colour.Format, colour_view, static_cast<UINT>(colour.Width), colour.Height, checks_what, checks_hr);
  if (p.samples_ready && !p.camera_ready) Log("camera motion not available: %s (0x%08lx)", checks_what, checks_hr);
  if (p.mask_ready) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  {
    D3D12_RESOURCE_DESC clean = colour;
    clean.Format = p.swap_format;
    // From an FP16 game the HUD-less image is converted into it (hdr_out.h), not copied.
    clean.Flags = p.hdr ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    clean.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_RESOURCE_DESC alpha = clean;
    alpha.Format = DXGI_FORMAT_R8_UNORM;
    alpha.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    p.ui_ready = p.ui.Init(device, colour_view, static_cast<UINT>(colour.Width), colour.Height, checks_what, checks_hr);
    if (p.ui_ready) {
      checks_what = "its textures";
      checks_hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &clean, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&p.hudless));
      if (SUCCEEDED(checks_hr)) checks_hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &alpha, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&p.ui_alpha));
      p.ui_ready = SUCCEEDED(checks_hr);
    }
    if (!p.ui_ready) Log("keeping overlays out of generated frames is not available: %s (0x%08lx)", checks_what, checks_hr);
  }
  HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&p.mvec));
  if (SUCCEEDED(hr) && p.compose_ready && p.mask_ready) hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&p.composed));
  if (SUCCEEDED(hr)) {
    D3D12_RESOURCE_DESC zero = desc;
    zero.Flags = D3D12_RESOURCE_FLAG_NONE;
    hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &zero, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&p.no_motion));
  }
  desc.Format = DXGI_FORMAT_R32_FLOAT;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
  D3D12_CLEAR_VALUE clear{};
  clear.Format = DXGI_FORMAT_R32_FLOAT;
  clear.Color[0] = CONSTANT_DEPTH;
  if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear, IID_PPV_ARGS(&p.depth));
  const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
  if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&p.rtv_heap));
  if (FAILED(hr)) {
    Log("DLSS-G's motion vector / depth inputs (0x%08lx)", hr);
    p.mvec.Reset();
    p.inputs_failed = true;
    return false;
  }
  device->CreateRenderTargetView(p.depth.Get(), nullptr, p.rtv_heap->GetCPUDescriptorHandleForHeapStart());
  p.input_width = in.motion_width;
  p.input_height = in.motion_height;
  p.input_format = in.motion_format;
  Log("DLSS-G inputs: motion vectors %ux%u format %u, depth R32_FLOAT", in.motion_width, in.motion_height, static_cast<unsigned>(in.motion_format));
  return true;
}

void SetDlssgOptions(Presenter& p, bool on, uint32_t frames, bool ui) {
  if (p.options_set && p.last_on == on && p.last_frames == frames && p.last_ui == ui) return;
  // A new frame count while generating left DLSS-G on but generating nothing (harness, 2x -> 4x). Off for one frame first; the next frame's
  // call turns it back on with the new count.
  if (p.options_set && p.last_on && on && p.last_frames != frames) on = false;
  p.options_set = true;
  p.last_on = on;
  p.last_frames = frames;
  p.last_ui = ui;
  sl::DLSSGOptions options{};
  options.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
  options.numFramesToGenerate = std::clamp(frames, 1u, Sl().max_frames);
  options.numBackBuffers = BUFFERS;
  options.mvecDepthWidth = p.input_width != 0 ? p.input_width : p.width;
  options.mvecDepthHeight = p.input_height != 0 ? p.input_height : p.height;
  options.colorWidth = p.width;
  options.colorHeight = p.height;
  options.colorBufferFormat = p.swap_format;
  options.mvecBufferFormat = p.input_format != DXGI_FORMAT_UNKNOWN ? p.input_format : DXGI_FORMAT_R16G16_FLOAT;
  options.depthBufferFormat = DXGI_FORMAT_R32_FLOAT;
  options.hudLessBufferFormat = p.swap_format;
  // The scene and the overlays are generated apart and put together afterwards.
  options.enableUserInterfaceRecomposition = ui ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  const sl::Result res = slDLSSGSetOptions(VIEWPORT, options);
  Log("DLSS-G %s (%u generated per real frame, overlays %s): %d", on ? "on" : "off", options.numFramesToGenerate, ui ? "kept out" : "in the picture", static_cast<int>(res));
}

// Two GPU fence time-outs within 5 s while frame gen is wanted: DLSS-G off until frame gen is turned off and on again.
void GuardGpu(Presenter& p, bool wanted) {
  constexpr uint32_t TRIP = 2;
  constexpr ULONGLONG WINDOW_MS = 5000;
  const uint32_t timeouts = g_fence_timeouts.load();
  if (timeouts != p.hang_seen) {
    const ULONGLONG now = GetTickCount64();
    if (p.hang_count == 0 || now - p.hang_first > WINDOW_MS) {
      p.hang_first = now;
      p.hang_count = 0;
    }
    p.hang_count += timeouts - p.hang_seen;
    p.hang_seen = timeouts;
    if (wanted && !p.hang_off && p.hang_count >= TRIP) {
      p.hang_off = true;
      Log("GPU guard: %u GPU waits over 500 ms within %llu ms; DLSS-G off before the driver resets. Frame gen off and on again starts it again",
          p.hang_count, now - p.hang_first);
    }
  }
  if (p.hang_off && !wanted) {
    p.hang_off = false;
    p.hang_count = 0;
    Log("GPU guard: frame gen turned off; it may start again");
  }
}

// DLSS-G gets a HUD-less image and UI alpha on this frame.
bool KeepingOverlaysOut(const Presenter& p, const FrameInputs& in) { return in.hudless != nullptr && p.ui_ready; }

// The compose pass runs on this frame.
bool Composing(const Presenter& p, const FrameInputs& in, bool motion) { return motion && in.compose && in.depth != nullptr && p.compose_ready; }

LONGLONG QpcNow() {
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  return now.QuadPart;
}

double QpcMs(LONGLONG ticks) {
  static const double per_ms = [] {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return static_cast<double>(frequency.QuadPart) / 1000.0;
  }();
  return static_cast<double>(ticks) / per_ms;
}

sl::float4x4 ForStreamline(const camera::Mat4& m) {
  const camera::Mat4 t = camera::Transpose(m);
  sl::float4x4 r;
  std::memcpy(&r, t.m, sizeof(t.m));
  return r;
}

void SetFrameConstants(const Presenter& p, const FrameInputs& in, bool motion, bool reset, const camera::Decision& camera_now, sl::FrameToken& token) {
  sl::Constants c{};
  c.cameraViewToClip = c.clipToCameraView = c.clipToLensClip = c.clipToPrevClip = c.prevClipToClip = Identity();
  c.jitterOffset = {0, 0};
  c.mvecScale = {motion ? in.motion_scale[0] : 0.f, motion ? in.motion_scale[1] : 0.f};
  c.cameraPinholeOffset = {0, 0};
  c.cameraPos = {0, 0, 0};
  c.cameraUp = {0, 1, 0};
  c.cameraRight = {1, 0, 0};
  c.cameraFwd = {0, 0, 1};
  c.cameraNear = 0.1f;
  c.cameraFar = 100.f;
  c.cameraFOV = 1.f;
  c.cameraAspectRatio = static_cast<float>(p.width) / static_cast<float>(p.height);
  c.depthInverted = in.depth != nullptr && in.depth_inverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  c.cameraMotionIncluded = sl::Boolean::eTrue;
  c.motionVectors3D = sl::Boolean::eFalse;
  c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  // The compose pass has already taken each edge's motion from the near surface, so DLSS-G must not widen it again.
  c.motionVectorsDilated = Composing(p, in, motion) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  // The real camera where it is known. The fit gives the motion between frames and nothing about the camera itself.
  if (motion && camera_now.source != camera::Source::none) {
    camera::Mat4 inverse;
    if (camera::Inverse(camera_now.clip_to_prev, inverse)) {
      c.clipToPrevClip = ForStreamline(camera_now.clip_to_prev);
      c.prevClipToClip = ForStreamline(inverse);
    }
    camera::Parts parts;
    if (camera_now.source == camera::Source::matrix && camera::Decompose(camera_now.world_to_clip, parts) && camera::Inverse(parts.view_to_clip, inverse)) {
      c.cameraViewToClip = ForStreamline(parts.view_to_clip);
      c.clipToCameraView = ForStreamline(inverse);
      c.cameraPos = {parts.pos[0], parts.pos[1], parts.pos[2]};
      c.cameraUp = {parts.up[0], parts.up[1], parts.up[2]};
      c.cameraRight = {parts.right[0], parts.right[1], parts.right[2]};
      c.cameraFwd = {parts.fwd[0], parts.fwd[1], parts.fwd[2]};
      c.cameraNear = parts.near_plane;
      c.cameraFar = parts.far_plane;
      c.cameraFOV = parts.fov;
      c.cameraAspectRatio = parts.aspect;
    }
  }
  slSetConstants(c, token, VIEWPORT);
}

void NvofLog(const char* text) { Log("%s", text); }

// After Present: the frame's last timestamp, on the same queue, and its three resolved for the next use of this buffer (TakeFrameTimes).
void EndFrameTimes(Presenter& p, UINT bi) {
  if (p.times == nullptr) return;
  p.tail_alloc[bi]->Reset();
  p.tail_list->Reset(p.tail_alloc[bi].Get(), nullptr);
  p.tail_list->EndQuery(p.time_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3 * bi + 2);
  p.tail_list->ResolveQueryData(p.time_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3 * bi, 3, p.time_readback.Get(), 3 * bi * sizeof(UINT64));
  p.tail_list->Close();
  ID3D12CommandList* const lists[] = {p.tail_list.Get()};
  Sl().queue->ExecuteCommandLists(1, lists);
  p.timed[bi] = true;
}

// This buffer's last frame has finished on the GPU (Run waited for it), so its timestamps are there. Logged every 600 frames.
void TakeFrameTimes(Presenter& p, UINT bi) {
  if (p.times == nullptr || !p.timed[bi]) return;
  const UINT64 start = p.times[3 * bi], end = p.times[3 * bi + 1], after = p.times[3 * bi + 2];
  if (end >= start && after >= end) {
    p.own_ms_sum += static_cast<double>(end - start) / p.ticks_per_ms;
    p.through_ms_sum += static_cast<double>(after - start) / p.ticks_per_ms;
    ++p.timed_count;
  }
  if (p.timed_count >= 600) {
    Log("helper frame on the GPU (start to end, the game's work in between included): its own copies and passes %.2f ms, through DLSS-G's Present %.2f ms; "
        "the Present call %.2f ms on the CPU",
        p.own_ms_sum / p.timed_count, p.through_ms_sum / p.timed_count, p.present_count != 0 ? p.present_ms_sum / p.present_count : 0.0);
    p.own_ms_sum = p.through_ms_sum = p.present_ms_sum = 0;
    p.timed_count = p.present_count = 0;
  }
}

// Records the copies of the frame and its inputs and tags the inputs.
void RecordFrame(Presenter& p, const FrameInputs& in, bool motion, bool reset, bool all_still, const camera::Decision& camera_now, UINT slot, UINT bi, sl::FrameToken& token) {
  p.alloc[bi]->Reset();
  p.list->Reset(p.alloc[bi].Get(), nullptr);
  TakeFrameTimes(p, bi);
  if (p.times != nullptr) p.list->EndQuery(p.time_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3 * bi);
  // The shared slots stay in COMMON: copies promote them to COPY_SOURCE and they decay back after the list.
  if (p.hdr) {
    // An FP16 frame: converted into hdr_frame (kept in UNORDERED_ACCESS between frames), then copied into the swap chain.
    p.hdr_pass.Record(p.list.Get(), bi, 0, p.slots[slot].Get(), p.hdr_frame.Get(), p.pq);
    Barrier(p.list.Get(), p.hdr_frame.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(p.list.Get(), p.back[bi].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    p.list->CopyResource(p.back[bi].Get(), p.hdr_frame.Get());
    Barrier(p.list.Get(), p.back[bi].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Barrier(p.list.Get(), p.hdr_frame.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  } else {
    Barrier(p.list.Get(), p.back[bi].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    p.list->CopyResource(p.back[bi].Get(), p.slots[slot].Get());
    Barrier(p.list.Get(), p.back[bi].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
  }
  if (motion) {
    const auto depth_to = [&](D3D12_RESOURCE_STATES state) {
      if (p.depth_state != state) Barrier(p.list.Get(), p.depth.Get(), p.depth_state, state);
      p.depth_state = state;
    };
    if (in.depth != nullptr) {
      depth_to(D3D12_RESOURCE_STATE_COPY_DEST);
      p.list->CopyResource(p.depth.Get(), in.depth);
      p.depth_constant = false;
    } else if (!p.depth_constant) {
      depth_to(D3D12_RESOURCE_STATE_RENDER_TARGET);
      const float constant[4] = {CONSTANT_DEPTH, 0.f, 0.f, 0.f};
      p.list->ClearRenderTargetView(p.rtv_heap->GetCPUDescriptorHandleForHeapStart(), constant, 0, nullptr);
      p.depth_constant = true;
    }
    depth_to(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const auto mvec_to = [&](D3D12_RESOURCE_STATES state) {
      if (p.mvec_state != state) Barrier(p.list.Get(), p.mvec.Get(), p.mvec_state, state);
      p.mvec_state = state;
    };
    const bool masking = in.checks && p.mask_ready;
    const bool composing = Composing(p, in, motion);
    // The flow everything below works on: the effect's, or NVIDIA Optical Flow's for this frame in the same size, format and scale. A frame the engine has
    // nothing for (its first, or after a failure) keeps the effect's.
    ID3D12Resource* flow = in.motion;
    if (in.nvof) {
      nvof::Frame f;
      f.colour = in.hudless != nullptr ? in.hudless : p.slots[slot].Get();
      f.colour_view = p.colour_view;
      f.depth = in.depth != nullptr ? p.depth.Get() : nullptr;
      f.depth_inverted = in.depth_inverted;
      f.width = p.input_width;
      f.height = p.input_height;
      f.format = p.input_format;
      f.scale[0] = in.motion_scale[0];
      f.scale[1] = in.motion_scale[1];
      f.reset = in.reset;
      f.size = in.nvof_size;
      if (ID3D12Resource* const made = p.nvof.Run(Sl().device.Get(), Sl().queue.Get(), p.list.Get(), f, &NvofLog)) flow = made;
      if (flow == nullptr) flow = p.no_motion.Get();
    } else {
      p.nvof.Forget();
    }
    if (composing) {
      // The mask comes last: compose's averaging would bring the world's motion back onto the HUD.
      ID3D12Resource* const composed_to = masking ? p.composed.Get() : p.mvec.Get();
      if (!masking) mvec_to(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      compose::Camera camera_input;
      if (p.camera_ready) {
        camera_input.colour = in.hudless != nullptr ? in.hudless : p.slots[slot].Get();
        camera_input.clip_to_prev = camera_now.source != camera::Source::none ? camera_now.clip_to_prev.m : nullptr;
      }
      p.compose.Record(p.list.Get(), bi, flow, p.depth.Get(), composed_to, p.input_width, p.input_height, in.depth_inverted, in.motion_scale, reset, p.camera_ready ? &camera_input : nullptr);
      double ms = 0;
      if (p.compose.TakeAverageMs(600, ms)) Log("compose pass: %.2f ms on the GPU at %ux%u (works at %ux%u)", ms, p.input_width, p.input_height, p.compose.WorkWidth(), p.compose.WorkHeight());
    } else {
      p.compose.ForgetHistory();
    }
    if (masking) {
      if (composing) Barrier(p.list.Get(), p.composed.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      mvec_to(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      p.mask.Record(p.list.Get(), bi, in.hudless != nullptr ? in.hudless : p.slots[slot].Get(), composing ? p.composed.Get() : flow, p.mvec.Get(), in.motion_scale, all_still);
      if (composing) Barrier(p.list.Get(), p.composed.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      double ms = 0;
      if (p.mask.TakeAverageMs(600, ms)) Log("HUD mask: %.2f ms on the GPU at %ux%u", ms, p.input_width, p.input_height);
    } else if (!composing) {
      mvec_to(D3D12_RESOURCE_STATE_COPY_DEST);
      // NVOF's result and the zero motion sit in NON_PIXEL_SHADER_RESOURCE; an effect's shared slot is in COMMON and promotes itself.
      const bool own = flow != in.motion;
      if (own) Barrier(p.list.Get(), flow, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
      p.list->CopyResource(p.mvec.Get(), flow);
      if (own) Barrier(p.list.Get(), flow, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    mvec_to(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    const sl::Extent extent{0, 0, p.input_width, p.input_height};
    sl::Resource depth(sl::ResourceType::eTex2d, p.depth.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource mvec(sl::ResourceType::eTex2d, p.mvec.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    // Overlays: the frame from before they were drawn, and where the finished frame differs from it.
    const bool ui = KeepingOverlaysOut(p, in);
    if (ui) {
      const auto to = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES want) {
        if (state != want) Barrier(p.list.Get(), r, state, want);
        state = want;
      };
      if (p.hdr) {
        to(p.hudless.Get(), p.hudless_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        p.hdr_pass.Record(p.list.Get(), bi, 1, in.hudless, p.hudless.Get(), p.pq);
      } else {
        to(p.hudless.Get(), p.hudless_state, D3D12_RESOURCE_STATE_COPY_DEST);
        p.list->CopyResource(p.hudless.Get(), in.hudless);
      }
      to(p.hudless.Get(), p.hudless_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
      to(p.ui_alpha.Get(), p.ui_alpha_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
      p.ui.Record(p.list.Get(), bi, p.slots[slot].Get(), in.hudless, p.ui_alpha.Get());
      to(p.ui_alpha.Get(), p.ui_alpha_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    const sl::Extent frame_extent{0, 0, p.width, p.height};
    sl::Resource hudless(sl::ResourceType::eTex2d, p.hudless.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::Resource ui_alpha(sl::ResourceType::eTex2d, p.ui_alpha.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    sl::ResourceTag tags[] = {sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &extent),
                              sl::ResourceTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &extent),
                              sl::ResourceTag(ui ? &hudless : nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent, &frame_extent),
                              sl::ResourceTag(ui ? &ui_alpha : nullptr, sl::kBufferTypeUIAlpha, sl::ResourceLifecycle::eValidUntilPresent, &frame_extent)};
    slSetTagForFrame(token, VIEWPORT, tags, static_cast<uint32_t>(std::size(tags)), reinterpret_cast<sl::CommandBuffer*>(p.list.Get()));
  } else {
    p.compose.ForgetHistory();
  }
  if (p.times != nullptr) p.list->EndQuery(p.time_queries.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 3 * bi + 1);
  p.list->Close();
}

}  // namespace

// ---- Log ----
void OpenLog(const wchar_t* path, const wchar_t* mode, int share) {
  std::lock_guard<std::mutex> hold(g_log_lock);
  if (g_log == nullptr) g_log = _wfsopen(path, mode, share);
}

void Log(const char* format, ...) {
  char text[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  std::lock_guard<std::mutex> hold(g_log_lock);
  if (g_log == nullptr) return;
  SYSTEMTIME now;
  GetLocalTime(&now);
  std::fprintf(g_log, "%02u:%02u:%02u.%03u %s%s", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, text,
               text[0] != '\0' && text[std::strlen(text) - 1] == '\n' ? "" : "\n");
  std::fflush(g_log);
}

// ---- Streamline ----
Streamline& Sl() {
  static Streamline s;
  return s;
}

bool Fail(const char* format, ...) {
  va_list args;
  va_start(args, format);
  vsnprintf(Sl().error, sizeof(Sl().error), format, args);
  va_end(args);
  Log("%s", Sl().error);
  return false;
}

// Mods that make DLSS-G work on older RTX cards (such as MFG-Unlocker or dlssg_for_sm86). In a game they sit next to the game's exe, but in
// D3D11 and 32-bit games DLSS-G runs in HSF's helper, which they never reach on their own. They hook slInit and NVIDIA's DLL loads, so they
// must be in the process before sl.interposer.dll is. HSF ships none of them: it loads the user's own copy.

// MFG-Unlocker's exported status query (MfgUnlockSingleModuleQuery, MIT): its 64-byte version 1 layout. overlayVisible is 1 while its menu
// is open. Its menu draws on the helper's swap chain, so the helper's window takes the mouse then, ReShade's menu open or not.
struct MfgUnlockerStatus {
  uint32_t size, version, backend_owner, duplicate_suppressed, dxgi_hooked, vulkan_hooked, overlay_visible, render_failures;
  uint32_t shortcut_key, shortcut_binding;
  uint64_t dxgi_frames, vulkan_frames, rendered_frames;
};
static_assert(sizeof(MfgUnlockerStatus) == 64);
using MfgUnlockerQueryFn = BOOL(WINAPI*)(MfgUnlockerStatus*);
MfgUnlockerQueryFn g_mfg_unlocker_query[4] = {};
std::atomic<uint32_t> g_mfg_unlocker_queries{0};

void LoadMod(const std::wstring& path, const char* why) {
  // The altered search path lets a mod find its own files and DLLs next to it.
  const HMODULE mod = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (mod == nullptr) {
    Log("mod NOT loaded (%s): %ls (error %lu)", why, path.c_str(), GetLastError());
    return;
  }
  Log("mod loaded (%s): %ls", why, path.c_str());
  const auto query = reinterpret_cast<MfgUnlockerQueryFn>(GetProcAddress(mod, "MfgUnlockSingleModuleQuery"));
  const uint32_t count = g_mfg_unlocker_queries.load();
  if (query != nullptr && count < std::size(g_mfg_unlocker_query)) {
    g_mfg_unlocker_query[count] = query;
    g_mfg_unlocker_queries.store(count + 1);
    Log("mod %ls reports when its menu is open: the helper's window takes the mouse while it is", path.c_str());
  }
}

bool ModMenuOpen() {
  const uint32_t count = g_mfg_unlocker_queries.load();
  for (uint32_t i = 0; i < count; ++i) {
    MfgUnlockerStatus status{};
    status.size = sizeof(status);
    if (g_mfg_unlocker_query[i](&status) && status.overlay_visible != 0) return true;
  }
  return false;
}

// The product name in a DLL's version resource, or empty.
std::wstring ProductName(const std::wstring& path) {
  DWORD ignored = 0;
  const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &ignored);
  if (size == 0) return {};
  std::vector<uint8_t> data(size);
  if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return {};
  struct Translation { WORD language, codepage; };
  Translation* translations = nullptr;
  UINT bytes = 0;
  if (!VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &bytes) || bytes < sizeof(Translation)) return {};
  wchar_t key[64];
  swprintf(key, std::size(key), L"\\StringFileInfo\\%04x%04x\\ProductName", translations[0].language, translations[0].codepage);
  wchar_t* name = nullptr;
  if (!VerQueryValueW(data.data(), key, reinterpret_cast<void**>(&name), &bytes) || name == nullptr) return {};
  return name;
}

bool IsFile(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

// Every DLL in <folder>\mods (any mod, the user's choice), then the known ones next to the game's exe (the folder above HSF_streamline), found by
// what they are, not their file name, as users rename them to version.dll or winmm.dll. Other DLLs there (ReShade, other mods) are left alone.
void LoadMods(const std::wstring& folder) {
  std::vector<std::wstring> loaded_names;
  const std::wstring mods = folder + L"\\mods";
  WIN32_FIND_DATAW found{};
  HANDLE search = FindFirstFileW((mods + L"\\*.dll").c_str(), &found);
  if (search != INVALID_HANDLE_VALUE) {
    do {
      if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
      LoadMod(mods + L"\\" + found.cFileName, "HSF_streamline\\mods");
      loaded_names.push_back(ProductName(mods + L"\\" + found.cFileName));
    } while (FindNextFileW(search, &found));
    FindClose(search);
  }

  const size_t slash = folder.find_last_of(L'\\');
  if (slash == std::wstring::npos) return;
  const std::wstring game = folder.substr(0, slash);
  // dlssg_for_sm86 has no version resource: it is its proxy DLL next to its own dlssg_sm86 folder.
  const bool sm86_here = IsFile(game + L"\\dlssg_sm86\\backend\\sm86_backend.dll");
  search = FindFirstFileW((game + L"\\*.dll").c_str(), &found);
  if (search == INVALID_HANDLE_VALUE) return;
  do {
    if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    const std::wstring path = game + L"\\" + found.cFileName;
    const std::wstring product = ProductName(path);
    if (product == L"MFG Unlocker") {
      if (std::find(loaded_names.begin(), loaded_names.end(), product) != loaded_names.end()) {
        Log("mod skipped: %ls (already loaded from HSF_streamline\\mods)", path.c_str());
        continue;
      }
      LoadMod(path, "MFG-Unlocker next to the game");
      loaded_names.push_back(product);
    } else if (sm86_here && product.empty()) {
      // Only its proxy names, and never dxgi.dll or d3d12.dll in the helper: those are on its own rendering path.
      static const wchar_t* const PROXY[] = {L"version.dll", L"winmm.dll", L"dbghelp.dll", L"dinput8.dll"};
      for (const wchar_t* name : PROXY)
        if (_wcsicmp(found.cFileName, name) == 0) LoadMod(path, "dlssg_for_sm86 next to the game");
    }
  } while (FindNextFileW(search, &found));
  FindClose(search);
}

bool InitStreamline(const wchar_t* folder, LUID adapter, const wchar_t* adapter_name) {
  Streamline& s = Sl();
  s.folder = folder;
  LoadMods(s.folder);
  const std::wstring dll = s.folder + L"\\sl.interposer.dll";
  s.interposer = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (s.interposer == nullptr) return Fail("Streamline not found: %ls (error %lu)", dll.c_str(), GetLastError());

  // sl.interposer.dll is delay-loaded by name; it is already loaded from the folder above, so the delay-load binds to that copy.
  const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
  const wchar_t* paths[] = {s.folder.c_str()};
  sl::Preferences pref{};
  pref.showConsole = false;
  pref.logLevel = sl::LogLevel::eDefault;
  pref.pathsToPlugins = paths;
  pref.numPathsToPlugins = 1;
  pref.pathToLogsAndData = s.folder.c_str();
  pref.logMessageCallback = SlLog;
  pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  pref.featuresToLoad = features;
  pref.numFeaturesToLoad = static_cast<uint32_t>(std::size(features));
  pref.engine = sl::EngineType::eCustom;
  pref.engineVersion = "1.0";
  pref.projectId = "4f1c2a7e-9b3d-4e8a-a1c6-5d2e7f9b0c13";  // made-up (no NVIDIA app id); Streamline accepts it
  pref.renderAPI = sl::RenderAPI::eD3D12;
  sl::Result res = slInit(pref);
  if (res != sl::Result::eOk) return Fail("slInit failed (%d)", static_cast<int>(res));

  sl::AdapterInfo info{};
  info.deviceLUID = reinterpret_cast<uint8_t*>(&adapter);
  info.deviceLUIDSizeInBytes = sizeof(LUID);
  if ((res = slIsFeatureSupported(sl::kFeatureDLSS_G, info)) != sl::Result::eOk)
    return Fail("DLSS-G is not supported on %ls (Streamline result %d)", adapter_name, static_cast<int>(res));
  return true;
}

bool UseDevice(ID3D12Device* device) {
  Streamline& s = Sl();
  s.device = device;
  const sl::Result res = slSetD3DDevice(device);
  if (res != sl::Result::eOk) return Fail("slSetD3DDevice failed (%d)", static_cast<int>(res));
  D3D12_COMMAND_QUEUE_DESC qdesc{};
  qdesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  const HRESULT hr = device->CreateCommandQueue(&qdesc, IID_PPV_ARGS(&s.queue));
  if (FAILED(hr)) return Fail("HSF's command queue (0x%08lx)", hr);
  sl::DLSSGState state{};
  if (slDLSSGGetState(VIEWPORT, state, nullptr) == sl::Result::eOk) s.max_frames = std::max(1u, state.numFramesToGenerateMax);
  Log("Streamline ready: DLSS-G up to %u generated frames per real one", s.max_frames);
  return true;
}

// ---- Per Start ----
DXGI_FORMAT SwapFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

const char* MakeCommandObjects(Presenter& p, HRESULT& hr) {
  ID3D12Device* const device = Sl().device.Get();
  if (FAILED(hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&p.frame_fence)))) return "HSF's frame fence";
  p.ready_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  p.frame_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  for (auto& alloc : p.alloc)
    if (FAILED(hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)))) return "command allocators";
  if (FAILED(hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p.alloc[0].Get(), nullptr, IID_PPV_ARGS(&p.list))))
    return "the command list";
  p.list->Close();
  // Timing is a report only: without it frames still go out.
  UINT64 frequency = 0;
  bool tail = true;
  for (auto& alloc : p.tail_alloc) tail = tail && SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
  if (tail && SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, p.tail_alloc[0].Get(), nullptr, IID_PPV_ARGS(&p.tail_list))) &&
      SUCCEEDED(Sl().queue->GetTimestampFrequency(&frequency)) && frequency != 0) {
    p.tail_list->Close();
    const D3D12_QUERY_HEAP_DESC query{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 3 * BUFFERS, 0};
    const D3D12_HEAP_PROPERTIES readback{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC buffer{};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 3 * BUFFERS * sizeof(UINT64);
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    void* mapped = nullptr;
    if (SUCCEEDED(device->CreateQueryHeap(&query, IID_PPV_ARGS(&p.time_queries))) &&
        SUCCEEDED(device->CreateCommittedResource(&readback, D3D12_HEAP_FLAG_NONE, &buffer, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&p.time_readback))) &&
        SUCCEEDED(p.time_readback->Map(0, nullptr, &mapped))) {
      p.times = static_cast<const UINT64*>(mapped);
      p.ticks_per_ms = static_cast<double>(frequency) / 1000.0;
    }
  }
  return nullptr;
}

namespace {

// ---- The overlay window (HSF.ini VkOverlayWindow on Vulkan, OverlayWindow on D3D11 and DX9) ----
// A window of its own over the game's, never its child, as AeonSR's frame generation presents (MIT, github.com/BarbatosAWLS/AeonSR,
// overlay_presenter.cpp): a child window on the game's window shares the game thread's input queue, and with the pointer inside the window
// every mouse move went through this thread (Valheim: choppy only with the pointer inside and frame gen on). Layered and transparent, so the
// mouse goes through to the game; never activated, so the game keeps the focus.
// While ReShade's menu is open with MFG-Unlocker loaded, the overlay window docks into the game's window as a child for a moment: the mod's
// menu only opens and takes clicks on a window inside the game's. The mouse may be choppy then; it is smooth again once the menu closes.
std::atomic<bool> g_overlay_docked{false};

LRESULT CALLBACK OverlayProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
  if (message == WM_ERASEBKGND) return 1;
  if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
  if (message == WM_NCHITTEST && !g_overlay_docked.load()) return HTTRANSPARENT;
  if (message == WM_DESTROY) {
    PostQuitMessage(0);
    return 0;
  }
  return DefWindowProcW(hwnd, message, wp, lp);
}

// The game's client area on the screen in `r`; false when the overlay does not belong there (the game's window minimised or not in front).
bool OverlayPlace(HWND game, RECT& r) {
  DWORD front_pid = 0, game_pid = 0;
  if (const HWND front = GetForegroundWindow()) GetWindowThreadProcessId(front, &front_pid);
  GetWindowThreadProcessId(game, &game_pid);
  RECT client{};
  POINT origin{0, 0};
  if (!GetClientRect(game, &client) || !ClientToScreen(game, &origin) || client.right <= 0 || client.bottom <= 0) return false;
  r = {origin.x, origin.y, origin.x + client.right, origin.y + client.bottom};
  return !IsIconic(game) && game_pid != 0 && front_pid == game_pid;
}

void OverlayThread(Presenter& p, const std::atomic<bool>* close_when) {
  static const ATOM atom = [] {
    WNDCLASSW wc{};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = ThisModule();
    wc.lpszClassName = L"HSF_DLSSG_OVERLAY";
    return RegisterClassW(&wc);
  }();
  (void)atom;
  RECT r{};
  OverlayPlace(p.parent, r);
  const LONG_PTR overlay_ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED | WS_EX_TRANSPARENT;
  const HWND window = CreateWindowExW(static_cast<DWORD>(overlay_ex), L"HSF_DLSSG_OVERLAY", L"HSF DLSS-G",
                                      WS_POPUP, r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr, ThisModule(), nullptr);
  if (window == nullptr) {
    Log("creating the overlay window over the game's window %p failed (%lu)", static_cast<void*>(p.parent), GetLastError());
    p.window_failed = true;
    return;
  }
  SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
  Log("overlay window %p over the game's window %p (not a child: no shared input queue): %ldx%ld", static_cast<void*>(window), static_cast<void*>(p.parent),
      r.right - r.left, r.bottom - r.top);
  p.child = window;
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  SetTimer(window, 1, 50, nullptr);
  bool wanted = true, shown = false;  // wanted: the presenter's show/hide (frame gen on or off)
  RECT shown_rect{};
  const auto follow = [&] {
    RECT where{};
    if (!wanted || !OverlayPlace(p.parent, where)) {
      if (shown) ShowWindow(window, SW_HIDE);
      shown = false;
      return;
    }
    if (shown && EqualRect(&where, &shown_rect)) return;
    SetWindowPos(window, HWND_TOPMOST, where.left, where.top, where.right - where.left, where.bottom - where.top, SWP_NOACTIVATE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER);
    if (shown) Log("overlay window follows the game window: %ldx%ld at %ld,%ld", where.right - where.left, where.bottom - where.top, where.left, where.top);
    shown = true;
    shown_rect = where;
  };
  bool docked = false;
  const auto dock = [&](bool on) {
    docked = on;
    g_overlay_docked.store(on);
    if (on) {
      RECT client{};
      GetClientRect(p.parent, &client);
      SetWindowLongPtrW(window, GWL_EXSTYLE, WS_EX_NOPARENTNOTIFY);
      SetWindowLongPtrW(window, GWL_STYLE, WS_CHILD | WS_CLIPSIBLINGS | WS_DISABLED);
      SetParent(window, p.parent);
      SetWindowPos(window, HWND_TOP, 0, 0, client.right, client.bottom, SWP_NOACTIVATE | SWP_FRAMECHANGED | (wanted ? SWP_SHOWWINDOW : SWP_HIDEWINDOW));
      shown = wanted;
      Log("ReShade's menu is open with MFG-Unlocker loaded: the overlay window docks into the game's window, so the mod's menu opens and can be clicked");
    } else {
      SetParent(window, nullptr);
      EnableWindow(window, TRUE);
      SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP);
      SetWindowLongPtrW(window, GWL_EXSTYLE, overlay_ex);
      SetLayeredWindowAttributes(window, 0, 255, LWA_ALPHA);
      SetWindowPos(window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED | SWP_HIDEWINDOW);
      shown = false;
      Log("the menus are closed: the overlay window is on its own over the game again");
      follow();
    }
  };
  follow();
  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (message.message == WM_HSF_SHOW && message.hwnd == window) {
      wanted = message.wParam != 0;
      Log(wanted ? "frames again: the overlay window is shown" : "frame gen off: the overlay window is hidden, the game shows its own frames");
      if (docked) ShowWindow(window, wanted ? SW_SHOWNA : SW_HIDE);
      else follow();
      continue;
    }
    if (message.message == WM_TIMER && message.hwnd == window) {
      if (close_when != nullptr && close_when->load()) {
        DestroyWindow(window);
        continue;
      }
      const bool want_dock = p.take_mouse.load() && g_mfg_unlocker_queries.load() != 0;
      if (want_dock != docked) dock(want_dock);
      if (docked) {
        // Docked, the window lets the mouse through to the game (ReShade's menu) and takes it only while the mod's own menu is open.
        const bool take = ModMenuOpen();
        if (take == !IsWindowEnabled(window)) {
          EnableWindow(window, take ? TRUE : FALSE);
          Log(take ? "MFG-Unlocker's menu is open: the docked window takes the mouse" : "the docked window lets the mouse through to the game");
        }
        RECT client{}, own{};
        GetClientRect(p.parent, &client);
        GetClientRect(window, &own);
        if (client.right > 0 && client.bottom > 0 && (client.right != own.right || client.bottom != own.bottom))
          SetWindowPos(window, nullptr, 0, 0, client.right, client.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
      } else {
        follow();
      }
      continue;
    }
    DispatchMessageW(&message);
  }
}

}  // namespace

void WindowThread(Presenter& p, const std::atomic<bool>* close_when) {
  if (p.overlay) return OverlayThread(p, close_when);
  static const ATOM atom = [] {
    WNDCLASSW wc{};
    wc.lpfnWndProc = ChildProc;
    wc.hInstance = ThisModule();
    wc.lpszClassName = L"HSF_DLSSG";
    return RegisterClassW(&wc);
  }();
  (void)atom;
  RECT client{};
  GetClientRect(p.parent, &client);
  // Disabled: mouse and keyboard go to the game's window. No parent notify: nothing is sent to the game's thread while the child is made.
  const HWND child = CreateWindowExW(WS_EX_NOPARENTNOTIFY, L"HSF_DLSSG", L"HSF DLSS-G", WS_CHILD | WS_VISIBLE | WS_DISABLED | WS_CLIPSIBLINGS, 0, 0,
                                     client.right, client.bottom, p.parent, nullptr, ThisModule(), nullptr);
  if (child == nullptr) {
    Log("creating the child window on the game's window %p failed (%lu)", static_cast<void*>(p.parent), GetLastError());
    p.window_failed = true;
    return;
  }
  Log("child window %p on the game's window %p: %ldx%ld", static_cast<void*>(child), static_cast<void*>(p.parent), client.right, client.bottom);  p.child = child;
  // The game's input queue is shared with this thread: answer its messages without waiting for a free core.
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  SetTimer(child, 1, 100, nullptr);
  // This thread must keep reading input with GetMessage: the input queue is shared with the game's thread, and input this thread leaves
  // unread blocks the game's input too (tried: the game stopped responding after a few seconds).
  MSG message;
  while (GetMessageW(&message, nullptr, 0, 0) > 0) {
    if (message.message == WM_HSF_SHOW && message.hwnd == child) {
      ShowWindow(child, message.wParam != 0 ? SW_SHOWNA : SW_HIDE);
      Log(message.wParam != 0 ? "frames again: the child window is shown" : "frame gen off: the child window is hidden, the game shows its own frames");
      continue;
    }
    if (message.message == WM_TIMER && message.hwnd == child) {
      if (close_when != nullptr && close_when->load()) {
        DestroyWindow(child);  // WM_DESTROY -> WM_QUIT; done here, on its own thread (no race with a WM_CLOSE from elsewhere)
        continue;
      }
      const bool take_mouse = p.take_mouse.load();
      if (take_mouse == !IsWindowEnabled(child)) {
        EnableWindow(child, take_mouse ? TRUE : FALSE);
        Log(take_mouse ? "ReShade's or MFG-Unlocker's menu is open: the child window takes the mouse (a mod's menu on it can be clicked)" : "the child window lets the mouse through to the game again");
      }
      RECT parent_client{}, own{};
      GetClientRect(p.parent, &parent_client);
      GetClientRect(child, &own);
      if (parent_client.right > 0 && parent_client.bottom > 0 && (parent_client.right != own.right || parent_client.bottom != own.bottom)) {
        SetWindowPos(child, nullptr, 0, 0, parent_client.right, parent_client.bottom, SWP_NOZORDER | SWP_NOACTIVATE);
        Log("child window follows the game window: %ldx%ld", parent_client.right, parent_client.bottom);
      }
      continue;
    }
    DispatchMessageW(&message);
  }
}

void Run(Presenter& p, Host& host) {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
  ID3D12CommandQueue* const queue = Sl().queue.Get();
  if (!MakeSwapChain(p, host)) host.SwapChainFailed();
  // Reflex's options and sleep belong to the game's frames (FrameStart, or the helper's Reflex thread); the presenter only uses the game frame's token.
  UINT64 last = 0;
  uint32_t last_index = 0;  // the last frame's Streamline index, 0 before the first
  while (!host.Stopping() && p.swap) {
    if (p.ready->GetCompletedValue() <= last) {
      // Frame gen off: the game sends no frames and shows its own. After a short quiet spell the child goes out of the way.
      const bool on = host.On();
      p.ready->SetEventOnCompletion(last + 1, p.ready_event);
      if (WaitForSingleObject(p.ready_event, on ? 100 : 25) == WAIT_TIMEOUT && !on && !p.hidden && p.child.load() != nullptr) {
        p.hidden = true;
        PostMessageW(p.child.load(), WM_HSF_SHOW, 0, 0);
      }
      continue;
    }
    const UINT64 id = p.ready->GetCompletedValue();
    if (last != 0 && id > last + 1) host.CountSkipped(static_cast<uint32_t>(id - last - 1));
    const UINT slot = static_cast<UINT>(id % RING);
    sl::FrameToken* token = host.GameToken(id);
    // Two frames on one game token (the game presented twice in one of its frames): Streamline refuses a second set of constants for a token, so the
    // second frame gets a token of its own.
    if (token != nullptr && last != 0 && static_cast<uint32_t>(*token) == last_index) token = nullptr;
    if (token == nullptr) {
      // No game frame token (Streamline refused one, or no message got through): a frame of the presenter's own, so the frame still goes out.
      uint32_t index = host.OwnFrameIndex();
      slGetNewFrameToken(token, &index);
      if (token == nullptr) {
        last = id;
        queue->Signal(p.consumed.Get(), id);
        continue;
      }
    }
    FrameInputs in = host.Inputs(id);
    // No flow effect, NVOF on: NVOF makes the flow alone, in a texture of HSF's choosing. Once DLSS-G's inputs exist they set the size, as for an effect's flow.
    const bool nvof_alone = in.motion == nullptr && in.nvof;
    if (nvof_alone) {
      in.motion_width = p.mvec ? p.input_width : in.depth != nullptr ? in.depth_width : p.width;
      in.motion_height = p.mvec ? p.input_height : in.depth != nullptr ? in.depth_height : p.height;
      in.motion_format = p.mvec ? p.input_format : DXGI_FORMAT_R16G16_FLOAT;
      in.motion_scale[0] = 1.f;
      in.motion_scale[1] = 1.f;
    }
    const bool motion = (in.motion != nullptr || nvof_alone) && MakeInputs(p, in);
    // DLSS-G has one size for motion and depth, set by the first motion vectors. A game that changed size since then gets the constant.
    if (in.depth_width != p.input_width || in.depth_height != p.input_height) in.depth = nullptr;
    // Before DLSS-G gets this frame: whether it is a scene cut, or part of a still picture.
    frame_checks::Verdict verdict;
    if (motion && in.checks && p.grid_ready && p.grid.Run(in.hudless != nullptr ? in.hudless : p.slots[slot].Get(), p.width, p.height, verdict)) {
      if (verdict.cut) {
        ++p.cuts_unlogged;
        const ULONGLONG now = GetTickCount64();
        if (now - p.last_cut_log >= 1000) {
          Log("scene cut: %u since the last line; this one changed %.0f%% of the picture, histogram moved %.2f", p.cuts_unlogged, verdict.changed * 100.f, verdict.shape);
          p.last_cut_log = now;
          p.cuts_unlogged = 0;
        }
      }
      // At most one line a second: in some scenes the picture stops and moves several times a second.
      if (verdict.still != p.was_still) {
        ++p.still_changes;
        const ULONGLONG now = GetTickCount64();
        if (now - p.last_still_log >= 1000) {
          Log(verdict.still ? "still picture: zero motion (%u changes since the last line)" : "picture moving again (%u changes since the last line)", p.still_changes);
          p.last_still_log = now;
          p.still_changes = 0;
        }
      }
      p.was_still = verdict.still;
      double ms = 0;
      if (p.grid.TakeAverageWaitMs(600, ms)) Log("frame checks: the presenter waits %.2f ms a frame for the grid", ms);
    }
    const bool reset = last == 0 || id > last + 1 || in.reset || verdict.cut;
    // For NVIDIA Optical Flow a skipped frame is no break: it matches against the last picture it was given, which is the last one shown.
    in.reset = in.reset || verdict.cut;
    // The still world's motion from the camera. Needs the compose pass, and so real depth, like the pass itself.
    camera::Decision camera_now;
    if (motion && in.motion != nullptr && in.camera_motion && p.camera_ready && Composing(p, in, motion)) {
      const int64_t hint = host.CameraHint();
      if (hint != p.last_hint) {
        p.chooser.SetHint(hint);
        p.last_hint = hint;
      }
      p.chooser.SetFitAllowed(in.camera_fit);
      const LONGLONG ticks = QpcNow();
      const float* grid = nullptr;
      if (p.samples.Run(in.motion, in.depth, p.input_width, p.input_height, grid)) {
        static camera::Sample samples[camera::SAMPLES];
        const uint32_t n = camera::MakeSamples(grid, in.motion_scale, samples);
        camera_now = p.chooser.Frame(samples, n, in.camera, in.camera_count, in.camera_frame, reset, static_cast<float>(p.width), static_cast<float>(p.height));
        if (in.camera_record_folder != nullptr) {
          if (!p.flow_file.IsOpen()) {
            CreateDirectoryW(in.camera_record_folder, nullptr);
            SYSTEMTIME t;
            GetLocalTime(&t);
            wchar_t path[MAX_PATH];
            swprintf(path, MAX_PATH, L"%ls\\%04u%02u%02u-%02u%02u%02u-flow.bin", in.camera_record_folder, t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
            Log(p.flow_file.Open(path) ? "camera record: writing %ls" : "camera record: could not write %ls", path);
          }
          camera::record::FlowFrame head{};
          head.camera_frame = in.camera_frame;
          head.id = id;
          head.scale[0] = in.motion_scale[0];
          head.scale[1] = in.motion_scale[1];
          head.width = p.width;
          head.height = p.height;
          head.input_width = p.input_width;
          head.input_height = p.input_height;
          head.reset = reset ? 1 : 0;
          head.depth_inverted = in.depth_inverted ? 1 : 0;
          head.source = static_cast<int32_t>(camera_now.source);
          head.handed = std::clamp(in.camera_count, 0, camera::CANDIDATES);
          head.locked = p.chooser.LockedKey();
          head.share = camera_now.share;
          p.flow_file.Frame(head, in.camera, grid);
        }
        host.CameraResult(camera_now, p.chooser.LockedKey());
        p.camera_ms_sum += QpcMs(QpcNow() - ticks);
        if (++p.camera_ms_count >= 600) {
          Log("camera: the presenter spends %.2f ms a frame on it; %d matrices handed over this frame", p.camera_ms_sum / p.camera_ms_count, in.camera_count);
          p.camera_ms_sum = 0;
          p.camera_ms_count = 0;
          const camera::Chooser::Report report = p.chooser.TakeReport();
          Log("camera: of the last %u frames %u could be judged, %u of those moving, %u of those with candidates; %.0f of %u grid points a frame have depth; %d candidates scored so far", report.frames,
              report.usable, report.moving, report.with_candidates, report.frames != 0 ? static_cast<double>(report.samples) / report.frames : 0.0, camera::SAMPLES, report.tallied);
          if (p.chooser.LockedKey() == 0)
            for (int i = 0; i < report.top_count; ++i) {
              const camera::Chooser::Scored& s = report.top[i];
              Log("camera: candidate: constant buffer of %u bytes at offset %u, %s, written %u times before it in a frame, aspect %.3f: explains %.0f%% over %d frames, %.0f%% last (a lock needs %.0f%% over %d)",
                  camera::KeyWidth(s.key), camera::KeyOffset(s.key), camera::KeyTransposed(s.key) ? "by columns" : "by rows", camera::KeyOccurrence(s.key), s.aspect, s.mean * 100.f, s.frames, s.last * 100.f,
                  camera::Chooser::LOCK_SHARE * 100.f, camera::Chooser::LOCK_FRAMES);
            }
          if (p.chooser.LockedKey() == 0)
            for (const camera::Chooser::Scored& s : report.most_scored)
              if (s.frames != 0)
                Log("camera: scored most: constant buffer of %u bytes at offset %u, %s, written %u times before it in a frame, aspect %.3f: explains %.0f%% over %d frames, %.0f%% last", camera::KeyWidth(s.key), camera::KeyOffset(s.key),
                    camera::KeyTransposed(s.key) ? "by columns" : "by rows", camera::KeyOccurrence(s.key), s.aspect, s.mean * 100.f, s.frames, s.last * 100.f);
        }
        if (camera_now.changed) {
          static const char* const NAMES[] = {"flow only", "fitted from the flow and depth", "the game's matrix"};
          const int64_t key = p.chooser.LockedKey();
          Log("camera: %s, it explains %.0f%% of the flow; matrix locked: %s", NAMES[static_cast<int>(camera_now.source)], camera_now.share * 100.f, key != 0 ? "yes" : "no");
          if (key != 0)
            Log("camera: locked matrix is in a constant buffer of %u bytes at offset %u, %s, written %u times before it in a frame", camera::KeyWidth(key), camera::KeyOffset(key),
                camera::KeyTransposed(key) ? "by columns" : "by rows", camera::KeyOccurrence(key));
        }
      }
    }
    if (in.camera_record_folder == nullptr && p.flow_file.IsOpen()) {
      Log("camera record: done, %u frames", p.flow_file.Frames());
      p.flow_file.Close();
    }
    SetFrameConstants(p, in, motion, reset, camera_now, *token);
    // Generated frames only with motion vectors; without them real frames go out as they are.
    const bool wanted = host.On();
    GuardGpu(p, wanted);
    const bool on = wanted && !p.hang_off;
    p.take_mouse.store(host.TakeMouse() || ModMenuOpen());
    SetDlssgOptions(p, on && motion, std::max(1u, host.Frames()), motion && KeepingOverlaysOut(p, in));

    const UINT bi = p.swap->GetCurrentBackBufferIndex();
    if (p.frame_fence->GetCompletedValue() < p.alloc_value[bi]) {
      p.frame_fence->SetEventOnCompletion(p.alloc_value[bi], p.frame_event);
      WaitForSingleObject(p.frame_event, 1000);
    }
    RecordFrame(p, in, motion, reset, verdict.still, camera_now, slot, bi, *token);
    host.NvofState(in.nvof && motion ? static_cast<int>(p.nvof.GetState()) : 0);
    ID3D12CommandList* const lists[] = {p.list.Get()};
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(p.consumed.Get(), id);  // the slot is free once these copies have run

    slPCLSetMarker(sl::PCLMarker::ePresentStart, *token);
    const LONGLONG present_from = QpcNow();
    const HRESULT hr = p.swap->Present(0, p.tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    p.present_ms_sum += QpcMs(QpcNow() - present_from);
    ++p.present_count;
    slPCLSetMarker(sl::PCLMarker::ePresentEnd, *token);
    EndFrameTimes(p, bi);
    host.AfterPresent(id);
    if (p.hidden && p.child.load() != nullptr) {
      // Shown only after this frame is on the swap chain, so the old picture never flashes.
      p.hidden = false;
      PostMessageW(p.child.load(), WM_HSF_SHOW, 1, 0);
    }
    queue->Signal(p.frame_fence.Get(), ++p.frame_value);
    p.alloc_value[bi] = p.frame_value;
    if (FAILED(hr)) {
      static HRESULT logged = S_OK;
      if (hr != logged) Log("Present failed (0x%08lx)", hr);
      logged = hr;
    }
    last = id;
    last_index = static_cast<uint32_t>(*token);

    sl::DLSSGState state{};
    slDLSSGGetState(VIEWPORT, state, nullptr);
    host.CountFrame(state, motion, on);
  }

  // DLSS-G off and the GPU idle before anything goes away. The game's GPU may be waiting for `consumed`: release it.
  Log("stopping: DLSS-G off");
  if (p.swap) SetDlssgOptions(p, false, p.last_frames, false);
  queue->Signal(p.consumed.Get(), UINT64_MAX >> 1);
  queue->Signal(p.frame_fence.Get(), ++p.frame_value);
  p.frame_fence->SetEventOnCompletion(p.frame_value, p.frame_event);
  WaitForSingleObject(p.frame_event, 2000);
  p.nvof.Shutdown();
  Log("stopping: GPU idle, untagging");
  if (p.swap) {
    sl::ResourceTag none[] = {sl::ResourceTag(nullptr, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
                              sl::ResourceTag(nullptr, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent),
                              sl::ResourceTag(nullptr, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent),
                              sl::ResourceTag(nullptr, sl::kBufferTypeUIAlpha, sl::ResourceLifecycle::eValidUntilPresent)};
    sl::FrameToken* end_token = nullptr;
    uint32_t end_index = host.OwnFrameIndex();
    slGetNewFrameToken(end_token, &end_index);
    if (end_token != nullptr) slSetTagForFrame(*end_token, VIEWPORT, none, static_cast<uint32_t>(std::size(none)), nullptr);
  }
  Log("stopping: releasing the swap chain");
  for (auto& buffer : p.back) buffer.Reset();
  p.swap.Reset();
}

}  // namespace hsf::dlssg_core
