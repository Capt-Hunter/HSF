// Vulkan games: the bridge from the game's Vulkan device to a Direct3D 11 device of HSF's own on the same GPU, like dx9_bridge.h for DX9.
// DLSS-G's hand-off (dlssg_remote.cpp, the helper) only takes Direct3D 11 textures. HSF's D3D11 device makes shared textures (NT handles) and
// the game's Vulkan device imports them (VK_KHR_external_memory_win32, which ReShade's layer turns on). Each frame, at ReShade's present, the
// game's queue copies the finished frame, LaunchPad's flow and HSF.fx's depth copy into a free set and signals a fence of ReShade's.
// Same frame (the add-on's vkCreateDevice hook, vk_hook.h, or HSF's Vulkan layer turned on external semaphores for the device): a shared
// D3D11 fence is imported as a Vulkan timeline semaphore. The game's queue signals it after the copies, and D3D11 waits for it on the GPU
// before its own copies, so the set goes to DLSS-G in the frame it was made. No CPU waits.
// Without them (neither the hook nor the layer got them in; ReShade's layer does not turn external semaphores on): the game thread waits on
// the CPU, up to about a frame, for ReShade's fence to pass the set's copies, and the set goes over in the same frame; too late, it goes over
// at a later present once the fence passed it (one frame behind).
// Either way a set is written again only once D3D11 has finished copying it to the helper (a D3D11 fence, checked on the CPU); with no free set
// the frame is skipped.
#pragma once

#include <reshade.hpp>

#include <d3d11.h>

#include <cstddef>
#include <cstdint>

namespace hsf::vk {

// Game thread. Makes HSF's D3D11 device on the GPU of `device` (a Vulkan device of ReShade's). True if it is there (also when it already was
// for this device); false, and why in `error`, if not.
bool Start(reshade::api::device* device, char* error, size_t error_size);
// Frees everything, the D3D11 device too. The game's queue must be idle. `release_vulkan` false: the images on the game's device are left to it
// (at ReShade's destroy_device the device is already out of ReShade's table, so destroying them through its layer would crash).
void Stop(bool release_vulkan = true);
ID3D11Device* Device();  // HSF's D3D11 device, nullptr before Start
bool Follows(reshade::api::device* device);  // Start was made for `device` and Stop has not run since
bool SameFrame();        // true if frames go over in the frame they were made (HSF's Vulkan layer is in), false if one frame behind

// The textures on HSF's D3D11 device that hold this frame's (same frame) or an earlier frame's copies; color nullptr = nothing new to hand over this frame.
struct Frame {
  ID3D11Texture2D* color = nullptr;
  ID3D11Texture2D* motion = nullptr;
  ID3D11Texture2D* depth = nullptr;
  ID3D11Texture2D* hudless = nullptr;  // the same frame from before the overlays (BridgeHudless), or nullptr
  uint64_t id = 0;  // pass to Handed after dlssg::Submit
};
// Game thread, at ReShade's finish_effects (effects done, overlays not drawn yet). Copies `back_buffer` into the set Bridge writes next, so
// that set's Frame::hudless is this frame without ReShade's menu, HSF's overlay or other add-ons' overlays. Nothing if that set is not free.
void BridgeHudless(reshade::api::command_queue* queue, reshade::api::resource back_buffer, reshade::api::resource_usage back_buffer_state);
// Game thread, at ReShade's present (the frame is finished, overlays drawn). Queues this frame's copies of `back_buffer`, `motion` and `depth`
// (each a resource of ReShade's on `queue`'s device, in the states given; motion and depth may be {0}) and hands back this frame's set (same
// frame: HSF's D3D11 device waits for it on the GPU) or the newest earlier set the GPU has finished. A size or format change makes that input's textures again.
Frame Bridge(reshade::api::command_queue* queue, reshade::api::resource back_buffer, reshade::api::resource_usage back_buffer_state,
             reshade::api::resource motion, reshade::api::resource depth);
// Game thread, right after dlssg::Submit took `frame` (it queued its copies on HSF's D3D11 device): the set may be written again once they ran.
void Handed(const Frame& frame);

// The DXGI format a Vulkan copy of `format` goes into (typeless back buffers become UNORM), or DXGI_FORMAT_UNKNOWN if the bridge does not take it.
DXGI_FORMAT ToDxgi(reshade::api::format format);

// For the log, since the last call.
struct Stats {
  uint32_t frames = 0;    // sets queued on the game's GPU
  uint32_t handed = 0;    // sets handed to D3D11
  uint32_t busy = 0;      // frames skipped: no free set
  uint32_t failed = 0;    // frames not bridged (an import or copy could not be made)
  uint32_t waits = 0, late = 0;  // no shared semaphores: CPU waits for a frame's copies, and those that ran out of time (the frame went one behind)
  float wait_ms = 0;      // all those waits together
  const char* problem = "";  // the last thing that went wrong, or ""
};
Stats TakeStats();

}  // namespace hsf::vk
