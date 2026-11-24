#include "fp16_swap.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <atomic>
#include <iterator>

#include "hdr_out.h"

namespace hsf::fp16swap {

namespace {

using Microsoft::WRL::ComPtr;

using GetBufferFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, REFIID, void**);
using GetDescFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, DXGI_SWAP_CHAIN_DESC*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
using GetDesc1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*, DXGI_SWAP_CHAIN_DESC1*);
using CheckColorSpaceFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE, UINT*);
using SetColorSpace1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, DXGI_COLOR_SPACE_TYPE);
using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT, DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);

// IDXGISwapChain3's method table.
constexpr int SLOT_GET_BUFFER = 9, SLOT_GET_DESC = 12, SLOT_RESIZE_BUFFERS = 13, SLOT_GET_DESC1 = 18, SLOT_CHECK_COLOR_SPACE = 37, SLOT_SET_COLOR_SPACE1 = 38,
              SLOT_RESIZE_BUFFERS1 = 39;
constexpr DXGI_FORMAT GAME_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT REAL_FORMAT = DXGI_FORMAT_R10G10B10A2_UNORM;
constexpr UINT RING = 3;  // conversions in flight

// A method table HSF wrote into, with what was there before. The game's swap chain and Streamline's can be of two kinds (ReShade's and Streamline's).
// A kind can have more than one table: ReLimiter's VSync control puts a copy of Streamline's table on the object (verbatim from index -1, the
// class locator), a new one after a resize; BLACKTAIL went black when HSF had hooked only the first copy and the game took its back buffers
// through the second. HSF hooks the class table as well, and a table it never wrote into is matched to one it did by that locator.
struct Table {
  void** table = nullptr;
  GetBufferFn get_buffer = nullptr;
  GetDescFn get_desc = nullptr;
  ResizeBuffersFn resize = nullptr;
  GetDesc1Fn get_desc1 = nullptr;
  CheckColorSpaceFn check_space = nullptr;
  SetColorSpace1Fn set_space = nullptr;
  ResizeBuffers1Fn resize1 = nullptr;
  void* locator = nullptr;  // index -1: the same in a verbatim copy
};
Table g_tables[6];

LineFn g_line = nullptr;
SRWLOCK g_lock = SRWLOCK_INIT;
std::atomic<bool> g_want{false};    // the swap chain DXGI made is R10G10B10A2 in place of the game's FP16
std::atomic<bool> g_active{false};  // and HSF converts it
std::atomic<void*> g_game{nullptr}, g_made{nullptr};
// DXGI's own swap chain under Streamline's: the one DLSS-G presents on and Windows shows. Not held.
std::atomic<void*> g_native{nullptr};
// The colour space goes on again at the next Present (after a start or a resize) and then every SPACE_EVERY presents. Rise of the Tomb Raider
// with RenoDX showed HSF's HDR10 frames as SDR although the set at the start returned S_OK: nothing in the game sets a colour space later, so
// that one set had to hold through every layer. Setting it again at Present, also straight on DXGI's swap chain, does not depend on that.
std::atomic<bool> g_space_due{false};
constexpr UINT64 SPACE_EVERY = 300;
UINT64 g_presents = 0;
HRESULT g_space_logged[2] = {S_FALSE, S_FALSE};  // what the last logged push got from Streamline's swap chain and DXGI's
ComPtr<ID3D12Device> g_game_device;  // as the game sees it (ReShade's, under ReShade): the FP16 textures are made through it, as if the game had made them
ComPtr<ID3D12Device> g_device;       // under any wrapper: HSF's own work
ComPtr<ID3D12CommandQueue> g_queue;  // the game's queue under any wrapper, so the conversion lands after the game's last draw
ComPtr<ID3D12Resource> g_proxy[DXGI_MAX_SWAP_CHAIN_BUFFERS];
UINT g_count = 0, g_width = 0, g_height = 0;
ComPtr<ID3D12Resource> g_mid;  // the converted frame, copied into the back buffer (back buffers take no shader writes)
hdr_out::Pass g_pass;
bool g_pass_ready = false;
ComPtr<ID3D12CommandAllocator> g_alloc[RING];
ComPtr<ID3D12GraphicsCommandList> g_list;
ComPtr<ID3D12Fence> g_fence;
HANDLE g_event = nullptr;
UINT64 g_value = 0, g_alloc_value[RING] = {};
UINT64 g_frame = 0;
bool g_pq = false;
DXGI_COLOR_SPACE_TYPE g_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
bool g_failed_logged = false;
// A resize is decided at Streamline's swap chain only: the wrapper above it (ReShade) passes it down, and on the way RenoDX may turn the
// format the game asked for (HDR10 in BLACKTAIL) into FP16. Deciding at the game's would see the format before that.
bool Decides(void* object) { return object == g_made.load(); }

