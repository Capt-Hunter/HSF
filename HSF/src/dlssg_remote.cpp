// HSF's DLSS-G presenter: the game side of dlssg.h, with the presenter in hsf-dlssg-helper64.exe (dlssg_helper.cpp). 32-bit games need it as Streamline
// has no 32-bit build; 64-bit games use it because RTSS's overlay ends the game when DLSS-G runs in the same process as ReShade.
// See dlssg_link.h. The game's Direct3D 11 device makes the frame and motion vector slots and both fences (shared, NT handles); their handles
// are duplicated into the helper, which opens them on its Direct3D 12 device. Per game frame (game thread): CPU slot check on `consumed`
// (never a GPU wait) -> copy back buffer and motion vectors -> signal `ready`. Logs: HSF_streamline\hsf_dlssg_game.log (this side) and
// HSF_streamline\hsf_dlssg.log (the helper).
// Reflex on the game's frame: after the game's Present (FrameStart) the game thread asks the helper to sleep on Reflex and waits for its
// answer; Submit sends the render submit markers and writes the frame's Streamline index into its slot (dlssg_link.h messages).

#include "dlssg.h"
#include "dlssg_link.h"
#include "dlssg_slots.h"

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <share.h>
#include <string>

using Microsoft::WRL::ComPtr;
namespace link = hsf::dlssg_link;

