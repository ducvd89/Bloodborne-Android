// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vulkan/vulkan.h>

namespace Dlss {
struct Image {
  VkImage image;
  VkImageView view;
  VkFormat format;
  uint32_t width, height;
  VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
};
struct Frame {
  VkCommandBuffer command;
  Image color, depth, motion, output;
  uint32_t width, height;
  int preset;
  float jitter_x, jitter_y;
  bool reset;
};
// Caller keeps images alive until submission completes and waits before
// destruction. wait_submitted retires previous submissions without submitting
// the current command buffer.
class Upscaler {
public:
  Upscaler(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
           std::function<void()> wait_submitted);
  ~Upscaler();
  bool Available() const;
  bool Record(const Frame &frame);
  const std::string &Problem() const;

private:
  struct Impl;
  std::unique_ptr<Impl> impl;
};
} // namespace Dlss
