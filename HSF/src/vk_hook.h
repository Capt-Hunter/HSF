// Vulkan games: the add-on's hook on vulkan-1.dll's vkCreateDevice. It adds what HSF's Vulkan bridge needs for the same-frame hand-off
// (shared semaphores with timeline, shared memory; see vk_extend.h) to the game's request, so no Vulkan layer has to be installed.
// ReShade loads add-ons inside the game's vkCreateInstance, so the hook is in before the game makes its device. Calls to vkCreateDevice
// through vkGetInstanceProcAddr reach the same export. ReShade's layer sits below the hook and passes the longer list on.
// HSF's own layer (vk_layer.cpp) does the same from outside, for a game whose device the hook misses.
#pragma once

#include <cstdint>

namespace hsf::vk {

// DllMain, attach. Hooks vkCreateDevice if vulkan-1.dll is in the process (nothing in other games). Returns a line for the log.
const char* HookCreateDevice();
// DllMain, detach (not at process exit). ReShade may unload and load the add-on again, so the hook must never point into a freed HSF. The
// patch itself stays (another program may have hooked on top of it); its stub goes to the original until the next HSF takes it back.
void UnhookCreateDevice();
// What the hook turned on for a device (a VkDevice): bit 1 shared semaphores with timeline, bit 2 shared memory; 0 if the hook did not see it.
int HookFlags(const void* vk_device);
// The hook's line about the newest device it saw, or "" if none.
const char* HookReport();

}  // namespace hsf::vk
