#include "apex_fg.h"

#include "hsf_log.h"
#include "sl_bring.h"

#include <nvsdk_ngx_params.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <string>
#include <type_traits>
#include <variant>

namespace hsf::apex {
namespace {

using Microsoft::WRL::ComPtr;
constexpr const char* LOG = "APEX";
constexpr D3D12_RESOURCE_STATES READ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES READ_ANY = READ | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
constexpr D3D12_RESOURCE_STATES UAV = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

using InitFn = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, ID3D12Device*, NVSDK_NGX_Version, const NVSDK_NGX_FeatureCommonInfo*);
using PopulateFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Parameter*);
using CreateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, NVSDK_NGX_Feature, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
using EvaluateFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
using ReleaseFn = NVSDK_NGX_Result(NVSDK_CONV*)(NVSDK_NGX_Handle*);
using ShutdownFn = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12Device*);

// The provider ABI and resource keys are documented by Apex nr-replay (FG-V.B20)
// and NVIDIA's NVSDK_NGX_Parameter and DLSS-G headers, v310.9.16.
class Parameters final : public NVSDK_NGX_Parameter {
public:
  void Set(const char* name, unsigned long long value) override { Put(name, value); }
  void Set(const char* name, float value) override { Put(name, static_cast<double>(value)); }
  void Set(const char* name, double value) override { Put(name, value); }
  void Set(const char* name, unsigned value) override { Put(name, static_cast<unsigned long long>(value)); }
  void Set(const char* name, int value) override { Put(name, static_cast<double>(value)); }
  void Set(const char* name, ID3D11Resource* value) override { Put(name, value); }
  void Set(const char* name, ID3D12Resource* value) override { Put(name, value); }
  void Set(const char* name, void* value) override { Put(name, value); }

  NVSDK_NGX_Result Get(const char* name, unsigned long long* out) const override { return Number(name, out); }
  NVSDK_NGX_Result Get(const char* name, float* out) const override { return Number(name, out); }
  NVSDK_NGX_Result Get(const char* name, double* out) const override { return Number(name, out); }
  NVSDK_NGX_Result Get(const char* name, unsigned* out) const override { return Number(name, out); }
  NVSDK_NGX_Result Get(const char* name, int* out) const override { return Number(name, out); }
  NVSDK_NGX_Result Get(const char* name, ID3D11Resource** out) const override { return Pointer(name, out); }
  NVSDK_NGX_Result Get(const char* name, ID3D12Resource** out) const override { return Pointer(name, out); }
  NVSDK_NGX_Result Get(const char* name, void** out) const override { return Pointer(name, out); }
  void Reset() override { values_.clear(); }

private:
  using Value = std::variant<unsigned long long, double, ID3D11Resource*, ID3D12Resource*, void*>;
  std::map<std::string, Value, std::less<>> values_;

  void Put(const char* name, Value value) {
    const auto it = values_.lower_bound(name);
    if (it != values_.end() && it->first == name) it->second = value;
    else values_.emplace_hint(it, name, value);
  }

  template <typename T>
  NVSDK_NGX_Result Number(const char* name, T* out) const {
    const auto it = values_.find(name);
    if (it == values_.end() || out == nullptr) return NVSDK_NGX_Result_Fail;
    return std::visit([out](auto value) {
      using V = decltype(value);
      if constexpr (std::is_arithmetic_v<V>) {
        if constexpr (std::is_integral_v<T>) {
          if constexpr (std::is_floating_point_v<V>) {
            if (!(value >= static_cast<V>(std::numeric_limits<T>::lowest()) &&
                  value < static_cast<V>(std::numeric_limits<T>::max()) + 1.0)) return NVSDK_NGX_Result_Fail;
          } else if (value > static_cast<V>(std::numeric_limits<T>::max())) {
            return NVSDK_NGX_Result_Fail;
          }
        }
        *out = static_cast<T>(value);
        return NVSDK_NGX_Result_Success;
      } else {
        return NVSDK_NGX_Result_Fail;
      }
    }, it->second);
  }

