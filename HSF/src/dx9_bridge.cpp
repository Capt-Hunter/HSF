// See dx9_bridge.h.

#include "dx9_bridge.h"

#include "gpu_fence9.h"

#include <dxgi1_2.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace hsf::dx9 {
namespace {

// GPU route: D3D9 writes slot N % 3. With the GPU fences D3D11 reads that same slot in the same frame, and D3D9 comes back to it three frames
// later. Without them D3D11 reads slot (N - 1) % 3, and the third is still being read from two frames ago.
constexpr int GPU_RING = 3;
// CPU route: frame N is copied into slot N % 3 and its read-back to system memory is queued right away (GetRenderTargetData, with an event
// query after it); it is taken across at frame N + 2, when the GPU has long finished it, so the game thread does not wait for the GPU.
constexpr int CPU_RING = 3;
constexpr int CPU_LAG = 2;

LONGLONG Now() {
  LARGE_INTEGER t;
  QueryPerformanceCounter(&t);
  return t.QuadPart;
}
double Ms(LONGLONG ticks) {
  static LARGE_INTEGER freq{};
  if (freq.QuadPart == 0) QueryPerformanceFrequency(&freq);
  return static_cast<double>(ticks) * 1000.0 / static_cast<double>(freq.QuadPart);
}

char g_problem[200] = "";
void Problem(const char* format, const char* name, unsigned value) {
  std::snprintf(g_problem, sizeof(g_problem), format, name, value);
}

// X8 formats go as A8: DXGI's B8G8R8X8 is a different copy group from B8G8R8A8, and the helper's swap chain wants an A8 format.
D3DFORMAT CopyFormat(D3DFORMAT format) {
  if (format == D3DFMT_X8R8G8B8) return D3DFMT_A8R8G8B8;
  if (format == D3DFMT_X8B8G8R8) return D3DFMT_A8B8G8R8;
  return format;
}

// ---- Copying rows from D3D9's system memory copy into D3D11's mapped upload texture, split over the Windows thread pool ----
// A 2560x1440 frame is 14 MB; one core copies that in about 1.5 ms, four in well under half that. The game thread takes chunks too, and waits
// for the pool's before it returns, so no callback is left running when the add-on is unloaded.
struct CopyJob {
  BYTE* dst = nullptr;
  const BYTE* src = nullptr;
  UINT dst_pitch = 0, src_pitch = 0, row_bytes = 0, rows = 0;
  int chunks = 0;
  std::atomic<int> next{0};
};
CopyJob g_job;
PTP_WORK g_work = nullptr;
int g_helpers = -1;

void RunChunks() {
  for (;;) {
    const int c = g_job.next.fetch_add(1);
    if (c >= g_job.chunks) return;
    const UINT first = static_cast<UINT>(static_cast<uint64_t>(g_job.rows) * c / g_job.chunks);
    const UINT last = static_cast<UINT>(static_cast<uint64_t>(g_job.rows) * (c + 1) / g_job.chunks);
    if (g_job.dst_pitch == g_job.src_pitch) {
      std::memcpy(g_job.dst + static_cast<size_t>(first) * g_job.dst_pitch, g_job.src + static_cast<size_t>(first) * g_job.src_pitch,
                  static_cast<size_t>(last - first) * g_job.dst_pitch);
    } else {
      for (UINT y = first; y < last; ++y)
        std::memcpy(g_job.dst + static_cast<size_t>(y) * g_job.dst_pitch, g_job.src + static_cast<size_t>(y) * g_job.src_pitch, g_job.row_bytes);
    }
  }
}

VOID CALLBACK CopyWork(PTP_CALLBACK_INSTANCE, PVOID, PTP_WORK) { RunChunks(); }

void CopyRows(BYTE* dst, UINT dst_pitch, const BYTE* src, UINT src_pitch, UINT row_bytes, UINT rows) {
  if (g_helpers < 0) {
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    g_helpers = static_cast<int>(std::clamp<DWORD>(info.dwNumberOfProcessors, 1, 4)) - 1;
  }
  const size_t bytes = static_cast<size_t>(row_bytes) * rows;
  const int helpers = bytes >= (1u << 20) ? g_helpers : 0;
  g_job.dst = dst;
  g_job.src = src;
  g_job.dst_pitch = dst_pitch;
  g_job.src_pitch = src_pitch;
  g_job.row_bytes = row_bytes;
  g_job.rows = rows;
  g_job.chunks = helpers > 0 ? (helpers + 1) * 4 : 1;
  g_job.next.store(0);
  if (helpers > 0 && g_work == nullptr) g_work = CreateThreadpoolWork(CopyWork, nullptr, nullptr);
  const int submitted = helpers > 0 && g_work != nullptr ? helpers : 0;
  for (int i = 0; i < submitted; ++i) SubmitThreadpoolWork(g_work);
  RunChunks();
  if (submitted > 0) WaitForThreadpoolWorkCallbacks(g_work, FALSE);
}

UINT BytesPerTexel(D3DFORMAT format) {
  switch (format) {
    case D3DFMT_A16B16G16R16:
    case D3DFMT_A16B16G16R16F:
    case D3DFMT_G32R32F: return 8;
    case D3DFMT_A32B32G32R32F: return 16;
    case D3DFMT_R16F: return 2;
    default: return 4;
  }
}

// One input (the frame, the flow or the depth): HSF's D3D9 copies on the game's device and the D3D11 texture it comes out in.
struct Lane {
  const char* name = "";
  bool may_half = false;     // flow and depth: copied at half width and height when HSF.ini Dx9HalfInputs=1 (DLSS-G takes them smaller than the frame)
  bool half = false;         // this lane's copies are half size
  UINT source_width = 0, source_height = 0;
  UINT width = 0, height = 0;  // HSF's copies
  D3DFORMAT source_format = D3DFMT_UNKNOWN, copy_format = D3DFMT_UNKNOWN;
  DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
  bool refused = false;     // failed for this size and format: not tried again until they change
  bool made = false;
  int64_t last_frame = -1;  // GPU route: the frame last copied into HSF's D3D9 texture, -1 = none waiting
  double ms = 0;            // game thread time in this lane since the last TakeStats
  // GPU route.
  ComPtr<IDirect3DTexture9> shared9[GPU_RING];
  ComPtr<IDirect3DSurface9> shared9_surface[GPU_RING];
  ComPtr<IDirect3DQuery9> written[GPU_RING];  // D3D9 finished the copy into slot i
  ComPtr<ID3D11Texture2D> shared11[GPU_RING];
  // CPU route.
  ComPtr<IDirect3DSurface9> target[CPU_RING];
  ComPtr<IDirect3DSurface9> system[CPU_RING];
  ComPtr<IDirect3DQuery9> copied[CPU_RING];  // the read-back of slot i into system memory is done
  int64_t slot_frame[CPU_RING] = {-1, -1, -1};
  ComPtr<ID3D11Texture2D> upload;  // D3D11 dynamic texture the CPU writes into; copied on the GPU into `out`
  ComPtr<ID3D11Texture2D> out;

