// Local optional GPU integration test; requires an NVIDIA RTX GPU and DLSS SDK.
#include "video_core/renderer_vulkan/dlss/dlss.h"
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <vector>
#include <vulkan/vulkan.h>

#include "nvsdk_ngx_vk.h"
int main() {
  unsigned int nie = 0, nde = 0;
  const char **ies = nullptr, **des = nullptr;
  NVSDK_NGX_VULKAN_RequiredExtensions(&nie, &ies, &nde, &des);
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "bbport DLSS probe";
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ic{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ic.pApplicationInfo = &app;
  ic.enabledExtensionCount = nie;
  ic.ppEnabledExtensionNames = ies;
  VkInstance instance;
  if (vkCreateInstance(&ic, nullptr, &instance))
    return 1;
  uint32_t n = 0;
  vkEnumeratePhysicalDevices(instance, &n, nullptr);
  std::vector<VkPhysicalDevice> ps(n);
  vkEnumeratePhysicalDevices(instance, &n, ps.data());
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  for (auto p : ps) {
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(p, &pp);
    if (pp.vendorID == 0x10de) {
      physical = p;
      printf("GPU: %s\n", pp.deviceName);
      break;
    }
  }
  if (!physical)
    return 2;
  uint32_t qn = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qs(qn);
  vkGetPhysicalDeviceQueueFamilyProperties(physical, &qn, qs.data());
  uint32_t q = 0;
  while (!(qs[q].queueFlags & VK_QUEUE_COMPUTE_BIT))
    ++q;
  float priority = 1;
  VkDeviceQueueCreateInfo qc{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qc.queueFamilyIndex = q;
  qc.queueCount = 1;
  qc.pQueuePriorities = &priority;
  VkDeviceCreateInfo dc{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dc.queueCreateInfoCount = 1;
  dc.pQueueCreateInfos = &qc;
  const char *extensions[] = {"VK_NVX_binary_import",
                              "VK_NVX_image_view_handle",
                              "VK_KHR_push_descriptor"};
  VkPhysicalDeviceVulkan12Features f12{
      VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
  f12.bufferDeviceAddress = VK_TRUE;
  dc.pNext = &f12;
  dc.enabledExtensionCount = 3;
  dc.ppEnabledExtensionNames = extensions;
  for (unsigned i = 0; i < nde; ++i)
    printf("Extension: %s\n", des[i]);
  VkDevice device;
  if (vkCreateDevice(physical, &dc, nullptr, &device))
    return 3;

  auto check = [](VkResult r) {
    if (r != VK_SUCCESS)
      throw std::runtime_error("Vulkan error " + std::to_string(r));
  };
  VkQueue queue;
  vkGetDeviceQueue(device, q, 0, &queue);
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = q;
  VkCommandPool pool;
  check(vkCreateCommandPool(device, &pci, nullptr, &pool));
  VkCommandBufferAllocateInfo cai{
      VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkCommandBuffer cmd;
  check(vkAllocateCommandBuffers(device, &cai, &cmd));
  VkPhysicalDeviceMemoryProperties mem;
  vkGetPhysicalDeviceMemoryProperties(physical, &mem);
  auto memory_type = [&](uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < mem.memoryTypeCount; ++i)
      if ((bits & (1u << i)) &&
          (mem.memoryTypes[i].propertyFlags & flags) == flags)
        return i;
    throw std::runtime_error("No memory type");
  };
  struct Owned {
    Dlss::Image image;
    VkDeviceMemory memory;
  };
  auto image = [&](VkFormat format, uint32_t w, uint32_t h, bool depth) {
    Owned o{};
    VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = format;
    ci.extent = {w, h, 1};
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    if (!depth)
      ci.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    else
      ci.usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    check(vkCreateImage(device, &ci, nullptr, &o.image.image));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(device, o.image.image, &mr);
    VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ma.allocationSize = mr.size;
    ma.memoryTypeIndex =
        memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(device, &ma, nullptr, &o.memory));
    check(vkBindImageMemory(device, o.image.image, o.memory, 0));
    o.image.format = format;
    o.image.width = w;
    o.image.height = h;
    o.image.aspect =
        depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = o.image.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = format;
    vi.subresourceRange = {o.image.aspect, 0, 1, 0, 1};
    check(vkCreateImageView(device, &vi, nullptr, &o.image.view));
    return o;
  };
  auto color = image(VK_FORMAT_R16G16B16A16_SFLOAT, 1920, 1080, false),
       depth = image(VK_FORMAT_D32_SFLOAT, 1920, 1080, true),
       motion = image(VK_FORMAT_R16G16_SFLOAT, 1920, 1080, false),
       output = image(VK_FORMAT_R16G16B16A16_SFLOAT, 1920, 1080, false);
  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bi.size = 1920 * 1080 * 8;
  bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buffer;
  check(vkCreateBuffer(device, &bi, nullptr, &buffer));
  VkMemoryRequirements mr;
  vkGetBufferMemoryRequirements(device, buffer, &mr);
  VkMemoryAllocateInfo ma{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ma.allocationSize = mr.size;
  ma.memoryTypeIndex =
      memory_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkDeviceMemory bm;
  check(vkAllocateMemory(device, &ma, nullptr, &bm));
  check(vkBindBufferMemory(device, buffer, bm, 0));
  auto barrier = [&](Dlss::Image i, VkImageLayout old, VkImageLayout next) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask =
        old == VK_IMAGE_LAYOUT_UNDEFINED
            ? 0
            : VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = old;
    b.newLayout = next;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = i.image;
    b.subresourceRange = {i.aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
                         nullptr, 1, &b);
  };
  {
    Dlss::Upscaler dlss(instance, physical, device,
                        [&] { check(vkDeviceWaitIdle(device)); });
    if (!dlss.Available()) {
      fprintf(stderr, "%s\n", dlss.Problem().c_str());
      return 5;
    }
    const uint32_t widths[] = {1920, 1280, 1129, 960, 640};
    const uint32_t heights[] = {1080, 720, 635, 540, 360};
    for (int preset = 0; preset < 5; ++preset)
      for (int frame = 0; frame < 3; ++frame) {
        check(vkResetCommandBuffer(cmd, 0));
        VkCommandBufferBeginInfo begin{
            VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(cmd, &begin));
        for (auto o : {color, depth, motion, output})
          barrier(o.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        VkImageSubresourceRange cr{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
            dr{VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        VkClearColorValue solid{{0.25f, 0.5f, 0.75f, 1.f}}, zero{{0, 0, 0, 0}};
        VkClearDepthStencilValue z{0.5f, 0};
        vkCmdClearColorImage(cmd, color.image.image, VK_IMAGE_LAYOUT_GENERAL,
                             &solid, 1, &cr);
        vkCmdClearColorImage(cmd, motion.image.image, VK_IMAGE_LAYOUT_GENERAL,
                             &zero, 1, &cr);
        vkCmdClearColorImage(cmd, output.image.image, VK_IMAGE_LAYOUT_GENERAL,
                             &zero, 1, &cr);
        vkCmdClearDepthStencilImage(cmd, depth.image.image,
                                    VK_IMAGE_LAYOUT_GENERAL, &z, 1, &dr);
        for (auto o : {color, depth, motion, output})
          barrier(o.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        Dlss::Frame f{cmd,
                      color.image,
                      depth.image,
                      motion.image,
                      output.image,
                      widths[preset],
                      heights[preset],
                      preset,
                      0,
                      0,
                      frame == 0};
        if (!dlss.Record(f)) {
          fprintf(stderr, "%s\n", dlss.Problem().c_str());
          return 6;
        }
        barrier(output.image, VK_IMAGE_LAYOUT_GENERAL,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {1920, 1080, 1};
        vkCmdCopyImageToBuffer(cmd, output.image.image,
                               VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1,
                               &copy);
        VkMemoryBarrier read{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
        read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        read.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &read, 0,
                             nullptr, 0, nullptr);
        check(vkEndCommandBuffer(cmd));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &cmd;
        check(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE));
        check(vkQueueWaitIdle(queue));
        void *ptr;
        check(vkMapMemory(device, bm, 0, VK_WHOLE_SIZE, 0, &ptr));
        const auto *values = static_cast<const _Float16 *>(ptr);
        size_t good = 0;
        double sum = 0;
        for (size_t i = 0; i < 1920 * 1080; ++i) {
          float value = values[4 * i];
          if (std::isfinite(value) && value > 0.1f && value < 0.4f)
            ++good;
          sum += value;
        }
        vkUnmapMemory(device, bm);
        printf(
            "preset %d frame %d: finite expected red pixels %zu/%d mean %.4f\n",
            preset, frame, good, 1920 * 1080, sum / (1920 * 1080));
        if (good < 1920 * 1080 * 0.99)
          return 7;
      }
  }
  for (auto o : {color, depth, motion, output}) {
    vkDestroyImageView(device, o.image.view, nullptr);
    vkDestroyImage(device, o.image.image, nullptr);
    vkFreeMemory(device, o.memory, nullptr);
  }
  vkDestroyBuffer(device, buffer, nullptr);
  vkFreeMemory(device, bm, nullptr);
  vkDestroyCommandPool(device, pool, nullptr);
  vkDestroyDevice(device, nullptr);
  vkDestroyInstance(instance, nullptr);
  puts("PASS: DLSS 4.5 models M/L, five qualities, history and recreation, GPU "
       "readback");
}
