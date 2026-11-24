// See ngx_vk.h.
#define VK_NO_PROTOTYPES
#include "ngx_vk.h"

#include <windows.h>

#include <vulkan/vulkan.h>

#include <intrin.h>

#include <cstdarg>
#include <cstdio>
#include <vector>

#include "hook_patch.h"
#include "ngx_call.h"

namespace hsf::ngx {
namespace {

namespace api = reshade::api;

using call::NGX_SUCCESS;

// NGX's Vulkan resource, laid out as nvsdk_ngx_defs_vk.h has it (NVSDK_NGX_Resource_VK).
struct ImageViewInfoVk {
  VkImageView view;
  VkImage image;
  VkImageSubresourceRange range;
  VkFormat format;
  unsigned int width, height;
};
struct BufferInfoVk {
  VkBuffer buffer;
  unsigned int size;
};
struct ResourceVk {
  union {
    ImageViewInfoVk image;
    BufferInfoVk buffer;
  } resource;
  int type;  // 0 image view, 1 buffer
  bool read_write;
};
constexpr int RESOURCE_IMAGE_VIEW = 0;
// NGX's rule for what it reads, as Streamline sets it before its own call (ResourceState::eTextureRead).
constexpr VkImageLayout GAME_LAYOUT = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

using EvaluateFn = int(__cdecl*)(VkCommandBuffer, const void*, const Parameter*, void*);
using CreateFn = int(__cdecl*)(VkCommandBuffer, int, const Parameter*, void**);
using Create1Fn = int(__cdecl*)(VkDevice, VkCommandBuffer, int, const Parameter*, void**);
EvaluateFn g_real_evaluate = nullptr, g_real_evaluate_c = nullptr;
CreateFn g_real_create = nullptr;
Create1Fn g_real_create1 = nullptr;

struct Functions {
  PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
  PFN_vkCmdCopyImage CmdCopyImage = nullptr;
  PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
  PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
};

// HSF's copies on the game's device, made through ReShade so the bridge can copy from them with ReShade's command lists.
struct Copies {
  api::resource motion{0}, depth{0}, staging{0};  // staging: a buffer for depth formats that cannot be copied to a colour image directly
  uint32_t width = 0, height = 0, depth_width = 0, depth_height = 0;
  VkFormat motion_format = VK_FORMAT_UNDEFINED, depth_format = VK_FORMAT_UNDEFINED;
};
struct OldCopies {
  Copies copies;
  uint64_t serial;
};

SRWLOCK g_lock = SRWLOCK_INIT;  // guards everything from here to the hooks
LogFunction g_log = nullptr;
api::device* g_device = nullptr;
Functions g_vk;
// Each call writes the next copy of a ring, so a copy the bridge reads is not written again for RING - 1 more calls. In an emulator the
// command buffers with the calls are recorded on one thread and the frames presented on another, often a frame apart.
constexpr int RING = 3;
Copies g_ring[RING];
bool g_written[RING] = {};  // the copy holds a call's data and sits in SHADER_READ_ONLY_OPTIMAL
int g_write = 0;
std::vector<OldCopies> g_old;  // still read by frames on the GPU when replaced
FrameVk g_frame;                // the last call recorded
bool g_have_frame = false;
bool g_logged_first = false;  // the first call taken is logged once, whether or not it worked
// A call's copy is handed over only once its command buffer has gone to a queue (ReShade's execute_command_list): a bridge copy submitted
// after it on the same queue then reads it whole.
struct Pending {
  VkCommandBuffer cmd;
  FrameVk frame;
};
std::vector<Pending> g_pending;
FrameVk g_submitted;
bool g_have_submitted = false;
void* g_submit_queue = nullptr;  // the queue the last call's command buffer went to
constexpr uint64_t NO_SUBMITS_AFTER = 30;  // calls recorded with no submit seen: ReShade does not report submits, hand over the last call recorded
uint64_t g_serial = 0;
uint64_t g_calls = 0;  // every upscaler call that reached MainCall, taken or not: its clock. g_serial counts only the taken ones and stops when none are
call::Created g_created;  // from the last CreateFeature for DLSS
call::MainCall g_main;
int g_refused_motion = 0, g_refused_depth = 0;
bool g_tried = false;
hookpatch::HookSet g_hooks;  // NGX's Vulkan calls: removed by UnhookVk only
int g_hooked = 0, g_found = 0;
// TakeTimesVk: when the last call was taken and its command buffer went to a queue, and the times between them since the last read.
LARGE_INTEGER g_freq{};
LONGLONG g_last_call = 0, g_last_submit = 0;
std::vector<float> g_call_ms, g_submit_ms;

// The time from `last` to now in ms into `into` (at most a few thousand kept between reads), and now into `last`.
void Stamp(LONGLONG& last, std::vector<float>& into) {
  if (g_freq.QuadPart == 0) QueryPerformanceFrequency(&g_freq);
  LARGE_INTEGER now;
  QueryPerformanceCounter(&now);
  if (last != 0 && into.size() < 4000) into.push_back(static_cast<float>(static_cast<double>(now.QuadPart - last) * 1000.0 / static_cast<double>(g_freq.QuadPart)));
  last = now.QuadPart;
}

struct Lock {
  Lock() { AcquireSRWLockExclusive(&g_lock); }
  ~Lock() { ReleaseSRWLockExclusive(&g_lock); }
};

void Line(const char* format, ...) {
  if (g_log == nullptr) return;
  char text[500];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  g_log(text);
}

const ImageViewInfoVk* Image(const Parameter* parameters, const char* name) {
  void* pointer = nullptr;
  if (parameters->Get(name, &pointer) != NGX_SUCCESS || pointer == nullptr) return nullptr;
  const ResourceVk* const resource = static_cast<const ResourceVk*>(pointer);
  if (resource->type != RESOURCE_IMAGE_VIEW || resource->resource.image.image == VK_NULL_HANDLE) return nullptr;
  return &resource->resource.image;
}

// The vectors' formats the bridge takes (vk_bridge.cpp ToDxgi).
api::format MotionFormat(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R16G16_SFLOAT: return api::format::r16g16_float;
    case VK_FORMAT_R32G32_SFLOAT: return api::format::r32g32_float;
    default: return api::format::unknown;
  }
}
// Depth: 1 copied straight (a colour R32 image), 2 through a buffer (32-bit float depth, whose depth plane comes out as R32 floats), 0 not copied.
int DepthRoute(VkFormat format) {
  switch (format) {
    case VK_FORMAT_R32_SFLOAT: return 1;
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT: return 2;
    default: return 0;
  }
}
VkImageAspectFlags DepthAspects(VkFormat format) {
  if (format == VK_FORMAT_D32_SFLOAT_S8_UINT) return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  if (format == VK_FORMAT_D32_SFLOAT) return VK_IMAGE_ASPECT_DEPTH_BIT;
  return VK_IMAGE_ASPECT_COLOR_BIT;
}

void Release(Copies& c) {
  if (g_device == nullptr) return;
  if (c.motion.handle != 0) g_device->destroy_resource(c.motion);
  if (c.depth.handle != 0) g_device->destroy_resource(c.depth);
  if (c.staging.handle != 0) g_device->destroy_resource(c.staging);
  c = Copies{};
}

// quiet: the ring's other copies, made the same way, are not logged again.
bool MakeCopies(Copies& out, uint32_t width, uint32_t height, VkFormat motion_format, uint32_t depth_width, uint32_t depth_height, VkFormat depth_format, bool quiet) {
  const api::resource_usage usage = api::resource_usage::shader_resource | api::resource_usage::copy_dest | api::resource_usage::copy_source;
  // Made in no layout: each call's copy overwrites the whole image and leaves it readable.
  if (!g_device->create_resource(api::resource_desc(width, height, 1, 1, MotionFormat(motion_format), 1, api::memory_heap::gpu_only, usage), nullptr,
                                 api::resource_usage::undefined, &out.motion)) {
    Line("Vulkan DLSS: the vectors' copy (%ux%u, VkFormat %d) could not be made", width, height, static_cast<int>(motion_format));
    return false;
  }
  out.width = width;
  out.height = height;
  out.motion_format = motion_format;
  const int route = DepthRoute(depth_format);
  if (route != 0 && depth_width != 0 && depth_height != 0) {
    bool made = g_device->create_resource(api::resource_desc(depth_width, depth_height, 1, 1, api::format::r32_float, 1, api::memory_heap::gpu_only, usage), nullptr,
                                          api::resource_usage::undefined, &out.depth);
    if (made && route == 2)
      made = g_device->create_resource(api::resource_desc(static_cast<uint64_t>(depth_width) * depth_height * 4, api::memory_heap::gpu_only,
                                                          api::resource_usage::copy_dest | api::resource_usage::copy_source),
                                       nullptr, api::resource_usage::undefined, &out.staging);
    if (made) {
      out.depth_width = depth_width;
      out.depth_height = depth_height;
      out.depth_format = depth_format;
    } else {
      Line("Vulkan DLSS: the depth copy (%ux%u, VkFormat %d) could not be made, the effect's depth is used", depth_width, depth_height, static_cast<int>(depth_format));
      if (out.depth.handle != 0) g_device->destroy_resource(out.depth);
      if (out.staging.handle != 0) g_device->destroy_resource(out.staging);
      out.depth = out.staging = {0};
    }
  }
  if (!quiet)
    Line("Vulkan DLSS: copying vectors %ux%u (VkFormat %d) and %s after each call, into a ring of %d", width, height, static_cast<int>(motion_format),
         out.depth.handle != 0 ? (route == 2 ? "depth through a buffer" : "depth") : "no depth", RING);
  return true;
}

VkImageMemoryBarrier ImageBarrier(VkImage image, VkImageAspectFlags aspects, uint32_t level, uint32_t layer, VkImageLayout from, VkImageLayout to,
                                  VkAccessFlags from_access, VkAccessFlags to_access) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = from_access;
  b.dstAccessMask = to_access;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {aspects, level, 1, layer, 1};
  return b;
}