  void ReleaseGame() {
    for (int i = 0; i < GPU_RING; ++i) {
      shared11[i].Reset();
      shared9_surface[i].Reset();
      shared9[i].Reset();
      written[i].Reset();
    }
    for (int i = 0; i < CPU_RING; ++i) {
      target[i].Reset();
      system[i].Reset();
      copied[i].Reset();
      slot_frame[i] = -1;
    }
    made = false;
    last_frame = -1;
  }
  void ReleaseAll() {
    ReleaseGame();
    upload.Reset();
    out.Reset();
    source_width = source_height = width = height = 0;
    half = false;
    source_format = copy_format = D3DFMT_UNKNOWN;
    dxgi = DXGI_FORMAT_UNKNOWN;
    refused = false;
  }
};

struct Bridge9 {
  IDirect3DDevice9* game = nullptr;  // not referenced: ReShade keeps it alive while its effect runtime is there
  Route route = Route::none;
  ComPtr<ID3D11Device> device;
  ComPtr<ID3D11DeviceContext> context;
  ComPtr<ID3D11Query> read_done[GPU_RING];  // GPU route: D3D11 finished everything up to the start of frame i's Bridge
  // GPU route with GPU fences (gpu_fence9.h): D3D11 takes this frame's copies in the same frame, and nobody waits on the CPU.
  bool fence_wanted = true;    // HSF.ini Dx9GpuFence
  bool fence = false;          // the fences are there
  bool fence_refused = false;  // they could not be made or failed: event queries until the next Reset of the device
  bool copied = false;         // a lane copied into its shared texture this frame: the game's queue must signal
  ComPtr<IDirect3DQuery9> flush9;  // ending and asking for it sends D3D9's waiting commands to the GPU
  int64_t frame = 0;
  int64_t slots_free_for = 0;  // GPU route: the frame whose slots were last waited free (BridgeHudless or Bridge, whichever came first)
  bool half = true;            // HSF.ini Dx9HalfInputs
  bool half_failed = false;    // the GPU would not copy at half size: full size from then on
  Lane color, motion, depth, hudless;
  ID3D11Texture2D* hudless_out = nullptr;  // BridgeHudless's result for the next Bridge
  bool hudless_due = false;
  // GPU route: the depth copy shrunk to DEPTH_GRID_W x DEPTH_GRID_H every 30th frame and read back on the CPU two frames later, without
  // waiting, for the direction finder and the debug view (the CPU route reads its full copy instead).
  ComPtr<IDirect3DSurface9> probe_target, probe_system;
  ComPtr<IDirect3DQuery9> probe_done;
  int64_t probe_frame = -1;  // the frame the read-back waiting in probe_system was queued at, -1 = none
  bool probe_refused = false;
};

Bridge9 g;
double g_ms_sum = 0, g_ms_worst = 0;
uint32_t g_frames = 0, g_failed = 0;

// The DXGI adapter the game's D3D9 device runs on: by LUID on D3D9Ex, otherwise by the monitor the D3D9 adapter drives; the first one if neither matches.
ComPtr<IDXGIAdapter1> FindAdapter(IDirect3DDevice9* game) {
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
  ComPtr<IDirect3D9> d3d;
  D3DDEVICE_CREATION_PARAMETERS params{};
  LUID luid{};
  bool have_luid = false;
  HMONITOR monitor = nullptr;
  if (SUCCEEDED(game->GetDirect3D(&d3d)) && SUCCEEDED(game->GetCreationParameters(&params))) {
    ComPtr<IDirect3D9Ex> ex;
    if (SUCCEEDED(d3d.As(&ex))) have_luid = SUCCEEDED(ex->GetAdapterLUID(params.AdapterOrdinal, &luid));
    monitor = d3d->GetAdapterMonitor(params.AdapterOrdinal);
  }
  ComPtr<IDXGIAdapter1> adapter, first;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    if (first == nullptr) first = adapter;
    DXGI_ADAPTER_DESC1 desc{};
    adapter->GetDesc1(&desc);
    if (have_luid && desc.AdapterLuid.LowPart == luid.LowPart && desc.AdapterLuid.HighPart == luid.HighPart) return adapter;
    if (!have_luid && monitor != nullptr) {
      ComPtr<IDXGIOutput> output;
      for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
        DXGI_OUTPUT_DESC out_desc{};
        if (SUCCEEDED(output->GetDesc(&out_desc)) && out_desc.Monitor == monitor) return adapter;
      }
    }
  }
  return first;
}

