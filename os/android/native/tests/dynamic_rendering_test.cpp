// Host test of the dynamic rendering emulation (../dynamic_rendering.cpp) on a
// real driver with validation layers; run_dynamic_rendering_test.sh builds and
// runs it with LIBERTY_EMULATE_DYNAMIC_RENDERING=1.
#include <vulkan/vulkan.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>
#include "dynamic_rendering.h"
#include "quad_vert.h"
#include "quad_frag.h"

static int g_validation_errors = 0;
static VKAPI_ATTR VkBool32 VKAPI_CALL Messenger(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
  if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
    if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ++g_validation_errors;
    fprintf(stderr, "VALIDATION: %s\n", data->pMessage);
  }
  return VK_FALSE;
}
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { fprintf(stderr, "%s failed: %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static VkInstance g_instance; static VkDevice g_device; static PFN_vkGetDeviceProcAddr g_gdpa;
template <typename T> T Instance(const char* name) {
  if (auto hook = dynamic_rendering::InterceptInstanceProc(g_instance, name, vkGetInstanceProcAddr)) return reinterpret_cast<T>(hook);
  return reinterpret_cast<T>(vkGetInstanceProcAddr(g_instance, name));
}
template <typename T> T Device(const char* name) {
  if (auto hook = dynamic_rendering::InterceptDeviceProc(g_device, name)) return reinterpret_cast<T>(hook);
  return reinterpret_cast<T>(g_gdpa(g_device, name));
}

int main() {
  const char* layers[] = {"VK_LAYER_KHRONOS_validation"};
  const char* instance_extensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME};
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO}; app.apiVersion = VK_API_VERSION_1_1;
  VkDebugUtilsMessengerCreateInfoEXT messenger_info{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
  messenger_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
  messenger_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT;
  messenger_info.pfnUserCallback = Messenger;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO}; ici.pNext = &messenger_info; ici.pApplicationInfo = &app;
  ici.enabledLayerCount = 1; ici.ppEnabledLayerNames = layers; ici.enabledExtensionCount = 1; ici.ppEnabledExtensionNames = instance_extensions;
  CHECK(vkCreateInstance(&ici, nullptr, &g_instance));
  VkDebugUtilsMessengerEXT messenger;
  CHECK(reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(g_instance, "vkCreateDebugUtilsMessengerEXT"))(
      g_instance, &messenger_info, nullptr, &messenger));
  uint32_t n = 1; VkPhysicalDevice pd; vkEnumeratePhysicalDevices(g_instance, &n, &pd);

  auto enumerate = Instance<PFN_vkEnumerateDeviceExtensionProperties>("vkEnumerateDeviceExtensionProperties");
  auto features2 = Instance<PFN_vkGetPhysicalDeviceFeatures2>("vkGetPhysicalDeviceFeatures2");
  auto create_device = Instance<PFN_vkCreateDevice>("vkCreateDevice");
  g_gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(vkGetInstanceProcAddr(g_instance, "vkGetDeviceProcAddr"));
  uint32_t ext_count = 0; enumerate(pd, nullptr, &ext_count, nullptr);
  std::vector<VkExtensionProperties> exts(ext_count); enumerate(pd, nullptr, &ext_count, exts.data());
  bool advertised = false; for (auto& e : exts) advertised |= !strcmp(e.extensionName, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
  VkPhysicalDeviceDynamicRenderingFeatures dr{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES};
  VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; f2.pNext = &dr; features2(pd, &f2);
  printf("advertised=%d dynamicRendering=%d\n", advertised, dr.dynamicRendering);

  // Like VulkanDevice: the dynamic rendering features in the middle of the chain.
  VkPhysicalDeviceDynamicRenderingFeatures dr_enable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES}; dr_enable.dynamicRendering = VK_TRUE;
  VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
  timeline.pNext = &dr_enable;
  VkPhysicalDeviceFeatures2 enable{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2}; enable.pNext = &timeline;
  enable.features.sampleRateShading = VK_TRUE;
  float priority = 1; VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO}; qci.queueCount = 1; qci.pQueuePriorities = &priority;
  const char* device_extensions[] = {VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME};
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO}; dci.pNext = &enable; dci.queueCreateInfoCount = 1; dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 2; dci.ppEnabledExtensionNames = device_extensions;
  CHECK(create_device(pd, &dci, nullptr, &g_device));
  if (timeline.pNext != &dr_enable) { fprintf(stderr, "caller chain not restored\n"); return 1; }

  auto CreateImage = Device<PFN_vkCreateImage>("vkCreateImage");
  auto CreateImageView = Device<PFN_vkCreateImageView>("vkCreateImageView");
  auto DestroyImageView = Device<PFN_vkDestroyImageView>("vkDestroyImageView");
  auto DestroyImage = Device<PFN_vkDestroyImage>("vkDestroyImage");
  auto CreateGraphicsPipelines = Device<PFN_vkCreateGraphicsPipelines>("vkCreateGraphicsPipelines");
  auto CmdBeginRendering = Device<PFN_vkCmdBeginRenderingKHR>("vkCmdBeginRenderingKHR");
  auto CmdEndRendering = Device<PFN_vkCmdEndRenderingKHR>("vkCmdEndRenderingKHR");
  auto DestroyDevice = Device<PFN_vkDestroyDevice>("vkDestroyDevice");
  if (!CmdBeginRendering || !CmdEndRendering) { fprintf(stderr, "no vkCmdBeginRenderingKHR\n"); return 1; }