template <typename... Args>
void Line(const char* format, Args... args) {
  if (g_line != nullptr) g_line(format, args...);
}

bool WriteSlot(void** table, int slot, void* value) {
  DWORD old = 0;
  if (!VirtualProtect(&table[slot], sizeof(void*), PAGE_READWRITE, &old)) return false;
  table[slot] = value;
  VirtualProtect(&table[slot], sizeof(void*), old, &old);
  return true;
}

void* Locator(void** table) {
  void* locator = nullptr;
  if (table == nullptr || !ReadProcessMemory(GetCurrentProcess(), table - 1, &locator, sizeof(locator), nullptr)) return nullptr;
  return locator;
}

const Table* For(void* object) {
  void** const table = *static_cast<void***>(object);
  for (const Table& t : g_tables) {
    if (t.table == table) return &t;
  }
  void* const locator = Locator(table);
  if (locator == nullptr) return nullptr;
  for (const Table& t : g_tables) {
    if (t.table != nullptr && t.locator == locator) return &t;
  }
  return nullptr;
}

bool Ours(void* object) { return object == g_game.load() || object == g_made.load(); }

void WaitIdle() {
  if (!g_fence || !g_queue) return;
  g_queue->Signal(g_fence.Get(), ++g_value);
  if (g_fence->GetCompletedValue() < g_value) {
    g_fence->SetEventOnCompletion(g_value, g_event);
    WaitForSingleObject(g_event, 2000);
  }
}

// Under g_lock. Before a resize, or when the swap chain goes back to a format DLSS-G takes.
void DropTextures() {
  WaitIdle();
  for (auto& p : g_proxy) p.Reset();
  g_mid.Reset();
  g_pass_ready = false;
  g_count = g_width = g_height = 0;
}

// Streamline's swap chain: its back buffers are what DLSS-G takes.
ComPtr<IDXGISwapChain3> Made() {
  ComPtr<IDXGISwapChain3> swap;
  void* const made = g_made.load();
  if (made != nullptr) static_cast<IUnknown*>(made)->QueryInterface(IID_PPV_ARGS(&swap));
  return swap;
}

// Streamline's back buffer, past HSF's own GetBuffer (with no wrapper, the game holds Streamline's swap chain itself).
HRESULT MadeBuffer(UINT index, ID3D12Resource** out) {
  void* const made = g_made.load();
  const Table* const t = made != nullptr ? For(made) : nullptr;
  if (t == nullptr) return E_FAIL;
  return t->get_buffer(static_cast<IDXGISwapChain*>(made), index, IID_PPV_ARGS(out));
}

// HDR10 when the display the window is on is in HDR mode, as RenoDX checks it (src/utils/swapchain.hpp); sRGB otherwise. Set through the game's
// swap chain, so a wrapper over Streamline (ReShade) knows it too.
void ApplySpace() {
  ComPtr<IDXGISwapChain3> native = Made();
  if (!native) return;
  ComPtr<IDXGIOutput> output;
  ComPtr<IDXGIOutput6> output6;
  DXGI_OUTPUT_DESC1 out{};
  g_pq = SUCCEEDED(native->GetContainingOutput(&output)) && SUCCEEDED(output.As(&output6)) && SUCCEEDED(output6->GetDesc1(&out)) &&
         (out.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 || out.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709);
  g_space = g_pq ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
  void* const game = g_game.load();
  const Table* const t = game != nullptr ? For(game) : nullptr;
  HRESULT hr = E_FAIL;
  if (t != nullptr && t->set_space != nullptr) hr = t->set_space(static_cast<IDXGISwapChain3*>(game), g_space);
  else hr = native->SetColorSpace1(g_space);
  Line("FP16 (scRGB) swap chain: DLSS-G gets %s frames (display %s), colour space set: 0x%08lx", g_pq ? "HDR10" : "sRGB", g_pq ? "in HDR mode" : "in SDR mode", hr);
  g_space_due = true;
}

