// See sl_bring.h. Streamline is loaded by hand and its calls looked up by name, so the add-on keeps loading in games and folders that have none.
#include "sl_bring.h"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_core_api.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>
#include <wrl/client.h>

#include <share.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <memory>
#include <string>
#include <type_traits>

#include "compose.h"
#include "debug_view.h"
#include "frame_checks.h"
#include "nvof_flow.h"

namespace hsf::slbring {
namespace {

using Microsoft::WRL::ComPtr;

// Places in the factory's method table. IDXGIFactory and IDXGIFactory2 are fixed by COM.
constexpr int SLOT_CREATE_SWAP_CHAIN = 10;
constexpr int SLOT_CREATE_FOR_HWND = 15;
// The same for the swap chain's Present and Present1.
constexpr int SLOT_PRESENT = 8;
constexpr int SLOT_PRESENT1 = 22;
const sl::ViewportHandle VIEWPORT{0};

using CreateFactory2Fn = HRESULT(WINAPI*)(UINT, REFIID, void**);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using CreateForHwndFn = HRESULT(STDMETHODCALLTYPE*)(IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

std::wstring g_log_path;
std::wstring g_folder;
SRWLOCK g_log_lock = SRWLOCK_INIT;
FILE* g_log = nullptr;

HMODULE g_interposer = nullptr;
CreateFactory2Fn g_sl_create_factory = nullptr;
PFun_slSetD3DDevice* g_sl_set_device = nullptr;
PFun_slIsFeatureLoaded* g_sl_is_loaded = nullptr;
PFun_slIsFeatureSupported* g_sl_is_supported = nullptr;
PFun_slGetFeatureFunction* g_sl_get_function = nullptr;
PFun_slDLSSGGetState* g_sl_dlssg_state = nullptr;
PFun_slGetNewFrameToken* g_sl_new_token = nullptr;
PFun_slSetTagForFrame* g_sl_tag = nullptr;
PFun_slSetConstants* g_sl_constants = nullptr;
PFun_slDLSSGSetOptions* g_sl_dlssg_options = nullptr;
PFun_slReflexSetOptions* g_sl_reflex_options = nullptr;
PFun_slReflexSleep* g_sl_reflex_sleep = nullptr;
PFun_slPCLSetMarker* g_sl_marker = nullptr;
PFun_slUpgradeInterface* g_sl_upgrade = nullptr;
StartOptions g_start_options;

std::atomic<bool> g_tried{false};
std::atomic<bool> g_up{false};
std::atomic<ID3D12Device*> g_device{nullptr};
ID3D12Device* g_device_set = nullptr;  // the one Streamline was told about
SRWLOCK g_create_lock = SRWLOCK_INIT;
IDXGIFactory2* g_sl_factory = nullptr;

// Two places to catch swap chain creation, when a wrapper such as ReShade's sits over DXGI. The order the game ends up with is ReShade, then Streamline, then DXGI. So ReShade presents once per
// real frame and its effects run before Streamline takes the frame, which is what lets this frame's optical flow reach DLSS-G; with Streamline on top, ReShade would see generated frames as
// well and its effects would run after the frame had gone. Under a wrapper: game -> g_table (remembers the game's queue) -> wrapper -> g_native_table (makes the swap chain through Streamline's
// factory) -> Streamline -> g_table, whose wrapper is skipped this time -> DXGI. With no wrapper only g_table is written and it does the second job.
void** g_table = nullptr;
CreateSwapChainFn g_real_create = nullptr;
CreateForHwndFn g_real_for_hwnd = nullptr;
void** g_native_table = nullptr;
CreateSwapChainFn g_native_create = nullptr;
CreateForHwndFn g_native_for_hwnd = nullptr;
thread_local bool t_inside = false;  // inside Streamline's factory: its call down must reach DXGI
thread_local IUnknown* t_game_queue = nullptr;  // what the game passed, before the wrapper unwrapped it: Streamline needs the game's own queue and device
thread_local IUnknown* t_native_factory = nullptr;

void** g_present_table = nullptr;
PresentFn g_real_present = nullptr;
Present1Fn g_real_present1 = nullptr;
std::atomic<void*> g_game_swap_chain{nullptr};
UINT g_back_buffers = 3;

std::atomic<uint32_t> g_swap_chains{0}, g_failed{0};
std::atomic<int> g_plugin_loaded{-1}, g_supported{-1}, g_status{-1};
std::atomic<uint32_t> g_max_frames{0};
uint64_t g_next_report = 0;
std::atomic<uint32_t> g_flip_queue_waits{0};
std::atomic<uint32_t> g_presented{0}, g_fed{0};
std::atomic<bool> g_generating{false};
std::atomic<uint32_t> g_shown_per_real{0};
std::atomic<uint32_t> g_cap_fps{0};
std::atomic<bool> g_boost{false}, g_sleep_on{true};

void Line(const char* format, ...) {
  char text[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  AcquireSRWLockExclusive(&g_log_lock);
  if (g_log == nullptr && !g_log_path.empty()) g_log = _wfsopen(g_log_path.c_str(), L"a", _SH_DENYNO);  // readable while the game runs, or is hung
  if (g_log != nullptr) {
    fprintf(g_log, "%10.2f [%5lu] %s\n", static_cast<double>(GetTickCount64()) / 1000.0, GetCurrentThreadId(), text);
    fflush(g_log);
  }
  ReleaseSRWLockExclusive(&g_log_lock);
}

void SlLogLine(sl::LogType type, const char* message) {
  // Streamline's errors and warnings say why a feature will not run, and they do not come back in its results. The rest goes to its own log in the folder.
  if (type == sl::LogType::eInfo) return;
  std::string text = message != nullptr ? message : "";
  // This one can come with every frame. The first few show it happens, then one in a thousand shows it goes on.
  if (text.find("no flip-queue slot freed") != std::string::npos) {
    const uint32_t count = g_flip_queue_waits.fetch_add(1) + 1;
    if (count > 5 && count % 1000 != 0) return;
    text = "#" + std::to_string(count) + " " + text;
  }
  while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
  Line("Streamline %s: %s", type == sl::LogType::eError ? "error" : "warning", text.c_str());
}

template <typename T>
bool Lookup(T*& out, const char* name) {
  out = reinterpret_cast<T*>(GetProcAddress(g_interposer, name));
  if (out == nullptr) Line("sl.interposer.dll has no %s", name);
  return out != nullptr;
}

bool StartStreamline() {
  const std::wstring dll = g_folder + L"\\sl.interposer.dll";
  g_interposer = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
  if (g_interposer == nullptr) {
    Line("Streamline not found: %ls (error %lu)", dll.c_str(), GetLastError());
    return false;
  }
  PFun_slInit* init = nullptr;
  g_sl_create_factory = reinterpret_cast<CreateFactory2Fn>(GetProcAddress(g_interposer, "CreateDXGIFactory2"));
  if (!Lookup(init, "slInit") || !Lookup(g_sl_set_device, "slSetD3DDevice") || !Lookup(g_sl_is_loaded, "slIsFeatureLoaded") || !Lookup(g_sl_is_supported, "slIsFeatureSupported") ||
      !Lookup(g_sl_get_function, "slGetFeatureFunction") || !Lookup(g_sl_new_token, "slGetNewFrameToken") || !Lookup(g_sl_tag, "slSetTagForFrame") || !Lookup(g_sl_constants, "slSetConstants") ||
      !Lookup(g_sl_upgrade, "slUpgradeInterface") || g_sl_create_factory == nullptr) {
    return false;
  }
  const sl::Feature features[] = {sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL};
  const wchar_t* paths[] = {g_folder.c_str()};
  sl::Preferences pref{};
  pref.showConsole = false;
  pref.logLevel = sl::LogLevel::eDefault;
  pref.pathsToPlugins = paths;
  pref.numPathsToPlugins = 1;
  pref.pathToLogsAndData = g_folder.c_str();
  pref.logMessageCallback = SlLogLine;
  if (!g_start_options.log) {
    // Jusant's crash dumps end in a thread of sl.interposer.dll holding a log line's time stamp, on an uncaught C++ exception.
    pref.logLevel = sl::LogLevel::eOff;
    pref.pathToLogsAndData = nullptr;
    pref.logMessageCallback = nullptr;
  }
  pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseFrameBasedResourceTagging;
  if (g_start_options.factory_proxy) pref.flags |= sl::PreferenceFlags::eUseDXGIFactoryProxy;
  if (g_start_options.manual_hooking) pref.flags |= sl::PreferenceFlags::eUseManualHooking;
  Line("Streamline options: log %d, factory proxy %d, manual hooking %d", g_start_options.log ? 1 : 0, g_start_options.factory_proxy ? 1 : 0, g_start_options.manual_hooking ? 1 : 0);
  pref.featuresToLoad = features;
  pref.numFeaturesToLoad = static_cast<uint32_t>(std::size(features));
  pref.engine = sl::EngineType::eCustom;
  pref.engineVersion = "1.0";
  pref.projectId = "4f1c2a7e-9b3d-4e8a-a1c6-5d2e7f9b0c13";  // made-up, as for the helper
  pref.renderAPI = sl::RenderAPI::eD3D12;
  // ReShade may unload and load this add-on again during startup, and Streamline stays in the process through that, so its slInit is remembered outside the add-on.
  wchar_t done[2] = {};
  if (GetEnvironmentVariableW(L"HSF_SL_BRING_UP", done, 2) != 0) {
    Line("Streamline was started earlier in this process");
    return true;
  }
  const sl::Result result = init(pref, sl::kSDKVersion);
  Line("slInit: %d, from %ls", static_cast<int>(result), dll.c_str());
  if (result != sl::Result::eOk) return false;
  SetEnvironmentVariableW(L"HSF_SL_BRING_UP", L"1");
  return true;
}

bool WriteSlot(void** table, int slot, void* value) {
  DWORD old = 0;
  if (!VirtualProtect(&table[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
  table[slot] = value;
  VirtualProtect(&table[slot], sizeof(void*), old, &old);
  return true;
}

// Tells Streamline about the device, then gives its factory. nullptr means the game gets a plain swap chain.
// The device comes from the game's queue, so under ReShade it is ReShade's proxy and not the native one. DLSS-G makes its present queue on this device, and RTSS calls D3D11On12CreateDevice
// on what it finds there, which makes dxgi.dll abort under ReShade for any device that is not a proxy (the crash of 0.3.0).
IDXGIFactory2* FactoryForDevice(ID3D12CommandQueue* queue) {
  ID3D12Device* device = nullptr;
  if (FAILED(queue->GetDevice(IID_PPV_ARGS(&device))) || device == nullptr) {
    Line("the game's queue gave no device: left to the game");
    return nullptr;
  }
  if (device == g_device_set) {
    device->Release();
  } else {
    // That reference stays: Streamline keeps using the device.
    const sl::Result result = g_sl_set_device(device);
    Line("slSetD3DDevice %p, from the game's queue (native one %p): %d", device, g_device.load(), static_cast<int>(result));
    if (result != sl::Result::eOk) {
      device->Release();
      return nullptr;
    }
    g_device_set = device;
    const LUID luid = device->GetAdapterLuid();
    sl::AdapterInfo info{};
    info.deviceLUID = reinterpret_cast<uint8_t*>(const_cast<LUID*>(&luid));
    info.deviceLUIDSizeInBytes = sizeof(LUID);
    const sl::Result supported = g_sl_is_supported(sl::kFeatureDLSS_G, info);
    g_supported = static_cast<int>(supported);
    Line("DLSS-G on this adapter: %d (0 means supported)", static_cast<int>(supported));
  }
  if (g_sl_factory == nullptr) {
    t_inside = true;
    const HRESULT hr = g_sl_create_factory(0, IID_PPV_ARGS(&g_sl_factory));
    t_inside = false;
    Line("Streamline's factory: 0x%08lx", hr);
    if (FAILED(hr)) g_sl_factory = nullptr;
    if (g_sl_factory != nullptr && g_start_options.manual_hooking) {
      // With manual hooking the factory may come back plain. Streamline's own objects answer this interface; anything else is upgraded.
      static const GUID sl_object = {0xADEC44E2, 0x61F0, 0x45C3, {0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF}};
      IUnknown* base = nullptr;
      if (SUCCEEDED(g_sl_factory->QueryInterface(sl_object, reinterpret_cast<void**>(&base))) && base != nullptr) {
        base->Release();
        Line("Streamline's factory is already its own");
      } else {
        t_inside = true;
        const sl::Result upgraded = g_sl_upgrade(reinterpret_cast<void**>(&g_sl_factory));
        t_inside = false;
        Line("slUpgradeInterface on the factory: %d", static_cast<int>(upgraded));
      }
    }
  }
  return g_sl_factory;
}

// Who handles Present on the swap chain Streamline made: Streamline sends out generated frames from its own hook there. Logged when it changes (Jusant with RenoDX DLSS
// showed no generated frames at all).
void** g_made_table = nullptr;
std::string g_present_owner;

std::string ModuleAt(const void* address) {
  HMODULE module = nullptr;
  char path[MAX_PATH] = "not a module";
  if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<const char*>(address), &module)) return path;
  GetModuleFileNameA(module, path, MAX_PATH);
  const char* const name = std::strrchr(path, '\\') != nullptr ? std::strrchr(path, '\\') + 1 : path;
  char text[MAX_PATH + 32];
  std::snprintf(text, sizeof(text), "%s+0x%llx", name, static_cast<unsigned long long>(static_cast<const char*>(address) - reinterpret_cast<const char*>(module)));
  return text;
}

// Where the code at address goes, following jumps (jmp rel32, jmp [rip+0], and mov rax, imm64; jmp rax) a few steps, as hooks are chained.
std::string Owner(const void* address) {
  std::string out = ModuleAt(address);
  const uint8_t* at = static_cast<const uint8_t*>(address);
  for (int step = 0; step < 6 && at != nullptr; ++step) {
    uint8_t code[16] = {};
    if (!ReadProcessMemory(GetCurrentProcess(), at, code, sizeof(code), nullptr)) break;
    const uint8_t* next = nullptr;
    if (code[0] == 0xE9) {
      int32_t relative;
      std::memcpy(&relative, code + 1, 4);
      next = at + 5 + relative;
    } else if (code[0] == 0xFF && code[1] == 0x25) {
      int32_t relative;
      std::memcpy(&relative, code + 2, 4);
      const uint8_t* const slot = at + 6 + relative;
      if (!ReadProcessMemory(GetCurrentProcess(), slot, &next, sizeof(next), nullptr)) break;
    } else if (code[0] == 0x48 && code[1] == 0xB8 && code[10] == 0xFF && code[11] == 0xE0) {
      std::memcpy(&next, code + 2, sizeof(next));
    } else {
      break;
    }
    out += " -> " + ModuleAt(next);
    at = next;
  }
  return out;
}

void CheckPresentOwner() {
  if (g_made_table == nullptr) return;
  void* slots[2] = {};
  if (!ReadProcessMemory(GetCurrentProcess(), g_made_table + 8, &slots[0], sizeof(void*), nullptr) || !ReadProcessMemory(GetCurrentProcess(), g_made_table + 22, &slots[1], sizeof(void*), nullptr)) return;
  const std::string now = "Present " + Owner(slots[0]) + ", Present1 " + Owner(slots[1]);
  if (now == g_present_owner) return;
  g_present_owner = now;
  Line("the swap chain's table %p (%s): %s", g_made_table, Owner(g_made_table).c_str(), now.c_str());
}

void AfterSwapChain(HRESULT hr, IUnknown* swap_chain, UINT game_flags) {
  Line("swap chain through Streamline: 0x%08lx, %p, game asked for flags 0x%x", hr, swap_chain, game_flags);
  if (SUCCEEDED(hr) && swap_chain != nullptr) {
    g_made_table = *reinterpret_cast<void***>(swap_chain);
    g_present_owner.clear();
    CheckPresentOwner();
  }
  if (FAILED(hr)) {
    g_failed.fetch_add(1);
    return;
  }
  g_swap_chains.fetch_add(1);
  bool loaded = false;
  const sl::Result result = g_sl_is_loaded(sl::kFeatureDLSS_G, loaded);
  g_plugin_loaded = result == sl::Result::eOk && loaded ? 1 : 0;
  Line("DLSS-G's plugin loaded: %d, result %d", loaded ? 1 : 0, static_cast<int>(result));
  void* function = nullptr;
  const sl::Result found = g_sl_get_function(sl::kFeatureDLSS_G, "slDLSSGGetState", function);
  g_sl_dlssg_state = found == sl::Result::eOk ? reinterpret_cast<PFun_slDLSSGGetState*>(function) : nullptr;
  if (found != sl::Result::eOk) Line("slDLSSGGetState: %d", static_cast<int>(found));
  const auto feature_call = [](sl::Feature feature, const char* name, auto*& out) {
    void* call = nullptr;
    const sl::Result got = g_sl_get_function(feature, name, call);
    out = got == sl::Result::eOk ? reinterpret_cast<std::remove_reference_t<decltype(out)>>(call) : nullptr;
    if (out == nullptr) Line("%s: %d", name, static_cast<int>(got));
  };
  feature_call(sl::kFeatureDLSS_G, "slDLSSGSetOptions", g_sl_dlssg_options);
  feature_call(sl::kFeatureReflex, "slReflexSetOptions", g_sl_reflex_options);
  feature_call(sl::kFeatureReflex, "slReflexSleep", g_sl_reflex_sleep);
  feature_call(sl::kFeaturePCL, "slPCLSetMarker", g_sl_marker);
  if (g_sl_dlssg_state != nullptr) {
    sl::DLSSGState state{};
    g_sl_dlssg_state(VIEWPORT, state, nullptr);
    g_max_frames = state.numFramesToGenerateMax;
  }
  IDXGISwapChain1* with_desc = nullptr;
  if (swap_chain != nullptr && SUCCEEDED(swap_chain->QueryInterface(IID_PPV_ARGS(&with_desc)))) {
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (SUCCEEDED(with_desc->GetDesc1(&desc))) g_back_buffers = desc.BufferCount;
    with_desc->Release();
  }
}

// ---- Each frame: tokens and markers, on the thread that presents. ----

sl::FrameToken* g_token = nullptr;
uint32_t g_frame_index = 0;
bool g_marked_submit_end = false;  // this frame's "render submit end" and "present start" are out
bool g_fed_this_frame = false, g_fed_last_frame = false;
uint32_t g_applied_cap = UINT32_MAX;
bool g_applied_boost = false;
bool g_force_sync_logged = false;

// Where each real frame's time goes, for the log: what a five second count of frames cannot show. All on the present thread.
struct FrameTimes {
  double last = 0, average = 0, worst = 0;
  double present_sum = 0, present_worst = 0, sleep_sum = 0, sleep_worst = 0, feed_worst = 0;
  uint32_t frames = 0, spikes = 0;
} g_times;

double NowMs() {
  static const double to_ms = [] {
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    return 1000.0 / static_cast<double>(frequency.QuadPart);
  }();
  LARGE_INTEGER counter;
  QueryPerformanceCounter(&counter);
  return static_cast<double>(counter.QuadPart) * to_ms;
}

void CountFrameTime() {
  const double now = NowMs();
  const double gap = g_times.last != 0 ? now - g_times.last : 0;
  g_times.last = now;
  if (gap <= 0 || gap > 1000) return;
  if (g_times.average != 0 && gap > g_times.average * 1.5) ++g_times.spikes;
  g_times.average = g_times.average == 0 ? gap : g_times.average * 0.95 + gap * 0.05;
  g_times.worst = std::max(g_times.worst, gap);
  ++g_times.frames;
}

void Mark(sl::PCLMarker marker) {
  if (g_sl_marker != nullptr && g_token != nullptr) g_sl_marker(marker, *g_token);
}

// Low latency is always on: DLSS-G refuses to run without it.
void ApplyReflex() {
  const uint32_t cap = g_cap_fps.load();
  const bool boost = g_boost.load();
  if (g_sl_reflex_options == nullptr || (cap == g_applied_cap && boost == g_applied_boost)) return;
  g_applied_cap = cap;
  g_applied_boost = boost;
  sl::ReflexOptions reflex{};
  reflex.mode = boost ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eLowLatency;
  reflex.frameLimitUs = cap != 0 ? 1000000u / cap : 0;
  const sl::Result result = g_sl_reflex_options(reflex);
  Line("Reflex: low latency%s, FPS cap %u: %d", boost ? " + boost" : "", cap, static_cast<int>(result));
}

void BeginFrame(bool sleep) {
  ApplyReflex();
  g_token = nullptr;
  g_marked_submit_end = false;
  if (g_sl_new_token == nullptr || g_sl_marker == nullptr) return;
  const uint32_t index = ++g_frame_index;
  if (g_sl_new_token(g_token, &index) != sl::Result::eOk) {
    g_token = nullptr;
    return;
  }
  if (sleep && g_sleep_on.load() && g_sl_reflex_sleep != nullptr) {
    const double before = NowMs();
    g_sl_reflex_sleep(*g_token);
    const double slept = NowMs() - before;
    g_times.sleep_sum += slept;
    g_times.sleep_worst = std::max(g_times.sleep_worst, slept);
  }
  Mark(sl::PCLMarker::eSimulationStart);
}

// ---- DLSS-G's options ----

struct AppliedOptions {
  bool set = false, on = false;
  uint32_t frames = 0, width = 0, height = 0, input_width = 0, input_height = 0;
  DXGI_FORMAT colour = DXGI_FORMAT_UNKNOWN, motion = DXGI_FORMAT_UNKNOWN;
  bool dynamic = false;
} g_options;

void SetOptions(bool on, uint32_t frames, uint32_t width, uint32_t height, uint32_t input_width, uint32_t input_height, DXGI_FORMAT colour, DXGI_FORMAT motion, bool dynamic, uint32_t in_use_width, uint32_t in_use_height) {
  if (g_sl_dlssg_options == nullptr) return;
  const uint32_t most = std::max(g_max_frames.load(), 1u);
  frames = std::clamp(frames, 1u, most);
  AppliedOptions& a = g_options;
  if (a.set && a.on == on && a.frames == frames && a.width == width && a.height == height && a.input_width == input_width && a.input_height == input_height && a.colour == colour && a.motion == motion && a.dynamic == dynamic) return;
  // A new frame count while generating leaves DLSS-G on but generating nothing, as the helper found: off for one frame first, and the next frame's call turns it on again.
  if (a.set && a.on && on && a.frames != frames) on = false;
  a = {true, on, frames, width, height, input_width, input_height, colour, motion, dynamic};
  sl::DLSSGOptions options{};
  options.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
  options.numFramesToGenerate = frames;
  options.numBackBuffers = g_back_buffers;
  options.mvecDepthWidth = input_width;
  options.mvecDepthHeight = input_height;
  options.colorWidth = width;
  options.colorHeight = height;
  options.colorBufferFormat = colour;
  options.mvecBufferFormat = motion;
  options.depthBufferFormat = DXGI_FORMAT_R32_FLOAT;
  options.hudLessBufferFormat = colour;
  // A game's upscaler can change its render size from frame to frame, inside textures that stay one size.
  if (dynamic) {
    options.flags = options.flags | sl::DLSSGFlags::eDynamicResolutionEnabled;
    options.dynamicResWidth = in_use_width;
    options.dynamicResHeight = in_use_height;
  }
  const sl::Result result = g_sl_dlssg_options(VIEWPORT, options);
  g_generating = on;
  Line("DLSS-G %s, %u generated per real frame, picture %ux%u format %u, inputs %ux%u: %d", on ? "ON" : "off", frames, width, height, static_cast<unsigned>(colour), input_width, input_height, static_cast<int>(result));
}

// ---- Present: everything that must be just before or after the real one ----

void BeforePresent() {
  if (g_sl_new_token == nullptr) return;
  if (g_token == nullptr) BeginFrame(false);
  Mark(sl::PCLMarker::eSimulationEnd);
  Mark(sl::PCLMarker::eRenderSubmitStart);
  // The inputs come while the wrapper presents, under this call. After a frame with none, generation stops and this frame's markers go out now.
  g_fed_last_frame = g_fed_this_frame;
  g_fed_this_frame = false;
  if (!g_fed_last_frame) {
    if (g_options.set && g_options.on) {
      Line("no inputs last frame: generation stops");
      const AppliedOptions a = g_options;
      SetOptions(false, a.frames, a.width, a.height, a.input_width, a.input_height, a.colour, a.motion, a.dynamic, a.input_width, a.input_height);
    }
    Mark(sl::PCLMarker::eRenderSubmitEnd);
    Mark(sl::PCLMarker::ePresentStart);
    g_marked_submit_end = true;
  }
}

void AfterPresent() {
  if (g_sl_new_token == nullptr) return;
  Mark(sl::PCLMarker::ePresentEnd);
  g_presented.fetch_add(1);
  BeginFrame(true);
}

// Waiting for vertical sync would hold back DLSS-G's paced frames; while it generates it paces them itself.
UINT SyncFor(UINT asked) {
  if (asked == 0 || !g_generating.load()) return asked;
  if (!g_force_sync_logged) {
    g_force_sync_logged = true;
    Line("the game asks for vsync (%u): presenting without while DLSS-G generates", asked);
  }
  return 0;
}

HRESULT STDMETHODCALLTYPE HookPresent(IDXGISwapChain* swap_chain, UINT sync, UINT flags) {
  if (swap_chain != g_game_swap_chain.load() || (flags & DXGI_PRESENT_TEST) != 0) return g_real_present(swap_chain, sync, flags);
  BeforePresent();
  const double before = NowMs();
  const HRESULT hr = g_real_present(swap_chain, SyncFor(sync), flags);
  const double took = NowMs() - before;
  g_times.present_sum += took;
  g_times.present_worst = std::max(g_times.present_worst, took);
  CountFrameTime();
  AfterPresent();
  return hr;
}

HRESULT STDMETHODCALLTYPE HookPresent1(IDXGISwapChain1* swap_chain, UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* parameters) {
  if (swap_chain != g_game_swap_chain.load() || (flags & DXGI_PRESENT_TEST) != 0) return g_real_present1(swap_chain, sync, flags, parameters);
  BeforePresent();
  const double before = NowMs();
  const HRESULT hr = g_real_present1(swap_chain, SyncFor(sync), flags, parameters);
  const double took = NowMs() - before;
  g_times.present_sum += took;
  g_times.present_worst = std::max(g_times.present_worst, took);
  CountFrameTime();
  AfterPresent();
  return hr;
}

// `swap_chain` is the one the game holds.
void CatchPresent(IUnknown* swap_chain) {
  if (swap_chain == nullptr) return;
  void** const table = *reinterpret_cast<void***>(swap_chain);
  g_game_swap_chain = swap_chain;
  if (g_present_table == table) return;
  if (g_present_table != nullptr) {
    Line("a second kind of swap chain: its frames are not followed");
    return;
  }
  g_real_present = reinterpret_cast<PresentFn>(table[SLOT_PRESENT]);
  g_real_present1 = reinterpret_cast<Present1Fn>(table[SLOT_PRESENT1]);
  const bool written = WriteSlot(table, SLOT_PRESENT, reinterpret_cast<void*>(&HookPresent)) && WriteSlot(table, SLOT_PRESENT1, reinterpret_cast<void*>(&HookPresent1));
  Line("following Present on the table %p: %d", table, written ? 1 : 0);
  if (written) g_present_table = table;
}

// The queue under a wrapper's. DLSS-G makes the swap chain on a queue of its own, its pacer, on the device it was given, so under ReShade that queue is a proxy, and DXGI must get the real one:
// DXGI ties the back buffers to that queue and the device is lost if it is not the one DLSS-G presents from. ReShade's object keeps the real queue in a field after its method table.
IUnknown* RealQueue(IUnknown* queue) {
  const auto owner_of = [](const void* address) {
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &module);
    return module;
  };
  HMODULE wrapper = nullptr;
  if (g_table != nullptr) wrapper = owner_of(g_table);
  if (wrapper == nullptr || owner_of(*reinterpret_cast<void**>(queue)) != wrapper) return queue;
  void** const words = reinterpret_cast<void**>(queue);
  for (int i = 1; i <= 8; ++i) {
    MEMORY_BASIC_INFORMATION info{};
    void* const word = words[i];
    if (VirtualQuery(word, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT || (info.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY)) == 0) continue;
    void* const table = *static_cast<void**>(word);
    const HMODULE owner = owner_of(table);
    if (owner == nullptr || owner == wrapper) continue;
    ID3D12CommandQueue* real = nullptr;
    if (FAILED(static_cast<IUnknown*>(word)->QueryInterface(IID_PPV_ARGS(&real)))) continue;
    real->Release();
    return static_cast<IUnknown*>(word);
  }
  Line("the real queue under %p was not found", queue);
  return queue;
}

// Makes the swap chain through Streamline's factory. `factory` is what reached this layer, which is what DXGI itself must get in the end.
template <typename MakeThrough, typename MakeDirect>
HRESULT ThroughStreamline(IUnknown* factory, IUnknown* queue, UINT game_flags, IUnknown** made, MakeThrough&& through, MakeDirect&& direct) {
  IUnknown* const game_queue = t_game_queue != nullptr ? t_game_queue : queue;
  ID3D12CommandQueue* d3d12_queue = nullptr;
  if (game_queue == nullptr || FAILED(game_queue->QueryInterface(IID_PPV_ARGS(&d3d12_queue)))) {
    Line("swap chain for a device that is not D3D12: left to the game");
    return direct();
  }
  AcquireSRWLockExclusive(&g_create_lock);
  IDXGIFactory2* const sl_factory = FactoryForDevice(d3d12_queue);
  d3d12_queue->Release();
  HRESULT hr = E_FAIL;
  if (sl_factory != nullptr) {
    t_inside = true;
    t_native_factory = factory;
    hr = through(sl_factory, game_queue);
    t_inside = false;
    t_native_factory = nullptr;
    AfterSwapChain(hr, made != nullptr ? *made : nullptr, game_flags);
  }
  ReleaseSRWLockExclusive(&g_create_lock);
  return sl_factory != nullptr ? hr : direct();
}

HRESULT NativeForHwnd(IDXGIFactory2* factory, IUnknown* queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output, IDXGISwapChain1** swap_chain, CreateForHwndFn real) {
  return ThroughStreamline(factory, queue, desc->Flags, reinterpret_cast<IUnknown**>(swap_chain),
                           [&](IDXGIFactory2* sl_factory, IUnknown* game_queue) {
                             // The game's own flags. Adding the frame latency waitable for a game that never waits on it turns off the queue limit that holds it back (Red Dead Redemption 2).
                             return sl_factory->CreateSwapChainForHwnd(game_queue, hwnd, desc, fullscreen, output, swap_chain);
                           },
                           [&] { return real(factory, queue, hwnd, desc, fullscreen, output, swap_chain); });
}

HRESULT NativeCreate(IDXGIFactory* factory, IUnknown* queue, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swap_chain, CreateSwapChainFn real) {
  return ThroughStreamline(factory, queue, desc->Flags, reinterpret_cast<IUnknown**>(swap_chain),
                           [&](IDXGIFactory2* sl_factory, IUnknown* game_queue) {
                             return sl_factory->CreateSwapChain(game_queue, desc, swap_chain);
                           },
                           [&] { return real(factory, queue, desc, swap_chain); });
}

// Under the wrapper, on DXGI's own table.
HRESULT STDMETHODCALLTYPE HookNativeForHwnd(IDXGIFactory2* factory, IUnknown* queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output, IDXGISwapChain1** swap_chain) {
  if (t_inside) return g_native_for_hwnd(factory, RealQueue(queue), hwnd, desc, fullscreen, output, swap_chain);
  return NativeForHwnd(factory, queue, hwnd, desc, fullscreen, output, swap_chain, g_native_for_hwnd);
}

HRESULT STDMETHODCALLTYPE HookNativeCreate(IDXGIFactory* factory, IUnknown* queue, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swap_chain) {
  if (t_inside) return g_native_create(factory, RealQueue(queue), desc, swap_chain);
  return NativeCreate(factory, queue, desc, swap_chain, g_native_create);
}

// On the table of the factory the game holds.
HRESULT STDMETHODCALLTYPE HookCreateForHwnd(IDXGIFactory2* factory, IUnknown* queue, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreen, IDXGIOutput* output, IDXGISwapChain1** swap_chain) {
  if (t_inside) {
    // Streamline's call down. Its base factory may be the wrapper's: going through that would wrap this swap chain a second time.
    if (g_native_for_hwnd != nullptr && t_native_factory != nullptr) return g_native_for_hwnd(static_cast<IDXGIFactory2*>(t_native_factory), RealQueue(queue), hwnd, desc, fullscreen, output, swap_chain);
    return g_real_for_hwnd(factory, queue, hwnd, desc, fullscreen, output, swap_chain);
  }
  HRESULT hr;
  if (g_native_for_hwnd != nullptr) {
    t_game_queue = queue;
    hr = g_real_for_hwnd(factory, queue, hwnd, desc, fullscreen, output, swap_chain);
    t_game_queue = nullptr;
  } else {
    hr = NativeForHwnd(factory, queue, hwnd, desc, fullscreen, output, swap_chain, g_real_for_hwnd);
  }
  if (SUCCEEDED(hr) && swap_chain != nullptr) CatchPresent(*swap_chain);
  return hr;
}

HRESULT STDMETHODCALLTYPE HookCreateSwapChain(IDXGIFactory* factory, IUnknown* queue, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** swap_chain) {
  if (t_inside) {
    if (g_native_create != nullptr && t_native_factory != nullptr) return g_native_create(static_cast<IDXGIFactory*>(t_native_factory), RealQueue(queue), desc, swap_chain);
    return g_real_create(factory, queue, desc, swap_chain);
  }
  HRESULT hr;
  if (g_native_create != nullptr) {
    t_game_queue = queue;
    hr = g_real_create(factory, queue, desc, swap_chain);
    t_game_queue = nullptr;
  } else {
    hr = NativeCreate(factory, queue, desc, swap_chain, g_real_create);
  }
  if (SUCCEEDED(hr) && swap_chain != nullptr) CatchPresent(*swap_chain);
  return hr;
}

bool IsSystemDxgi(const void* address) {
  HMODULE module = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(address), &module)) return false;
  wchar_t path[MAX_PATH], system[MAX_PATH];
  if (GetModuleFileNameW(module, path, MAX_PATH) == 0 || GetSystemDirectoryW(system, MAX_PATH) == 0) return false;
  wcscat_s(system, L"\\dxgi.dll");
  return _wcsicmp(path, system) == 0;
}

// DXGI's own factory inside a wrapper's: ReShade's object keeps it in a field just after its method table. Checked by where its methods live, so another wrapper just gets the old order.
void** FindNativeTable(IUnknown* wrapper) {
  void** const words = reinterpret_cast<void**>(wrapper);
  for (int i = 1; i <= 4; ++i) {
    MEMORY_BASIC_INFORMATION info{};
    void* const word = words[i];
    if (VirtualQuery(word, &info, sizeof(info)) == 0 || info.State != MEM_COMMIT || (info.Protect & (PAGE_READWRITE | PAGE_READONLY | PAGE_WRITECOPY)) == 0) continue;
    void** const table = *static_cast<void***>(word);
    if (!IsSystemDxgi(table)) continue;
    if (IsSystemDxgi(table[SLOT_CREATE_SWAP_CHAIN]) && IsSystemDxgi(table[SLOT_CREATE_FOR_HWND])) return table;
  }
  return nullptr;
}

// The table of the factory the game will get: dxgi.dll by that name is the one the game imports, which may be a wrapper such as ReShade's and not the system's.
bool CatchSwapChains() {
  HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
  const auto create = dxgi != nullptr ? reinterpret_cast<CreateFactory2Fn>(GetProcAddress(dxgi, "CreateDXGIFactory2")) : nullptr;
  if (create == nullptr) {
    Line("no CreateDXGIFactory2 in dxgi.dll");
    return false;
  }
  IDXGIFactory2* factory = nullptr;
  const HRESULT hr = create(0, IID_PPV_ARGS(&factory));
  if (FAILED(hr)) {
    Line("a factory to find the method table: 0x%08lx", hr);
    return false;
  }
  void** const table = *reinterpret_cast<void***>(factory);
  g_real_create = reinterpret_cast<CreateSwapChainFn>(table[SLOT_CREATE_SWAP_CHAIN]);
  g_real_for_hwnd = reinterpret_cast<CreateForHwndFn>(table[SLOT_CREATE_FOR_HWND]);
  const bool written = WriteSlot(table, SLOT_CREATE_SWAP_CHAIN, reinterpret_cast<void*>(&HookCreateSwapChain)) && WriteSlot(table, SLOT_CREATE_FOR_HWND, reinterpret_cast<void*>(&HookCreateForHwnd));
  HMODULE owner = nullptr;
  wchar_t owner_path[MAX_PATH] = L"?";
  if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(table), &owner)) GetModuleFileNameW(owner, owner_path, MAX_PATH);
  Line("catching swap chains on the factory table %p in %ls: %d", table, owner_path, written ? 1 : 0);
  const bool wrapped = !IsSystemDxgi(table);
  void** const native = written && wrapped ? FindNativeTable(factory) : nullptr;
  factory->Release();
  if (!written) return false;
  g_table = table;
  if (native != nullptr) {
    g_native_create = reinterpret_cast<CreateSwapChainFn>(native[SLOT_CREATE_SWAP_CHAIN]);
    g_native_for_hwnd = reinterpret_cast<CreateForHwndFn>(native[SLOT_CREATE_FOR_HWND]);
    const bool under = WriteSlot(native, SLOT_CREATE_SWAP_CHAIN, reinterpret_cast<void*>(&HookNativeCreate)) && WriteSlot(native, SLOT_CREATE_FOR_HWND, reinterpret_cast<void*>(&HookNativeForHwnd));
    Line("under the wrapper, on DXGI's table %p: %d", native, under ? 1 : 0);
    if (under) {
      g_native_table = native;
    } else {
      g_native_create = nullptr;
      g_native_for_hwnd = nullptr;
    }
  } else if (wrapped) {
    Line("this wrapper's own factory was not found: Streamline goes on top of it, so effect textures would come a frame late");
  }
  return true;
}