namespace hsf::dlssg {
namespace {

constexpr UINT RING = link::RING;

FILE* g_log = nullptr;

void Log(const char* format, ...) {
  char text[1024];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  if (g_log == nullptr) return;
  SYSTEMTIME now;
  GetLocalTime(&now);
  std::fprintf(g_log, "%02u:%02u:%02u.%03u %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, text);
  std::fflush(g_log);
}

struct Remote {
  ComPtr<ID3D11Device5> device11;
  ComPtr<ID3D11DeviceContext4> context11;
  ComPtr<ID3D11Texture2D> slots[RING];
  dlssg_slots::InputSlots<RING> motion, depth, hudless;
  ComPtr<ID3D11Fence> ready, consumed;
  UINT64 next_id = 1;
  UINT width = 0, height = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  bool reset_pending = false;  // a reset asked for on a frame that never got a slot goes with the next frame that does
  HANDLE mapping = nullptr;
  link::Block* b = nullptr;
  HANDLE process = nullptr;
  HANDLE request_event = nullptr, done_event = nullptr;  // Reflex messages (dlssg_link.h); duplicates are in the helper
  uint32_t game_index = 0;                               // the game frame in progress (FrameStart -> Submit), 0 = none
  LONGLONG game_start = 0;                               // QPC where that frame started (after Reflex's sleep), for HSF's latency timer
  HANDLE pace_event = nullptr;                           // Submit's wait for the helper (made on first use)
  // Pacing starts once the helper has taken a frame, so a helper still starting never holds the game. A wait that runs out pauses it until
  // pace_resume (QPC).
  UINT64 pace_taken = 0;
  LONGLONG pace_resume = 0;
  // The part of Submit's wait moved to the frame's start (FrameStart), before the game reads its input: same pace, a younger frame. Learned from
  // Submit's waits, which then stay near EARLY_LEFT_MS.
  double early_ms = 0;
  HANDLE early_timer = nullptr;
  // Timing of Submit's copies on the game's GPU, a report only: a disjoint query and two timestamps per frame, read back COPY_TIMES frames later.
  static constexpr UINT COPY_TIMES = 4;
  ComPtr<ID3D11Query> copy_disjoint[COPY_TIMES], copy_begin[COPY_TIMES], copy_end[COPY_TIMES];
  bool copy_timed[COPY_TIMES] = {};
  UINT copy_frame = 0;
  bool copy_timing_failed = false;
};

std::unique_ptr<Remote> g_r;
char g_failed[300] = "";  // set once the helper failed: later Starts refuse with it
bool g_on = true;
uint32_t g_frames = 1;
uint32_t g_skipped = 0, g_max_frames = 0;
double g_copy_ms_sum = 0;  // Submit's copies on the GPU (start to end), and the frames counted
uint32_t g_copy_ms_count = 0;
uint32_t g_paced = 0;       // game frames that waited for the helper (Submit), and how long in all
LONGLONG g_paced_ticks = 0;
uint32_t g_early = 0;  // game frames that waited at their start (FrameStart), and how long in all
LONGLONG g_early_ticks = 0;
uint32_t g_cap_fps = 0;
bool g_boost = false;
bool g_overlay_window = false;  // SetOverlayWindow, for the next Start only
uint32_t g_color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;  // SetColorSpace, for the next Start only
uint32_t g_frame_counter = 0;  // Streamline frame indices of the game's frames: never repeated in this process
uint32_t g_sleep_timeouts = 0;
bool g_sleep_on = true;
int64_t g_camera_hint = 0;

LONGLONG Qpc() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}

void Finish() {
  if (g_r) {
    if (g_r->b != nullptr) UnmapViewOfFile(g_r->b);
    if (g_r->mapping != nullptr) CloseHandle(g_r->mapping);
    if (g_r->process != nullptr) CloseHandle(g_r->process);
    if (g_r->request_event != nullptr) CloseHandle(g_r->request_event);
    if (g_r->done_event != nullptr) CloseHandle(g_r->done_event);
    if (g_r->pace_event != nullptr) CloseHandle(g_r->pace_event);
    if (g_r->early_timer != nullptr) CloseHandle(g_r->early_timer);
    g_r.reset();
  }
}

// One Reflex message to the helper (dlssg_link.h). False when the helper is not taking them (not running, or MESSAGES behind).
bool Send(Remote& r, uint32_t kind, uint32_t index) {
  link::Block* const b = r.b;
  if (b->state != link::STATE_RUNNING) return false;
  const int32_t write = b->message_write;
  if (write - b->message_read >= static_cast<int32_t>(link::MESSAGES)) return false;
  InterlockedExchange64(&b->messages[static_cast<uint32_t>(write) % link::MESSAGES], link::Message(kind, index));
  InterlockedExchange(reinterpret_cast<volatile LONG*>(&b->message_write), write + 1);
  SetEvent(r.request_event);
  return true;
}

// The helper ended by itself (failed set-up or crash): note why, free everything.
void CheckHelper() {
  if (!g_r || WaitForSingleObject(g_r->process, 0) != WAIT_OBJECT_0) return;
  DWORD code = 0;
  GetExitCodeProcess(g_r->process, &code);
  if (g_r->b->state == link::STATE_FAILED)
    std::snprintf(g_failed, sizeof(g_failed), "DLSS-G helper: %s", g_r->b->error);
  else
    std::snprintf(g_failed, sizeof(g_failed), "the 64-bit DLSS-G helper exited (code 0x%08lx, see HSF_streamline\\hsf_dlssg.log)",
                  static_cast<unsigned long>(code));
  Log("%s", g_failed);
  Finish();
}

// Duplicates `handle` (closing ours) into the helper; 0 on failure.
uint64_t Give(HANDLE process, HANDLE handle) {
  HANDLE theirs = nullptr;
  if (!DuplicateHandle(GetCurrentProcess(), handle, process, &theirs, 0, FALSE, DUPLICATE_SAME_ACCESS | DUPLICATE_CLOSE_SOURCE)) {
    CloseHandle(handle);
    return 0;
  }
  return static_cast<uint64_t>(reinterpret_cast<uintptr_t>(theirs));
}

// Game thread: the shared motion vector slots, made on the first frame that has vectors and handed to the helper. Tried once per Start.
bool MakeMotionSlots(Remote& r, ID3D11Texture2D* source) {
  return r.motion.Prepare(
      r.device11.Get(), source, "motion vectors", Log,
      [&](UINT i, HANDLE handle) {
        r.b->motion[i] = Give(r.process, handle);
        return r.b->motion[i] != 0 ? S_OK : HRESULT_FROM_WIN32(GetLastError());
      },
      [&](UINT width, UINT height, DXGI_FORMAT format) {
        r.b->motion_width = width;
        r.b->motion_height = height;
        r.b->motion_format = static_cast<uint32_t>(format);
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&r.b->motion_ready), 1);
      });
}

// Game thread: the same for depth.
bool MakeDepthSlots(Remote& r, ID3D11Texture2D* source) {
  return r.depth.Prepare(
      r.device11.Get(), source, "depth", Log,
      [&](UINT i, HANDLE handle) {
        r.b->depth[i] = Give(r.process, handle);
        return r.b->depth[i] != 0 ? S_OK : HRESULT_FROM_WIN32(GetLastError());
      },
      [&](UINT width, UINT height, DXGI_FORMAT format) {
        r.b->depth_width = width;
        r.b->depth_height = height;
        r.b->depth_format = static_cast<uint32_t>(format);
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&r.b->depth_ready), 1);
      });
}

