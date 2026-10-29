#include "render/Renderer.h"
#include "core/Log.h"
#include "generated/shaders_spv.h"

#include <imgui.h>
#include <imgui_impl_vulkan.h>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <cstring>

namespace kst {

namespace {

enum PipeKind { kMesh = 0, kId = 1, kLine = 2, kBg = 3, kGrid = 4, kGlassBack = 5, kGlassFront = 6 };

struct MeshPC {
    glm::mat4 viewProj;
    glm::vec4 camPos;
    glm::vec4 viewDir;
    glm::uvec4 hover;
    glm::vec4 params;
};
static_assert(sizeof(MeshPC) == 128, "push constant layout (128 is the guaranteed minimum limit)");

struct BgPC { glm::vec4 top, bottom; };
struct GridPC { glm::mat4 viewProj; glm::vec4 camPos; glm::vec4 params; glm::vec4 flags; };

void imageBarrier(VkCommandBuffer cmd, VkImage img, VkImageLayout from, VkImageLayout to,
                  VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                  VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess, VkImageAspectFlags aspect) {
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    di.imageMemoryBarrierCount = 1;
    di.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
}

constexpr VkPipelineStageFlags2 kAttachStages = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT |
    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;

} // namespace

// ------------------------------------------------------------------ resources

bool Renderer::createBuffer(GpuBuffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, bool readback) {
    destroyBuffer(b);
    size = std::max<VkDeviceSize>(size, 16);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (hostVisible)
        ai.flags = (readback ? VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT : VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT) |
                   VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(ctx_->vma, &bi, &ai, &b.buf, &b.alloc, &info) != VK_SUCCESS) {
        b = {};
        log::error("GPU buffer allocation failed (" + std::to_string(size) + " bytes)");
        return false;
    }
    b.mapped = info.pMappedData;
    b.size = size;
    return true;
}

void Renderer::destroyBuffer(GpuBuffer& b) {
    if (b.buf) vmaDestroyBuffer(ctx_->vma, b.buf, b.alloc);
    b = {};
}