// The part in use of the vectors and depth into one of HSF's copies, with the game's images back in NGX's layout. A copy written before
// comes from SHADER_READ_ONLY_OPTIMAL, not UNDEFINED: a transition from UNDEFINED may drop what a late reader still finds there.
void Record(VkCommandBuffer cmd, const Copies& copies, bool copy_written, const ImageViewInfoVk& motion, uint32_t mx, uint32_t my, const ImageViewInfoVk* depth, uint32_t dx,
            uint32_t dy) {
  const VkImage motion_copy = reinterpret_cast<VkImage>(copies.motion.handle);
  const VkImage depth_copy = reinterpret_cast<VkImage>(copies.depth.handle);
  const bool with_depth = depth != nullptr && depth_copy != VK_NULL_HANDLE;
  const VkImageAspectFlags depth_aspects = with_depth ? DepthAspects(depth->format) : 0;
  const uint32_t ml = motion.range.baseMipLevel, ma = motion.range.baseArrayLayer;
  const VkImageLayout copy_from = copy_written ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
  const VkAccessFlags copy_read = copy_written ? VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT : 0;
  VkImageMemoryBarrier before[4];
  uint32_t count = 0;
  before[count++] = ImageBarrier(motion.image, VK_IMAGE_ASPECT_COLOR_BIT, ml, ma, GAME_LAYOUT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
                                 VK_ACCESS_TRANSFER_READ_BIT);
  before[count++] = ImageBarrier(motion_copy, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, copy_from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copy_read, VK_ACCESS_TRANSFER_WRITE_BIT);
  if (with_depth) {
    before[count++] = ImageBarrier(depth->image, depth_aspects, depth->range.baseMipLevel, depth->range.baseArrayLayer, GAME_LAYOUT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                   VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    before[count++] = ImageBarrier(depth_copy, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, copy_from, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copy_read, VK_ACCESS_TRANSFER_WRITE_BIT);
  }
  g_vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, count, before);

  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, ml, ma, 1};
  region.srcOffset = {static_cast<int32_t>(mx), static_cast<int32_t>(my), 0};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent = {copies.width, copies.height, 1};
  g_vk.CmdCopyImage(cmd, motion.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, motion_copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
  if (with_depth) {
    const uint32_t dl = depth->range.baseMipLevel, da = depth->range.baseArrayLayer;
    if (DepthRoute(depth->format) == 1) {
      region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, dl, da, 1};
      region.srcOffset = {static_cast<int32_t>(dx), static_cast<int32_t>(dy), 0};
      region.extent = {copies.depth_width, copies.depth_height, 1};
      g_vk.CmdCopyImage(cmd, depth->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth_copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    } else {
      const VkBuffer staging = reinterpret_cast<VkBuffer>(copies.staging.handle);
      VkBufferImageCopy out{};
      out.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, dl, da, 1};
      out.imageOffset = {static_cast<int32_t>(dx), static_cast<int32_t>(dy), 0};
      out.imageExtent = {copies.depth_width, copies.depth_height, 1};
      g_vk.CmdCopyImageToBuffer(cmd, depth->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &out);
      VkBufferMemoryBarrier written{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
      written.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      written.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
      written.srcQueueFamilyIndex = written.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      written.buffer = staging;
      written.size = VK_WHOLE_SIZE;
      g_vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 1, &written, 0, nullptr);
      VkBufferImageCopy in{};
      in.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      in.imageExtent = {copies.depth_width, copies.depth_height, 1};
      g_vk.CmdCopyBufferToImage(cmd, staging, depth_copy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &in);
    }
  }

  VkImageMemoryBarrier after[4];
  count = 0;
  after[count++] = ImageBarrier(motion.image, VK_IMAGE_ASPECT_COLOR_BIT, ml, ma, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, GAME_LAYOUT, VK_ACCESS_TRANSFER_READ_BIT,
                                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  after[count++] = ImageBarrier(motion_copy, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
  if (with_depth) {
    after[count++] = ImageBarrier(depth->image, depth_aspects, depth->range.baseMipLevel, depth->range.baseArrayLayer, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, GAME_LAYOUT,
                                  VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    after[count++] = ImageBarrier(depth_copy, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT);
  }
  g_vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, count, after);
}

// Before the call goes on (see HookEvaluate): the part in use of the vectors and depth copied on its command buffer, and what HSF reads of the call
// in `out`. False when the call is not taken: nothing is recorded then. Logs every change of what is copied.
bool Take(VkCommandBuffer cmd, const Parameter* parameters, FrameVk& out) {
  if (cmd == VK_NULL_HANDLE || parameters == nullptr) return false;
  // Only the upscalers have vectors under this name, so frame generation or anything else on this call never gets here.
  const ImageViewInfoVk* const motion = Image(parameters, "MotionVectors");
  if (motion == nullptr) return false;
  const ImageViewInfoVk* depth = Image(parameters, "Depth");

  Lock lock;
  if (g_device == nullptr || g_vk.CmdCopyImage == nullptr) return false;
  if (MotionFormat(motion->format) == api::format::unknown) {
    if (g_refused_motion != motion->format) Line("Vulkan DLSS: vectors in VkFormat %d are not taken, so this game stays on flow", static_cast<int>(motion->format));
    g_refused_motion = motion->format;
    return false;
  }
  if (depth != nullptr && DepthRoute(depth->format) == 0) {
    if (g_refused_depth != depth->format) Line("Vulkan DLSS: depth in VkFormat %d is not copied, the effect's depth is used", static_cast<int>(depth->format));
    g_refused_depth = depth->format;
    depth = nullptr;
  }

  const call::DlssCall c = call::Read(parameters, g_created, motion->width, motion->height);
  const uint32_t width = c.width, height = c.height, mx = c.x, my = c.y;
  call::DepthPart d;
  if (depth != nullptr) d = call::ReadDepthPart(parameters, c, depth->width, depth->height);
  const uint32_t depth_width = d.width, depth_height = d.height, dx = d.x, dy = d.y;
  if (!g_main.Take(c.Area(), ++g_calls)) return false;

  const VkFormat depth_format = depth != nullptr ? depth->format : VK_FORMAT_UNDEFINED;
  const Copies& now = g_ring[0];
  if (now.motion.handle == 0 || now.width != width || now.height != height || now.motion_format != motion->format || (now.depth.handle != 0) != (depth != nullptr) ||
      now.depth_width != depth_width || now.depth_height != depth_height || now.depth_format != depth_format) {
    Copies made[RING];
    bool all = true;
    for (int i = 0; i < RING && all; ++i) all = MakeCopies(made[i], width, height, motion->format, depth_width, depth_height, depth_format, i != 0);
    if (!all) {
      for (Copies& m : made) Release(m);
      return false;
    }
    for (int i = 0; i < RING; ++i) {
      if (g_ring[i].motion.handle != 0) g_old.push_back({g_ring[i], g_serial});
      g_ring[i] = made[i];
      g_written[i] = false;
    }
    g_write = 0;
  }
  while (!g_old.empty() && g_serial - g_old.front().serial > 16) {
    Release(g_old.front().copies);
    g_old.erase(g_old.begin());
  }
  const int slot = g_write;
  g_write = (g_write + 1) % RING;
  const Copies& copies = g_ring[slot];
  if (copies.depth.handle == 0) depth = nullptr;

  Record(cmd, copies, g_written[slot], *motion, mx, my, depth, dx, dy);
  g_written[slot] = true;

  FrameVk f;
  f.motion = copies.motion;
  f.depth = depth != nullptr ? copies.depth : api::resource{0};
  f.width = width;
  f.height = height;
  f.to_pixels[0] = c.to_pixels[0];
  f.to_pixels[1] = c.to_pixels[1];
  f.jitter[0] = c.jitter[0];
  f.jitter[1] = c.jitter[1];
  f.jittered = c.jittered;
  f.depth_inverted = c.depth_inverted;
  f.reset = c.reset;
  f.serial = ++g_serial;
  out = f;
  if (!g_logged_first) {
    g_logged_first = true;
    Line("Vulkan DLSS: first call on thread %lu: vectors %ux%u in use at %u,%u of %ux%u, MV scale %.3f %.3f, depth %s %ux%u, flags %s0x%x, image layout taken as "
         "SHADER_READ_ONLY_OPTIMAL, copied before the call goes on",
         GetCurrentThreadId(), width, height, mx, my, motion->width, motion->height, f.to_pixels[0], f.to_pixels[1], depth != nullptr ? "copied" : "not copied",
         depth_width, depth_height, c.flags >= 0 ? "" : "not known, taken as low resolution ", c.flags >= 0 ? c.flags : 0);
  }
  return true;
}

// After the call worked: the copy waits for its command buffer to go to a queue.
void Publish(VkCommandBuffer cmd, const FrameVk& f) {
  Lock lock;
  if (g_have_frame && f.serial < g_frame.serial) return;  // a newer call on another thread was handed over first
  g_frame = f;
  g_have_frame = true;
  Stamp(g_last_call, g_call_ms);
  // A command buffer recorded again before it went to a queue replaces its older call.
  for (size_t i = 0; i < g_pending.size(); ++i)
    if (g_pending[i].cmd == cmd) {
      g_pending.erase(g_pending.begin() + static_cast<ptrdiff_t>(i));
      break;
    }
  if (g_pending.size() >= RING) g_pending.erase(g_pending.begin());
  g_pending.push_back({cmd, f});
}

void NoteCreate(const Parameter* parameters, int feature, int result, void* caller) {
  if (result != NGX_SUCCESS) return;
  NoteMade(feature, MADE_VULKAN, caller);  // for the menu's MOD STACK
  Lock lock;
  if (call::ReadCreate(parameters, feature, g_created))
    Line("Vulkan DLSS: made, feature %d, flags 0x%x, render %ux%u, output %ux%u", feature, g_created.flags, g_created.width, g_created.height, g_created.out_width,
         g_created.out_height);
}

int __cdecl HookCreate(VkCommandBuffer cmd, int feature, const Parameter* parameters, void** handle) {
  const int result = g_real_create(cmd, feature, parameters, handle);
  NoteCreate(parameters, feature, result, _ReturnAddress());
  return result;
}
int __cdecl HookCreate1(VkDevice device, VkCommandBuffer cmd, int feature, const Parameter* parameters, void** handle) {
  const int result = g_real_create1(device, cmd, feature, parameters, handle);
  NoteCreate(parameters, feature, result, _ReturnAddress());
  return result;
}
// The copies are recorded BEFORE the call goes on, for the same reason as on Direct3D 12 (ngx_input.cpp HookEvaluate): another mod's DLSS under
// this hook can leave the game's images in another layout, or with other contents. The copy is handed over only if the call worked.
int __cdecl HookEvaluate(VkCommandBuffer cmd, const void* handle, const Parameter* parameters, void* callback) {
  FrameVk f;
  const bool taken = Take(cmd, parameters, f);
  const int result = g_real_evaluate(cmd, handle, parameters, callback);
  if (taken && result == NGX_SUCCESS) Publish(cmd, f);
  return result;
}
int __cdecl HookEvaluateC(VkCommandBuffer cmd, const void* handle, const Parameter* parameters, void* callback) {
  FrameVk f;
  const bool taken = Take(cmd, parameters, f);
  const int result = g_real_evaluate_c(cmd, handle, parameters, callback);
  if (taken && result == NGX_SUCCESS) Publish(cmd, f);
  return result;
}

}  // namespace

