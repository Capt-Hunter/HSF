// See vk_bridge.h.
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include "vk_bridge.h"
#include "vk_hook.h"

#include <vulkan/vulkan.h>

#include <d3d11_4.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;
namespace api = reshade::api;

namespace hsf::vk {
namespace {

constexpr uint32_t SETS = 3;

struct Functions {
  PFN_vkCreateImage CreateImage = nullptr;
  PFN_vkDestroyImage DestroyImage = nullptr;
  PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
  PFN_vkAllocateMemory AllocateMemory = nullptr;
  PFN_vkFreeMemory FreeMemory = nullptr;
  PFN_vkBindImageMemory BindImageMemory = nullptr;
  PFN_vkGetMemoryWin32HandlePropertiesKHR GetMemoryWin32HandlePropertiesKHR = nullptr;
  PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
  PFN_vkCmdCopyImage CmdCopyImage = nullptr;
  // Same frame only (HSF's Vulkan layer turned the extension on):
  PFN_vkCreateSemaphore CreateSemaphore = nullptr;
  PFN_vkDestroySemaphore DestroySemaphore = nullptr;
  PFN_vkImportSemaphoreWin32HandleKHR ImportSemaphoreWin32HandleKHR = nullptr;
  PFN_vkQueueSubmit QueueSubmit = nullptr;
  PFN_vkSignalSemaphore SignalSemaphore = nullptr;  // the safety net (Vulkan 1.2, or VK_KHR_timeline_semaphore's)
};

// How long D3D11 may wait for a set the game's GPU never signals before the safety net lets it go.
constexpr ULONGLONG STUCK_MS = 2000;
// Without shared semaphores: how long the game thread may wait for its frame's copies on the GPU. About one frame at 60 fps; longer and the
// frame goes one frame behind, as before.
constexpr uint64_t CPU_WAIT_NS = 16000000;

// One input of one set: a texture of HSF's D3D11 device and the same memory as an image on the game's Vulkan device.
struct Input {
  ComPtr<ID3D11Texture2D> texture;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint32_t width = 0, height = 0;
  api::format format = api::format::unknown;
};

struct Set {
  Input color, motion, depth, hudless;
  bool has_motion = false, has_depth = false;
  uint64_t hudless_id = 0;  // the frame (its vk_id) whose copy from before the overlays `hudless` holds (0 = none)
  uint64_t vk_id = 0;     // ReShade's fence reaches this once the game's GPU finished this set's copies (0 = never written)
  bool handed = false;    // given to D3D11 (or dropped for a newer one)
  uint64_t d3d_id = 0;    // HSF's D3D11 fence reaches this once D3D11 finished copying it to the helper (0 = nothing pending)
};

struct State {
  api::device* device = nullptr;
  VkDevice vk_device = VK_NULL_HANDLE;
  Functions vk;
  ComPtr<ID3D11Device5> device11;
  ComPtr<ID3D11DeviceContext4> context11;
  ComPtr<ID3D11Fence> d3d_done;
  uint64_t d3d_next = 1;
  api::fence fence = {0};  // ReShade's, on the game's device (a timeline semaphore)
  // Same frame: one fence, D3D11's side and Vulkan's side. The game's queue signals it with a set's vk_id after the set's copies.
  bool same_frame = false;
  ComPtr<ID3D11Fence> shared_fence;
  VkSemaphore semaphore = VK_NULL_HANDLE;
  ULONGLONG stuck_since = 0;  // when the set Bridge wants first waited on a signal that has not come (0 = not waiting)
  uint64_t next_id = 1;
  Set sets[SETS];
  bool pending_hand = false;  // Bridge handed a set and Handed has not run yet
  uint64_t pending_d3d = 0;
  Stats stats;
  char problem[200] = "";
};

State* g = nullptr;

void Problem(const char* text) {
  if (g == nullptr) return;
  std::snprintf(g->problem, sizeof(g->problem), "%s", text);
  ++g->stats.failed;
}

VkFormat ToVk(api::format format) {
  switch (format) {
    case api::format::b8g8r8a8_typeless:
    case api::format::b8g8r8a8_unorm:
    case api::format::b8g8r8a8_unorm_srgb: return VK_FORMAT_B8G8R8A8_UNORM;
    case api::format::r8g8b8a8_typeless:
    case api::format::r8g8b8a8_unorm:
    case api::format::r8g8b8a8_unorm_srgb: return VK_FORMAT_R8G8B8A8_UNORM;
    case api::format::r10g10b10a2_typeless:
    case api::format::r10g10b10a2_unorm: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case api::format::r16g16b16a16_typeless:
    case api::format::r16g16b16a16_float: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case api::format::r16g16_float: return VK_FORMAT_R16G16_SFLOAT;
    case api::format::r32g32_float: return VK_FORMAT_R32G32_SFLOAT;
    case api::format::r32_float: return VK_FORMAT_R32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
  }
}

void Release(Input& input) {
  if (input.image != VK_NULL_HANDLE) g->vk.DestroyImage(g->vk_device, input.image, nullptr);
  if (input.memory != VK_NULL_HANDLE) g->vk.FreeMemory(g->vk_device, input.memory, nullptr);
  input = Input{};
}

// Makes `input` for `width` x `height` in `format`: the D3D11 texture with an NT handle, imported into Vulkan as a D3D11 texture.
bool Make(Input& input, uint32_t width, uint32_t height, api::format format) {
  Release(input);
  const DXGI_FORMAT dxgi = ToDxgi(format);
  const VkFormat vk_format = ToVk(format);
  if (dxgi == DXGI_FORMAT_UNKNOWN || vk_format == VK_FORMAT_UNDEFINED) {
    char text[120];
    std::snprintf(text, sizeof(text), "format %u is not bridged yet", static_cast<unsigned>(format));
    Problem(text);
    return false;
  }
  const D3D11_TEXTURE2D_DESC desc{width, height, 1, 1, dxgi, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0,
                                  D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE};
  ComPtr<IDXGIResource1> shared;
  HANDLE handle = nullptr;
  if (FAILED(g->device11->CreateTexture2D(&desc, nullptr, &input.texture)) || FAILED(input.texture.As(&shared)) ||
      FAILED(shared->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &handle))) {
    Problem("HSF's D3D11 device could not make a shared texture");
    input = Input{};
    return false;
  }
  struct CloseOnExit {
    HANDLE h;
    ~CloseOnExit() { CloseHandle(h); }  // the import does not take the handle over
  } close_on_exit{handle};

  VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
  VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.pNext = &external;
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = vk_format;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
  info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  if (g->vk.CreateImage(g->vk_device, &info, nullptr, &input.image) != VK_SUCCESS) {
    Problem("Vulkan could not make the image for a shared texture");
    input = Input{};
    return false;
  }
  VkMemoryRequirements needs{};
  g->vk.GetImageMemoryRequirements(g->vk_device, input.image, &needs);
  VkMemoryWin32HandlePropertiesKHR handle_properties{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
  uint32_t types = needs.memoryTypeBits;
  if (g->vk.GetMemoryWin32HandlePropertiesKHR != nullptr &&
      g->vk.GetMemoryWin32HandlePropertiesKHR(g->vk_device, VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT, handle, &handle_properties) == VK_SUCCESS &&
      (types & handle_properties.memoryTypeBits) != 0)
    types &= handle_properties.memoryTypeBits;
  uint32_t type = 0;
  while (type < 32 && (types & (1u << type)) == 0) ++type;
  VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated.image = input.image;
  VkImportMemoryWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
  import.pNext = &dedicated;
  import.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_D3D11_TEXTURE_BIT;
  import.handle = handle;
  VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.pNext = &import;
  allocate.allocationSize = needs.size;
  allocate.memoryTypeIndex = type;
  if (type >= 32 || g->vk.AllocateMemory(g->vk_device, &allocate, nullptr, &input.memory) != VK_SUCCESS ||
      g->vk.BindImageMemory(g->vk_device, input.image, input.memory, 0) != VK_SUCCESS) {
    Problem("Vulkan could not import a shared texture of HSF's D3D11 device");
    Release(input);
    return false;
  }
  input.width = width;
  input.height = height;
  input.format = format;
  return true;
}

// `input` matches `desc`, made again if not. False if it could not be made.
bool Fit(Input& input, const api::resource_desc& desc) {
  if (input.image != VK_NULL_HANDLE && input.width == desc.texture.width && input.height == desc.texture.height && input.format == desc.texture.format)
    return true;
  return Make(input, desc.texture.width, desc.texture.height, desc.texture.format);
}

void ImageBarrier(VkCommandBuffer list, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access, VkAccessFlags dst_access) {
  VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  g->vk.CmdPipelineBarrier(list, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

void Copy(VkCommandBuffer list, VkImage source, const Input& to) {
  VkImageCopy copy{};
  copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  copy.dstSubresource = copy.srcSubresource;
  copy.extent = {to.width, to.height, 1};
  g->vk.CmdCopyImage(list, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
}

// A ReShade resource handle as the VkImage it is (a pointer in 64-bit builds, a 64-bit integer in 32-bit ones).
VkImage ImageOf(api::resource resource) { return (VkImage)(resource.handle); }

// D3D11 finished with the set (nothing pending, or its fence passed).
bool FreeOnD3d(const Set& set) { return set.d3d_id == 0 || g->d3d_done->GetCompletedValue() >= set.d3d_id; }

// Records the copies of `back_buffer`, `motion` and `depth` into `set` on the queue's immediate command list and sends them to the GPU.
bool Record(api::command_queue* queue, Set& set, api::resource back_buffer, api::resource_usage back_buffer_state, api::resource motion,
            api::resource depth) {
  api::device* const device = g->device;
  const api::resource_desc back_desc = device->get_resource_desc(back_buffer);
  if (!Fit(set.color, back_desc)) return false;
  set.has_motion = motion.handle != 0 && device->get_resource_desc(motion).type == api::resource_type::texture_2d && Fit(set.motion, device->get_resource_desc(motion));
  set.has_depth = depth.handle != 0 && device->get_resource_desc(depth).type == api::resource_type::texture_2d && Fit(set.depth, device->get_resource_desc(depth));

  api::command_list* const cmd = queue->get_immediate_command_list();
  if (cmd == nullptr) {
    Problem("the game's queue has no immediate command list");
    return false;
  }
  const VkCommandBuffer list = reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(cmd->get_native()));
  api::resource sources[3] = {back_buffer};
  api::resource_usage before[3] = {back_buffer_state};
  api::resource_usage copying[3] = {api::resource_usage::copy_source};
  uint32_t count = 1;
  if (set.has_motion) sources[count] = motion, before[count] = api::resource_usage::shader_resource, copying[count++] = api::resource_usage::copy_source;
  if (set.has_depth) sources[count] = depth, before[count] = api::resource_usage::shader_resource, copying[count++] = api::resource_usage::copy_source;
  cmd->barrier(count, sources, before, copying);
  Input* const targets[3] = {&set.color, set.has_motion ? &set.motion : nullptr, set.has_depth ? &set.depth : nullptr};
  for (Input* t : targets)
    if (t != nullptr) ImageBarrier(list, t->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
  Copy(list, ImageOf(back_buffer), set.color);
  if (set.has_motion) Copy(list, ImageOf(motion), set.motion);
  if (set.has_depth) Copy(list, ImageOf(depth), set.depth);
  // D3D11 reads them next: general layout, all writes made available.
  for (Input* t : targets)
    if (t != nullptr) ImageBarrier(list, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
  cmd->barrier(count, sources, copying, before);
  queue->flush_immediate_command_list();
  return true;
}

// Records the copy of `back_buffer` into `to` on the queue's immediate command list, after what ReShade has recorded there so far, and sends it.
bool RecordOne(api::command_queue* queue, Input& to, api::resource back_buffer, api::resource_usage state) {
  if (!Fit(to, g->device->get_resource_desc(back_buffer))) return false;
  api::command_list* const cmd = queue->get_immediate_command_list();
  if (cmd == nullptr) return false;
  const VkCommandBuffer list = reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(cmd->get_native()));
  cmd->barrier(back_buffer, state, api::resource_usage::copy_source);
  ImageBarrier(list, to.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
  Copy(list, ImageOf(back_buffer), to);
  ImageBarrier(list, to.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
  cmd->barrier(back_buffer, api::resource_usage::copy_source, state);
  queue->flush_immediate_command_list();
  return true;
}

// The set Bridge writes next, if it is free now (the same test Bridge makes).
Set* NextSet() {
  Set& set = g->sets[g->next_id % SETS];
  if (!FreeOnD3d(set)) return nullptr;
  if (!g->same_frame && set.vk_id != 0 && !set.handed) return nullptr;
  return &set;
}

// Same frame: the game's queue signals the shared semaphore with `value` once everything sent so far has run.
bool SignalShared(api::command_queue* queue, uint64_t value) {
  VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
  timeline.signalSemaphoreValueCount = 1;
  timeline.pSignalSemaphoreValues = &value;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.pNext = &timeline;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &g->semaphore;
  const VkQueue native = reinterpret_cast<VkQueue>(static_cast<uintptr_t>(queue->get_native()));
  return g->vk.QueueSubmit(native, 1, &submit, VK_NULL_HANDLE) == VK_SUCCESS;
}

// Same frame: a shared D3D11 fence, imported into the game's device as a timeline semaphore. False (and nothing kept) if any step fails.
bool MakeShared() {
  ComPtr<ID3D11Fence> fence;
  HANDLE handle = nullptr;
  if (FAILED(g->device11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))) ||
      FAILED(fence->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle)))
    return false;
  struct CloseOnExit {
    HANDLE h;
    ~CloseOnExit() { CloseHandle(h); }  // the import does not take an NT handle over
  } close_on_exit{handle};
  VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
  type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
  type.initialValue = 0;
  VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  info.pNext = &type;
  VkSemaphore semaphore = VK_NULL_HANDLE;
  if (g->vk.CreateSemaphore(g->vk_device, &info, nullptr, &semaphore) != VK_SUCCESS) return false;
  VkImportSemaphoreWin32HandleInfoKHR import{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
  import.semaphore = semaphore;
  import.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;  // D3D11 fences too (also named D3D11_FENCE_BIT)
  import.handle = handle;
  if (g->vk.ImportSemaphoreWin32HandleKHR(g->vk_device, &import) != VK_SUCCESS) {
    g->vk.DestroySemaphore(g->vk_device, semaphore, nullptr);
    return false;
  }
  g->shared_fence = fence;
  g->semaphore = semaphore;
  return true;
}

// Same frame, safety net. `set` is busy on D3D11's side. If that is because the game's GPU never signalled it (a lost or reset
// device, a submit that never ran), D3D11 would wait for ever and frame generation would freeze. After STUCK_MS HSF signals the
// semaphore from the CPU, so D3D11 goes on, and the bridge drops to the one-frame-behind route, which needs no shared semaphore.
void Unstick(const Set& set) {
  if (g->shared_fence->GetCompletedValue() >= set.vk_id) {
    g->stuck_since = 0;  // Vulkan's side is done: D3D11 is only slow
    return;
  }
  const ULONGLONG now = GetTickCount64();
  if (g->stuck_since == 0) {
    g->stuck_since = now;
    return;
  }
  if (now - g->stuck_since < STUCK_MS) return;
  g->stuck_since = 0;
  g->same_frame = false;
  for (Set& s : g->sets) s.handed = true;  // nothing from the same-frame route is waiting to be handed over
  bool freed = false;
  if (g->vk.SignalSemaphore != nullptr) {
    VkSemaphoreSignalInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
    signal.semaphore = g->semaphore;
    signal.value = g->next_id - 1;  // the highest value D3D11 was told to wait for
    freed = g->vk.SignalSemaphore(g->vk_device, &signal) == VK_SUCCESS;
  }
  Problem(freed ? "the game's GPU did not signal the shared fence for 2 s; HSF let D3D11 go on, frames now one frame behind"
                : "the game's GPU did not signal the shared fence for 2 s and HSF could not signal it; frame generation may stay frozen");
}

}  // namespace

DXGI_FORMAT ToDxgi(api::format format) {
  switch (format) {
    case api::format::b8g8r8a8_typeless:
    case api::format::b8g8r8a8_unorm:
    case api::format::b8g8r8a8_unorm_srgb: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case api::format::r8g8b8a8_typeless:
    case api::format::r8g8b8a8_unorm:
    case api::format::r8g8b8a8_unorm_srgb: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case api::format::r10g10b10a2_typeless:
    case api::format::r10g10b10a2_unorm: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case api::format::r16g16b16a16_typeless:
    case api::format::r16g16b16a16_float: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case api::format::r16g16_float: return DXGI_FORMAT_R16G16_FLOAT;
    case api::format::r32g32_float: return DXGI_FORMAT_R32G32_FLOAT;
    case api::format::r32_float: return DXGI_FORMAT_R32_FLOAT;
    default: return DXGI_FORMAT_UNKNOWN;
  }
}

// Same frame if the add-on's vkCreateDevice hook (vk_hook.h) or HSF's Vulkan layer gave this device shared semaphores (bit 1). The hook
// learns that only once vkCreateDevice returned, after ReShade's init_device, so a Start made there asks again on the next one.
// Also when ReShade reports shared fences with NT handles on the device: another add-on (GITC Uplift) or the game turned the extension on,
// for example while RTSS keeps HSF's hook out.
void TrySameFrame() {
  if (g->same_frame) return;
  int flags = HookFlags(g->vk_device);
  if (g->device->check_capability(api::device_caps::shared_fence_nt_handle)) flags |= 1;
  const HMODULE layer = GetModuleHandleW(L"hsf_vk_layer64.dll");
  using AskLayer = int (*)(VkDevice);
  const auto ask = layer != nullptr ? reinterpret_cast<AskLayer>(GetProcAddress(layer, "HsfVkLayerDevice")) : nullptr;
  if (ask != nullptr) flags |= ask(g->vk_device);
  if ((flags & 1) != 0 && g->vk.CreateSemaphore != nullptr && g->vk.DestroySemaphore != nullptr &&
      g->vk.ImportSemaphoreWin32HandleKHR != nullptr && g->vk.QueueSubmit != nullptr)
    g->same_frame = MakeShared();
}

bool Start(api::device* device, char* error, size_t error_size) {
  error[0] = '\0';
  if (g != nullptr && g->device == device) {
    TrySameFrame();
    return true;
  }
  Stop();
  const auto fail = [&](const char* what) {
    std::snprintf(error, error_size, "Vulkan bridge: %s", what);
    Stop();
    return false;
  };
  g = new State();
  g->device = device;
  g->vk_device = reinterpret_cast<VkDevice>(static_cast<uintptr_t>(device->get_native()));

  const HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
  const auto get = loader != nullptr ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(loader, "vkGetDeviceProcAddr")) : nullptr;
  if (get == nullptr) return fail("vulkan-1.dll's vkGetDeviceProcAddr was not found");
#define LOAD(name) g->vk.name = reinterpret_cast<PFN_vk##name>(get(g->vk_device, "vk" #name));
  LOAD(CreateImage) LOAD(DestroyImage) LOAD(GetImageMemoryRequirements) LOAD(AllocateMemory) LOAD(FreeMemory) LOAD(BindImageMemory)
  LOAD(GetMemoryWin32HandlePropertiesKHR) LOAD(CmdPipelineBarrier) LOAD(CmdCopyImage)
  LOAD(CreateSemaphore) LOAD(DestroySemaphore) LOAD(ImportSemaphoreWin32HandleKHR) LOAD(QueueSubmit)
#undef LOAD
  g->vk.SignalSemaphore = reinterpret_cast<PFN_vkSignalSemaphore>(get(g->vk_device, "vkSignalSemaphore"));
  if (g->vk.SignalSemaphore == nullptr) g->vk.SignalSemaphore = reinterpret_cast<PFN_vkSignalSemaphore>(get(g->vk_device, "vkSignalSemaphoreKHR"));
  if (g->vk.CreateImage == nullptr || g->vk.AllocateMemory == nullptr || g->vk.CmdCopyImage == nullptr || g->vk.CmdPipelineBarrier == nullptr)
    return fail("the game's Vulkan device has no copy functions");

  // HSF's D3D11 device on the same GPU: the DXGI adapter with the Vulkan device's vendor and device IDs.
  uint32_t vendor = 0, device_id = 0;
  device->get_property(api::device_properties::vendor_id, &vendor);
  device->get_property(api::device_properties::device_id, &device_id);
  ComPtr<IDXGIFactory1> factory;
  if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return fail("no DXGI factory");
  ComPtr<IDXGIAdapter1> adapter;
  for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter->GetDesc1(&desc)) && desc.VendorId == vendor && desc.DeviceId == device_id) break;
    adapter.Reset();
  }
  if (adapter == nullptr) return fail("the game's GPU was not found among the DXGI adapters");
  ComPtr<ID3D11Device> device11;
  ComPtr<ID3D11DeviceContext> context;
  const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_1;
  if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &level, 1, D3D11_SDK_VERSION, &device11,
                               nullptr, &context)) ||
      FAILED(device11.As(&g->device11)) || FAILED(context.As(&g->context11)))
    return fail("HSF's D3D11 device (11.1 with fences) could not be made");
  if (FAILED(g->device11->CreateFence(0, D3D11_FENCE_FLAG_NONE, IID_PPV_ARGS(&g->d3d_done)))) return fail("HSF's D3D11 fence could not be made");
  if (!device->create_fence(0, api::fence_flags::none, &g->fence)) return fail("ReShade could not make a fence on the game's device");

  TrySameFrame();
  return true;
}