// At Present: HSF's colour space on Streamline's swap chain (its hook passes it down) and straight on DXGI's. Logged the first time after a start
// or a resize, and whenever a result changes.
void PushSpace(bool due) {
  HRESULT made_hr = E_NOINTERFACE, native_hr = E_NOINTERFACE;
  if (ComPtr<IDXGISwapChain3> made = Made()) made_hr = made->SetColorSpace1(g_space);
  ComPtr<IDXGISwapChain3> native;
  IUnknown* const raw = static_cast<IUnknown*>(g_native.load());
  if (raw != nullptr && raw != g_made.load() && SUCCEEDED(raw->QueryInterface(IID_PPV_ARGS(&native)))) native_hr = native->SetColorSpace1(g_space);
  if (!due && made_hr == g_space_logged[0] && native_hr == g_space_logged[1]) return;
  g_space_logged[0] = made_hr;
  g_space_logged[1] = native_hr;
  Line("FP16: colour space %u set again at Present %llu: on Streamline's swap chain 0x%08lx, on DXGI's under it 0x%08lx%s", static_cast<unsigned>(g_space),
       static_cast<unsigned long long>(g_presents), made_hr, native_hr, native_hr == E_NOINTERFACE ? " (not known: no separate one)" : "");
}

// Under g_lock. The game's FP16 back buffers, at the real ones' size and count.
bool MakeTextures() {
  if (g_count != 0) return true;
  ComPtr<IDXGISwapChain3> made = Made();
  DXGI_SWAP_CHAIN_DESC1 desc{};
  if (!made || FAILED(made->GetDesc1(&desc)) || !g_game_device) return false;
  D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  D3D12_RESOURCE_DESC tex{};
  tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  tex.Width = desc.Width;
  tex.Height = desc.Height;
  tex.DepthOrArraySize = 1;
  tex.MipLevels = 1;
  tex.Format = GAME_FORMAT;
  tex.SampleDesc.Count = 1;
  tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  const UINT count = desc.BufferCount < DXGI_MAX_SWAP_CHAIN_BUFFERS ? desc.BufferCount : DXGI_MAX_SWAP_CHAIN_BUFFERS;
  for (UINT i = 0; i < count; ++i) {
    // COMMON is PRESENT: where a back buffer starts.
    const HRESULT hr = g_game_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&g_proxy[i]));
    if (FAILED(hr)) {
      Line("FP16 back buffer %u of %u (%ux%u): 0x%08lx", i, count, desc.Width, desc.Height, hr);
      for (auto& p : g_proxy) p.Reset();
      return false;
    }
  }
  g_count = count;
  g_width = desc.Width;
  g_height = desc.Height;
  ComPtr<ID3D12Resource> first;
  const DXGI_FORMAT real = SUCCEEDED(MadeBuffer(0, &first)) ? first->GetDesc().Format : DXGI_FORMAT_UNKNOWN;
  Line("FP16 back buffers for the game: %u at %ux%u; Streamline's are format %u", count, desc.Width, desc.Height, static_cast<unsigned>(real));
  return true;
}

// Under g_lock.
bool MakeConverter() {
  if (g_pass_ready) return true;
  if (!g_device) return false;
  HRESULT hr = S_OK;
  const char* what = "the command list";
  if (!g_list) {
    for (auto& a : g_alloc) {
      if (FAILED(hr = g_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)))) break;
    }
    if (SUCCEEDED(hr)) hr = g_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc[0].Get(), nullptr, IID_PPV_ARGS(&g_list));
    if (SUCCEEDED(hr)) hr = g_list->Close();
    if (SUCCEEDED(hr)) hr = g_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    if (SUCCEEDED(hr) && g_event == nullptr) g_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr)) g_list.Reset();
  }
  if (SUCCEEDED(hr)) {
    what = "the converted frame";
    D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC tex{};
    tex.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex.Width = g_width;
    tex.Height = g_height;
    tex.DepthOrArraySize = 1;
    tex.MipLevels = 1;
    tex.Format = REAL_FORMAT;
    tex.SampleDesc.Count = 1;
    tex.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    hr = g_device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_mid));
  }
  if (SUCCEEDED(hr) && !g_pass.Init(g_device.Get(), GAME_FORMAT, g_width, g_height, RING, what, hr) && SUCCEEDED(hr)) hr = E_FAIL;
  if (FAILED(hr)) {
    if (!g_failed_logged) Line("FP16 conversion: %s: 0x%08lx", what != nullptr ? what : "?", hr);
    g_failed_logged = true;
    return false;
  }
  g_pass_ready = true;
  return true;
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

