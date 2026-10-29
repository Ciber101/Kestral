#pragma once
#include "core/Types.h"
#include "render/VkContext.h"
#include <unordered_map>
#include <vector>

struct ImDrawData;

namespace kst {

enum InstanceFlags : uint32_t { kInstSelected = 1, kInstHover = 2 };

// One appearance as the shader reads it (see shaders/mesh.frag).
struct MaterialGpu {
    glm::vec4 c0{0.72f, 0.74f, 0.77f, 1.f};   // base color, opacity
    glm::vec4 c1{0.25f, 0.25f, 0.27f, 0.f};   // second color, pattern id
    glm::vec4 c2{0.f, 0.45f, 10.f, 0.f};      // metal, roughness, pattern scale (mm), glow
};

struct RenderInstance {
    glm::mat4 xform{1.f};
    glm::vec4 color{0.72f, 0.74f, 0.77f, 1.f};
    uint32_t bodyId = 0;
    uint32_t flags = 0;
    uint32_t selStart = 0, selCount = 0;   // range in the selected-face list (count <= 255)
    uint32_t material = 0;                 // index into the scene's materials
    uint32_t faceTable = 0;                // start + 1 of this instance's per face materials (0 = none)
    bool transparent = false;              // some of it is see through: drawn again in the blended pass
};

// One unique mesh and every place it is drawn. Patterns become one batch with many instances.
struct SceneBatch {
    MeshPtr mesh;
    std::vector<RenderInstance> instances;
};

struct FrameParams {
    glm::mat4 view{1.f}, proj{1.f};
    glm::vec3 eye{0.f}, forward{0, 0, -1};
    bool ortho = false;
    uint32_t hoverBody = 0, hoverFace = 0;   // face is +1 encoded, 0 = none
    uint32_t hoverMode = 0;                  // 0 body, 1 face
    bool edges = true, grid = true, origin = true;
    glm::vec3 bgTop{0.976f, 0.980f, 0.988f}, bgBottom{0.800f, 0.835f, 0.870f}, gridColor{0.42f, 0.45f, 0.50f};
    float gridMinor = 10.f, gridExtent = 1000.f;
    bool pick = false;
    int pickX = 0, pickY = 0;
    float sectionAxis = 0.f;    // 0 off. 1 X, 2 Y, 3 Z. Negative keeps the other side.
    float sectionOffset = 0.f;
};

struct PickResult {
    bool valid = false;
    uint32_t body = 0;
    uint32_t face = 0;   // +1 encoded, 0 = none
    int x = 0, y = 0;
};

class Renderer {
public:
    bool init(VkContext* ctx, std::string& err);
    bool initImGui();
    void shutdown();

    // Cheap when the set of meshes is unchanged (only instance data is rebuilt).
    // selFaces: face ids referenced by RenderInstance::selStart / selCount (selected faces glow).
    // mats: every appearance in the scene; faceMats: per face material tables (material + 1, 0 = body's).
    void setScene(const std::vector<SceneBatch>& batches, const std::vector<uint32_t>& selFaces,
                  const std::vector<MaterialGpu>& mats, const std::vector<uint32_t>& faceMats);
    // Records and presents one frame. Returns false if skipped (minimized or swapchain rebuilt).
    bool drawFrame(const FrameParams& fp, ImDrawData* ui);
    const PickResult& lastPick() const { return pick_; }
    bool pickInFlight() const;
    void requestResize() { resizeRequested_ = true; }
    // A picture of the whole window (scene and interface) as the next frame presents it.
    void requestCapture() { captureReq_ = true; }
    bool canCapture() const;
    // True once a requested picture is ready (then it is handed over and cleared). RGBA.
    bool takeCapture(std::vector<uint8_t>& rgba, int& w, int& h);