bool MakeHudlessSlots(Remote& r, ID3D11Texture2D* source) {
  return r.hudless.Prepare(
      r.device11.Get(), source, "HUD-less image", Log,
      [&](UINT i, HANDLE handle) {
        r.b->hudless[i] = Give(r.process, handle);
        return r.b->hudless[i] != 0 ? S_OK : HRESULT_FROM_WIN32(GetLastError());
      },
      [&](UINT, UINT, DXGI_FORMAT) { InterlockedExchange(reinterpret_cast<volatile LONG*>(&r.b->hudless_ready), 1); });
}

bool Create(Remote& r, ID3D11Device* game_device, HWND window, UINT width, UINT height, DXGI_FORMAT format, const wchar_t* sl_folder, char* error,
            size_t error_size) {
  HRESULT hr = S_OK;
  const auto fail = [&](const char* what) {
    std::snprintf(error, error_size, "%s (0x%08x, %lu)", what, static_cast<unsigned>(hr), GetLastError());
    Log("%s", error);
    return false;
  };
  r.width = width;
  r.height = height;
  r.format = format;
  ComPtr<ID3D11DeviceContext> immediate;
  if (FAILED(hr = game_device->QueryInterface(IID_PPV_ARGS(&r.device11)))) return fail("the game's device has no ID3D11Device5 (fences)");
  r.device11->GetImmediateContext(&immediate);
  if (FAILED(hr = immediate.As(&r.context11))) return fail("the game's context has no ID3D11DeviceContext4 (fences)");
  ComPtr<IDXGIDevice> dxgi_device;
  ComPtr<IDXGIAdapter> adapter;
  DXGI_ADAPTER_DESC adesc{};
  if (FAILED(hr = game_device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) || FAILED(hr = dxgi_device->GetAdapter(&adapter)) ||
      FAILED(hr = adapter->GetDesc(&adesc)))
    return fail("the game's adapter");

  const std::wstring folder = sl_folder;
  const std::wstring exe = folder + L"\\" + link::HELPER_EXE;
  if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
    hr = E_FAIL;
    std::snprintf(error, error_size, "the 64-bit DLSS-G helper is missing: %ls", exe.c_str());
    Log("%s", error);
    return false;
  }

  // The shared block, named for this process and Start.
  wchar_t name[96];
  swprintf_s(name, L"Local\\HSF_DLSSG_%lu_%llu", GetCurrentProcessId(), static_cast<unsigned long long>(GetTickCount64()));
  r.mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(link::Block), name);
  if (r.mapping == nullptr) return hr = E_FAIL, fail("the shared block");
  r.b = static_cast<link::Block*>(MapViewOfFile(r.mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(link::Block)));
  if (r.b == nullptr) return hr = E_FAIL, fail("mapping the shared block");
  std::memset(r.b, 0, sizeof(link::Block));
  r.b->magic = link::MAGIC;
  r.b->version = link::VERSION;
  r.b->game_pid = GetCurrentProcessId();
  r.b->width = width;
  r.b->height = height;
  r.b->format = static_cast<uint32_t>(format);
  r.b->parent_window = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(window));
  r.b->adapter_luid_low = adesc.AdapterLuid.LowPart;
  r.b->adapter_luid_high = adesc.AdapterLuid.HighPart;
  r.b->on = g_on ? 1 : 0;
  r.b->frames = static_cast<int32_t>(g_frames);
  r.b->camera_hint = g_camera_hint;
  r.b->fps_cap = static_cast<int32_t>(g_cap_fps);
  r.b->boost = g_boost ? 1 : 0;
  r.b->overlay_window = g_overlay_window ? 1 : 0;
  g_overlay_window = false;  // one Start only: the Vulkan route sets it before each of its own
  r.b->color_space = static_cast<int32_t>(g_color_space);
  g_color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  r.b->sleep_on = g_sleep_on ? 1 : 0;

  // The helper, suspended until its handles are in the block. Its folder holds Streamline's 64-bit DLLs (and no 32-bit dxgi.dll).
  std::wstring command = L"\"" + exe + L"\" " + name;
  STARTUPINFOW si{sizeof(si)};
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, folder.c_str(), &si, &pi))
    return hr = E_FAIL, fail("starting the 64-bit DLSS-G helper");
  r.process = pi.hProcess;
  // The helper must never outlive the game. A job that kills its processes when its last handle closes: this process holds
  // the only handle and never closes it, so when the game ends in any way (exit, crash, killed) Windows ends the helper. The helper's own
  // watchdog (game process ended -> exit within 3 s) stays as the second line.
  static const HANDLE job = [] {
    const HANDLE j = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (j != nullptr && !SetInformationJobObject(j, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
      CloseHandle(j);
      return HANDLE(nullptr);
    }
    return j;
  }();
  if (job == nullptr || !AssignProcessToJobObject(job, pi.hProcess))
    Log("WARNING: the helper could not be tied to the game's lifetime (job object, error %lu): only its own watchdog ends it", GetLastError());
  const auto abandon = [&](const char* what) {
    TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hThread);
    return fail(what);
  };

  // Fences and frame slots: made by the game's device, handed to the helper.
  const auto share_fence = [&](ComPtr<ID3D11Fence>& fence, uint64_t& out) {
    HANDLE handle = nullptr;
    if (FAILED(hr = r.device11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))) ||
        FAILED(hr = fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle)))
      return false;
    return (out = Give(pi.hProcess, handle)) != 0;
  };
  if (!share_fence(r.ready, r.b->ready_fence) || !share_fence(r.consumed, r.b->consumed_fence)) return abandon("sharing the fences with the helper");
  // Reflex's events: ours stay open here, duplicates go to the helper.
  const auto share_event = [&](HANDLE& ours, uint64_t& out) {
    HANDLE theirs = nullptr;
    ours = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (ours == nullptr || !DuplicateHandle(GetCurrentProcess(), ours, pi.hProcess, &theirs, 0, FALSE, DUPLICATE_SAME_ACCESS)) return false;
    out = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(theirs));
    return true;
  };
  if (!share_event(r.request_event, r.b->request_event) || !share_event(r.done_event, r.b->done_event))
    return hr = E_FAIL, abandon("sharing Reflex's events with the helper");
  const D3D11_TEXTURE2D_DESC slot_desc{width, height, 1, 1, format, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                                       D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
  for (UINT i = 0; i < RING; ++i) {
    const HANDLE handle = dlssg_slots::SharedTexture(r.device11.Get(), slot_desc, r.slots[i], hr);
    if (handle == nullptr || (r.b->slots[i] = Give(pi.hProcess, handle)) == 0) return abandon("sharing the frame textures with the helper");
  }
  ResumeThread(pi.hThread);
  CloseHandle(pi.hThread);
  Log("helper started (pid %lu) for %ux%u format %u on window %p, block %ls", pi.dwProcessId, width, height, static_cast<unsigned>(format),
      static_cast<void*>(window), name);
  return true;
}

}  // namespace

