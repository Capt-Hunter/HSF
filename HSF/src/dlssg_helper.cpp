// hsf-dlssg-helper64.exe: DLSS Frame Generation for D3D11 and DX9 games, 32-bit and 64-bit. See dlssg_link.h.
// Started by the add-on (dlssg_remote.cpp) from <game>\HSF_streamline with the name of the shared block on the command line. Everything
// after "the shared block is open" is the presenter side of DLSS-G (dlssg_core.h), in its own 64-bit process:
//   presenter thread: dlssg_core's Run; uses the game frame's token and the inputs in the block
//   Reflex thread:    the add-on's messages (dlssg_link.h): a token per game frame, Reflex's sleep while the game thread waits for the answer,
//                     the game frame's markers
//   window thread:    a disabled WS_CHILD of the game's window (another process's window), following its client size
//   main thread:      waits for the add-on's stop flag or for the game process to end
// No ReShade here, so the Direct3D 12 device is a plain one. Log: hsf_dlssg.log next to this exe.

#include "dlssg_core.h"
#include "dlssg_link.h"

#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <sl.h>
#include <sl_consts.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include "reflex_report.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <share.h>
#include <string>
#include <thread>

using Microsoft::WRL::ComPtr;
namespace link = hsf::dlssg_link;
namespace core = hsf::dlssg_core;
using core::Log;
using core::RING;
using core::Sl;

namespace {

link::Block* g_b = nullptr;
std::atomic<bool> g_stop{false};

std::wstring g_folder;  // this exe's folder: Streamline's DLLs and the log
std::wstring g_record_folder;  // HSF_recordings (camera_record.h)

// ---- Streamline and the Direct3D 12 device (the shared part is dlssg_core.h's) ----
ComPtr<ID3D12Device> g_device;

bool InitStreamline() {
  // The game's adapter, by LUID, through the system dxgi.dll (this folder has none). Found before Streamline loads, as its name goes in the errors.
  LUID luid{g_b->adapter_luid_low, g_b->adapter_luid_high};
  using CreateFactoryFn = HRESULT(WINAPI*)(REFIID, void**);
  const auto create_factory = reinterpret_cast<CreateFactoryFn>(GetProcAddress(LoadLibraryW(L"dxgi.dll"), "CreateDXGIFactory1"));
  ComPtr<IDXGIFactory4> factory;
  ComPtr<IDXGIAdapter1> adapter;
  if (create_factory == nullptr || FAILED(create_factory(IID_PPV_ARGS(&factory))) ||
      FAILED(factory->EnumAdapterByLuid(luid, IID_PPV_ARGS(&adapter))))
    return core::Fail("the game's adapter (LUID %08lx:%08lx) was not found", static_cast<unsigned long>(luid.HighPart), luid.LowPart);
  DXGI_ADAPTER_DESC1 adesc{};
  adapter->GetDesc1(&adesc);
  if (!core::InitStreamline(g_folder.c_str(), luid, adesc.Description)) return false;

  using CreateDeviceFn = HRESULT(WINAPI*)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
  const auto create_device = reinterpret_cast<CreateDeviceFn>(GetProcAddress(LoadLibraryW(L"d3d12.dll"), "D3D12CreateDevice"));
  if (create_device == nullptr) return core::Fail("d3d12.dll has no D3D12CreateDevice");
  const HRESULT hr = create_device(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_device));
  if (FAILED(hr)) return core::Fail("creating the helper's Direct3D 12 device (0x%08lx)", hr);
  Log("Direct3D 12 device on %ls", adesc.Description);
  if (!core::UseDevice(g_device.Get())) return false;
  g_b->max_frames = static_cast<int32_t>(Sl().max_frames);
  return true;
}

// ---- The presenter ----
struct Presenter : core::Presenter {
  ComPtr<ID3D12Resource> motion[RING], depth[RING], hudless[RING];
  bool motion_open = false, motion_failed = false, depth_open = false, depth_failed = false, hudless_open = false, hudless_failed = false;
} g_p;