// ---- This frame's inputs for DLSS-G, recorded on a list of HSF's own ----

constexpr D3D12_RESOURCE_STATES READ_ANY = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  D3D12_RESOURCE_BARRIER b{};
  b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  b.Transition.pResource = resource;
  b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  b.Transition.StateBefore = from;
  b.Transition.StateAfter = to;
  list->ResourceBarrier(1, &b);
}

DXGI_FORMAT ReadableFormat(DXGI_FORMAT format) {
  switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default: return format;
  }
}

struct Feeder {
  ID3D12Device* device = nullptr;
  UINT width = 0, height = 0, input_width = 0, input_height = 0;
  DXGI_FORMAT colour_format = DXGI_FORMAT_UNKNOWN, motion_format = DXGI_FORMAT_UNKNOWN;
  ComPtr<ID3D12CommandAllocator> alloc[compose::FRAMES];
  ComPtr<ID3D12GraphicsCommandList> list;
  ComPtr<ID3D12Fence> fence;
  HANDLE event = nullptr;
  UINT64 value = 0, alloc_value[compose::FRAMES] = {};
  UINT64 count = 0;
  compose::Pass compose;
  frame_checks::Mask mask;
  bool compose_ready = false, mask_ready = false, had_compose = false;
  debugview::Pass view;
  bool view_tried = false, view_ready = false;
  nvof::Flow nvof;
  ComPtr<ID3D12Resource> mvec, composed, colour;
  // Zero motion in the motion format, for a frame NVIDIA Optical Flow has nothing for while no effect gives a flow (its first, or after a failure).
  // Committed resources start as zeros and nothing writes it; it stays in NON_PIXEL_SHADER_RESOURCE, as NVOF's own result does.
  ComPtr<ID3D12Resource> no_motion;
  D3D12_RESOURCE_STATES mvec_state = D3D12_RESOURCE_STATE_UNORDERED_ACCESS, colour_state = D3D12_RESOURCE_STATE_COPY_DEST;
  ~Feeder() {
    if (fence && event != nullptr && fence->GetCompletedValue() < value) {
      fence->SetEventOnCompletion(value, event);
      WaitForSingleObject(event, 2000);
    }
    if (event != nullptr) CloseHandle(event);
  }
};
std::unique_ptr<Feeder> g_feeder;
bool g_feeder_failed = false;
std::atomic<int> g_nvof_state{0};
void NvofLine(const char* text) { Line("%s", text); }