bool Start(ID3D11Device* game_device, HWND window, UINT width, UINT height, DXGI_FORMAT format, const wchar_t* sl_folder, char* error,
           size_t error_size) {
  error[0] = '\0';
  if (g_log == nullptr) {
    const std::wstring path = std::wstring(sl_folder) + L"\\hsf_dlssg_game.log";
    g_log = _wfsopen(path.c_str(), L"w", _SH_DENYWR);
    // The helpers append to one log (one helper may still be finishing while the next starts): start it fresh once per game run.
    DeleteFileW((std::wstring(sl_folder) + L"\\hsf_dlssg.log").c_str());
    Log("HSF DLSS-G, game side: DLSS-G runs in %ls", link::HELPER_EXE);
  }
  Stop();
  if (g_failed[0] != '\0') {
    std::snprintf(error, error_size, "%s", g_failed);
    return false;
  }
  g_r = std::make_unique<Remote>();
  if (!Create(*g_r, game_device, window, width, height, format, sl_folder, error, error_size)) {
    Finish();
    return false;
  }
  return true;
}

// Never waits long: the helper only reads slots and fences it holds references to itself, and it exits by itself (stop flag, and a 3 s watchdog in
// the helper), so the game side lets go at once. Waiting here froze the game (Tomb Raider's start-up ResizeBuffers).
const char* Stop(DWORD wait_ms) {
  CheckHelper();
  if (!g_r) return nullptr;
  InterlockedExchange(reinterpret_cast<volatile LONG*>(&g_r->b->stop), 1);
  if (WaitForSingleObject(g_r->process, std::min<DWORD>(wait_ms, 50)) == WAIT_OBJECT_0)
    Log("stopped");
  else
    Log("stop asked; the helper finishes by itself (at most 3 s)");
  Finish();
  return nullptr;
}

bool Reap() { return false; }

bool Stopping() { return false; }