#define D(name) auto name = Device<PFN_vk##name>("vk" #name)
  D(AllocateMemory); D(BindImageMemory); D(GetImageMemoryRequirements); D(CreateBuffer); D(GetBufferMemoryRequirements);
  D(BindBufferMemory); D(MapMemory); D(CreateCommandPool); D(AllocateCommandBuffers); D(BeginCommandBuffer); D(EndCommandBuffer);
  D(CmdPipelineBarrier); D(CmdBindPipeline); D(CmdPushConstants); D(CmdSetViewport); D(CmdSetScissor); D(CmdDraw);
  D(CmdCopyImageToBuffer); D(GetDeviceQueue); D(QueueSubmit); D(QueueWaitIdle); D(CreateShaderModule); D(CreatePipelineLayout);
  D(DestroyPipeline); D(DestroyPipelineLayout); D(DestroyShaderModule); D(DestroyCommandPool); D(DestroyBuffer); D(FreeMemory);
  VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  auto memory_type = [&](uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags) return i;
    exit(2); };
  std::vector<VkDeviceMemory> memories;
  constexpr uint32_t W = 64, H = 64;
  auto image = [&](VkFormat format, VkSampleCountFlagBits samples, VkImageUsageFlags usage, VkImageAspectFlags aspect, VkImageView* view) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}; ii.imageType = VK_IMAGE_TYPE_2D; ii.format = format; ii.extent = {W, H, 1};
    ii.mipLevels = 1; ii.arrayLayers = 1; ii.samples = samples; ii.usage = usage;
    VkImage img; CHECK(CreateImage(g_device, &ii, nullptr, &img));
    VkMemoryRequirements req; GetImageMemoryRequirements(g_device, img, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; ai.allocationSize = req.size; ai.memoryTypeIndex = memory_type(req.memoryTypeBits, 0);
    VkDeviceMemory mem; CHECK(AllocateMemory(g_device, &ai, nullptr, &mem)); memories.push_back(mem); CHECK(BindImageMemory(g_device, img, mem, 0));
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO}; vi.image = img; vi.viewType = VK_IMAGE_VIEW_TYPE_2D; vi.format = format;
    vi.subresourceRange = {aspect, 0, 1, 0, 1}; CHECK(CreateImageView(g_device, &vi, nullptr, view)); return img; };
  const VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM, depth_format = VK_FORMAT_D32_SFLOAT_S8_UINT;
  VkImageView color_ms_view, color_view, depth_ms_view, depth_view;
  VkImage color_ms = image(color_format, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &color_ms_view);
  VkImage color = image(color_format, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT, &color_view);
  VkImage depth_ms = image(depth_format, VK_SAMPLE_COUNT_4_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, &depth_ms_view);
  VkImage depth = image(depth_format, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, &depth_view);

  VkPushConstantRange range{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}; pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &range;
  VkPipelineLayout layout; CHECK(CreatePipelineLayout(g_device, &pli, nullptr, &layout));
  VkShaderModuleCreateInfo smi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  VkShaderModule vs, fs; smi.codeSize = sizeof(quad_vert); smi.pCode = quad_vert; CHECK(CreateShaderModule(g_device, &smi, nullptr, &vs));
  smi.codeSize = sizeof(quad_frag); smi.pCode = quad_frag; CHECK(CreateShaderModule(g_device, &smi, nullptr, &fs));
  VkPipelineShaderStageCreateInfo stages[2] = {{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", nullptr},
                                               {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", nullptr}};
  VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO}; ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO}; vp.viewportCount = 1; vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO}; rs.lineWidth = 1; rs.cullMode = VK_CULL_MODE_NONE;
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO}; ms.rasterizationSamples = VK_SAMPLE_COUNT_4_BIT;
  VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO}; ds.depthTestEnable = VK_TRUE; ds.depthWriteEnable = VK_TRUE; ds.depthCompareOp = VK_COMPARE_OP_LESS;
  VkPipelineColorBlendAttachmentState blend_attachment{}; blend_attachment.colorWriteMask = 0xF;
  VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO}; blend.attachmentCount = 1; blend.pAttachments = &blend_attachment;
  VkDynamicState dynamic_states[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO}; dyn.dynamicStateCount = 2; dyn.pDynamicStates = dynamic_states;
  VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO}; rendering.colorAttachmentCount = 1;
  rendering.pColorAttachmentFormats = &color_format; rendering.depthAttachmentFormat = depth_format;
  rendering.stencilAttachmentFormat = depth_format;
  VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO}; gpi.pNext = &rendering; gpi.stageCount = 2; gpi.pStages = stages;
  gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia; gpi.pViewportState = &vp; gpi.pRasterizationState = &rs; gpi.pMultisampleState = &ms;
  gpi.pDepthStencilState = &ds; gpi.pColorBlendState = &blend; gpi.pDynamicState = &dyn; gpi.layout = layout;
  VkPipeline pipeline; CHECK(CreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipeline));
  // For render pass instances without a stencil attachment.
  rendering.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;
  VkPipeline depth_only_pipeline; CHECK(CreateGraphicsPipelines(g_device, VK_NULL_HANDLE, 1, &gpi, nullptr, &depth_only_pipeline));

  VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO}; bi.size = W * H * 4 * 3; bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buffer; CHECK(CreateBuffer(g_device, &bi, nullptr, &buffer));
  VkMemoryRequirements breq; GetBufferMemoryRequirements(g_device, buffer, &breq);
  VkMemoryAllocateInfo bai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO}; bai.allocationSize = breq.size;
  bai.memoryTypeIndex = memory_type(breq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkDeviceMemory bmem; CHECK(AllocateMemory(g_device, &bai, nullptr, &bmem)); memories.push_back(bmem); CHECK(BindBufferMemory(g_device, buffer, bmem, 0));

  VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO}; VkCommandPool pool; CHECK(CreateCommandPool(g_device, &cpi, nullptr, &pool));
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO}; cai.commandPool = pool; cai.commandBufferCount = 1;
  VkCommandBuffer cb; CHECK(AllocateCommandBuffers(g_device, &cai, &cb));
  VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; CHECK(BeginCommandBuffer(cb, &cbi));

  auto barrier = [&](VkImage img, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst,
                     VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage) {
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}; b.srcAccessMask = src; b.dstAccessMask = dst; b.oldLayout = from; b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED; b.image = img; b.subresourceRange = {aspect, 0, 1, 0, 1};
    CmdPipelineBarrier(cb, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b); };
  const VkPipelineStageFlags attach_stages = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  const VkAccessFlags color_rw = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  const VkAccessFlags depth_rw = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  for (VkImage img : {color_ms, color}) barrier(img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0, color_rw, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, attach_stages);
  for (VkImage img : {depth_ms, depth}) barrier(img, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0, depth_rw, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, attach_stages);

  struct PC { float color[4]; float xrange[2]; float z; float pad; };
  auto pass = [&](VkAttachmentLoadOp load, const PC* pc, bool with_stencil, VkRect2D area, bool resolve) {
    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; ca.imageView = color_ms_view; ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.resolveMode = resolve ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE; ca.resolveImageView = color_view; ca.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp = load; ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE; ca.clearValue.color = {{1, 0, 0, 1}};
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO}; da.imageView = depth_ms_view; da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    da.resolveMode = resolve ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE; da.resolveImageView = depth_view; da.resolveImageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    da.loadOp = load; da.storeOp = VK_ATTACHMENT_STORE_OP_STORE; da.clearValue.depthStencil = {1.0f, 0};
    VkRenderingAttachmentInfo sa = da; sa.clearValue.depthStencil = {0.0f, 7};
    if (!resolve) { ca.resolveImageView = VK_NULL_HANDLE; da.resolveImageView = VK_NULL_HANDLE; }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO}; ri.renderArea = area; ri.layerCount = 1;
    ri.colorAttachmentCount = 1; ri.pColorAttachments = &ca; ri.pDepthAttachment = &da;
    if (with_stencil) ri.pStencilAttachment = &sa;
    CmdBeginRendering(cb, &ri);
    if (pc) {
    VkViewport viewport{0, 0, W, H, 0, 1}; VkRect2D scissor = area;
    CmdSetViewport(cb, 0, 1, &viewport); CmdSetScissor(cb, 0, 1, &scissor);
    CmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, with_stencil ? pipeline : depth_only_pipeline);
    CmdPushConstants(cb, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(*pc), pc);
    CmdDraw(cb, 6, 1, 0, 0);
    }
    CmdEndRendering(cb); };
  const PC green{{0, 1, 0, 1}, {-1, 0}, 0.5f, 0}, blue{{0, 0, 1, 1}, {-1, 1}, 0.7f, 0};
  pass(VK_ATTACHMENT_LOAD_OP_CLEAR, &green, true, {{0, 0}, {W, H}}, false);
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; mb.srcAccessMask = color_rw | depth_rw; mb.dstAccessMask = color_rw | depth_rw;
  CmdPipelineBarrier(cb, attach_stages, attach_stages, 0, 1, &mb, 0, nullptr, 0, nullptr);
  // Depth only on a depth/stencil format: the stencil cleared to 7 must survive.
  pass(VK_ATTACHMENT_LOAD_OP_LOAD, &blue, false, {{32, 0}, {32, H}}, false);
  CmdPipelineBarrier(cb, attach_stages, attach_stages, 0, 1, &mb, 0, nullptr, 0, nullptr);
  pass(VK_ATTACHMENT_LOAD_OP_LOAD, nullptr, true, {{0, 0}, {W, H}}, true);
  barrier(color, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, color_rw, VK_ACCESS_TRANSFER_READ_BIT, attach_stages, VK_PIPELINE_STAGE_TRANSFER_BIT);
  barrier(depth, VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, depth_rw | color_rw, VK_ACCESS_TRANSFER_READ_BIT, attach_stages, VK_PIPELINE_STAGE_TRANSFER_BIT);
  VkBufferImageCopy copy{}; copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; copy.imageExtent = {W, H, 1};
  CmdCopyImageToBuffer(cb, color, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
  copy.bufferOffset = W * H * 4; copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
  CmdCopyImageToBuffer(cb, depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
  copy.bufferOffset = W * H * 8; copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
  CmdCopyImageToBuffer(cb, depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
  CHECK(EndCommandBuffer(cb));
  VkQueue queue; GetDeviceQueue(g_device, 0, 0, &queue);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO}; si.commandBufferCount = 1; si.pCommandBuffers = &cb;
  CHECK(QueueSubmit(queue, 1, &si, VK_NULL_HANDLE)); CHECK(QueueWaitIdle(queue));
  void* mapped; CHECK(MapMemory(g_device, bmem, 0, VK_WHOLE_SIZE, 0, &mapped));
  const uint8_t* px = static_cast<uint8_t*>(mapped); const float* dz = reinterpret_cast<const float*>(px + W * H * 4);
  int failures = 0;
  auto expect = [&](uint32_t x, uint32_t y, uint8_t r, uint8_t g, uint8_t b, float z) {
    const uint8_t* p = px + (y * W + x) * 4; const float d = dz[y * W + x];
    const uint8_t st = px[W * H * 8 + y * W + x];
    const bool ok = p[0] == r && p[1] == g && p[2] == b && std::fabs(d - z) < 1e-4f && st == 7;
    printf("(%u,%u) rgb=%u,%u,%u depth=%.3f stencil=%u %s\n", x, y, p[0], p[1], p[2], d, st, ok ? "ok" : "WRONG"); failures += !ok; };
  expect(16, 32, 0, 255, 0, 0.5f);   // first pass, nearer
  expect(48, 32, 0, 0, 255, 0.7f);   // second pass, where depth was cleared to 1
  expect(40, 10, 0, 0, 255, 0.7f);
  // Exercise framebuffer invalidation and device teardown.
  DestroyImageView(g_device, color_ms_view, nullptr);
  DestroyImageView(g_device, color_view, nullptr); DestroyImageView(g_device, depth_ms_view, nullptr); DestroyImageView(g_device, depth_view, nullptr);
  for (VkImage img : {color_ms, color, depth_ms, depth}) DestroyImage(g_device, img, nullptr);
  DestroyPipeline(g_device, pipeline, nullptr); DestroyPipeline(g_device, depth_only_pipeline, nullptr); DestroyPipelineLayout(g_device, layout, nullptr);
  DestroyShaderModule(g_device, vs, nullptr); DestroyShaderModule(g_device, fs, nullptr);
  DestroyCommandPool(g_device, pool, nullptr); DestroyBuffer(g_device, buffer, nullptr);
  for (VkDeviceMemory m : memories) FreeMemory(g_device, m, nullptr);
  DestroyDevice(g_device, nullptr);
  reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(g_instance, "vkDestroyDebugUtilsMessengerEXT"))(g_instance, messenger, nullptr);
  vkDestroyInstance(g_instance, nullptr);
  printf("pixel failures=%d validation errors=%d\n", failures, g_validation_errors);
  return failures || g_validation_errors;
}