  template <typename T>
  NVSDK_NGX_Result Pointer(const char* name, T** out) const {
    const auto it = values_.find(name);
    if (it == values_.end() || out == nullptr) return NVSDK_NGX_Result_Fail;
    return std::visit([out](auto value) {
      using V = decltype(value);
      if constexpr (std::is_same_v<V, T*> || std::is_same_v<V, void*> ||
                    (std::is_same_v<T, void> && std::is_pointer_v<V>)) {
        *out = static_cast<T*>(value);
        return NVSDK_NGX_Result_Success;
      } else {
        return NVSDK_NGX_Result_Fail;
      }
    }, it->second);
  }
};

template <typename Fn>
bool Resolve(HMODULE module, const char* name, Fn& fn) {
  fn = reinterpret_cast<Fn>(GetProcAddress(module, name));
  if (!fn) hsflog::Writef(LOG, "missing provider export %s", name);
  return fn != nullptr;
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
  if (from == to) return;
  D3D12_RESOURCE_BARRIER barrier{};
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, to};
  list->ResourceBarrier(1, &barrier);
}

}  // namespace

struct Provider::Api {
  HMODULE module = nullptr;
  ComPtr<ID3D12Device> device;
  PopulateFn populate = nullptr;
  CreateFn create = nullptr;
  EvaluateFn evaluate = nullptr;
  ReleaseFn release = nullptr;
  ShutdownFn shutdown = nullptr;
  bool debug = false;
  unsigned max_generated = 1;

  ~Api() {
    if (device) {
      const auto result = shutdown(device.Get());
      hsflog::Writef(LOG, "provider shutdown 0x%08x", static_cast<unsigned>(result));
    }
    if (module) FreeLibrary(module);
  }
};

Provider::Provider() = default;
Provider::~Provider() = default;

bool Provider::Init(const wchar_t* path, ID3D12Device* device, bool debug) {
  auto api = std::make_unique<Api>();
  hsflog::Writef(LOG, "loading provider %ls", path);
  api->module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
  if (!api->module) {
    hsflog::Writef(LOG, "provider or dependency could not load (Win32 %lu)", GetLastError());
    return false;
  }
  InitFn init = nullptr;
  FARPROC presentation = GetProcAddress(api->module, "NRFG_PresentationControl");
  if (!presentation) hsflog::Write(LOG, "missing Apex provider identifier NRFG_PresentationControl");
  const bool exports = Resolve(api->module, "NVSDK_NGX_D3D12_Init_Ext", init)
      & Resolve(api->module, "NVSDK_NGX_D3D12_PopulateParameters_Impl", api->populate)
      & Resolve(api->module, "NVSDK_NGX_D3D12_CreateFeature", api->create)
      & Resolve(api->module, "NVSDK_NGX_D3D12_EvaluateFeature", api->evaluate)
      & Resolve(api->module, "NVSDK_NGX_D3D12_ReleaseFeature", api->release)
      & Resolve(api->module, "NVSDK_NGX_D3D12_Shutdown1", api->shutdown);
  if (!exports || !presentation) return false;
  const std::wstring dll_path(path);
  const std::wstring folder = dll_path.substr(0, dll_path.find_last_of(L"\\/"));
  const auto result = init(1, folder.c_str(), device, NVSDK_NGX_Version_API, nullptr);
  hsflog::Writef(LOG, "D3D12 provider init 0x%08x", static_cast<unsigned>(result));
  if (NVSDK_NGX_FAILED(result)) return false;
  api->device = device;
  api->debug = debug;
  Parameters parameters;
  unsigned capacity = 1;
  if (!NVSDK_NGX_FAILED(api->populate(&parameters))) parameters.Get("DLSSG.MultiFrameCountMax", &capacity);
  api->max_generated = std::clamp(capacity, 1u, 2u);
  hsflog::Writef(LOG, "provider reports %u generated frames; HSF supports up to %u", capacity, api->max_generated);
  api_ = std::move(api);
  return true;
}

unsigned Provider::MaxGenerated() const { return api_->max_generated; }

struct Session::State {
  Provider::Api* api = nullptr;
  ComPtr<ID3D12CommandQueue> queue;
  Parameters parameters;
  NVSDK_NGX_Handle* feature = nullptr;
  ComPtr<ID3D12Resource> original, interpolated, second_interpolated, provider_real;
  unsigned width = 0, height = 0, motion_width = 0, motion_height = 0;
  bool failed = false;
  unsigned submitted = 0, last_frames = 0;
  struct Slot {
    ComPtr<ID3D12CommandAllocator> allocator;
    UINT64 busy_until = 0;
  };
  std::array<Slot, 4> slots;
  ComPtr<ID3D12GraphicsCommandList> copy_list;
  ComPtr<ID3D12Fence> fence;
  UINT64 fence_value = 0;
  size_t slot = 0;