// One input's slots, once the add-on has made them (`ready`).
bool OpenSlots(int32_t ready, const uint64_t* handles, ComPtr<ID3D12Resource>* slots, bool& open, bool& failed, const char* name) {
  if (open || failed) return open;
  if (ready == 0) return false;
  failed = true;
  HRESULT hr = S_OK;
  for (UINT i = 0; i < RING && SUCCEEDED(hr); ++i) {
    const HANDLE handle = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(handles[i]));
    hr = g_device->OpenSharedHandle(handle, IID_PPV_ARGS(&slots[i]));
    CloseHandle(handle);
  }
  if (FAILED(hr)) {
    Log("opening the %s slots failed (0x%08lx)", name, hr);
    return false;
  }
  failed = false;
  open = true;
  return true;
}

// ---- Reflex on the game's frame (dlssg_link.h messages) ----
// The game thread's Streamline tokens by index (one slGetNewFrameToken per index, ever); the presenter finds a slot's token here.
constexpr uint32_t TOKENS = 64;
struct TokenEntry {
  std::atomic<uint32_t> index{0};
  std::atomic<sl::FrameToken*> token{nullptr};
} g_tokens[TOKENS];
std::atomic<bool> g_presented{false};        // the presenter has presented once: before that Reflex has nothing to pace by
std::atomic<uint32_t> g_own_index{0x80000000u};  // indices for frames with no game token (never the game's range)
int32_t g_applied_cap = -1, g_applied_boost = -1;

// Presenter thread: one frame's latency on HSF's timer, from its start on the game thread (QPC from the add-on) to now.
void NoteLatency(int64_t start) {
  if (start == 0) return;
  LARGE_INTEGER now, freq;
  QueryPerformanceCounter(&now);
  QueryPerformanceFrequency(&freq);
  const int64_t us = (now.QuadPart - start) * 1000000 / freq.QuadPart;
  if (us <= 0 || us > 1000000) return;
  const LONG value = static_cast<LONG>(us);
  InterlockedAdd(reinterpret_cast<volatile LONG*>(&g_b->latency_sum_us), value);
  InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_b->latency_count));
  LONG worst = g_b->latency_max_us;
  while (value > worst) {
    const LONG seen = InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&g_b->latency_max_us), value, worst);
    if (seen == worst) break;
    worst = seen;
  }
  const int32_t at = g_b->latency_write;
  g_b->latency_us[static_cast<uint32_t>(at) % link::LATENCY_HISTORY] = value;
  InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_b->latency_write), at + 1);
}

sl::FrameToken* FindToken(uint32_t index) {
  if (index == 0) return nullptr;
  TokenEntry& e = g_tokens[index % TOKENS];
  sl::FrameToken* const token = e.token.load();
  return e.index.load() == index ? token : nullptr;
}

void ApplyReflex() {
  const int32_t cap = std::max<int32_t>(0, static_cast<int32_t>(g_b->fps_cap)), boost = g_b->boost != 0 ? 1 : 0;
  if (cap == g_applied_cap && boost == g_applied_boost) return;
  g_applied_cap = cap;
  g_applied_boost = boost;
  sl::ReflexOptions reflex{};
  reflex.mode = boost ? sl::ReflexMode::eLowLatencyWithBoost : sl::ReflexMode::eLowLatency;
  reflex.frameLimitUs = cap != 0 ? 1000000u / static_cast<uint32_t>(cap) : 0;
  const sl::Result res = slReflexSetOptions(reflex);
  Log("Reflex: low latency%s, FPS cap %s%d (%u us per frame on screen): %d", boost ? " + boost" : "", cap != 0 ? "" : "off ", cap,
      reflex.frameLimitUs, static_cast<int>(res));
}

