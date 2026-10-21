// VK_KHR_dynamic_rendering on top of render passes; see dynamic_rendering.h.
#include "dynamic_rendering.h"

#include <android/log.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace dynamic_rendering {
namespace {

constexpr const char* kTag = "LibertyDriver";

// Instance-level functions of the selected loader. They are the loader's
// trampolines and do not depend on the instance they were queried from.
struct InstanceFunctions {
  PFN_vkGetPhysicalDeviceProperties GetPhysicalDeviceProperties = nullptr;
  PFN_vkEnumerateDeviceExtensionProperties EnumerateDeviceExtensionProperties = nullptr;
  PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2 = nullptr;
  PFN_vkGetPhysicalDeviceFeatures2 GetPhysicalDeviceFeatures2KHR = nullptr;
  PFN_vkCreateDevice CreateDevice = nullptr;
  PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
};

std::mutex g_instance_mutex;
InstanceFunctions g_instance;
std::unordered_map<VkPhysicalDevice, bool> g_physical_emulated;

void LoadInstanceFunctions(VkInstance instance, PFN_vkGetInstanceProcAddr get) {
  if (!instance || !get) return;
  std::lock_guard<std::mutex> lock(g_instance_mutex);
  InstanceFunctions& f = g_instance;
#define LOAD(member, name) \
  if (!f.member) f.member = reinterpret_cast<decltype(f.member)>(get(instance, name))
  LOAD(GetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
  LOAD(EnumerateDeviceExtensionProperties, "vkEnumerateDeviceExtensionProperties");
  LOAD(GetPhysicalDeviceFeatures2, "vkGetPhysicalDeviceFeatures2");
  LOAD(GetPhysicalDeviceFeatures2KHR, "vkGetPhysicalDeviceFeatures2KHR");
  LOAD(CreateDevice, "vkCreateDevice");
  LOAD(GetDeviceProcAddr, "vkGetDeviceProcAddr");
#undef LOAD
}

std::vector<VkExtensionProperties> DriverExtensions(
    VkPhysicalDevice physical_device,
    PFN_vkEnumerateDeviceExtensionProperties enumerate = nullptr) {
  std::vector<VkExtensionProperties> extensions;
  if (!enumerate) enumerate = g_instance.EnumerateDeviceExtensionProperties;
  if (!enumerate) return extensions;
  for (int attempt = 0; attempt < 4; ++attempt) {
    uint32_t count = 0;
    if (enumerate(physical_device, nullptr, &count, nullptr) != VK_SUCCESS) break;
    extensions.resize(count);
    const VkResult result = enumerate(physical_device, nullptr, &count, extensions.data());
    extensions.resize(count);
    if (result != VK_INCOMPLETE) break;
  }
  return extensions;
}

bool HasExtension(const std::vector<VkExtensionProperties>& extensions, const char* name) {
  return std::any_of(extensions.begin(), extensions.end(), [name](const VkExtensionProperties& e) {
    return std::strcmp(e.extensionName, name) == 0;
  });
}

// Emulated when the driver offers neither Vulkan 1.3 nor the extension. A
// Vulkan 1.0 device is left alone: the render pass path needs nothing newer,
// but nothing that runs the game is 1.0.
// LIBERTY_EMULATE_DYNAMIC_RENDERING=1 emulates it on any Vulkan 1.1+ device,
// to test the translation on a driver that has the extension.
bool NeedsEmulation(VkPhysicalDevice physical_device,
                    PFN_vkGetPhysicalDeviceProperties get_properties,
                    PFN_vkEnumerateDeviceExtensionProperties enumerate) {
  if (!get_properties || !enumerate) return false;
  VkPhysicalDeviceProperties properties{};
  get_properties(physical_device, &properties);
  if (properties.apiVersion < VK_API_VERSION_1_1) return false;
  const char* forced = std::getenv("LIBERTY_EMULATE_DYNAMIC_RENDERING");
  if (forced && forced[0] == '1') return true;
  return properties.apiVersion < VK_API_VERSION_1_3 &&
         !HasExtension(DriverExtensions(physical_device, enumerate),
                       VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
}

bool PhysicalEmulated(VkPhysicalDevice physical_device) {
  {
    std::lock_guard<std::mutex> lock(g_instance_mutex);
    const auto it = g_physical_emulated.find(physical_device);
    if (it != g_physical_emulated.end()) return it->second;
  }
  const bool emulated = NeedsEmulation(physical_device, g_instance.GetPhysicalDeviceProperties,
                                       g_instance.EnumerateDeviceExtensionProperties);
  if (emulated) {
    __android_log_print(ANDROID_LOG_INFO, kTag,
                        "VK_KHR_dynamic_rendering is emulated with render passes on this device");
  }
  std::lock_guard<std::mutex> lock(g_instance_mutex);
  g_physical_emulated[physical_device] = emulated;
  return emulated;
}

// The dispatch table pointer every dispatchable handle starts with. A device
// and the command buffers allocated from it share it.
void* DispatchKey(const void* handle) { return *static_cast<void* const*>(handle); }

using Key = std::vector<uint64_t>;

struct ImageInfo {
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  VkExtent2D extent = {};
};

struct ViewInfo {
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
  // Of the view's base mip level; zero when unknown (swapchain images).
  VkExtent2D extent = {};
};

struct Device {
  VkDevice handle = VK_NULL_HANDLE;
  bool depth_stencil_resolve = false;

  PFN_vkDestroyDevice DestroyDevice = nullptr;
  PFN_vkCreateRenderPass CreateRenderPass = nullptr;
  PFN_vkCreateRenderPass2 CreateRenderPass2 = nullptr;
  PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
  PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
  PFN_vkDestroyFramebuffer DestroyFramebuffer = nullptr;
  PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
  PFN_vkCmdEndRenderPass CmdEndRenderPass = nullptr;
  PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
  PFN_vkCreateImage CreateImage = nullptr;
  PFN_vkDestroyImage DestroyImage = nullptr;
  PFN_vkCreateImageView CreateImageView = nullptr;
  PFN_vkDestroyImageView DestroyImageView = nullptr;

  std::mutex mutex;
  std::unordered_map<VkImage, ImageInfo> images;
  std::unordered_map<VkImageView, ViewInfo> views;
  std::map<Key, VkRenderPass> render_passes;
  std::map<Key, VkFramebuffer> framebuffers;
  // Framebuffers to destroy with an image view.
  std::unordered_map<VkImageView, std::vector<Key>> view_framebuffers;
  bool warned_depth_resolve = false;
  bool warned_unsupported = false;
};

std::mutex g_devices_mutex;
std::unordered_map<void*, std::unique_ptr<Device>> g_devices;

Device* FindDevice(const void* dispatchable, bool exact) {
  std::lock_guard<std::mutex> lock(g_devices_mutex);
  const auto it = g_devices.find(DispatchKey(dispatchable));
  if (it != g_devices.end()) return it->second.get();
  // Should the dispatch key ever differ, a single emulated device is still
  // unambiguous for command buffers.
  if (!exact && g_devices.size() == 1) return g_devices.begin()->second.get();
  return nullptr;
}

bool HasDepth(VkFormat format) {
  switch (format) {
    case VK_FORMAT_D16_UNORM:
    case VK_FORMAT_X8_D24_UNORM_PACK32:
    case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return true;
    default:
      return false;
  }
}

bool HasStencil(VkFormat format) {
  switch (format) {
    case VK_FORMAT_S8_UINT:
    case VK_FORMAT_D16_UNORM_S8_UINT:
    case VK_FORMAT_D24_UNORM_S8_UINT:
    case VK_FORMAT_D32_SFLOAT_S8_UINT:
      return true;
    default:
      return false;
  }
}

// One subpass, every attachment in the layout it is used in from start to
// end, so the render pass performs no layout transitions of its own - the
// application's barriers around vkCmdBeginRendering stay what synchronizes.
struct PassDescription {
  struct Attachment {
    VkFormat format;
    VkSampleCountFlagBits samples;
    VkAttachmentLoadOp load;
    VkAttachmentStoreOp store;
    VkAttachmentLoadOp stencil_load;
    VkAttachmentStoreOp stencil_store;
    VkImageLayout layout;
  };
  std::vector<Attachment> attachments;
  std::vector<uint32_t> color;    // Attachment index or VK_ATTACHMENT_UNUSED.
  std::vector<uint32_t> resolve;  // Empty, or one entry per color attachment.
  uint32_t depth_stencil = VK_ATTACHMENT_UNUSED;
  uint32_t depth_stencil_resolve = VK_ATTACHMENT_UNUSED;
  VkResolveModeFlagBits depth_resolve_mode = VK_RESOLVE_MODE_NONE;
  VkResolveModeFlagBits stencil_resolve_mode = VK_RESOLVE_MODE_NONE;

  uint32_t Add(const Attachment& attachment) {
    attachments.push_back(attachment);
    return uint32_t(attachments.size() - 1);
  }

  Key MakeKey() const {
    Key key;
    key.push_back(attachments.size());
    for (const Attachment& a : attachments) {
      key.insert(key.end(), {uint64_t(a.format), uint64_t(a.samples), uint64_t(a.load),
                             uint64_t(a.store), uint64_t(a.stencil_load),
                             uint64_t(a.stencil_store), uint64_t(a.layout)});
    }
    key.push_back(color.size());
    key.insert(key.end(), color.begin(), color.end());
    key.push_back(resolve.size());
    key.insert(key.end(), resolve.begin(), resolve.end());
    key.insert(key.end(), {depth_stencil, depth_stencil_resolve, uint64_t(depth_resolve_mode),
                           uint64_t(stencil_resolve_mode)});
    return key;
  }
};

VkRenderPass CreatePass(Device& device, const PassDescription& pass) {
  VkRenderPass render_pass = VK_NULL_HANDLE;
  const bool needs_2 = pass.depth_stencil_resolve != VK_ATTACHMENT_UNUSED;
  if (needs_2) {
    std::vector<VkAttachmentDescription2> attachments;
    for (const auto& a : pass.attachments) {
      VkAttachmentDescription2 d{};
      d.sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2;
      d.format = a.format;
      d.samples = a.samples;
      d.loadOp = a.load;
      d.storeOp = a.store;
      d.stencilLoadOp = a.stencil_load;
      d.stencilStoreOp = a.stencil_store;
      d.initialLayout = a.layout;
      d.finalLayout = a.layout;
      attachments.push_back(d);
    }
    const auto reference = [&pass](uint32_t index) {
      VkAttachmentReference2 r{};
      r.sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2;
      r.attachment = index;
      r.layout = index == VK_ATTACHMENT_UNUSED ? VK_IMAGE_LAYOUT_UNDEFINED
                                               : pass.attachments[index].layout;
      return r;
    };
    std::vector<VkAttachmentReference2> color, resolve;
    for (uint32_t index : pass.color) color.push_back(reference(index));
    for (uint32_t index : pass.resolve) resolve.push_back(reference(index));
    const VkAttachmentReference2 depth_stencil = reference(pass.depth_stencil);
    const VkAttachmentReference2 depth_stencil_resolve = reference(pass.depth_stencil_resolve);
    VkSubpassDescriptionDepthStencilResolve ds_resolve{};
    ds_resolve.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE;
    ds_resolve.depthResolveMode = pass.depth_resolve_mode;
    ds_resolve.stencilResolveMode = pass.stencil_resolve_mode;
    ds_resolve.pDepthStencilResolveAttachment = &depth_stencil_resolve;
    VkSubpassDescription2 subpass{};
    subpass.sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2;
    subpass.pNext = &ds_resolve;
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = uint32_t(color.size());
    subpass.pColorAttachments = color.data();
    subpass.pResolveAttachments = resolve.empty() ? nullptr : resolve.data();
    subpass.pDepthStencilAttachment =
        pass.depth_stencil != VK_ATTACHMENT_UNUSED ? &depth_stencil : nullptr;
    VkRenderPassCreateInfo2 info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2;
    info.attachmentCount = uint32_t(attachments.size());
    info.pAttachments = attachments.data();
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    if (device.CreateRenderPass2(device.handle, &info, nullptr, &render_pass) != VK_SUCCESS) {
      return VK_NULL_HANDLE;
    }
    return render_pass;
  }

  std::vector<VkAttachmentDescription> attachments;
  for (const auto& a : pass.attachments) {
    attachments.push_back({0, a.format, a.samples, a.load, a.store, a.stencil_load,
                           a.stencil_store, a.layout, a.layout});
  }
  const auto reference = [&pass](uint32_t index) {
    return VkAttachmentReference{index, index == VK_ATTACHMENT_UNUSED
                                            ? VK_IMAGE_LAYOUT_UNDEFINED
                                            : pass.attachments[index].layout};
  };
  std::vector<VkAttachmentReference> color, resolve;
  for (uint32_t index : pass.color) color.push_back(reference(index));
  for (uint32_t index : pass.resolve) resolve.push_back(reference(index));
  const VkAttachmentReference depth_stencil = reference(pass.depth_stencil);
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = uint32_t(color.size());
  subpass.pColorAttachments = color.data();
  subpass.pResolveAttachments = resolve.empty() ? nullptr : resolve.data();
  subpass.pDepthStencilAttachment =
      pass.depth_stencil != VK_ATTACHMENT_UNUSED ? &depth_stencil : nullptr;
  VkRenderPassCreateInfo info{};
  info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  info.attachmentCount = uint32_t(attachments.size());
  info.pAttachments = attachments.data();
  info.subpassCount = 1;
  info.pSubpasses = &subpass;
  if (device.CreateRenderPass(device.handle, &info, nullptr, &render_pass) != VK_SUCCESS) {
    return VK_NULL_HANDLE;
  }
  return render_pass;
}

// With device.mutex held.
VkRenderPass GetPass(Device& device, const PassDescription& pass) {
  const Key key = pass.MakeKey();
  const auto it = device.render_passes.find(key);
  if (it != device.render_passes.end()) return it->second;
  const VkRenderPass render_pass = CreatePass(device, pass);
  if (render_pass == VK_NULL_HANDLE) {
    __android_log_print(ANDROID_LOG_ERROR, kTag,
                        "dynamic rendering emulation: render pass creation failed "
                        "(%zu attachments)", pass.attachments.size());
    return VK_NULL_HANDLE;
  }
  device.render_passes.emplace(key, render_pass);
  return render_pass;
}

// --- Device-level replacements ------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL CreateImage(VkDevice device_handle, const VkImageCreateInfo* info,
                                           const VkAllocationCallbacks* allocator,
                                           VkImage* image) {
  Device* device = FindDevice(device_handle, false);
  const VkResult result = device->CreateImage(device_handle, info, allocator, image);
  if (result == VK_SUCCESS) {
    std::lock_guard<std::mutex> lock(device->mutex);
    device->images[*image] = {info->samples, {info->extent.width, info->extent.height}};
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyImage(VkDevice device_handle, VkImage image,
                                        const VkAllocationCallbacks* allocator) {
  Device* device = FindDevice(device_handle, false);
  {
    std::lock_guard<std::mutex> lock(device->mutex);
    device->images.erase(image);
  }
  device->DestroyImage(device_handle, image, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateImageView(VkDevice device_handle,
                                               const VkImageViewCreateInfo* info,
                                               const VkAllocationCallbacks* allocator,
                                               VkImageView* view) {
  Device* device = FindDevice(device_handle, false);
  const VkResult result = device->CreateImageView(device_handle, info, allocator, view);
  if (result == VK_SUCCESS) {
    std::lock_guard<std::mutex> lock(device->mutex);
    ViewInfo& view_info = device->views[*view];
    view_info.format = info->format;
    // Swapchain images are not created through vkCreateImage: single-sampled,
    // extent unknown.
    const auto image = device->images.find(info->image);
    if (image != device->images.end()) {
      const uint32_t mip = info->subresourceRange.baseMipLevel;
      view_info.samples = image->second.samples;
      view_info.extent = {std::max<uint32_t>(image->second.extent.width >> mip, 1),
                          std::max<uint32_t>(image->second.extent.height >> mip, 1)};
    }
  }
  return result;
}

VKAPI_ATTR void VKAPI_CALL DestroyImageView(VkDevice device_handle, VkImageView view,
                                            const VkAllocationCallbacks* allocator) {
  Device* device = FindDevice(device_handle, false);
  {
    // A view may only be destroyed once no pending work uses it, and neither
    // can a framebuffer made from it then.
    std::lock_guard<std::mutex> lock(device->mutex);
    device->views.erase(view);
    const auto users = device->view_framebuffers.find(view);
    if (users != device->view_framebuffers.end()) {
      for (const Key& key : users->second) {
        const auto framebuffer = device->framebuffers.find(key);
        if (framebuffer == device->framebuffers.end()) continue;
        device->DestroyFramebuffer(device_handle, framebuffer->second, nullptr);
        device->framebuffers.erase(framebuffer);
      }
      device->view_framebuffers.erase(users);
    }
  }
  device->DestroyImageView(device_handle, view, allocator);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateGraphicsPipelines(
    VkDevice device_handle, VkPipelineCache cache, uint32_t count,
    const VkGraphicsPipelineCreateInfo* infos, const VkAllocationCallbacks* allocator,
    VkPipeline* pipelines) {
  Device* device = FindDevice(device_handle, false);
  std::vector<VkGraphicsPipelineCreateInfo> copies(infos, infos + count);
  for (VkGraphicsPipelineCreateInfo& info : copies) {
    if (info.renderPass != VK_NULL_HANDLE) continue;
    const VkPipelineRenderingCreateInfo* rendering = nullptr;
    for (auto* s = static_cast<const VkBaseInStructure*>(info.pNext); s; s = s->pNext) {
      if (s->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
        rendering = reinterpret_cast<const VkPipelineRenderingCreateInfo*>(s);
        break;
      }
    }
    // Render pass compatibility only compares formats and sample counts, and
    // ignores resolve attachments for a single subpass.
    const VkSampleCountFlagBits samples =
        info.pMultisampleState ? info.pMultisampleState->rasterizationSamples
                               : VK_SAMPLE_COUNT_1_BIT;
    PassDescription pass;
    if (rendering) {
      for (uint32_t i = 0; i < rendering->colorAttachmentCount; ++i) {
        const VkFormat format =
            rendering->pColorAttachmentFormats ? rendering->pColorAttachmentFormats[i]
                                               : VK_FORMAT_UNDEFINED;
        pass.color.push_back(format == VK_FORMAT_UNDEFINED
            ? VK_ATTACHMENT_UNUSED
            : pass.Add({format, samples, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                        VK_ATTACHMENT_STORE_OP_DONT_CARE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                        VK_ATTACHMENT_STORE_OP_DONT_CARE,
                        VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL}));
      }
      const VkFormat depth_stencil = rendering->depthAttachmentFormat != VK_FORMAT_UNDEFINED
                                         ? rendering->depthAttachmentFormat
                                         : rendering->stencilAttachmentFormat;
      if (depth_stencil != VK_FORMAT_UNDEFINED) {
        pass.depth_stencil = pass.Add({depth_stencil, samples, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                       VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                       VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                                       VK_ATTACHMENT_STORE_OP_DONT_CARE,
                                       VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL});
      }
      if (rendering->viewMask) {
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "dynamic rendering emulation: multiview pipelines are unsupported");
      }
      // The renderer chains the structure first; the driver does not know it.
      if (info.pNext == rendering) info.pNext = rendering->pNext;
    }
    {
      std::lock_guard<std::mutex> lock(device->mutex);
      info.renderPass = GetPass(*device, pass);
    }
    info.subpass = 0;
    if (info.renderPass == VK_NULL_HANDLE) {
      std::fill_n(pipelines, count, VK_NULL_HANDLE);
      return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
  }
  return device->CreateGraphicsPipelines(device_handle, cache, count, copies.data(), allocator,
                                         pipelines);
}

VKAPI_ATTR void VKAPI_CALL CmdBeginRendering(VkCommandBuffer command_buffer,
                                             const VkRenderingInfo* info) {
  Device* device = FindDevice(command_buffer, false);
  if (!device) {
    __android_log_print(ANDROID_LOG_ERROR, kTag,
                        "dynamic rendering emulation: unknown command buffer");
    return;
  }
  PassDescription pass;
  std::vector<VkImageView> views;
  std::vector<VkExtent2D> extents;
  std::vector<VkClearValue> clears;
  std::unique_lock<std::mutex> lock(device->mutex);
  if ((info->flags || info->viewMask) && !device->warned_unsupported) {
    device->warned_unsupported = true;
    __android_log_print(ANDROID_LOG_ERROR, kTag,
                        "dynamic rendering emulation: flags 0x%x / viewMask 0x%x are unsupported",
                        info->flags, info->viewMask);
  }
  const auto view_info = [device](VkImageView view) {
    const auto it = device->views.find(view);
    if (it != device->views.end()) return it->second;
    __android_log_print(ANDROID_LOG_ERROR, kTag,
                        "dynamic rendering emulation: image view of unknown format");
    return ViewInfo{};
  };
  const auto add = [&](VkImageView view, const PassDescription::Attachment& attachment,
                       const VkClearValue& clear) {
    views.push_back(view);
    extents.push_back(device->views.count(view) ? device->views[view].extent : VkExtent2D{});
    clears.push_back(clear);
    return pass.Add(attachment);
  };

  bool any_resolve = false;
  std::vector<uint32_t> resolve(info->colorAttachmentCount, VK_ATTACHMENT_UNUSED);
  for (uint32_t i = 0; i < info->colorAttachmentCount; ++i) {
    const VkRenderingAttachmentInfo& a = info->pColorAttachments[i];
    if (a.imageView == VK_NULL_HANDLE) {
      pass.color.push_back(VK_ATTACHMENT_UNUSED);
      continue;
    }
    const ViewInfo view = view_info(a.imageView);
    pass.color.push_back(add(a.imageView,
                             {view.format, view.samples, a.loadOp, a.storeOp,
                              VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
                              a.imageLayout},
                             a.clearValue));
    if (a.resolveMode != VK_RESOLVE_MODE_NONE && a.resolveImageView != VK_NULL_HANDLE) {
      // A render pass resolves color by averaging (sample 0 for integers),
      // which is what the resolve modes valid for each format amount to.
      const ViewInfo target = view_info(a.resolveImageView);
      resolve[i] = add(a.resolveImageView,
                       {target.format, VK_SAMPLE_COUNT_1_BIT, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                        VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                        VK_ATTACHMENT_STORE_OP_DONT_CARE, a.resolveImageLayout},
                       VkClearValue{});
      any_resolve = true;
    }
  }
  if (any_resolve) pass.resolve = resolve;

  const VkRenderingAttachmentInfo* depth =
      info->pDepthAttachment && info->pDepthAttachment->imageView ? info->pDepthAttachment
                                                                  : nullptr;
  const VkRenderingAttachmentInfo* stencil =
      info->pStencilAttachment && info->pStencilAttachment->imageView ? info->pStencilAttachment
                                                                      : nullptr;
  if (depth || stencil) {
    // Both name the same view when both are present.
    const VkRenderingAttachmentInfo& main = depth ? *depth : *stencil;
    const ViewInfo view = view_info(main.imageView);
    // An aspect without its attachment is left as it is.
    const VkAttachmentLoadOp keep_load = VK_ATTACHMENT_LOAD_OP_LOAD;
    const VkAttachmentStoreOp keep_store = VK_ATTACHMENT_STORE_OP_STORE;
    const bool has_depth = HasDepth(view.format), has_stencil = HasStencil(view.format);
    PassDescription::Attachment attachment{
        view.format,
        view.samples,
        !has_depth ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : depth ? depth->loadOp : keep_load,
        !has_depth ? VK_ATTACHMENT_STORE_OP_DONT_CARE : depth ? depth->storeOp : keep_store,
        !has_stencil ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : stencil ? stencil->loadOp : keep_load,
        !has_stencil ? VK_ATTACHMENT_STORE_OP_DONT_CARE : stencil ? stencil->storeOp : keep_store,
        main.imageLayout};
    VkClearValue clear{};
    if (depth) clear.depthStencil.depth = depth->clearValue.depthStencil.depth;
    if (stencil) clear.depthStencil.stencil = stencil->clearValue.depthStencil.stencil;
    pass.depth_stencil = add(main.imageView, attachment, clear);

    const VkResolveModeFlagBits depth_mode =
        depth && depth->resolveImageView ? depth->resolveMode : VK_RESOLVE_MODE_NONE;
    const VkResolveModeFlagBits stencil_mode =
        stencil && stencil->resolveImageView ? stencil->resolveMode : VK_RESOLVE_MODE_NONE;
    if (depth_mode != VK_RESOLVE_MODE_NONE || stencil_mode != VK_RESOLVE_MODE_NONE) {
      if (device->depth_stencil_resolve) {
        const VkRenderingAttachmentInfo& resolving =
            depth_mode != VK_RESOLVE_MODE_NONE ? *depth : *stencil;
        const ViewInfo target = view_info(resolving.resolveImageView);
        const bool target_depth = HasDepth(target.format);
        const bool target_stencil = HasStencil(target.format);
        pass.depth_stencil_resolve = add(
            resolving.resolveImageView,
            {target.format, VK_SAMPLE_COUNT_1_BIT,
             target_depth && depth_mode == VK_RESOLVE_MODE_NONE ? keep_load
                                                                : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
             target_depth ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
             target_stencil && stencil_mode == VK_RESOLVE_MODE_NONE
                 ? keep_load
                 : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
             target_stencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
             resolving.resolveImageLayout},
            VkClearValue{});
        pass.depth_resolve_mode = depth_mode;
        pass.stencil_resolve_mode = stencil_mode;
      } else if (!device->warned_depth_resolve) {
        // The renderer only resolves depth when the driver reports
        // VK_KHR_depth_stencil_resolve, which the proxy then enables.
        device->warned_depth_resolve = true;
        __android_log_print(ANDROID_LOG_ERROR, kTag,
                            "dynamic rendering emulation: depth/stencil resolve without "
                            "VK_KHR_depth_stencil_resolve is dropped");
      }
    }
  }

  const VkRenderPass render_pass = GetPass(*device, pass);
  if (render_pass == VK_NULL_HANDLE) return;

  // The framebuffer takes the extent of its smallest attachment, so it does
  // not depend on the render area. Unknown extents (swapchain images) fall
  // back to the end of the render area, which every attachment covers.
  uint32_t width = UINT32_MAX, height = UINT32_MAX;
  for (const VkExtent2D& extent : extents) {
    if (!extent.width) {
      width = std::min(width, uint32_t(info->renderArea.offset.x) + info->renderArea.extent.width);
      height =
          std::min(height, uint32_t(info->renderArea.offset.y) + info->renderArea.extent.height);
    } else {
      width = std::min(width, extent.width);
      height = std::min(height, extent.height);
    }
  }
  if (extents.empty()) {
    width = uint32_t(info->renderArea.offset.x) + info->renderArea.extent.width;
    height = uint32_t(info->renderArea.offset.y) + info->renderArea.extent.height;
  }
  width = std::max<uint32_t>(width, 1);
  height = std::max<uint32_t>(height, 1);
  const uint32_t layers = std::max<uint32_t>(info->layerCount, 1);
  Key key{uint64_t(reinterpret_cast<uintptr_t>(render_pass)), width, height, layers};
  for (VkImageView view : views) key.push_back(uint64_t(reinterpret_cast<uintptr_t>(view)));
  VkFramebuffer framebuffer = VK_NULL_HANDLE;
  const auto existing = device->framebuffers.find(key);
  if (existing != device->framebuffers.end()) {
    framebuffer = existing->second;
  } else {
    VkFramebufferCreateInfo framebuffer_info{};
    framebuffer_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebuffer_info.renderPass = render_pass;
    framebuffer_info.attachmentCount = uint32_t(views.size());
    framebuffer_info.pAttachments = views.data();
    framebuffer_info.width = width;
    framebuffer_info.height = height;
    framebuffer_info.layers = layers;
    if (device->CreateFramebuffer(device->handle, &framebuffer_info, nullptr, &framebuffer) !=
        VK_SUCCESS) {
      __android_log_print(ANDROID_LOG_ERROR, kTag,
                          "dynamic rendering emulation: framebuffer creation failed");
      return;
    }
    device->framebuffers.emplace(key, framebuffer);
    for (VkImageView view : views) device->view_framebuffers[view].push_back(key);
  }

  VkRenderPassBeginInfo begin{};
  begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  begin.renderPass = render_pass;
  begin.framebuffer = framebuffer;
  begin.renderArea = info->renderArea;
  begin.clearValueCount = uint32_t(clears.size());
  begin.pClearValues = clears.data();
  lock.unlock();
  device->CmdBeginRenderPass(command_buffer, &begin, VK_SUBPASS_CONTENTS_INLINE);
}

VKAPI_ATTR void VKAPI_CALL CmdEndRendering(VkCommandBuffer command_buffer) {
  Device* device = FindDevice(command_buffer, false);
  if (device) device->CmdEndRenderPass(command_buffer);
}

VKAPI_ATTR void VKAPI_CALL DestroyDevice(VkDevice device_handle,
                                         const VkAllocationCallbacks* allocator) {
  if (!device_handle) return;
  std::unique_ptr<Device> device;
  {
    std::lock_guard<std::mutex> lock(g_devices_mutex);
    const auto it = g_devices.find(DispatchKey(device_handle));
    if (it != g_devices.end()) {
      device = std::move(it->second);
      g_devices.erase(it);
    }
  }
  if (!device) return;
  for (const auto& framebuffer : device->framebuffers) {
    device->DestroyFramebuffer(device_handle, framebuffer.second, nullptr);
  }
  for (const auto& render_pass : device->render_passes) {
    device->DestroyRenderPass(device_handle, render_pass.second, nullptr);
  }
  device->DestroyDevice(device_handle, allocator);
}

// --- Instance-level replacements -----------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL EnumerateDeviceExtensionProperties(
    VkPhysicalDevice physical_device, const char* layer, uint32_t* count,
    VkExtensionProperties* properties) {
  if (layer || !PhysicalEmulated(physical_device)) {
    return g_instance.EnumerateDeviceExtensionProperties(physical_device, layer, count,
                                                         properties);
  }
  std::vector<VkExtensionProperties> extensions = DriverExtensions(physical_device);
  if (!HasExtension(extensions, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME)) {
    VkExtensionProperties dynamic_rendering{};
    std::strncpy(dynamic_rendering.extensionName, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
                 VK_MAX_EXTENSION_NAME_SIZE - 1);
    dynamic_rendering.specVersion = 1;
    extensions.push_back(dynamic_rendering);
  }
  if (!properties) {
    *count = uint32_t(extensions.size());
    return VK_SUCCESS;
  }
  const uint32_t written = std::min<uint32_t>(*count, uint32_t(extensions.size()));
  std::copy_n(extensions.begin(), written, properties);
  *count = written;
  return written < extensions.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

void SetDynamicRenderingFeature(VkPhysicalDevice physical_device,
                                VkPhysicalDeviceFeatures2* features) {
  if (!features || !PhysicalEmulated(physical_device)) return;
  for (auto* s = reinterpret_cast<VkBaseOutStructure*>(features->pNext); s; s = s->pNext) {
    if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES) {
      reinterpret_cast<VkPhysicalDeviceDynamicRenderingFeatures*>(s)->dynamicRendering = VK_TRUE;
    }
  }
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2(VkPhysicalDevice physical_device,
                                                      VkPhysicalDeviceFeatures2* features) {
  g_instance.GetPhysicalDeviceFeatures2(physical_device, features);
  SetDynamicRenderingFeature(physical_device, features);
}

VKAPI_ATTR void VKAPI_CALL GetPhysicalDeviceFeatures2KHR(VkPhysicalDevice physical_device,
                                                         VkPhysicalDeviceFeatures2* features) {
  g_instance.GetPhysicalDeviceFeatures2KHR(physical_device, features);
  SetDynamicRenderingFeature(physical_device, features);
}

VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical_device,
                                            const VkDeviceCreateInfo* info,
                                            const VkAllocationCallbacks* allocator,
                                            VkDevice* device_handle) {
  bool requested = false;
  std::vector<const char*> extensions;
  for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
    if (std::strcmp(info->ppEnabledExtensionNames[i], VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME) ==
        0) {
      requested = true;
    } else {
      extensions.push_back(info->ppEnabledExtensionNames[i]);
    }
  }
  if (!requested || !PhysicalEmulated(physical_device)) {
    return g_instance.CreateDevice(physical_device, info, allocator, device_handle);
  }

  // VK_KHR_create_renderpass2 with VK_KHR_depth_stencil_resolve carries
  // depth/stencil resolves; the native renderer only asks for them when the
  // driver reports VK_KHR_depth_stencil_resolve.
  const std::vector<VkExtensionProperties> supported = DriverExtensions(physical_device);
  const auto enabled = [&extensions](const char* name) {
    return std::any_of(extensions.begin(), extensions.end(),
                       [name](const char* e) { return std::strcmp(e, name) == 0; });
  };
  const bool render_pass_2 = HasExtension(supported, VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
  const bool depth_stencil_resolve =
      render_pass_2 && HasExtension(supported, VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
  if (render_pass_2 && !enabled(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME)) {
    extensions.push_back(VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME);
  }
  if (depth_stencil_resolve && !enabled(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME)) {
    extensions.push_back(VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME);
  }
  VkDeviceCreateInfo copy = *info;
  copy.enabledExtensionCount = uint32_t(extensions.size());
  copy.ppEnabledExtensionNames = extensions.data();

  // The driver does not know the dynamic rendering feature structure: unlink
  // it for the call (the chain belongs to the caller and is restored after).
  VkBaseOutStructure* unlinked = nullptr;
  VkBaseOutStructure* unlinked_from = nullptr;
  for (auto* s = reinterpret_cast<VkBaseOutStructure*>(&copy); s->pNext; s = s->pNext) {
    if (s->pNext->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES) {
      unlinked = s->pNext;
      unlinked_from = s;
      s->pNext = unlinked->pNext;
      break;
    }
  }
  const VkResult result =
      g_instance.CreateDevice(physical_device, &copy, allocator, device_handle);
  if (unlinked && unlinked_from != reinterpret_cast<VkBaseOutStructure*>(&copy)) {
    unlinked_from->pNext = unlinked;
  }
  if (result != VK_SUCCESS) return result;

  auto device = std::make_unique<Device>();
  device->handle = *device_handle;
  device->depth_stencil_resolve = depth_stencil_resolve;
  const auto get = g_instance.GetDeviceProcAddr;
  bool loaded = get != nullptr;
#define LOAD(member, name)                                                                 \
  device->member = loaded ? reinterpret_cast<decltype(device->member)>(get(*device_handle, name)) \
                          : nullptr;                                                        \
  loaded = loaded && device->member != nullptr
  LOAD(DestroyDevice, "vkDestroyDevice");
  LOAD(CreateRenderPass, "vkCreateRenderPass");
  LOAD(DestroyRenderPass, "vkDestroyRenderPass");
  LOAD(CreateFramebuffer, "vkCreateFramebuffer");
  LOAD(DestroyFramebuffer, "vkDestroyFramebuffer");
  LOAD(CmdBeginRenderPass, "vkCmdBeginRenderPass");
  LOAD(CmdEndRenderPass, "vkCmdEndRenderPass");
  LOAD(CreateGraphicsPipelines, "vkCreateGraphicsPipelines");
  LOAD(CreateImage, "vkCreateImage");
  LOAD(DestroyImage, "vkDestroyImage");
  LOAD(CreateImageView, "vkCreateImageView");
  LOAD(DestroyImageView, "vkDestroyImageView");
#undef LOAD
  if (loaded && render_pass_2) {
    device->CreateRenderPass2 = reinterpret_cast<PFN_vkCreateRenderPass2>(
        get(*device_handle, "vkCreateRenderPass2KHR"));
    device->depth_stencil_resolve = depth_stencil_resolve && device->CreateRenderPass2;
  }
  if (!loaded) {
    __android_log_print(ANDROID_LOG_ERROR, kTag,
                        "dynamic rendering emulation: device functions are missing");
    const auto destroy =
        get ? reinterpret_cast<PFN_vkDestroyDevice>(get(*device_handle, "vkDestroyDevice"))
            : nullptr;
    if (destroy) destroy(*device_handle, allocator);
    *device_handle = VK_NULL_HANDLE;
    return VK_ERROR_INITIALIZATION_FAILED;
  }
  __android_log_print(ANDROID_LOG_INFO, kTag,
                      "dynamic rendering emulated with render passes (depth/stencil resolve %s)",
                      device->depth_stencil_resolve ? "available" : "unavailable");
  std::lock_guard<std::mutex> lock(g_devices_mutex);
  g_devices[DispatchKey(*device_handle)] = std::move(device);
  return VK_SUCCESS;
}

}  // namespace

PFN_vkVoidFunction InterceptInstanceProc(VkInstance instance, const char* name,
                                         PFN_vkGetInstanceProcAddr get) {
#define HOOK(fn) \
  if (std::strcmp(name, "vk" #fn) == 0) return reinterpret_cast<PFN_vkVoidFunction>(&fn)
  if (std::strcmp(name, "vkCreateDevice") != 0 &&
      std::strcmp(name, "vkEnumerateDeviceExtensionProperties") != 0 &&
      std::strncmp(name, "vkGetPhysicalDeviceFeatures2", 28) != 0) {
    return nullptr;
  }
  LoadInstanceFunctions(instance, get);
  if (!g_instance.GetPhysicalDeviceProperties || !g_instance.EnumerateDeviceExtensionProperties ||
      !g_instance.CreateDevice || !g_instance.GetDeviceProcAddr) {
    return nullptr;
  }
  HOOK(CreateDevice);
  HOOK(EnumerateDeviceExtensionProperties);
  if (g_instance.GetPhysicalDeviceFeatures2) HOOK(GetPhysicalDeviceFeatures2);
  if (g_instance.GetPhysicalDeviceFeatures2KHR) HOOK(GetPhysicalDeviceFeatures2KHR);
  return nullptr;
#undef HOOK
}

PFN_vkVoidFunction InterceptDeviceProc(VkDevice device, const char* name) {
  if (!device || !name || !FindDevice(device, true)) return nullptr;
#define HOOK(fn, replacement) \
  if (std::strcmp(name, fn) == 0) return reinterpret_cast<PFN_vkVoidFunction>(&replacement)
  HOOK("vkCmdBeginRenderingKHR", CmdBeginRendering);
  HOOK("vkCmdBeginRendering", CmdBeginRendering);
  HOOK("vkCmdEndRenderingKHR", CmdEndRendering);
  HOOK("vkCmdEndRendering", CmdEndRendering);
  HOOK("vkCreateGraphicsPipelines", CreateGraphicsPipelines);
  HOOK("vkCreateImage", CreateImage);
  HOOK("vkDestroyImage", DestroyImage);
  HOOK("vkCreateImageView", CreateImageView);
  HOOK("vkDestroyImageView", DestroyImageView);
  HOOK("vkDestroyDevice", DestroyDevice);
#undef HOOK
  return nullptr;
}

bool Emulated(VkInstance instance, VkPhysicalDevice physical_device,
              PFN_vkGetInstanceProcAddr get) {
  // The driver probes run before the loader is final (a custom driver may
  // fall back to the system one), so nothing is cached here.
  return NeedsEmulation(
      physical_device,
      reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
          get(instance, "vkGetPhysicalDeviceProperties")),
      reinterpret_cast<PFN_vkEnumerateDeviceExtensionProperties>(
          get(instance, "vkEnumerateDeviceExtensionProperties")));
}

}  // namespace dynamic_rendering