bool Make(Lane& lane) {
  HRESULT hr = S_OK;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = lane.width;
  desc.Height = lane.height;
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = lane.dxgi;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DEFAULT;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  if (g.route == Route::gpu) {
    for (int i = 0; i < GPU_RING; ++i) {
      HANDLE handle = nullptr;
      hr = g.game->CreateTexture(lane.width, lane.height, 1, D3DUSAGE_RENDERTARGET, lane.copy_format, D3DPOOL_DEFAULT, &lane.shared9[i], &handle);
      if (FAILED(hr) || handle == nullptr) break;
      hr = lane.shared9[i]->GetSurfaceLevel(0, &lane.shared9_surface[i]);
      if (FAILED(hr)) break;
      hr = g.game->CreateQuery(D3DQUERYTYPE_EVENT, &lane.written[i]);
      if (FAILED(hr)) break;
      hr = g.device->OpenSharedResource(handle, IID_PPV_ARGS(&lane.shared11[i]));
      if (FAILED(hr)) break;
    }
  } else {
    for (int i = 0; i < CPU_RING && SUCCEEDED(hr); ++i) {
      hr = g.game->CreateRenderTarget(lane.width, lane.height, lane.copy_format, D3DMULTISAMPLE_NONE, 0, FALSE, &lane.target[i], nullptr);
      if (SUCCEEDED(hr)) hr = g.game->CreateOffscreenPlainSurface(lane.width, lane.height, lane.copy_format, D3DPOOL_SYSTEMMEM, &lane.system[i], nullptr);
      if (SUCCEEDED(hr)) hr = g.game->CreateQuery(D3DQUERYTYPE_EVENT, &lane.copied[i]);
    }
    if (SUCCEEDED(hr) && lane.out == nullptr) hr = g.device->CreateTexture2D(&desc, nullptr, &lane.out);
    if (SUCCEEDED(hr) && lane.upload == nullptr) {
      D3D11_TEXTURE2D_DESC up = desc;
      up.Usage = D3D11_USAGE_DYNAMIC;
      up.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
      hr = g.device->CreateTexture2D(&up, nullptr, &lane.upload);
    }
  }
  if (FAILED(hr)) {
    std::snprintf(g_problem, sizeof(g_problem), "%s: could not make HSF's copies (%ux%u D3D9 format %u, %s route): 0x%08lx", lane.name, lane.width,
                  lane.height, static_cast<unsigned>(lane.copy_format), ROUTE_NAMES[static_cast<int>(g.route)], static_cast<unsigned long>(hr));
    lane.ReleaseGame();
    return false;
  }
  lane.made = true;
  return true;
}