bool MakeFeeder(const FrameFeed& in, const D3D12_RESOURCE_DESC& back, const D3D12_RESOURCE_DESC& motion) {
  auto f = std::make_unique<Feeder>();
  f->device = in.device;
  f->width = static_cast<UINT>(back.Width);
  f->height = back.Height;
  f->input_width = static_cast<UINT>(motion.Width);
  f->input_height = motion.Height;
  f->colour_format = back.Format;
  f->motion_format = motion.Format;
  HRESULT hr = S_OK;
  for (auto& alloc : f->alloc)
    if (SUCCEEDED(hr)) hr = in.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
  if (SUCCEEDED(hr)) hr = in.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, f->alloc[0].Get(), nullptr, IID_PPV_ARGS(&f->list));
  if (SUCCEEDED(hr)) hr = f->list->Close();
  if (SUCCEEDED(hr)) hr = in.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&f->fence));
  f->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if (FAILED(hr) || f->event == nullptr) {
    Line("HSF's command list for the inputs: 0x%08lx", hr);
    return false;
  }
  const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  const char* what = nullptr;
  const DXGI_FORMAT colour_view = ReadableFormat(back.Format);
  f->compose_ready = in.compose && f->compose.Init(in.device, in.queue, motion.Format, f->input_width, f->input_height, what, hr);
  if (in.compose && !f->compose_ready) Line("compose pass not available: %s (0x%08lx). DLSS-G gets the flow as it is.", what, hr);
  f->mask_ready = in.mask && f->mask.Init(in.device, in.queue, motion.Format, back.Format, colour_view, f->width, f->height, f->input_width, f->input_height, what, hr);
  if (in.mask && !f->mask_ready) Line("HUD mask not available: %s (0x%08lx)", what, hr);
  D3D12_RESOURCE_DESC desc{};
  desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  desc.Width = motion.Width;
  desc.Height = motion.Height;
  desc.DepthOrArraySize = 1;
  desc.MipLevels = 1;
  desc.SampleDesc.Count = 1;
  desc.Format = motion.Format;
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  hr = S_OK;
  if (f->compose_ready || f->mask_ready) hr = in.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&f->mvec));
  if (SUCCEEDED(hr) && f->compose_ready && f->mask_ready) hr = in.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&f->composed));
  if (SUCCEEDED(hr) && in.motion == nullptr) {
    D3D12_RESOURCE_DESC zero = desc;
    zero.Flags = D3D12_RESOURCE_FLAG_NONE;
    hr = in.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &zero, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&f->no_motion));
  }
  if (SUCCEEDED(hr) && f->mask_ready) {
    D3D12_RESOURCE_DESC colour = back;
    colour.Flags = D3D12_RESOURCE_FLAG_NONE;
    colour.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    colour.Alignment = 0;
    hr = in.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &colour, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&f->colour));
  }
  if (FAILED(hr)) {
    Line("HSF's textures for the inputs: 0x%08lx", hr);
    return false;
  }
  Line("inputs: flow %ux%u format %u, compose %s, HUD mask %s", f->input_width, f->input_height, static_cast<unsigned>(motion.Format), f->compose_ready ? "on" : "off", f->mask_ready ? "on" : "off");
  g_feeder = std::move(f);
  return true;
}

