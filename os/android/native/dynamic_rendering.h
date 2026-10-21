// VK_KHR_dynamic_rendering on top of render passes, for drivers without it.
//
// The native renderer records every render pass instance with
// vkCmdBeginRendering. The stock ARM Mali-G610 driver of Android 12 reports
// Vulkan 1.1 without VK_KHR_dynamic_rendering, so the proxy advertises the
// extension itself and translates: pipelines get a compatible single-subpass
// render pass, and each vkCmdBeginRendering becomes vkCmdBeginRenderPass with a
// cached render pass and framebuffer. Only what the renderer uses is covered:
// no suspend/resume, no secondary command buffers, no multiview.
#pragma once

#include <vulkan/vulkan.h>

namespace dynamic_rendering {

// Wraps instance-level functions the emulation needs to see. Returns the
// replacement for `name`, or nullptr to keep `real`. `get` is the selected
// loader's vkGetInstanceProcAddr.
PFN_vkVoidFunction InterceptInstanceProc(VkInstance instance, const char* name,
                                         PFN_vkGetInstanceProcAddr get);

// Wraps device-level functions of a device the emulation is active on.
// Returns the replacement for `name`, or nullptr to keep the driver's.
PFN_vkVoidFunction InterceptDeviceProc(VkDevice device, const char* name);

// Whether the physical device gets VK_KHR_dynamic_rendering from the proxy.
// Uses the selected loader's functions for `instance`.
bool Emulated(VkInstance instance, VkPhysicalDevice physical_device,
              PFN_vkGetInstanceProcAddr get);

}  // namespace dynamic_rendering
