// A stand-in D3D12 game for tools\stack_sweep.ps1 (build\x64\d3d12_scene.exe). It holds no HSF code: it acts as a game does, so ReShade, HSF and other
// add-ons load into it from its folder as they would into a real game.
//   With --sl <sl.interposer.dll>, it is a game that ships Streamline: it loads the interposer, starts Streamline with DLSS-G, Reflex and PCL, makes its
//   device and DXGI factory through the interposer's exports, and each frame sends a frame token, Reflex sleep and markers, its depth, motion and hudless
//   picture as frame tags, and the camera constants, then turns DLSS-G on. Without --sl it is a plain D3D12 game.
//   --order reshade-first (default) loads dxgi.dll from its folder (ReShade) before the interposer; --order sl-first loads the interposer first.
// The picture: still grey blocks behind, and bright boxes that move across, with depth and motion vectors that match. A box in a corner is the HUD: it is
// on the screen and not in the hudless picture.
// Options: --size WxH (1280x720), --borderless, --hdr10, --fp16, --flags 0xNNN (swap chain flags, e.g. 0x842 as Resident Evil 7 asks), --fps N (60),
// --frames-gen N (frames DLSS-G makes per real frame, default its most), --seconds N (20), --log <path> (d3d12_scene.log next to the exe).
// Once a second it logs what DLSS-G says. Exit code 0 when it ran to the end, 3 when it could not start.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT BUFFERS = 3;
constexpr UINT BOXES = 4;

FILE* g_log = nullptr;

void Line(const char* format, ...) {
  char text[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  std::printf("scene: %s\n", text);
  std::fflush(stdout);
  if (g_log != nullptr) {
    std::fprintf(g_log, "%s\n", text);
    std::fflush(g_log);
  }
}

struct Options {
  std::wstring sl;  // path to sl.interposer.dll, empty for a plain game
  bool sl_first = false;
  UINT width = 1280, height = 720;
  bool borderless = false, hdr10 = false, fp16 = false;
  UINT flags = 0;
  UINT fps = 60;
  UINT frames_gen = 0;  // 0 = DLSS-G's most
  UINT seconds = 20;
  std::wstring log;
};

bool ParseArgs(int argc, wchar_t** argv, Options& o) {
  for (int i = 1; i < argc; ++i) {
    const std::wstring a = argv[i];
    const bool more = i + 1 < argc;
    if (a == L"--sl" && more) o.sl = argv[++i];
    else if (a == L"--order" && more) o.sl_first = std::wstring(argv[++i]) == L"sl-first";
    else if (a == L"--size" && more) {
      if (swscanf_s(argv[++i], L"%ux%u", &o.width, &o.height) != 2) return false;
    } else if (a == L"--borderless") o.borderless = true;
    else if (a == L"--hdr10") o.hdr10 = true;
    else if (a == L"--fp16") o.fp16 = true;
    else if (a == L"--flags" && more) o.flags = static_cast<UINT>(wcstoul(argv[++i], nullptr, 0));
    else if (a == L"--fps" && more) o.fps = std::max(1u, static_cast<UINT>(_wtoi(argv[++i])));
    else if (a == L"--frames-gen" && more) o.frames_gen = static_cast<UINT>(_wtoi(argv[++i]));
    else if (a == L"--seconds" && more) o.seconds = std::max(1u, static_cast<UINT>(_wtoi(argv[++i])));
    else if (a == L"--log" && more) o.log = argv[++i];
    else return false;
  }
  return true;
}

// ---- Streamline, as a game calls it ----

struct Streamline {
  HMODULE module = nullptr;
  PFun_slInit* init = nullptr;
  PFun_slSetD3DDevice* set_device = nullptr;
  PFun_slIsFeatureLoaded* is_loaded = nullptr;
  PFun_slGetFeatureFunction* get_function = nullptr;
  PFun_slGetNewFrameToken* new_token = nullptr;
  PFun_slSetTagForFrame* tag = nullptr;
  PFun_slSetConstants* constants = nullptr;
  PFun_slDLSSGSetOptions* dlssg_options = nullptr;
  PFun_slDLSSGGetState* dlssg_state = nullptr;
  PFun_slReflexSetOptions* reflex_options = nullptr;
  PFun_slReflexSleep* reflex_sleep = nullptr;
  PFun_slPCLSetMarker* marker = nullptr;
  bool started = false;
} g_sl;

template <typename T>
bool Export(HMODULE module, T*& out, const char* name) {
  out = reinterpret_cast<T*>(GetProcAddress(module, name));
  if (out == nullptr) Line("export missing: %s", name);
  return out != nullptr;
}

bool LoadStreamline(const std::wstring& path) {
  g_sl.module = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (g_sl.module == nullptr) {
    Line("sl.interposer.dll did not load: %ls (error %lu)", path.c_str(), GetLastError());
    return false;
  }
  Line("sl.interposer.dll loaded: %ls", path.c_str());
  return Export(g_sl.module, g_sl.init, "slInit") && Export(g_sl.module, g_sl.set_device, "slSetD3DDevice") &&
         Export(g_sl.module, g_sl.is_loaded, "slIsFeatureLoaded") && Export(g_sl.module, g_sl.get_function, "slGetFeatureFunction") &&
         Export(g_sl.module, g_sl.new_token, "slGetNewFrameToken") && Export(g_sl.module, g_sl.tag, "slSetTagForFrame") &&
         Export(g_sl.module, g_sl.constants, "slSetConstants");
}

bool StartStreamline(const std::wstring& path) {
  static std::wstring folder;
  folder = path.substr(0, path.find_last_of(L"\\/"));
  const wchar_t* paths[] = {folder.c_str()};
  const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
  sl::Preferences pref{};
  pref.showConsole = false;
  pref.logLevel = sl::LogLevel::eOff;
  pref.pathsToPlugins = paths;
  pref.numPathsToPlugins = 1;
  pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  pref.featuresToLoad = features;
  pref.numFeaturesToLoad = static_cast<uint32_t>(std::size(features));
  pref.engine = sl::EngineType::eCustom;
  pref.engineVersion = "1.0";
  pref.projectId = "0d7c3b1e-5a2f-4c8e-9b6d-3e1f7a9c2b54";  // made-up
  pref.renderAPI = sl::RenderAPI::eD3D12;
  const sl::Result result = g_sl.init(pref, sl::kSDKVersion);
  Line("slInit: %d", static_cast<int>(result));
  return result == sl::Result::eOk;
}

template <typename T>
void FeatureFunction(sl::Feature feature, const char* name, T*& out) {
  void* call = nullptr;
  const sl::Result got = g_sl.get_function(feature, name, call);
  out = got == sl::Result::eOk ? reinterpret_cast<T*>(call) : nullptr;
  if (out == nullptr) Line("%s: %d", name, static_cast<int>(got));
}

// ---- Window ----

bool g_quit = false;

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
  if (message == WM_CLOSE || (message == WM_KEYDOWN && w == VK_ESCAPE)) {
    g_quit = true;
    return 0;
  }
  return DefWindowProcW(window, message, w, l);
}