// Takes the add-on's messages as they come: tokens, Reflex sleeps and markers for the game's frames. The game thread waits for each sleep's
// answer (with a time-out), so this thread does nothing else.
void ReflexThread() {
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
  const HANDLE request = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(g_b->request_event));
  const HANDLE done = reinterpret_cast<HANDLE>(static_cast<uintptr_t>(g_b->done_event));
  ULONGLONG next_state = 0;
  uint32_t newest_index = 0;  // the highest frame index the add-on has sent
  while (!g_stop.load()) {
    WaitForSingleObject(request, 100);
    if (GetTickCount64() >= next_state) {
      next_state = GetTickCount64() + 1000;
      static sl::ReflexState state{};  // large (per-frame reports): not on the stack
      static uint64_t report_frame = 0;
      const bool ok = slReflexGetState(state) == sl::Result::eOk;
      InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_b->reflex_available), ok && state.lowLatencyAvailable ? 1 : 0);
      // The latency report's summary for the overlay: the numbers first, then report_frames (the add-on takes that one).
      const hsf::reflex_report::Summary s = ok ? hsf::reflex_report::Summarize(state, report_frame, newest_index) : hsf::reflex_report::Summary{};
      static uint32_t raw_log = 0, foreign_logs = 0;
      if (ok && (raw_log++ % 10 == 0 || (s.foreign != 0 && foreign_logs++ < 20))) Log("Reflex report: %s", s.raw);
      g_b->report_total_us = s.total_us;
      g_b->report_game_us = s.game_us;
      g_b->report_submit_us = s.submit_us;
      g_b->report_handoff_us = s.handoff_us;
      g_b->report_present_us = s.present_us;
      g_b->report_gpu_us = s.gpu_us;
      InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_b->report_frames), s.frames);
    }
    while (!g_stop.load() && g_b->message_read != g_b->message_write) {
      const int32_t read = g_b->message_read;
      const int64_t m = g_b->messages[static_cast<uint32_t>(read) % link::MESSAGES];
      InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_b->message_read), read + 1);
      const uint32_t kind = link::MessageKind(m);
      uint32_t index = link::MessageIndex(m);
      newest_index = index > newest_index ? index : newest_index;
      if (kind == link::MSG_SLEEP || kind == link::MSG_FRAME) {
        ApplyReflex();
        sl::FrameToken* token = nullptr;
        if (slGetNewFrameToken(token, &index) == sl::Result::eOk && token != nullptr) {
          TokenEntry& e = g_tokens[index % TOKENS];
          e.index = 0;
          e.token = token;
          e.index = index;
          if (kind == link::MSG_SLEEP && g_presented.load() && g_b->sleep_on != 0) {
            LARGE_INTEGER before, after, freq;
            QueryPerformanceCounter(&before);
            slReflexSleep(*token);
            QueryPerformanceCounter(&after);
            QueryPerformanceFrequency(&freq);
            const LONG us = static_cast<LONG>((after.QuadPart - before.QuadPart) * 1000000 / freq.QuadPart);
            InterlockedAdd(reinterpret_cast<volatile LONG*>(&g_b->sleep_us), us);
            InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_b->sleeps));
            if (us > 250000) Log("Reflex held the game thread %ld ms (frame %u)", us / 1000, index);
          }
          slPCLSetMarker(sl::PCLMarker::eSimulationStart, *token);
        }
        if (kind == link::MSG_SLEEP) {
          InterlockedExchange64(&g_b->slept, static_cast<LONG64>(index));
          SetEvent(done);
        }
      } else if (sl::FrameToken* const token = FindToken(index)) {
        if (kind == link::MSG_SUBMIT_START) {
          slPCLSetMarker(sl::PCLMarker::eSimulationEnd, *token);
          slPCLSetMarker(sl::PCLMarker::eRenderSubmitStart, *token);
        } else if (kind == link::MSG_SUBMIT_END) {
          slPCLSetMarker(sl::PCLMarker::eRenderSubmitEnd, *token);
        }
      }
    }
  }
  // Stopping: the game thread must never wait for a sleep that will not come.
  InterlockedExchange64(&g_b->slept, INT64_MAX);
  SetEvent(done);
}