void Stop(bool release_vulkan) {
  if (g == nullptr) return;
  if (release_vulkan && g->vk_device != VK_NULL_HANDLE && g->vk.DestroyImage != nullptr)
    for (Set& set : g->sets)
      for (Input* input : {&set.color, &set.motion, &set.depth, &set.hudless}) Release(*input);
  if (release_vulkan && g->fence.handle != 0) g->device->destroy_fence(g->fence);
  if (release_vulkan && g->semaphore != VK_NULL_HANDLE) g->vk.DestroySemaphore(g->vk_device, g->semaphore, nullptr);
  delete g;
  g = nullptr;
}

ID3D11Device* Device() { return g != nullptr ? g->device11.Get() : nullptr; }
bool Follows(api::device* device) { return g != nullptr && g->device == device; }
bool SameFrame() { return g != nullptr && g->same_frame; }

void BridgeHudless(api::command_queue* queue, api::resource back_buffer, api::resource_usage back_buffer_state) {
  if (g == nullptr || queue == nullptr || back_buffer.handle == 0) return;
  Set* const set = NextSet();
  if (set == nullptr) return;
  set->hudless_id = RecordOne(queue, set->hudless, back_buffer, back_buffer_state) ? g->next_id : 0;
}

Frame Bridge(api::command_queue* queue, api::resource back_buffer, api::resource_usage back_buffer_state, api::resource motion, api::resource depth) {
  Frame out;
  if (g == nullptr || queue == nullptr || back_buffer.handle == 0) return out;
  if (g->pending_hand) Handed(Frame{nullptr, nullptr, nullptr, 0});  // the last one was never confirmed: let it go anyway

  if (g->same_frame) {
    // This frame's copies into a set D3D11 is done with; D3D11 waits for them on the GPU and the set goes over now.
    const uint64_t id = g->next_id;
    Set& set = g->sets[id % SETS];
    if (!FreeOnD3d(set)) {
      ++g->stats.busy;
      Unstick(set);
      return out;
    }
    g->stuck_since = 0;
    if (!Record(queue, set, back_buffer, back_buffer_state, motion, depth)) return out;
    if (!SignalShared(queue, id)) {
      Problem("the game's queue could not signal the shared fence");
      return out;
    }
    g->context11->Wait(g->shared_fence.Get(), id);
    ++g->next_id;
    ++g->stats.frames;
    set.vk_id = id;
    set.handed = true;
    set.d3d_id = g->d3d_next++;
    g->pending_hand = true;
    g->pending_d3d = set.d3d_id;
    out.color = set.color.texture.Get();
    out.motion = set.has_motion ? set.motion.texture.Get() : nullptr;
    out.depth = set.has_depth ? set.depth.texture.Get() : nullptr;
    out.hudless = set.hudless_id == id ? set.hudless.texture.Get() : nullptr;
    out.id = set.d3d_id;
    ++g->stats.handed;
    return out;
  }

  // 1. Queue this frame's copies into a set that nobody uses now, and wait on the CPU (up to CPU_WAIT_NS) until the game's GPU finished them,
  // so this very frame goes over. The game thread pays the wait out of Reflex's sleep before the next frame. Too late: it goes next time.
  {
    const uint64_t id = g->next_id;
    Set& set = g->sets[id % SETS];
    if ((set.vk_id != 0 && !set.handed) || !FreeOnD3d(set)) {
      ++g->stats.busy;
    } else if (Record(queue, set, back_buffer, back_buffer_state, motion, depth)) {
      if (!queue->signal(g->fence, id)) {
        Problem("the game's queue could not signal ReShade's fence");
      } else {
        set.vk_id = id;
        set.handed = false;
        set.d3d_id = 0;
        ++g->next_id;
        ++g->stats.frames;
        LARGE_INTEGER start, end, rate;
        QueryPerformanceCounter(&start);
        const bool in_time = g->device->wait(g->fence, id, CPU_WAIT_NS);
        QueryPerformanceCounter(&end);
        QueryPerformanceFrequency(&rate);
        g->stats.wait_ms += static_cast<float>(static_cast<double>(end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(rate.QuadPart));
        ++g->stats.waits;
        if (!in_time) ++g->stats.late;
      }
    }
  }

  // 2. Hand over the newest set the game's GPU has finished (this frame's, if the wait made it); older finished ones are dropped.
  const uint64_t done = g->device->get_completed_fence_value(g->fence);
  Set* newest = nullptr;
  for (Set& set : g->sets)
    if (set.vk_id != 0 && !set.handed && set.vk_id <= done && (newest == nullptr || set.vk_id > newest->vk_id)) newest = &set;
  if (newest != nullptr) {
    for (Set& set : g->sets)
      if (&set != newest && set.vk_id != 0 && !set.handed && set.vk_id <= done) set.handed = true;
    newest->handed = true;
    newest->d3d_id = g->d3d_next++;
    g->pending_hand = true;
    g->pending_d3d = newest->d3d_id;
    out.color = newest->color.texture.Get();
    out.motion = newest->has_motion ? newest->motion.texture.Get() : nullptr;
    out.depth = newest->has_depth ? newest->depth.texture.Get() : nullptr;
    out.hudless = newest->hudless_id == newest->vk_id ? newest->hudless.texture.Get() : nullptr;
    out.id = newest->d3d_id;
    ++g->stats.handed;
  }
  return out;
}

void Handed(const Frame&) {
  if (g == nullptr || !g->pending_hand) return;
  g->context11->Signal(g->d3d_done.Get(), g->pending_d3d);
  g->context11->Flush();
  g->pending_hand = false;
}

Stats TakeStats() {
  Stats s;
  if (g == nullptr) return s;
  s = g->stats;
  s.problem = g->problem;
  g->stats = Stats{};
  return s;
}

}  // namespace hsf::vk