HWND MakeWindow(const Options& o) {
  WNDCLASSW wc{};
  wc.lpfnWndProc = WindowProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.lpszClassName = L"HsfD3d12Scene";
  RegisterClassW(&wc);
  DWORD style = WS_OVERLAPPEDWINDOW;
  RECT rect{0, 0, static_cast<LONG>(o.width), static_cast<LONG>(o.height)};
  int x = 80, y = 80;
  if (o.borderless) {
    style = WS_POPUP;
    rect = {0, 0, GetSystemMetrics(SM_CXSCREEN), GetSystemMetrics(SM_CYSCREEN)};
    x = y = 0;
  } else {
    AdjustWindowRect(&rect, style, FALSE);
  }
  HWND window = CreateWindowExW(0, wc.lpszClassName, L"HSF D3D12 scene", style | WS_VISIBLE, x, y, rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
                                wc.hInstance, nullptr);
  if (window != nullptr) {
    SetForegroundWindow(window);
    SetFocus(window);
  }
  return window;
}

// ---- Drawing ----

struct Frame {
  ComPtr<ID3D12CommandAllocator> allocator;
  UINT64 fence_value = 0;
};

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = resource;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  return b;
}

ComPtr<ID3D12Resource> MakeTexture(ID3D12Device* device, UINT width, UINT height, DXGI_FORMAT format, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state,
                                   const D3D12_CLEAR_VALUE* clear) {
  D3D12_HEAP_PROPERTIES heap{};
  heap.Type = D3D12_HEAP_TYPE_DEFAULT;
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = width;
  desc.Height = height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.Format = format;
  desc.SampleDesc.Count = 1;
  desc.Flags = flags;
  ComPtr<ID3D12Resource> texture;
  if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(&texture)))) return nullptr;
  return texture;
}

