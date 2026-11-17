// The game's DLSS data on Vulkan. A mod that runs DLSS on a Vulkan game (GITC Uplift in the Kyty emulator) hands the driver's _nvngx.dll the vectors and
// depth every frame through NGX's Vulkan calls. HSF hooks those calls the same way as on Direct3D 12 (ngx_input.h), always calls through, and copies the
// part in use of the vectors and depth on the same command buffer just before each call goes on, into images of its own on the game's device. The Vulkan bridge
// (vk_bridge.h) takes them over to HSF's helper in place of the flow.
#pragma once

#include <reshade.hpp>

#include <cstdint>
#include <vector>

#include "ngx_input.h"

namespace hsf::ngx {

void StartVk(LogFunction log);
// Often, from the thread that presents, with ReShade's device for the game's Vulkan device: hooks _nvngx.dll once it is loaded.
void PollVk(reshade::api::device* device);
// release: destroy HSF's images. False at ReShade's destroy_device, where the device is already out of ReShade's table (the images go with the device).
void StopVk(bool release);
// Removes the Vulkan NGX hooks (only those): before HSF unloads in a running game.
void UnhookVk();

// What the last DLSS call gave, as HSF's copies on the game's device: in shader_resource state between calls, the size of the part in use.
struct FrameVk {
  reshade::api::resource motion{0};
  reshade::api::resource depth{0};  // {0}: this depth format is not copied (the effect's depth is used instead)
  uint32_t width = 0, height = 0;   // the vectors' copy
  float to_pixels[2] = {};          // NGX's MV.Scale: a vector times this is in pixels of the part in use
  float jitter[2] = {};             // in pixels of the render size
  bool jittered = false;            // the vectors include the jitter
  int depth_inverted = -1;          // from DLSS's flags: 1 reversed, 0 normal, -1 not known
  bool reset = false;
  uint64_t serial = 0;              // goes up with each call taken
};
// The newest call whose command buffer has gone to a queue (or, when ReShade reports no submits, the newest recorded). False until there is one.
// The images stay alive for at least 16 more calls.
bool LatestVk(FrameVk* out);
// ReShade's execute_command_list: marks a call's copy as submitted.
void OnSubmitVk(reshade::api::command_queue* queue, reshade::api::command_list* cmd);
// The native VkQueue the last call's command buffer went to, nullptr before one.
void* SubmitQueueVk();
// For the frame-time log: the time between the DLSS calls taken, and between their command buffers going to a queue, in ms, since the last call.
// Both lists are moved out and start again.
void TakeTimesVk(std::vector<float>& calls, std::vector<float>& submits);
// How many of the Vulkan calls got hooked and how many _nvngx.dll has, and whether _nvngx.dll has been found yet.
int HookedCallsVk();
int FoundCallsVk();
bool LoadedVk();
void RescanVk();

}  // namespace hsf::ngx