    struct Stats { uint32_t meshes = 0, instances = 0, drawCalls = 0; uint64_t triangles = 0; } stats;

private:
    struct GpuBuffer { VkBuffer buf = VK_NULL_HANDLE; VmaAllocation alloc = nullptr; void* mapped = nullptr; VkDeviceSize size = 0; };
    struct GpuImage { VkImage img = VK_NULL_HANDLE; VmaAllocation alloc = nullptr; VkImageView view = VK_NULL_HANDLE; };
    struct MeshRange { uint32_t firstIndex, indexCount; int32_t vertexOffset; uint32_t faceBase, edgeFirst, edgeCount; };
    struct InstanceGpu { float m[16]; float color[4]; uint32_t info[4]; uint32_t info2[4]; };
    struct TransDraw { uint32_t instance; uint32_t firstIndex, indexCount; int32_t vertexOffset; glm::vec3 center; };
    struct Frame {
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore acquired = VK_NULL_HANDLE;
        GpuBuffer instances, meshCmds, lineCmds, readback, sel, mats, faceMats;
        VkDescriptorSet set = VK_NULL_HANDLE;
        uint64_t version = 0;
        bool pickPending = false;
        int pickX = 0, pickY = 0;
    };
    static constexpr int kFrames = 2;

    bool createBuffer(GpuBuffer& b, VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible, bool readback = false);
    void destroyBuffer(GpuBuffer& b);
    bool createImage(GpuImage& img, VkFormat fmt, VkImageUsageFlags usage, VkSampleCountFlagBits samples, VkImageAspectFlags aspect);
    void destroyImage(GpuImage& img);
    bool upload(GpuBuffer& dst, const void* data, VkDeviceSize size, VkBufferUsageFlags usage);
    bool createAttachments();
    void destroyAttachments();
    bool createPipelines(std::string& err);
    VkPipeline makePipeline(const uint32_t* vs, size_t vsSize, const uint32_t* fs, size_t fsSize, VkPipelineLayout layout,
                            int kind, VkSampleCountFlagBits samples, VkFormat colorFmt, bool depthWrite);
    void rebuildGeometry(const std::vector<SceneBatch>& batches);
    void writeFrameScene(Frame& f);
    bool recreateSwapchain();

    VkContext* ctx_ = nullptr;
    Frame frames_[kFrames];
    int frameIndex_ = 0;
    VkCommandPool uploadPool_ = VK_NULL_HANDLE;
    VkFence uploadFence_ = VK_NULL_HANDLE;

    GpuImage msaaColor_, msaaDepth_, idColor_, idDepth_;
    VkFormat depthFormat_ = VK_FORMAT_D32_SFLOAT;
    VkFormat idFormat_ = VK_FORMAT_R32G32_UINT;

    VkDescriptorSetLayout setLayout_ = VK_NULL_HANDLE;
    VkDescriptorPool descPool_ = VK_NULL_HANDLE;
    void bindSelBuffer(Frame& f);
    void bindStorage(Frame& f, uint32_t binding, const GpuBuffer& b);
    VkPipelineLayout meshLayout_ = VK_NULL_HANDLE, bgLayout_ = VK_NULL_HANDLE, gridLayout_ = VK_NULL_HANDLE;
    VkPipeline meshPipe_ = VK_NULL_HANDLE, idPipe_ = VK_NULL_HANDLE, linePipe_ = VK_NULL_HANDLE, bgPipe_ = VK_NULL_HANDLE, gridPipe_ = VK_NULL_HANDLE;
    VkPipeline glassBackPipe_ = VK_NULL_HANDLE, glassFrontPipe_ = VK_NULL_HANDLE;   // see through pass

    // Geometry (all unique meshes packed in shared buffers)
    GpuBuffer vbo_, ibo_, faceBuf_, edgeVbo_;
    std::vector<MeshPtr> uploaded_;
    std::unordered_map<const MeshData*, MeshRange> ranges_;

    // Scene (CPU side, copied into each frame's host visible buffers when changed)
    std::vector<InstanceGpu> instances_;
    std::vector<VkDrawIndexedIndirectCommand> meshCmds_;
    std::vector<VkDrawIndirectCommand> lineCmds_;
    std::vector<uint32_t> selFaces_;
    std::vector<MaterialGpu> mats_;
    std::vector<uint32_t> faceMats_;
    std::vector<TransDraw> trans_;
    uint64_t sceneVersion_ = 1;

    PickResult pick_;
    bool resizeRequested_ = false;
    bool imguiReady_ = false;
    bool captureReq_ = false, captureReady_ = false;
    std::vector<uint8_t> capture_;
    int captureW_ = 0, captureH_ = 0;
    GpuBuffer captureBuf_;
};

} // namespace kst