void SetConstants(const FrameFeed& in, bool dilated, bool reset, UINT width, UINT height) {
  const auto identity = [] {
    sl::float4x4 m{};
    m.setRow(0, {1, 0, 0, 0});
    m.setRow(1, {0, 1, 0, 0});
    m.setRow(2, {0, 0, 1, 0});
    m.setRow(3, {0, 0, 0, 1});
    return m;
  };
  sl::Constants c{};
  c.cameraViewToClip = c.clipToCameraView = c.clipToLensClip = c.clipToPrevClip = c.prevClipToClip = identity();
  c.jitterOffset = {in.jitter[0], in.jitter[1]};
  c.mvecScale = {in.scale[0], in.scale[1]};
  c.cameraPinholeOffset = {0, 0};
  c.cameraPos = {0, 0, 0};
  c.cameraUp = {0, 1, 0};
  c.cameraRight = {1, 0, 0};
  c.cameraFwd = {0, 0, 1};
  c.cameraNear = 0.1f;
  c.cameraFar = 100.f;
  c.cameraFOV = 1.f;
  c.cameraAspectRatio = static_cast<float>(width) / static_cast<float>(height);
  c.depthInverted = in.depth_inverted ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  c.cameraMotionIncluded = sl::Boolean::eTrue;
  c.motionVectors3D = sl::Boolean::eFalse;
  c.reset = reset ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  // The compose pass has already taken each edge's motion from the near surface, so DLSS-G must not widen it again.
  c.motionVectorsDilated = dilated ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  c.motionVectorsJittered = in.jittered ? sl::Boolean::eTrue : sl::Boolean::eFalse;
  const sl::Result result = g_sl_constants(c, *g_token, VIEWPORT);
  if (result != sl::Result::eOk) {
    static int logged = 0;
    if (logged++ < 5) Line("slSetConstants: %d", static_cast<int>(result));
  }
}

}  // namespace