void StartVk(LogFunction log) { g_log = log; }

void PollVk(api::device* device) {
  if (device == nullptr || device->get_api() != api::device_api::vulkan) return;
  if (g_device != device) {
    const HMODULE loader = GetModuleHandleW(L"vulkan-1.dll");
    const auto get = loader != nullptr ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(GetProcAddress(loader, "vkGetDeviceProcAddr")) : nullptr;
    if (get == nullptr) return;
    const VkDevice native = reinterpret_cast<VkDevice>(device->get_native());
    Functions vk;
    vk.CmdPipelineBarrier = reinterpret_cast<PFN_vkCmdPipelineBarrier>(get(native, "vkCmdPipelineBarrier"));
    vk.CmdCopyImage = reinterpret_cast<PFN_vkCmdCopyImage>(get(native, "vkCmdCopyImage"));
    vk.CmdCopyImageToBuffer = reinterpret_cast<PFN_vkCmdCopyImageToBuffer>(get(native, "vkCmdCopyImageToBuffer"));
    vk.CmdCopyBufferToImage = reinterpret_cast<PFN_vkCmdCopyBufferToImage>(get(native, "vkCmdCopyBufferToImage"));
    if (vk.CmdPipelineBarrier == nullptr || vk.CmdCopyImage == nullptr || vk.CmdCopyImageToBuffer == nullptr || vk.CmdCopyBufferToImage == nullptr) return;
    Lock lock;
    g_device = device;
    g_vk = vk;
    for (int i = 0; i < RING; ++i) {
      g_ring[i] = Copies{};
      g_written[i] = false;
    }
    g_write = 0;
    g_old.clear();
    g_pending.clear();
    g_have_submitted = false;
    g_submit_queue = nullptr;
    g_have_frame = false;
    g_logged_first = false;
  }
  if (g_tried) return;
  HMODULE module = GetModuleHandleW(L"_nvngx.dll");
  if (module == nullptr) module = GetModuleHandleW(L"nvngx.dll");
  if (module == nullptr) return;
  g_tried = true;
  HMODULE pinned = nullptr;
  wchar_t path[MAX_PATH] = L"";
  GetModuleFileNameW(module, path, MAX_PATH);
  GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(module), &pinned);
  int found = 0, hooked = 0;
  const auto hook = [&](const char* name, void* detour, void** real) {
    void* const target = reinterpret_cast<void*>(GetProcAddress(module, name));
    if (target == nullptr) return;
    ++found;
    if (g_hooks.Add(name, target, detour, real, g_log)) ++hooked;
  };
  hook("NVSDK_NGX_VULKAN_CreateFeature", reinterpret_cast<void*>(&HookCreate), reinterpret_cast<void**>(&g_real_create));
  hook("NVSDK_NGX_VULKAN_CreateFeature1", reinterpret_cast<void*>(&HookCreate1), reinterpret_cast<void**>(&g_real_create1));
  hook("NVSDK_NGX_VULKAN_EvaluateFeature", reinterpret_cast<void*>(&HookEvaluate), reinterpret_cast<void**>(&g_real_evaluate));
  hook("NVSDK_NGX_VULKAN_EvaluateFeature_C", reinterpret_cast<void*>(&HookEvaluateC), reinterpret_cast<void**>(&g_real_evaluate_c));
  g_found = found;
  g_hooked = hooked;
  Line("Vulkan DLSS: %ls found, %d of the %d Vulkan calls this driver has hooked", path, hooked, found);
}