// Waits (spinning, at most `ms`) for a query; true when it is done.
template <typename Done>
bool Wait(Done done, double ms) {
  const LONGLONG start = Now();
  while (!done()) {
    if (Ms(Now() - start) > ms) return false;
    YieldProcessor();
  }
  return true;
}

void SampleDepth(const Lane& lane, const D3DLOCKED_RECT& locked);
void ProbeDepth(IDirect3DSurface9* copy, int64_t n);

char g_fence_text[220] = "";  // for the log: why the GPU fences are not used, or ""

void StopFence() {
  hsf::fence9::Stop();
  g.fence = false;
  g.copied = false;
  g.flush9.Reset();
}

// GPU route, once per device (and again after a Reset): the GPU fences, if this device and Windows can.
void StartFence() {
  if (g.fence || g.fence_refused || !g.fence_wanted) return;
  g.fence_refused = true;
  ComPtr<IDirect3DDevice9Ex> ex;
  char why[120] = "";
  if (FAILED(g.game->QueryInterface(IID_PPV_ARGS(&ex)))) return;
  if (!hsf::fence9::Start(ex.Get(), g.device.Get(), g.context.Get(), why, sizeof(why))) {
    std::snprintf(g_fence_text, sizeof(g_fence_text), "no GPU fences: %s", why);
    return;
  }
  if (FAILED(g.game->CreateQuery(D3DQUERYTYPE_EVENT, &g.flush9))) {
    std::snprintf(g_fence_text, sizeof(g_fence_text), "no GPU fences: could not make a D3D9 query");
    StopFence();
    return;
  }
  g.fence = true;
  g.fence_refused = false;
  g_fence_text[0] = '\0';
}

// A fence call failed mid-game: back to the event queries, every lane starts again.
void FenceFailed(const char* what) {
  std::snprintf(g_fence_text, sizeof(g_fence_text), "GPU fences stopped: %s", what);
  std::snprintf(g_problem, sizeof(g_problem), "%s", g_fence_text);
  ++g_failed;
  StopFence();
  g.fence_refused = true;
  g.color.last_frame = g.motion.last_frame = g.depth.last_frame = g.hudless.last_frame = -1;
}

// A half-size copy the GPU refused: flow and depth go at full size from now on (both, so they stay the same size, as DLSS-G needs).
void HalfFailed(const Lane& lane) {
  Problem("%s: the GPU would not copy at half size (D3D9 format %u); flow and depth go at full size", lane.name, static_cast<unsigned>(lane.copy_format));
  g.half_failed = true;
  g.motion.ReleaseAll();
  g.depth.ReleaseAll();
}

// GPU route: D3D9 is about to write this frame's slots; waits until D3D11 has finished reading them (two frames ago).
void FreeSlots(int64_t n) {
  if (g.route != Route::gpu || g.slots_free_for == n) return;
  g.slots_free_for = n;
  StartFence();
  // A fence signal that never came stops both GPU queues: seen within half a second, let go, and back to the event queries.
  if (g.fence) {
    if (const char* const stalled = hsf::fence9::Stalled()) FenceFailed(stalled);
  }
  if (g.fence) {
    // The game's GPU queue waits until D3D11 has finished everything it was given so far (it last read this frame's slots three frames ago).
    if (!hsf::fence9::BridgeDone()) FenceFailed("the game's queue could not wait for HSF's");
    return;
  }
  // This frame writes slot n % 3, which D3D11 last read at frame n - 2; the query ended at frame n - 1's start covers that read.
  if (n >= 2) {
    ID3D11DeviceContext* const context = g.context.Get();
    ID3D11Query* const q = g.read_done[(n - 1) % GPU_RING].Get();
    Wait([context, q] { return context->GetData(q, nullptr, 0, 0) == S_OK; }, 50.0);
  }
  g.context->End(g.read_done[n % GPU_RING].Get());
}

