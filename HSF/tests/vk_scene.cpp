// A small Vulkan program for bringing up HSF's Vulkan route without a game (build\x64\vk_scene.exe).
// It draws with GPU copies only, so it needs no shaders: a large textured "world" image is panned across the window like a moving camera,
// and a sprite moves in a circle on top as a moving object. Optical flow (LaunchPad) has real motion to follow.
// ReShade's Vulkan layer loads into it when the exe is listed in ReShade's app list; add-ons are read from the exe's folder.
// Keys: Space pauses the motion, Esc quits. Options: --size WxH (default 1280x720), --seconds N (quit after N seconds), --fps N (draw at most
// N frames a second, to see generated frames by eye at a low base rate).
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_WIN32_KHR
#include <windows.h>
#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <vector>

namespace {

#define VK_GLOBAL(X) X(vkCreateInstance) X(vkEnumerateInstanceExtensionProperties)
#define VK_INSTANCE(X)                                                                                                          \
  X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) \
  X(vkGetPhysicalDeviceMemoryProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) X(vkCreateWin32SurfaceKHR) X(vkDestroySurfaceKHR) \
  X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR)
#define VK_DEVICE(X)                                                                                                                 \
  X(vkDestroyDevice) X(vkGetDeviceQueue) X(vkDeviceWaitIdle) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
  X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkQueueSubmit) X(vkCreateImage) X(vkDestroyImage) X(vkGetImageMemoryRequirements)      \
  X(vkAllocateMemory) X(vkFreeMemory) X(vkBindImageMemory) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements)       \
  X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateCommandPool) X(vkDestroyCommandPool) X(vkAllocateCommandBuffers)    \
  X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdPipelineBarrier) X(vkCmdCopyImage)                      \
  X(vkCmdCopyBufferToImage) X(vkCmdClearColorImage) X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkCreateFence) X(vkDestroyFence)      \
  X(vkWaitForFences) X(vkResetFences)

PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr = nullptr;
#define DECLARE(name) PFN_##name name = nullptr;
VK_GLOBAL(DECLARE)
VK_INSTANCE(DECLARE)
VK_DEVICE(DECLARE)
#undef DECLARE

constexpr uint32_t FRAMES = 2;  // frames in flight
constexpr uint32_t SPRITE = 160;

bool g_quit = false, g_paused = false, g_resized = false;

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
  switch (message) {
    case WM_CLOSE: g_quit = true; return 0;
    case WM_SIZE: g_resized = true; return 0;
    case WM_KEYDOWN:
      if (wparam == VK_ESCAPE) g_quit = true;
      if (wparam == VK_SPACE) g_paused = !g_paused;
      return 0;
    default: return DefWindowProcW(window, message, wparam, lparam);
  }
}

[[noreturn]] void Fail(const char* what, VkResult result) {
  std::fprintf(stderr, "vk_scene: %s failed (%d)\n", what, static_cast<int>(result));
  std::exit(1);
}
#define CHECK(call)                                  \
  do {                                               \
    const VkResult check_result = (call);            \
    if (check_result != VK_SUCCESS) Fail(#call, check_result); \
  } while (0)

struct Vulkan {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice gpu = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  uint32_t family = 0;
  VkQueue queue = VK_NULL_HANDLE;
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkPhysicalDeviceMemoryProperties memory = {};
};

uint32_t MemoryType(const Vulkan& vk, uint32_t bits, VkMemoryPropertyFlags want) {
  for (uint32_t i = 0; i < vk.memory.memoryTypeCount; ++i)
    if ((bits & (1u << i)) != 0 && (vk.memory.memoryTypes[i].propertyFlags & want) == want) return i;
  Fail("finding a memory type", VK_ERROR_OUT_OF_DEVICE_MEMORY);
}

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint32_t width = 0, height = 0;
};