void Start(const wchar_t* log_path, const wchar_t* folder, const StartOptions& options) {
  g_log_path = log_path;
  g_folder = folder;
  g_start_options = options;
  // RenoDX DLSS decides when it starts whether to hook Streamline's Present or ReShade's, and ReShade starts it right after HSF. With ReShade's, DLSS-G showed no frames
  // in Jusant. Loading the DLL does not start Streamline: slInit still waits for the first device. HSF's reference is kept for the life of the process.
  if (options.early_load && GetModuleHandleW(L"sl.interposer.dll") == nullptr) {
    const std::wstring dll = g_folder + L"\\sl.interposer.dll";
    const HMODULE early = LoadLibraryExW(dll.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    Line("sl.interposer.dll loaded early, as HSF loads: %s", early != nullptr ? "yes" : "no");
  }
}

void NoteDevice(void* device) {
  g_device = static_cast<ID3D12Device*>(device);
  Line("D3D12 device %p", device);
  if (g_tried.exchange(true)) return;
  Line("=== HSF bringing Streamline in, from %ls ===", g_folder.c_str());
  if (!StartStreamline()) return;
  if (!CatchSwapChains()) return;
  g_up = true;
}

void Poll() {
  if (g_sl_dlssg_state == nullptr) return;
  const uint64_t now = GetTickCount64();
  if (now < g_next_report) return;
  g_next_report = now + 5000;
  sl::DLSSGState state{};
  const sl::Result result = g_sl_dlssg_state(sl::ViewportHandle(0), state, nullptr);
  g_status = static_cast<int>(state.status);
  g_max_frames = state.numFramesToGenerateMax;
  g_shown_per_real = state.numFramesActuallyPresented;
  CheckPresentOwner();
  Line("DLSS-G: result %d status 0x%x max frames %u showing %u per real frame; real frames +%u, with inputs +%u", static_cast<int>(result), static_cast<uint32_t>(state.status), state.numFramesToGenerateMax,
       state.numFramesActuallyPresented, g_presented.exchange(0), g_fed.exchange(0));
  if (g_times.frames != 0) {
    const double frames = static_cast<double>(g_times.frames);
    Line("frame times: average %.1f ms, worst %.1f, %u over 1.5x average; Present average %.1f worst %.1f, HSF's part of it worst %.1f; Reflex sleep average %.1f worst %.1f", g_times.average, g_times.worst, g_times.spikes, g_times.present_sum / frames, g_times.present_worst, g_times.feed_worst, g_times.sleep_sum / frames, g_times.sleep_worst);
    const double last = g_times.last, average = g_times.average;
    g_times = {};
    g_times.last = last;
    g_times.average = average;
  }
}

void LogLine(const char* text) { Line("%s", text); }

void SetReflex(uint32_t fps_cap, bool boost, bool sleep) {
  g_cap_fps = fps_cap;
  g_boost = boost;
  g_sleep_on = sleep;
}

namespace {
bool FeedFrame(const FrameFeed& in);
}

bool Frame(const FrameFeed& in) {
  const double before = NowMs();
  const bool fed = FeedFrame(in);
  g_times.feed_worst = std::max(g_times.feed_worst, NowMs() - before);
  return fed;
}

namespace {
bool FeedFrame(const FrameFeed& in) {
  // No flow effect is fine with NVIDIA Optical Flow on: it makes the flow alone, at the depth's size in a format of HSF's choosing.
  const bool nvof_alone = in.motion == nullptr && in.nvof;
  if ((in.motion == nullptr && !nvof_alone) || in.depth == nullptr || in.back_buffer == nullptr) return false;
  if (in.tag == nullptr && (g_sl_tag == nullptr || g_sl_constants == nullptr || g_sl_dlssg_options == nullptr || g_token == nullptr)) return false;
  const D3D12_RESOURCE_DESC back = in.back_buffer->GetDesc();
  const D3D12_RESOURCE_DESC depth_desc = in.depth->GetDesc();
  D3D12_RESOURCE_DESC motion_desc = depth_desc;
  if (nvof_alone) motion_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
  else motion_desc = in.motion->GetDesc();
  // DLSS-G takes one size for depth and motion. A smaller flow (LumeniteFX Kernel's is 1/8 size, as in FF7R) is brought up to the depth's size by the
  // compose pass, which reads it at its own size; HSF's motion texture and everything sized to the input then follow the depth. LaunchPad's flow already matches.
  const bool resize = depth_desc.Width != motion_desc.Width || depth_desc.Height != motion_desc.Height;
  // Only a flow smaller than the depth is brought up. A depth smaller than the flow is a game's dynamic resolution (AC Shadows changes it about three
  // times a second under load): following it rebuilt everything sized to the input, compose pass included, at each change, which shook the picture and
  // crashed the game. Those frames go without HSF's flow instead, and on the in-game route DLSS-G falls back to the game's vectors until the sizes match.
  if (resize && (motion_desc.Width >= depth_desc.Width || motion_desc.Height >= depth_desc.Height)) {
    static int logged = 0;
    if (logged < 3) {
      ++logged;
      Line("depth is %llux%u, smaller than the flow (%llux%u): the game's dynamic resolution; this frame goes without HSF's flow", depth_desc.Width, depth_desc.Height,
           motion_desc.Width, motion_desc.Height);
    }
    return false;
  }
  if (resize) {
    if (!in.compose || (in.width != 0 && in.height != 0)) {
      static bool logged = false;
      if (!logged) Line("depth is %llux%u and flow %llux%u: DLSS-G has one size for both, and only the compose pass (Compose=1, whole picture) can bring the flow to the depth's size, so nothing is generated", depth_desc.Width, depth_desc.Height, motion_desc.Width, motion_desc.Height);
      logged = true;
      return false;
    }
    static bool logged = false;
    if (!logged) Line("depth is %llux%u and flow %llux%u: the compose pass brings the flow to the depth's size", depth_desc.Width, depth_desc.Height, motion_desc.Width, motion_desc.Height);
    logged = true;
    motion_desc.Width = depth_desc.Width;
    motion_desc.Height = depth_desc.Height;
  }
  // A new size or format: everything sized to it starts again.
  Feeder* f = g_feeder.get();
  if (f == nullptr || f->device != in.device || f->width != back.Width || f->height != back.Height || f->colour_format != back.Format || f->input_width != motion_desc.Width || f->input_height != motion_desc.Height || f->motion_format != motion_desc.Format ||
      (nvof_alone && !f->no_motion)) {
    g_feeder.reset();
    if (!g_feeder_failed && !MakeFeeder(in, back, motion_desc)) g_feeder_failed = true;
    f = g_feeder.get();
    if (f == nullptr) return false;
  }
  if (resize && !f->compose_ready) return false;  // MakeFeeder said why
  const UINT frame = static_cast<UINT>(f->count % compose::FRAMES);
  if (f->fence->GetCompletedValue() < f->alloc_value[frame]) {
    f->fence->SetEventOnCompletion(f->alloc_value[frame], f->event);
    WaitForSingleObject(f->event, 200);
  }
  if (f->fence->GetCompletedValue() < f->alloc_value[frame]) {
    Line("the GPU is over 200 ms behind: a frame without inputs");
    return false;
  }
  const bool reset = in.reset || (in.tag == nullptr && (!g_options.set || !g_options.on));
  ID3D12GraphicsCommandList* const list = f->list.Get();
  f->alloc[frame]->Reset();
  list->Reset(f->alloc[frame].Get(), nullptr);
  const bool composing = f->compose_ready && in.compose;
  const bool masking = f->mask_ready && in.mask;
  if (!composing && f->had_compose) f->compose.ForgetHistory();
  f->had_compose = composing;
  // The flow everything below works on: the effect's, or NVIDIA Optical Flow's for this frame in the same size, format and scale. A frame the engine has
  // nothing for (its first, or after a failure) keeps the effect's. The back buffer cannot be read by a shader where it is, so the engine's side copies it.
  ID3D12Resource* flow = in.motion;
  bool flow_made = false;
  if (in.nvof) {
    nvof::Frame n;
    n.colour = in.back_buffer;
    n.colour_view = ReadableFormat(back.Format);
    n.colour_state = static_cast<D3D12_RESOURCE_STATES>(in.back_buffer_state);
    n.copy_colour = true;
    n.depth = in.depth;
    n.depth_inverted = in.depth_inverted;
    n.width = f->input_width;
    n.height = f->input_height;
    n.format = f->motion_format;
    n.scale[0] = in.scale[0];
    n.scale[1] = in.scale[1];
    n.reset = in.reset;
    n.size = in.nvof_size;
    if (ID3D12Resource* const made = f->nvof.Run(in.device, in.queue, list, n, &NvofLine)) {
      flow = made;
      flow_made = true;
    }
    g_nvof_state = static_cast<int>(f->nvof.GetState());
    if (flow == nullptr) {
      flow = f->no_motion.Get();
      flow_made = true;  // same state as NVOF's result
    }
  } else {
    f->nvof.Forget();
    g_nvof_state = 0;
  }
  const auto mvec_to = [&](D3D12_RESOURCE_STATES state) {
    if (f->mvec_state != state) Barrier(list, f->mvec.Get(), f->mvec_state, state);
    f->mvec_state = state;
  };
  if (composing) {
    ID3D12Resource* const composed_to = masking ? f->composed.Get() : f->mvec.Get();
    if (!masking) mvec_to(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    f->compose.Record(list, frame, flow, in.depth, composed_to, f->input_width, f->input_height, in.depth_inverted, in.scale, reset);
  }
  if (masking) {
    if (f->colour_state != D3D12_RESOURCE_STATE_COPY_DEST) Barrier(list, f->colour.Get(), f->colour_state, D3D12_RESOURCE_STATE_COPY_DEST);
    Barrier(list, in.back_buffer, static_cast<D3D12_RESOURCE_STATES>(in.back_buffer_state), D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(f->colour.Get(), in.back_buffer);
    Barrier(list, in.back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, static_cast<D3D12_RESOURCE_STATES>(in.back_buffer_state));
    Barrier(list, f->colour.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    f->colour_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (composing) Barrier(list, f->composed.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    mvec_to(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    f->mask.Record(list, frame, f->colour.Get(), composing ? f->composed.Get() : flow, f->mvec.Get(), in.scale, false);
    if (composing) Barrier(list, f->composed.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  }
  const bool own_motion = composing || masking;
  if (own_motion) mvec_to(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
  // Streamline copies both while this list runs, as they are only sure to hold this frame's data until the next frame's effects.
  const bool partial = in.width != 0 && in.height != 0;
  const sl::Extent extent{partial ? in.y : 0, partial ? in.x : 0, partial ? in.width : f->input_width, partial ? in.height : f->input_height};
  ID3D12Resource* const tag_motion = own_motion ? f->mvec.Get() : flow;
  const uint32_t motion_state = static_cast<uint32_t>(own_motion || flow_made ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : READ_ANY);
  if (in.tag != nullptr) {
    const uint32_t ext[4] = {extent.left, extent.top, extent.width, extent.height};
    in.tag(list, in.depth, static_cast<uint32_t>(READ_ANY), tag_motion, motion_state, ext, in.scale, composing || in.dilated);
  } else {
    sl::Resource depth(sl::ResourceType::eTex2d, in.depth, static_cast<uint32_t>(READ_ANY));
    sl::Resource mvec(sl::ResourceType::eTex2d, tag_motion, motion_state);
    sl::ResourceTag tags[] = {sl::ResourceTag(&depth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eOnlyValidNow, &extent), sl::ResourceTag(&mvec, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eOnlyValidNow, &extent)};
    const sl::Result tagged = g_sl_tag(*g_token, VIEWPORT, tags, static_cast<uint32_t>(std::size(tags)), reinterpret_cast<sl::CommandBuffer*>(list));
    if (tagged != sl::Result::eOk) {
      static int logged = 0;
      if (logged++ < 5) Line("slSetTagForFrame: %d", static_cast<int>(tagged));
    }
  }
  // The view goes over the picture after the tags, so DLSS-G still gets this frame's real inputs.
  if (in.view != 0) {
    if (!f->view_tried) {
      f->view_tried = true;
      const char* what = nullptr;
      HRESULT hr = S_OK;
      f->view_ready = f->view.Init(in.device, back.Format, f->width, f->height, what, hr);
      Line("depth and motion view: %s%s (0x%08lx)", f->view_ready ? "ready" : "not available: ", f->view_ready ? "" : what, hr);
    }
    if (f->view_ready)
      f->view.Record(list, frame, static_cast<debugview::Kind>(in.view), tag_motion, in.depth, partial ? in.x : 0, partial ? in.y : 0, partial ? in.width : 0, partial ? in.height : 0,
                     in.depth_inverted, in.scale, in.back_buffer, static_cast<D3D12_RESOURCE_STATES>(in.back_buffer_state));
  }
  list->Close();
  ID3D12CommandList* const lists[] = {list};
  in.queue->ExecuteCommandLists(1, lists);
  in.queue->Signal(f->fence.Get(), ++f->value);
  f->alloc_value[frame] = f->value;
  ++f->count;
  double ms = 0;
  if (composing && f->compose.TakeAverageMs(600, ms)) Line("compose pass: %.2f ms on the GPU", ms);
  if (masking && f->mask.TakeAverageMs(600, ms)) Line("HUD mask: %.2f ms on the GPU", ms);
  // Someone else's Streamline: its owner sends the constants, options and markers.
  if (in.tag != nullptr) return true;

  SetConstants(in, composing || in.dilated, reset, f->width, f->height);
  SetOptions(in.on, in.frames, f->width, f->height, f->input_width, f->input_height, ReadableFormat(back.Format), motion_desc.Format, partial, extent.width, extent.height);
  if (!g_marked_submit_end) {
    Mark(sl::PCLMarker::eRenderSubmitEnd);
    Mark(sl::PCLMarker::ePresentStart);
    g_marked_submit_end = true;
  }
  g_fed_this_frame = true;
  g_fed.fetch_add(1);
  return true;
}
}  // namespace

void Stop(bool process_exiting) {
  if (!process_exiting && g_table != nullptr) {
    WriteSlot(g_table, SLOT_CREATE_SWAP_CHAIN, reinterpret_cast<void*>(g_real_create));
    WriteSlot(g_table, SLOT_CREATE_FOR_HWND, reinterpret_cast<void*>(g_real_for_hwnd));
    g_table = nullptr;
    if (g_native_table != nullptr) {
      WriteSlot(g_native_table, SLOT_CREATE_SWAP_CHAIN, reinterpret_cast<void*>(g_native_create));
      WriteSlot(g_native_table, SLOT_CREATE_FOR_HWND, reinterpret_cast<void*>(g_native_for_hwnd));
      g_native_table = nullptr;
    }
    if (g_present_table != nullptr) {
      WriteSlot(g_present_table, SLOT_PRESENT, reinterpret_cast<void*>(g_real_present));
      WriteSlot(g_present_table, SLOT_PRESENT1, reinterpret_cast<void*>(g_real_present1));
      g_present_table = nullptr;
    }
    g_feeder.reset();
  } else {
    // The game's queue may be gone already, and the feeder's parts would wait on it.
    (void)g_feeder.release();
  }
  g_sl_dlssg_state = nullptr;
  Line("=== stopped catching swap chains ===");
  AcquireSRWLockExclusive(&g_log_lock);
  if (g_log != nullptr) {
    fclose(g_log);
    g_log = nullptr;
  }
  ReleaseSRWLockExclusive(&g_log_lock);
}

Stats GetStats() {
  Stats s{};
  s.streamline_up = g_up.load();
  s.swap_chains = g_swap_chains.load();
  s.failed = g_failed.load();
  s.plugin_loaded = g_plugin_loaded.load();
  s.supported = g_supported.load();
  s.status = g_status.load();
  s.max_frames = g_max_frames.load();
  s.generating = g_generating.load();
  s.shown_per_real = g_shown_per_real.load();
  s.nvof_state = g_nvof_state.load();
  return s;
}

}  // namespace hsf::slbring