// A resize of the game's swap chain or Streamline's: what format goes down, and the textures go. `asked` is the game's format.
DXGI_FORMAT BeforeResize(DXGI_FORMAT asked, bool& fp16_after) {
  AcquireSRWLockExclusive(&g_lock);
  DropTextures();
  ReleaseSRWLockExclusive(&g_lock);
  if (asked == GAME_FORMAT) {
    fp16_after = true;
    return REAL_FORMAT;
  }
  fp16_after = asked == DXGI_FORMAT_UNKNOWN && g_want.load();
  return asked;
}

void AfterResize(HRESULT hr, DXGI_FORMAT asked, bool fp16_after) {
  if (FAILED(hr)) return;
  const bool was = g_want.exchange(fp16_after);
  g_active = fp16_after;
  if (fp16_after) {
    if (!was) Line("the game's swap chain is now FP16 (scRGB): HSF converts it for DLSS-G");
    ApplySpace();
  } else if (was) {
    Line("the game's swap chain is now format %u: no FP16 conversion", static_cast<unsigned>(asked));
  }
}

HRESULT STDMETHODCALLTYPE HookGetBuffer(IDXGISwapChain* swap, UINT index, REFIID riid, void** out) {
  const Table* const t = For(swap);
  if (g_active.load() && swap == g_game.load() && out != nullptr) {
    AcquireSRWLockExclusive(&g_lock);
    HRESULT hr = E_FAIL;
    const bool made = MakeTextures();
    if (made && index < g_count) hr = g_proxy[index]->QueryInterface(riid, out);
    ReleaseSRWLockExclusive(&g_lock);
    if (made) return index < g_count ? hr : DXGI_ERROR_INVALID_CALL;
  }
  return t->get_buffer(swap, index, riid, out);
}

HRESULT STDMETHODCALLTYPE HookGetDesc(IDXGISwapChain* swap, DXGI_SWAP_CHAIN_DESC* desc) {
  const HRESULT hr = For(swap)->get_desc(swap, desc);
  if (SUCCEEDED(hr) && desc != nullptr && g_active.load() && swap == g_game.load() && desc->BufferDesc.Format == REAL_FORMAT) desc->BufferDesc.Format = GAME_FORMAT;
  return hr;
}

HRESULT STDMETHODCALLTYPE HookGetDesc1(IDXGISwapChain1* swap, DXGI_SWAP_CHAIN_DESC1* desc) {
  const HRESULT hr = For(swap)->get_desc1(swap, desc);
  if (SUCCEEDED(hr) && desc != nullptr && g_active.load() && swap == g_game.load() && desc->Format == REAL_FORMAT) desc->Format = GAME_FORMAT;
  return hr;
}

HRESULT STDMETHODCALLTYPE HookResize(IDXGISwapChain* swap, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
  const Table* const t = For(swap);
  if (!Decides(swap)) return t->resize(swap, count, width, height, format, flags);
  bool fp16_after = false;
  const DXGI_FORMAT down = BeforeResize(format, fp16_after);
  const HRESULT hr = t->resize(swap, count, width, height, down, flags);
  AfterResize(hr, format, fp16_after);
  return hr;
}

HRESULT STDMETHODCALLTYPE HookResize1(IDXGISwapChain3* swap, UINT count, UINT width, UINT height, DXGI_FORMAT format, UINT flags, const UINT* masks, IUnknown* const* queues) {
  const Table* const t = For(swap);
  if (!Decides(swap)) return t->resize1(swap, count, width, height, format, flags, masks, queues);
  bool fp16_after = false;
  const DXGI_FORMAT down = BeforeResize(format, fp16_after);
  const HRESULT hr = t->resize1(swap, count, width, height, down, flags, masks, queues);
  AfterResize(hr, format, fp16_after);
  return hr;
}