  ~State() {
    if (fence_value && fence->GetCompletedValue() < fence_value) {
      const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
      const bool done = event && SUCCEEDED(fence->SetEventOnCompletion(fence_value, event)) &&
                        WaitForSingleObject(event, 2000) == WAIT_OBJECT_0;
      if (event) CloseHandle(event);
      if (!done) hsflog::Writef(LOG, "presentation fence timeout on shutdown, device 0x%08lx", api->device->GetDeviceRemovedReason());
    }
    if (feature) api->release(feature);
  }

  HRESULT CopyTo(ID3D12Resource* target, ID3D12Resource* source, D3D12_RESOURCE_STATES source_state) {
    HRESULT hr = copy_list->Reset(slots[slot].allocator.Get(), nullptr);
    if (FAILED(hr)) return hr;
    auto* list = copy_list.Get();
    Barrier(list, target, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    Barrier(list, source, source_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyResource(target, source);
    Barrier(list, source, D3D12_RESOURCE_STATE_COPY_SOURCE, source_state);
    Barrier(list, target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    hr = list->Close();
    if (SUCCEEDED(hr)) {
      ID3D12CommandList* const commands[] = {list};
      queue->ExecuteCommandLists(1, commands);
    }
    return hr;
  }

  HRESULT CopyCurrent(IDXGISwapChain3* swap, ID3D12Resource* source, D3D12_RESOURCE_STATES source_state) {
    ComPtr<ID3D12Resource> back;
    HRESULT hr = swap->GetBuffer(swap->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back));
    return FAILED(hr) ? hr : CopyTo(back.Get(), source, source_state);
  }
};

Session::Session() = default;
Session::~Session() = default;

bool Session::Init(Provider& provider, const slbring::FrameFeed& feed, unsigned input_width, unsigned input_height) {
  auto state = std::make_unique<State>();
  state->api = provider.api_.get();
  state->queue = feed.queue;
  auto desc = feed.back_buffer->GetDesc();
  state->width = static_cast<unsigned>(desc.Width);
  state->height = desc.Height;
  state->motion_width = input_width;
  state->motion_height = input_height;
  desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  desc.Alignment = 0;
  desc.Flags = D3D12_RESOURCE_FLAG_NONE;
  const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
  HRESULT hr = feed.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, READ, nullptr, IID_PPV_ARGS(&state->original));
  desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
  for (auto** output : {state->interpolated.GetAddressOf(), state->provider_real.GetAddressOf()}) {
    if (SUCCEEDED(hr)) hr = feed.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, UAV, nullptr, IID_PPV_ARGS(output));
  }
  for (auto& slot : state->slots) {
    if (SUCCEEDED(hr)) hr = feed.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator));
  }
  if (SUCCEEDED(hr)) hr = feed.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, state->slots[0].allocator.Get(), nullptr, IID_PPV_ARGS(&state->copy_list));
  if (SUCCEEDED(hr)) hr = state->copy_list->Close();
  if (SUCCEEDED(hr)) hr = feed.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&state->fence));
  if (FAILED(hr)) {
    hsflog::Writef(LOG, "FP16 resources or presentation queue setup failed (0x%08lx)", hr);
    return false;
  }
  auto& p = state->parameters;
  const auto populated = state->api->populate(&p);
  if (NVSDK_NGX_FAILED(populated)) {
    hsflog::Writef(LOG, "provider parameters failed (0x%08x)", static_cast<unsigned>(populated));
    return false;
  }
  p.Set("CreationNodeMask", 1u);
  p.Set("VisibilityNodeMask", 1u);
  p.Set(NVSDK_NGX_Parameter_Width, state->width);
  p.Set(NVSDK_NGX_Parameter_Height, state->height);
  p.Set("DLSSG.Width", state->width);
  p.Set("DLSSG.Height", state->height);
  p.Set("DLSSG.BackbufferFormat", static_cast<unsigned>(desc.Format));
  p.Set("DLSSG.InternalWidth", input_width);
  p.Set("DLSSG.InternalHeight", input_height);
  p.Set("DLSSG.DynamicResolution", 1u);
  state_ = std::move(state);
  return true;
}