ID3D11Texture2D* StepGpu(Lane& lane, IDirect3DSurface9* source, int64_t n) {
  ID3D11Texture2D* previous = nullptr;
  const int w = static_cast<int>(n % GPU_RING), r = static_cast<int>((n - 1) % GPU_RING);
  if (FAILED(g.game->StretchRect(source, nullptr, lane.shared9_surface[w].Get(), nullptr, lane.half ? D3DTEXF_POINT : D3DTEXF_NONE))) {
    if (lane.half) {
      HalfFailed(lane);
      return nullptr;
    }
    Problem("%s: StretchRect into the shared copy failed (D3D9 format %u)", lane.name, static_cast<unsigned>(lane.copy_format));
    ++g_failed;
    lane.last_frame = -1;
    return nullptr;
  }
  if (&lane == &g.depth) ProbeDepth(lane.shared9_surface[w].Get(), n);
  if (g.fence) {
    // This frame's copy, right away: Bridge has the game's queue signal after the copies, and D3D11 waits for that on the GPU.
    g.copied = true;
    lane.last_frame = n;
    return lane.shared11[w].Get();
  }
  lane.written[w]->Issue(D3DISSUE_END);
  if (lane.last_frame == n - 1) {
    IDirect3DQuery9* const q = lane.written[r].Get();
    if (Wait([q] { return q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_OK; }, 50.0))
      previous = lane.shared11[r].Get();
    else
      ++g_failed;
  }
  lane.last_frame = n;
  return previous;
}