HRESULT STDMETHODCALLTYPE HookCheckSpace(IDXGISwapChain3* swap, DXGI_COLOR_SPACE_TYPE space, UINT* support) {
  if (g_active.load() && Ours(swap) && space == DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 && support != nullptr) {
    *support = DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT;
    return S_OK;
  }
  return For(swap)->check_space(swap, space, support);
}

// The game (or RenoDX) sets scRGB: the real swap chain gets HSF's colour space, which matches what the conversion writes.
HRESULT STDMETHODCALLTYPE HookSetSpace(IDXGISwapChain3* swap, DXGI_COLOR_SPACE_TYPE space) {
  if (g_active.load() && Ours(swap)) space = g_space;
  return For(swap)->set_space(swap, space);
}

// A table whose GetBuffer is already HSF's was copied from one HSF wrote into: For() finds it by its locator, and its slots are not originals.
bool Carries(void** table) {
  void* slot = nullptr;
  return ReadProcessMemory(GetCurrentProcess(), table + SLOT_GET_BUFFER, &slot, sizeof(slot), nullptr) && slot == reinterpret_cast<void*>(&HookGetBuffer);
}

void HookTableAt(void** const table) {
  if (table == nullptr) return;
  for (Table& t : g_tables) {
    if (t.table == table) return;
  }
  if (Carries(table)) return;
  for (Table& t : g_tables) {
    if (t.table != nullptr) continue;
    t.get_buffer = reinterpret_cast<GetBufferFn>(table[SLOT_GET_BUFFER]);
    t.get_desc = reinterpret_cast<GetDescFn>(table[SLOT_GET_DESC]);
    t.resize = reinterpret_cast<ResizeBuffersFn>(table[SLOT_RESIZE_BUFFERS]);
    t.get_desc1 = reinterpret_cast<GetDesc1Fn>(table[SLOT_GET_DESC1]);
    t.check_space = reinterpret_cast<CheckColorSpaceFn>(table[SLOT_CHECK_COLOR_SPACE]);
    t.set_space = reinterpret_cast<SetColorSpace1Fn>(table[SLOT_SET_COLOR_SPACE1]);
    t.resize1 = reinterpret_cast<ResizeBuffers1Fn>(table[SLOT_RESIZE_BUFFERS1]);
    t.locator = Locator(table);
    t.table = table;
    const bool written = WriteSlot(table, SLOT_GET_BUFFER, reinterpret_cast<void*>(&HookGetBuffer)) &&
                         WriteSlot(table, SLOT_GET_DESC, reinterpret_cast<void*>(&HookGetDesc)) &&
                         WriteSlot(table, SLOT_RESIZE_BUFFERS, reinterpret_cast<void*>(&HookResize)) &&
                         WriteSlot(table, SLOT_GET_DESC1, reinterpret_cast<void*>(&HookGetDesc1)) &&
                         WriteSlot(table, SLOT_CHECK_COLOR_SPACE, reinterpret_cast<void*>(&HookCheckSpace)) &&
                         WriteSlot(table, SLOT_SET_COLOR_SPACE1, reinterpret_cast<void*>(&HookSetSpace)) &&
                         WriteSlot(table, SLOT_RESIZE_BUFFERS1, reinterpret_cast<void*>(&HookResize1));
    Line("FP16 watch on the swap chain table %p: %d", table, written ? 1 : 0);
    return;
  }
  Line("FP16 watch: no room for the table %p", table);
}

void HookTable(void* object) {
  if (object != nullptr) HookTableAt(*static_cast<void***>(object));
}

}  // namespace

bool Rewrite(DXGI_FORMAT& format) {
  if (format != GAME_FORMAT) {
    g_want = false;
    return false;
  }
  format = REAL_FORMAT;
  g_want = true;
  return true;
}