bool Renderer::createImage(GpuImage& img, VkFormat fmt, VkImageUsageFlags usage, VkSampleCountFlagBits samples, VkImageAspectFlags aspect) {
    destroyImage(img);
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {ctx_->extent.width, ctx_->extent.height, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = samples;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    VmaAllocationCreateInfo ai{};
    ai.usage = VMA_MEMORY_USAGE_AUTO;
    if (usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT) ai.preferredFlags = VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT;
    if (vmaCreateImage(ctx_->vma, &ii, &ai, &img.img, &img.alloc, nullptr) != VK_SUCCESS) { img = {}; return false; }
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = img.img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCreateImageView(ctx_->device, &vi, nullptr, &img.view);
    return true;
}

void Renderer::destroyImage(GpuImage& img) {
    if (img.view) vkDestroyImageView(ctx_->device, img.view, nullptr);
    if (img.img) vmaDestroyImage(ctx_->vma, img.img, img.alloc);
    img = {};
}

bool Renderer::upload(GpuBuffer& dst, const void* data, VkDeviceSize size, VkBufferUsageFlags usage) {
    if (!createBuffer(dst, size, usage | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false)) return false;
    if (size == 0 || !data) return true;
    GpuBuffer staging;
    if (!createBuffer(staging, size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return false;
    std::memcpy(staging.mapped, data, size);
    vmaFlushAllocation(ctx_->vma, staging.alloc, 0, VK_WHOLE_SIZE);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = uploadPool_;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(ctx_->device, &ai, &cmd);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkBufferCopy region{0, 0, size};
    vkCmdCopyBuffer(cmd, staging.buf, dst.buf, 1, &region);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkResetFences(ctx_->device, 1, &uploadFence_);
    vkQueueSubmit(ctx_->queue, 1, &si, uploadFence_);
    vkWaitForFences(ctx_->device, 1, &uploadFence_, VK_TRUE, UINT64_MAX);
    vkFreeCommandBuffers(ctx_->device, uploadPool_, 1, &cmd);
    destroyBuffer(staging);
    return true;
}

bool Renderer::createAttachments() {
    auto msaa = ctx_->msaa;
    bool ok = true;
    ok &= createImage(msaaColor_, ctx_->swapFormat, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, msaa, VK_IMAGE_ASPECT_COLOR_BIT);
    ok &= createImage(msaaDepth_, depthFormat_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, msaa, VK_IMAGE_ASPECT_DEPTH_BIT);
    ok &= createImage(idColor_, idFormat_, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    ok &= createImage(idDepth_, depthFormat_, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    return ok;
}

void Renderer::destroyAttachments() {
    destroyImage(msaaColor_);
    destroyImage(msaaDepth_);
    destroyImage(idColor_);
    destroyImage(idDepth_);
}

// ------------------------------------------------------------------ pipelines

VkPipeline Renderer::makePipeline(const uint32_t* vs, size_t vsSize, const uint32_t* fs, size_t fsSize, VkPipelineLayout layout,
                                  int kind, VkSampleCountFlagBits samples, VkFormat colorFmt, bool depthWrite) {
    VkDevice dev = ctx_->device;
    auto mod = [&](const uint32_t* code, size_t size) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size;
        ci.pCode = code;
        VkShaderModule m = VK_NULL_HANDLE;
        vkCreateShaderModule(dev, &ci, nullptr, &m);
        return m;
    };
    VkShaderModule vm = mod(vs, vsSize), fm = mod(fs, fsSize);
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vm, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fm, "main", nullptr};

    std::vector<VkVertexInputBindingDescription> bindings;
    std::vector<VkVertexInputAttributeDescription> attrs;
    auto addInstanceAttrs = [&] {
        bindings.push_back({1, sizeof(InstanceGpu), VK_VERTEX_INPUT_RATE_INSTANCE});
        for (uint32_t i = 0; i < 4; ++i) attrs.push_back({2 + i, 1, VK_FORMAT_R32G32B32A32_SFLOAT, i * 16});
        attrs.push_back({6, 1, VK_FORMAT_R32G32B32A32_SFLOAT, 64});
        attrs.push_back({7, 1, VK_FORMAT_R32G32B32A32_UINT, 80});
        if (kind != kLine) attrs.push_back({8, 1, VK_FORMAT_R32G32B32A32_UINT, 96});
    };
    const bool glass = kind == kGlassBack || kind == kGlassFront;
    if (kind == kMesh || kind == kId || glass) {
        bindings.push_back({0, 24, VK_VERTEX_INPUT_RATE_VERTEX});
        attrs.push_back({0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0});
        attrs.push_back({1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12});
        addInstanceAttrs();
    } else if (kind == kLine) {
        bindings.push_back({0, 12, VK_VERTEX_INPUT_RATE_VERTEX});
        attrs.push_back({0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0});
        addInstanceAttrs();
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = (uint32_t)bindings.size();
    vi.pVertexBindingDescriptions = bindings.data();
    vi.vertexAttributeDescriptionCount = (uint32_t)attrs.size();
    vi.pVertexAttributeDescriptions = attrs.data();

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = kind == kLine ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;   // two sided: imported meshes can be inside out
    // See through bodies: inner (back) faces first, then the outer ones, so glass reads right.
    if (kind == kGlassBack) rs.cullMode = VK_CULL_MODE_FRONT_BIT;
    if (kind == kGlassFront) rs.cullMode = VK_CULL_MODE_BACK_BIT;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.f;
    if (kind == kMesh || kind == kId || glass) {
        // Push faces slightly back (reverse Z: toward 0) so edges draw cleanly on top.
        rs.depthBiasEnable = VK_TRUE;
        rs.depthBiasConstantFactor = -1.f;
        rs.depthBiasSlopeFactor = -1.25f;
    }

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = samples;

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = kind == kBg ? VK_FALSE : VK_TRUE;
    ds.depthWriteEnable = depthWrite ? VK_TRUE : VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL;   // reverse Z

    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = kind == kId ? (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT)
                                    : (VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT);
    if (kind == kGrid) {
        cb.blendEnable = VK_TRUE;
        cb.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.colorBlendOp = VK_BLEND_OP_ADD;
        cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    if (glass) {
        cb.blendEnable = VK_TRUE;   // premultiplied alpha
        cb.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.colorBlendOp = VK_BLEND_OP_ADD;
        cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        cb.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &cb;

    std::vector<VkDynamicState> dyn = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    if (kind == kLine && ctx_->wideLines) dyn.push_back(VK_DYNAMIC_STATE_LINE_WIDTH);
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = (uint32_t)dyn.size();
    dy.pDynamicStates = dyn.data();

    VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    rci.colorAttachmentCount = 1;
    rci.pColorAttachmentFormats = &colorFmt;
    rci.depthAttachmentFormat = depthFormat_;

    VkGraphicsPipelineCreateInfo pi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pi.pNext = &rci;
    pi.stageCount = 2;
    pi.pStages = stages;
    pi.pVertexInputState = &vi;
    pi.pInputAssemblyState = &ia;
    pi.pViewportState = &vp;
    pi.pRasterizationState = &rs;
    pi.pMultisampleState = &ms;
    pi.pDepthStencilState = &ds;
    pi.pColorBlendState = &cbs;
    pi.pDynamicState = &dy;
    pi.layout = layout;
    VkPipeline p = VK_NULL_HANDLE;
    VkResult r = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &p);
    if (r != VK_SUCCESS) log::error(std::string("pipeline creation failed: ") + vkResultName(r));
    vkDestroyShaderModule(dev, vm, nullptr);
    vkDestroyShaderModule(dev, fm, nullptr);
    return p;
}

bool Renderer::createPipelines(std::string& err) {
    VkDevice dev = ctx_->device;
    // binding 0: triangle -> face id table. binding 1: selected face ids (per frame).
    // binding 2: appearances. binding 3: per face appearance tables (both per frame).
    VkDescriptorSetLayoutBinding b[4] = {
        {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo dl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl.bindingCount = 4;
    dl.pBindings = b;
    vkCreateDescriptorSetLayout(dev, &dl, nullptr, &setLayout_);
    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 16};
    VkDescriptorPoolCreateInfo dp{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dp.maxSets = 4;
    dp.poolSizeCount = 1;
    dp.pPoolSizes = &ps;
    vkCreateDescriptorPool(dev, &dp, nullptr, &descPool_);
    for (auto& f : frames_) {
        VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        da.descriptorPool = descPool_;
        da.descriptorSetCount = 1;
        da.pSetLayouts = &setLayout_;
        vkAllocateDescriptorSets(dev, &da, &f.set);
        bindSelBuffer(f);
        bindStorage(f, 2, f.mats);
        bindStorage(f, 3, f.faceMats);
    }

    VkPushConstantRange meshRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(MeshPC)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &setLayout_;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &meshRange;
    vkCreatePipelineLayout(dev, &pl, nullptr, &meshLayout_);

    VkPushConstantRange bgRange{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(BgPC)};
    VkPipelineLayoutCreateInfo bl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    bl.pushConstantRangeCount = 1;
    bl.pPushConstantRanges = &bgRange;
    vkCreatePipelineLayout(dev, &bl, nullptr, &bgLayout_);

    VkPushConstantRange gridRange{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(GridPC)};
    VkPipelineLayoutCreateInfo gl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    gl.pushConstantRangeCount = 1;
    gl.pPushConstantRanges = &gridRange;
    vkCreatePipelineLayout(dev, &gl, nullptr, &gridLayout_);

    auto msaa = ctx_->msaa;
    auto cf = ctx_->swapFormat;
    meshPipe_ = makePipeline(spv::mesh_vert, sizeof spv::mesh_vert, spv::mesh_frag, sizeof spv::mesh_frag, meshLayout_, kMesh, msaa, cf, true);
    idPipe_ = makePipeline(spv::mesh_vert, sizeof spv::mesh_vert, spv::id_frag, sizeof spv::id_frag, meshLayout_, kId, VK_SAMPLE_COUNT_1_BIT, idFormat_, true);
    linePipe_ = makePipeline(spv::line_vert, sizeof spv::line_vert, spv::line_frag, sizeof spv::line_frag, meshLayout_, kLine, msaa, cf, false);
    bgPipe_ = makePipeline(spv::bg_vert, sizeof spv::bg_vert, spv::bg_frag, sizeof spv::bg_frag, bgLayout_, kBg, msaa, cf, false);
    gridPipe_ = makePipeline(spv::grid_vert, sizeof spv::grid_vert, spv::grid_frag, sizeof spv::grid_frag, gridLayout_, kGrid, msaa, cf, false);
    glassBackPipe_ = makePipeline(spv::mesh_vert, sizeof spv::mesh_vert, spv::mesh_frag, sizeof spv::mesh_frag, meshLayout_, kGlassBack, msaa, cf, false);
    glassFrontPipe_ = makePipeline(spv::mesh_vert, sizeof spv::mesh_vert, spv::mesh_frag, sizeof spv::mesh_frag, meshLayout_, kGlassFront, msaa, cf, false);
    if (!meshPipe_ || !idPipe_ || !linePipe_ || !bgPipe_ || !gridPipe_ || !glassBackPipe_ || !glassFrontPipe_) {
        err = "Could not create GPU pipelines";
        return false;
    }
    return true;
}

// ------------------------------------------------------------------ init / shutdown

bool Renderer::init(VkContext* ctx, std::string& err) {
    ctx_ = ctx;
    VkDevice dev = ctx_->device;
    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = ctx_->queueFamily;
    cpi.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    vkCreateCommandPool(dev, &cpi, nullptr, &uploadPool_);
    VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(dev, &fci, nullptr, &uploadFence_);

    for (auto& f : frames_) {
        vkCreateCommandPool(dev, &cpi, nullptr, &f.pool);
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = f.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &ai, &f.cmd);
        VkFenceCreateInfo sf{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        sf.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(dev, &sf, nullptr, &f.fence);
        VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        vkCreateSemaphore(dev, &si, nullptr, &f.acquired);
        createBuffer(f.readback, 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);
        createBuffer(f.instances, sizeof(InstanceGpu) * 64, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, true);
        createBuffer(f.meshCmds, sizeof(VkDrawIndexedIndirectCommand) * 16, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true);
        createBuffer(f.lineCmds, sizeof(VkDrawIndirectCommand) * 16, VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT, true);
        createBuffer(f.sel, 1024, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        createBuffer(f.mats, sizeof(MaterialGpu) * 64, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        createBuffer(f.faceMats, 4096, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
    }
    if (!createAttachments()) { err = "Could not allocate viewport images"; return false; }
    if (!createPipelines(err)) return false;
    rebuildGeometry({});
    return true;
}

bool Renderer::initImGui() {
    ImGui_ImplVulkan_InitInfo ii{};
    ii.ApiVersion = VK_API_VERSION_1_3;
    ii.Instance = ctx_->instance;
    ii.PhysicalDevice = ctx_->phys;
    ii.Device = ctx_->device;
    ii.QueueFamily = ctx_->queueFamily;
    ii.Queue = ctx_->queue;
    ii.DescriptorPoolSize = 64;
    ii.MinImageCount = ctx_->minImageCount;
    ii.ImageCount = (uint32_t)ctx_->images.size();
    ii.UseDynamicRendering = true;
    ii.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
    ii.PipelineInfoMain.PipelineRenderingCreateInfo.pColorAttachmentFormats = &ctx_->swapFormat;
    ii.CheckVkResultFn = [](VkResult r) {
        if (r != VK_SUCCESS) log::warn(std::string("imgui vulkan: ") + vkResultName(r));
    };
    imguiReady_ = ImGui_ImplVulkan_Init(&ii);
    return imguiReady_;
}

bool Renderer::canCapture() const { return ctx_ && ctx_->canCapture; }

bool Renderer::takeCapture(std::vector<uint8_t>& rgba, int& w, int& h) {
    if (!captureReady_) return false;
    rgba.swap(capture_);
    capture_.clear();
    w = captureW_;
    h = captureH_;
    captureReady_ = false;
    return true;
}

void Renderer::shutdown() {
    if (!ctx_ || !ctx_->device) return;
    VkDevice dev = ctx_->device;
    vkDeviceWaitIdle(dev);
    destroyBuffer(captureBuf_);
    if (imguiReady_) ImGui_ImplVulkan_Shutdown();
    imguiReady_ = false;
    for (auto& f : frames_) {
        destroyBuffer(f.instances);
        destroyBuffer(f.meshCmds);
        destroyBuffer(f.lineCmds);
        destroyBuffer(f.readback);
        destroyBuffer(f.sel);
        destroyBuffer(f.mats);
        destroyBuffer(f.faceMats);
        vkDestroyFence(dev, f.fence, nullptr);
        vkDestroySemaphore(dev, f.acquired, nullptr);
        vkDestroyCommandPool(dev, f.pool, nullptr);
        f = {};
    }
    destroyBuffer(vbo_);
    destroyBuffer(ibo_);
    destroyBuffer(faceBuf_);
    destroyBuffer(edgeVbo_);
    uploaded_.clear();
    destroyAttachments();
    for (VkPipeline p : {meshPipe_, idPipe_, linePipe_, bgPipe_, gridPipe_, glassBackPipe_, glassFrontPipe_})
        if (p) vkDestroyPipeline(dev, p, nullptr);
    for (VkPipelineLayout l : {meshLayout_, bgLayout_, gridLayout_})
        if (l) vkDestroyPipelineLayout(dev, l, nullptr);
    if (descPool_) vkDestroyDescriptorPool(dev, descPool_, nullptr);
    if (setLayout_) vkDestroyDescriptorSetLayout(dev, setLayout_, nullptr);
    if (uploadFence_) vkDestroyFence(dev, uploadFence_, nullptr);
    if (uploadPool_) vkDestroyCommandPool(dev, uploadPool_, nullptr);
}

// ------------------------------------------------------------------ scene

void Renderer::rebuildGeometry(const std::vector<SceneBatch>& batches) {
    vkDeviceWaitIdle(ctx_->device);   // rare: only when new unique meshes arrive
    uploaded_.clear();
    ranges_.clear();
    std::vector<float> verts, edges;
    std::vector<uint32_t> idx, faces;
    for (auto& b : batches) {
        if (!b.mesh || ranges_.count(b.mesh.get())) continue;
        const MeshData& m = *b.mesh;
        MeshRange r{};
        r.vertexOffset = (int32_t)(verts.size() / 6);
        r.firstIndex = (uint32_t)idx.size();
        r.indexCount = (uint32_t)m.indices.size();
        r.faceBase = (uint32_t)faces.size();
        r.edgeFirst = (uint32_t)(edges.size() / 3);
        r.edgeCount = (uint32_t)(m.edgeVerts.size() / 3);
        verts.insert(verts.end(), m.verts.begin(), m.verts.end());
        idx.insert(idx.end(), m.indices.begin(), m.indices.end());
        faces.insert(faces.end(), m.triFace.begin(), m.triFace.end());
        edges.insert(edges.end(), m.edgeVerts.begin(), m.edgeVerts.end());
        ranges_[b.mesh.get()] = r;
        uploaded_.push_back(b.mesh);
    }
    if (faces.empty()) faces.push_back(0);
    upload(vbo_, verts.data(), verts.size() * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    upload(ibo_, idx.data(), idx.size() * 4, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    upload(faceBuf_, faces.data(), faces.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    upload(edgeVbo_, edges.data(), edges.size() * 4, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (faceBuf_.buf) {
        for (auto& f : frames_) {
            if (!f.set) continue;
            VkDescriptorBufferInfo bi{faceBuf_.buf, 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstSet = f.set;
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo = &bi;
            vkUpdateDescriptorSets(ctx_->device, 1, &w, 0, nullptr);
        }
    }
}

void Renderer::bindSelBuffer(Frame& f) {
    if (!f.set || !f.sel.buf) return;
    VkDescriptorBufferInfo bi{f.sel.buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = f.set;
    w.dstBinding = 1;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(ctx_->device, 1, &w, 0, nullptr);
}

void Renderer::bindStorage(Frame& f, uint32_t binding, const GpuBuffer& b) {
    if (!f.set || !b.buf) return;
    VkDescriptorBufferInfo bi{b.buf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = f.set;
    w.dstBinding = binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &bi;
    vkUpdateDescriptorSets(ctx_->device, 1, &w, 0, nullptr);
}

void Renderer::setScene(const std::vector<SceneBatch>& batches, const std::vector<uint32_t>& selFaces,
                        const std::vector<MaterialGpu>& mats, const std::vector<uint32_t>& faceMats) {
    selFaces_ = selFaces;
    if (selFaces_.empty()) selFaces_.push_back(0xffffffffu);
    mats_ = mats;
    if (mats_.empty()) mats_.push_back(MaterialGpu{});
    faceMats_ = faceMats;
    if (faceMats_.empty()) faceMats_.push_back(0);
    trans_.clear();
    bool needRebuild = false;
    for (auto& b : batches)
        if (b.mesh && !b.instances.empty() && !ranges_.count(b.mesh.get())) { needRebuild = true; break; }
    // Also release GPU memory when lots of meshes are no longer used.
    if (!needRebuild && uploaded_.size() > batches.size() * 2 + 16) needRebuild = true;
    if (needRebuild) rebuildGeometry(batches);

    instances_.clear();
    meshCmds_.clear();
    lineCmds_.clear();
    stats = {};
    for (auto& b : batches) {
        if (!b.mesh || b.instances.empty()) continue;
        auto it = ranges_.find(b.mesh.get());
        if (it == ranges_.end()) continue;
        const MeshRange& r = it->second;
        uint32_t first = (uint32_t)instances_.size();
        for (auto& in : b.instances) {
            InstanceGpu g{};
            std::memcpy(g.m, glm::value_ptr(in.xform), 64);
            std::memcpy(g.color, glm::value_ptr(in.color), 16);
            g.info[0] = in.bodyId;
            g.info[1] = in.flags & 15u;
            g.info[2] = r.faceBase;
            g.info[3] = in.selCount ? ((in.selStart << 8) | std::min<uint32_t>(in.selCount, 255u)) : 0u;
            g.info2[0] = in.material < mats_.size() ? in.material : 0u;
            g.info2[1] = in.faceTable;
            if (in.transparent && r.indexCount) {
                glm::vec3 c = b.mesh->bounds.valid() ? glm::vec3(in.xform * glm::vec4(b.mesh->bounds.center(), 1.f)) : glm::vec3(in.xform[3]);
                trans_.push_back({(uint32_t)instances_.size(), r.firstIndex, r.indexCount, r.vertexOffset, c});
            }
            instances_.push_back(g);
        }
        uint32_t n = (uint32_t)b.instances.size();
        if (r.indexCount) meshCmds_.push_back({r.indexCount, n, r.firstIndex, r.vertexOffset, first});
        if (r.edgeCount) lineCmds_.push_back({r.edgeCount, n, r.edgeFirst, first});
        stats.meshes++;
        stats.instances += n;
        stats.triangles += (uint64_t)(r.indexCount / 3) * n;
    }
    sceneVersion_++;
}

void Renderer::writeFrameScene(Frame& f) {
    auto ensure = [&](GpuBuffer& b, VkDeviceSize need, VkBufferUsageFlags usage) {
        if (b.size < need) createBuffer(b, need * 3 / 2 + 256, usage, true);
    };
    ensure(f.instances, instances_.size() * sizeof(InstanceGpu), VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    ensure(f.meshCmds, meshCmds_.size() * sizeof(VkDrawIndexedIndirectCommand), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    ensure(f.lineCmds, lineCmds_.size() * sizeof(VkDrawIndirectCommand), VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT);
    if (f.sel.size < selFaces_.size() * 4) {
        createBuffer(f.sel, selFaces_.size() * 4 * 3 / 2 + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        bindSelBuffer(f);   // safe: this frame's fence has been waited on
    }
    if (f.sel.mapped) {
        std::memcpy(f.sel.mapped, selFaces_.data(), selFaces_.size() * 4);
        vmaFlushAllocation(ctx_->vma, f.sel.alloc, 0, VK_WHOLE_SIZE);
    }
    if (f.mats.size < mats_.size() * sizeof(MaterialGpu)) {
        createBuffer(f.mats, mats_.size() * sizeof(MaterialGpu) * 3 / 2 + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        bindStorage(f, 2, f.mats);
    }
    if (f.mats.mapped) {
        std::memcpy(f.mats.mapped, mats_.data(), mats_.size() * sizeof(MaterialGpu));
        vmaFlushAllocation(ctx_->vma, f.mats.alloc, 0, VK_WHOLE_SIZE);
    }
    if (f.faceMats.size < faceMats_.size() * 4) {
        createBuffer(f.faceMats, faceMats_.size() * 4 * 3 / 2 + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
        bindStorage(f, 3, f.faceMats);
    }
    if (f.faceMats.mapped) {
        std::memcpy(f.faceMats.mapped, faceMats_.data(), faceMats_.size() * 4);
        vmaFlushAllocation(ctx_->vma, f.faceMats.alloc, 0, VK_WHOLE_SIZE);
    }
    if (f.instances.mapped && !instances_.empty()) std::memcpy(f.instances.mapped, instances_.data(), instances_.size() * sizeof(InstanceGpu));
    if (f.meshCmds.mapped && !meshCmds_.empty()) std::memcpy(f.meshCmds.mapped, meshCmds_.data(), meshCmds_.size() * sizeof(VkDrawIndexedIndirectCommand));
    if (f.lineCmds.mapped && !lineCmds_.empty()) std::memcpy(f.lineCmds.mapped, lineCmds_.data(), lineCmds_.size() * sizeof(VkDrawIndirectCommand));
    vmaFlushAllocation(ctx_->vma, f.instances.alloc, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(ctx_->vma, f.meshCmds.alloc, 0, VK_WHOLE_SIZE);
    vmaFlushAllocation(ctx_->vma, f.lineCmds.alloc, 0, VK_WHOLE_SIZE);
    f.version = sceneVersion_;
}

bool Renderer::pickInFlight() const {
    for (auto& f : frames_)
        if (f.pickPending) return true;
    return false;
}

bool Renderer::recreateSwapchain() {
    std::string err;
    if (!ctx_->createSwapchain(err)) {
        if (!err.empty()) log::error(err);
        return false;
    }
    destroyAttachments();
    createAttachments();
    if (imguiReady_) ImGui_ImplVulkan_SetMinImageCount(ctx_->minImageCount);
    resizeRequested_ = false;
    return true;
}

// ------------------------------------------------------------------ frame

bool Renderer::drawFrame(const FrameParams& fp, ImDrawData* ui) {
    VkDevice dev = ctx_->device;
    if (resizeRequested_ || !ctx_->swapchain) {
        if (!recreateSwapchain()) return false;
    }
    Frame& f = frames_[frameIndex_];
    vkWaitForFences(dev, 1, &f.fence, VK_TRUE, UINT64_MAX);

    if (f.pickPending) {
        vmaInvalidateAllocation(ctx_->vma, f.readback.alloc, 0, VK_WHOLE_SIZE);
        const uint32_t* px = static_cast<const uint32_t*>(f.readback.mapped);
        pick_.valid = true;
        pick_.body = px ? px[0] : 0;
        pick_.face = px ? px[1] : 0;
        pick_.x = f.pickX;
        pick_.y = f.pickY;
        f.pickPending = false;
    }
    if (f.version != sceneVersion_) writeFrameScene(f);

    uint32_t imageIndex = 0;
    VkResult acq = vkAcquireNextImageKHR(dev, ctx_->swapchain, UINT64_MAX, f.acquired, VK_NULL_HANDLE, &imageIndex);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) { resizeRequested_ = true; return false; }
    if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) {
        log::error(std::string("acquire failed: ") + vkResultName(acq));
        resizeRequested_ = true;
        return false;
    }

    vkResetFences(dev, 1, &f.fence);
    vkResetCommandPool(dev, f.pool, 0);
    VkCommandBuffer cmd = f.cmd;
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);

    const VkExtent2D ext = ctx_->extent;
    VkViewport viewport{0, 0, (float)ext.width, (float)ext.height, 0.f, 1.f};
    VkRect2D full{{0, 0}, ext};
    glm::mat4 vp = fp.proj * fp.view;
    MeshPC pc{};
    pc.viewProj = vp;
    pc.camPos = glm::vec4(fp.eye, fp.ortho ? 1.f : 0.f);
    pc.viewDir = glm::vec4(fp.forward, 0.f);
    pc.hover = glm::uvec4(fp.hoverBody, fp.hoverFace, fp.hoverMode, 0);
    pc.params = glm::vec4(0.f, fp.sectionAxis, fp.sectionOffset, 0.f);   // x: 0 solid pass, 1 see through pass

    VkDeviceSize zero = 0;
    auto bindMeshState = [&](VkPipeline pipe) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, meshLayout_, 0, 1, &f.set, 0, nullptr);
        vkCmdPushConstants(cmd, meshLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
        VkBuffer vbs[2] = {vbo_.buf, f.instances.buf};
        VkDeviceSize offs[2] = {0, 0};
        vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
        vkCmdBindIndexBuffer(cmd, ibo_.buf, 0, VK_INDEX_TYPE_UINT32);
    };
    auto drawMeshes = [&] {
        uint32_t n = (uint32_t)meshCmds_.size();
        if (ctx_->multiDrawIndirect) vkCmdDrawIndexedIndirect(cmd, f.meshCmds.buf, 0, n, sizeof(VkDrawIndexedIndirectCommand));
        else for (uint32_t i = 0; i < n; ++i) vkCmdDrawIndexedIndirect(cmd, f.meshCmds.buf, i * sizeof(VkDrawIndexedIndirectCommand), 1, sizeof(VkDrawIndexedIndirectCommand));
    };
    stats.drawCalls = 0;

    // ---- 1. ID pass: one pixel under the cursor (body id, face id) -> readback buffer
    bool doPick = fp.pick && !meshCmds_.empty() && fp.pickX >= 0 && fp.pickY >= 0 &&
                  fp.pickX < (int)ext.width && fp.pickY < (int)ext.height;
    if (fp.pick && !doPick) {
        // Nothing to hit: report "no hit" without GPU work.
        pick_ = {true, 0, 0, fp.pickX, fp.pickY};
    }
    if (doPick) {
        imageBarrier(cmd, idColor_.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        imageBarrier(cmd, idDepth_.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                     kAttachStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, kAttachStages,
                     VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = idColor_.view;
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color.uint32[0] = 0;
        ca.clearValue.color.uint32[1] = 0;
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = idDepth_.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        da.clearValue.depthStencil.depth = 0.f;
        VkRect2D px{{fp.pickX, fp.pickY}, {1, 1}};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = px;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &px);
        bindMeshState(idPipe_);
        drawMeshes();
        vkCmdEndRendering(cmd);
        imageBarrier(cmd, idColor_.img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {fp.pickX, fp.pickY, 0};
        region.imageExtent = {1, 1, 1};
        vkCmdCopyImageToBuffer(cmd, idColor_.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, f.readback.buf, 1, &region);
        VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        bb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
        bb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
        bb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
        bb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
        bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bb.buffer = f.readback.buf;
        bb.size = VK_WHOLE_SIZE;
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.bufferMemoryBarrierCount = 1;
        di.pBufferMemoryBarriers = &bb;
        vkCmdPipelineBarrier2(cmd, &di);
        f.pickPending = true;
        f.pickX = fp.pickX;
        f.pickY = fp.pickY;
        stats.drawCalls++;
    }

    // ---- 2. Main pass: MSAA scene resolved into the swapchain image
    VkImage swapImg = ctx_->images[imageIndex];
    imageBarrier(cmd, swapImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    imageBarrier(cmd, msaaColor_.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT,
                 VK_IMAGE_ASPECT_COLOR_BIT);
    imageBarrier(cmd, msaaDepth_.img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                 kAttachStages, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, kAttachStages,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    {
        bool ms = ctx_->msaa != VK_SAMPLE_COUNT_1_BIT;
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = ms ? msaaColor_.view : ctx_->views[imageIndex];
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        if (ms) {
            ca.resolveMode = VK_RESOLVE_MODE_AVERAGE_BIT;
            ca.resolveImageView = ctx_->views[imageIndex];
            ca.resolveImageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        }
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        ca.storeOp = ms ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
        ca.clearValue.color = {{0.9f, 0.92f, 0.94f, 1.f}};
        VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        da.imageView = msaaDepth_.view;
        da.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        da.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        da.clearValue.depthStencil.depth = 0.f;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = full;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        ri.pDepthAttachment = &da;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdSetViewport(cmd, 0, 1, &viewport);
        vkCmdSetScissor(cmd, 0, 1, &full);

        // Background gradient
        BgPC bg{glm::vec4(fp.bgTop, 1.f), glm::vec4(fp.bgBottom, 1.f)};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, bgPipe_);
        vkCmdPushConstants(cmd, bgLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof bg, &bg);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        stats.drawCalls++;

        if (!meshCmds_.empty()) {
            bindMeshState(meshPipe_);
            drawMeshes();
            stats.drawCalls += ctx_->multiDrawIndirect ? 1 : (uint32_t)meshCmds_.size();
        }
        if (fp.edges && !lineCmds_.empty()) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, linePipe_);
            vkCmdPushConstants(cmd, meshLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pc, &pc);
            if (ctx_->wideLines) vkCmdSetLineWidth(cmd, std::min(1.0f, ctx_->lineWidthMax));
            VkBuffer vbs[2] = {edgeVbo_.buf, f.instances.buf};
            VkDeviceSize offs[2] = {0, 0};
            vkCmdBindVertexBuffers(cmd, 0, 2, vbs, offs);
            uint32_t n = (uint32_t)lineCmds_.size();
            if (ctx_->multiDrawIndirect) vkCmdDrawIndirect(cmd, f.lineCmds.buf, 0, n, sizeof(VkDrawIndirectCommand));
            else for (uint32_t i = 0; i < n; ++i) vkCmdDrawIndirect(cmd, f.lineCmds.buf, i * sizeof(VkDrawIndirectCommand), 1, sizeof(VkDrawIndirectCommand));
            stats.drawCalls += ctx_->multiDrawIndirect ? 1 : n;
        }
        if (fp.grid || fp.origin) {
            GridPC g{};
            g.viewProj = vp;
            g.camPos = glm::vec4(fp.eye, 0.f);
            g.params = glm::vec4(fp.gridExtent, fp.gridMinor, fp.gridExtent, fp.grid ? 0.f : 1.f);
            g.flags = glm::vec4(fp.origin ? 1.f : 0.f, fp.gridColor);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, gridPipe_);
            vkCmdPushConstants(cmd, gridLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof g, &g);
            vkCmdDraw(cmd, 6, 1, 0, 0);
            stats.drawCalls++;
        }
        // See through bodies last, far to near, each drawn inside faces first.
        if (!trans_.empty()) {
            std::vector<std::pair<float, size_t>> order;
            order.reserve(trans_.size());
            for (size_t i = 0; i < trans_.size(); ++i) order.push_back({glm::length(trans_[i].center - fp.eye), i});
            std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
            MeshPC gp = pc;
            gp.params.x = 1.f;
            bindMeshState(glassBackPipe_);
            vkCmdPushConstants(cmd, meshLayout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof gp, &gp);
            for (auto& [d, i] : order) {
                const TransDraw& t = trans_[i];
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glassBackPipe_);
                vkCmdDrawIndexed(cmd, t.indexCount, 1, t.firstIndex, t.vertexOffset, t.instance);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, glassFrontPipe_);
                vkCmdDrawIndexed(cmd, t.indexCount, 1, t.firstIndex, t.vertexOffset, t.instance);
                stats.drawCalls += 2;
            }
        }
        (void)zero;
        vkCmdEndRendering(cmd);
    }

    // ---- 3. UI pass on top of the resolved image
    if (ui && imguiReady_) {
        VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        ca.imageView = ctx_->views[imageIndex];
        ca.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        ca.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        ca.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = full;
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &ca;
        imageBarrier(cmd, swapImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_IMAGE_ASPECT_COLOR_BIT);
        vkCmdBeginRendering(cmd, &ri);
        ImGui_ImplVulkan_RenderDrawData(ui, cmd);
        vkCmdEndRendering(cmd);
    }

    // ---- 4. Window screenshot (asked for by the MCP server): copy the finished image out.
    bool capturing = false;
    if (captureReq_ && ctx_->canCapture) {
        VkDeviceSize need = (VkDeviceSize)ext.width * ext.height * 4;
        if (captureBuf_.size < need) {
            destroyBuffer(captureBuf_);
            createBuffer(captureBuf_, need, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true);
        }
        if (captureBuf_.buf) {
            imageBarrier(cmd, swapImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {ext.width, ext.height, 1};
            vkCmdCopyImageToBuffer(cmd, swapImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, captureBuf_.buf, 1, &region);
            VkBufferMemoryBarrier2 bb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            bb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            bb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            bb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
            bb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            bb.srcQueueFamilyIndex = bb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            bb.buffer = captureBuf_.buf;
            bb.size = VK_WHOLE_SIZE;
            VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            di.bufferMemoryBarrierCount = 1;
            di.pBufferMemoryBarriers = &bb;
            vkCmdPipelineBarrier2(cmd, &di);
            imageBarrier(cmd, swapImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
                         VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_ASPECT_COLOR_BIT);
            capturing = true;
        }
    }
    if (!capturing)
        imageBarrier(cmd, swapImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_NONE, 0, VK_IMAGE_ASPECT_COLOR_BIT);
    vkEndCommandBuffer(cmd);

    VkSemaphoreSubmitInfo waitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitInfo.semaphore = f.acquired;
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphoreSubmitInfo sigInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    sigInfo.semaphore = ctx_->renderDone[imageIndex];
    sigInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cbi.commandBuffer = cmd;
    VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    si.waitSemaphoreInfoCount = 1;
    si.pWaitSemaphoreInfos = &waitInfo;
    si.commandBufferInfoCount = 1;
    si.pCommandBufferInfos = &cbi;
    si.signalSemaphoreInfoCount = 1;
    si.pSignalSemaphoreInfos = &sigInfo;
    VkResult sr = vkQueueSubmit2(ctx_->queue, 1, &si, f.fence);
    if (sr != VK_SUCCESS) {
        log::error(std::string("queue submit failed: ") + vkResultName(sr));
        f.pickPending = false;
        if (sr == VK_ERROR_DEVICE_LOST) throw std::runtime_error("GPU device lost");
        return false;
    }
    if (capturing) {
        // A one off wait: the picture is wanted now, and it costs one frame.
        vkWaitForFences(dev, 1, &f.fence, VK_TRUE, UINT64_MAX);
        vmaInvalidateAllocation(ctx_->vma, captureBuf_.alloc, 0, VK_WHOLE_SIZE);
        const uint8_t* src = static_cast<const uint8_t*>(captureBuf_.mapped);
        if (src) {
            size_t n = (size_t)ext.width * ext.height;
            capture_.resize(n * 4);
            bool bgr = ctx_->swapFormat == VK_FORMAT_B8G8R8A8_UNORM || ctx_->swapFormat == VK_FORMAT_B8G8R8A8_SRGB;
            for (size_t i = 0; i < n; ++i) {
                capture_[i * 4 + 0] = src[i * 4 + (bgr ? 2 : 0)];
                capture_[i * 4 + 1] = src[i * 4 + 1];
                capture_[i * 4 + 2] = src[i * 4 + (bgr ? 0 : 2)];
                capture_[i * 4 + 3] = 255;
            }
            captureW_ = (int)ext.width;
            captureH_ = (int)ext.height;
            captureReady_ = true;
        }
        captureReq_ = false;
    }

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &ctx_->renderDone[imageIndex];
    pi.swapchainCount = 1;
    pi.pSwapchains = &ctx_->swapchain;
    pi.pImageIndices = &imageIndex;
    VkResult pr = vkQueuePresentKHR(ctx_->queue, &pi);
    if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR || acq == VK_SUBOPTIMAL_KHR) resizeRequested_ = true;
    else if (pr != VK_SUCCESS) log::warn(std::string("present: ") + vkResultName(pr));

    frameIndex_ = (frameIndex_ + 1) % kFrames;
    return true;
}

} // namespace kst