bool LatestVk(FrameVk* out) {
  Lock lock;
  if (g_have_submitted) {
    *out = g_submitted;
    return true;
  }
  if (!g_have_frame || g_serial < NO_SUBMITS_AFTER) return false;
  *out = g_frame;
  return true;
}

void OnSubmitVk(api::command_queue* queue, api::command_list* cmd) {
  if (queue == nullptr || cmd == nullptr) return;
  const VkCommandBuffer native = reinterpret_cast<VkCommandBuffer>(cmd->get_native());
  Lock lock;
  for (size_t i = 0; i < g_pending.size(); ++i) {
    if (g_pending[i].cmd != native) continue;
    if (!g_have_submitted) Line("Vulkan DLSS: the calls' command buffers are seen going to a queue; each copy is handed over once its call was submitted");
    g_submitted = g_pending[i].frame;
    Stamp(g_last_submit, g_submit_ms);
    g_have_submitted = true;
    g_submit_queue = reinterpret_cast<void*>(queue->get_native());
    g_pending.erase(g_pending.begin(), g_pending.begin() + static_cast<ptrdiff_t>(i) + 1);
    return;
  }
}

void TakeTimesVk(std::vector<float>& calls, std::vector<float>& submits) {
  Lock lock;
  calls.swap(g_call_ms);
  submits.swap(g_submit_ms);
  g_call_ms.clear();
  g_submit_ms.clear();
}

void* SubmitQueueVk() {
  Lock lock;
  return g_submit_queue;
}

int HookedCallsVk() { return g_hooked; }
int FoundCallsVk() { return g_found; }
bool LoadedVk() { return g_tried; }

void RescanVk() {
  // Hooks already in stay in (the hook set skips them); the next poll tries the rest again, and finds _nvngx.dll if it was loaded since.
  if (g_tried && g_hooked >= g_found && g_found > 0) return;
  g_tried = false;
}

void UnhookVk() { g_hooks.RemoveAll(nullptr); }

void StopVk(bool release) {
  Lock lock;
  if (release) {
    for (OldCopies& old : g_old) Release(old.copies);
    for (Copies& c : g_ring) Release(c);
  }
  g_old.clear();
  for (int i = 0; i < RING; ++i) {
    g_ring[i] = Copies{};
    g_written[i] = false;
  }
  g_write = 0;
  g_pending.clear();
  g_have_submitted = false;
  g_submitted = FrameVk{};
  g_submit_queue = nullptr;
  g_have_frame = false;
  g_logged_first = false;
  g_frame = FrameVk{};
  g_device = nullptr;
}

}  // namespace hsf::ngx