void Start(IUnknown* game, IUnknown* made, IUnknown* queue, IUnknown* real_queue, LineFn line, void* made_class_table) {
  g_line = line;
  if (game == nullptr || queue == nullptr || real_queue == nullptr) return;
  AcquireSRWLockExclusive(&g_lock);
  DropTextures();
  g_game = game;
  g_made = made != nullptr ? made : game;
  ComPtr<ID3D12CommandQueue> game_queue;
  g_game_device.Reset();
  if (SUCCEEDED(queue->QueryInterface(IID_PPV_ARGS(&game_queue)))) game_queue->GetDevice(IID_PPV_ARGS(&g_game_device));
  ComPtr<ID3D12CommandQueue> real;
  if (SUCCEEDED(real_queue->QueryInterface(IID_PPV_ARGS(&real))) && real != g_queue) {
    // Another device: HSF's own objects go with the old one.
    g_list.Reset();
    for (auto& a : g_alloc) a.Reset();
    g_fence.Reset();
    g_value = 0;
    for (auto& v : g_alloc_value) v = 0;
    g_queue = real;
    g_device.Reset();
    g_queue->GetDevice(IID_PPV_ARGS(&g_device));
  }
  ReleaseSRWLockExclusive(&g_lock);
  // The class table first: a copy put on the object before this still holds Streamline's own methods, and is hooked next as itself.
  if (made_class_table != nullptr && made_class_table != *reinterpret_cast<void**>(made != nullptr ? made : game)) {
    HookTableAt(static_cast<void**>(made_class_table));
    Line("FP16: Streamline's swap chain has a copy of its table on it (%p, its class table %p): both carry HSF's methods",
         *reinterpret_cast<void**>(made != nullptr ? made : game), made_class_table);
  }
  HookTable(game);
  HookTable(made);
  g_active = g_want.load() && g_game_device && g_device;
  if (g_want.load() && !g_active.load()) Line("FP16 (scRGB) swap chain: no device to convert it on");
  if (g_active.load()) {
    Line("the game's swap chain is FP16 (scRGB), which DLSS-G does not take: DXGI's is R10G10B10A2 and HSF converts each frame");
    ApplySpace();
  }
}

void BeforePresent(IUnknown* game) {
  if (!g_active.load() || game != g_game.load()) return;
  // A copy made before HSF hooked the class table may be put back on an object later: it is hooked as itself.
  for (void* const object : {g_game.load(), g_made.load()}) {
    if (object != nullptr && For(object) != nullptr && !Carries(*static_cast<void***>(object))) HookTable(object);
  }
  const bool due = g_space_due.exchange(false);
  if (due || ++g_presents % SPACE_EVERY == 0) PushSpace(due);
  AcquireSRWLockExclusive(&g_lock);
  ComPtr<IDXGISwapChain3> made = Made();
  const UINT index = made ? made->GetCurrentBackBufferIndex() : UINT_MAX;
  ComPtr<ID3D12Resource> real;
  if (index < g_count && g_proxy[index] && MakeConverter() && SUCCEEDED(MadeBuffer(index, &real))) {
    const UINT slot = static_cast<UINT>(g_frame++ % RING);
    if (g_fence->GetCompletedValue() < g_alloc_value[slot]) {
      g_fence->SetEventOnCompletion(g_alloc_value[slot], g_event);
      WaitForSingleObject(g_event, 2000);
    }
    g_alloc[slot]->Reset();
    g_list->Reset(g_alloc[slot].Get(), nullptr);
    // The game left its texture in PRESENT (COMMON), which a shader read promotes from.
    g_pass.Record(g_list.Get(), slot, 0, g_proxy[index].Get(), g_mid.Get(), g_pq);
    Barrier(g_list.Get(), g_mid.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(g_list.Get(), real.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    g_list->CopyResource(real.Get(), g_mid.Get());
    Barrier(g_list.Get(), real.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Barrier(g_list.Get(), g_mid.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (SUCCEEDED(g_list->Close())) {
      ID3D12CommandList* const lists[] = {g_list.Get()};
      g_queue->ExecuteCommandLists(1, lists);
      g_queue->Signal(g_fence.Get(), ++g_value);
      g_alloc_value[slot] = g_value;
    }
  }
  ReleaseSRWLockExclusive(&g_lock);
}

void SetNative(IUnknown* native) { g_native = native; }

bool Active() { return g_active.load(); }

}  // namespace hsf::fp16swap
