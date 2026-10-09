// SPDX-License-Identifier: GPL-2.0-or-later
// Check NGX with bbport's actual Vulkan device and feature configuration.
#include "video_core/renderer_vulkan/vk_instance.h"

#include "video_core/renderer_vulkan/dlss/dlss.h"
#include <cstdio>
int main() {
  Vulkan::Instance instance(-1, false);
  if (!instance.IsDlssSupported()) {
    std::fprintf(stderr, "DLSS Vulkan extensions not enabled\n");
    return 1;
  }
  Dlss::Upscaler dlss(instance.GetInstance(), instance.GetPhysicalDevice(),
                      instance.GetDevice(),
                      [&] { instance.GetDevice().waitIdle(); });
  if (!dlss.Available()) {
    std::fprintf(stderr, "%s\n", dlss.Problem().c_str());
    return 2;
  }
  std::puts("PASS: NGX DLSS available on bbport's renderer device");
}