// The presenter thread's part in dlssg_core.cpp's Run.
class HelperHost final : public core::Host {
 public:
  bool Stopping() override { return g_stop.load(); }
  void SwapChainFailed() override { g_b->note = link::NOTE_NO_SWAP; }
  void CountSkipped(uint32_t frames) override { InterlockedAdd(reinterpret_cast<volatile LONG*>(&g_b->skipped), static_cast<LONG>(frames)); }
  sl::FrameToken* GameToken(UINT64 id) override { return FindToken(static_cast<uint32_t>(g_b->slot_index[id % RING])); }
  uint32_t OwnFrameIndex() override { return g_own_index.fetch_add(1) + 1; }
  core::FrameInputs Inputs(UINT64 id) override {
    const UINT slot = static_cast<UINT>(id % RING);
    core::FrameInputs in;
    in.reset = g_b->slot_reset[slot] != 0;
    in.compose = (g_b->slot_compose[slot] & link::FIX_COMPOSE) != 0;
    in.checks = (g_b->slot_compose[slot] & link::FIX_CHECKS) != 0;
    in.camera_motion = (g_b->slot_compose[slot] & link::FIX_CAMERA) != 0;
    in.camera_fit = (g_b->slot_compose[slot] & link::FIX_FIT) != 0;
    in.nvof = (g_b->slot_compose[slot] & link::FIX_NVOF) != 0;
    in.nvof_size = static_cast<UINT>((g_b->slot_compose[slot] >> link::NVOF_SIZE_SHIFT) & link::NVOF_SIZE_MASK);
    if ((g_b->slot_compose[slot] & link::FIX_RECORD) != 0) in.camera_record_folder = g_record_folder.c_str();
    in.camera = g_b->camera[slot];
    in.camera_count = g_b->camera_count[slot];
    in.camera_frame = g_b->camera_frame[slot];
    if (static_cast<UINT64>(g_b->motion_frame[slot]) == id && OpenSlots(g_b->motion_ready, g_b->motion, g_p.motion, g_p.motion_open, g_p.motion_failed, "motion vector")) {
      in.motion = g_p.motion[slot].Get();
      in.motion_width = g_b->motion_width;
      in.motion_height = g_b->motion_height;
      in.motion_format = static_cast<DXGI_FORMAT>(g_b->motion_format);
      in.motion_scale[0] = g_b->motion_scale[slot][0];
      in.motion_scale[1] = g_b->motion_scale[slot][1];
    }
    if (static_cast<UINT64>(g_b->depth_frame[slot]) == id && OpenSlots(g_b->depth_ready, g_b->depth, g_p.depth, g_p.depth_open, g_p.depth_failed, "depth")) {
      in.depth = g_p.depth[slot].Get();
      in.depth_width = g_b->depth_width;
      in.depth_height = g_b->depth_height;
      in.depth_inverted = g_b->depth_inverted[slot] != 0;
    }
    if (static_cast<UINT64>(g_b->hudless_frame[slot]) == id && OpenSlots(g_b->hudless_ready, g_b->hudless, g_p.hudless, g_p.hudless_open, g_p.hudless_failed, "HUD-less"))
      in.hudless = g_p.hudless[slot].Get();
    return in;
  }
  int64_t CameraHint() override { return g_b->camera_hint; }
  void CameraResult(const hsf::camera::Decision& decision, int64_t locked) override {
    g_b->camera_source = static_cast<int32_t>(decision.source);
    g_b->camera_share = static_cast<int32_t>(decision.share * 100.f);
    g_b->camera_locked = locked;
  }
  bool On() override { return g_b->on != 0; }
  bool TakeMouse() override { return g_b->menu_open != 0; }
  void NvofState(int state) override { g_b->nvof_state = state; }
  uint32_t Frames() override { return static_cast<uint32_t>(std::max<int32_t>(1, static_cast<int32_t>(g_b->frames))); }
  void AfterPresent(UINT64 id) override {
    g_presented = true;
    NoteLatency(InterlockedExchange64(&g_b->slot_start[id % RING], 0));
  }
  void CountFrame(const sl::DLSSGState& state, bool motion, bool on) override {
    InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_b->real));
    InterlockedAdd(reinterpret_cast<volatile LONG*>(&g_b->shown), static_cast<LONG>(std::max(1u, state.numFramesActuallyPresented)));
    if (state.numFramesActuallyPresented > 1) InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_b->generated_seen));
    InterlockedOr(reinterpret_cast<volatile LONG*>(&g_b->status), static_cast<LONG>(state.status));
    g_b->note = g_p.hang_off ? link::NOTE_GPU_HANG : !motion ? link::NOTE_NO_MOTION : !on ? link::NOTE_PAUSED : link::NOTE_NONE;
  }
};

