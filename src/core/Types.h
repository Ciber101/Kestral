#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace kst {

struct Aabb {
    glm::vec3 min{1e30f}, max{-1e30f};
    void add(const glm::vec3& p) { min = glm::min(min, p); max = glm::max(max, p); }
    void add(const Aabb& b) { if (b.valid()) { add(b.min); add(b.max); } }
    bool valid() const { return min.x <= max.x; }
    glm::vec3 center() const { return valid() ? (min + max) * 0.5f : glm::vec3(0); }
    float diag() const { return valid() ? glm::length(max - min) : 0.f; }
    Aabb transformed(const glm::mat4& m) const {
        Aabb r;
        if (!valid()) return r;
        for (int i = 0; i < 8; ++i) {
            glm::vec3 c((i & 1) ? max.x : min.x, (i & 2) ? max.y : min.y, (i & 4) ? max.z : min.z);
            r.add(glm::vec3(m * glm::vec4(c, 1.f)));
        }
        return r;
    }
};

// Display mesh for one geometry prototype. Shared (immutable) between all instances.
struct MeshData {
    std::vector<float> verts;        // x y z nx ny nz
    std::vector<uint32_t> indices;   // triangles
    std::vector<uint32_t> triFace;   // face index per triangle
    std::vector<float> edgeVerts;    // line list x y z pairs (B-rep edges)
    std::vector<uint32_t> faceColors; // optional, per B-rep face: RGBA8 (A = 0 means use the body color)
    uint32_t faceCount = 0;
    uint32_t edgeCount = 0;
    Aabb bounds;
    size_t vertexCount() const { return verts.size() / 6; }
    size_t triCount() const { return indices.size() / 3; }
};
using MeshPtr = std::shared_ptr<const MeshData>;

// Opaque B-rep handle so most code never includes OpenCASCADE headers.
struct ShapeHandle;
using ShapePtr = std::shared_ptr<const ShapeHandle>;

struct CancelToken {
    std::atomic<bool> cancelled{false};
    bool isCancelled() const { return cancelled.load(std::memory_order_relaxed); }
};
using CancelPtr = std::shared_ptr<CancelToken>;

// UTF-8 string <-> filesystem path helpers (Windows paths can contain any characters).
inline std::filesystem::path u8path(const std::string& s) {
    return std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}
inline std::string pathToU8(const std::filesystem::path& p) {
    auto u = p.u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

inline std::string toLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

} // namespace kst