ID3D11Texture2D* StepCpu(Lane& lane, IDirect3DSurface9* source, int64_t n) {
  ID3D11Texture2D* previous = nullptr;
  // First take frame n - 2 across: its read-back was queued two frames ago and is done by now (the wait is only a guard).
  const int64_t want = n - CPU_LAG;
  if (want >= 0) {
    const int r = static_cast<int>(want % CPU_RING);
    if (lane.slot_frame[r] == want) {
      lane.slot_frame[r] = -1;
      IDirect3DQuery9* const q = lane.copied[r].Get();
      D3DLOCKED_RECT locked{};
      D3D11_MAPPED_SUBRESOURCE mapped{};
      if (Wait([q] { return q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_OK; }, 50.0) &&
          SUCCEEDED(lane.system[r]->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
        if (SUCCEEDED(g.context->Map(lane.upload.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
          CopyRows(static_cast<BYTE*>(mapped.pData), mapped.RowPitch, static_cast<const BYTE*>(locked.pBits), static_cast<UINT>(locked.Pitch),
                   lane.width * BytesPerTexel(lane.copy_format), lane.height);
          g.context->Unmap(lane.upload.Get(), 0);
          g.context->CopyResource(lane.out.Get(), lane.upload.Get());
          previous = lane.out.Get();
        } else {
          ++g_failed;
        }
        if (&lane == &g.depth) SampleDepth(lane, locked);
        lane.system[r]->UnlockRect();
      } else {
        ++g_failed;
      }
    }
  }
  // Then this frame: copy on the GPU, queue the read-back, mark it.
  const int w = static_cast<int>(n % CPU_RING);
  lane.slot_frame[w] = -1;
  if (FAILED(g.game->StretchRect(source, nullptr, lane.target[w].Get(), nullptr, lane.half ? D3DTEXF_POINT : D3DTEXF_NONE))) {
    if (lane.half) {
      HalfFailed(lane);
      return nullptr;
    }
    Problem("%s: StretchRect into HSF's copy failed (D3D9 format %u)", lane.name, static_cast<unsigned>(lane.copy_format));
    ++g_failed;
    return previous;
  }
  if (FAILED(g.game->GetRenderTargetData(lane.target[w].Get(), lane.system[w].Get()))) {
    Problem("%s: reading HSF's copy back failed (D3D9 format %u)", lane.name, static_cast<unsigned>(lane.copy_format));
    ++g_failed;
    return previous;
  }
  lane.copied[w]->Issue(D3DISSUE_END);
  lane.slot_frame[w] = n;
  return previous;
}

// One input, one frame: this frame's copy in, and out comes this frame's (GPU route with fences), the frame before's (GPU route without) or
// the one from two frames ago (CPU route); nullptr when there is none yet.
ID3D11Texture2D* Step(Lane& lane, IDirect3DSurface9* source, int64_t n) {
  if (source == nullptr) {
    lane.last_frame = -1;
    return nullptr;
  }
  D3DSURFACE_DESC desc{};
  if (FAILED(source->GetDesc(&desc))) return nullptr;
  const bool half = lane.may_half && g.half && !g.half_failed && desc.Width >= 2 && desc.Height >= 2;
  if (desc.Width != lane.source_width || desc.Height != lane.source_height || desc.Format != lane.source_format || half != lane.half) {
    lane.ReleaseAll();
    lane.source_width = desc.Width;
    lane.source_height = desc.Height;
    lane.half = half;
    lane.width = half ? desc.Width / 2 : desc.Width;
    lane.height = half ? desc.Height / 2 : desc.Height;
    lane.source_format = desc.Format;
    lane.copy_format = CopyFormat(desc.Format);
    lane.dxgi = ToDxgi(lane.copy_format);
    if (lane.dxgi == DXGI_FORMAT_UNKNOWN) {
      lane.refused = true;
      Problem("%s: D3D9 format %u has no DXGI match here", lane.name, static_cast<unsigned>(desc.Format));
    }
  }
  if (lane.refused) return nullptr;
  if (!lane.made && !Make(lane)) {
    if (lane.half) {
      HalfFailed(lane);
      return nullptr;
    }
    lane.refused = true;
    ++g_failed;
    return nullptr;
  }
  const LONGLONG start = Now();
  ID3D11Texture2D* const previous = g.route == Route::gpu ? StepGpu(lane, source, n) : StepCpu(lane, source, n);
  lane.ms += Ms(Now() - start);
  return previous;
}

// The depth copy read from the CPU route's locked copy on a grid of DEPTH_GRID_W x DEPTH_GRID_H points every 30th frame: its range for the log
// (almost none means ReShade handed over a flat buffer) and the grid itself for HSF's direction finder and debug view (DepthGrid).
float g_depth_min = 0, g_depth_max = 0;
uint32_t g_depth_checks = 0;
std::vector<float> g_depth_grid;
uint32_t g_depth_grid_serial = 0;
void SampleGrid(const D3DLOCKED_RECT& locked, UINT width, UINT height) {
  g_depth_grid.resize(DEPTH_GRID_W * DEPTH_GRID_H);
  for (UINT y = 0; y < DEPTH_GRID_H; ++y) {
    const UINT py = (y * 2 + 1) * height / (DEPTH_GRID_H * 2);
    const float* const row = reinterpret_cast<const float*>(static_cast<const BYTE*>(locked.pBits) + static_cast<size_t>(py) * locked.Pitch);
    for (UINT x = 0; x < DEPTH_GRID_W; ++x) {
      const float v = row[(x * 2 + 1) * width / (DEPTH_GRID_W * 2)];
      g_depth_grid[y * DEPTH_GRID_W + x] = v;
      if (g_depth_checks == 0 && x == 0 && y == 0) g_depth_min = g_depth_max = v;
      g_depth_min = std::min(g_depth_min, v);
      g_depth_max = std::max(g_depth_max, v);
    }
  }
  ++g_depth_checks;
  ++g_depth_grid_serial;
}
void SampleDepth(const Lane& lane, const D3DLOCKED_RECT& locked) {
  if (lane.copy_format != D3DFMT_R32F || g.frame % 30 != 0) return;
  SampleGrid(locked, lane.width, lane.height);
}

void ReleaseProbe() {
  g.probe_target.Reset();
  g.probe_system.Reset();
  g.probe_done.Reset();
  g.probe_frame = -1;
}

// GPU route, after the depth lane copied frame n into `copy`: takes a waiting read-back if the GPU has finished it, and every 30th frame
// queues a new one. Never waits.
void ProbeDepth(IDirect3DSurface9* copy, int64_t n) {
  if (g.depth.copy_format != D3DFMT_R32F || g.probe_refused) return;
  if (g.probe_target == nullptr) {
    HRESULT hr = g.game->CreateRenderTarget(DEPTH_GRID_W, DEPTH_GRID_H, D3DFMT_R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &g.probe_target, nullptr);
    if (SUCCEEDED(hr))
      hr = g.game->CreateOffscreenPlainSurface(DEPTH_GRID_W, DEPTH_GRID_H, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &g.probe_system, nullptr);
    if (SUCCEEDED(hr)) hr = g.game->CreateQuery(D3DQUERYTYPE_EVENT, &g.probe_done);
    if (FAILED(hr)) {
      Problem("%s: could not make the depth check's copies (0x%08x)", "depth", static_cast<unsigned>(hr));
      ReleaseProbe();
      g.probe_refused = true;
      return;
    }
  }
  if (g.probe_frame >= 0 && n - g.probe_frame >= 2 && g.probe_done->GetData(nullptr, 0, 0) == S_OK) {
    D3DLOCKED_RECT locked{};
    if (SUCCEEDED(g.probe_system->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
      SampleGrid(locked, DEPTH_GRID_W, DEPTH_GRID_H);
      g.probe_system->UnlockRect();
    }
    g.probe_frame = -1;
  }
  if (g.probe_frame >= 0 || n % 30 != 0) return;
  if (FAILED(g.game->StretchRect(copy, nullptr, g.probe_target.Get(), nullptr, D3DTEXF_POINT)) ||
      FAILED(g.game->GetRenderTargetData(g.probe_target.Get(), g.probe_system.Get()))) {
    Problem("%s: the GPU would not shrink the depth copy for the depth check (D3D9 format %u)", "depth", static_cast<unsigned>(D3DFMT_R32F));
    ReleaseProbe();
    g.probe_refused = true;
    return;
  }
  g.probe_done->Issue(D3DISSUE_END);
  g.probe_frame = n;
}

ComPtr<IDirect3DSurface9> Level0(IDirect3DTexture9* texture) {
  ComPtr<IDirect3DSurface9> surface;
  if (texture != nullptr) texture->GetSurfaceLevel(0, &surface);
  return surface;
}

char g_lanes_text[400] = "";

}  // namespace

DXGI_FORMAT ToDxgi(D3DFORMAT format) {
  switch (format) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8: return DXGI_FORMAT_B8G8R8A8_UNORM;  // X8 goes across as A8 (CopyFormat)
    case D3DFMT_A8B8G8R8:
    case D3DFMT_X8B8G8R8: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case D3DFMT_A2B10G10R10: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case D3DFMT_A16B16G16R16: return DXGI_FORMAT_R16G16B16A16_UNORM;
    case D3DFMT_A16B16G16R16F: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case D3DFMT_A32B32G32R32F: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case D3DFMT_G16R16F: return DXGI_FORMAT_R16G16_FLOAT;
    case D3DFMT_G32R32F: return DXGI_FORMAT_R32G32_FLOAT;
    case D3DFMT_G16R16: return DXGI_FORMAT_R16G16_UNORM;
    case D3DFMT_R32F: return DXGI_FORMAT_R32_FLOAT;
    case D3DFMT_R16F: return DXGI_FORMAT_R16_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

bool Start(IDirect3DDevice9* game, char* error, size_t error_size) {
  if (g.device != nullptr && g.game == game) return true;
  Stop();
  const auto fail = [&](const char* what, HRESULT hr) {
    std::snprintf(error, error_size, "DX9 bridge: %s (0x%08lx)", what, static_cast<unsigned long>(hr));
    Stop();
    return false;
  };
  const ComPtr<IDXGIAdapter1> adapter = FindAdapter(game);
  if (adapter == nullptr) return fail("no DXGI adapter", E_FAIL);
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
  const HRESULT hr = D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, levels, static_cast<UINT>(std::size(levels)), D3D11_SDK_VERSION,
                                       &g.device, nullptr, &g.context);
  if (FAILED(hr)) return fail("could not make HSF's Direct3D 11 device", hr);
  g.game = game;
  {
    // Only asks whether the device is D3D9Ex; the reference goes right away. Kept, it was still alive when ReShade checks the device at
    // game exit (before destroy_device reaches HSF), so ReShade warned "Reference count ... is inconsistent! Leaking resources".
    ComPtr<IDirect3DDevice9Ex> ex;
    g.route = SUCCEEDED(game->QueryInterface(IID_PPV_ARGS(&ex))) ? Route::gpu : Route::cpu;
  }
  if (g.route == Route::gpu) {
    D3D11_QUERY_DESC query{D3D11_QUERY_EVENT, 0};
    for (auto& q : g.read_done)
      if (FAILED(g.device->CreateQuery(&query, &q))) return fail("could not make a Direct3D 11 query", E_FAIL);
  }
  g.color.name = "frame";
  g.motion.name = "flow";
  g.motion.may_half = true;
  g.depth.name = "depth";
  g.depth.may_half = true;
  g.hudless.name = "frame without overlays";
  g.half_failed = false;
  g.fence_refused = false;
  g_fence_text[0] = '\0';
  error[0] = '\0';
  return true;
}

void Stop() {
  g.color.ReleaseAll();
  g.motion.ReleaseAll();
  g.depth.ReleaseAll();
  g.hudless.ReleaseAll();
  g.hudless_due = false;
  ReleaseProbe();
  g.probe_refused = false;
  StopFence();
  for (auto& q : g.read_done) q.Reset();
  g.context.Reset();
  g.device.Reset();
  g.game = nullptr;
  g.route = Route::none;
  g.frame = 0;
  g.slots_free_for = 0;
  if (g_work != nullptr) {
    CloseThreadpoolWork(g_work);  // no callback is running: CopyRows waits for them all
    g_work = nullptr;
  }
}

void ReleaseGameResources() {
  g.color.ReleaseGame();
  g.motion.ReleaseGame();
  g.depth.ReleaseGame();
  g.hudless.ReleaseGame();
  g.hudless_due = false;
  ReleaseProbe();
  // The fences hang on the device's kernel handles, which a Reset replaces: made again on the next Bridge.
  StopFence();
  g.fence_refused = false;
}

void SetHalfInputs(bool half) { g.half = half; }
void SetGpuFence(bool on) { g.fence_wanted = on; }
bool GpuFence() { return g.fence; }
void TestLoseFenceSignal() { hsf::fence9::TestLoseNextSignal(); }

ID3D11Device* Device() { return g.device.Get(); }
Route CurrentRoute() { return g.route; }

Frame Bridge(IDirect3DSurface9* back_buffer, IDirect3DTexture9* motion, IDirect3DTexture9* depth) {
  Frame f;
  if (g.device == nullptr) return f;
  const LONGLONG start = Now();
  const int64_t n = ++g.frame;
  FreeSlots(n);
  f.color = Step(g.color, back_buffer, n);
  f.motion = Step(g.motion, Level0(motion).Get(), n);
  f.depth = Step(g.depth, Level0(depth).Get(), n);
  if (f.motion != nullptr) {
    f.motion_width = g.motion.width;
    f.motion_height = g.motion.height;
  }
  if (g.hudless_due) f.hudless = g.hudless_out;
  else g.hudless.last_frame = -1;  // no copy this frame: the next one starts the lane again
  g.hudless_due = false;
  g.hudless_out = nullptr;
  if (g.fence && g.copied) {
    // Send D3D9's waiting commands (the copies) to the GPU, then have its queue signal behind them; D3D11 waits for that signal on the GPU
    // before anything it is given from here on, so it reads the finished copies and the game thread does not wait.
    g.flush9->Issue(D3DISSUE_END);
    g.flush9->GetData(nullptr, 0, D3DGETDATA_FLUSH);
    if (!hsf::fence9::GameDone()) {
      FenceFailed("the game's queue could not signal");
      f = Frame{};
    }
  }
  g.copied = false;
  const double ms = Ms(Now() - start);
  g_ms_sum += ms;
  g_ms_worst = std::max(g_ms_worst, ms);
  ++g_frames;
  return f;
}

void BridgeHudless(IDirect3DSurface9* back_buffer) {
  if (g.device == nullptr || back_buffer == nullptr) return;
  const LONGLONG start = Now();
  const int64_t n = g.frame + 1;  // the frame number the next Bridge will use
  FreeSlots(n);
  g.hudless_out = Step(g.hudless, back_buffer, n);
  g.hudless_due = true;
  g_ms_sum += Ms(Now() - start);
}

Stats TakeStats() {
  Stats s;
  s.frames = g_frames;
  s.bridge_ms = g_frames != 0 ? static_cast<float>(g_ms_sum / g_frames) : 0.f;
  s.worst_ms = static_cast<float>(g_ms_worst);
  s.failed = g_failed;
  s.problem = g_problem;
  s.depth_checks = g_depth_checks;
  s.depth_min = g_depth_min;
  s.depth_max = g_depth_max;
  const double frames = g_frames != 0 ? g_frames : 1;
  const int behind = g.route != Route::gpu ? CPU_LAG : g.fence ? 0 : 1;
  std::snprintf(g_lanes_text, sizeof(g_lanes_text), "frame %.2f, without overlays %.2f, flow %.2f (%ux%u), depth %.2f (%ux%u) ms; read %d frame%s behind%s%s",
                g.color.ms / frames, g.hudless.ms / frames, g.motion.ms / frames, g.motion.width, g.motion.height, g.depth.ms / frames, g.depth.width,
                g.depth.height, behind, behind == 1 ? "" : "s", g.fence ? (hsf::fence9::KernelContexts() > 1 ? " (GPU fences, several kernel contexts)" : " (GPU fences)") : g_fence_text[0] != '\0' ? "; " : "",
                g.fence ? "" : g_fence_text);
  s.lanes = g_lanes_text;
  g.color.ms = g.hudless.ms = g.motion.ms = g.depth.ms = 0;
  g_depth_checks = 0;
  g_ms_sum = g_ms_worst = 0;
  g_frames = g_failed = 0;
  return s;
}

uint32_t DepthGrid(const float** values) {
  *values = g_depth_grid.data();
  return g_depth_grid.empty() ? 0 : g_depth_grid_serial;
}

}  // namespace hsf::dx9