void Run() {
  HelperHost host;
  core::Run(g_p, host);
}

// The frame slots and fences from the add-on's handles, and the command list.
bool Create(Presenter& p) {
  HRESULT hr = S_OK;
  p.width = g_b->width;
  p.height = g_b->height;
  p.format = static_cast<DXGI_FORMAT>(g_b->format);
  p.swap_format = core::SwapFormat(p.format);
  p.parent = reinterpret_cast<HWND>(static_cast<uintptr_t>(g_b->parent_window));
  p.overlay = g_b->overlay_window != 0;
  p.game_space = static_cast<DXGI_COLOR_SPACE_TYPE>(g_b->color_space);
  if (p.swap_format == DXGI_FORMAT_UNKNOWN) return core::Fail("back buffer format %u cannot go into the DLSS-G swap chain", static_cast<unsigned>(p.format));
  const auto handle = [](uint64_t value) { return reinterpret_cast<HANDLE>(static_cast<uintptr_t>(value)); };
  hr = g_device->OpenSharedHandle(handle(g_b->ready_fence), IID_PPV_ARGS(&p.ready));
  CloseHandle(handle(g_b->ready_fence));
  if (FAILED(hr)) return core::Fail("opening the game's ready fence (0x%08lx)", hr);
  hr = g_device->OpenSharedHandle(handle(g_b->consumed_fence), IID_PPV_ARGS(&p.consumed));
  CloseHandle(handle(g_b->consumed_fence));
  if (FAILED(hr)) return core::Fail("opening the game's consumed fence (0x%08lx)", hr);
  for (UINT i = 0; i < RING; ++i) {
    hr = g_device->OpenSharedHandle(handle(g_b->slots[i]), IID_PPV_ARGS(&p.slots[i]));
    CloseHandle(handle(g_b->slots[i]));
    if (FAILED(hr)) return core::Fail("opening the game's frame slot %u (0x%08lx)", i, hr);
  }
  if (const char* const what = core::MakeCommandObjects(p, hr)) return core::Fail("%s (0x%08lx)", what, hr);
  return true;
}

}  // namespace

// The graphics DLLs from System32, before anything else loads them by name. A child process of a packaged game (Game Pass) searches the
// game's package folder first, where a ReShade dxgi.dll (or d3d11.dll, d3d12.dll) would load into this helper and run its add-ons and
// effects here. LOAD_LIBRARY_SEARCH_SYSTEM32 does not stop that: a packaged process searches its package first even with that flag, and a
// Game Pass Arkham Knight log showed ReShade's dxgi.dll loaded by that very call. So each is loaded by its full System32 path, which is no
// search at all; every later load by name, Streamline's included, then finds the one already loaded. The helper does not import them, so
// nothing loads them before this runs. Returns false when the dxgi.dll in this process is not System32's (written to the shared block).
bool LoadSystemGraphicsDlls() {
  wchar_t system[MAX_PATH];
  const UINT length = GetSystemDirectoryW(system, MAX_PATH);
  if (length == 0 || length >= MAX_PATH) {
    Log("the System32 folder was not found (%lu)", GetLastError());
    return true;
  }
  const std::wstring folder(system, length);
  for (const wchar_t* const name : {L"dxgi.dll", L"d3d12.dll", L"d3d11.dll"}) {
    const std::wstring path = folder + L"\\" + name;
    if (LoadLibraryExW(path.c_str(), nullptr, 0) == nullptr) Log("%ls was not loaded (%lu)", path.c_str(), GetLastError());
  }
  const HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
  wchar_t loaded[MAX_PATH] = L"";
  if (dxgi != nullptr) GetModuleFileNameW(dxgi, loaded, MAX_PATH);
  const std::wstring got = loaded;
  const bool system_dxgi = got.size() > folder.size() && _wcsnicmp(got.c_str(), folder.c_str(), folder.size()) == 0 && got[folder.size()] == L'\\';
  Log(system_dxgi ? "dxgi.dll in the helper: %ls" : "dxgi.dll in the helper is NOT Windows' own: %ls (ReShade and its add-ons run in the helper)", loaded);
  return system_dxgi;
}

