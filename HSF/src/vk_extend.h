// What HSF's Vulkan bridge needs on a game's Vulkan device, added to the game's vkCreateDevice request. Used by the add-on's hook on
// vulkan-1.dll's vkCreateDevice (vk_hook.cpp, the normal way) and by HSF's Vulkan layer (vk_layer.cpp, the fallback for games the hook misses).
//   VK_KHR_external_memory(_win32)     shared textures (ReShade 6.8's layer adds these too),
//   VK_KHR_external_semaphore(_win32)  a shared D3D11 fence imported as a Vulkan semaphore (no other layer adds them),
//   timelineSemaphore                  that semaphore counts up, as the D3D11 fence does (Vulkan 1.0 and 1.1 also need the extension).
// Each is added only if the GPU offers it and the game did not ask already.
#pragma once

#include <vulkan/vulkan.h>

#include <cstring>
#include <string>
#include <vector>

namespace hsf::vk {

// The changed request. Keep it in place (not copied or moved) until vkCreateDevice returned: `info` points into it.
struct Extended {
  VkDeviceCreateInfo info{};
  std::vector<const char*> names;
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
  VkBool32* flipped = nullptr;  // the game's own timelineSemaphore switch, if it had one and HSF turned it on
  bool semaphores = false;      // shared semaphores with timeline: same-frame hand-off
  bool memory = false;          // shared textures
  bool changed = false;         // anything differs from the game's request
  std::string added;            // for the log

  // Before trying the game's own request again: the game's feature list as the game gave it.
  void Restore() {
    if (flipped != nullptr) *flipped = VK_FALSE;
    flipped = nullptr;
  }
  // Bits for the bridge: 1 shared semaphores, 2 shared memory.
  int Flags() const { return (semaphores ? 1 : 0) | (memory ? 2 : 0); }
};

// Fills `out` from the game's request, the extensions the GPU offers and the GPU's Vulkan version.
inline void Extend(const VkDeviceCreateInfo& game, const std::vector<VkExtensionProperties>& offered, uint32_t api_version, Extended& out) {
  out.info = game;
  out.names.assign(game.ppEnabledExtensionNames, game.ppEnabledExtensionNames + game.enabledExtensionCount);
  const auto asked = [&](const char* name) {
    for (uint32_t i = 0; i < game.enabledExtensionCount; ++i)
      if (std::strcmp(game.ppEnabledExtensionNames[i], name) == 0) return true;
    return false;
  };
  const auto offers = [&](const char* name) {
    for (const auto& e : offered)
      if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
  };
  const auto add = [&](const char* name) {
    if (asked(name)) return true;
    if (!offers(name)) return false;
    out.names.push_back(name);
    out.added += out.added.empty() ? name : std::string(", ") + name;
    out.changed = true;
    return true;
  };
  out.memory = add(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME) && add(VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME);
  out.semaphores = add(VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME) && add(VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME);
  if (out.semaphores && VK_API_VERSION_MAJOR(api_version) == 1 && VK_API_VERSION_MINOR(api_version) < 2 && !add(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME))
    out.semaphores = false;

  // Timeline semaphores: in the game's Vulkan 1.2 feature list if it gave one, else in the timeline feature list it gave, else HSF's own
  // (both of the game's at once is not allowed, so HSF never adds a second).
  if (out.semaphores) {
    bool found = false;
    for (auto* p = static_cast<const VkBaseInStructure*>(game.pNext); p != nullptr && !found; p = p->pNext) {
      VkBool32* on = nullptr;
      if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
        on = &const_cast<VkPhysicalDeviceVulkan12Features*>(reinterpret_cast<const VkPhysicalDeviceVulkan12Features*>(p))->timelineSemaphore;
      else if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)
        on = &const_cast<VkPhysicalDeviceTimelineSemaphoreFeatures*>(reinterpret_cast<const VkPhysicalDeviceTimelineSemaphoreFeatures*>(p))->timelineSemaphore;
      if (on == nullptr) continue;
      found = true;
      if (*on == VK_FALSE) {
        *on = VK_TRUE;
        out.flipped = on;
        out.changed = true;
      }
    }
    if (!found) {
      out.timeline.timelineSemaphore = VK_TRUE;
      out.timeline.pNext = const_cast<void*>(out.info.pNext);
      out.info.pNext = &out.timeline;
      out.changed = true;
    }
  }
  out.info.enabledExtensionCount = static_cast<uint32_t>(out.names.size());
  out.info.ppEnabledExtensionNames = out.names.data();
}

}  // namespace hsf::vk