void Identity(sl::float4x4& m) {
  m = {};
  m.row[0].x = m.row[1].y = m.row[2].z = m.row[3].w = 1.0f;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  Options o;
  if (!ParseArgs(argc, argv, o)) {
    std::printf("usage: d3d12_scene [--sl <sl.interposer.dll>] [--order reshade-first|sl-first] [--size WxH] [--borderless] [--hdr10|--fp16] [--flags 0xNNN] [--fps N] "
                "[--frames-gen N] [--seconds N] [--log <path>]\n");
    return 3;
  }
  if (o.log.empty()) {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    o.log = exe;
    o.log = o.log.substr(0, o.log.find_last_of(L"\\/")) + L"\\d3d12_scene.log";
  }
  _wfopen_s(&g_log, o.log.c_str(), L"w");
  Line("start: streamline %ls, order %s, %ux%u %s, %s, swap chain flags 0x%x, %u fps, %u s", o.sl.empty() ? L"none" : o.sl.c_str(), o.sl_first ? "sl-first" : "reshade-first",
       o.width, o.height, o.borderless ? "borderless" : "windowed", o.hdr10 ? "HDR10" : o.fp16 ? "FP16" : "SDR", o.flags, o.fps, o.seconds);

  // Load order: which of ReShade (dxgi.dll in this folder) and Streamline comes first.
  const bool with_sl = !o.sl.empty();
  HMODULE dxgi = nullptr, d3d12 = nullptr;
  if (with_sl && o.sl_first && !LoadStreamline(o.sl)) return 3;
  dxgi = LoadLibraryW(L"dxgi.dll");
  d3d12 = LoadLibraryW(L"d3d12.dll");
  if (dxgi == nullptr || d3d12 == nullptr) {
    Line("dxgi.dll or d3d12.dll did not load");
    return 3;
  }
  if (with_sl && !o.sl_first && !LoadStreamline(o.sl)) return 3;
  wchar_t dxgi_path[MAX_PATH] = {};
  GetModuleFileNameW(dxgi, dxgi_path, MAX_PATH);
  Line("dxgi.dll is %ls", dxgi_path);
  if (with_sl) g_sl.started = StartStreamline(o.sl);

  // A Streamline game makes its device and factory through the interposer's exports.
  using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
  using CreateFactoryFn = HRESULT(WINAPI*)(UINT, REFIID, void**);
  const HMODULE device_from = g_sl.started ? g_sl.module : d3d12;
  const HMODULE factory_from = g_sl.started ? g_sl.module : dxgi;
  const auto create_device = reinterpret_cast<CreateDeviceFn>(GetProcAddress(device_from, "D3D12CreateDevice"));
  const auto create_factory = reinterpret_cast<CreateFactoryFn>(GetProcAddress(factory_from, "CreateDXGIFactory2"));
  if (create_device == nullptr || create_factory == nullptr) {
    Line("D3D12CreateDevice or CreateDXGIFactory2 not found");
    return 3;
  }
  ComPtr<IDXGIFactory4> factory;
  ComPtr<ID3D12Device> device;
  HRESULT hr = create_factory(0, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    Line("CreateDXGIFactory2: 0x%08lx", hr);
    return 3;
  }
  hr = create_device(nullptr, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&device));
  if (FAILED(hr)) {
    Line("D3D12CreateDevice: 0x%08lx", hr);
    return 3;
  }
  if (g_sl.started) Line("slSetD3DDevice: %d", static_cast<int>(g_sl.set_device(device.Get())));

  D3D12_COMMAND_QUEUE_DESC queue_desc{};
  queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
  ComPtr<ID3D12CommandQueue> queue;
  if (FAILED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue)))) return 3;

  const HWND window = MakeWindow(o);
  if (window == nullptr) return 3;
  RECT client{};
  GetClientRect(window, &client);
  const UINT width = static_cast<UINT>(client.right - client.left), height = static_cast<UINT>(client.bottom - client.top);

  const DXGI_FORMAT format = o.hdr10 ? DXGI_FORMAT_R10G10B10A2_UNORM : o.fp16 ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R8G8B8A8_UNORM;
  DXGI_SWAP_CHAIN_DESC1 sc{};
  sc.Width = width;
  sc.Height = height;
  sc.Format = format;
  sc.SampleDesc.Count = 1;
  sc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sc.BufferCount = BUFFERS;
  sc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
  sc.Flags = o.flags;
  ComPtr<IDXGISwapChain1> swap1;
  hr = factory->CreateSwapChainForHwnd(queue.Get(), window, &sc, nullptr, nullptr, &swap1);
  Line("CreateSwapChainForHwnd: 0x%08lx", hr);
  if (FAILED(hr)) return 3;
  ComPtr<IDXGISwapChain3> swap;
  if (FAILED(swap1.As(&swap))) return 3;
  factory->MakeWindowAssociation(window, DXGI_MWA_NO_ALT_ENTER);
  if (o.hdr10 || o.fp16) {
    const DXGI_COLOR_SPACE_TYPE space = o.hdr10 ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709;
    Line("SetColorSpace1(%d): 0x%08lx", static_cast<int>(space), swap->SetColorSpace1(space));
  }
  HANDLE waitable = nullptr;
  if ((o.flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT) != 0) {
    ComPtr<IDXGISwapChain2> swap2;
    if (SUCCEEDED(swap.As(&swap2))) waitable = swap2->GetFrameLatencyWaitableObject();
  }
  const bool tearing = (o.flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;

  // Streamline's features need the device first.
  sl::ViewportHandle viewport{0};
  UINT gen_max = 0;
  if (g_sl.started) {
    bool loaded = false;
    g_sl.is_loaded(sl::kFeatureDLSS_G, loaded);
    Line("DLSS-G loaded: %d", loaded ? 1 : 0);
    FeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", g_sl.dlssg_options);
    FeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", g_sl.dlssg_state);
    FeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", g_sl.reflex_options);
    FeatureFunction(sl::kFeatureReflex, "slReflexSleep", g_sl.reflex_sleep);
    FeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", g_sl.marker);
    if (g_sl.reflex_options != nullptr) {
      sl::ReflexOptions reflex{};
      reflex.mode = sl::ReflexMode::eLowLatency;
      reflex.frameLimitUs = 1000000u / o.fps;
      Line("slReflexSetOptions: %d", static_cast<int>(g_sl.reflex_options(reflex)));
    }
    if (g_sl.dlssg_state != nullptr) {
      sl::DLSSGState state{};
      g_sl.dlssg_state(viewport, state, nullptr);
      gen_max = state.numFramesToGenerateMax;
      Line("DLSS-G most frames per real frame: %u", gen_max);
    }
  }

  // Render targets: the scene (hudless), depth, motion.
  D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{};
  rtv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_desc.NumDescriptors = BUFFERS + 2;
  ComPtr<ID3D12DescriptorHeap> rtv_heap;
  D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{};
  dsv_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
  dsv_desc.NumDescriptors = 1;
  ComPtr<ID3D12DescriptorHeap> dsv_heap;
  if (FAILED(device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv_heap))) || FAILED(device->CreateDescriptorHeap(&dsv_desc, IID_PPV_ARGS(&dsv_heap)))) return 3;
  const UINT rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
  const auto rtv = [&](UINT i) {
    D3D12_CPU_DESCRIPTOR_HANDLE h = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    h.ptr += static_cast<SIZE_T>(i) * rtv_step;
    return h;
  };
  ComPtr<ID3D12Resource> back[BUFFERS];
  for (UINT i = 0; i < BUFFERS; ++i) {
    if (FAILED(swap->GetBuffer(i, IID_PPV_ARGS(&back[i])))) return 3;
    device->CreateRenderTargetView(back[i].Get(), nullptr, rtv(i));
  }
  const D3D12_RESOURCE_STATES READ = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
  ComPtr<ID3D12Resource> scene = MakeTexture(device.Get(), width, height, format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, READ, nullptr);
  ComPtr<ID3D12Resource> motion = MakeTexture(device.Get(), width, height, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, READ, nullptr);
  D3D12_CLEAR_VALUE depth_clear{};
  depth_clear.Format = DXGI_FORMAT_D32_FLOAT;
  depth_clear.DepthStencil.Depth = 1.0f;
  ComPtr<ID3D12Resource> depth = MakeTexture(device.Get(), width, height, DXGI_FORMAT_R32_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, READ, &depth_clear);
  if (!scene || !motion || !depth) return 3;
  device->CreateRenderTargetView(scene.Get(), nullptr, rtv(BUFFERS));
  device->CreateRenderTargetView(motion.Get(), nullptr, rtv(BUFFERS + 1));
  D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};
  dsv.Format = DXGI_FORMAT_D32_FLOAT;
  dsv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
  device->CreateDepthStencilView(depth.Get(), &dsv, dsv_heap->GetCPUDescriptorHandleForHeapStart());

  Frame frames[BUFFERS];
  for (Frame& f : frames)
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.allocator)))) return 3;
  ComPtr<ID3D12GraphicsCommandList> list;
  if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames[0].allocator.Get(), nullptr, IID_PPV_ARGS(&list)))) return 3;
  list->Close();
  ComPtr<ID3D12Fence> fence;
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return 3;
  HANDLE fence_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  UINT64 fence_next = 1;

  // HDR10 and FP16 want brighter numbers for the same look; the picture only has to move, not look right.
  const float bright = o.fp16 ? 2.0f : 1.0f;
  const LONG box = static_cast<LONG>(height / 6);
  const float speed = static_cast<float>(width) / (4.0f * static_cast<float>(o.fps));  // a box crosses the screen in 4 s
  bool options_sent = false;
  UINT64 count = 0;
  UINT presented_seen = 0;
  LARGE_INTEGER freq, start, now;
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&start);
  double next_frame = 0.0;
  UINT next_report = 1;
  UINT presents_this_second = 0;
  bool ok = true;

  while (!g_quit) {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }
    QueryPerformanceCounter(&now);
    const double t = static_cast<double>(now.QuadPart - start.QuadPart) / static_cast<double>(freq.QuadPart);
    if (t >= o.seconds) break;
    if (t >= next_report) {
      // DLSS-G makes nothing while the window has no focus, so each line says whether it had it.
      const char* focus = GetForegroundWindow() == window ? "focus" : "NO FOCUS";
      if (g_sl.dlssg_state != nullptr) {
        sl::DLSSGState state{};
        const sl::Result r = g_sl.dlssg_state(viewport, state, nullptr);
        Line("second %u: %u real frames, %s, DLSS-G state %d status 0x%x presented %u", next_report, presents_this_second, focus, static_cast<int>(r),
             static_cast<unsigned>(state.status), state.numFramesActuallyPresented);
        presented_seen = std::max(presented_seen, state.numFramesActuallyPresented);
      } else {
        Line("second %u: %u real frames, %s", next_report, presents_this_second, focus);
      }
      presents_this_second = 0;
      ++next_report;
    }
    // Without Streamline's Reflex the scene holds its own frame rate.
    if (g_sl.reflex_sleep == nullptr) {
      if (t < next_frame) {
        Sleep(static_cast<DWORD>(std::max(0.0, (next_frame - t) * 1000.0 - 0.5)));
        continue;
      }
      next_frame = std::max(next_frame + 1.0 / o.fps, t);
    }
    if (waitable != nullptr) WaitForSingleObjectEx(waitable, 1000, TRUE);

    sl::FrameToken* token = nullptr;
    if (g_sl.started) {
      const uint32_t index = static_cast<uint32_t>(count);
      g_sl.new_token(token, &index);
      if (g_sl.reflex_sleep != nullptr) g_sl.reflex_sleep(*token);
      if (g_sl.marker != nullptr) g_sl.marker(sl::PCLMarker::eSimulationStart, *token);
      if (g_sl.marker != nullptr) g_sl.marker(sl::PCLMarker::eSimulationEnd, *token);
      if (g_sl.marker != nullptr) g_sl.marker(sl::PCLMarker::eRenderSubmitStart, *token);
    }

    const UINT i = swap->GetCurrentBackBufferIndex();
    Frame& f = frames[i];
    if (fence->GetCompletedValue() < f.fence_value) {
      fence->SetEventOnCompletion(f.fence_value, fence_event);
      WaitForSingleObject(fence_event, 2000);
    }
    f.allocator->Reset();
    list->Reset(f.allocator.Get(), nullptr);

    D3D12_RESOURCE_BARRIER to_write[] = {Transition(scene.Get(), READ, D3D12_RESOURCE_STATE_RENDER_TARGET),
                                         Transition(motion.Get(), READ, D3D12_RESOURCE_STATE_RENDER_TARGET),
                                         Transition(depth.Get(), READ, D3D12_RESOURCE_STATE_DEPTH_WRITE)};
    list->ResourceBarrier(3, to_write);
    const float background[4] = {0.05f * bright, 0.06f * bright, 0.08f * bright, 1.0f};
    list->ClearRenderTargetView(rtv(BUFFERS), background, 0, nullptr);
    const float still_motion[4] = {0, 0, 0, 0};
    list->ClearRenderTargetView(rtv(BUFFERS + 1), still_motion, 0, nullptr);
    list->ClearDepthStencilView(dsv_heap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    // Still blocks behind, at middle depth.
    for (LONG y = box / 2; y + box / 2 < static_cast<LONG>(height); y += box * 2) {
      for (LONG x = box / 2; x + box / 2 < static_cast<LONG>(width); x += box * 2) {
        const D3D12_RECT r{x, y, x + box / 2, y + box / 2};
        const float grey[4] = {0.3f * bright, 0.3f * bright, 0.3f * bright, 1.0f};
        list->ClearRenderTargetView(rtv(BUFFERS), grey, 1, &r);
        list->ClearDepthStencilView(dsv_heap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 0.8f, 0, 1, &r);
      }
    }
    // Moving boxes in front: motion points back to where each pixel was last frame, in pixels.
    for (UINT b = 0; b < BOXES; ++b) {
      const float travel = std::fmod(static_cast<float>(count) * speed * (1.0f + 0.5f * b), static_cast<float>(width + box));
      const LONG x = static_cast<LONG>(travel) - box;
      const LONG y = static_cast<LONG>(height) * static_cast<LONG>(b + 1) / (BOXES + 1) - box / 2;
      const D3D12_RECT r{std::max(0L, x), y, std::min(static_cast<LONG>(width), x + box), y + box};
      if (r.right <= r.left) continue;
      const float colour[4] = {(b == 0 ? 1.0f : 0.2f) * bright, (b == 1 ? 1.0f : 0.3f) * bright, (b >= 2 ? 1.0f : 0.2f) * bright, 1.0f};
      const float back_motion[4] = {-speed * (1.0f + 0.5f * b), 0, 0, 0};
      list->ClearRenderTargetView(rtv(BUFFERS), colour, 1, &r);
      list->ClearRenderTargetView(rtv(BUFFERS + 1), back_motion, 1, &r);
      list->ClearDepthStencilView(dsv_heap->GetCPUDescriptorHandleForHeapStart(), D3D12_CLEAR_FLAG_DEPTH, 0.3f, 0, 1, &r);
    }
    D3D12_RESOURCE_BARRIER to_read[] = {Transition(scene.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                        Transition(motion.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, READ),
                                        Transition(depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE, READ),
                                        Transition(back[i].Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)};
    list->ResourceBarrier(4, to_read);
    list->CopyResource(back[i].Get(), scene.Get());
    D3D12_RESOURCE_BARRIER after_copy[] = {Transition(scene.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, READ),
                                           Transition(back[i].Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_RENDER_TARGET)};
    list->ResourceBarrier(2, after_copy);
    // The HUD: on the screen only.
    const D3D12_RECT hud{16, 16, 16 + box, 16 + box / 3};
    const float hud_colour[4] = {0.9f * bright, 0.9f * bright, 0.2f * bright, 1.0f};
    list->ClearRenderTargetView(rtv(i), hud_colour, 1, &hud);

    if (g_sl.started && token != nullptr) {
      sl::Resource depth_res(sl::ResourceType::eTex2d, depth.Get(), static_cast<uint32_t>(READ));
      sl::Resource motion_res(sl::ResourceType::eTex2d, motion.Get(), static_cast<uint32_t>(READ));
      sl::Resource hudless_res(sl::ResourceType::eTex2d, scene.Get(), static_cast<uint32_t>(READ));
      const sl::ResourceTag tags[] = {sl::ResourceTag(&depth_res, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent),
                                      sl::ResourceTag(&motion_res, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent),
                                      sl::ResourceTag(&hudless_res, sl::kBufferTypeHUDLessColor, sl::ResourceLifecycle::eValidUntilPresent)};
      const sl::Result tagged = g_sl.tag(*token, viewport, tags, static_cast<uint32_t>(std::size(tags)), reinterpret_cast<sl::CommandBuffer*>(list.Get()));
      if (count < 3) Line("slSetTagForFrame: %d", static_cast<int>(tagged));
      sl::Constants c{};
      Identity(c.cameraViewToClip);
      Identity(c.clipToCameraView);
      Identity(c.clipToLensClip);
      Identity(c.clipToPrevClip);
      Identity(c.prevClipToClip);
      c.jitterOffset = {0, 0};
      c.mvecScale = {1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height)};
      c.cameraPinholeOffset = {0, 0};
      c.cameraPos = {0, 0, 0};
      c.cameraUp = {0, 1, 0};
      c.cameraRight = {1, 0, 0};
      c.cameraFwd = {0, 0, 1};
      c.cameraNear = 0.1f;
      c.cameraFar = 1000.0f;
      c.cameraFOV = 1.0f;
      c.cameraAspectRatio = static_cast<float>(width) / static_cast<float>(height);
      c.motionVectorsInvalidValue = 0.0f;
      c.depthInverted = sl::Boolean::eFalse;
      c.cameraMotionIncluded = sl::Boolean::eTrue;
      c.motionVectors3D = sl::Boolean::eFalse;
      c.reset = count == 0 ? sl::Boolean::eTrue : sl::Boolean::eFalse;
      const sl::Result set = g_sl.constants(c, *token, viewport);
      if (count < 3) Line("slSetConstants: %d", static_cast<int>(set));
      if (!options_sent && g_sl.dlssg_options != nullptr) {
        sl::DLSSGOptions dlssg{};
        dlssg.mode = sl::DLSSGMode::eOn;
        dlssg.numFramesToGenerate = std::max(1u, o.frames_gen != 0 ? std::min(o.frames_gen, std::max(gen_max, 1u)) : gen_max);
        dlssg.numBackBuffers = BUFFERS;
        dlssg.mvecDepthWidth = width;
        dlssg.mvecDepthHeight = height;
        dlssg.colorWidth = width;
        dlssg.colorHeight = height;
        dlssg.colorBufferFormat = format;
        dlssg.mvecBufferFormat = DXGI_FORMAT_R16G16_FLOAT;
        dlssg.depthBufferFormat = DXGI_FORMAT_R32_FLOAT;
        dlssg.hudLessBufferFormat = format;
        Line("slDLSSGSetOptions on, %u per real frame: %d", dlssg.numFramesToGenerate, static_cast<int>(g_sl.dlssg_options(viewport, dlssg)));
        options_sent = true;
      }
    }

    const D3D12_RESOURCE_BARRIER to_present = Transition(back[i].Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    list->ResourceBarrier(1, &to_present);
    list->Close();
    ID3D12CommandList* lists[] = {list.Get()};
    queue->ExecuteCommandLists(1, lists);
    if (g_sl.started && token != nullptr) {
      if (g_sl.marker != nullptr) g_sl.marker(sl::PCLMarker::eRenderSubmitEnd, *token);
      if (g_sl.marker != nullptr) g_sl.marker(sl::PCLMarker::ePresentStart, *token);
    }
    // Vsync off, as games with frame generation run; the frame rate is held by Reflex or the wait above.
    hr = swap->Present(0, tearing ? DXGI_PRESENT_ALLOW_TEARING : 0);
    if (g_sl.started && token != nullptr && g_sl.marker != nullptr) g_sl.marker(sl::PCLMarker::ePresentEnd, *token);
    if (FAILED(hr)) {
      Line("Present: 0x%08lx", hr);
      ok = false;
      break;
    }
    queue->Signal(fence.Get(), fence_next);
    f.fence_value = fence_next++;
    ++count;
    ++presents_this_second;
  }

  queue->Signal(fence.Get(), fence_next);
  fence->SetEventOnCompletion(fence_next, fence_event);
  WaitForSingleObject(fence_event, 5000);
  Line("end: %s, %llu real frames, most DLSS-G presented per frame %u", ok ? "ran to the end" : "stopped on an error", count, presented_seen);
  // A game shuts Streamline down before it quits; without it Streamline's own teardown fails fast (0xC0000409) as the process ends.
  if (g_sl.started) {
    const auto shutdown = reinterpret_cast<PFun_slShutdown*>(GetProcAddress(g_sl.module, "slShutdown"));
    if (shutdown != nullptr) {
      swap.Reset();
      swap1.Reset();
      Line("slShutdown: %d", static_cast<int>(shutdown()));
    }
  }
  Line("exit");
  if (g_log != nullptr) std::fclose(g_log);
  ExitProcess(ok ? 0 : 4);
}