bool Running() {
  CheckHelper();
  return g_r != nullptr;
}

// The game thread waits for the helper's Reflex sleep: at most a little over two game frames at the cap, or 100 ms without one. A helper that
// does not answer (busy, stopping) never holds the game longer than that.
// The learned part of the pacing wait, at the frame's start: a high resolution timer, then a spin for the last part.
void EarlyWait(Remote& r) {
  if (r.early_ms < 0.2) return;
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  const LONGLONG from = Qpc();
  const LONGLONG until = from + static_cast<LONGLONG>(r.early_ms * static_cast<double>(freq.QuadPart) / 1000.0);
  const LONGLONG spin = freq.QuadPart / 2000;  // the last half millisecond
  if (r.early_timer == nullptr) {
    r.early_timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (r.early_timer == nullptr) r.early_timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  }
  if (until - from > spin && r.early_timer != nullptr) {
    LARGE_INTEGER due;
    due.QuadPart = -((until - from - spin) * 10000000 / freq.QuadPart);  // relative, 100 ns units
    if (SetWaitableTimerEx(r.early_timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(r.early_timer, 100);
  }
  while (Qpc() < until) YieldProcessor();
  ++g_early;
  g_early_ticks += Qpc() - from;
}

void FrameStart() {
  if (!Running()) return;
  Remote& r = *g_r;
  EarlyWait(r);
  const uint32_t index = ++g_frame_counter;
  r.game_index = 0;
  r.game_start = 0;
  // Sleep off (A/B test): a token and the simulation start marker, no wait.
  if (!g_sleep_on) {
    if (Send(r, link::MSG_FRAME, index)) {
      r.game_index = index;
      r.game_start = Qpc();
    }
    return;
  }
  if (!Send(r, link::MSG_SLEEP, index)) return;
  r.game_index = index;
  struct Started {
    Remote& r;
    ~Started() { r.game_start = Qpc(); }
  } started{r};
  const DWORD limit = 100 + (g_cap_fps != 0 ? 2000u * (g_frames + 1) / g_cap_fps : 0u);
  const ULONGLONG until = GetTickCount64() + limit;
  // 64-bit value in a 32-bit process: read in one piece.
  const auto slept = [&] { return InterlockedCompareExchange64(&r.b->slept, 0, 0); };
  while (slept() < static_cast<int64_t>(index)) {
    const ULONGLONG now = GetTickCount64();
    if (now >= until || WaitForSingleObject(r.done_event, static_cast<DWORD>(until - now)) == WAIT_TIMEOUT) {
      if (slept() < static_cast<int64_t>(index) && g_sleep_timeouts++ % 100 == 0)
        Log("Reflex sleep: no answer from the helper within %lu ms (frame %u; %u times so far)", limit, index, g_sleep_timeouts);
      break;
    }
  }
}

// Reads the timing of the frame that used this set COPY_TIMES frames ago, if the GPU has it (never waits), then starts this frame's.
UINT BeginCopyTiming(Remote& r) {
  if (r.copy_timing_failed) return Remote::COPY_TIMES;
  const UINT i = r.copy_frame++ % Remote::COPY_TIMES;
  if (r.copy_disjoint[i] == nullptr) {
    D3D11_QUERY_DESC disjoint{D3D11_QUERY_TIMESTAMP_DISJOINT, 0}, stamp{D3D11_QUERY_TIMESTAMP, 0};
    if (FAILED(r.device11->CreateQuery(&disjoint, &r.copy_disjoint[i])) || FAILED(r.device11->CreateQuery(&stamp, &r.copy_begin[i])) ||
        FAILED(r.device11->CreateQuery(&stamp, &r.copy_end[i]))) {
      r.copy_timing_failed = true;
      return Remote::COPY_TIMES;
    }
  }
  if (r.copy_timed[i]) {
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT d{};
    UINT64 begin = 0, end = 0;
    if (r.context11->GetData(r.copy_disjoint[i].Get(), &d, sizeof(d), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
        r.context11->GetData(r.copy_begin[i].Get(), &begin, sizeof(begin), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK &&
        r.context11->GetData(r.copy_end[i].Get(), &end, sizeof(end), D3D11_ASYNC_GETDATA_DONOTFLUSH) == S_OK && !d.Disjoint && d.Frequency != 0 && end >= begin) {
      g_copy_ms_sum += static_cast<double>(end - begin) * 1000.0 / static_cast<double>(d.Frequency);
      ++g_copy_ms_count;
    }
  }
  r.context11->Begin(r.copy_disjoint[i].Get());
  r.context11->End(r.copy_begin[i].Get());
  r.copy_timed[i] = true;
  return i;
}

void EndCopyTiming(Remote& r, UINT i) {
  if (i >= Remote::COPY_TIMES) return;
  r.context11->End(r.copy_end[i].Get());
  r.context11->End(r.copy_disjoint[i].Get());
}

void Submit(ID3D11Texture2D* back_buffer, const FrameInputs& inputs) {
  if (!Running()) return;
  Remote& r = *g_r;
  // The game frame's Streamline index: from FrameStart, or (first frame, or the helper was not taking messages then) a token without a sleep.
  uint32_t index = r.game_index;
  LONGLONG start = r.game_start;
  r.game_index = 0;
  r.game_start = 0;
  if (index == 0) {
    index = ++g_frame_counter;
    if (!Send(r, link::MSG_FRAME, index)) index = 0;
    start = Qpc();
  }
  if (index != 0) Send(r, link::MSG_SUBMIT_START, index);
  struct SubmitEnd {
    Remote& r;
    uint32_t index;
    ~SubmitEnd() {
      if (index != 0) Send(r, link::MSG_SUBMIT_END, index);
    }
  } submit_end{r, index};
  D3D11_TEXTURE2D_DESC desc{};
  back_buffer->GetDesc(&desc);
  if (desc.Width != r.width || desc.Height != r.height || desc.Format != r.format || desc.SampleDesc.Count != 1) return;
  const UINT64 id = r.next_id;
  r.reset_pending |= inputs.reset;
  // The helper shows the newest frame it has and drops the rest, so a game running ahead of it renders frames nobody sees (FFXV at 4K: about
  // half). Instead the game waits here until the helper has taken the frame before this one: it runs at the helper's pace, and every frame it
  // renders is shown. A helper that keeps up has taken it already, so no wait. On the CPU, never on the GPU, and never for long: past
  // PACE_WAIT_MS the frame goes on as before (a slot if there is one, a skip if not).
  // Only once the helper takes frames: while it starts (its first Present can take seconds) every wait would run out, and the game would crawl.
  // A wait that runs out pauses pacing for PACE_PAUSE_MS.
  constexpr DWORD PACE_WAIT_MS = 50, PACE_PAUSE_MS = 1000;
  // Of each wait, EARLY_LEFT_MS stays here and the rest moves to the frame's start, EARLY_STEP of the difference each frame.
  constexpr double EARLY_LEFT_MS = 0.5, EARLY_STEP = 0.2, EARLY_MAX_MS = 40.0;
  const UINT64 taken = r.consumed->GetCompletedValue();
  if (taken > r.pace_taken) r.pace_taken = taken;
  LARGE_INTEGER freq;
  QueryPerformanceFrequency(&freq);
  const LONGLONG now = Qpc();
  bool pacing = r.pace_taken != 0 && now >= r.pace_resume;
  double waited_ms = 0;
  if (pacing && id > 1 && taken < id - 1) {
    if (r.pace_event == nullptr) r.pace_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (r.pace_event != nullptr && SUCCEEDED(r.consumed->SetEventOnCompletion(id - 1, r.pace_event))) {
      const bool ran_out = WaitForSingleObject(r.pace_event, PACE_WAIT_MS) == WAIT_TIMEOUT;
      const LONGLONG waited = Qpc() - now;
      ++g_paced;
      g_paced_ticks += waited;
      waited_ms = static_cast<double>(waited) * 1000.0 / static_cast<double>(freq.QuadPart);
      if (ran_out) {
        r.pace_resume = Qpc() + freq.QuadPart * PACE_PAUSE_MS / 1000;
        pacing = false;
      }
    }
  }
  r.early_ms = pacing ? std::clamp(r.early_ms + EARLY_STEP * (waited_ms - EARLY_LEFT_MS), 0.0, EARLY_MAX_MS) : 0.0;
  if (id > RING && r.consumed->GetCompletedValue() < id - RING) {
    ++g_skipped;
    return;
  }
  ++r.next_id;
  const UINT slot = static_cast<UINT>(id % RING);
  InterlockedExchange64(&r.b->slot_index[slot], static_cast<LONG64>(index));
  InterlockedExchange64(&r.b->slot_start[slot], static_cast<LONG64>(start));
  const UINT timing = BeginCopyTiming(r);
  r.context11->CopyResource(r.slots[slot].Get(), back_buffer);
  r.b->slot_reset[slot] = r.reset_pending ? 1 : 0;
  r.reset_pending = false;
  r.b->slot_compose[slot] = (inputs.compose ? link::FIX_COMPOSE : 0) | (inputs.checks ? link::FIX_CHECKS : 0) | (inputs.camera_motion ? link::FIX_CAMERA : 0) | (inputs.camera_fit ? link::FIX_FIT : 0) | (inputs.camera_record ? link::FIX_RECORD : 0) |
                            (inputs.nvof ? link::FIX_NVOF | (static_cast<int32_t>(std::min<uint32_t>(inputs.nvof_size, link::NVOF_SIZE_MASK)) << link::NVOF_SIZE_SHIFT) : 0);
  r.b->camera_count[slot] = 0;
  if (inputs.camera_motion && inputs.camera != nullptr) {
    const int count = std::clamp(inputs.camera_count, 0, hsf::camera::CANDIDATES);
    std::memcpy(r.b->camera[slot], inputs.camera, static_cast<size_t>(count) * sizeof(hsf::camera::Candidate));
    r.b->camera_count[slot] = count;
    r.b->camera_frame[slot] = inputs.camera_frame;
  }
  InterlockedExchange64(&r.b->motion_frame[slot], 0);
  InterlockedExchange64(&r.b->depth_frame[slot], 0);
  InterlockedExchange64(&r.b->hudless_frame[slot], 0);
  if (inputs.hudless != nullptr && MakeHudlessSlots(r, inputs.hudless) && r.hudless.Copy(r.context11.Get(), slot, inputs.hudless))
    InterlockedExchange64(&r.b->hudless_frame[slot], static_cast<LONG64>(id));
  if (inputs.depth != nullptr && MakeDepthSlots(r, inputs.depth) && r.depth.Copy(r.context11.Get(), slot, inputs.depth)) {
    r.b->depth_inverted[slot] = inputs.depth_inverted ? 1 : 0;
    InterlockedExchange64(&r.b->depth_frame[slot], static_cast<LONG64>(id));
  }
  if (inputs.motion != nullptr && (inputs.motion_scale[0] != 0.f || inputs.motion_scale[1] != 0.f) && MakeMotionSlots(r, inputs.motion) &&
      r.motion.Copy(r.context11.Get(), slot, inputs.motion)) {
    r.b->motion_scale[slot][0] = inputs.motion_scale[0];
    r.b->motion_scale[slot][1] = inputs.motion_scale[1];
    InterlockedExchange64(&r.b->motion_frame[slot], static_cast<LONG64>(id));
  }
  EndCopyTiming(r, timing);
  r.context11->Signal(r.ready.Get(), id);
  r.context11->Flush();
}

bool Feeding() { return g_on && Running(); }

bool MotionSize(UINT* width, UINT* height) {
  if (!g_r || g_r->motion.Width() == 0) return false;
  *width = g_r->motion.Width();
  *height = g_r->motion.Height();
  return true;
}

void SetOptions(bool on, uint32_t frames) {
  // Back on after a spell of the game showing its own frames: DLSS-G must not blend across the gap.
  if (on && !g_on && g_r) g_r->reset_pending = true;
  g_on = on;
  g_frames = std::max(1u, frames);
  if (g_r) {
    g_r->b->on = on ? 1 : 0;
    g_r->b->frames = static_cast<int32_t>(g_frames);
  }
}

void SetOverlayWindow(bool on) { g_overlay_window = on; }

void SetColorSpace(uint32_t dxgi_color_space) { g_color_space = dxgi_color_space; }

void SetMenuOpen(bool open) {
  if (g_r) g_r->b->menu_open = open ? 1 : 0;
}

void SetReflex(uint32_t fps_cap, bool boost) {
  g_cap_fps = fps_cap;
  g_boost = boost;
  if (g_r) {
    g_r->b->fps_cap = static_cast<int32_t>(fps_cap);
    g_r->b->boost = boost ? 1 : 0;
  }
}

void SetCameraHint(int64_t key) {
  g_camera_hint = key;
  if (g_r) g_r->b->camera_hint = key;
}

void SetReflexSleep(bool on) {
  g_sleep_on = on;
  if (g_r) g_r->b->sleep_on = on ? 1 : 0;
}

size_t LatencyHistory(float* out, size_t max) {
  if (!g_r) return 0;
  const link::Block* const b = g_r->b;
  const uint32_t write = static_cast<uint32_t>(b->latency_write);
  const size_t count = std::min<size_t>({max, write, link::LATENCY_HISTORY});
  for (size_t i = 0; i < count; ++i) out[i] = static_cast<float>(b->latency_us[(write - count + i) % link::LATENCY_HISTORY]) / 1000.f;
  return count;
}

Stats TakeStats() {
  static LONGLONG since = 0;
  static LARGE_INTEGER freq{};
  if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  const double seconds = since != 0 ? static_cast<double>(now.QuadPart - since) / static_cast<double>(freq.QuadPart) : 0.0;
  since = now.QuadPart;
  Stats stats;
  const bool running = Running();
  uint32_t real = 0, shown = 0;
  stats.skipped = g_skipped;
  g_skipped = 0;
  stats.copy_ms = g_copy_ms_count != 0 ? static_cast<float>(g_copy_ms_sum / g_copy_ms_count) : 0.f;
  g_copy_ms_sum = 0;
  g_copy_ms_count = 0;
  stats.paced = g_paced;
  stats.paced_ms = g_paced != 0 ? static_cast<float>(static_cast<double>(g_paced_ticks) * 1000.0 / static_cast<double>(freq.QuadPart) / g_paced) : 0.f;
  g_paced = 0;
  g_paced_ticks = 0;
  stats.early = g_early;
  stats.early_ms = g_early != 0 ? static_cast<float>(static_cast<double>(g_early_ticks) * 1000.0 / static_cast<double>(freq.QuadPart) / g_early) : 0.f;
  g_early = 0;
  g_early_ticks = 0;
  if (running) {
    link::Block* const b = g_r->b;
    const auto take = [](volatile int32_t& counter) { return static_cast<uint32_t>(InterlockedExchange(reinterpret_cast<volatile LONG*>(&counter), 0)); };
    real = take(b->real);
    shown = take(b->shown);
    stats.skipped += take(b->skipped);
    stats.status = take(b->status);
    stats.generating = take(b->generated_seen) != 0;
    if (b->max_frames > 0) g_max_frames = static_cast<uint32_t>(b->max_frames);
    stats.nvof_state = b->nvof_state;
    stats.foreign_dxgi = b->foreign_dxgi != 0;
    const uint32_t sleeps = take(b->sleeps), sleep_us = take(b->sleep_us);
    stats.reflex_game_paced = sleeps != 0;
    stats.reflex_sleep_ms = sleeps != 0 ? static_cast<float>(sleep_us) / 1000.f / static_cast<float>(sleeps) : 0.f;
    stats.reflex_available = b->reflex_available != 0;
    stats.camera_source = static_cast<hsf::camera::Source>(b->camera_source);
    stats.camera_share = b->camera_share;
    stats.camera_locked = b->camera_locked;
    const uint32_t latencies = take(b->latency_count), latency_us = take(b->latency_sum_us);
    stats.latency_max_ms = static_cast<float>(take(b->latency_max_us)) / 1000.f;
    stats.latency_ms = latencies != 0 ? static_cast<float>(latency_us) / 1000.f / static_cast<float>(latencies) : 0.f;
    // The helper's summary: taken once (frames back to 0) so an old one is not shown again.
    stats.report_frames = take(b->report_frames);
    if (stats.report_frames != 0) {
      stats.report_total_ms = b->report_total_us / 1000.f;
      stats.report_game_ms = b->report_game_us / 1000.f;
      stats.report_submit_ms = b->report_submit_us / 1000.f;
      stats.report_handoff_ms = b->report_handoff_us / 1000.f;
      stats.report_present_ms = b->report_present_us / 1000.f;
      stats.report_gpu_ms = b->report_gpu_us / 1000.f;
    }
    const int32_t note = b->note;
    stats.gpu_hang = note == link::NOTE_GPU_HANG;
    stats.note = b->state == link::STATE_STARTING ? "starting the 64-bit DLSS-G helper"
                 : note >= 0 && note < static_cast<int32_t>(std::size(link::NOTE_TEXT)) ? link::NOTE_TEXT[note]
                                                                                       : "";
  } else {
    stats.note = g_failed;
  }
  stats.max_frames = g_max_frames;
  stats.reflex_sleep_on = g_sleep_on;
  if (seconds > 0) {
    stats.real_fps = static_cast<float>(real / seconds);
    stats.shown_fps = static_cast<float>(shown / seconds);
  }
  return stats;
}

}  // namespace hsf::dlssg