Image MakeImage(const Vulkan& vk, uint32_t width, uint32_t height) {
  Image result;
  result.width = width;
  result.height = height;
  VkImageCreateInfo info = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  info.imageType = VK_IMAGE_TYPE_2D;
  info.format = VK_FORMAT_B8G8R8A8_UNORM;
  info.extent = {width, height, 1};
  info.mipLevels = 1;
  info.arrayLayers = 1;
  info.samples = VK_SAMPLE_COUNT_1_BIT;
  info.tiling = VK_IMAGE_TILING_OPTIMAL;
  info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  CHECK(vkCreateImage(vk.device, &info, nullptr, &result.image));
  VkMemoryRequirements needs;
  vkGetImageMemoryRequirements(vk.device, result.image, &needs);
  VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  allocate.allocationSize = needs.size;
  allocate.memoryTypeIndex = MemoryType(vk, needs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  CHECK(vkAllocateMemory(vk.device, &allocate, nullptr, &result.memory));
  CHECK(vkBindImageMemory(vk.device, result.image, result.memory, 0));
  return result;
}

void Barrier(VkCommandBuffer list, VkImage image, VkImageLayout from, VkImageLayout to, VkAccessFlags src_access, VkAccessFlags dst_access) {
  VkImageMemoryBarrier barrier = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  barrier.srcAccessMask = src_access;
  barrier.dstAccessMask = dst_access;
  barrier.oldLayout = from;
  barrier.newLayout = to;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(list, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// The world must not repeat: on a regular pattern optical flow cannot tell a move of half a period one way from the other, and generated
// frames warp. So: a smooth colour field from waves of unrelated wavelengths, with one disc of random size, place and colour per 80 px cell.
uint32_t Hash(uint32_t a, uint32_t b, uint32_t salt) {
  uint32_t h = a * 0x8da6b343u ^ b * 0xd8163841u ^ salt * 0xcb1ab31fu;
  h ^= h >> 13;
  h *= 0x5bd1e995u;
  return h ^ (h >> 15);
}

uint32_t WorldPixel(uint32_t x, uint32_t y) {
  const float fx = static_cast<float>(x), fy = static_cast<float>(y);
  float r = 110 + 50 * std::sin(fx * 0.0031f + 1.3f) + 30 * std::sin(fy * 0.0047f + fx * 0.0011f);
  float g = 100 + 50 * std::sin(fy * 0.0027f + 0.4f) + 30 * std::sin(fx * 0.0053f - fy * 0.0017f);
  float b = 120 + 50 * std::sin((fx + fy) * 0.0023f + 2.1f) + 30 * std::sin(fx * 0.0041f + 0.7f);
  constexpr int CELL = 80;
  const int cx = static_cast<int>(x) / CELL, cy = static_cast<int>(y) / CELL;
  for (int dy = -1; dy <= 1; ++dy)
    for (int dx = -1; dx <= 1; ++dx) {
      const int nx = cx + dx, ny = cy + dy;
      if (nx < 0 || ny < 0) continue;
      const uint32_t h = Hash(static_cast<uint32_t>(nx), static_cast<uint32_t>(ny), 1);
      const float px = static_cast<float>(nx * CELL + static_cast<int>(h % CELL));
      const float py = static_cast<float>(ny * CELL + static_cast<int>((h >> 8) % CELL));
      const float radius = 8.0f + static_cast<float>((h >> 16) % 34);
      const float d = std::sqrt((fx - px) * (fx - px) + (fy - py) * (fy - py));
      if (d < radius) {
        const uint32_t c = Hash(static_cast<uint32_t>(nx), static_cast<uint32_t>(ny), 2);
        const bool ring = d > radius - 3;
        r = ring ? 20.0f : static_cast<float>(c & 0xff);
        g = ring ? 20.0f : static_cast<float>((c >> 8) & 0xff);
        b = ring ? 20.0f : static_cast<float>((c >> 16) & 0xff);
      }
    }
  const auto clamp = [](float v) { return static_cast<uint32_t>(v < 0 ? 0 : v > 255 ? 255 : v); };
  return 0xff000000u | (clamp(r) << 16) | (clamp(g) << 8) | clamp(b);  // B8G8R8A8 in memory: B, G, R, A
}

// The sprite: a yellow disc with a red ring and a dark centre stripe, on black.
uint32_t SpritePixel(uint32_t x, uint32_t y) {
  const float dx = static_cast<float>(x) - SPRITE / 2.0f, dy = static_cast<float>(y) - SPRITE / 2.0f;
  const float d = std::sqrt(dx * dx + dy * dy);
  if (d > SPRITE / 2.0f - 2) return 0xff101010u;
  if (d > SPRITE / 2.0f - 16) return 0xffd02020u;
  if (std::fabs(dx) < 6) return 0xff202020u;
  return 0xfff0d020u;
}

struct Swapchain {
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkExtent2D extent = {};
  std::vector<VkImage> images;
  std::vector<VkSemaphore> done;  // per image: the copies finished, present may run
};

Swapchain MakeSwapchain(const Vulkan& vk, VkSwapchainKHR old) {
  Swapchain result;
  VkSurfaceCapabilitiesKHR caps;
  CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(vk.gpu, vk.surface, &caps));
  result.extent = caps.currentExtent;
  if (result.extent.width == 0 || result.extent.height == 0) return result;  // minimised
  uint32_t count = 0;
  CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, vk.surface, &count, nullptr));
  std::vector<VkSurfaceFormatKHR> formats(count);
  CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(vk.gpu, vk.surface, &count, formats.data()));
  bool unorm = false;
  for (const auto& f : formats) unorm |= f.format == VK_FORMAT_B8G8R8A8_UNORM;
  if (!unorm) Fail("the window does not take B8G8R8A8_UNORM", VK_ERROR_FORMAT_NOT_SUPPORTED);
  VkSwapchainCreateInfoKHR info = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  info.surface = vk.surface;
  info.minImageCount = caps.minImageCount < 3 ? 3 : caps.minImageCount;
  if (caps.maxImageCount != 0 && info.minImageCount > caps.maxImageCount) info.minImageCount = caps.maxImageCount;
  info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
  info.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
  info.imageExtent = result.extent;
  info.imageArrayLayers = 1;
  info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  info.preTransform = caps.currentTransform;
  info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  info.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  info.clipped = VK_TRUE;
  info.oldSwapchain = old;
  CHECK(vkCreateSwapchainKHR(vk.device, &info, nullptr, &result.swapchain));
  CHECK(vkGetSwapchainImagesKHR(vk.device, result.swapchain, &count, nullptr));
  result.images.resize(count);
  CHECK(vkGetSwapchainImagesKHR(vk.device, result.swapchain, &count, result.images.data()));
  result.done.resize(count);
  VkSemaphoreCreateInfo semaphore = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (auto& s : result.done) CHECK(vkCreateSemaphore(vk.device, &semaphore, nullptr, &s));
  return result;
}