FrameResult Session::Record(ID3D12GraphicsCommandList* list, const slbring::FrameFeed& feed, ID3D12Resource* motion,
                            D3D12_RESOURCE_STATES motion_state, ID3D12Resource* depth, bool dilated, bool reset, unsigned frames) {
  auto& s = *state_;
  if (s.failed) return FrameResult::failed;
  frames = std::clamp(frames, 1u, s.api->max_generated);
  if (frames == 2 && !s.second_interpolated) {
    const auto desc = s.interpolated->GetDesc();
    const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
    const HRESULT hr = s.api->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, UAV, nullptr,
                                                              IID_PPV_ARGS(&s.second_interpolated));
    if (FAILED(hr)) {
      hsflog::Writef(LOG, "second interpolated FP16 resource failed 0x%08lx", hr);
      s.failed = true;
      return FrameResult::failed;
    }
  }
  reset |= s.last_frames != frames;
  s.last_frames = frames;
  auto& p = s.parameters;
  if (!s.feature) {
    const auto created = s.api->create(list, NVSDK_NGX_Feature_FrameGeneration, &p, &s.feature);
    hsflog::Writef(LOG, "CreateFeature 0x%08x, colour in/out format %u, %ux%u, scRGB %u, HDR conversion off, motion %u and depth %u at %ux%u",
                   static_cast<unsigned>(created), static_cast<unsigned>(s.original->GetDesc().Format), s.width, s.height,
                   feed.colour_space, static_cast<unsigned>(motion->GetDesc().Format), static_cast<unsigned>(depth->GetDesc().Format), s.motion_width, s.motion_height);
    if (!s.feature || NVSDK_NGX_FAILED(created)) {
      s.failed = true;
      return FrameResult::failed;
    }
    reset = true;
  }
  const auto source_state = static_cast<D3D12_RESOURCE_STATES>(feed.back_buffer_state);
  Barrier(list, feed.back_buffer, source_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
  Barrier(list, s.original.Get(), READ, D3D12_RESOURCE_STATE_COPY_DEST);
  list->CopyResource(s.original.Get(), feed.back_buffer);
  Barrier(list, s.original.Get(), D3D12_RESOURCE_STATE_COPY_DEST, READ);
  Barrier(list, feed.back_buffer, D3D12_RESOURCE_STATE_COPY_SOURCE, source_state);
  p.Set("DLSSG.Backbuffer", s.original.Get());
  p.Set("DLSSG.MVecs", motion);
  p.Set("DLSSG.Depth", depth);
  p.Set("DLSSG.OutputReal", s.provider_real.Get());
  p.Set("DLSSG.ColorBuffersHDR", 1u);
  p.Set("DLSSG.Reset", reset ? 1u : 0u);
  p.Set("DLSSG.BackbufferFrameID", s.submitted + 1);
  p.Set("DLSSG.MultiFrameCount", frames);
  p.Set("DLSSG.DepthInverted", static_cast<unsigned>(feed.depth_inverted));
  p.Set("DLSSG.CameraMotionIncluded", 1u);
  p.Set("DLSSG.MvecDilated", static_cast<unsigned>(dilated));
  p.Set("DLSSG.MvecJittered", static_cast<unsigned>(feed.jittered));
  p.Set("DLSSG.JitterOffsetX", feed.jitter[0]);
  p.Set("DLSSG.JitterOffsetY", feed.jitter[1]);
  const unsigned render_width = feed.width ? feed.width : s.motion_width;
  const unsigned render_height = feed.height ? feed.height : s.motion_height;
  // HSF's shared inputs express motion in UV units. Apex's direct provider expects pixels in the motion subrect.
  const float motion_scale_x = feed.scale[0] * static_cast<float>(render_width);
  const float motion_scale_y = feed.scale[1] * static_cast<float>(render_height);
  p.Set("DLSSG.MvecScaleX", motion_scale_x);
  p.Set("DLSSG.MvecScaleY", motion_scale_y);
  if (s.api->debug && s.submitted == 0)
    hsflog::Writef(LOG, "motion scale UV %.6g %.6g, Apex pixels %.6g %.6g", feed.scale[0], feed.scale[1], motion_scale_x, motion_scale_y);
  p.Set("DLSSG.MVecsSubrectBaseX", feed.x);
  p.Set("DLSSG.MVecsSubrectBaseY", feed.y);
  p.Set("DLSSG.MVecsSubrectWidth", render_width);
  p.Set("DLSSG.MVecsSubrectHeight", render_height);
  p.Set("DLSSG.DepthSubrectBaseX", feed.x);
  p.Set("DLSSG.DepthSubrectBaseY", feed.y);
  p.Set("DLSSG.DepthSubrectWidth", render_width);
  p.Set("DLSSG.DepthSubrectHeight", render_height);
  Barrier(list, motion, motion_state, READ);
  Barrier(list, depth, READ_ANY, READ);
  NVSDK_NGX_Result evaluated = NVSDK_NGX_Result_Success;
  for (unsigned index = 1; index <= frames; ++index) {
    p.Set("DLSSG.MultiFrameIndex", index);
    p.Set("DLSSG.OutputInterpolated", index == 1 ? s.interpolated.Get() : s.second_interpolated.Get());
    evaluated = s.api->evaluate(list, s.feature, &p, nullptr);
    if (NVSDK_NGX_FAILED(evaluated)) break;
  }
  Barrier(list, depth, READ, READ_ANY);
  Barrier(list, motion, READ, motion_state);
  const unsigned submitted = ++s.submitted;
  if (NVSDK_NGX_FAILED(evaluated) || submitted == 1 || (s.api->debug && (reset || submitted % 120 == 0))) {
    hsflog::Writef(LOG, "EvaluateFeature frame %u 0x%08x, reset %u",
                   submitted, static_cast<unsigned>(evaluated), static_cast<unsigned>(reset));
  }
  if (NVSDK_NGX_FAILED(evaluated)) {
    s.failed = true;
    return FrameResult::failed;
  }
  return reset ? FrameResult::real_only : FrameResult::generated;
}