int wmain(int argc, wchar_t** argv) {
  wchar_t exe[MAX_PATH];
  GetModuleFileNameW(nullptr, exe, MAX_PATH);
  g_folder = exe;
  g_folder.resize(g_folder.find_last_of(L'\\'));
  g_record_folder = g_folder.substr(0, g_folder.find_last_of(L'\\')) + L"\\HSF_recordings";  // beside this exe's folder, in the game's
  // Appended, shared: a stopping helper and the next one can overlap (the add-on starts the file fresh once per game run).
  core::OpenLog((g_folder + L"\\hsf_dlssg.log").c_str(), L"a", _SH_DENYNO);
  Log("HSF DLSS-G helper (64-bit), pid %lu", GetCurrentProcessId());
  const bool system_dxgi = LoadSystemGraphicsDlls();
  if (argc < 2) {
    Log("no shared block named on the command line");
    return 1;
  }
  const HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, argv[1]);
  if (mapping != nullptr) g_b = static_cast<link::Block*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(link::Block)));
  if (g_b == nullptr || g_b->magic != link::MAGIC || g_b->version != link::VERSION) {
    Log("shared block %ls missing or of another version", argv[1]);
    return 1;
  }
  g_b->foreign_dxgi = system_dxgi ? 0 : 1;
  const HANDLE game = OpenProcess(SYNCHRONIZE, FALSE, g_b->game_pid);
  if (game == nullptr) {
    Log("cannot watch the game process %lu (%lu)", static_cast<unsigned long>(g_b->game_pid), GetLastError());
    return 1;
  }
  // Watchdog, from the start (Streamline's set-up takes seconds): the add-on's stop flag or the game's end sets g_stop; if this process is not
  // gone 3 s later (a thread stuck in Streamline, DXGI or the window), it ends itself. The add-on never waits for it.
  std::thread([game] {
    while (g_b->stop == 0 && WaitForSingleObject(game, 20) == WAIT_TIMEOUT) {
    }
    if (g_b->stop == 0) {
      // The game is gone: nothing to hand back, nothing to clean up. End at once (TerminateProcess, not ExitProcess: ExitProcess waited ~3.5 s in
      // DLL detach during Streamline's set-up, tools/helper_exit_test.ps1). The add-on's job object normally ends this process before this runs.
      Log("the game process ended: ending the helper now");
      TerminateProcess(GetCurrentProcess(), 0);
    }
    Log("stop asked by the add-on");
    g_stop = true;
    if (g_p.ready_event != nullptr) SetEvent(g_p.ready_event);
    Sleep(3000);
    Log("not stopped after 3 s: ending the helper");
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_b->state), link::STATE_STOPPED);
    TerminateProcess(GetCurrentProcess(), 2);
  }).detach();
  if (!InitStreamline() || !Create(g_p)) {
    if (g_stop.load()) TerminateProcess(GetCurrentProcess(), 0);  // stopped during set-up: not a failure
    strncpy_s(g_b->error, Sl().error, _TRUNCATE);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_b->state), link::STATE_FAILED);
    TerminateProcess(GetCurrentProcess(), 1);
  }
  if (g_stop.load()) {
    Log("stopped during set-up");
    TerminateProcess(GetCurrentProcess(), 0);
  }
  Log("started: %ux%u format %u on the game's window %p", g_p.width, g_p.height, static_cast<unsigned>(g_p.format), static_cast<void*>(g_p.parent));
  ApplyReflex();  // Reflex on (DLSS-G needs it) before the first frame
  InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_b->state), link::STATE_RUNNING);
  std::thread window([] { core::WindowThread(g_p, &g_stop); });
  std::thread reflex(ReflexThread);
  std::thread presenter(Run);
  presenter.join();  // Run returns once g_stop is set (the watchdog wakes it)
  reflex.join();     // wakes at least every 100 ms and sees g_stop
  Log("stopping: presenter done, waiting for the window thread");
  window.join();     // the window thread destroys its child on its next timer tick once g_stop is set
  InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_b->state), link::STATE_STOPPED);
  Log("stopped");
  TerminateProcess(GetCurrentProcess(), 0);
}