void DropSwapchain(const Vulkan& vk, Swapchain& chain) {
  for (auto s : chain.done) vkDestroySemaphore(vk.device, s, nullptr);
  if (chain.swapchain != VK_NULL_HANDLE) vkDestroySwapchainKHR(vk.device, chain.swapchain, nullptr);
  chain = {};
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
  uint32_t width = 1280, height = 720;
  double seconds = 0, fps = 0;
  for (int i = 1; i < argc; ++i) {
    if (std::wcscmp(argv[i], L"--size") == 0 && i + 1 < argc) swscanf_s(argv[++i], L"%ux%u", &width, &height);
    else if (std::wcscmp(argv[i], L"--seconds") == 0 && i + 1 < argc) seconds = _wtof(argv[++i]);
    else if (std::wcscmp(argv[i], L"--fps") == 0 && i + 1 < argc) fps = _wtof(argv[++i]);
  }
  // The frame cap: a high resolution timer to just before each frame's time, then a short spin.
  const HANDLE cap_timer = fps > 0 ? CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS) : nullptr;
  LONGLONG next_frame = 0;

  const HMODULE loader = LoadLibraryW(L"vulkan-1.dll");
  if (loader == nullptr) Fail("loading vulkan-1.dll", VK_ERROR_INITIALIZATION_FAILED);
  vkGetInstanceProcAddr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(loader, "vkGetInstanceProcAddr"));
#define LOAD_GLOBAL(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(nullptr, #name));
  VK_GLOBAL(LOAD_GLOBAL)