bool Session::BeginPresent(ID3D12Resource* back) {
  auto& s = *state_;
  if (s.failed) return false;
  auto& slot = s.slots[s.slot];
  const auto completed = s.fence->GetCompletedValue();
  if (completed == UINT64_MAX || completed < slot.busy_until) {
    if (s.api->debug || completed == UINT64_MAX) hsflog::Writef(LOG, "presentation queue busy or removed (device 0x%08lx)", s.api->device->GetDeviceRemovedReason());
    return false;
  }
  HRESULT hr = slot.allocator->Reset();
  if (SUCCEEDED(hr)) hr = s.CopyTo(back, s.interpolated.Get(), UAV);
  if (FAILED(hr)) {
    hsflog::Writef(LOG, "generated-frame copy failed 0x%08lx", hr);
    s.failed = true;
  }
  return SUCCEEDED(hr);
}

HRESULT Session::CopySecondGenerated(IDXGISwapChain3* swap) {
  auto& s = *state_;
  if (!s.second_interpolated) return E_UNEXPECTED;
  return s.CopyCurrent(swap, s.second_interpolated.Get(), UAV);
}

HRESULT Session::RestoreReal(IDXGISwapChain3* swap) {
  auto& s = *state_;
  const HRESULT hr = s.CopyCurrent(swap, s.original.Get(), READ);
  if (FAILED(hr)) {
    hsflog::Writef(LOG, "real-frame copy failed 0x%08lx, device 0x%08lx", hr, s.api->device->GetDeviceRemovedReason());
    s.failed = true;
  }
  return hr;
}

void Session::EndPresent() {
  auto& s = *state_;
  const UINT64 next = s.fence_value + 1;
  const HRESULT hr = s.queue->Signal(s.fence.Get(), next);
  if (SUCCEEDED(hr)) {
    s.fence_value = next;
    s.slots[s.slot].busy_until = next;
    s.slot = (s.slot + 1) % s.slots.size();
  } else {
    hsflog::Writef(LOG, "presentation fence signal failed 0x%08lx", hr);
    s.failed = true;
  }
}

}  // namespace hsf::apex