#undef LOAD_GLOBAL

  // The window.
  WNDCLASSEXW window_class = {sizeof(window_class)};
  window_class.lpfnWndProc = WindowProc;
  window_class.hInstance = GetModuleHandleW(nullptr);
  window_class.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  window_class.lpszClassName = L"HSF vk_scene";
  RegisterClassExW(&window_class);
  RECT rect = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
  AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
  const HWND window = CreateWindowExW(0, window_class.lpszClassName, L"HSF vk_scene", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                                      rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr, window_class.hInstance, nullptr);
  ShowWindow(window, SW_SHOW);

  Vulkan vk;
  {
    const char* extensions[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkApplicationInfo app = {VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "HSF vk_scene";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = extensions;
    CHECK(vkCreateInstance(&info, nullptr, &vk.instance));
  }
#define LOAD_INSTANCE(name) name = reinterpret_cast<PFN_##name>(vkGetInstanceProcAddr(vk.instance, #name));
  VK_INSTANCE(LOAD_INSTANCE)
#undef LOAD_INSTANCE

  {
    VkWin32SurfaceCreateInfoKHR info = {VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    info.hinstance = window_class.hInstance;
    info.hwnd = window;
    CHECK(vkCreateWin32SurfaceKHR(vk.instance, &info, nullptr, &vk.surface));
  }

  // The GPU: the first discrete one with a queue that draws and presents to the window.
  {
    uint32_t count = 0;
    CHECK(vkEnumeratePhysicalDevices(vk.instance, &count, nullptr));
    std::vector<VkPhysicalDevice> gpus(count);
    CHECK(vkEnumeratePhysicalDevices(vk.instance, &count, gpus.data()));
    for (int pass = 0; pass < 2 && vk.gpu == VK_NULL_HANDLE; ++pass)
      for (const auto gpu : gpus) {
        VkPhysicalDeviceProperties properties;
        vkGetPhysicalDeviceProperties(gpu, &properties);
        if (pass == 0 && properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, nullptr);
        std::vector<VkQueueFamilyProperties> list(families);
        vkGetPhysicalDeviceQueueFamilyProperties(gpu, &families, list.data());
        for (uint32_t f = 0; f < families && vk.gpu == VK_NULL_HANDLE; ++f) {
          VkBool32 present = VK_FALSE;
          vkGetPhysicalDeviceSurfaceSupportKHR(gpu, f, vk.surface, &present);
          if ((list[f].queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0 && present) {
            vk.gpu = gpu;
            vk.family = f;
            std::printf("vk_scene: %s\n", properties.deviceName);
          }
        }
      }
    if (vk.gpu == VK_NULL_HANDLE) Fail("finding a GPU that draws to the window", VK_ERROR_INITIALIZATION_FAILED);
    vkGetPhysicalDeviceMemoryProperties(vk.gpu, &vk.memory);
  }

  {
    const float priority = 1.0f;
    VkDeviceQueueCreateInfo queue = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex = vk.family;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo info = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = extensions;
    CHECK(vkCreateDevice(vk.gpu, &info, nullptr, &vk.device));
  }
#define LOAD_DEVICE(name) name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(vk.device, #name));
  VK_DEVICE(LOAD_DEVICE)
#undef LOAD_DEVICE
  vkGetDeviceQueue(vk.device, vk.family, 0, &vk.queue);

  VkCommandPool pool;
  {
    VkCommandPoolCreateInfo info = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    info.queueFamilyIndex = vk.family;
    CHECK(vkCreateCommandPool(vk.device, &info, nullptr, &pool));
  }
  VkCommandBuffer lists[FRAMES];
  {
    VkCommandBufferAllocateInfo info = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    info.commandPool = pool;
    info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    info.commandBufferCount = FRAMES;
    CHECK(vkAllocateCommandBuffers(vk.device, &info, lists));
  }

  // The world and the sprite, filled once from a staging buffer. The world is large enough to pan across a 4K window.
  const Image world = MakeImage(vk, 5760, 3240);
  const Image sprite = MakeImage(vk, SPRITE, SPRITE);
  {
    const VkDeviceSize world_bytes = VkDeviceSize(world.width) * world.height * 4, sprite_bytes = VkDeviceSize(SPRITE) * SPRITE * 4;
    VkBufferCreateInfo info = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = world_bytes + sprite_bytes;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VkBuffer staging;
    CHECK(vkCreateBuffer(vk.device, &info, nullptr, &staging));
    VkMemoryRequirements needs;
    vkGetBufferMemoryRequirements(vk.device, staging, &needs);
    VkMemoryAllocateInfo allocate = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    allocate.allocationSize = needs.size;
    allocate.memoryTypeIndex = MemoryType(vk, needs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory memory;
    CHECK(vkAllocateMemory(vk.device, &allocate, nullptr, &memory));
    CHECK(vkBindBufferMemory(vk.device, staging, memory, 0));
    void* mapped = nullptr;
    CHECK(vkMapMemory(vk.device, memory, 0, info.size, 0, &mapped));
    auto* pixels = static_cast<uint32_t*>(mapped);
    for (uint32_t y = 0; y < world.height; ++y)
      for (uint32_t x = 0; x < world.width; ++x) pixels[size_t(y) * world.width + x] = WorldPixel(x, y);
    pixels += size_t(world.width) * world.height;
    for (uint32_t y = 0; y < SPRITE; ++y)
      for (uint32_t x = 0; x < SPRITE; ++x) pixels[size_t(y) * SPRITE + x] = SpritePixel(x, y);
    vkUnmapMemory(vk.device, memory);

    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(vkBeginCommandBuffer(lists[0], &begin));
    const VkImage images[2] = {world.image, sprite.image};
    const uint32_t widths[2] = {world.width, SPRITE}, heights[2] = {world.height, SPRITE};
    const VkDeviceSize offsets[2] = {0, world_bytes};
    for (int i = 0; i < 2; ++i) {
      Barrier(lists[0], images[i], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
      VkBufferImageCopy copy = {};
      copy.bufferOffset = offsets[i];
      copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      copy.imageExtent = {widths[i], heights[i], 1};
      vkCmdCopyBufferToImage(lists[0], staging, images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
      Barrier(lists[0], images[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_ACCESS_TRANSFER_READ_BIT);
    }
    CHECK(vkEndCommandBuffer(lists[0]));
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &lists[0];
    CHECK(vkQueueSubmit(vk.queue, 1, &submit, VK_NULL_HANDLE));
    CHECK(vkDeviceWaitIdle(vk.device));
    vkDestroyBuffer(vk.device, staging, nullptr);
    vkFreeMemory(vk.device, memory, nullptr);
  }

  VkSemaphore acquired[FRAMES];
  VkFence in_flight[FRAMES];
  {
    VkSemaphoreCreateInfo semaphore = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (uint32_t i = 0; i < FRAMES; ++i) {
      CHECK(vkCreateSemaphore(vk.device, &semaphore, nullptr, &acquired[i]));
      CHECK(vkCreateFence(vk.device, &fence, nullptr, &in_flight[i]));
    }
  }

  Swapchain chain = MakeSwapchain(vk, VK_NULL_HANDLE);
  LARGE_INTEGER frequency, start, last_title;
  QueryPerformanceFrequency(&frequency);
  QueryPerformanceCounter(&start);
  last_title = start;
  double motion_time = 0, previous = 0;
  uint32_t frame = 0, frames_since_title = 0;

  while (!g_quit) {
    MSG message;
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
    if (g_quit) break;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    const double t = double(now.QuadPart - start.QuadPart) / double(frequency.QuadPart);
    if (seconds > 0 && t >= seconds) break;
    if (!g_paused) motion_time += t - previous;
    previous = t;

    if (g_resized || chain.swapchain == VK_NULL_HANDLE) {
      g_resized = false;
      CHECK(vkDeviceWaitIdle(vk.device));
      Swapchain old = chain;
      chain = MakeSwapchain(vk, old.swapchain);
      DropSwapchain(vk, old);
      if (chain.swapchain == VK_NULL_HANDLE) {
        Sleep(16);
        continue;
      }
    }

    const uint32_t f = frame % FRAMES;
    CHECK(vkWaitForFences(vk.device, 1, &in_flight[f], VK_TRUE, UINT64_MAX));
    uint32_t index = 0;
    VkResult result = vkAcquireNextImageKHR(vk.device, chain.swapchain, UINT64_MAX, acquired[f], VK_NULL_HANDLE, &index);
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
      g_resized = true;
      continue;
    }
    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) Fail("vkAcquireNextImageKHR", result);
    CHECK(vkResetFences(vk.device, 1, &in_flight[f]));

    // Camera: a slow figure-of-eight pan over the world. Object: the sprite circles the window's centre.
    const uint32_t w = chain.extent.width < world.width ? chain.extent.width : world.width;
    const uint32_t h = chain.extent.height < world.height ? chain.extent.height : world.height;
    const double pan_x = (std::sin(motion_time * 0.35) * 0.5 + 0.5) * double(world.width - w);
    const double pan_y = (std::sin(motion_time * 0.7) * 0.5 + 0.5) * double(world.height - h);
    const double radius = (w < h ? w : h) * 0.3;
    const double sprite_x = w / 2.0 + std::cos(motion_time * 1.6) * radius - SPRITE / 2.0;
    const double sprite_y = h / 2.0 + std::sin(motion_time * 1.6) * radius - SPRITE / 2.0;

    VkCommandBuffer list = lists[f];
    CHECK(vkResetCommandBuffer(list, 0));
    VkCommandBufferBeginInfo begin = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    CHECK(vkBeginCommandBuffer(list, &begin));
    const VkImage target = chain.images[index];
    Barrier(list, target, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
    if (w < chain.extent.width || h < chain.extent.height) {
      const VkClearColorValue black = {};
      const VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vkCmdClearColorImage(list, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
      Barrier(list, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_ACCESS_TRANSFER_WRITE_BIT);
    }
    VkImageCopy copy = {};
    copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.dstSubresource = copy.srcSubresource;
    copy.srcOffset = {static_cast<int32_t>(pan_x), static_cast<int32_t>(pan_y), 0};
    copy.extent = {w, h, 1};
    vkCmdCopyImage(list, world.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    if (sprite_x >= 0 && sprite_y >= 0 && sprite_x + SPRITE <= w && sprite_y + SPRITE <= h) {
      Barrier(list, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_ACCESS_TRANSFER_WRITE_BIT);
      copy.srcOffset = {0, 0, 0};
      copy.dstOffset = {static_cast<int32_t>(sprite_x), static_cast<int32_t>(sprite_y), 0};
      copy.extent = {SPRITE, SPRITE, 1};
      vkCmdCopyImage(list, sprite.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    }
    Barrier(list, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT, 0);
    CHECK(vkEndCommandBuffer(list));

    const VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo submit = {VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &acquired[f];
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &list;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &chain.done[index];
    CHECK(vkQueueSubmit(vk.queue, 1, &submit, in_flight[f]));

    VkPresentInfoKHR present = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &chain.done[index];
    present.swapchainCount = 1;
    present.pSwapchains = &chain.swapchain;
    present.pImageIndices = &index;
    result = vkQueuePresentKHR(vk.queue, &present);
    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) g_resized = true;
    else if (result != VK_SUCCESS) Fail("vkQueuePresentKHR", result);
    ++frame;
    ++frames_since_title;

    if (fps > 0) {
      const LONGLONG period = static_cast<LONGLONG>(double(frequency.QuadPart) / fps);
      LARGE_INTEGER t_now;
      QueryPerformanceCounter(&t_now);
      next_frame = (next_frame == 0 || t_now.QuadPart - next_frame > period) ? t_now.QuadPart + period : next_frame + period;
      const LONGLONG spin = frequency.QuadPart / 1000;
      if (cap_timer != nullptr && next_frame - t_now.QuadPart > spin) {
        LARGE_INTEGER due;
        due.QuadPart = -((next_frame - t_now.QuadPart - spin) * 10000000 / frequency.QuadPart);
        if (SetWaitableTimerEx(cap_timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(cap_timer, 1000);
      }
      do QueryPerformanceCounter(&t_now);
      while (t_now.QuadPart < next_frame);
    }

    const double since = double(now.QuadPart - last_title.QuadPart) / double(frequency.QuadPart);
    if (since >= 1.0) {
      wchar_t title[96];
      std::swprintf(title, 96, L"HSF vk_scene  %ux%u  %.0f fps%ls", chain.extent.width, chain.extent.height, frames_since_title / since,
                    g_paused ? L"  (paused)" : L"");
      SetWindowTextW(window, title);
      last_title = now;
      frames_since_title = 0;
    }
  }

  vkDeviceWaitIdle(vk.device);
  std::printf("vk_scene: %u frames\n", frame);
  DropSwapchain(vk, chain);
  for (uint32_t i = 0; i < FRAMES; ++i) {
    vkDestroySemaphore(vk.device, acquired[i], nullptr);
    vkDestroyFence(vk.device, in_flight[i], nullptr);
  }
  for (const Image* image : {&world, &sprite}) {
    vkDestroyImage(vk.device, image->image, nullptr);
    vkFreeMemory(vk.device, image->memory, nullptr);
  }
  vkDestroyCommandPool(vk.device, pool, nullptr);
  vkDestroyDevice(vk.device, nullptr);
  vkDestroySurfaceKHR(vk.instance, vk.surface, nullptr);
  vkDestroyInstance(vk.instance, nullptr);
  DestroyWindow(window);
  return 0;
}
