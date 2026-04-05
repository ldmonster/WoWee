#include "rendering/m2_renderer.hpp"
#include "rendering/m2_model_classifier.hpp"
#include "rendering/vk_context.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/vk_frame_data.hpp"
#include "rendering/camera.hpp"
#include "rendering/frustum.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "core/logger.hpp"
#include "core/profiler.hpp"
#include <chrono>
#include <cctype>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <unordered_set>
#include <functional>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <random>
#include <limits>
#include <future>
#include <thread>

namespace wowee {
namespace rendering {

namespace {

// Seeded RNG for animation time offsets and variation timers. Using rand()
// without srand() produces the same sequence every launch, causing all
// doodads (trees, torches, grass) to sway/flicker in sync.
std::mt19937& rng() {
    static std::mt19937 gen(std::random_device{}());
    return gen;
}
uint32_t randRange(uint32_t maxExclusive) {
    if (maxExclusive == 0) return 0;
    return std::uniform_int_distribution<uint32_t>(0, maxExclusive - 1)(rng());
}
float randFloat(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(rng());
}

// Shared lava UV scroll timer — ensures consistent animation across all render passes
const auto kLavaAnimStart = std::chrono::steady_clock::now();

bool envFlagEnabled(const char* key, bool defaultValue) {
    const char* raw = std::getenv(key);
    if (!raw || !*raw) return defaultValue;
    std::string v(raw);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return !(v == "0" || v == "false" || v == "off" || v == "no");
}

static constexpr uint32_t kParticleFlagRandomized = 0x40;
static constexpr uint32_t kParticleFlagTiled = 0x80;
static constexpr float kSmokeEmitInterval = 1.0f / 48.0f;

float computeGroundDetailDownOffset(const M2ModelGPU& model, float scale) {
    // Keep a tiny sink to avoid hovering, but cap pivot compensation so details
    // don't get pushed below the terrain on models with large positive boundMin.
    const float pivotComp = glm::clamp(std::max(0.0f, model.boundMin.z * scale), 0.0f, 0.10f);
    const float terrainSink = 0.03f;
    return pivotComp + terrainSink;
}

void getTightCollisionBounds(const M2ModelGPU& model, glm::vec3& outMin, glm::vec3& outMax) {
    glm::vec3 center = (model.boundMin + model.boundMax) * 0.5f;
    glm::vec3 half = (model.boundMax - model.boundMin) * 0.5f;

    // Per-shape collision fitting:
    // - small solid props (boxes/crates/chests): tighter than full mesh, but
    //   larger than default to prevent walk-through on narrow objects
    // - default: tighter fit (avoid oversized blockers)
    // - stepped low platforms (tree curbs/planters): wider XY + lower Z
    if (model.collisionTreeTrunk) {
        // Tree trunk: proportional cylinder at the base of the tree.
        float modelHoriz = std::max(model.boundMax.x - model.boundMin.x,
                                    model.boundMax.y - model.boundMin.y);
        float trunkHalf = std::clamp(modelHoriz * 0.05f, 0.5f, 5.0f);
        half.x = trunkHalf;
        half.y = trunkHalf;
        // Height proportional to trunk width, capped at 3.5 units.
        half.z = std::min(trunkHalf * 2.5f, 3.5f);
        // Shift center down so collision is at the base (trunk), not mid-canopy.
        center.z = model.boundMin.z + half.z;
    } else if (model.collisionNarrowVerticalProp) {
        // Tall thin props (lamps/posts): keep passable gaps near walls.
        half.x *= 0.30f;
        half.y *= 0.30f;
        half.z *= 0.96f;
    } else if (model.collisionSmallSolidProp) {
        // Keep full tight mesh bounds for small solid props to avoid clip-through.
        half.x *= 1.00f;
        half.y *= 1.00f;
        half.z *= 1.00f;
    } else if (model.collisionSteppedLowPlatform) {
        half.x *= 0.98f;
        half.y *= 0.98f;
        half.z *= 0.52f;
    } else {
        half.x *= 0.66f;
        half.y *= 0.66f;
        half.z *= 0.76f;
    }

    outMin = center - half;
    outMax = center + half;
}

float getEffectiveCollisionTopLocal(const M2ModelGPU& model,
                                    const glm::vec3& localPos,
                                    const glm::vec3& localMin,
                                    const glm::vec3& localMax) {
    if (!model.collisionSteppedFountain && !model.collisionSteppedLowPlatform) {
        return localMax.z;
    }

    glm::vec2 center((localMin.x + localMax.x) * 0.5f, (localMin.y + localMax.y) * 0.5f);
    glm::vec2 half((localMax.x - localMin.x) * 0.5f, (localMax.y - localMin.y) * 0.5f);
    if (half.x < 1e-4f || half.y < 1e-4f) {
        return localMax.z;
    }

    float nx = (localPos.x - center.x) / half.x;
    float ny = (localPos.y - center.y) / half.y;
    float r = std::sqrt(nx * nx + ny * ny);

    float h = localMax.z - localMin.z;
    if (model.collisionSteppedFountain) {
        if (r > 0.85f) return localMin.z + h * 0.18f;  // outer lip
        if (r > 0.65f) return localMin.z + h * 0.36f;  // mid step
        if (r > 0.45f) return localMin.z + h * 0.54f;  // inner step
        if (r > 0.28f) return localMin.z + h * 0.70f;  // center platform / statue base
        if (r > 0.14f) return localMin.z + h * 0.84f;  // statue body / sword
        return localMin.z + h * 0.96f;                  // statue head / top
    }

    // Low square curb/planter profile:
    // use edge distance (not radial) so corner blocks don't become too low and
    // clip-through at diagonals.
    float edge = std::max(std::abs(nx), std::abs(ny));
    if (edge > 0.92f) return localMin.z + h * 0.06f;
    if (edge > 0.72f) return localMin.z + h * 0.30f;
    return localMin.z + h * 0.62f;
}

bool segmentIntersectsAABB(const glm::vec3& from, const glm::vec3& to,
                           const glm::vec3& bmin, const glm::vec3& bmax,
                           float& outEnterT) {
    glm::vec3 d = to - from;
    float tEnter = 0.0f;
    float tExit = 1.0f;

    for (int axis = 0; axis < 3; axis++) {
        if (std::abs(d[axis]) < 1e-6f) {
            if (from[axis] < bmin[axis] || from[axis] > bmax[axis]) {
                return false;
            }
            continue;
        }

        float inv = 1.0f / d[axis];
        float t0 = (bmin[axis] - from[axis]) * inv;
        float t1 = (bmax[axis] - from[axis]) * inv;
        if (t0 > t1) std::swap(t0, t1);

        tEnter = std::max(tEnter, t0);
        tExit = std::min(tExit, t1);
        if (tEnter > tExit) return false;
    }

    outEnterT = tEnter;
    return tExit >= 0.0f && tEnter <= 1.0f;
}

void transformAABB(const glm::mat4& modelMatrix,
                   const glm::vec3& localMin,
                   const glm::vec3& localMax,
                   glm::vec3& outMin,
                   glm::vec3& outMax) {
    const glm::vec3 corners[8] = {
        {localMin.x, localMin.y, localMin.z},
        {localMin.x, localMin.y, localMax.z},
        {localMin.x, localMax.y, localMin.z},
        {localMin.x, localMax.y, localMax.z},
        {localMax.x, localMin.y, localMin.z},
        {localMax.x, localMin.y, localMax.z},
        {localMax.x, localMax.y, localMin.z},
        {localMax.x, localMax.y, localMax.z}
    };

    outMin = glm::vec3(std::numeric_limits<float>::max());
    outMax = glm::vec3(-std::numeric_limits<float>::max());
    for (const auto& c : corners) {
        glm::vec3 wc = glm::vec3(modelMatrix * glm::vec4(c, 1.0f));
        outMin = glm::min(outMin, wc);
        outMax = glm::max(outMax, wc);
    }
}

float pointAABBDistanceSq(const glm::vec3& p, const glm::vec3& bmin, const glm::vec3& bmax) {
    glm::vec3 q = glm::clamp(p, bmin, bmax);
    glm::vec3 d = p - q;
    return glm::dot(d, d);
}

// Möller–Trumbore ray-triangle intersection.
// Returns distance along ray if hit, negative if miss.
float rayTriangleIntersect(const glm::vec3& origin, const glm::vec3& dir,
                           const glm::vec3& v0, const glm::vec3& v1, const glm::vec3& v2) {
    constexpr float EPSILON = 1e-6f;
    glm::vec3 e1 = v1 - v0;
    glm::vec3 e2 = v2 - v0;
    glm::vec3 h = glm::cross(dir, e2);
    float a = glm::dot(e1, h);
    if (a > -EPSILON && a < EPSILON) return -1.0f;
    float f = 1.0f / a;
    glm::vec3 s = origin - v0;
    float u = f * glm::dot(s, h);
    if (u < 0.0f || u > 1.0f) return -1.0f;
    glm::vec3 q = glm::cross(s, e1);
    float v = f * glm::dot(dir, q);
    if (v < 0.0f || u + v > 1.0f) return -1.0f;
    float t = f * glm::dot(e2, q);
    return t > EPSILON ? t : -1.0f;
}

// Closest point on triangle to a point (Ericson, Real-Time Collision Detection §5.1.5).
glm::vec3 closestPointOnTriangle(const glm::vec3& p,
                                  const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;
    glm::vec3 bp = p - b;
    float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        return a + v * ab;
    }
    glm::vec3 cp = p - c;
    float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        return a + w * ac;
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + w * (c - b);
    }
    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom;
    float w = vc * denom;
    return a + ab * v + ac * w;
}

} // namespace

// Thread-local scratch buffers for collision queries (allows concurrent getFloorHeight calls)
static thread_local std::vector<size_t> tl_m2_candidateScratch;
static thread_local std::unordered_set<uint32_t> tl_m2_candidateIdScratch;
static thread_local std::vector<uint32_t> tl_m2_collisionTriScratch;

// Forward declaration (defined after animation helpers)
static void computeBoneMatrices(const M2ModelGPU& model, M2Instance& instance);

void M2Instance::updateModelMatrix() {
    modelMatrix = glm::mat4(1.0f);
    modelMatrix = glm::translate(modelMatrix, position);

    // Rotation in radians
    modelMatrix = glm::rotate(modelMatrix, rotation.x, glm::vec3(1.0f, 0.0f, 0.0f));
    modelMatrix = glm::rotate(modelMatrix, rotation.y, glm::vec3(0.0f, 1.0f, 0.0f));
    modelMatrix = glm::rotate(modelMatrix, rotation.z, glm::vec3(0.0f, 0.0f, 1.0f));

    modelMatrix = glm::scale(modelMatrix, glm::vec3(scale));
    invModelMatrix = glm::inverse(modelMatrix);
}

M2Renderer::M2Renderer() {
}

M2Renderer::~M2Renderer() {
    shutdown();
}

bool M2Renderer::initialize(VkContext* ctx, VkDescriptorSetLayout perFrameLayout,
                            pipeline::AssetManager* assets) {
    if (initialized_) { assetManager = assets; return true; }
    vkCtx_ = ctx;
    assetManager = assets;

    const unsigned hc = std::thread::hardware_concurrency();
    const size_t availableCores = (hc > 1u) ? static_cast<size_t>(hc - 1u) : 1ull;
    // Keep headroom for other frame tasks: M2 gets about half of non-main cores by default.
    const size_t defaultAnimThreads = std::max<size_t>(1, availableCores / 2);
    numAnimThreads_ = static_cast<uint32_t>(std::max<size_t>(
        1, envSizeOrDefault("WOWEE_M2_ANIM_THREADS", defaultAnimThreads)));
    LOG_INFO("Initializing M2 renderer (Vulkan, ", numAnimThreads_, " anim threads)...");

    VkDevice device = vkCtx_->getDevice();

    // --- Descriptor set layouts ---

    // Material set layout (set 1): binding 0 = sampler2D, binding 2 = M2Material UBO
    // (M2Params moved to push constants alongside model matrix)
    {
        VkDescriptorSetLayoutBinding bindings[2] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bindings[1].binding = 2;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 2;
        ci.pBindings = bindings;
        vkCreateDescriptorSetLayout(device, &ci, nullptr, &materialSetLayout_);
    }

    // Bone set layout (set 2): binding 0 = STORAGE_BUFFER (bone matrices)
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1;
        ci.pBindings = &binding;
        vkCreateDescriptorSetLayout(device, &ci, nullptr, &boneSetLayout_);
    }

    // Instance data set layout (set 3): binding 0 = STORAGE_BUFFER (per-instance data)
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1;
        ci.pBindings = &binding;
        vkCreateDescriptorSetLayout(device, &ci, nullptr, &instanceSetLayout_);
    }

    // Particle texture set layout (set 1 for particles): binding 0 = sampler2D
    {
        VkDescriptorSetLayoutBinding binding{};
        binding.binding = 0;
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ci.bindingCount = 1;
        ci.pBindings = &binding;
        vkCreateDescriptorSetLayout(device, &ci, nullptr, &particleTexLayout_);
    }

    // --- Descriptor pools ---
    {
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_MATERIAL_SETS + 256},
            {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, MAX_MATERIAL_SETS + 256},
        };
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = MAX_MATERIAL_SETS + 256;
        ci.poolSizeCount = 2;
        ci.pPoolSizes = sizes;
        ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        vkCreateDescriptorPool(device, &ci, nullptr, &materialDescPool_);
    }
    {
        VkDescriptorPoolSize sizes[] = {
            {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, MAX_BONE_SETS},
        };
        VkDescriptorPoolCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        ci.maxSets = MAX_BONE_SETS;
        ci.poolSizeCount = 1;
        ci.pPoolSizes = sizes;
        ci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        vkCreateDescriptorPool(device, &ci, nullptr, &boneDescPool_);
    }

    // Create a small identity-bone SSBO + descriptor set so that non-animated
    // draws always have a valid set 2 bound.  The Intel ANV driver segfaults
    // on vkCmdDrawIndexed when a declared descriptor set slot is unbound.
    {
        // Single identity matrix (bone 0 = identity)
        glm::mat4 identity(1.0f);
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.size = sizeof(glm::mat4);
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo allocInfo{};
        vmaCreateBuffer(ctx->getAllocator(), &bci, &aci,
                        &dummyBoneBuffer_, &dummyBoneAlloc_, &allocInfo);
        if (allocInfo.pMappedData) {
            memcpy(allocInfo.pMappedData, &identity, sizeof(identity));
        }

        dummyBoneSet_ = allocateBoneSet();
        if (dummyBoneSet_) {
            VkDescriptorBufferInfo bufInfo{};
            bufInfo.buffer = dummyBoneBuffer_;
            bufInfo.offset = 0;
            bufInfo.range = sizeof(glm::mat4);
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = dummyBoneSet_;
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &bufInfo;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
    }

    // Mega bone SSBO — consolidates all animated instance bones into one buffer per frame.
    // Slot 0 = identity matrix (for non-animated instances), slots 1..N = animated instances.
    {
        const VkDeviceSize megaSize = MEGA_BONE_MAX_INSTANCES * MAX_BONES_PER_INSTANCE * sizeof(glm::mat4);
        glm::mat4 identity(1.0f);
        for (int i = 0; i < 2; i++) {
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = megaSize;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo allocInfo{};
            vmaCreateBuffer(ctx->getAllocator(), &bci, &aci,
                            &megaBoneBuffer_[i], &megaBoneAlloc_[i], &allocInfo);
            megaBoneMapped_[i] = allocInfo.pMappedData;

            // Slot 0: identity matrix (for non-animated instances)
            if (megaBoneMapped_[i]) {
                memcpy(megaBoneMapped_[i], &identity, sizeof(identity));
            }

            megaBoneSet_[i] = allocateBoneSet();
            if (megaBoneSet_[i]) {
                VkDescriptorBufferInfo bufInfo{};
                bufInfo.buffer = megaBoneBuffer_[i];
                bufInfo.offset = 0;
                bufInfo.range = megaSize;
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = megaBoneSet_[i];
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &bufInfo;
                vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            }
        }
    }

    // Instance data SSBO — per-frame buffer holding per-instance transforms, fade, bones.
    // Shader reads instanceData[push.instanceDataOffset + gl_InstanceIndex].
    {
        static_assert(sizeof(M2InstanceGPU) == 96, "M2InstanceGPU must be 96 bytes (std430)");
        const VkDeviceSize instBufSize = MAX_INSTANCE_DATA * sizeof(M2InstanceGPU);

        // Descriptor pool for 2 sets (double-buffered)
        VkDescriptorPoolSize poolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2};
        VkDescriptorPoolCreateInfo poolCi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolCi.maxSets = 2;
        poolCi.poolSizeCount = 1;
        poolCi.pPoolSizes = &poolSize;
        vkCreateDescriptorPool(device, &poolCi, nullptr, &instanceDescPool_);

        for (int i = 0; i < 2; i++) {
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = instBufSize;
            bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo allocInfo{};
            vmaCreateBuffer(ctx->getAllocator(), &bci, &aci,
                            &instanceBuffer_[i], &instanceAlloc_[i], &allocInfo);
            instanceMapped_[i] = allocInfo.pMappedData;

            VkDescriptorSetAllocateInfo setAi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            setAi.descriptorPool = instanceDescPool_;
            setAi.descriptorSetCount = 1;
            setAi.pSetLayouts = &instanceSetLayout_;
            vkAllocateDescriptorSets(device, &setAi, &instanceSet_[i]);

            VkDescriptorBufferInfo bufInfo{};
            bufInfo.buffer = instanceBuffer_[i];
            bufInfo.offset = 0;
            bufInfo.range = instBufSize;
            VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            write.dstSet = instanceSet_[i];
            write.dstBinding = 0;
            write.descriptorCount = 1;
            write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            write.pBufferInfo = &bufInfo;
            vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
        }
    }

    // GPU frustum culling — compute pipeline, buffers, descriptors.
    // Compute shader tests each instance bounding sphere against 6 frustum planes + distance.
    // Output: uint visibility[] read back by CPU to skip culled instances in sortedVisible_ build.
    {
        static_assert(sizeof(CullInstanceGPU) == 32, "CullInstanceGPU must be 32 bytes (std430)");
        static_assert(sizeof(CullUniformsGPU) == 128, "CullUniformsGPU must be 128 bytes (std140)");

        // Descriptor set layout: binding 0 = UBO (frustum+camera), 1 = SSBO (input), 2 = SSBO (output)
        VkDescriptorSetLayoutBinding bindings[3] = {};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo layoutCi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layoutCi.bindingCount = 3;
        layoutCi.pBindings = bindings;
        vkCreateDescriptorSetLayout(device, &layoutCi, nullptr, &cullSetLayout_);

        // Pipeline layout (no push constants — everything via UBO)
        VkPipelineLayoutCreateInfo plCi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plCi.setLayoutCount = 1;
        plCi.pSetLayouts = &cullSetLayout_;
        vkCreatePipelineLayout(device, &plCi, nullptr, &cullPipelineLayout_);

        // Load compute shader
        rendering::VkShaderModule cullComp;
        if (!cullComp.loadFromFile(device, "assets/shaders/m2_cull.comp.spv")) {
            LOG_ERROR("M2Renderer: failed to load m2_cull.comp.spv — GPU culling disabled");
        } else {
            VkComputePipelineCreateInfo cpCi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
            cpCi.stage = cullComp.stageInfo(VK_SHADER_STAGE_COMPUTE_BIT);
            cpCi.layout = cullPipelineLayout_;
            if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &cpCi, nullptr, &cullPipeline_) != VK_SUCCESS) {
                LOG_ERROR("M2Renderer: failed to create cull compute pipeline");
                cullPipeline_ = VK_NULL_HANDLE;
            }
            cullComp.destroy();
        }

        // Descriptor pool: 2 sets × 3 descriptors each (1 UBO + 2 SSBO)
        VkDescriptorPoolSize poolSizes[2] = {};
        poolSizes[0] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2};
        poolSizes[1] = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};  // 2 input + 2 output
        VkDescriptorPoolCreateInfo poolCi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        poolCi.maxSets = 2;
        poolCi.poolSizeCount = 2;
        poolCi.pPoolSizes = poolSizes;
        vkCreateDescriptorPool(device, &poolCi, nullptr, &cullDescPool_);

        const VkDeviceSize uniformSize = sizeof(CullUniformsGPU);
        const VkDeviceSize inputSize   = MAX_CULL_INSTANCES * sizeof(CullInstanceGPU);
        const VkDeviceSize outputSize  = MAX_CULL_INSTANCES * sizeof(uint32_t);

        for (int i = 0; i < 2; i++) {
            // Uniform buffer (frustum planes + camera)
            {
                VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bci.size = uniformSize;
                bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
                VmaAllocationCreateInfo aci{};
                aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
                aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo ai{};
                vmaCreateBuffer(ctx->getAllocator(), &bci, &aci,
                                &cullUniformBuffer_[i], &cullUniformAlloc_[i], &ai);
                cullUniformMapped_[i] = ai.pMappedData;
            }
            // Input SSBO (per-instance cull data)
            {
                VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bci.size = inputSize;
                bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                VmaAllocationCreateInfo aci{};
                aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
                aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo ai{};
                vmaCreateBuffer(ctx->getAllocator(), &bci, &aci,
                                &cullInputBuffer_[i], &cullInputAlloc_[i], &ai);
                cullInputMapped_[i] = ai.pMappedData;
            }
            // Output SSBO (visibility flags — GPU writes, CPU reads)
            {
                VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bci.size = outputSize;
                bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
                VmaAllocationCreateInfo aci{};
                aci.usage = VMA_MEMORY_USAGE_GPU_TO_CPU;
                aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo ai{};
                vmaCreateBuffer(ctx->getAllocator(), &bci, &aci,
                                &cullOutputBuffer_[i], &cullOutputAlloc_[i], &ai);
                cullOutputMapped_[i] = ai.pMappedData;
            }

            // Allocate and write descriptor set
            VkDescriptorSetAllocateInfo setAi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            setAi.descriptorPool = cullDescPool_;
            setAi.descriptorSetCount = 1;
            setAi.pSetLayouts = &cullSetLayout_;
            vkAllocateDescriptorSets(device, &setAi, &cullSet_[i]);

            VkDescriptorBufferInfo uboInfo{cullUniformBuffer_[i], 0, uniformSize};
            VkDescriptorBufferInfo inputInfo{cullInputBuffer_[i], 0, inputSize};
            VkDescriptorBufferInfo outputInfo{cullOutputBuffer_[i], 0, outputSize};

            VkWriteDescriptorSet writes[3] = {};
            writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[0].dstSet = cullSet_[i];
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo = &uboInfo;

            writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[1].dstSet = cullSet_[i];
            writes[1].dstBinding = 1;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[1].pBufferInfo = &inputInfo;

            writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[2].dstSet = cullSet_[i];
            writes[2].dstBinding = 2;
            writes[2].descriptorCount = 1;
            writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            writes[2].pBufferInfo = &outputInfo;

            vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
        }
    }

    // --- Pipeline layouts ---

    // Main M2 pipeline layout: set 0 = perFrame, set 1 = material, set 2 = bones, set 3 = instances
    // Push constant: int texCoordSet + int isFoliage + int instanceDataOffset (12 bytes)
    {
        VkDescriptorSetLayout setLayouts[] = {perFrameLayout, materialSetLayout_, boneSetLayout_, instanceSetLayout_};
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset = 0;
        pushRange.size = 12; // int texCoordSet + int isFoliage + int instanceDataOffset

        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 4;
        ci.pSetLayouts = setLayouts;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pushRange;
        vkCreatePipelineLayout(device, &ci, nullptr, &pipelineLayout_);
    }

    // Particle pipeline layout: set 0 = perFrame, set 1 = particleTex
    // Push constant: vec2 tileCount + int alphaKey (12 bytes)
    {
        VkDescriptorSetLayout setLayouts[] = {perFrameLayout, particleTexLayout_};
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        pushRange.offset = 0;
        pushRange.size = 12; // vec2 + int

        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 2;
        ci.pSetLayouts = setLayouts;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pushRange;
        vkCreatePipelineLayout(device, &ci, nullptr, &particlePipelineLayout_);
    }

    // Smoke pipeline layout: set 0 = perFrame
    // Push constant: float screenHeight (4 bytes)
    {
        VkDescriptorSetLayout setLayouts[] = {perFrameLayout};
        VkPushConstantRange pushRange{};
        pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        pushRange.offset = 0;
        pushRange.size = 4;

        VkPipelineLayoutCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        ci.setLayoutCount = 1;
        ci.pSetLayouts = setLayouts;
        ci.pushConstantRangeCount = 1;
        ci.pPushConstantRanges = &pushRange;
        vkCreatePipelineLayout(device, &ci, nullptr, &smokePipelineLayout_);
    }

    // --- Load shaders ---
    rendering::VkShaderModule m2Vert, m2Frag;
    rendering::VkShaderModule particleVert, particleFrag;
    rendering::VkShaderModule smokeVert, smokeFrag;

    (void)m2Vert.loadFromFile(device, "assets/shaders/m2.vert.spv");
    (void)m2Frag.loadFromFile(device, "assets/shaders/m2.frag.spv");
    (void)particleVert.loadFromFile(device, "assets/shaders/m2_particle.vert.spv");
    (void)particleFrag.loadFromFile(device, "assets/shaders/m2_particle.frag.spv");
    (void)smokeVert.loadFromFile(device, "assets/shaders/m2_smoke.vert.spv");
    (void)smokeFrag.loadFromFile(device, "assets/shaders/m2_smoke.frag.spv");

    if (!m2Vert.isValid() || !m2Frag.isValid()) {
        LOG_ERROR("M2: Missing required shaders, cannot initialize");
        return false;
    }

    VkRenderPass mainPass = vkCtx_->getImGuiRenderPass();

    // --- Build M2 model pipelines ---
    // Vertex input: 18 floats = 72 bytes stride
    // loc 0: vec3 pos (0), loc 1: vec3 normal (12), loc 2: vec2 uv0 (24),
    // loc 5: vec2 uv1 (32), loc 3: vec4 boneWeights (40), loc 4: vec4 boneIndices (56)
    VkVertexInputBindingDescription m2Binding{};
    m2Binding.binding = 0;
    m2Binding.stride = 18 * sizeof(float);
    m2Binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> m2Attrs = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},                     // position
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 3 * sizeof(float)},     // normal
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 6 * sizeof(float)},        // texCoord0
        {5, 0, VK_FORMAT_R32G32_SFLOAT, 8 * sizeof(float)},        // texCoord1
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 10 * sizeof(float)}, // boneWeights
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 14 * sizeof(float)}, // boneIndices (float)
    };

    // Pipeline derivatives — opaque is the base, others derive from it for shared state optimization
    auto buildM2Pipeline = [&](VkPipelineColorBlendAttachmentState blendState, bool depthWrite,
                               VkPipelineCreateFlags flags = 0, VkPipeline basePipeline = VK_NULL_HANDLE) -> VkPipeline {
        return PipelineBuilder()
            .setShaders(m2Vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                        m2Frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({m2Binding}, m2Attrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setDepthTest(true, depthWrite, VK_COMPARE_OP_LESS_OR_EQUAL)
            .setColorBlendAttachment(blendState)
            .setMultisample(vkCtx_->getMsaaSamples())
            .setLayout(pipelineLayout_)
            .setRenderPass(mainPass)
            .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
            .setFlags(flags)
            .setBasePipeline(basePipeline)
            .build(device, vkCtx_->getPipelineCache());
    };

    opaquePipeline_ = buildM2Pipeline(PipelineBuilder::blendDisabled(), true,
                                      VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT);
    alphaTestPipeline_ = buildM2Pipeline(PipelineBuilder::blendAlpha(), true,
                                         VK_PIPELINE_CREATE_DERIVATIVE_BIT, opaquePipeline_);
    alphaPipeline_ = buildM2Pipeline(PipelineBuilder::blendAlpha(), false,
                                     VK_PIPELINE_CREATE_DERIVATIVE_BIT, opaquePipeline_);
    additivePipeline_ = buildM2Pipeline(PipelineBuilder::blendAdditive(), false,
                                        VK_PIPELINE_CREATE_DERIVATIVE_BIT, opaquePipeline_);

    // --- Build particle pipelines ---
    if (particleVert.isValid() && particleFrag.isValid()) {
        VkVertexInputBindingDescription pBind{};
        pBind.binding = 0;
        pBind.stride = 9 * sizeof(float); // pos3 + color4 + size1 + tile1
        pBind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::vector<VkVertexInputAttributeDescription> pAttrs = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},                    // position
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 3 * sizeof(float)}, // color
            {2, 0, VK_FORMAT_R32_SFLOAT, 7 * sizeof(float)},          // size
            {3, 0, VK_FORMAT_R32_SFLOAT, 8 * sizeof(float)},          // tile
        };

        auto buildParticlePipeline = [&](VkPipelineColorBlendAttachmentState blend) -> VkPipeline {
            return PipelineBuilder()
                .setShaders(particleVert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                            particleFrag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
                .setVertexInput({pBind}, pAttrs)
                .setTopology(VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
                .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
                .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
                .setColorBlendAttachment(blend)
                .setMultisample(vkCtx_->getMsaaSamples())
                .setLayout(particlePipelineLayout_)
                .setRenderPass(mainPass)
                .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
                .build(device, vkCtx_->getPipelineCache());
        };

        particlePipeline_ = buildParticlePipeline(PipelineBuilder::blendAlpha());
        particleAdditivePipeline_ = buildParticlePipeline(PipelineBuilder::blendAdditive());
    }

    // --- Build smoke pipeline ---
    if (smokeVert.isValid() && smokeFrag.isValid()) {
        VkVertexInputBindingDescription sBind{};
        sBind.binding = 0;
        sBind.stride = 6 * sizeof(float); // pos3 + lifeRatio1 + size1 + isSpark1
        sBind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::vector<VkVertexInputAttributeDescription> sAttrs = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},           // position
            {1, 0, VK_FORMAT_R32_SFLOAT, 3 * sizeof(float)}, // lifeRatio
            {2, 0, VK_FORMAT_R32_SFLOAT, 4 * sizeof(float)}, // size
            {3, 0, VK_FORMAT_R32_SFLOAT, 5 * sizeof(float)}, // isSpark
        };

        smokePipeline_ = PipelineBuilder()
            .setShaders(smokeVert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                        smokeFrag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({sBind}, sAttrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
            .setColorBlendAttachment(PipelineBuilder::blendAlpha())
            .setMultisample(vkCtx_->getMsaaSamples())
            .setLayout(smokePipelineLayout_)
            .setRenderPass(mainPass)
            .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
            .build(device, vkCtx_->getPipelineCache());
    }

    // --- Build ribbon pipelines ---
    // Vertex format: pos(3) + color(3) + alpha(1) + uv(2) = 9 floats = 36 bytes
    {
        rendering::VkShaderModule ribVert, ribFrag;
        (void)ribVert.loadFromFile(device, "assets/shaders/m2_ribbon.vert.spv");
        (void)ribFrag.loadFromFile(device, "assets/shaders/m2_ribbon.frag.spv");
        if (ribVert.isValid() && ribFrag.isValid()) {
            // Reuse particleTexLayout_ for set 1 (single texture sampler)
            VkDescriptorSetLayout ribLayouts[] = {perFrameLayout, particleTexLayout_};
            VkPipelineLayoutCreateInfo lci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            lci.setLayoutCount = 2;
            lci.pSetLayouts = ribLayouts;
            vkCreatePipelineLayout(device, &lci, nullptr, &ribbonPipelineLayout_);

            VkVertexInputBindingDescription rBind{};
            rBind.binding = 0;
            rBind.stride = 9 * sizeof(float);
            rBind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

            std::vector<VkVertexInputAttributeDescription> rAttrs = {
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},                    // pos
                {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 3 * sizeof(float)},    // color
                {2, 0, VK_FORMAT_R32_SFLOAT,       6 * sizeof(float)},    // alpha
                {3, 0, VK_FORMAT_R32G32_SFLOAT,    7 * sizeof(float)},    // uv
            };

            auto buildRibbonPipeline = [&](VkPipelineColorBlendAttachmentState blend) -> VkPipeline {
                return PipelineBuilder()
                    .setShaders(ribVert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                                ribFrag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
                    .setVertexInput({rBind}, rAttrs)
                    .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
                    .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
                    .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
                    .setColorBlendAttachment(blend)
                    .setMultisample(vkCtx_->getMsaaSamples())
                    .setLayout(ribbonPipelineLayout_)
                    .setRenderPass(mainPass)
                    .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
                    .build(device, vkCtx_->getPipelineCache());
            };

            ribbonPipeline_         = buildRibbonPipeline(PipelineBuilder::blendAlpha());
            ribbonAdditivePipeline_ = buildRibbonPipeline(PipelineBuilder::blendAdditive());
        }
        ribVert.destroy(); ribFrag.destroy();
    }

    // Clean up shader modules
    m2Vert.destroy(); m2Frag.destroy();
    particleVert.destroy(); particleFrag.destroy();
    smokeVert.destroy(); smokeFrag.destroy();

    // --- Create dynamic particle buffers (mapped for CPU writes) ---
    {
        VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

        VmaAllocationCreateInfo aci{};
        aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo allocInfo{};

        // Smoke particle buffer
        bci.size = MAX_SMOKE_PARTICLES * 6 * sizeof(float);
        vmaCreateBuffer(vkCtx_->getAllocator(), &bci, &aci, &smokeVB_, &smokeVBAlloc_, &allocInfo);
        smokeVBMapped_ = allocInfo.pMappedData;

        // M2 particle buffer
        bci.size = MAX_M2_PARTICLES * 9 * sizeof(float);
        vmaCreateBuffer(vkCtx_->getAllocator(), &bci, &aci, &m2ParticleVB_, &m2ParticleVBAlloc_, &allocInfo);
        m2ParticleVBMapped_ = allocInfo.pMappedData;

        // Dedicated glow sprite buffer (separate from particle VB to avoid data race)
        bci.size = MAX_GLOW_SPRITES * 9 * sizeof(float);
        vmaCreateBuffer(vkCtx_->getAllocator(), &bci, &aci, &glowVB_, &glowVBAlloc_, &allocInfo);
        glowVBMapped_ = allocInfo.pMappedData;

        // Ribbon vertex buffer — triangle strip: pos(3)+color(3)+alpha(1)+uv(2)=9 floats/vert
        bci.size = MAX_RIBBON_VERTS * 9 * sizeof(float);
        vmaCreateBuffer(vkCtx_->getAllocator(), &bci, &aci, &ribbonVB_, &ribbonVBAlloc_, &allocInfo);
        ribbonVBMapped_ = allocInfo.pMappedData;
    }

    // --- Create white fallback texture ---
    {
        uint8_t white[] = {255, 255, 255, 255};
        whiteTexture_ = std::make_unique<VkTexture>();
        whiteTexture_->upload(*vkCtx_, white, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
        whiteTexture_->createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_REPEAT);
    }

    // --- Generate soft radial gradient glow texture ---
    {
        static constexpr int SZ = 64;
        std::vector<uint8_t> px(SZ * SZ * 4);
        float half = SZ / 2.0f;
        for (int y = 0; y < SZ; y++) {
            for (int x = 0; x < SZ; x++) {
                float dx = (x + 0.5f - half) / half;
                float dy = (y + 0.5f - half) / half;
                float r = std::sqrt(dx * dx + dy * dy);
                float a = std::max(0.0f, 1.0f - r);
                a = a * a; // Quadratic falloff
                int idx = (y * SZ + x) * 4;
                px[idx + 0] = 255;
                px[idx + 1] = 255;
                px[idx + 2] = 255;
                px[idx + 3] = static_cast<uint8_t>(a * 255);
            }
        }
        glowTexture_ = std::make_unique<VkTexture>();
        glowTexture_->upload(*vkCtx_, px.data(), SZ, SZ, VK_FORMAT_R8G8B8A8_UNORM);
        glowTexture_->createSampler(device, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE);
        // Pre-allocate glow texture descriptor set (reused every frame)
        if (particleTexLayout_ && materialDescPool_) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = materialDescPool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &particleTexLayout_;
            if (vkAllocateDescriptorSets(device, &ai, &glowTexDescSet_) == VK_SUCCESS) {
                VkDescriptorImageInfo imgInfo = glowTexture_->descriptorInfo();
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = glowTexDescSet_;
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &imgInfo;
                vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            }
        }
    }
    textureCacheBudgetBytes_ =
        envSizeMBOrDefault("WOWEE_M2_TEX_CACHE_MB", 4096) * 1024ull * 1024ull;
    modelCacheLimit_ = envSizeMBOrDefault("WOWEE_M2_MODEL_LIMIT", 6000);
    LOG_INFO("M2 texture cache budget: ", textureCacheBudgetBytes_ / (1024 * 1024), " MB");
    LOG_INFO("M2 model cache limit: ", modelCacheLimit_);

    LOG_INFO("M2 renderer initialized (Vulkan)");
    initialized_ = true;
    return true;
}

void M2Renderer::shutdown() {
    LOG_INFO("Shutting down M2 renderer...");
    if (!vkCtx_) return;

    vkDeviceWaitIdle(vkCtx_->getDevice());
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator alloc = vkCtx_->getAllocator();

    // Delete model GPU resources
    for (auto& [id, model] : models) {
        destroyModelGPU(model);
    }
    models.clear();

    // Destroy instance bone buffers
    for (auto& inst : instances) {
        destroyInstanceBones(inst);
    }
    instances.clear();
    spatialGrid.clear();
    instanceIndexById.clear();
    instanceDedupMap_.clear();

    // Delete cached textures
    textureCache.clear();
    textureCacheBytes_ = 0;
    textureCacheCounter_ = 0;
    texturePropsByPtr_.clear();
    failedTextureCache_.clear();
    failedTextureRetryAt_.clear();
    loggedTextureLoadFails_.clear();
    textureLookupSerial_ = 0;
    textureBudgetRejectWarnings_ = 0;
    whiteTexture_.reset();
    glowTexture_.reset();

    // Clean up particle/ribbon buffers
    if (smokeVB_) { vmaDestroyBuffer(alloc, smokeVB_, smokeVBAlloc_); smokeVB_ = VK_NULL_HANDLE; }
    if (m2ParticleVB_) { vmaDestroyBuffer(alloc, m2ParticleVB_, m2ParticleVBAlloc_); m2ParticleVB_ = VK_NULL_HANDLE; }
    if (glowVB_) { vmaDestroyBuffer(alloc, glowVB_, glowVBAlloc_); glowVB_ = VK_NULL_HANDLE; }
    if (ribbonVB_) { vmaDestroyBuffer(alloc, ribbonVB_, ribbonVBAlloc_); ribbonVB_ = VK_NULL_HANDLE; }
    smokeParticles.clear();

    // Destroy pipelines
    auto destroyPipeline = [&](VkPipeline& p) { if (p) { vkDestroyPipeline(device, p, nullptr); p = VK_NULL_HANDLE; } };
    destroyPipeline(opaquePipeline_);
    destroyPipeline(alphaTestPipeline_);
    destroyPipeline(alphaPipeline_);
    destroyPipeline(additivePipeline_);
    destroyPipeline(particlePipeline_);
    destroyPipeline(particleAdditivePipeline_);
    destroyPipeline(smokePipeline_);
    destroyPipeline(ribbonPipeline_);
    destroyPipeline(ribbonAdditivePipeline_);

    if (pipelineLayout_) { vkDestroyPipelineLayout(device, pipelineLayout_, nullptr); pipelineLayout_ = VK_NULL_HANDLE; }
    if (particlePipelineLayout_) { vkDestroyPipelineLayout(device, particlePipelineLayout_, nullptr); particlePipelineLayout_ = VK_NULL_HANDLE; }
    if (smokePipelineLayout_) { vkDestroyPipelineLayout(device, smokePipelineLayout_, nullptr); smokePipelineLayout_ = VK_NULL_HANDLE; }
    if (ribbonPipelineLayout_) { vkDestroyPipelineLayout(device, ribbonPipelineLayout_, nullptr); ribbonPipelineLayout_ = VK_NULL_HANDLE; }

    // Destroy descriptor pools and layouts
    if (dummyBoneBuffer_) { vmaDestroyBuffer(alloc, dummyBoneBuffer_, dummyBoneAlloc_); dummyBoneBuffer_ = VK_NULL_HANDLE; }
    // dummyBoneSet_ is freed implicitly when boneDescPool_ is destroyed
    dummyBoneSet_ = VK_NULL_HANDLE;
    // Mega bone SSBO cleanup (sets freed implicitly with boneDescPool_)
    for (int i = 0; i < 2; i++) {
        if (megaBoneBuffer_[i]) { vmaDestroyBuffer(alloc, megaBoneBuffer_[i], megaBoneAlloc_[i]); megaBoneBuffer_[i] = VK_NULL_HANDLE; }
        megaBoneMapped_[i] = nullptr;
        megaBoneSet_[i] = VK_NULL_HANDLE;
    }
    if (materialDescPool_) { vkDestroyDescriptorPool(device, materialDescPool_, nullptr); materialDescPool_ = VK_NULL_HANDLE; }
    if (boneDescPool_) { vkDestroyDescriptorPool(device, boneDescPool_, nullptr); boneDescPool_ = VK_NULL_HANDLE; }
    // Instance data SSBO cleanup (sets freed with instanceDescPool_)
    for (int i = 0; i < 2; i++) {
        if (instanceBuffer_[i]) { vmaDestroyBuffer(alloc, instanceBuffer_[i], instanceAlloc_[i]); instanceBuffer_[i] = VK_NULL_HANDLE; }
        instanceMapped_[i] = nullptr;
        instanceSet_[i] = VK_NULL_HANDLE;
    }
    if (instanceDescPool_) { vkDestroyDescriptorPool(device, instanceDescPool_, nullptr); instanceDescPool_ = VK_NULL_HANDLE; }

    // GPU frustum culling compute pipeline + buffers cleanup
    if (cullPipeline_) { vkDestroyPipeline(device, cullPipeline_, nullptr); cullPipeline_ = VK_NULL_HANDLE; }
    if (cullPipelineLayout_) { vkDestroyPipelineLayout(device, cullPipelineLayout_, nullptr); cullPipelineLayout_ = VK_NULL_HANDLE; }
    for (int i = 0; i < 2; i++) {
        if (cullUniformBuffer_[i]) { vmaDestroyBuffer(alloc, cullUniformBuffer_[i], cullUniformAlloc_[i]); cullUniformBuffer_[i] = VK_NULL_HANDLE; }
        if (cullInputBuffer_[i])   { vmaDestroyBuffer(alloc, cullInputBuffer_[i], cullInputAlloc_[i]); cullInputBuffer_[i] = VK_NULL_HANDLE; }
        if (cullOutputBuffer_[i])  { vmaDestroyBuffer(alloc, cullOutputBuffer_[i], cullOutputAlloc_[i]); cullOutputBuffer_[i] = VK_NULL_HANDLE; }
        cullUniformMapped_[i] = cullInputMapped_[i] = cullOutputMapped_[i] = nullptr;
        cullSet_[i] = VK_NULL_HANDLE;
    }
    if (cullDescPool_) { vkDestroyDescriptorPool(device, cullDescPool_, nullptr); cullDescPool_ = VK_NULL_HANDLE; }
    if (cullSetLayout_) { vkDestroyDescriptorSetLayout(device, cullSetLayout_, nullptr); cullSetLayout_ = VK_NULL_HANDLE; }

    if (materialSetLayout_) { vkDestroyDescriptorSetLayout(device, materialSetLayout_, nullptr); materialSetLayout_ = VK_NULL_HANDLE; }
    if (boneSetLayout_) { vkDestroyDescriptorSetLayout(device, boneSetLayout_, nullptr); boneSetLayout_ = VK_NULL_HANDLE; }
    if (instanceSetLayout_) { vkDestroyDescriptorSetLayout(device, instanceSetLayout_, nullptr); instanceSetLayout_ = VK_NULL_HANDLE; }
    if (particleTexLayout_) { vkDestroyDescriptorSetLayout(device, particleTexLayout_, nullptr); particleTexLayout_ = VK_NULL_HANDLE; }

    // Destroy shadow resources
    destroyPipeline(shadowPipeline_);
    if (shadowPipelineLayout_) { vkDestroyPipelineLayout(device, shadowPipelineLayout_, nullptr); shadowPipelineLayout_ = VK_NULL_HANDLE; }
    for (auto& pool : shadowTexPool_) { if (pool) { vkDestroyDescriptorPool(device, pool, nullptr); pool = VK_NULL_HANDLE; } }
    if (shadowParamsPool_) { vkDestroyDescriptorPool(device, shadowParamsPool_, nullptr); shadowParamsPool_ = VK_NULL_HANDLE; }
    if (shadowParamsLayout_) { vkDestroyDescriptorSetLayout(device, shadowParamsLayout_, nullptr); shadowParamsLayout_ = VK_NULL_HANDLE; }
    if (shadowParamsUBO_) { vmaDestroyBuffer(alloc, shadowParamsUBO_, shadowParamsAlloc_); shadowParamsUBO_ = VK_NULL_HANDLE; }

    initialized_ = false;
}

void M2Renderer::destroyModelGPU(M2ModelGPU& model) {
    if (!vkCtx_) return;
    VmaAllocator alloc = vkCtx_->getAllocator();
    if (model.vertexBuffer) { vmaDestroyBuffer(alloc, model.vertexBuffer, model.vertexAlloc); model.vertexBuffer = VK_NULL_HANDLE; }
    if (model.indexBuffer) { vmaDestroyBuffer(alloc, model.indexBuffer, model.indexAlloc); model.indexBuffer = VK_NULL_HANDLE; }
    VkDevice device = vkCtx_->getDevice();
    for (auto& batch : model.batches) {
        if (batch.materialSet) { vkFreeDescriptorSets(device, materialDescPool_, 1, &batch.materialSet); batch.materialSet = VK_NULL_HANDLE; }
        if (batch.materialUBO) { vmaDestroyBuffer(alloc, batch.materialUBO, batch.materialUBOAlloc); batch.materialUBO = VK_NULL_HANDLE; }
    }
    // Free pre-allocated particle texture descriptor sets
    for (auto& pSet : model.particleTexSets) {
        if (pSet) { vkFreeDescriptorSets(device, materialDescPool_, 1, &pSet); pSet = VK_NULL_HANDLE; }
    }
    model.particleTexSets.clear();
    // Free ribbon texture descriptor sets
    for (auto& rSet : model.ribbonTexSets) {
        if (rSet) { vkFreeDescriptorSets(device, materialDescPool_, 1, &rSet); rSet = VK_NULL_HANDLE; }
    }
    model.ribbonTexSets.clear();
}

void M2Renderer::destroyInstanceBones(M2Instance& inst, bool defer) {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();
    VmaAllocator alloc = vkCtx_->getAllocator();
    for (int i = 0; i < 2; i++) {
        // Snapshot handles before clearing the instance — needed for both
        // immediate and deferred paths.
        VkDescriptorSet boneSet = inst.boneSet[i];
        ::VkBuffer boneBuf = inst.boneBuffer[i];
        VmaAllocation boneAlloc = inst.boneAlloc[i];
        inst.boneSet[i] = VK_NULL_HANDLE;
        inst.boneBuffer[i] = VK_NULL_HANDLE;
        inst.boneMapped[i] = nullptr;

        if (!defer) {
            // Immediate destruction (safe after vkDeviceWaitIdle)
            if (boneSet != VK_NULL_HANDLE) {
                vkFreeDescriptorSets(device, boneDescPool_, 1, &boneSet);
            }
            if (boneBuf) {
                vmaDestroyBuffer(alloc, boneBuf, boneAlloc);
            }
        } else if (boneSet != VK_NULL_HANDLE || boneBuf) {
            // Deferred destruction — the loop destroys bone sets for ALL frame
            // slots, so the other slot's command buffer may still be in flight.
            // Must wait for all fences, not just the current frame's.
            VkDescriptorPool pool = boneDescPool_;
            vkCtx_->deferAfterAllFrameFences([device, alloc, pool, boneSet, boneBuf, boneAlloc]() {
                if (boneSet != VK_NULL_HANDLE) {
                    VkDescriptorSet s = boneSet;
                    vkFreeDescriptorSets(device, pool, 1, &s);
                }
                if (boneBuf) {
                    vmaDestroyBuffer(alloc, boneBuf, boneAlloc);
                }
            });
        }
    }
}

VkDescriptorSet M2Renderer::allocateMaterialSet() {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = materialDescPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &materialSetLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult result = vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &set);
    if (result != VK_SUCCESS) {
        LOG_ERROR("M2Renderer: material descriptor set allocation failed (", result, ")");
        return VK_NULL_HANDLE;
    }
    return set;
}

VkDescriptorSet M2Renderer::allocateBoneSet() {
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = boneDescPool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &boneSetLayout_;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkResult result = vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &set);
    if (result != VK_SUCCESS) {
        LOG_ERROR("M2Renderer: bone descriptor set allocation failed (", result, ")");
        return VK_NULL_HANDLE;
    }
    return set;
}

// ---------------------------------------------------------------------------
// M2 collision mesh: build spatial grid + classify triangles
// ---------------------------------------------------------------------------
void M2ModelGPU::CollisionMesh::build() {
    if (indices.size() < 3 || vertices.empty()) return;
    triCount = static_cast<uint32_t>(indices.size() / 3);

    // Bounding box for grid
    glm::vec3 bmin(std::numeric_limits<float>::max());
    glm::vec3 bmax(-std::numeric_limits<float>::max());
    for (const auto& v : vertices) {
        bmin = glm::min(bmin, v);
        bmax = glm::max(bmax, v);
    }

    gridOrigin = glm::vec2(bmin.x, bmin.y);
    gridCellsX = std::max(1, std::min(32, static_cast<int>(std::ceil((bmax.x - bmin.x) / CELL_SIZE))));
    gridCellsY = std::max(1, std::min(32, static_cast<int>(std::ceil((bmax.y - bmin.y) / CELL_SIZE))));

    cellFloorTris.resize(gridCellsX * gridCellsY);
    cellWallTris.resize(gridCellsX * gridCellsY);
    triBounds.resize(triCount);

    for (uint32_t ti = 0; ti < triCount; ti++) {
        uint16_t i0 = indices[ti * 3];
        uint16_t i1 = indices[ti * 3 + 1];
        uint16_t i2 = indices[ti * 3 + 2];
        if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size()) continue;

        const auto& v0 = vertices[i0];
        const auto& v1 = vertices[i1];
        const auto& v2 = vertices[i2];

        triBounds[ti].minZ = std::min({v0.z, v1.z, v2.z});
        triBounds[ti].maxZ = std::max({v0.z, v1.z, v2.z});

        glm::vec3 normal = glm::cross(v1 - v0, v2 - v0);
        float normalLen = glm::length(normal);
        float absNz = (normalLen > 0.001f) ? std::abs(normal.z / normalLen) : 0.0f;
        bool isFloor = (absNz >= 0.35f);  // ~70° max slope (relaxed for steep stairs)
        bool isWall  = (absNz < 0.65f);

        float triMinX = std::min({v0.x, v1.x, v2.x});
        float triMaxX = std::max({v0.x, v1.x, v2.x});
        float triMinY = std::min({v0.y, v1.y, v2.y});
        float triMaxY = std::max({v0.y, v1.y, v2.y});

        int cxMin = std::clamp(static_cast<int>((triMinX - gridOrigin.x) / CELL_SIZE), 0, gridCellsX - 1);
        int cxMax = std::clamp(static_cast<int>((triMaxX - gridOrigin.x) / CELL_SIZE), 0, gridCellsX - 1);
        int cyMin = std::clamp(static_cast<int>((triMinY - gridOrigin.y) / CELL_SIZE), 0, gridCellsY - 1);
        int cyMax = std::clamp(static_cast<int>((triMaxY - gridOrigin.y) / CELL_SIZE), 0, gridCellsY - 1);

        for (int cy = cyMin; cy <= cyMax; cy++) {
            for (int cx = cxMin; cx <= cxMax; cx++) {
                int ci = cy * gridCellsX + cx;
                if (isFloor) cellFloorTris[ci].push_back(ti);
                if (isWall)  cellWallTris[ci].push_back(ti);
            }
        }
    }
}

void M2ModelGPU::CollisionMesh::getFloorTrisInRange(
        float minX, float minY, float maxX, float maxY,
        std::vector<uint32_t>& out) const {
    out.clear();
    if (gridCellsX == 0 || gridCellsY == 0) return;
    int cxMin = std::clamp(static_cast<int>((minX - gridOrigin.x) / CELL_SIZE), 0, gridCellsX - 1);
    int cxMax = std::clamp(static_cast<int>((maxX - gridOrigin.x) / CELL_SIZE), 0, gridCellsX - 1);
    int cyMin = std::clamp(static_cast<int>((minY - gridOrigin.y) / CELL_SIZE), 0, gridCellsY - 1);
    int cyMax = std::clamp(static_cast<int>((maxY - gridOrigin.y) / CELL_SIZE), 0, gridCellsY - 1);
    const size_t cellCount = static_cast<size_t>(cxMax - cxMin + 1) *
                             static_cast<size_t>(cyMax - cyMin + 1);
    out.reserve(cellCount * 8);
    for (int cy = cyMin; cy <= cyMax; cy++) {
        for (int cx = cxMin; cx <= cxMax; cx++) {
            const auto& cell = cellFloorTris[cy * gridCellsX + cx];
            out.insert(out.end(), cell.begin(), cell.end());
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

void M2ModelGPU::CollisionMesh::getWallTrisInRange(
        float minX, float minY, float maxX, float maxY,
        std::vector<uint32_t>& out) const {
    out.clear();
    if (gridCellsX == 0 || gridCellsY == 0) return;
    int cxMin = std::clamp(static_cast<int>((minX - gridOrigin.x) / CELL_SIZE), 0, gridCellsX - 1);
    int cxMax = std::clamp(static_cast<int>((maxX - gridOrigin.x) / CELL_SIZE), 0, gridCellsX - 1);
    int cyMin = std::clamp(static_cast<int>((minY - gridOrigin.y) / CELL_SIZE), 0, gridCellsY - 1);
    int cyMax = std::clamp(static_cast<int>((maxY - gridOrigin.y) / CELL_SIZE), 0, gridCellsY - 1);
    const size_t cellCount = static_cast<size_t>(cxMax - cxMin + 1) *
                             static_cast<size_t>(cyMax - cyMin + 1);
    out.reserve(cellCount * 8);
    for (int cy = cyMin; cy <= cyMax; cy++) {
        for (int cx = cxMin; cx <= cxMax; cx++) {
            const auto& cell = cellWallTris[cy * gridCellsX + cx];
            out.insert(out.end(), cell.begin(), cell.end());
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
}

bool M2Renderer::hasModel(uint32_t modelId) const {
    return models.find(modelId) != models.end();
}

bool M2Renderer::loadModel(const pipeline::M2Model& model, uint32_t modelId) {
    if (models.find(modelId) != models.end()) {
        // Already loaded
        return true;
    }
    if (models.size() >= modelCacheLimit_) {
        if (modelLimitRejectWarnings_ < 3) {
            LOG_WARNING("M2 model cache full (", models.size(), "/", modelCacheLimit_,
                        "), skipping model load: id=", modelId, " name=", model.name);
        }
        ++modelLimitRejectWarnings_;
        return false;
    }

    bool hasGeometry = !model.vertices.empty() && !model.indices.empty();
    bool hasParticles = !model.particleEmitters.empty();
    bool hasRibbons   = !model.ribbonEmitters.empty();
    if (!hasGeometry && !hasParticles && !hasRibbons) {
        LOG_WARNING("M2 model has no renderable content: ", model.name);
        return false;
    }

    M2ModelGPU gpuModel;
    gpuModel.name = model.name;

    // Use tight bounds from actual vertices for collision/camera occlusion.
    // Header bounds in some M2s are overly conservative.
    glm::vec3 tightMin(0.0f);
    glm::vec3 tightMax(0.0f);
    if (hasGeometry) {
        tightMin = glm::vec3(std::numeric_limits<float>::max());
        tightMax = glm::vec3(-std::numeric_limits<float>::max());
        for (const auto& v : model.vertices) {
            tightMin = glm::min(tightMin, v.position);
            tightMax = glm::max(tightMax, v.position);
        }
    }

    // Classify model from name and geometry — pure function, no GPU dependencies.
    auto cls = classifyM2Model(model.name, tightMin, tightMax,
                                model.vertices.size(),
                                model.particleEmitters.size());
    const bool isInvisibleTrap   = cls.isInvisibleTrap;
    const bool groundDetailModel = cls.isGroundDetail;
    if (isInvisibleTrap) {
        LOG_INFO("Loading InvisibleTrap model: ", model.name, " (will be invisible, no collision)");
    }

    gpuModel.isInvisibleTrap             = cls.isInvisibleTrap;
    gpuModel.collisionSteppedFountain    = cls.collisionSteppedFountain;
    gpuModel.collisionSteppedLowPlatform = cls.collisionSteppedLowPlatform;
    gpuModel.collisionBridge             = cls.collisionBridge;
    gpuModel.collisionPlanter            = cls.collisionPlanter;
    gpuModel.collisionStatue             = cls.collisionStatue;
    gpuModel.collisionTreeTrunk          = cls.collisionTreeTrunk;
    gpuModel.collisionNarrowVerticalProp = cls.collisionNarrowVerticalProp;
    gpuModel.collisionSmallSolidProp     = cls.collisionSmallSolidProp;
    gpuModel.collisionNoBlock            = cls.collisionNoBlock;
    gpuModel.isGroundDetail              = cls.isGroundDetail;
    gpuModel.isFoliageLike               = cls.isFoliageLike;
    gpuModel.disableAnimation            = cls.disableAnimation;
    gpuModel.shadowWindFoliage           = cls.shadowWindFoliage;
    gpuModel.isFireflyEffect             = cls.isFireflyEffect;
    gpuModel.isSmoke                     = cls.isSmoke;
    gpuModel.isSpellEffect               = cls.isSpellEffect;
    gpuModel.isLavaModel                 = cls.isLavaModel;
    gpuModel.isInstancePortal            = cls.isInstancePortal;
    gpuModel.isWaterVegetation           = cls.isWaterVegetation;
    gpuModel.isElvenLike                 = cls.isElvenLike;
    gpuModel.isLanternLike               = cls.isLanternLike;
    gpuModel.isKoboldFlame               = cls.isKoboldFlame;
    gpuModel.boundMin = tightMin;
    gpuModel.boundMax = tightMax;
    gpuModel.boundRadius = model.boundRadius;
    gpuModel.indexCount = static_cast<uint32_t>(model.indices.size());
    gpuModel.vertexCount = static_cast<uint32_t>(model.vertices.size());

    // Store bone/sequence data for animation
    gpuModel.bones = model.bones;
    gpuModel.sequences = model.sequences;
    gpuModel.globalSequenceDurations = model.globalSequenceDurations;
    gpuModel.hasAnimation = false;
    for (const auto& bone : model.bones) {
        if (bone.translation.hasData() || bone.rotation.hasData() || bone.scale.hasData()) {
            gpuModel.hasAnimation = true;
            break;
        }
    }


    // Build collision mesh + spatial grid from M2 bounding geometry
    gpuModel.collision.vertices = model.collisionVertices;
    gpuModel.collision.indices = model.collisionIndices;
    gpuModel.collision.build();
    if (gpuModel.collision.valid()) {
        core::Logger::getInstance().debug("  M2 collision mesh: ", gpuModel.collision.triCount,
            " tris, grid ", gpuModel.collision.gridCellsX, "x", gpuModel.collision.gridCellsY);
    }

    // Identify idle variation sequences (animation ID 0 = Stand)
    for (int i = 0; i < static_cast<int>(model.sequences.size()); i++) {
        if (model.sequences[i].id == 0 && model.sequences[i].duration > 0) {
            gpuModel.idleVariationIndices.push_back(i);
        }
    }

    // Batch all GPU uploads (VB, IB, textures) into a single command buffer
    // submission with one fence wait, instead of one fence wait per upload.
    vkCtx_->beginUploadBatch();

    if (hasGeometry) {
        // Create VBO with interleaved vertex data
        // Format: position (3), normal (3), texcoord0 (2), texcoord1 (2), boneWeights (4), boneIndices (4 as float)
        const size_t floatsPerVertex = 18;
        std::vector<float> vertexData;
        vertexData.reserve(model.vertices.size() * floatsPerVertex);

        for (const auto& v : model.vertices) {
            vertexData.push_back(v.position.x);
            vertexData.push_back(v.position.y);
            vertexData.push_back(v.position.z);
            vertexData.push_back(v.normal.x);
            vertexData.push_back(v.normal.y);
            vertexData.push_back(v.normal.z);
            vertexData.push_back(v.texCoords[0].x);
            vertexData.push_back(v.texCoords[0].y);
            vertexData.push_back(v.texCoords[1].x);
            vertexData.push_back(v.texCoords[1].y);
            float w0 = v.boneWeights[0] / 255.0f;
            float w1 = v.boneWeights[1] / 255.0f;
            float w2 = v.boneWeights[2] / 255.0f;
            float w3 = v.boneWeights[3] / 255.0f;
            vertexData.push_back(w0);
            vertexData.push_back(w1);
            vertexData.push_back(w2);
            vertexData.push_back(w3);
            vertexData.push_back(static_cast<float>(std::min(v.boneIndices[0], uint8_t(127))));
            vertexData.push_back(static_cast<float>(std::min(v.boneIndices[1], uint8_t(127))));
            vertexData.push_back(static_cast<float>(std::min(v.boneIndices[2], uint8_t(127))));
            vertexData.push_back(static_cast<float>(std::min(v.boneIndices[3], uint8_t(127))));
        }

        // Upload vertex buffer to GPU
        {
            auto buf = uploadBuffer(*vkCtx_,
                vertexData.data(), vertexData.size() * sizeof(float),
                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
            gpuModel.vertexBuffer = buf.buffer;
            gpuModel.vertexAlloc = buf.allocation;
        }

        // Upload index buffer to GPU
        {
            auto buf = uploadBuffer(*vkCtx_,
                model.indices.data(), model.indices.size() * sizeof(uint16_t),
                VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
            gpuModel.indexBuffer = buf.buffer;
            gpuModel.indexAlloc = buf.allocation;
        }

        if (!gpuModel.vertexBuffer || !gpuModel.indexBuffer) {
            LOG_ERROR("M2Renderer::loadModel: GPU buffer upload failed for model ", modelId);
        }
    }

    // Load ALL textures from the model into a local vector.
    // textureLoadFailed[i] is true if texture[i] had a named path that failed to load.
    // Such batches are hidden (batchOpacity=0) rather than rendered white.
    std::vector<VkTexture*> allTextures;
    std::vector<bool> textureLoadFailed;
    std::vector<std::string> textureKeysLower;
    if (assetManager) {
        for (size_t ti = 0; ti < model.textures.size(); ti++) {
            const auto& tex = model.textures[ti];
            std::string texPath = tex.filename;
            // Some extracted M2 texture strings contain embedded NUL + garbage suffix.
            // Truncate at first NUL so valid paths like "...foo.blp\0junk" still resolve.
            size_t nul = texPath.find('\0');
            if (nul != std::string::npos) {
                texPath.resize(nul);
            }
            if (!texPath.empty()) {
                std::string texKey = texPath;
                std::replace(texKey.begin(), texKey.end(), '/', '\\');
                std::transform(texKey.begin(), texKey.end(), texKey.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                VkTexture* texPtr = loadTexture(texPath, tex.flags);
                bool failed = (texPtr == whiteTexture_.get());
                if (failed) {
                    static uint32_t loggedModelTextureFails = 0;
                    static bool loggedModelTextureFailSuppressed = false;
                    if (loggedModelTextureFails < 250) {
                        LOG_WARNING("M2 model ", model.name, " texture[", ti, "] failed to load: ", texPath);
                        ++loggedModelTextureFails;
                    } else if (!loggedModelTextureFailSuppressed) {
                        LOG_WARNING("M2 model texture-failure warnings suppressed after ",
                                    loggedModelTextureFails, " entries");
                        loggedModelTextureFailSuppressed = true;
                    }
                }
                if (isInvisibleTrap) {
                    LOG_INFO("  InvisibleTrap texture[", ti, "]: ", texPath, " -> ", (failed ? "WHITE" : "OK"));
                }
                allTextures.push_back(texPtr);
                textureLoadFailed.push_back(failed);
                textureKeysLower.push_back(std::move(texKey));
            } else {
                if (isInvisibleTrap) {
                    LOG_INFO("  InvisibleTrap texture[", ti, "]: EMPTY (using white fallback)");
                }
                allTextures.push_back(whiteTexture_.get());
                textureLoadFailed.push_back(false);  // Empty filename = intentional white (type!=0)
                textureKeysLower.emplace_back();
            }
        }
    }

    static const bool kGlowDiag = envFlagEnabled("WOWEE_M2_GLOW_DIAG", false);
    if (kGlowDiag) {
        if (gpuModel.isLanternLike) {
            for (size_t ti = 0; ti < model.textures.size(); ++ti) {
                const std::string key = (ti < textureKeysLower.size()) ? textureKeysLower[ti] : std::string();
                LOG_DEBUG("M2 GLOW TEX '", model.name, "' tex[", ti, "]='", key, "' flags=0x",
                          std::hex, model.textures[ti].flags, std::dec);
            }
        }
    }

    // Copy particle emitter data and resolve textures
    gpuModel.particleEmitters = model.particleEmitters;
    gpuModel.particleTextures.resize(model.particleEmitters.size(), whiteTexture_.get());
    for (size_t ei = 0; ei < model.particleEmitters.size(); ei++) {
        uint16_t texIdx = model.particleEmitters[ei].texture;
        if (texIdx < allTextures.size() && allTextures[texIdx] != nullptr) {
            gpuModel.particleTextures[ei] = allTextures[texIdx];
        } else {
            LOG_WARNING("M2 '", model.name, "' particle emitter[", ei,
                        "] texture index ", texIdx, " out of range (", allTextures.size(),
                        " textures) — using white fallback");
        }
    }

    // Pre-allocate one stable descriptor set per particle emitter to avoid per-frame allocation.
    // This prevents materialDescPool_ exhaustion when many emitters are active each frame.
    if (particleTexLayout_ && materialDescPool_ && !model.particleEmitters.empty()) {
        VkDevice device = vkCtx_->getDevice();
        gpuModel.particleTexSets.resize(model.particleEmitters.size(), VK_NULL_HANDLE);
        for (size_t ei = 0; ei < model.particleEmitters.size(); ei++) {
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = materialDescPool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &particleTexLayout_;
            if (vkAllocateDescriptorSets(device, &ai, &gpuModel.particleTexSets[ei]) == VK_SUCCESS) {
                VkTexture* tex = gpuModel.particleTextures[ei];
                VkDescriptorImageInfo imgInfo = tex->descriptorInfo();
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                write.dstSet = gpuModel.particleTexSets[ei];
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &imgInfo;
                vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            }
        }
    }

    // Copy ribbon emitter data and resolve textures
    gpuModel.ribbonEmitters = model.ribbonEmitters;
    if (!model.ribbonEmitters.empty()) {
        VkDevice device = vkCtx_->getDevice();
        gpuModel.ribbonTextures.resize(model.ribbonEmitters.size(), whiteTexture_.get());
        gpuModel.ribbonTexSets.resize(model.ribbonEmitters.size(), VK_NULL_HANDLE);
        for (size_t ri = 0; ri < model.ribbonEmitters.size(); ri++) {
            // Resolve texture via textureLookup table
            uint16_t texLookupIdx = model.ribbonEmitters[ri].textureIndex;
            uint32_t texIdx = (texLookupIdx < model.textureLookup.size())
                              ? model.textureLookup[texLookupIdx] : UINT32_MAX;
            if (texIdx < allTextures.size() && allTextures[texIdx] != nullptr) {
                gpuModel.ribbonTextures[ri] = allTextures[texIdx];
            } else {
                LOG_WARNING("M2 '", model.name, "' ribbon emitter[", ri,
                            "] texLookup=", texLookupIdx, " resolved texIdx=", texIdx,
                            " out of range (", allTextures.size(),
                            " textures) — using white fallback");
            }
            // Allocate descriptor set (reuse particleTexLayout_ = single sampler)
            if (particleTexLayout_ && materialDescPool_) {
                VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                ai.descriptorPool = materialDescPool_;
                ai.descriptorSetCount = 1;
                ai.pSetLayouts = &particleTexLayout_;
                if (vkAllocateDescriptorSets(device, &ai, &gpuModel.ribbonTexSets[ri]) == VK_SUCCESS) {
                    VkTexture* tex = gpuModel.ribbonTextures[ri];
                    VkDescriptorImageInfo imgInfo = tex->descriptorInfo();
                    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    write.dstSet = gpuModel.ribbonTexSets[ri];
                    write.dstBinding = 0;
                    write.descriptorCount = 1;
                    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    write.pImageInfo = &imgInfo;
                    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
                }
            }
        }
        LOG_DEBUG("  Ribbon emitters loaded: ", model.ribbonEmitters.size());
    }

    // Copy texture transform data for UV animation
    gpuModel.textureTransforms = model.textureTransforms;
    gpuModel.textureTransformLookup = model.textureTransformLookup;
    gpuModel.hasTextureAnimation = false;

    // Build per-batch GPU entries
    if (!model.batches.empty()) {
        for (const auto& batch : model.batches) {
            M2ModelGPU::BatchGPU bgpu;
            bgpu.indexStart = batch.indexStart;
            bgpu.indexCount = batch.indexCount;

            // Store texture animation index from batch
            bgpu.textureAnimIndex = batch.textureAnimIndex;
            if (bgpu.textureAnimIndex != 0xFFFF) {
                gpuModel.hasTextureAnimation = true;
            }

            // Store blend mode and flags from material
            if (batch.materialIndex < model.materials.size()) {
                bgpu.blendMode = model.materials[batch.materialIndex].blendMode;
                bgpu.materialFlags = model.materials[batch.materialIndex].flags;
                if (bgpu.blendMode >= 2) gpuModel.hasTransparentBatches = true;
            }

            // Copy LOD level from batch
            bgpu.submeshLevel = batch.submeshLevel;

            // Resolve texture: batch.textureIndex → textureLookup → allTextures
            VkTexture* tex = whiteTexture_.get();
            bool texFailed = false;
            std::string batchTexKeyLower;
            if (batch.textureIndex < model.textureLookup.size()) {
                uint16_t texIdx = model.textureLookup[batch.textureIndex];
                if (texIdx < allTextures.size()) {
                    tex = allTextures[texIdx];
                    texFailed = (texIdx < textureLoadFailed.size()) && textureLoadFailed[texIdx];
                    if (texIdx < textureKeysLower.size()) {
                        batchTexKeyLower = textureKeysLower[texIdx];
                    }
                }
                if (texIdx < model.textures.size()) {
                    bgpu.texFlags = static_cast<uint8_t>(model.textures[texIdx].flags & 0x3);
                }
            } else if (!allTextures.empty()) {
                LOG_WARNING("M2 '", model.name, "' batch textureIndex ", batch.textureIndex,
                            " out of range (textureLookup size=", model.textureLookup.size(),
                            ") — falling back to texture[0]");
                tex = allTextures[0];
                texFailed = !textureLoadFailed.empty() && textureLoadFailed[0];
                if (!textureKeysLower.empty()) {
                    batchTexKeyLower = textureKeysLower[0];
                }
            }

            if (texFailed && groundDetailModel) {
                static const std::string kDetailFallbackTexture = "World\\NoDXT\\Detail\\8des_detaildoodads01.blp";
                VkTexture* fallbackTex = loadTexture(kDetailFallbackTexture, 0);
                if (fallbackTex != nullptr && fallbackTex != whiteTexture_.get()) {
                    tex = fallbackTex;
                    texFailed = false;
                }
            }
            bgpu.texture = tex;
            const auto tcls = classifyBatchTexture(batchTexKeyLower);
            const bool modelLanternFamily = gpuModel.isLanternLike;
            bgpu.lanternGlowHint =
                tcls.exactLanternGlowTex ||
                ((tcls.hasGlowToken || (modelLanternFamily && tcls.hasFlameToken)) &&
                 (tcls.lanternFamily || modelLanternFamily) &&
                 (!tcls.likelyFlame || modelLanternFamily));
            bgpu.glowCardLike = bgpu.lanternGlowHint && tcls.hasGlowCardToken;
            bgpu.glowTint = tcls.glowTint;
            if (tex != nullptr && tex != whiteTexture_.get()) {
                auto pit = texturePropsByPtr_.find(tex);
                if (pit != texturePropsByPtr_.end()) {
                    bgpu.hasAlpha = pit->second.hasAlpha;
                    bgpu.colorKeyBlack = pit->second.colorKeyBlack;
                }
            }
            // textureCoordIndex is an index into a texture coord combo table, not directly
            // a UV set selector. Most batches have index=0 (UV set 0). We always use UV set 0
            // since we don't have the full combo table — dual-UV effects are rare edge cases.
            bgpu.textureUnit = 0;

            // Start at full opacity; hide only if texture failed to load.
            bgpu.batchOpacity = (texFailed && !groundDetailModel) ? 0.0f : 1.0f;

            // Apply at-rest transparency and color alpha from the M2 animation tracks.
            // These provide per-batch opacity for ghosts, ethereal effects, fading doodads, etc.
            // Skip zero values: some animated tracks start at 0 and animate up, and baking
            // that first keyframe would make the entire batch permanently invisible.
            if (bgpu.batchOpacity > 0.0f) {
                float animAlpha = 1.0f;
                if (batch.colorIndex < model.colorAlphas.size()) {
                    float ca = model.colorAlphas[batch.colorIndex];
                    if (ca > 0.001f) animAlpha *= ca;
                }
                if (batch.transparencyIndex < model.textureWeights.size()) {
                    float tw = model.textureWeights[batch.transparencyIndex];
                    if (tw > 0.001f) animAlpha *= tw;
                }
                bgpu.batchOpacity *= animAlpha;
            }

            // Compute batch center and radius for glow sprite positioning
            if ((bgpu.blendMode >= 3 || bgpu.colorKeyBlack) && batch.indexCount > 0) {
                glm::vec3 sum(0.0f);
                uint32_t counted = 0;
                for (uint32_t j = batch.indexStart; j < batch.indexStart + batch.indexCount; j++) {
                    if (j < model.indices.size()) {
                        uint16_t vi = model.indices[j];
                        if (vi < model.vertices.size()) {
                            sum += model.vertices[vi].position;
                            counted++;
                        }
                    }
                }
                if (counted > 0) {
                    bgpu.center = sum / static_cast<float>(counted);
                    float maxDist = 0.0f;
                    for (uint32_t j = batch.indexStart; j < batch.indexStart + batch.indexCount; j++) {
                        if (j < model.indices.size()) {
                            uint16_t vi = model.indices[j];
                            if (vi < model.vertices.size()) {
                                float d = glm::length(model.vertices[vi].position - bgpu.center);
                                maxDist = std::max(maxDist, d);
                            }
                        }
                    }
                    bgpu.glowSize = std::max(maxDist, 0.5f);
                }
            }

            // Optional diagnostics for glow/light batches (disabled by default).
            if (kGlowDiag && gpuModel.isLanternLike) {
                LOG_DEBUG("M2 GLOW DIAG '", model.name, "' batch ", gpuModel.batches.size(),
                          ": blend=", bgpu.blendMode, " matFlags=0x",
                          std::hex, bgpu.materialFlags, std::dec,
                          " colorKey=", bgpu.colorKeyBlack ? "Y" : "N",
                          " hasAlpha=", bgpu.hasAlpha ? "Y" : "N",
                          " unlit=", (bgpu.materialFlags & 0x01) ? "Y" : "N",
                          " lanternHint=", bgpu.lanternGlowHint ? "Y" : "N",
                          " glowSize=", bgpu.glowSize,
                          " tex=", bgpu.texture,
                          " idxCount=", bgpu.indexCount);
            }
            gpuModel.batches.push_back(bgpu);
        }
    } else {
        // Fallback: single batch covering all indices with first texture
        M2ModelGPU::BatchGPU bgpu;
        bgpu.indexStart = 0;
        bgpu.indexCount = gpuModel.indexCount;
        bgpu.texture = allTextures.empty() ? whiteTexture_.get() : allTextures[0];
        if (bgpu.texture != nullptr && bgpu.texture != whiteTexture_.get()) {
            auto pit = texturePropsByPtr_.find(bgpu.texture);
            if (pit != texturePropsByPtr_.end()) {
                bgpu.hasAlpha = pit->second.hasAlpha;
                bgpu.colorKeyBlack = pit->second.colorKeyBlack;
            }
        }
        gpuModel.batches.push_back(bgpu);
    }

    // Detect particle emitter volume models: box mesh (24 verts, 36 indices)
    // with disproportionately large bounds. These are invisible bounding volumes
    // that only exist to spawn particles — their mesh should never be rendered.
    if (!isInvisibleTrap && !groundDetailModel &&
        gpuModel.vertexCount <= 24 && gpuModel.indexCount <= 36
        && !model.particleEmitters.empty()) {
        glm::vec3 size = gpuModel.boundMax - gpuModel.boundMin;
        float maxDim = std::max({size.x, size.y, size.z});
        if (maxDim > 5.0f) {
            gpuModel.isInvisibleTrap = true;
            LOG_DEBUG("M2 emitter volume hidden: '", model.name, "' size=(",
                      size.x, " x ", size.y, " x ", size.z, ")");
        }
    }

    vkCtx_->endUploadBatch();

    // Allocate Vulkan descriptor sets and UBOs for each batch
    for (auto& bgpu : gpuModel.batches) {
        // Create combined UBO for M2Params (binding 1) + M2Material (binding 2)
        // We allocate them as separate buffers for clarity
        VmaAllocationInfo matAllocInfo{};
        {
            VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bci.size = sizeof(M2MaterialUBO);
            bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo aci{};
            aci.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            aci.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            vmaCreateBuffer(vkCtx_->getAllocator(), &bci, &aci, &bgpu.materialUBO, &bgpu.materialUBOAlloc, &matAllocInfo);

            // Write initial material data (static per-batch — fadeAlpha/interiorDarken updated at draw time)
            M2MaterialUBO mat{};
            mat.hasTexture = (bgpu.texture != nullptr && bgpu.texture != whiteTexture_.get()) ? 1 : 0;
            mat.alphaTest = (bgpu.blendMode == 1 || (bgpu.blendMode >= 2 && !bgpu.hasAlpha)) ? 1 : 0;
            mat.colorKeyBlack = bgpu.colorKeyBlack ? 1 : 0;
            mat.colorKeyThreshold = 0.08f;
            mat.unlit = (bgpu.materialFlags & 0x01) ? 1 : 0;
            mat.blendMode = bgpu.blendMode;
            mat.fadeAlpha = 1.0f;
            mat.interiorDarken = 0.0f;
            mat.specularIntensity = 0.5f;
            memcpy(matAllocInfo.pMappedData, &mat, sizeof(mat));
            bgpu.materialUBOMapped = matAllocInfo.pMappedData;
        }

        // Allocate descriptor set and write all bindings
        bgpu.materialSet = allocateMaterialSet();
        if (bgpu.materialSet) {
            VkTexture* batchTex = bgpu.texture ? bgpu.texture : whiteTexture_.get();
            VkDescriptorImageInfo imgInfo = batchTex->descriptorInfo();

            VkDescriptorBufferInfo matBufInfo{};
            matBufInfo.buffer = bgpu.materialUBO;
            matBufInfo.offset = 0;
            matBufInfo.range = sizeof(M2MaterialUBO);

            VkWriteDescriptorSet writes[2] = {};
            // binding 0: texture
            writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[0].dstSet = bgpu.materialSet;
            writes[0].dstBinding = 0;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[0].pImageInfo = &imgInfo;
            // binding 2: M2Material UBO
            writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[1].dstSet = bgpu.materialSet;
            writes[1].dstBinding = 2;
            writes[1].descriptorCount = 1;
            writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[1].pBufferInfo = &matBufInfo;

            vkUpdateDescriptorSets(vkCtx_->getDevice(), 2, writes, 0, nullptr);
        }
    }

    // Pre-compute available LOD levels to avoid per-instance batch iteration
    gpuModel.availableLODs = 0;
    for (const auto& b : gpuModel.batches) {
        if (b.submeshLevel < 8) gpuModel.availableLODs |= (1u << b.submeshLevel);
    }

    models[modelId] = std::move(gpuModel);
    spatialIndexDirty_ = true;  // Map may have rehashed — refresh cachedModel pointers

    LOG_DEBUG("Loaded M2 model: ", model.name, " (", models[modelId].vertexCount, " vertices, ",
              models[modelId].indexCount / 3, " triangles, ", models[modelId].batches.size(), " batches)");


    return true;
}

uint32_t M2Renderer::createInstance(uint32_t modelId, const glm::vec3& position,
                                     const glm::vec3& rotation, float scale) {
    auto modelIt = models.find(modelId);
    if (modelIt == models.end()) {
        LOG_WARNING("Cannot create instance: model ", modelId, " not loaded");
        return 0;
    }
    const auto& mdlRef = modelIt->second;
    modelUnusedSince_.erase(modelId);

    // Deduplicate: skip if same model already at nearly the same position.
    // Uses hash map for O(1) lookup instead of O(N) scan.
    if (!mdlRef.isGroundDetail) {
        DedupKey dk{modelId,
                    static_cast<int32_t>(std::round(position.x * 10.0f)),
                    static_cast<int32_t>(std::round(position.y * 10.0f)),
                    static_cast<int32_t>(std::round(position.z * 10.0f))};
        auto dit = instanceDedupMap_.find(dk);
        if (dit != instanceDedupMap_.end()) {
            return dit->second;
        }
    }

    M2Instance instance;
    instance.id = nextInstanceId++;
    instance.modelId = modelId;
    instance.position = position;
    if (mdlRef.isGroundDetail) {
        instance.position.z -= computeGroundDetailDownOffset(mdlRef, scale);
    }
    instance.rotation = rotation;
    instance.scale = scale;
    instance.updateModelMatrix();
    glm::vec3 localMin, localMax;
    getTightCollisionBounds(mdlRef, localMin, localMax);
    transformAABB(instance.modelMatrix, localMin, localMax, instance.worldBoundsMin, instance.worldBoundsMax);

    // Cache model flags on instance to avoid per-frame hash lookups
    instance.cachedHasAnimation = mdlRef.hasAnimation;
    instance.cachedDisableAnimation = mdlRef.disableAnimation;
    instance.cachedIsSmoke = mdlRef.isSmoke;
    instance.cachedHasParticleEmitters = !mdlRef.particleEmitters.empty();
    instance.cachedBoundRadius = mdlRef.boundRadius;
    instance.cachedIsGroundDetail = mdlRef.isGroundDetail;
    instance.cachedIsInvisibleTrap = mdlRef.isInvisibleTrap;
    instance.cachedIsInstancePortal = mdlRef.isInstancePortal;
    instance.cachedIsValid = mdlRef.isValid();
    instance.cachedModel = &mdlRef;

    // Initialize animation: play first sequence (usually Stand/Idle)
    const auto& mdl = mdlRef;
    if (mdl.hasAnimation && !mdl.disableAnimation && !mdl.sequences.empty()) {
        instance.currentSequenceIndex = 0;
        instance.idleSequenceIndex = 0;
        instance.animDuration = static_cast<float>(mdl.sequences[0].duration);
        instance.animTime = static_cast<float>(randRange(std::max(1u, mdl.sequences[0].duration)));
        instance.variationTimer = randFloat(3000.0f, 11000.0f);

        // Seed bone matrices from an existing instance of the same model so the
        // new instance renders immediately instead of being invisible until the
        // next update() computes bones (prevents pop-in flash).
        for (const auto& existing : instances) {
            if (existing.modelId == modelId && !existing.boneMatrices.empty()) {
                instance.boneMatrices = existing.boneMatrices;
                instance.bonesDirty[0] = instance.bonesDirty[1] = true;
                break;
            }
        }
        // If no sibling exists yet, compute bones immediately
        if (instance.boneMatrices.empty()) {
            computeBoneMatrices(mdlRef, instance);
        }
    }

    // Register in dedup map before pushing (uses original position, not ground-adjusted)
    if (!mdlRef.isGroundDetail) {
        DedupKey dk{modelId,
                    static_cast<int32_t>(std::round(position.x * 10.0f)),
                    static_cast<int32_t>(std::round(position.y * 10.0f)),
                    static_cast<int32_t>(std::round(position.z * 10.0f))};
        instanceDedupMap_[dk] = instance.id;
    }

    instances.push_back(instance);
    size_t idx = instances.size() - 1;
    // Track special instances for fast-path iteration
    if (mdlRef.isSmoke) {
        smokeInstanceIndices_.push_back(idx);
    }
    if (mdlRef.isInstancePortal) {
        portalInstanceIndices_.push_back(idx);
    }
    if (!mdlRef.particleEmitters.empty()) {
        particleInstanceIndices_.push_back(idx);
    }
    if (mdlRef.hasAnimation && !mdlRef.disableAnimation) {
        animatedInstanceIndices_.push_back(idx);
    } else if (!mdlRef.particleEmitters.empty()) {
        particleOnlyInstanceIndices_.push_back(idx);
    }
    instanceIndexById[instance.id] = idx;
    GridCell minCell = toCell(instance.worldBoundsMin);
    GridCell maxCell = toCell(instance.worldBoundsMax);
    for (int z = minCell.z; z <= maxCell.z; z++) {
        for (int y = minCell.y; y <= maxCell.y; y++) {
            for (int x = minCell.x; x <= maxCell.x; x++) {
                spatialGrid[GridCell{x, y, z}].push_back(instance.id);
            }
        }
    }

    return instance.id;
}

uint32_t M2Renderer::createInstanceWithMatrix(uint32_t modelId, const glm::mat4& modelMatrix,
                                                const glm::vec3& position) {
    if (models.find(modelId) == models.end()) {
        LOG_WARNING("Cannot create instance: model ", modelId, " not loaded");
        return 0;
    }
    modelUnusedSince_.erase(modelId);

    // Deduplicate: O(1) hash lookup
    {
        DedupKey dk{modelId,
                    static_cast<int32_t>(std::round(position.x * 10.0f)),
                    static_cast<int32_t>(std::round(position.y * 10.0f)),
                    static_cast<int32_t>(std::round(position.z * 10.0f))};
        auto dit = instanceDedupMap_.find(dk);
        if (dit != instanceDedupMap_.end()) {
            return dit->second;
        }
    }

    M2Instance instance;
    instance.id = nextInstanceId++;
    instance.modelId = modelId;
    instance.position = position;  // Used for frustum culling
    instance.rotation = glm::vec3(0.0f);
    instance.scale = 1.0f;
    instance.modelMatrix = modelMatrix;
    instance.invModelMatrix = glm::inverse(modelMatrix);
    glm::vec3 localMin, localMax;
    getTightCollisionBounds(models[modelId], localMin, localMax);
    transformAABB(instance.modelMatrix, localMin, localMax, instance.worldBoundsMin, instance.worldBoundsMax);
    // Cache model flags on instance to avoid per-frame hash lookups
    const auto& mdl2 = models[modelId];
    instance.cachedHasAnimation = mdl2.hasAnimation;
    instance.cachedDisableAnimation = mdl2.disableAnimation;
    instance.cachedIsSmoke = mdl2.isSmoke;
    instance.cachedHasParticleEmitters = !mdl2.particleEmitters.empty();
    instance.cachedBoundRadius = mdl2.boundRadius;
    instance.cachedIsGroundDetail = mdl2.isGroundDetail;
    instance.cachedIsInvisibleTrap = mdl2.isInvisibleTrap;
    instance.cachedIsValid = mdl2.isValid();
    instance.cachedModel = &mdl2;

    // Initialize animation
    if (mdl2.hasAnimation && !mdl2.disableAnimation && !mdl2.sequences.empty()) {
        instance.currentSequenceIndex = 0;
        instance.idleSequenceIndex = 0;
        instance.animDuration = static_cast<float>(mdl2.sequences[0].duration);
        instance.animTime = static_cast<float>(randRange(std::max(1u, mdl2.sequences[0].duration)));
        instance.variationTimer = randFloat(3000.0f, 11000.0f);

        // Seed bone matrices from an existing sibling so the instance renders immediately
        for (const auto& existing : instances) {
            if (existing.modelId == modelId && !existing.boneMatrices.empty()) {
                instance.boneMatrices = existing.boneMatrices;
                instance.bonesDirty[0] = instance.bonesDirty[1] = true;
                break;
            }
        }
        if (instance.boneMatrices.empty()) {
            computeBoneMatrices(mdl2, instance);
        }
    } else {
        instance.animTime = randFloat(0.0f, 10000.0f);
    }

    // Register in dedup map
    {
        DedupKey dk{modelId,
                    static_cast<int32_t>(std::round(position.x * 10.0f)),
                    static_cast<int32_t>(std::round(position.y * 10.0f)),
                    static_cast<int32_t>(std::round(position.z * 10.0f))};
        instanceDedupMap_[dk] = instance.id;
    }

    instances.push_back(instance);
    size_t idx = instances.size() - 1;
    if (mdl2.isSmoke) {
        smokeInstanceIndices_.push_back(idx);
    }
    if (!mdl2.particleEmitters.empty()) {
        particleInstanceIndices_.push_back(idx);
    }
    if (mdl2.hasAnimation && !mdl2.disableAnimation) {
        animatedInstanceIndices_.push_back(idx);
    } else if (!mdl2.particleEmitters.empty()) {
        particleOnlyInstanceIndices_.push_back(idx);
    }
    instanceIndexById[instance.id] = idx;
    GridCell minCell = toCell(instance.worldBoundsMin);
    GridCell maxCell = toCell(instance.worldBoundsMax);
    for (int z = minCell.z; z <= maxCell.z; z++) {
        for (int y = minCell.y; y <= maxCell.y; y++) {
            for (int x = minCell.x; x <= maxCell.x; x++) {
                spatialGrid[GridCell{x, y, z}].push_back(instance.id);
            }
        }
    }

    return instance.id;
}

// --- Bone animation helpers (same logic as CharacterRenderer) ---

static int findKeyframeIndex(const std::vector<uint32_t>& timestamps, float time) {
    if (timestamps.empty()) return -1;
    if (timestamps.size() == 1) return 0;
    // Binary search using float comparison to match original semantics exactly
    auto it = std::upper_bound(timestamps.begin(), timestamps.end(), time,
        [](float t, uint32_t ts) { return t < static_cast<float>(ts); });
    if (it == timestamps.begin()) return 0;
    size_t idx = static_cast<size_t>(it - timestamps.begin()) - 1;
    return static_cast<int>(std::min(idx, timestamps.size() - 2));
}

// Resolve sequence index and time for a track, handling global sequences.
static void resolveTrackTime(const pipeline::M2AnimationTrack& track,
                              int seqIdx, float time,
                              const std::vector<uint32_t>& globalSeqDurations,
                              int& outSeqIdx, float& outTime) {
    if (track.globalSequence >= 0 &&
        static_cast<size_t>(track.globalSequence) < globalSeqDurations.size()) {
        // Global sequence: always use sub-array 0, wrap time at global duration
        outSeqIdx = 0;
        float dur = static_cast<float>(globalSeqDurations[track.globalSequence]);
        if (dur > 0.0f) {
            // Use iterative subtraction instead of fmod() to preserve precision
            outTime = time;
            while (outTime >= dur) {
                outTime -= dur;
            }
        } else {
            outTime = 0.0f;
        }
    } else {
        outSeqIdx = seqIdx;
        outTime = time;
    }
}

static glm::vec3 interpVec3(const pipeline::M2AnimationTrack& track,
                             int seqIdx, float time, const glm::vec3& def,
                             const std::vector<uint32_t>& globalSeqDurations) {
    if (!track.hasData()) return def;
    int si; float t;
    resolveTrackTime(track, seqIdx, time, globalSeqDurations, si, t);
    if (si < 0 || si >= static_cast<int>(track.sequences.size())) return def;
    const auto& keys = track.sequences[si];
    if (keys.timestamps.empty() || keys.vec3Values.empty()) return def;
    auto safe = [&](const glm::vec3& v) -> glm::vec3 {
        if (std::isnan(v.x) || std::isnan(v.y) || std::isnan(v.z)) return def;
        return v;
    };
    if (keys.vec3Values.size() == 1) return safe(keys.vec3Values[0]);
    int idx = findKeyframeIndex(keys.timestamps, t);
    if (idx < 0) return def;
    size_t i0 = static_cast<size_t>(idx);
    size_t i1 = std::min(i0 + 1, keys.vec3Values.size() - 1);
    if (i0 == i1) return safe(keys.vec3Values[i0]);
    float t0 = static_cast<float>(keys.timestamps[i0]);
    float t1 = static_cast<float>(keys.timestamps[i1]);
    float dur = t1 - t0;
    float frac = (dur > 0.0f) ? glm::clamp((t - t0) / dur, 0.0f, 1.0f) : 0.0f;
    return safe(glm::mix(keys.vec3Values[i0], keys.vec3Values[i1], frac));
}

static glm::quat interpQuat(const pipeline::M2AnimationTrack& track,
                              int seqIdx, float time,
                              const std::vector<uint32_t>& globalSeqDurations) {
    glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
    if (!track.hasData()) return identity;
    int si; float t;
    resolveTrackTime(track, seqIdx, time, globalSeqDurations, si, t);
    if (si < 0 || si >= static_cast<int>(track.sequences.size())) return identity;
    const auto& keys = track.sequences[si];
    if (keys.timestamps.empty() || keys.quatValues.empty()) return identity;
    auto safe = [&](const glm::quat& q) -> glm::quat {
        float lenSq = q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w;
        if (lenSq < 0.000001f || std::isnan(lenSq)) return identity;
        return q;
    };
    if (keys.quatValues.size() == 1) return safe(keys.quatValues[0]);
    int idx = findKeyframeIndex(keys.timestamps, t);
    if (idx < 0) return identity;
    size_t i0 = static_cast<size_t>(idx);
    size_t i1 = std::min(i0 + 1, keys.quatValues.size() - 1);
    if (i0 == i1) return safe(keys.quatValues[i0]);
    float t0 = static_cast<float>(keys.timestamps[i0]);
    float t1 = static_cast<float>(keys.timestamps[i1]);
    float dur = t1 - t0;
    float frac = (dur > 0.0f) ? glm::clamp((t - t0) / dur, 0.0f, 1.0f) : 0.0f;
    return glm::slerp(safe(keys.quatValues[i0]), safe(keys.quatValues[i1]), frac);
}

static void computeBoneMatrices(const M2ModelGPU& model, M2Instance& instance) {
    ZoneScopedN("M2::computeBoneMatrices");
    size_t numBones = std::min(model.bones.size(), size_t(128));
    if (numBones == 0) return;
    instance.boneMatrices.resize(numBones);
    const auto& gsd = model.globalSequenceDurations;

    for (size_t i = 0; i < numBones; i++) {
        const auto& bone = model.bones[i];
        glm::vec3 trans = interpVec3(bone.translation, instance.currentSequenceIndex, instance.animTime, glm::vec3(0.0f), gsd);
        glm::quat rot = interpQuat(bone.rotation, instance.currentSequenceIndex, instance.animTime, gsd);
        glm::vec3 scl = interpVec3(bone.scale, instance.currentSequenceIndex, instance.animTime, glm::vec3(1.0f), gsd);

        // Sanity check scale to avoid degenerate matrices
        if (scl.x < 0.001f) scl.x = 1.0f;
        if (scl.y < 0.001f) scl.y = 1.0f;
        if (scl.z < 0.001f) scl.z = 1.0f;

        glm::mat4 local = glm::translate(glm::mat4(1.0f), bone.pivot);
        local = glm::translate(local, trans);
        local *= glm::toMat4(rot);
        local = glm::scale(local, scl);
        local = glm::translate(local, -bone.pivot);

        if (bone.parentBone >= 0 && static_cast<size_t>(bone.parentBone) < numBones) {
            instance.boneMatrices[i] = instance.boneMatrices[bone.parentBone] * local;
        } else {
            instance.boneMatrices[i] = local;
        }
    }
    instance.bonesDirty[0] = instance.bonesDirty[1] = true;
}

void M2Renderer::update(float deltaTime, const glm::vec3& cameraPos, const glm::mat4& viewProjection) {
    ZoneScopedN("M2Renderer::update");
    if (spatialIndexDirty_) {
        rebuildSpatialIndex();
    }

    float dtMs = deltaTime * 1000.0f;

    // Cache camera state for frustum-culling bone computation
    cachedCamPos_ = cameraPos;
    const float maxRenderDistance = (instances.size() > 2000) ? 800.0f : 2800.0f;
    cachedMaxRenderDistSq_ = maxRenderDistance * maxRenderDistance;

    // Build frustum for culling bones
    Frustum updateFrustum;
    updateFrustum.extractFromMatrix(viewProjection);

    // --- Smoke particle spawning (only iterate tracked smoke instances) ---
    std::uniform_real_distribution<float> distXY(-0.4f, 0.4f);
    std::uniform_real_distribution<float> distVelXY(-0.3f, 0.3f);
    std::uniform_real_distribution<float> distVelZ(3.0f, 5.0f);
    std::uniform_real_distribution<float> distLife(4.0f, 7.0f);
    std::uniform_real_distribution<float> distDrift(-0.2f, 0.2f);

    smokeEmitAccum += deltaTime;
    constexpr float emitInterval = kSmokeEmitInterval;  // 48 particles per second per emitter

    if (smokeEmitAccum >= emitInterval &&
        static_cast<int>(smokeParticles.size()) < MAX_SMOKE_PARTICLES) {
        for (size_t si : smokeInstanceIndices_) {
            if (si >= instances.size()) continue;
            auto& instance = instances[si];

            glm::vec3 emitWorld = glm::vec3(instance.modelMatrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
            bool spark = (smokeRng() % 8 == 0);

            SmokeParticle p;
            p.position = emitWorld + glm::vec3(distXY(smokeRng), distXY(smokeRng), 0.0f);
            if (spark) {
                p.velocity = glm::vec3(distVelXY(smokeRng) * 2.0f, distVelXY(smokeRng) * 2.0f, distVelZ(smokeRng) * 1.5f);
                p.maxLife = 0.8f + static_cast<float>(smokeRng() % 100) / 100.0f * 1.2f;
                p.size = 0.5f;
                p.isSpark = 1.0f;
            } else {
                p.velocity = glm::vec3(distVelXY(smokeRng), distVelXY(smokeRng), distVelZ(smokeRng));
                p.maxLife = distLife(smokeRng);
                p.size = 1.0f;
                p.isSpark = 0.0f;
            }
            p.life = 0.0f;
            p.instanceId = instance.id;
            smokeParticles.push_back(p);
            if (static_cast<int>(smokeParticles.size()) >= MAX_SMOKE_PARTICLES) break;
        }
        smokeEmitAccum = 0.0f;
    }

    // --- Update existing smoke particles (swap-and-pop for O(1) removal) ---
    for (size_t i = 0; i < smokeParticles.size(); ) {
        auto& p = smokeParticles[i];
        p.life += deltaTime;
        if (p.life >= p.maxLife) {
            smokeParticles[i] = smokeParticles.back();
            smokeParticles.pop_back();
            continue;
        }
        p.position += p.velocity * deltaTime;
        p.velocity.z *= 0.98f;  // Slight deceleration
        p.velocity.x += distDrift(smokeRng) * deltaTime;
        p.velocity.y += distDrift(smokeRng) * deltaTime;
        // Grow from 1.0 to 3.5 over lifetime
        float t = p.life / p.maxLife;
        p.size = 1.0f + t * 2.5f;
        ++i;
    }

    // --- Spin instance portals ---
    static constexpr float PORTAL_SPIN_SPEED = 1.2f; // radians/sec
    static constexpr float kTwoPi = 6.2831853f;
    for (size_t idx : portalInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& inst = instances[idx];
        inst.portalSpinAngle += PORTAL_SPIN_SPEED * deltaTime;
        if (inst.portalSpinAngle > kTwoPi)
            inst.portalSpinAngle -= kTwoPi;
        inst.rotation.z = inst.portalSpinAngle;
        inst.updateModelMatrix();
    }

    // --- Normal M2 animation update ---
    // Advance animTime for ALL instances (needed for texture UV animation on static doodads).
    // This is a tight loop touching only one float per instance — no hash lookups.
    for (auto& instance : instances) {
        instance.animTime += dtMs;
    }
    // Wrap animTime for particle-only instances so emission rate tracks keep looping.
    // 3333ms chosen as a safe wrap period: long enough to cover the longest known M2
    // particle emission cycle (~3s for torch/campfire effects) while preventing float
    // precision loss that accumulates over hours of runtime.
    static constexpr float kParticleWrapMs = 3333.0f;
    for (size_t idx : particleOnlyInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& instance = instances[idx];
        // Use iterative subtraction instead of fmod() to preserve precision
        while (instance.animTime > kParticleWrapMs) {
            instance.animTime -= kParticleWrapMs;
        }
    }

    boneWorkIndices_.clear();
    boneWorkIndices_.reserve(animatedInstanceIndices_.size());

    // Update animated instances (full animation state + bone computation culling)
    // Note: animTime was already advanced by dtMs in the global loop above.
    // Here we apply the speed factor: subtract the base dtMs and add dtMs*speed.
    for (size_t idx : animatedInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& instance = instances[idx];

        instance.animTime += dtMs * (instance.animSpeed - 1.0f);

        // For animation looping/variation, we need the actual model data.
        if (!instance.cachedModel) continue;
        const M2ModelGPU& model = *instance.cachedModel;

        // Validate sequence index
        if (instance.currentSequenceIndex < 0 ||
            instance.currentSequenceIndex >= static_cast<int>(model.sequences.size())) {
            instance.currentSequenceIndex = 0;
            if (!model.sequences.empty()) {
                instance.animDuration = static_cast<float>(model.sequences[0].duration);
            }
        }

        // Handle animation looping / variation transitions
        if (instance.animDuration <= 0.0f && instance.cachedHasParticleEmitters) {
            instance.animDuration = 3333.0f;
        }
        if (instance.animDuration > 0.0f && instance.animTime >= instance.animDuration) {
            if (instance.playingVariation) {
                instance.playingVariation = false;
                instance.currentSequenceIndex = instance.idleSequenceIndex;
                if (instance.idleSequenceIndex < static_cast<int>(model.sequences.size())) {
                    instance.animDuration = static_cast<float>(model.sequences[instance.idleSequenceIndex].duration);
                }
                instance.animTime = 0.0f;
                instance.variationTimer = randFloat(4000.0f, 10000.0f);
            } else {
                // Use iterative subtraction instead of fmod() to preserve precision
                float duration = std::max(1.0f, instance.animDuration);
                while (instance.animTime >= duration) {
                    instance.animTime -= duration;
                }
            }
        }

        // Idle variation timer
        if (!instance.playingVariation && model.idleVariationIndices.size() > 1) {
            instance.variationTimer -= dtMs;
            if (instance.variationTimer <= 0.0f) {
                int pick = static_cast<int>(randRange(static_cast<uint32_t>(model.idleVariationIndices.size())));
                int newSeq = model.idleVariationIndices[pick];
                if (newSeq != instance.currentSequenceIndex && newSeq < static_cast<int>(model.sequences.size())) {
                    instance.playingVariation = true;
                    instance.currentSequenceIndex = newSeq;
                    instance.animDuration = static_cast<float>(model.sequences[newSeq].duration);
                    instance.animTime = 0.0f;
                } else {
                    instance.variationTimer = randFloat(2000.0f, 6000.0f);
                }
            }
        }

        // Frustum + distance cull: skip expensive bone computation for off-screen instances.
        float worldRadius = instance.cachedBoundRadius * instance.scale;
        float cullRadius = worldRadius;
        glm::vec3 toCam = instance.position - cachedCamPos_;
        float distSq = glm::dot(toCam, toCam);
        float effectiveMaxDistSq = cachedMaxRenderDistSq_ * std::max(1.0f, cullRadius / 12.0f);
        if (distSq > effectiveMaxDistSq) continue;
        float paddedRadius = std::max(cullRadius * 1.5f, cullRadius + 3.0f);
        if (cullRadius > 0.0f && !updateFrustum.intersectsSphere(instance.position, paddedRadius)) continue;

        // LOD 3 skip: models beyond 150 units use the lowest LOD mesh which has
        // no visible skeletal animation.  Keep their last-computed bone matrices
        // (always valid — seeded on spawn) and avoid the expensive per-bone work.
        constexpr float kLOD3DistSq = 150.0f * 150.0f;
        if (distSq > kLOD3DistSq) continue;

        // Distance-based frame skipping: update distant bones less frequently
        uint32_t boneInterval = 1;
        if (distSq > 100.0f * 100.0f) boneInterval = 4;
        else if (distSq > 50.0f * 50.0f) boneInterval = 2;
        instance.frameSkipCounter++;
        if ((instance.frameSkipCounter % boneInterval) != 0) continue;

        boneWorkIndices_.push_back(idx);
    }

    // Compute bone matrices (expensive, parallel if enough work)
    const size_t animCount = boneWorkIndices_.size();
    if (animCount > 0) {
        static const size_t minParallelAnimInstances = std::max<size_t>(
            8, envSizeOrDefault("WOWEE_M2_ANIM_MT_MIN", 96));
        if (animCount < minParallelAnimInstances || numAnimThreads_ <= 1) {
            // Sequential — not enough work to justify thread overhead
            for (size_t i : boneWorkIndices_) {
                if (i >= instances.size()) continue;
                auto& inst = instances[i];
                if (!inst.cachedModel) continue;
                computeBoneMatrices(*inst.cachedModel, inst);
            }
        } else {
            // Parallel — dispatch across worker threads
            static const size_t minAnimWorkPerThread = std::max<size_t>(
                16, envSizeOrDefault("WOWEE_M2_ANIM_WORK_PER_THREAD", 64));
            const size_t maxUsefulThreads = std::max<size_t>(
                1, (animCount + minAnimWorkPerThread - 1) / minAnimWorkPerThread);
            const size_t numThreads = std::min(static_cast<size_t>(numAnimThreads_), maxUsefulThreads);
            if (numThreads <= 1) {
                for (size_t i : boneWorkIndices_) {
                    if (i >= instances.size()) continue;
                    auto& inst = instances[i];
                    if (!inst.cachedModel) continue;
                    computeBoneMatrices(*inst.cachedModel, inst);
                }
            } else {
                const size_t chunkSize = animCount / numThreads;
                const size_t remainder = animCount % numThreads;

                // Reuse persistent futures vector to avoid allocation
                animFutures_.clear();
                if (animFutures_.capacity() < numThreads) {
                    animFutures_.reserve(numThreads);
                }

                size_t start = 0;
                for (size_t t = 0; t < numThreads; ++t) {
                    size_t end = start + chunkSize + (t < remainder ? 1 : 0);
                    animFutures_.push_back(std::async(std::launch::async,
                        [this, start, end]() {
                            for (size_t j = start; j < end; ++j) {
                                size_t idx = boneWorkIndices_[j];
                                if (idx >= instances.size()) continue;
                                auto& inst = instances[idx];
                                if (!inst.cachedModel) continue;
                                computeBoneMatrices(*inst.cachedModel, inst);
                            }
                        }));
                    start = end;
                }

                for (auto& f : animFutures_) {
                    f.get();
                }
            }
        }
    }

    // Particle update (sequential — uses RNG, not thread-safe)
    // Only iterate instances that have particle emitters (pre-built list).
    for (size_t idx : particleInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& instance = instances[idx];
        // Distance cull: only update particles within visible range
        glm::vec3 toCam = instance.position - cachedCamPos_;
        float distSq = glm::dot(toCam, toCam);
        if (distSq > cachedMaxRenderDistSq_) continue;
        if (!instance.cachedModel) continue;
        emitParticles(instance, *instance.cachedModel, deltaTime);
        updateParticles(instance, deltaTime);
        if (!instance.cachedModel->ribbonEmitters.empty()) {
            updateRibbons(instance, *instance.cachedModel, deltaTime);
        }
    }

}

void M2Renderer::prepareRender(uint32_t frameIndex, const Camera& camera) {
    if (!initialized_ || instances.empty()) return;
    (void)camera;  // reserved for future frustum-based culling

    // --- Mega bone SSBO: assign slots and upload all animated instance bones ---
    // Slot 0 = identity (non-animated), slots 1..N = animated instances.
    uint32_t nextSlot = 1;
    for (size_t idx : animatedInstanceIndices_) {
        if (idx >= instances.size()) continue;
        auto& instance = instances[idx];

        if (instance.boneMatrices.empty()) {
            instance.megaBoneOffset = 0;  // Use identity slot
            continue;
        }

        if (nextSlot >= MEGA_BONE_MAX_INSTANCES) {
            instance.megaBoneOffset = 0;  // Overflow — use identity
            continue;
        }

        instance.megaBoneOffset = nextSlot * MAX_BONES_PER_INSTANCE;

        // Upload bone matrices to mega buffer
        if (megaBoneMapped_[frameIndex]) {
            int numBones = std::min(static_cast<int>(instance.boneMatrices.size()),
                                    static_cast<int>(MAX_BONES_PER_INSTANCE));
            auto* dst = static_cast<glm::mat4*>(megaBoneMapped_[frameIndex]) + instance.megaBoneOffset;
            memcpy(dst, instance.boneMatrices.data(), numBones * sizeof(glm::mat4));
        }

        nextSlot++;
    }
}

// Dispatch GPU frustum culling compute shader.
// Called on the primary command buffer BEFORE the render pass begins so that
// compute dispatch and memory barrier complete before secondary command buffers
// read the visibility output in render().
void M2Renderer::dispatchCullCompute(VkCommandBuffer cmd, uint32_t frameIndex, const Camera& camera) {
    if (!cullPipeline_ || instances.empty()) return;

    const uint32_t numInstances = std::min(static_cast<uint32_t>(instances.size()), MAX_CULL_INSTANCES);

    // --- Compute per-instance adaptive distances (same formula as old CPU cull) ---
    const float targetRenderDist = (instances.size() > 2000) ? 300.0f
                                 : (instances.size() > 1000) ? 500.0f
                                 : 1000.0f;
    const float shrinkRate = 0.005f;
    const float growRate   = 0.05f;
    float blendRate = (targetRenderDist < smoothedRenderDist_) ? shrinkRate : growRate;
    smoothedRenderDist_ = glm::mix(smoothedRenderDist_, targetRenderDist, blendRate);
    const float maxRenderDistance = smoothedRenderDist_;
    const float maxRenderDistanceSq = maxRenderDistance * maxRenderDistance;
    const float maxPossibleDistSq = maxRenderDistanceSq * 4.0f; // 2x safety margin

    // --- Upload frustum planes + camera (UBO, binding 0) ---
    const glm::mat4 vp = camera.getProjectionMatrix() * camera.getViewMatrix();
    Frustum frustum;
    frustum.extractFromMatrix(vp);
    const glm::vec3 camPos = camera.getPosition();

    if (cullUniformMapped_[frameIndex]) {
        auto* ubo = static_cast<CullUniformsGPU*>(cullUniformMapped_[frameIndex]);
        for (int i = 0; i < 6; i++) {
            const auto& p = frustum.getPlane(static_cast<Frustum::Side>(i));
            ubo->frustumPlanes[i] = glm::vec4(p.normal, p.distance);
        }
        ubo->cameraPos = glm::vec4(camPos, maxPossibleDistSq);
        ubo->instanceCount = numInstances;
    }

    // --- Upload per-instance cull data (SSBO, binding 1) ---
    if (cullInputMapped_[frameIndex]) {
        auto* input = static_cast<CullInstanceGPU*>(cullInputMapped_[frameIndex]);
        for (uint32_t i = 0; i < numInstances; i++) {
            const auto& inst = instances[i];
            float worldRadius = inst.cachedBoundRadius * inst.scale;
            float cullRadius = worldRadius;
            if (inst.cachedDisableAnimation) {
                cullRadius = std::max(cullRadius, 3.0f);
            }
            float effectiveMaxDistSq = maxRenderDistanceSq * std::max(1.0f, cullRadius / 12.0f);
            if (inst.cachedDisableAnimation)  effectiveMaxDistSq *= 2.6f;
            if (inst.cachedIsGroundDetail)     effectiveMaxDistSq *= 0.9f;

            float paddedRadius = std::max(cullRadius * 1.5f, cullRadius + 3.0f);

            uint32_t flags = 0;
            if (inst.cachedIsValid)          flags |= 1u;
            if (inst.cachedIsSmoke)           flags |= 2u;
            if (inst.cachedIsInvisibleTrap)   flags |= 4u;

            input[i].sphere = glm::vec4(inst.position, paddedRadius);
            input[i].effectiveMaxDistSq = effectiveMaxDistSq;
            input[i].flags = flags;
        }
    }

    // --- Dispatch compute shader ---
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cullPipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            cullPipelineLayout_, 0, 1, &cullSet_[frameIndex], 0, nullptr);

    const uint32_t groupCount = (numInstances + 63) / 64;
    vkCmdDispatch(cmd, groupCount, 1, 1);

    // --- Memory barrier: compute writes → host reads ---
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0, 1, &barrier, 0, nullptr, 0, nullptr);
}

void M2Renderer::render(VkCommandBuffer cmd, VkDescriptorSet perFrameSet, const Camera& camera) {
    if (instances.empty() || !opaquePipeline_) {
        return;
    }

    // Debug: log once when we start rendering
    static bool loggedOnce = false;
    if (!loggedOnce) {
        loggedOnce = true;
        LOG_INFO("M2 render: ", instances.size(), " instances, ", models.size(), " models");
    }

    // Reuse persistent buffers (clear instead of reallocating)
    glowSprites_.clear();

    lastDrawCallCount = 0;

    // GPU cull results — dispatchCullCompute() already updated smoothedRenderDist_.
    // Use the cached value (set by dispatchCullCompute or fallback below).
    const uint32_t frameIndex = vkCtx_->getCurrentFrame();
    const uint32_t numInstances = std::min(static_cast<uint32_t>(instances.size()), MAX_CULL_INSTANCES);
    const uint32_t* visibility = static_cast<const uint32_t*>(cullOutputMapped_[frameIndex]);
    const bool gpuCullAvailable = (cullPipeline_ != VK_NULL_HANDLE && visibility != nullptr);

    // If GPU culling was not dispatched, fallback: compute distances on CPU
    float maxRenderDistanceSq;
    if (!gpuCullAvailable) {
        const float targetRenderDist = (instances.size() > 2000) ? 300.0f
                                     : (instances.size() > 1000) ? 500.0f
                                     : 1000.0f;
        const float shrinkRate = 0.005f;
        const float growRate = 0.05f;
        float blendRate = (targetRenderDist < smoothedRenderDist_) ? shrinkRate : growRate;
        smoothedRenderDist_ = glm::mix(smoothedRenderDist_, targetRenderDist, blendRate);
        maxRenderDistanceSq = smoothedRenderDist_ * smoothedRenderDist_;
    } else {
        maxRenderDistanceSq = smoothedRenderDist_ * smoothedRenderDist_;
    }

    const float fadeStartFraction = 0.75f;
    const glm::vec3 camPos = camera.getPosition();

    // Build sorted visible instance list
    sortedVisible_.clear();
    const size_t expectedVisible = std::min(instances.size() / 3, size_t(600));
    if (sortedVisible_.capacity() < expectedVisible) {
        sortedVisible_.reserve(expectedVisible);
    }

    // GPU frustum culling — build frustum only for CPU fallback path
    Frustum frustum;
    if (!gpuCullAvailable) {
        const glm::mat4 vp = camera.getProjectionMatrix() * camera.getViewMatrix();
        frustum.extractFromMatrix(vp);
    }
    const float maxPossibleDistSq = maxRenderDistanceSq * 4.0f;

    for (uint32_t i = 0; i < numInstances; ++i) {
        const auto& instance = instances[i];

        if (gpuCullAvailable) {
            // GPU already tested flags + distance + frustum
            if (!visibility[i]) continue;
        } else {
            // CPU fallback: same culling logic as before
            if (!instance.cachedIsValid || instance.cachedIsSmoke || instance.cachedIsInvisibleTrap) continue;

            glm::vec3 toCam = instance.position - camPos;
            float distSqTest = glm::dot(toCam, toCam);
            if (distSqTest > maxPossibleDistSq) continue;

            float worldRadius = instance.cachedBoundRadius * instance.scale;
            float cullRadius = worldRadius;
            if (instance.cachedDisableAnimation) cullRadius = std::max(cullRadius, 3.0f);
            float effDistSq = maxRenderDistanceSq * std::max(1.0f, cullRadius / 12.0f);
            if (instance.cachedDisableAnimation) effDistSq *= 2.6f;
            if (instance.cachedIsGroundDetail) effDistSq *= 0.9f;
            if (distSqTest > effDistSq) continue;

            float paddedRadius = std::max(cullRadius * 1.5f, cullRadius + 3.0f);
            if (cullRadius > 0.0f && !frustum.intersectsSphere(instance.position, paddedRadius)) continue;
        }

        // Compute distSq + effectiveMaxDistSq for sorting and fade alpha (cheap for visible-only)
        glm::vec3 toCam = instance.position - camPos;
        float distSq = glm::dot(toCam, toCam);
        float worldRadius = instance.cachedBoundRadius * instance.scale;
        float cullRadius = worldRadius;
        if (instance.cachedDisableAnimation) cullRadius = std::max(cullRadius, 3.0f);
        float effectiveMaxDistSq = maxRenderDistanceSq * std::max(1.0f, cullRadius / 12.0f);
        if (instance.cachedDisableAnimation)  effectiveMaxDistSq *= 2.6f;
        if (instance.cachedIsGroundDetail)     effectiveMaxDistSq *= 0.9f;

        sortedVisible_.push_back({i, instance.modelId, distSq, effectiveMaxDistSq});
    }

    // Two-pass rendering: opaque/alpha-test first (depth write ON), then transparent/additive
    // (depth write OFF, sorted back-to-front) so transparent geometry composites correctly
    // against all opaque geometry rather than only against what was rendered before it.

    // Pass 1: sort by modelId for minimum buffer rebinds (opaque batches)
    std::sort(sortedVisible_.begin(), sortedVisible_.end(),
              [](const VisibleEntry& a, const VisibleEntry& b) { return a.modelId < b.modelId; });

    uint32_t currentModelId = UINT32_MAX;
    const M2ModelGPU* currentModel = nullptr;
    bool currentModelValid = false;

    // State tracking
    VkPipeline currentPipeline = VK_NULL_HANDLE;
    VkDescriptorSet currentMaterialSet = VK_NULL_HANDLE;

    // Push constants now carry per-batch data only; per-instance data is in instance SSBO.
    struct M2PushConstants {
        int32_t texCoordSet;        // UV set index (0 or 1)
        int32_t isFoliage;          // Foliage wind animation flag
        int32_t instanceDataOffset; // Base index into instance SSBO for this draw group
    };

    // Validate per-frame descriptor set before any Vulkan commands
    if (!perFrameSet) {
        LOG_ERROR("M2Renderer::render: perFrameSet is VK_NULL_HANDLE — skipping M2 render");
        return;
    }

    // Bind per-frame descriptor set (set 0) — shared across all draws
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);

    // Start with opaque pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, opaquePipeline_);
    currentPipeline = opaquePipeline_;

    // Bind dummy bone set (set 2) so non-animated draws have a valid binding.
    // Bind mega bone SSBO instead — all instances index into one buffer via boneBase.
    if (megaBoneSet_[frameIndex]) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 2, 1, &megaBoneSet_[frameIndex], 0, nullptr);
    } else if (dummyBoneSet_) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 2, 1, &dummyBoneSet_, 0, nullptr);
    }

    // Bind instance data SSBO (set 3) — per-instance transforms, fade, bones
    if (instanceSet_[frameIndex]) {
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                pipelineLayout_, 3, 1, &instanceSet_[frameIndex], 0, nullptr);
    }

    // Reset instance SSBO write cursor for this frame
    instanceDataCount_ = 0;
    auto* instSSBO = static_cast<M2InstanceGPU*>(instanceMapped_[frameIndex]);

    // =====================================================================
    // Opaque pass — instanced draws grouped by (modelId, LOD)
    // =====================================================================
    // sortedVisible_ is already sorted by modelId so consecutive entries share
    // the same vertex/index buffer.  Within each model group we sub-group by
    // targetLOD to guarantee all instances in one vkCmdDrawIndexed use the
    // same batch set.  Per-instance data (model matrix, fade, bones) is
    // written to the instance SSBO; the shader reads it via gl_InstanceIndex.
    {
        struct PendingInstance {
            uint32_t instanceIdx;
            float fadeAlpha;
            bool useBones;
            uint16_t targetLOD;
        };
        std::vector<PendingInstance> pending;
        pending.reserve(128);

        size_t visStart = 0;
        while (visStart < sortedVisible_.size()) {
            // Find group of consecutive entries with same modelId
            uint32_t groupModelId = sortedVisible_[visStart].modelId;
            size_t groupEnd = visStart;
            while (groupEnd < sortedVisible_.size() && sortedVisible_[groupEnd].modelId == groupModelId)
                groupEnd++;

            auto mdlIt = models.find(groupModelId);
            if (mdlIt == models.end() || !mdlIt->second.vertexBuffer || !mdlIt->second.indexBuffer) {
                visStart = groupEnd;
                continue;
            }
            const M2ModelGPU& model = mdlIt->second;

            bool modelNeedsAnimation = model.hasAnimation && !model.disableAnimation;
            const bool foliageLikeModel = model.isFoliageLike;
            const bool particleDominantEffect = model.isSpellEffect &&
                !model.particleEmitters.empty() && model.batches.size() <= 2;

            // Collect per-instance data for this model group
            pending.clear();
            for (size_t vi = visStart; vi < groupEnd; vi++) {
                const auto& entry = sortedVisible_[vi];
                if (entry.index >= instances.size()) continue;
                auto& instance = instances[entry.index];

                // Distance-based fade alpha
                float fadeFrac = model.disableAnimation ? 0.55f : fadeStartFraction;
                float fadeStartDistSq = entry.effectiveMaxDistSq * fadeFrac * fadeFrac;
                float fadeAlpha = 1.0f;
                if (entry.distSq > fadeStartDistSq) {
                    fadeAlpha = std::clamp((entry.effectiveMaxDistSq - entry.distSq) /
                                          (entry.effectiveMaxDistSq - fadeStartDistSq), 0.0f, 1.0f);
                }
                float instanceFadeAlpha = fadeAlpha;
                if (model.isGroundDetail) instanceFadeAlpha *= 0.82f;
                if (model.isInstancePortal) {
                    instanceFadeAlpha *= 0.12f;
                    if (entry.distSq < 400.0f * 400.0f) {
                        glm::vec3 center = glm::vec3(instance.modelMatrix * glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
                        GlowSprite gs;
                        gs.worldPos = center;
                        gs.color = glm::vec4(0.35f, 0.5f, 1.0f, 1.1f);
                        gs.size = instance.scale * 5.0f;
                        glowSprites_.push_back(gs);
                        GlowSprite halo = gs;
                        halo.color.a *= 0.3f;
                        halo.size *= 2.2f;
                        glowSprites_.push_back(halo);
                    }
                }

                // Bone readiness check
                if (modelNeedsAnimation && instance.boneMatrices.empty()) continue;
                bool needsBones = modelNeedsAnimation && !instance.boneMatrices.empty();
                if (needsBones && instance.megaBoneOffset == 0) continue;

                // LOD selection
                uint16_t desiredLOD = 0;
                if (entry.distSq > 150.0f * 150.0f) desiredLOD = 3;
                else if (entry.distSq > 80.0f * 80.0f) desiredLOD = 2;
                else if (entry.distSq > 40.0f * 40.0f) desiredLOD = 1;
                uint16_t targetLOD = desiredLOD;
                if (desiredLOD > 0 && !(model.availableLODs & (1u << desiredLOD))) targetLOD = 0;

                pending.push_back({entry.index, instanceFadeAlpha, needsBones, targetLOD});
            }

            if (pending.empty()) { visStart = groupEnd; continue; }

            // Sort by targetLOD so each sub-group occupies a contiguous SSBO range
            std::sort(pending.begin(), pending.end(),
                      [](const PendingInstance& a, const PendingInstance& b) { return a.targetLOD < b.targetLOD; });

            // Bind vertex/index buffers once per model group
            VkDeviceSize vbOffset = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &model.vertexBuffer, &vbOffset);
            vkCmdBindIndexBuffer(cmd, model.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

            // Write base instance data to SSBO (uvOffset=0 — overridden for tex-anim batches)
            uint32_t baseSSBOOffset = instanceDataCount_;
            for (const auto& p : pending) {
                if (instanceDataCount_ >= MAX_INSTANCE_DATA) break;
                auto& inst = instances[p.instanceIdx];
                auto& e = instSSBO[instanceDataCount_];
                e.model = inst.modelMatrix;
                e.uvOffset = glm::vec2(0.0f);
                e.fadeAlpha = p.fadeAlpha;
                e.useBones = p.useBones ? 1 : 0;
                e.boneBase = p.useBones ? static_cast<int32_t>(inst.megaBoneOffset) : 0;
                std::memset(e._pad, 0, sizeof(e._pad));
                instanceDataCount_++;
            }

            // Process LOD sub-groups within this model group
            size_t lodIdx = 0;
            while (lodIdx < pending.size()) {
                uint16_t lod = pending[lodIdx].targetLOD;
                size_t lodEnd = lodIdx + 1;
                while (lodEnd < pending.size() && pending[lodEnd].targetLOD == lod) lodEnd++;
                uint32_t groupSize = static_cast<uint32_t>(lodEnd - lodIdx);
                uint32_t groupSSBOOffset = baseSSBOOffset + static_cast<uint32_t>(lodIdx);

                for (size_t bi = 0; bi < model.batches.size(); bi++) {
                    const auto& batch = model.batches[bi];
                    if (batch.indexCount == 0) continue;
                    if (!model.isGroundDetail && batch.submeshLevel != lod) continue;
                    if (batch.batchOpacity < 0.01f) continue;

                    // Opaque gate — skip transparent batches
                    const bool rawTransparent = (batch.blendMode >= 2) || model.isSpellEffect;
                    if (rawTransparent) continue;

                    // Particle-dominant effects: emission geometry — skip opaque
                    if (particleDominantEffect && batch.blendMode <= 1) continue;

                    // Glow sprite check (per model+batch, sprites generated per instance)
                    const bool koboldFlameCard = batch.colorKeyBlack && model.isKoboldFlame;
                    const bool smallCardLikeBatch =
                        (batch.glowSize <= 1.35f) ||
                        (batch.lanternGlowHint && batch.glowSize <= 6.0f);
                    const bool batchUnlit = (batch.materialFlags & 0x01) != 0;
                    const bool shouldUseGlowSprite =
                        !koboldFlameCard &&
                        (model.isElvenLike || (model.isLanternLike && batch.lanternGlowHint)) &&
                        !model.isSpellEffect &&
                        smallCardLikeBatch &&
                        (batch.lanternGlowHint ||
                         (batch.blendMode >= 3) ||
                         (batch.colorKeyBlack && batchUnlit && batch.blendMode >= 1));
                    if (shouldUseGlowSprite) {
                        // Generate glow sprites for each instance in the group
                        for (size_t j = lodIdx; j < lodEnd; j++) {
                            auto& inst = instances[pending[j].instanceIdx];
                            float distSq = sortedVisible_[visStart].distSq; // approximate with group
                            if (distSq < 180.0f * 180.0f) {
                                glm::vec3 worldPos = glm::vec3(inst.modelMatrix * glm::vec4(batch.center, 1.0f));
                                GlowSprite gs;
                                gs.worldPos = worldPos;
                                if (batch.glowTint == 1 || model.isElvenLike)
                                    gs.color = glm::vec4(0.48f, 0.72f, 1.0f, 1.05f);
                                else if (batch.glowTint == 2)
                                    gs.color = glm::vec4(1.0f, 0.28f, 0.22f, 1.10f);
                                else
                                    gs.color = glm::vec4(1.0f, 0.82f, 0.46f, 1.15f);
                                gs.size = batch.glowSize * inst.scale * 1.45f;
                                glowSprites_.push_back(gs);
                                GlowSprite halo = gs;
                                halo.color.a *= 0.42f;
                                halo.size *= 1.8f;
                                glowSprites_.push_back(halo);
                            }
                        }
                        const bool cardLikeSkipMesh =
                            (batch.blendMode >= 3) || batch.colorKeyBlack || batchUnlit;
                        const bool lanternGlowCardSkip =
                            model.isLanternLike && batch.lanternGlowHint &&
                            smallCardLikeBatch && cardLikeSkipMesh;
                        if (lanternGlowCardSkip || (cardLikeSkipMesh && !model.isLanternLike))
                            continue;
                    }

                    // Handle texture animation: if this batch has per-instance uvOffset,
                    // write a separate SSBO range with the correct offsets.
                    bool hasBatchTexAnim = (batch.textureAnimIndex != 0xFFFF && model.hasTextureAnimation)
                                           || model.isLavaModel;
                    uint32_t drawOffset = groupSSBOOffset;
                    if (hasBatchTexAnim && instanceDataCount_ + groupSize <= MAX_INSTANCE_DATA) {
                        drawOffset = instanceDataCount_;
                        for (size_t j = lodIdx; j < lodEnd; j++) {
                            auto& inst = instances[pending[j].instanceIdx];
                            glm::vec2 uvOffset(0.0f);
                            if (batch.textureAnimIndex != 0xFFFF && model.hasTextureAnimation) {
                                uint16_t lookupIdx = batch.textureAnimIndex;
                                if (lookupIdx < model.textureTransformLookup.size()) {
                                    uint16_t transformIdx = model.textureTransformLookup[lookupIdx];
                                    if (transformIdx < model.textureTransforms.size()) {
                                        const auto& tt = model.textureTransforms[transformIdx];
                                        glm::vec3 trans = interpVec3(tt.translation,
                                            inst.currentSequenceIndex, inst.animTime,
                                            glm::vec3(0.0f), model.globalSequenceDurations);
                                        uvOffset = glm::vec2(trans.x, trans.y);
                                    }
                                }
                            }
                            if (model.isLavaModel && uvOffset == glm::vec2(0.0f)) {
                                float t = std::chrono::duration<float>(
                                    std::chrono::steady_clock::now() - kLavaAnimStart).count();
                                uvOffset = glm::vec2(t * 0.03f, -t * 0.08f);
                            }
                            // Copy base entry and override uvOffset
                            instSSBO[instanceDataCount_] = instSSBO[groupSSBOOffset + (j - lodIdx)];
                            instSSBO[instanceDataCount_].uvOffset = uvOffset;
                            instanceDataCount_++;
                        }
                    }

                    // Pipeline selection (per-model/batch, not per-instance)
                    const bool foliageCutout = foliageLikeModel && !model.isSpellEffect && batch.blendMode <= 3;
                    const bool forceCutout =
                        !model.isSpellEffect &&
                        (model.isGroundDetail || foliageCutout ||
                         batch.blendMode == 1 ||
                         (batch.blendMode >= 2 && !batch.hasAlpha) ||
                         batch.colorKeyBlack);

                    uint8_t effectiveBlendMode = batch.blendMode;
                    if (model.isSpellEffect) {
                        if (effectiveBlendMode <= 1) effectiveBlendMode = 3;
                        else if (effectiveBlendMode == 4 || effectiveBlendMode == 5) effectiveBlendMode = 3;
                    }
                    if (forceCutout) effectiveBlendMode = 1;

                    VkPipeline desiredPipeline;
                    if (forceCutout) {
                        desiredPipeline = opaquePipeline_;
                    } else {
                        switch (effectiveBlendMode) {
                            case 0: desiredPipeline = opaquePipeline_; break;
                            case 1: desiredPipeline = alphaTestPipeline_; break;
                            case 2: desiredPipeline = alphaPipeline_; break;
                            default: desiredPipeline = additivePipeline_; break;
                        }
                    }
                    if (desiredPipeline != currentPipeline) {
                        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
                        currentPipeline = desiredPipeline;
                    }

                    // Update material UBO
                    if (batch.materialUBOMapped) {
                        auto* mat = static_cast<M2MaterialUBO*>(batch.materialUBOMapped);
                        mat->interiorDarken = insideInterior ? 1.0f : 0.0f;
                        if (batch.colorKeyBlack)
                            mat->colorKeyThreshold = (effectiveBlendMode == 4 || effectiveBlendMode == 5) ? 0.7f : 0.08f;
                        if (forceCutout) {
                            mat->alphaTest = model.isGroundDetail ? 3 : (foliageCutout ? 2 : 1);
                            if (model.isGroundDetail) mat->unlit = 0;
                        }
                    }

                    // Bind material descriptor set (set 1)
                    if (!batch.materialSet) continue;
                    if (batch.materialSet != currentMaterialSet) {
                        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                                pipelineLayout_, 1, 1, &batch.materialSet, 0, nullptr);
                        currentMaterialSet = batch.materialSet;
                    }

                    // Push constants + instanced draw
                    M2PushConstants pc;
                    pc.texCoordSet = static_cast<int32_t>(batch.textureUnit);
                    pc.isFoliage = model.shadowWindFoliage ? 1 : 0;
                    pc.instanceDataOffset = static_cast<int32_t>(drawOffset);
                    vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
                    vkCmdDrawIndexed(cmd, batch.indexCount, groupSize, batch.indexStart, 0, 0);
                    lastDrawCallCount++;
                }

                lodIdx = lodEnd;
            }

            visStart = groupEnd;
        }
    }

    // =====================================================================
    // Pass 2: Transparent/additive batches — back-to-front per instance
    // =====================================================================
    // Transparent geometry must be drawn individually per instance in back-to-
    // front order for correct alpha compositing.  Each draw writes one
    // M2InstanceGPU entry and issues a single-instance indexed draw.
    std::sort(sortedVisible_.begin(), sortedVisible_.end(),
              [](const VisibleEntry& a, const VisibleEntry& b) { return a.distSq > b.distSq; });

    currentModelId = UINT32_MAX;
    currentModel = nullptr;
    currentModelValid = false;
    currentPipeline = opaquePipeline_;
    currentMaterialSet = VK_NULL_HANDLE;

    for (const auto& entry : sortedVisible_) {
        if (entry.index >= instances.size()) continue;
        auto& instance = instances[entry.index];

        // Quick skip: if model has no transparent batches at all
        if (entry.modelId != currentModelId) {
            auto mdlIt = models.find(entry.modelId);
            if (mdlIt == models.end()) continue;
            if (!mdlIt->second.hasTransparentBatches && !mdlIt->second.isSpellEffect) continue;
        }

        if (entry.modelId != currentModelId) {
            currentModelId = entry.modelId;
            currentModelValid = false;
            auto mdlIt = models.find(currentModelId);
            if (mdlIt == models.end()) continue;
            currentModel = &mdlIt->second;
            if (!currentModel->vertexBuffer || !currentModel->indexBuffer) continue;
            currentModelValid = true;
            VkDeviceSize vbOff = 0;
            vkCmdBindVertexBuffers(cmd, 0, 1, &currentModel->vertexBuffer, &vbOff);
            vkCmdBindIndexBuffer(cmd, currentModel->indexBuffer, 0, VK_INDEX_TYPE_UINT16);
        }
        if (!currentModelValid) continue;

        const M2ModelGPU& model = *currentModel;

        // Fade alpha
        float fadeAlpha = 1.0f;
        float fadeFrac = model.disableAnimation ? 0.55f : fadeStartFraction;
        float fadeStartDistSq = entry.effectiveMaxDistSq * fadeFrac * fadeFrac;
        if (entry.distSq > fadeStartDistSq) {
            fadeAlpha = std::clamp((entry.effectiveMaxDistSq - entry.distSq) /
                                  (entry.effectiveMaxDistSq - fadeStartDistSq), 0.0f, 1.0f);
        }
        float instanceFadeAlpha = fadeAlpha;
        if (model.isGroundDetail) instanceFadeAlpha *= 0.82f;
        if (model.isInstancePortal) instanceFadeAlpha *= 0.12f;

        bool modelNeedsAnimation = model.hasAnimation && !model.disableAnimation;
        if (modelNeedsAnimation && instance.boneMatrices.empty()) continue;
        bool needsBones = modelNeedsAnimation && !instance.boneMatrices.empty();
        if (needsBones && instance.megaBoneOffset == 0) continue;

        uint16_t desiredLOD = 0;
        if (entry.distSq > 150.0f * 150.0f) desiredLOD = 3;
        else if (entry.distSq > 80.0f * 80.0f) desiredLOD = 2;
        else if (entry.distSq > 40.0f * 40.0f) desiredLOD = 1;
        uint16_t targetLOD = desiredLOD;
        if (desiredLOD > 0 && !(model.availableLODs & (1u << desiredLOD))) targetLOD = 0;

        const bool particleDominantEffect = model.isSpellEffect &&
            !model.particleEmitters.empty() && model.batches.size() <= 2;

        for (const auto& batch : model.batches) {
            if (batch.indexCount == 0) continue;
            if (!model.isGroundDetail && batch.submeshLevel != targetLOD) continue;
            if (batch.batchOpacity < 0.01f) continue;

            // Pass 2 gate: only transparent/additive batches
            {
                const bool rawTransparent = (batch.blendMode >= 2) || model.isSpellEffect;
                if (!rawTransparent) continue;
            }

            // Skip glow sprites (handled in opaque pass)
            const bool batchUnlit = (batch.materialFlags & 0x01) != 0;
            const bool koboldFlameCard = batch.colorKeyBlack && model.isKoboldFlame;
            const bool smallCardLikeBatch =
                (batch.glowSize <= 1.35f) ||
                (batch.lanternGlowHint && batch.glowSize <= 6.0f);
            const bool shouldUseGlowSprite =
                !koboldFlameCard &&
                (model.isElvenLike || model.isLanternLike) &&
                !model.isSpellEffect &&
                smallCardLikeBatch &&
                (batch.lanternGlowHint || (batch.blendMode >= 3) ||
                 (batch.colorKeyBlack && batchUnlit && batch.blendMode >= 1));
            if (shouldUseGlowSprite) {
                const bool cardLikeSkipMesh = (batch.blendMode >= 3) || batch.colorKeyBlack || batchUnlit;
                const bool lanternGlowCardSkip =
                    model.isLanternLike &&
                    batch.lanternGlowHint &&
                    smallCardLikeBatch &&
                    cardLikeSkipMesh;
                if (lanternGlowCardSkip || (cardLikeSkipMesh && !model.isLanternLike))
                    continue;
            }

            if (particleDominantEffect) continue; // emission-only mesh

            // Compute UV offset for this instance + batch
            glm::vec2 uvOffset(0.0f);
            if (batch.textureAnimIndex != 0xFFFF && model.hasTextureAnimation) {
                uint16_t lookupIdx = batch.textureAnimIndex;
                if (lookupIdx < model.textureTransformLookup.size()) {
                    uint16_t transformIdx = model.textureTransformLookup[lookupIdx];
                    if (transformIdx < model.textureTransforms.size()) {
                        const auto& tt = model.textureTransforms[transformIdx];
                        glm::vec3 trans = interpVec3(tt.translation,
                            instance.currentSequenceIndex, instance.animTime,
                            glm::vec3(0.0f), model.globalSequenceDurations);
                        uvOffset = glm::vec2(trans.x, trans.y);
                    }
                }
            }
            if (model.isLavaModel && uvOffset == glm::vec2(0.0f)) {
                float t = std::chrono::duration<float>(std::chrono::steady_clock::now() - kLavaAnimStart).count();
                uvOffset = glm::vec2(t * 0.03f, -t * 0.08f);
            }

            // Write single instance entry to SSBO
            if (instanceDataCount_ >= MAX_INSTANCE_DATA) continue;
            uint32_t drawOffset = instanceDataCount_;
            auto& e = instSSBO[instanceDataCount_];
            e.model = instance.modelMatrix;
            e.uvOffset = uvOffset;
            e.fadeAlpha = instanceFadeAlpha;
            e.useBones = needsBones ? 1 : 0;
            e.boneBase = needsBones ? static_cast<int32_t>(instance.megaBoneOffset) : 0;
            std::memset(e._pad, 0, sizeof(e._pad));
            instanceDataCount_++;

            // Pipeline selection
            uint8_t effectiveBlendMode = batch.blendMode;
            if (model.isSpellEffect) {
                if (effectiveBlendMode <= 1) effectiveBlendMode = 3;
                else if (effectiveBlendMode == 4 || effectiveBlendMode == 5) effectiveBlendMode = 3;
            }

            VkPipeline desiredPipeline;
            switch (effectiveBlendMode) {
                case 2: desiredPipeline = alphaPipeline_; break;
                default: desiredPipeline = additivePipeline_; break;
            }
            if (desiredPipeline != currentPipeline) {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
                currentPipeline = desiredPipeline;
            }

            if (batch.materialUBOMapped) {
                auto* mat = static_cast<M2MaterialUBO*>(batch.materialUBOMapped);
                mat->interiorDarken = insideInterior ? 1.0f : 0.0f;
                if (batch.colorKeyBlack)
                    mat->colorKeyThreshold = (effectiveBlendMode == 4 || effectiveBlendMode == 5) ? 0.7f : 0.08f;
            }

            if (!batch.materialSet) continue;
            if (batch.materialSet != currentMaterialSet) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        pipelineLayout_, 1, 1, &batch.materialSet, 0, nullptr);
                currentMaterialSet = batch.materialSet;
            }

            // Push constants + single-instance draw
            M2PushConstants pc;
            pc.texCoordSet = static_cast<int32_t>(batch.textureUnit);
            pc.isFoliage = model.shadowWindFoliage ? 1 : 0;
            pc.instanceDataOffset = static_cast<int32_t>(drawOffset);
            vkCmdPushConstants(cmd, pipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(pc), &pc);
            vkCmdDrawIndexed(cmd, batch.indexCount, 1, batch.indexStart, 0, 0);
            lastDrawCallCount++;
        }
    }

    // Render glow sprites as billboarded additive point lights
    if (!glowSprites_.empty() && particleAdditivePipeline_ && glowVB_ && glowTexDescSet_) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, particleAdditivePipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                particlePipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                particlePipelineLayout_, 1, 1, &glowTexDescSet_, 0, nullptr);

        // Push constants for particle: tileCount(vec2) + alphaKey(int)
        struct { float tileX, tileY; int alphaKey; } particlePush = {1.0f, 1.0f, 0};
        vkCmdPushConstants(cmd, particlePipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(particlePush), &particlePush);

        // Write glow vertex data directly to mapped buffer (no temp vector)
        size_t uploadCount = std::min(glowSprites_.size(), MAX_GLOW_SPRITES);
        float* dst = static_cast<float*>(glowVBMapped_);
        for (size_t gi = 0; gi < uploadCount; gi++) {
            const auto& gs = glowSprites_[gi];
            *dst++ = gs.worldPos.x;
            *dst++ = gs.worldPos.y;
            *dst++ = gs.worldPos.z;
            *dst++ = gs.color.r;
            *dst++ = gs.color.g;
            *dst++ = gs.color.b;
            *dst++ = gs.color.a;
            *dst++ = gs.size;
            *dst++ = 0.0f;
        }

        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &glowVB_, &offset);
        vkCmdDraw(cmd, static_cast<uint32_t>(uploadCount), 1, 0, 0);
    }

}

bool M2Renderer::initializeShadow(VkRenderPass shadowRenderPass) {
    if (!vkCtx_ || shadowRenderPass == VK_NULL_HANDLE) return false;
    VkDevice device = vkCtx_->getDevice();

    // Create ShadowParams UBO
    VkBufferCreateInfo bufCI{};
    bufCI.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufCI.size = sizeof(ShadowParamsUBO);
    bufCI.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    VmaAllocationCreateInfo allocCI{};
    allocCI.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
    allocCI.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo allocInfo{};
    if (vmaCreateBuffer(vkCtx_->getAllocator(), &bufCI, &allocCI,
            &shadowParamsUBO_, &shadowParamsAlloc_, &allocInfo) != VK_SUCCESS) {
        LOG_ERROR("M2Renderer: failed to create shadow params UBO");
        return false;
    }
    ShadowParamsUBO defaultParams{};
    std::memcpy(allocInfo.pMappedData, &defaultParams, sizeof(defaultParams));

    // Create descriptor set layout: binding 0 = sampler2D, binding 1 = ShadowParams UBO
    VkDescriptorSetLayoutBinding layoutBindings[2]{};
    layoutBindings[0].binding = 0;
    layoutBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    layoutBindings[0].descriptorCount = 1;
    layoutBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    layoutBindings[1].binding = 1;
    layoutBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    layoutBindings[1].descriptorCount = 1;
    layoutBindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo layoutCI{};
    layoutCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCI.bindingCount = 2;
    layoutCI.pBindings = layoutBindings;
    if (vkCreateDescriptorSetLayout(device, &layoutCI, nullptr, &shadowParamsLayout_) != VK_SUCCESS) {
        LOG_ERROR("M2Renderer: failed to create shadow params layout");
        return false;
    }

    // Create descriptor pool
    VkDescriptorPoolSize poolSizes[2]{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[0].descriptorCount = 1;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[1].descriptorCount = 1;
    VkDescriptorPoolCreateInfo poolCI{};
    poolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCI.maxSets = 1;
    poolCI.poolSizeCount = 2;
    poolCI.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(device, &poolCI, nullptr, &shadowParamsPool_) != VK_SUCCESS) {
        LOG_ERROR("M2Renderer: failed to create shadow params pool");
        return false;
    }

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo setAlloc{};
    setAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setAlloc.descriptorPool = shadowParamsPool_;
    setAlloc.descriptorSetCount = 1;
    setAlloc.pSetLayouts = &shadowParamsLayout_;
    if (vkAllocateDescriptorSets(device, &setAlloc, &shadowParamsSet_) != VK_SUCCESS) {
        LOG_ERROR("M2Renderer: failed to allocate shadow params set");
        return false;
    }

    // Write descriptors (use white fallback for binding 0)
    VkDescriptorBufferInfo bufInfo{};
    bufInfo.buffer = shadowParamsUBO_;
    bufInfo.offset = 0;
    bufInfo.range = sizeof(ShadowParamsUBO);

    VkDescriptorImageInfo imgInfo{};
    imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imgInfo.imageView = whiteTexture_->getImageView();
    imgInfo.sampler = whiteTexture_->getSampler();

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = shadowParamsSet_;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &imgInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = shadowParamsSet_;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[1].pBufferInfo = &bufInfo;
    vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

    // Per-frame pools for foliage shadow texture sets (one per frame-in-flight, reset each frame)
    {
        VkDescriptorPoolSize texPoolSizes[2]{};
        texPoolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texPoolSizes[0].descriptorCount = 256;
        texPoolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        texPoolSizes[1].descriptorCount = 256;
        VkDescriptorPoolCreateInfo texPoolCI{};
        texPoolCI.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        texPoolCI.maxSets = 256;
        texPoolCI.poolSizeCount = 2;
        texPoolCI.pPoolSizes = texPoolSizes;
        for (uint32_t f = 0; f < kShadowTexPoolFrames; ++f) {
            if (vkCreateDescriptorPool(device, &texPoolCI, nullptr, &shadowTexPool_[f]) != VK_SUCCESS) {
                LOG_ERROR("M2Renderer: failed to create shadow texture pool ", f);
                return false;
            }
        }
    }

    // Create shadow pipeline layout: set 1 = shadowParamsLayout_, push constants = 128 bytes
    VkPushConstantRange pc{};
    pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pc.offset = 0;
    pc.size = 128;  // lightSpaceMatrix (64) + model (64)
    shadowPipelineLayout_ = createPipelineLayout(device, {shadowParamsLayout_}, {pc});
    if (!shadowPipelineLayout_) {
        LOG_ERROR("M2Renderer: failed to create shadow pipeline layout");
        return false;
    }

    // Load shadow shaders
    VkShaderModule vertShader, fragShader;
    if (!vertShader.loadFromFile(device, "assets/shaders/shadow.vert.spv")) {
        LOG_ERROR("M2Renderer: failed to load shadow vertex shader");
        return false;
    }
    if (!fragShader.loadFromFile(device, "assets/shaders/shadow.frag.spv")) {
        LOG_ERROR("M2Renderer: failed to load shadow fragment shader");
        return false;
    }

    // M2 vertex layout: 18 floats = 72 bytes stride
    // loc0=pos(off0), loc1=normal(off12), loc2=texCoord0(off24), loc5=texCoord1(off32),
    // loc3=boneWeights(off40), loc4=boneIndices(off56)
    // Shadow shader locations: 0=aPos, 1=aTexCoord, 2=aBoneWeights, 3=aBoneIndicesF
    // useBones=0 so locations 2,3 are never used
    VkVertexInputBindingDescription vertBind{};
    vertBind.binding = 0;
    vertBind.stride = 18 * sizeof(float);
    vertBind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
    std::vector<VkVertexInputAttributeDescription> vertAttrs = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT,    0},                     // aPos       -> position
        {1, 0, VK_FORMAT_R32G32_SFLOAT,       6 * sizeof(float)},     // aTexCoord  -> texCoord0
        {2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 10 * sizeof(float)},    // aBoneWeights
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 14 * sizeof(float)},    // aBoneIndicesF
    };

    shadowPipeline_ = PipelineBuilder()
        .setShaders(vertShader.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                    fragShader.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
        .setVertexInput({vertBind}, vertAttrs)
        .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
        // Foliage/leaf cards are effectively two-sided; front-face culling can
        // drop them from the shadow map depending on light/view orientation.
        .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
        .setDepthTest(true, true, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setDepthBias(0.05f, 0.20f)
        .setNoColorAttachment()
        .setLayout(shadowPipelineLayout_)
        .setRenderPass(shadowRenderPass)
        .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
        .build(device, vkCtx_->getPipelineCache());

    vertShader.destroy();
    fragShader.destroy();

    if (!shadowPipeline_) {
        LOG_ERROR("M2Renderer: failed to create shadow pipeline");
        return false;
    }
    LOG_INFO("M2Renderer shadow pipeline initialized");
    return true;
}

void M2Renderer::renderShadow(VkCommandBuffer cmd, const glm::mat4& lightSpaceMatrix, float globalTime,
                              const glm::vec3& shadowCenter, float shadowRadius) {
    if (!shadowPipeline_ || !shadowParamsSet_) return;
    if (instances.empty() || models.empty()) return;

    const float shadowRadiusSq = shadowRadius * shadowRadius;

    // Reset this frame slot's texture descriptor pool (safe: fence was waited on in beginFrame)
    const uint32_t frameIdx = vkCtx_->getCurrentFrame();
    VkDescriptorPool curShadowTexPool = shadowTexPool_[frameIdx];
    if (curShadowTexPool) {
        vkResetDescriptorPool(vkCtx_->getDevice(), curShadowTexPool, 0);
    }
    // Cache: texture imageView -> allocated descriptor set (avoids duplicates within frame)
    // Reuse persistent map — pool reset already invalidated the sets.
    shadowTexSetCache_.clear();
    auto& texSetCache = shadowTexSetCache_;

    auto getTexDescSet = [&](VkTexture* tex) -> VkDescriptorSet {
        VkImageView iv = tex->getImageView();
        auto cacheIt = texSetCache.find(iv);
        if (cacheIt != texSetCache.end()) return cacheIt->second;

        VkDescriptorSet set = VK_NULL_HANDLE;
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = curShadowTexPool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &shadowParamsLayout_;
        if (vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &set) != VK_SUCCESS) {
            return shadowParamsSet_; // fallback to white texture
        }
        VkDescriptorImageInfo imgInfo{};
        imgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imgInfo.imageView = iv;
        imgInfo.sampler = tex->getSampler();
        VkDescriptorBufferInfo bufInfo{};
        bufInfo.buffer = shadowParamsUBO_;
        bufInfo.offset = 0;
        bufInfo.range = sizeof(ShadowParamsUBO);
        VkWriteDescriptorSet writes[2]{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = set;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].pImageInfo = &imgInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = set;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].pBufferInfo = &bufInfo;
        vkUpdateDescriptorSets(vkCtx_->getDevice(), 2, writes, 0, nullptr);
        texSetCache[iv] = set;
        return set;
    };

    // Helper lambda to draw instances with a given foliageSway setting
    auto drawPass = [&](bool foliagePass) {
        ShadowParamsUBO params{};
        params.foliageSway = foliagePass ? 1 : 0;
        params.windTime = globalTime;
        params.foliageMotionDamp = 1.0f;
        // For foliage pass: enable texture+alphaTest in UBO (per-batch textures bound below)
        if (foliagePass) {
            params.useTexture = 1;
            params.alphaTest = 1;
        }

        VmaAllocationInfo allocInfo{};
        vmaGetAllocationInfo(vkCtx_->getAllocator(), shadowParamsAlloc_, &allocInfo);
        std::memcpy(allocInfo.pMappedData, &params, sizeof(params));

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipeline_);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_,
            0, 1, &shadowParamsSet_, 0, nullptr);

        uint32_t currentModelId = UINT32_MAX;
        const M2ModelGPU* currentModel = nullptr;

        for (const auto& instance : instances) {
            // Use cached flags to skip early without hash lookup
            if (!instance.cachedIsValid || instance.cachedIsSmoke || instance.cachedIsInvisibleTrap) continue;

            // Distance cull against shadow frustum
            glm::vec3 diff = instance.position - shadowCenter;
            if (glm::dot(diff, diff) > shadowRadiusSq) continue;

            if (!instance.cachedModel) continue;
            const M2ModelGPU& model = *instance.cachedModel;

            // Filter: only draw foliage models in foliage pass, non-foliage in non-foliage pass
            if (model.shadowWindFoliage != foliagePass) continue;

            // Bind vertex/index buffers when model changes
            if (instance.modelId != currentModelId) {
                currentModelId = instance.modelId;
                currentModel = &model;
                VkDeviceSize offset = 0;
                vkCmdBindVertexBuffers(cmd, 0, 1, &currentModel->vertexBuffer, &offset);
                vkCmdBindIndexBuffer(cmd, currentModel->indexBuffer, 0, VK_INDEX_TYPE_UINT16);
            }

            ShadowPush push{lightSpaceMatrix, instance.modelMatrix};
            vkCmdPushConstants(cmd, shadowPipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT,
                               0, 128, &push);

            for (const auto& batch : model.batches) {
                if (batch.submeshLevel > 0) continue;
                // For foliage: bind per-batch texture for alpha-tested shadows
                if (foliagePass && batch.hasAlpha && batch.texture) {
                    VkDescriptorSet texSet = getTexDescSet(batch.texture);
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_,
                        0, 1, &texSet, 0, nullptr);
                } else if (foliagePass) {
                    // Non-alpha batch: rebind default set (white texture, alpha test passes)
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadowPipelineLayout_,
                        0, 1, &shadowParamsSet_, 0, nullptr);
                }
                vkCmdDrawIndexed(cmd, batch.indexCount, 1, batch.indexStart, 0, 0);
            }
        }
    };

    // Pass 1: non-foliage (no wind displacement)
    drawPass(false);
    // Pass 2: foliage (wind displacement enabled, per-batch alpha-tested textures)
    drawPass(true);
}

// --- M2 Particle Emitter Helpers ---

float M2Renderer::interpFloat(const pipeline::M2AnimationTrack& track, float animTime,
                                int seqIdx, const std::vector<pipeline::M2Sequence>& /*seqs*/,
                                const std::vector<uint32_t>& globalSeqDurations) {
    if (!track.hasData()) return 0.0f;
    int si; float t;
    resolveTrackTime(track, seqIdx, animTime, globalSeqDurations, si, t);
    if (si < 0 || si >= static_cast<int>(track.sequences.size())) return 0.0f;
    const auto& keys = track.sequences[si];
    if (keys.timestamps.empty() || keys.floatValues.empty()) return 0.0f;
    if (keys.floatValues.size() == 1) return keys.floatValues[0];
    int idx = findKeyframeIndex(keys.timestamps, t);
    if (idx < 0) return 0.0f;
    size_t i0 = static_cast<size_t>(idx);
    size_t i1 = std::min(i0 + 1, keys.floatValues.size() - 1);
    if (i0 == i1) return keys.floatValues[i0];
    float t0 = static_cast<float>(keys.timestamps[i0]);
    float t1 = static_cast<float>(keys.timestamps[i1]);
    float dur = t1 - t0;
    float frac = (dur > 0.0f) ? glm::clamp((t - t0) / dur, 0.0f, 1.0f) : 0.0f;
    return glm::mix(keys.floatValues[i0], keys.floatValues[i1], frac);
}

// Interpolate an M2 FBlock (particle lifetime curve) at a given life ratio [0..1].
// FBlocks store per-lifetime keyframes for particle color, alpha, and scale.
// NOTE: interpFBlockFloat and interpFBlockVec3 share identical interpolation logic —
// if you fix a bug in one, update the other to match.
float M2Renderer::interpFBlockFloat(const pipeline::M2FBlock& fb, float lifeRatio) {
    if (fb.floatValues.empty()) return 1.0f;
    if (fb.floatValues.size() == 1 || fb.timestamps.empty()) return fb.floatValues[0];
    lifeRatio = glm::clamp(lifeRatio, 0.0f, 1.0f);
    for (size_t i = 0; i < fb.timestamps.size() - 1; i++) {
        if (lifeRatio <= fb.timestamps[i + 1]) {
            float t0 = fb.timestamps[i];
            float t1 = fb.timestamps[i + 1];
            float dur = t1 - t0;
            float frac = (dur > 0.0f) ? (lifeRatio - t0) / dur : 0.0f;
            size_t v0 = std::min(i, fb.floatValues.size() - 1);
            size_t v1 = std::min(i + 1, fb.floatValues.size() - 1);
            return glm::mix(fb.floatValues[v0], fb.floatValues[v1], frac);
        }
    }
    return fb.floatValues.back();
}

glm::vec3 M2Renderer::interpFBlockVec3(const pipeline::M2FBlock& fb, float lifeRatio) {
    if (fb.vec3Values.empty()) return glm::vec3(1.0f);
    if (fb.vec3Values.size() == 1 || fb.timestamps.empty()) return fb.vec3Values[0];
    lifeRatio = glm::clamp(lifeRatio, 0.0f, 1.0f);
    for (size_t i = 0; i < fb.timestamps.size() - 1; i++) {
        if (lifeRatio <= fb.timestamps[i + 1]) {
            float t0 = fb.timestamps[i];
            float t1 = fb.timestamps[i + 1];
            float dur = t1 - t0;
            float frac = (dur > 0.0f) ? (lifeRatio - t0) / dur : 0.0f;
            size_t v0 = std::min(i, fb.vec3Values.size() - 1);
            size_t v1 = std::min(i + 1, fb.vec3Values.size() - 1);
            return glm::mix(fb.vec3Values[v0], fb.vec3Values[v1], frac);
        }
    }
    return fb.vec3Values.back();
}

std::vector<glm::vec3> M2Renderer::getWaterVegetationPositions(const glm::vec3& camPos, float maxDist) const {
    std::vector<glm::vec3> result;
    float maxDistSq = maxDist * maxDist;
    for (const auto& inst : instances) {
        if (!inst.cachedModel || !inst.cachedModel->isWaterVegetation) continue;
        glm::vec3 diff = inst.position - camPos;
        if (glm::dot(diff, diff) <= maxDistSq) {
            result.push_back(inst.position);
        }
    }
    return result;
}

void M2Renderer::emitParticles(M2Instance& inst, const M2ModelGPU& gpu, float dt) {
    if (inst.emitterAccumulators.size() != gpu.particleEmitters.size()) {
        inst.emitterAccumulators.resize(gpu.particleEmitters.size(), 0.0f);
    }

    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    std::uniform_real_distribution<float> distN(-1.0f, 1.0f);
    std::uniform_int_distribution<int> distTile;

    for (size_t ei = 0; ei < gpu.particleEmitters.size(); ei++) {
        const auto& em = gpu.particleEmitters[ei];
        if (!em.enabled) continue;

        float rate = interpFloat(em.emissionRate, inst.animTime, inst.currentSequenceIndex,
                                  gpu.sequences, gpu.globalSequenceDurations);
        float life = interpFloat(em.lifespan, inst.animTime, inst.currentSequenceIndex,
                                  gpu.sequences, gpu.globalSequenceDurations);
        if (rate <= 0.0f || life <= 0.0f) continue;

        inst.emitterAccumulators[ei] += rate * dt;

        while (inst.emitterAccumulators[ei] >= 1.0f && inst.particles.size() < MAX_M2_PARTICLES) {
            inst.emitterAccumulators[ei] -= 1.0f;

            M2Particle p;
            p.emitterIndex = static_cast<int>(ei);
            p.life = 0.0f;
            p.maxLife = life;
            p.tileIndex = 0.0f;

            // Position: emitter position transformed by bone matrix
            glm::vec3 localPos = em.position;
            glm::mat4 boneXform = glm::mat4(1.0f);
            if (em.bone < inst.boneMatrices.size()) {
                boneXform = inst.boneMatrices[em.bone];
            }
            glm::vec3 worldPos = glm::vec3(inst.modelMatrix * boneXform * glm::vec4(localPos, 1.0f));
            p.position = worldPos;

            // Velocity: emission speed in upward direction + random spread
            float speed = interpFloat(em.emissionSpeed, inst.animTime, inst.currentSequenceIndex,
                                       gpu.sequences, gpu.globalSequenceDurations);
            float vRange = interpFloat(em.verticalRange, inst.animTime, inst.currentSequenceIndex,
                                        gpu.sequences, gpu.globalSequenceDurations);
            float hRange = interpFloat(em.horizontalRange, inst.animTime, inst.currentSequenceIndex,
                                        gpu.sequences, gpu.globalSequenceDurations);

            // Base direction: up in model space, transformed to world
            glm::vec3 dir(0.0f, 0.0f, 1.0f);
            // Add random spread
            dir.x += distN(particleRng_) * hRange;
            dir.y += distN(particleRng_) * hRange;
            dir.z += distN(particleRng_) * vRange;
            float lenSq = glm::dot(dir, dir);
            if (lenSq > 0.001f * 0.001f) dir *= glm::inversesqrt(lenSq);

            // Transform direction by bone + model orientation (rotation only)
            glm::mat3 rotMat = glm::mat3(inst.modelMatrix * boneXform);
            p.velocity = rotMat * dir * speed;

            // When emission speed is ~0 and bone animation isn't loaded (.anim files),
            // particles pile up at the same position. Give them a drift so they
            // spread outward like a mist/spray effect instead of clustering.
            if (std::abs(speed) < 0.01f) {
                if (gpu.isFireflyEffect) {
                    // Fireflies: gentle random drift in all directions
                    p.velocity = rotMat * glm::vec3(
                        distN(particleRng_) * 0.6f,
                        distN(particleRng_) * 0.6f,
                        distN(particleRng_) * 0.3f
                    );
                } else {
                    p.velocity = rotMat * glm::vec3(
                        distN(particleRng_) * 1.0f,
                        distN(particleRng_) * 1.0f,
                        -dist01(particleRng_) * 0.5f
                    );
                }
            }

            const uint32_t tilesX = std::max<uint16_t>(em.textureCols, 1);
            const uint32_t tilesY = std::max<uint16_t>(em.textureRows, 1);
            const uint32_t totalTiles = tilesX * tilesY;
            if ((em.flags & kParticleFlagTiled) && totalTiles > 1) {
                if (em.flags & kParticleFlagRandomized) {
                    distTile = std::uniform_int_distribution<int>(0, static_cast<int>(totalTiles - 1));
                    p.tileIndex = static_cast<float>(distTile(particleRng_));
                } else {
                    p.tileIndex = 0.0f;
                }
            }

            inst.particles.push_back(p);
        }
        // Cap accumulator to avoid bursts after lag
        if (inst.emitterAccumulators[ei] > 2.0f) {
            inst.emitterAccumulators[ei] = 0.0f;
        }
    }
}

void M2Renderer::updateParticles(M2Instance& inst, float dt) {
    if (!inst.cachedModel) return;
    const auto& gpu = *inst.cachedModel;

    for (size_t i = 0; i < inst.particles.size(); ) {
        auto& p = inst.particles[i];
        p.life += dt;
        if (p.life >= p.maxLife) {
            // Swap-and-pop removal
            inst.particles[i] = inst.particles.back();
            inst.particles.pop_back();
            continue;
        }
        // Apply gravity
        if (p.emitterIndex >= 0 && p.emitterIndex < static_cast<int>(gpu.particleEmitters.size())) {
            const auto& pem = gpu.particleEmitters[p.emitterIndex];
            float grav = interpFloat(pem.gravity,
                                      inst.animTime, inst.currentSequenceIndex,
                                      gpu.sequences, gpu.globalSequenceDurations);
            // When M2 gravity is 0, apply default gravity so particles arc downward.
            // Many fountain M2s rely on bone animation (.anim files) we don't load yet.
            // Firefly/ambient glow particles intentionally have zero gravity — skip fallback.
            if (grav == 0.0f && !gpu.isFireflyEffect) {
                float emSpeed = interpFloat(pem.emissionSpeed,
                                             inst.animTime, inst.currentSequenceIndex,
                                             gpu.sequences, gpu.globalSequenceDurations);
                if (std::abs(emSpeed) > 0.1f) {
                    grav = 4.0f;  // spray particles
                } else {
                    grav = 1.5f;  // mist/drift particles - gentler fall
                }
            }
            p.velocity.z -= grav * dt;
        }
        p.position += p.velocity * dt;
        i++;
    }
}

// ---------------------------------------------------------------------------
// Ribbon emitter simulation
// ---------------------------------------------------------------------------
void M2Renderer::updateRibbons(M2Instance& inst, const M2ModelGPU& gpu, float dt) {
    const auto& emitters = gpu.ribbonEmitters;
    if (emitters.empty()) return;

    // Grow per-instance state arrays if needed
    if (inst.ribbonEdges.size() != emitters.size()) {
        inst.ribbonEdges.resize(emitters.size());
    }
    if (inst.ribbonEdgeAccumulators.size() != emitters.size()) {
        inst.ribbonEdgeAccumulators.resize(emitters.size(), 0.0f);
    }

    for (size_t ri = 0; ri < emitters.size(); ri++) {
        const auto& em = emitters[ri];
        auto& edges    = inst.ribbonEdges[ri];
        auto& accum    = inst.ribbonEdgeAccumulators[ri];

        // Determine bone world position for spine
        glm::vec3 spineWorld = inst.position;
        if (em.bone < inst.boneMatrices.size()) {
            glm::vec4 local(em.position.x, em.position.y, em.position.z, 1.0f);
            spineWorld = glm::vec3(inst.modelMatrix * inst.boneMatrices[em.bone] * local);
        } else {
            glm::vec4 local(em.position.x, em.position.y, em.position.z, 1.0f);
            spineWorld = glm::vec3(inst.modelMatrix * local);
        }

        // Evaluate animated tracks (use first available sequence key, or fallback value)
        auto getFloatVal = [&](const pipeline::M2AnimationTrack& track, float fallback) -> float {
            for (const auto& seq : track.sequences) {
                if (!seq.floatValues.empty()) return seq.floatValues[0];
            }
            return fallback;
        };
        auto getVec3Val = [&](const pipeline::M2AnimationTrack& track, glm::vec3 fallback) -> glm::vec3 {
            for (const auto& seq : track.sequences) {
                if (!seq.vec3Values.empty()) return seq.vec3Values[0];
            }
            return fallback;
        };

        float visibility  = getFloatVal(em.visibilityTrack, 1.0f);
        float heightAbove = getFloatVal(em.heightAboveTrack, 0.5f);
        float heightBelow = getFloatVal(em.heightBelowTrack, 0.5f);
        glm::vec3 color   = getVec3Val(em.colorTrack, glm::vec3(1.0f));
        float alpha       = getFloatVal(em.alphaTrack, 1.0f);

        // Age existing edges and remove expired ones
        for (auto& e : edges) {
            e.age += dt;
            // Apply gravity
            if (em.gravity != 0.0f) {
                e.worldPos.z -= em.gravity * dt * dt * 0.5f;
            }
        }
        while (!edges.empty() && edges.front().age >= em.edgeLifetime) {
            edges.pop_front();
        }

        // Emit new edges based on edgesPerSecond
        if (visibility > 0.5f) {
            accum += em.edgesPerSecond * dt;
            while (accum >= 1.0f) {
                accum -= 1.0f;
                M2Instance::RibbonEdge e;
                e.worldPos    = spineWorld;
                e.color       = color;
                e.alpha       = alpha;
                e.heightAbove = heightAbove;
                e.heightBelow = heightBelow;
                e.age         = 0.0f;
                edges.push_back(e);
                // Cap trail length
                if (edges.size() > 128) edges.pop_front();
            }
        } else {
            accum = 0.0f;
        }
    }
}

// ---------------------------------------------------------------------------
// Ribbon rendering
// ---------------------------------------------------------------------------
void M2Renderer::renderM2Ribbons(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (!ribbonPipeline_ || !ribbonAdditivePipeline_ || !ribbonVB_ || !ribbonVBMapped_) return;

    // Build camera right vector for billboard orientation
    // For ribbons we orient the quad strip along the spine with screen-space up.
    // Simple approach: use world-space Z=up for the ribbon cross direction.
    const glm::vec3 upWorld(0.0f, 0.0f, 1.0f);

    float* dst     = static_cast<float*>(ribbonVBMapped_);
    size_t written = 0;

    ribbonDraws_.clear();
    auto& draws = ribbonDraws_;

    for (const auto& inst : instances) {
        if (!inst.cachedModel) continue;
        const auto& gpu = *inst.cachedModel;
        if (gpu.ribbonEmitters.empty()) continue;

        for (size_t ri = 0; ri < gpu.ribbonEmitters.size(); ri++) {
            if (ri >= inst.ribbonEdges.size()) continue;
            const auto& edges = inst.ribbonEdges[ri];
            if (edges.size() < 2) continue;

            const auto& em = gpu.ribbonEmitters[ri];

            // Select blend pipeline based on material blend mode
            bool additive = false;
            if (em.materialIndex < gpu.batches.size()) {
                additive = (gpu.batches[em.materialIndex].blendMode >= 3);
            }
            VkPipeline pipe = additive ? ribbonAdditivePipeline_ : ribbonPipeline_;

            // Descriptor set for texture
            VkDescriptorSet texSet = (ri < gpu.ribbonTexSets.size())
                                     ? gpu.ribbonTexSets[ri] : VK_NULL_HANDLE;
            if (!texSet) continue;

            uint32_t firstVert = static_cast<uint32_t>(written);

            // Emit triangle strip: 2 verts per edge (top + bottom)
            for (size_t ei = 0; ei < edges.size(); ei++) {
                if (written + 2 > MAX_RIBBON_VERTS) break;
                const auto& e = edges[ei];
                float t = (em.edgeLifetime > 0.0f)
                          ? 1.0f - (e.age / em.edgeLifetime) : 1.0f;
                float a = e.alpha * t;
                float u = static_cast<float>(ei) / static_cast<float>(edges.size() - 1);

                // Top vertex (above spine along upWorld)
                glm::vec3 top = e.worldPos + upWorld * e.heightAbove;
                dst[written * 9 + 0] = top.x;
                dst[written * 9 + 1] = top.y;
                dst[written * 9 + 2] = top.z;
                dst[written * 9 + 3] = e.color.r;
                dst[written * 9 + 4] = e.color.g;
                dst[written * 9 + 5] = e.color.b;
                dst[written * 9 + 6] = a;
                dst[written * 9 + 7] = u;
                dst[written * 9 + 8] = 0.0f; // v = top
                written++;

                // Bottom vertex (below spine)
                glm::vec3 bot = e.worldPos - upWorld * e.heightBelow;
                dst[written * 9 + 0] = bot.x;
                dst[written * 9 + 1] = bot.y;
                dst[written * 9 + 2] = bot.z;
                dst[written * 9 + 3] = e.color.r;
                dst[written * 9 + 4] = e.color.g;
                dst[written * 9 + 5] = e.color.b;
                dst[written * 9 + 6] = a;
                dst[written * 9 + 7] = u;
                dst[written * 9 + 8] = 1.0f; // v = bottom
                written++;
            }

            uint32_t vertCount = static_cast<uint32_t>(written) - firstVert;
            if (vertCount >= 4) {
                draws.push_back({texSet, pipe, firstVert, vertCount});
            } else {
                // Rollback if too few verts
                written = firstVert;
            }
        }
    }

    if (draws.empty() || written == 0) return;

    VkExtent2D ext = vkCtx_->getSwapchainExtent();
    VkViewport vp{};
    vp.x = 0; vp.y = 0;
    vp.width  = static_cast<float>(ext.width);
    vp.height = static_cast<float>(ext.height);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    VkRect2D sc{};
    sc.offset = {0, 0};
    sc.extent = ext;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    VkPipeline lastPipe = VK_NULL_HANDLE;
    for (const auto& dc : draws) {
        if (dc.pipeline != lastPipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dc.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    ribbonPipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
            lastPipe = dc.pipeline;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                ribbonPipelineLayout_, 1, 1, &dc.texSet, 0, nullptr);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &ribbonVB_, &offset);
        vkCmdDraw(cmd, dc.vertexCount, 1, dc.firstVertex, 0);
    }
}

void M2Renderer::renderM2Particles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (!particlePipeline_ || !m2ParticleVB_) return;

    // Collect all particles from all instances, grouped by texture+blend
    // Reuse persistent map — clear each group's vertex data but keep bucket structure.
    for (auto& [k, g] : particleGroups_) {
        g.vertexData.clear();
        g.preAllocSet = VK_NULL_HANDLE;
    }
    auto& groups = particleGroups_;

    size_t totalParticles = 0;

    for (auto& inst : instances) {
        if (inst.particles.empty()) continue;
        if (!inst.cachedModel) continue;
        const auto& gpu = *inst.cachedModel;

        for (const auto& p : inst.particles) {
            if (p.emitterIndex < 0 || p.emitterIndex >= static_cast<int>(gpu.particleEmitters.size())) continue;
            const auto& em = gpu.particleEmitters[p.emitterIndex];

            float lifeRatio = p.life / std::max(p.maxLife, 0.001f);
            glm::vec3 color = interpFBlockVec3(em.particleColor, lifeRatio);
            float alpha = std::min(interpFBlockFloat(em.particleAlpha, lifeRatio), 1.0f);
            float rawScale = interpFBlockFloat(em.particleScale, lifeRatio);

            if (!gpu.isSpellEffect && !gpu.isFireflyEffect) {
                color = glm::mix(color, glm::vec3(1.0f), 0.7f);
                if (rawScale > 2.0f) alpha *= 0.02f;
                if (em.blendingType == 3 || em.blendingType == 4) alpha *= 0.05f;
            }
            float scale = (gpu.isSpellEffect || gpu.isFireflyEffect) ? rawScale : std::min(rawScale, 1.5f);

            VkTexture* tex = whiteTexture_.get();
            if (p.emitterIndex < static_cast<int>(gpu.particleTextures.size())) {
                tex = gpu.particleTextures[p.emitterIndex];
            }

            uint16_t tilesX = std::max<uint16_t>(em.textureCols, 1);
            uint16_t tilesY = std::max<uint16_t>(em.textureRows, 1);
            uint32_t totalTiles = static_cast<uint32_t>(tilesX) * static_cast<uint32_t>(tilesY);
            ParticleGroupKey key{tex, em.blendingType, tilesX, tilesY};
            auto& group = groups[key];
            group.texture = tex;
            group.blendType = em.blendingType;
            group.tilesX = tilesX;
            group.tilesY = tilesY;
            // Capture pre-allocated descriptor set on first insertion for this key
            if (group.preAllocSet == VK_NULL_HANDLE &&
                p.emitterIndex < static_cast<int>(gpu.particleTexSets.size())) {
                group.preAllocSet = gpu.particleTexSets[p.emitterIndex];
            }

            group.vertexData.push_back(p.position.x);
            group.vertexData.push_back(p.position.y);
            group.vertexData.push_back(p.position.z);
            group.vertexData.push_back(color.r);
            group.vertexData.push_back(color.g);
            group.vertexData.push_back(color.b);
            group.vertexData.push_back(alpha);
            group.vertexData.push_back(scale);
            float tileIndex = p.tileIndex;
            if ((em.flags & kParticleFlagTiled) && totalTiles > 1) {
                float animSeconds = inst.animTime / 1000.0f;
                uint32_t animFrame = static_cast<uint32_t>(std::floor(animSeconds * totalTiles)) % totalTiles;
                tileIndex = p.tileIndex + static_cast<float>(animFrame);
                float tilesFloat = static_cast<float>(totalTiles);
                // Wrap tile index within totalTiles range
                while (tileIndex >= tilesFloat) {
                    tileIndex -= tilesFloat;
                }
            }
            group.vertexData.push_back(tileIndex);
            totalParticles++;
        }
    }

    if (totalParticles == 0) return;

    // Bind per-frame set (set 0) for particle pipeline
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            particlePipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);

    VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m2ParticleVB_, &vbOffset);

    VkPipeline currentPipeline = VK_NULL_HANDLE;

    for (auto& [key, group] : groups) {
        if (group.vertexData.empty()) continue;

        uint8_t blendType = group.blendType;
        VkPipeline desiredPipeline = (blendType == 3 || blendType == 4)
            ? particleAdditivePipeline_ : particlePipeline_;
        if (desiredPipeline != currentPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, desiredPipeline);
            currentPipeline = desiredPipeline;
        }

        // Use pre-allocated stable descriptor set; fall back to per-frame alloc only if unavailable
        VkDescriptorSet texSet = group.preAllocSet;
        if (texSet == VK_NULL_HANDLE) {
            // Fallback: allocate per-frame (pool exhaustion risk — should not happen in practice)
            VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = materialDescPool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &particleTexLayout_;
            if (vkAllocateDescriptorSets(vkCtx_->getDevice(), &ai, &texSet) == VK_SUCCESS) {
                VkTexture* tex = group.texture ? group.texture : whiteTexture_.get();
                VkDescriptorImageInfo imgInfo = tex->descriptorInfo();
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = texSet;
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &imgInfo;
                vkUpdateDescriptorSets(vkCtx_->getDevice(), 1, &write, 0, nullptr);
            }
        }
        if (texSet != VK_NULL_HANDLE) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    particlePipelineLayout_, 1, 1, &texSet, 0, nullptr);
        }

        // Push constants: tileCount + alphaKey
        struct { float tileX, tileY; int alphaKey; } pc = {
            static_cast<float>(group.tilesX), static_cast<float>(group.tilesY),
            (blendType == 1) ? 1 : 0
        };
        vkCmdPushConstants(cmd, particlePipelineLayout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(pc), &pc);

        // Upload and draw in chunks
        size_t count = group.vertexData.size() / 9;
        size_t offset = 0;
        while (offset < count) {
            size_t batch = std::min(count - offset, MAX_M2_PARTICLES);
            memcpy(m2ParticleVBMapped_, &group.vertexData[offset * 9], batch * 9 * sizeof(float));
            vkCmdDraw(cmd, static_cast<uint32_t>(batch), 1, 0, 0);
            offset += batch;
        }
    }
}

void M2Renderer::renderSmokeParticles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (smokeParticles.empty() || !smokePipeline_ || !smokeVB_) return;

    // Build vertex data: pos(3) + lifeRatio(1) + size(1) + isSpark(1) per particle
    size_t count = std::min(smokeParticles.size(), static_cast<size_t>(MAX_SMOKE_PARTICLES));
    float* dst = static_cast<float*>(smokeVBMapped_);
    for (size_t i = 0; i < count; i++) {
        const auto& p = smokeParticles[i];
        *dst++ = p.position.x;
        *dst++ = p.position.y;
        *dst++ = p.position.z;
        *dst++ = p.life / p.maxLife;
        *dst++ = p.size;
        *dst++ = p.isSpark;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, smokePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            smokePipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);

    // Push constant: screenHeight
    float screenHeight = static_cast<float>(vkCtx_->getSwapchainExtent().height);
    vkCmdPushConstants(cmd, smokePipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(float), &screenHeight);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &smokeVB_, &offset);
    vkCmdDraw(cmd, static_cast<uint32_t>(count), 1, 0, 0);
}

void M2Renderer::setInstancePosition(uint32_t instanceId, const glm::vec3& position) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];

    // Save old grid cells
    GridCell oldMinCell = toCell(inst.worldBoundsMin);
    GridCell oldMaxCell = toCell(inst.worldBoundsMax);

    inst.position = position;
    inst.updateModelMatrix();
    auto modelIt = models.find(inst.modelId);
    if (modelIt != models.end()) {
        glm::vec3 localMin, localMax;
        getTightCollisionBounds(modelIt->second, localMin, localMax);
        transformAABB(inst.modelMatrix, localMin, localMax, inst.worldBoundsMin, inst.worldBoundsMax);
    }

    // Incrementally update spatial grid
    GridCell newMinCell = toCell(inst.worldBoundsMin);
    GridCell newMaxCell = toCell(inst.worldBoundsMax);
    if (oldMinCell.x != newMinCell.x || oldMinCell.y != newMinCell.y || oldMinCell.z != newMinCell.z ||
        oldMaxCell.x != newMaxCell.x || oldMaxCell.y != newMaxCell.y || oldMaxCell.z != newMaxCell.z) {
        for (int z = oldMinCell.z; z <= oldMaxCell.z; z++) {
            for (int y = oldMinCell.y; y <= oldMaxCell.y; y++) {
                for (int x = oldMinCell.x; x <= oldMaxCell.x; x++) {
                    auto it = spatialGrid.find(GridCell{x, y, z});
                    if (it != spatialGrid.end()) {
                        auto& vec = it->second;
                        vec.erase(std::remove(vec.begin(), vec.end(), instanceId), vec.end());
                    }
                }
            }
        }
        for (int z = newMinCell.z; z <= newMaxCell.z; z++) {
            for (int y = newMinCell.y; y <= newMaxCell.y; y++) {
                for (int x = newMinCell.x; x <= newMaxCell.x; x++) {
                    spatialGrid[GridCell{x, y, z}].push_back(instanceId);
                }
            }
        }
    }
}

void M2Renderer::setInstanceAnimationFrozen(uint32_t instanceId, bool frozen) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    inst.animSpeed = frozen ? 0.0f : 1.0f;
    if (frozen) {
        inst.animTime = 0.0f;  // Reset to bind pose
    }
}

void M2Renderer::setInstanceAnimation(uint32_t instanceId, uint32_t animationId, bool loop) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return;
    const auto& seqs = inst.cachedModel->sequences;
    // Find the first sequence matching the requested animation ID
    for (int i = 0; i < static_cast<int>(seqs.size()); ++i) {
        if (seqs[i].id == animationId) {
            inst.currentSequenceIndex = i;
            inst.animDuration = static_cast<float>(seqs[i].duration);
            inst.animTime = 0.0f;
            inst.animSpeed = 1.0f;
            // Use playingVariation=true for one-shot (returns to idle when done)
            inst.playingVariation = !loop;
            return;
        }
    }
}

bool M2Renderer::hasAnimation(uint32_t instanceId, uint32_t animationId) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return false;
    const auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return false;
    for (const auto& seq : inst.cachedModel->sequences) {
        if (seq.id == animationId) return true;
    }
    return false;
}

float M2Renderer::getInstanceAnimDuration(uint32_t instanceId) const {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return 0.0f;
    const auto& inst = instances[idxIt->second];
    if (!inst.cachedModel) return 0.0f;
    const auto& seqs = inst.cachedModel->sequences;
    if (seqs.empty()) return 0.0f;
    int seqIdx = inst.currentSequenceIndex;
    if (seqIdx < 0 || seqIdx >= static_cast<int>(seqs.size())) seqIdx = 0;
    return seqs[seqIdx].duration; // in milliseconds
}

void M2Renderer::setInstanceTransform(uint32_t instanceId, const glm::mat4& transform) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    auto& inst = instances[idxIt->second];

    // Remove old grid cells before updating bounds
    GridCell oldMinCell = toCell(inst.worldBoundsMin);
    GridCell oldMaxCell = toCell(inst.worldBoundsMax);

    // Update model matrix directly
    inst.modelMatrix = transform;
    inst.invModelMatrix = glm::inverse(transform);

    // Extract position from transform for bounds
    inst.position = glm::vec3(transform[3]);

    // Update bounds
    auto modelIt = models.find(inst.modelId);
    if (modelIt != models.end()) {
        glm::vec3 localMin, localMax;
        getTightCollisionBounds(modelIt->second, localMin, localMax);
        transformAABB(inst.modelMatrix, localMin, localMax, inst.worldBoundsMin, inst.worldBoundsMax);
    }

    // Incrementally update spatial grid (remove old cells, add new cells)
    GridCell newMinCell = toCell(inst.worldBoundsMin);
    GridCell newMaxCell = toCell(inst.worldBoundsMax);
    if (oldMinCell.x != newMinCell.x || oldMinCell.y != newMinCell.y || oldMinCell.z != newMinCell.z ||
        oldMaxCell.x != newMaxCell.x || oldMaxCell.y != newMaxCell.y || oldMaxCell.z != newMaxCell.z) {
        // Remove from old cells
        for (int z = oldMinCell.z; z <= oldMaxCell.z; z++) {
            for (int y = oldMinCell.y; y <= oldMaxCell.y; y++) {
                for (int x = oldMinCell.x; x <= oldMaxCell.x; x++) {
                    auto it = spatialGrid.find(GridCell{x, y, z});
                    if (it != spatialGrid.end()) {
                        auto& vec = it->second;
                        vec.erase(std::remove(vec.begin(), vec.end(), instanceId), vec.end());
                    }
                }
            }
        }
        // Add to new cells
        for (int z = newMinCell.z; z <= newMaxCell.z; z++) {
            for (int y = newMinCell.y; y <= newMaxCell.y; y++) {
                for (int x = newMinCell.x; x <= newMaxCell.x; x++) {
                    spatialGrid[GridCell{x, y, z}].push_back(instanceId);
                }
            }
        }
    }
    // No spatialIndexDirty_ = true — handled incrementally
}

void M2Renderer::removeInstance(uint32_t instanceId) {
    auto idxIt = instanceIndexById.find(instanceId);
    if (idxIt == instanceIndexById.end()) return;
    size_t idx = idxIt->second;
    if (idx >= instances.size()) return;

    auto& inst = instances[idx];

    // Remove from spatial grid incrementally (same pattern as the move-update path)
    GridCell minCell = toCell(inst.worldBoundsMin);
    GridCell maxCell = toCell(inst.worldBoundsMax);
    for (int z = minCell.z; z <= maxCell.z; z++) {
        for (int y = minCell.y; y <= maxCell.y; y++) {
            for (int x = minCell.x; x <= maxCell.x; x++) {
                auto gIt = spatialGrid.find(GridCell{x, y, z});
                if (gIt != spatialGrid.end()) {
                    auto& vec = gIt->second;
                    vec.erase(std::remove(vec.begin(), vec.end(), instanceId), vec.end());
                }
            }
        }
    }

    // Remove from dedup map
    if (!inst.cachedIsGroundDetail) {
        DedupKey dk{inst.modelId,
                    static_cast<int32_t>(std::round(inst.position.x * 10.0f)),
                    static_cast<int32_t>(std::round(inst.position.y * 10.0f)),
                    static_cast<int32_t>(std::round(inst.position.z * 10.0f))};
        instanceDedupMap_.erase(dk);
    }

    destroyInstanceBones(inst, /*defer=*/true);

    // Swap-remove: move last element to the hole and pop_back to avoid O(n) shift
    instanceIndexById.erase(instanceId);
    if (idx < instances.size() - 1) {
        uint32_t movedId = instances.back().id;
        instances[idx] = std::move(instances.back());
        instances.pop_back();
        instanceIndexById[movedId] = idx;
    } else {
        instances.pop_back();
    }

    // Rebuild the lightweight auxiliary index vectors (smoke, portal, etc.)
    // These are small vectors of indices that are rebuilt cheaply.
    smokeInstanceIndices_.clear();
    portalInstanceIndices_.clear();
    animatedInstanceIndices_.clear();
    particleOnlyInstanceIndices_.clear();
    particleInstanceIndices_.clear();
    for (size_t i = 0; i < instances.size(); i++) {
        auto& ri = instances[i];
        if (ri.cachedIsSmoke) smokeInstanceIndices_.push_back(i);
        if (ri.cachedIsInstancePortal) portalInstanceIndices_.push_back(i);
        if (ri.cachedHasParticleEmitters) particleInstanceIndices_.push_back(i);
        if (ri.cachedHasAnimation && !ri.cachedDisableAnimation)
            animatedInstanceIndices_.push_back(i);
        else if (ri.cachedHasParticleEmitters)
            particleOnlyInstanceIndices_.push_back(i);
    }
}

void M2Renderer::setSkipCollision(uint32_t instanceId, bool skip) {
    for (auto& inst : instances) {
        if (inst.id == instanceId) {
            inst.skipCollision = skip;
            return;
        }
    }
}

void M2Renderer::removeInstances(const std::vector<uint32_t>& instanceIds) {
    if (instanceIds.empty() || instances.empty()) {
        return;
    }

    std::unordered_set<uint32_t> toRemove(instanceIds.begin(), instanceIds.end());
    const size_t oldSize = instances.size();
    for (auto& inst : instances) {
        if (toRemove.count(inst.id)) {
            destroyInstanceBones(inst, /*defer=*/true);
        }
    }
    instances.erase(std::remove_if(instances.begin(), instances.end(),
                   [&toRemove](const M2Instance& inst) {
                       return toRemove.find(inst.id) != toRemove.end();
                   }),
                   instances.end());

    if (instances.size() != oldSize) {
        rebuildSpatialIndex();
    }
}

void M2Renderer::clear() {
    if (vkCtx_) {
        vkDeviceWaitIdle(vkCtx_->getDevice());
        for (auto& [id, model] : models) {
            destroyModelGPU(model);
        }
        for (auto& inst : instances) {
            destroyInstanceBones(inst);
        }
        // Reset descriptor pools so new allocations succeed after reload.
        // destroyModelGPU/destroyInstanceBones don't free individual sets,
        // so the pools fill up across map changes without this reset.
        VkDevice device = vkCtx_->getDevice();
        if (materialDescPool_) {
            vkResetDescriptorPool(device, materialDescPool_, 0);
            // Re-allocate the glow texture descriptor set (pre-allocated during init,
            // invalidated by pool reset).
            if (glowTexture_ && particleTexLayout_) {
                VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                ai.descriptorPool = materialDescPool_;
                ai.descriptorSetCount = 1;
                ai.pSetLayouts = &particleTexLayout_;
                glowTexDescSet_ = VK_NULL_HANDLE;
                if (vkAllocateDescriptorSets(device, &ai, &glowTexDescSet_) == VK_SUCCESS) {
                    VkDescriptorImageInfo imgInfo = glowTexture_->descriptorInfo();
                    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                    write.dstSet = glowTexDescSet_;
                    write.dstBinding = 0;
                    write.descriptorCount = 1;
                    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    write.pImageInfo = &imgInfo;
                    vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
                }
            }
        }
        if (boneDescPool_) {
            vkResetDescriptorPool(device, boneDescPool_, 0);
            // Re-allocate the dummy bone set (invalidated by pool reset)
            dummyBoneSet_ = allocateBoneSet();
            if (dummyBoneSet_ && dummyBoneBuffer_) {
                VkDescriptorBufferInfo bufInfo{};
                bufInfo.buffer = dummyBoneBuffer_;
                bufInfo.offset = 0;
                bufInfo.range = sizeof(glm::mat4);
                VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = dummyBoneSet_;
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                write.pBufferInfo = &bufInfo;
                vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
            }
        }
    }
    models.clear();
    instances.clear();
    spatialGrid.clear();
    instanceIndexById.clear();
    instanceDedupMap_.clear();
    smokeParticles.clear();
    smokeInstanceIndices_.clear();
    portalInstanceIndices_.clear();
    animatedInstanceIndices_.clear();
    particleOnlyInstanceIndices_.clear();
    particleInstanceIndices_.clear();
    smokeEmitAccum = 0.0f;
}

void M2Renderer::setCollisionFocus(const glm::vec3& worldPos, float radius) {
    collisionFocusEnabled = (radius > 0.0f);
    collisionFocusPos = worldPos;
    collisionFocusRadius = std::max(0.0f, radius);
    collisionFocusRadiusSq = collisionFocusRadius * collisionFocusRadius;
}

void M2Renderer::clearCollisionFocus() {
    collisionFocusEnabled = false;
}

void M2Renderer::resetQueryStats() {
    queryTimeMs = 0.0;
    queryCallCount = 0;
}

M2Renderer::GridCell M2Renderer::toCell(const glm::vec3& p) const {
    return GridCell{
        static_cast<int>(std::floor(p.x / SPATIAL_CELL_SIZE)),
        static_cast<int>(std::floor(p.y / SPATIAL_CELL_SIZE)),
        static_cast<int>(std::floor(p.z / SPATIAL_CELL_SIZE))
    };
}

void M2Renderer::rebuildSpatialIndex() {
    spatialGrid.clear();
    instanceIndexById.clear();
    instanceDedupMap_.clear();
    instanceIndexById.reserve(instances.size());
    smokeInstanceIndices_.clear();
    portalInstanceIndices_.clear();
    animatedInstanceIndices_.clear();
    particleOnlyInstanceIndices_.clear();
    particleInstanceIndices_.clear();

    for (size_t i = 0; i < instances.size(); i++) {
        auto& inst = instances[i];
        instanceIndexById[inst.id] = i;

        // Re-cache model pointer (may have changed after model map modifications)
        auto mdlIt = models.find(inst.modelId);
        inst.cachedModel = (mdlIt != models.end()) ? &mdlIt->second : nullptr;

        // Rebuild dedup map (skip ground detail)
        if (!inst.cachedIsGroundDetail) {
            DedupKey dk{inst.modelId,
                        static_cast<int32_t>(std::round(inst.position.x * 10.0f)),
                        static_cast<int32_t>(std::round(inst.position.y * 10.0f)),
                        static_cast<int32_t>(std::round(inst.position.z * 10.0f))};
            instanceDedupMap_[dk] = inst.id;
        }

        if (inst.cachedIsSmoke) {
            smokeInstanceIndices_.push_back(i);
        }
        if (inst.cachedIsInstancePortal) {
            portalInstanceIndices_.push_back(i);
        }
        if (inst.cachedHasParticleEmitters) {
            particleInstanceIndices_.push_back(i);
        }
        if (inst.cachedHasAnimation && !inst.cachedDisableAnimation) {
            animatedInstanceIndices_.push_back(i);
        } else if (inst.cachedHasParticleEmitters) {
            particleOnlyInstanceIndices_.push_back(i);
        }

        GridCell minCell = toCell(inst.worldBoundsMin);
        GridCell maxCell = toCell(inst.worldBoundsMax);
        for (int z = minCell.z; z <= maxCell.z; z++) {
            for (int y = minCell.y; y <= maxCell.y; y++) {
                for (int x = minCell.x; x <= maxCell.x; x++) {
                    spatialGrid[GridCell{x, y, z}].push_back(inst.id);
                }
            }
        }
    }
    spatialIndexDirty_ = false;
}

void M2Renderer::gatherCandidates(const glm::vec3& queryMin, const glm::vec3& queryMax,
                                  std::vector<size_t>& outIndices) const {
    outIndices.clear();
    tl_m2_candidateIdScratch.clear();

    GridCell minCell = toCell(queryMin);
    GridCell maxCell = toCell(queryMax);
    for (int z = minCell.z; z <= maxCell.z; z++) {
        for (int y = minCell.y; y <= maxCell.y; y++) {
            for (int x = minCell.x; x <= maxCell.x; x++) {
                auto it = spatialGrid.find(GridCell{x, y, z});
                if (it == spatialGrid.end()) continue;
                for (uint32_t id : it->second) {
                    if (!tl_m2_candidateIdScratch.insert(id).second) continue;
                    auto idxIt = instanceIndexById.find(id);
                    if (idxIt != instanceIndexById.end()) {
                        outIndices.push_back(idxIt->second);
                    }
                }
            }
        }
    }

    // Safety fallback to preserve collision correctness if the spatial index
    // misses candidates (e.g. during streaming churn).
    if (outIndices.empty() && !instances.empty()) {
        outIndices.reserve(instances.size());
        for (size_t i = 0; i < instances.size(); i++) {
            outIndices.push_back(i);
        }
    }
}

void M2Renderer::cleanupUnusedModels() {
    // Build set of model IDs that are still referenced by instances
    std::unordered_set<uint32_t> usedModelIds;
    for (const auto& instance : instances) {
        usedModelIds.insert(instance.modelId);
    }

    const auto now = std::chrono::steady_clock::now();
    constexpr auto kGracePeriod = std::chrono::seconds(60);

    // Find models with no instances that have exceeded the grace period.
    // Models that just lost their last instance get tracked but not evicted
    // immediately — this prevents thrashing when GO models are briefly
    // instance-free between despawn and respawn cycles.
    std::vector<uint32_t> toRemove;
    for (const auto& [id, model] : models) {
        if (usedModelIds.find(id) != usedModelIds.end()) {
            // Model still in use — clear any pending unused timestamp
            modelUnusedSince_.erase(id);
            continue;
        }
        auto unusedIt = modelUnusedSince_.find(id);
        if (unusedIt == modelUnusedSince_.end()) {
            // First cycle with no instances — start the grace timer
            modelUnusedSince_[id] = now;
        } else if (now - unusedIt->second >= kGracePeriod) {
            // Grace period expired — mark for removal
            toRemove.push_back(id);
            modelUnusedSince_.erase(unusedIt);
        }
    }

    // Delete GPU resources and remove from map.
    // Wait for the GPU to finish all in-flight frames before destroying any
    // buffers — the previous frame's command buffer may still be referencing
    // vertex/index buffers that are about to be freed. Without this wait,
    // the GPU reads freed memory, which can cause VK_ERROR_DEVICE_LOST.
    if (!toRemove.empty() && vkCtx_) {
        vkDeviceWaitIdle(vkCtx_->getDevice());
    }
    for (uint32_t id : toRemove) {
        auto it = models.find(id);
        if (it != models.end()) {
            destroyModelGPU(it->second);
            models.erase(it);
        }
    }

    if (!toRemove.empty()) {
        LOG_INFO("M2 cleanup: removed ", toRemove.size(), " unused models, ", models.size(), " remaining");
    }
}

VkTexture* M2Renderer::loadTexture(const std::string& path, uint32_t texFlags) {
    constexpr uint64_t kFailedTextureRetryLookups = 512;
    auto normalizeKey = [](std::string key) {
        std::replace(key.begin(), key.end(), '/', '\\');
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return key;
    };
    std::string key = normalizeKey(path);
    const uint64_t lookupSerial = ++textureLookupSerial_;

    // Check cache
    auto it = textureCache.find(key);
    if (it != textureCache.end()) {
        it->second.lastUse = ++textureCacheCounter_;
        return it->second.texture.get();
    }
    auto failIt = failedTextureRetryAt_.find(key);
    if (failIt != failedTextureRetryAt_.end() && lookupSerial < failIt->second) {
        return whiteTexture_.get();
    }

    auto containsToken = [](const std::string& haystack, const char* token) {
        return haystack.find(token) != std::string::npos;
    };
    const bool colorKeyBlackHint =
        containsToken(key, "candle") ||
        containsToken(key, "flame") ||
        containsToken(key, "fire") ||
        containsToken(key, "torch") ||
        containsToken(key, "lamp") ||
        containsToken(key, "lantern") ||
        containsToken(key, "glow") ||
        containsToken(key, "flare") ||
        containsToken(key, "brazier") ||
        containsToken(key, "campfire") ||
        containsToken(key, "bonfire");

    // Check pre-decoded BLP cache first (populated by background worker threads)
    pipeline::BLPImage blp;
    if (predecodedBLPCache_) {
        auto pit = predecodedBLPCache_->find(key);
        if (pit != predecodedBLPCache_->end()) {
            blp = std::move(pit->second);
            predecodedBLPCache_->erase(pit);
        }
    }
    if (!blp.isValid()) {
        blp = assetManager->loadTexture(key);
    }
    if (!blp.isValid()) {
        // Cache misses briefly to avoid repeated expensive MPQ/disk probes.
        failedTextureCache_.insert(key);
        failedTextureRetryAt_[key] = lookupSerial + kFailedTextureRetryLookups;
        if (loggedTextureLoadFails_.insert(key).second) {
            LOG_WARNING("M2: Failed to load texture: ", path);
        }
        return whiteTexture_.get();
    }

    size_t base = static_cast<size_t>(blp.width) * static_cast<size_t>(blp.height) * 4ull;
    size_t approxBytes = base + (base / 3);
    if (textureCacheBytes_ + approxBytes > textureCacheBudgetBytes_) {
        static constexpr size_t kMaxFailedTextureCache = 200000;
        if (failedTextureCache_.size() < kMaxFailedTextureCache) {
            // Cache budget-rejected keys too; without this we repeatedly decode/load
            // the same textures every frame once budget is saturated.
            failedTextureCache_.insert(key);
            failedTextureRetryAt_[key] = lookupSerial + kFailedTextureRetryLookups;
        }
        if (textureBudgetRejectWarnings_ < 3) {
            LOG_WARNING("M2 texture cache full (", textureCacheBytes_ / (1024 * 1024),
                        " MB / ", textureCacheBudgetBytes_ / (1024 * 1024),
                        " MB), rejecting texture: ", path);
        }
        ++textureBudgetRejectWarnings_;
        return whiteTexture_.get();
    }

    // Track whether the texture actually uses alpha (any pixel with alpha < 255).
    bool hasAlpha = false;
    for (size_t i = 3; i < blp.data.size(); i += 4) {
        if (blp.data[i] != 255) {
            hasAlpha = true;
            break;
        }
    }

    // Create Vulkan texture
    auto tex = std::make_unique<VkTexture>();
    tex->upload(*vkCtx_, blp.data.data(), blp.width, blp.height, VK_FORMAT_R8G8B8A8_UNORM);

    // M2Texture flags: bit 0 = WrapS (1=repeat, 0=clamp), bit 1 = WrapT
    VkSamplerAddressMode wrapS = (texFlags & 0x1) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSamplerAddressMode wrapT = (texFlags & 0x2) ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    tex->createSampler(vkCtx_->getDevice(), VK_FILTER_LINEAR, wrapS, wrapT);

    VkTexture* texPtr = tex.get();

    TextureCacheEntry e;
    e.texture = std::move(tex);
    e.approxBytes = approxBytes;
    e.hasAlpha = hasAlpha;
    e.colorKeyBlack = colorKeyBlackHint;
    e.lastUse = ++textureCacheCounter_;
    textureCacheBytes_ += e.approxBytes;
    textureCache[key] = std::move(e);
    failedTextureCache_.erase(key);
    failedTextureRetryAt_.erase(key);
    texturePropsByPtr_[texPtr] = {hasAlpha, colorKeyBlackHint};
    LOG_DEBUG("M2: Loaded texture: ", path, " (", blp.width, "x", blp.height, ")");

    return texPtr;
}

uint32_t M2Renderer::getTotalTriangleCount() const {
    uint32_t total = 0;
    for (const auto& instance : instances) {
        if (instance.cachedModel) {
            total += instance.cachedModel->indexCount / 3;
        }
    }
    return total;
}

std::optional<float> M2Renderer::getFloorHeight(float glX, float glY, float glZ, float* outNormalZ) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    std::optional<float> bestFloor;
    float bestNormalZ = 1.0f;  // Default to flat

    glm::vec3 queryMin(glX - 2.0f, glY - 2.0f, glZ - 6.0f);
    glm::vec3 queryMax(glX + 2.0f, glY + 2.0f, glZ + 8.0f);
    gatherCandidates(queryMin, queryMax, tl_m2_candidateScratch);

    for (size_t idx : tl_m2_candidateScratch) {
        const auto& instance = instances[idx];
        if (collisionFocusEnabled &&
            pointAABBDistanceSq(collisionFocusPos, instance.worldBoundsMin, instance.worldBoundsMax) > collisionFocusRadiusSq) {
            continue;
        }

        if (!instance.cachedModel) continue;
        if (instance.scale <= 0.001f) continue;

        const M2ModelGPU& model = *instance.cachedModel;
        if (model.collisionNoBlock || model.isInvisibleTrap || model.isSpellEffect) continue;
        if (instance.skipCollision) continue;

        // --- Mesh-based floor: vertical ray vs collision triangles ---
        // Does NOT skip the AABB path — both contribute and highest wins.
        if (model.collision.valid()) {
            glm::vec3 localPos = glm::vec3(instance.invModelMatrix * glm::vec4(glX, glY, glZ, 1.0f));

            model.collision.getFloorTrisInRange(
                localPos.x - 1.0f, localPos.y - 1.0f,
                localPos.x + 1.0f, localPos.y + 1.0f,
                tl_m2_collisionTriScratch);

            glm::vec3 rayOrigin(localPos.x, localPos.y, localPos.z + 5.0f);
            glm::vec3 rayDir(0.0f, 0.0f, -1.0f);
            float bestHitZ = -std::numeric_limits<float>::max();
            bool hitAny = false;

            for (uint32_t ti : tl_m2_collisionTriScratch) {
                if (ti >= model.collision.triCount) continue;
                if (model.collision.triBounds[ti].maxZ < localPos.z - 10.0f ||
                    model.collision.triBounds[ti].minZ > localPos.z + 5.0f) continue;

                const auto& verts = model.collision.vertices;
                const auto& idx   = model.collision.indices;
                const auto& v0 = verts[idx[ti * 3]];
                const auto& v1 = verts[idx[ti * 3 + 1]];
                const auto& v2 = verts[idx[ti * 3 + 2]];

                // Two-sided: try both windings
                float tHit = rayTriangleIntersect(rayOrigin, rayDir, v0, v1, v2);
                if (tHit < 0.0f)
                    tHit = rayTriangleIntersect(rayOrigin, rayDir, v0, v2, v1);
                if (tHit < 0.0f) continue;

                float hitZ = rayOrigin.z - tHit;

                // Walkable normal check (world space)
                glm::vec3 worldN(0.0f, 0.0f, 1.0f);  // Default to flat
                glm::vec3 localN = glm::cross(v1 - v0, v2 - v0);
                float nLen = glm::length(localN);
                if (nLen > 0.001f) {
                    localN /= nLen;
                    if (localN.z < 0.0f) localN = -localN;
                    worldN = glm::normalize(
                        glm::vec3(instance.modelMatrix * glm::vec4(localN, 0.0f)));
                    if (std::abs(worldN.z) < 0.35f) continue; // too steep (~70° max slope)
                }

                if (hitZ <= localPos.z + 3.0f && hitZ > bestHitZ) {
                    bestHitZ = hitZ;
                    hitAny = true;
                    bestNormalZ = std::abs(worldN.z);  // Store normal for output
                }
            }

            if (hitAny) {
                glm::vec3 localHit(localPos.x, localPos.y, bestHitZ);
                glm::vec3 worldHit = glm::vec3(instance.modelMatrix * glm::vec4(localHit, 1.0f));
                if (worldHit.z <= glZ + 3.0f && (!bestFloor || worldHit.z > *bestFloor)) {
                    bestFloor = worldHit.z;
                }
            }
            // Fall through to AABB floor — both contribute, highest wins
        }

        float zMargin = model.collisionBridge ? 25.0f : 2.0f;
        if (glX < instance.worldBoundsMin.x || glX > instance.worldBoundsMax.x ||
            glY < instance.worldBoundsMin.y || glY > instance.worldBoundsMax.y ||
            glZ < instance.worldBoundsMin.z - zMargin || glZ > instance.worldBoundsMax.z + zMargin) {
            continue;
        }
        glm::vec3 localMin, localMax;
        getTightCollisionBounds(model, localMin, localMax);

        glm::vec3 localPos = glm::vec3(instance.invModelMatrix * glm::vec4(glX, glY, glZ, 1.0f));

        // Must be within doodad footprint in local XY.
        // Stepped low platforms get a small pad so walk-up snapping catches edges.
        float footprintPad = 0.0f;
        if (model.collisionSteppedLowPlatform) {
            footprintPad = model.collisionPlanter ? 0.22f : 0.16f;
            if (model.collisionBridge) {
                footprintPad = 0.35f;
            }
        }
        if (localPos.x < localMin.x - footprintPad || localPos.x > localMax.x + footprintPad ||
            localPos.y < localMin.y - footprintPad || localPos.y > localMax.y + footprintPad) {
            continue;
        }

        // Construct "top" point at queried XY in local space, then transform back.
        float localTopZ = getEffectiveCollisionTopLocal(model, localPos, localMin, localMax);
        glm::vec3 localTop(localPos.x, localPos.y, localTopZ);
        glm::vec3 worldTop = glm::vec3(instance.modelMatrix * glm::vec4(localTop, 1.0f));

        // Reachability filter: allow a bit more climb for stepped low platforms.
        float maxStepUp = 1.0f;
        if (model.collisionStatue) {
            maxStepUp = 2.5f;
        } else if (model.collisionSmallSolidProp) {
            maxStepUp = 2.0f;
        } else if (model.collisionSteppedFountain) {
            maxStepUp = 2.5f;
        } else if (model.collisionSteppedLowPlatform) {
            maxStepUp = model.collisionPlanter ? 3.0f : 2.4f;
            if (model.collisionBridge) {
                maxStepUp = 25.0f;
            }
        }
        if (worldTop.z > glZ + maxStepUp) continue;

        if (!bestFloor || worldTop.z > *bestFloor) {
            bestFloor = worldTop.z;
        }
    }

    // Output surface normal if requested
    if (outNormalZ) {
        *outNormalZ = bestNormalZ;
    }

    return bestFloor;
}

bool M2Renderer::checkCollision(const glm::vec3& from, const glm::vec3& to,
                                 glm::vec3& adjustedPos, float playerRadius) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    adjustedPos = to;
    bool collided = false;

    glm::vec3 queryMin = glm::min(from, to) - glm::vec3(7.0f, 7.0f, 5.0f);
    glm::vec3 queryMax = glm::max(from, to) + glm::vec3(7.0f, 7.0f, 5.0f);
    gatherCandidates(queryMin, queryMax, tl_m2_candidateScratch);

    // Check against all M2 instances in local space (rotation-aware).
    for (size_t idx : tl_m2_candidateScratch) {
        const auto& instance = instances[idx];
        if (collisionFocusEnabled &&
            pointAABBDistanceSq(collisionFocusPos, instance.worldBoundsMin, instance.worldBoundsMax) > collisionFocusRadiusSq) {
            continue;
        }

        const float broadMargin = playerRadius + 1.0f;
        if (from.x < instance.worldBoundsMin.x - broadMargin && adjustedPos.x < instance.worldBoundsMin.x - broadMargin) continue;
        if (from.x > instance.worldBoundsMax.x + broadMargin && adjustedPos.x > instance.worldBoundsMax.x + broadMargin) continue;
        if (from.y < instance.worldBoundsMin.y - broadMargin && adjustedPos.y < instance.worldBoundsMin.y - broadMargin) continue;
        if (from.y > instance.worldBoundsMax.y + broadMargin && adjustedPos.y > instance.worldBoundsMax.y + broadMargin) continue;
        if (from.z > instance.worldBoundsMax.z + 2.5f && adjustedPos.z > instance.worldBoundsMax.z + 2.5f) continue;
        if (from.z + 2.5f < instance.worldBoundsMin.z && adjustedPos.z + 2.5f < instance.worldBoundsMin.z) continue;

        if (!instance.cachedModel) continue;

        const M2ModelGPU& model = *instance.cachedModel;
        if (model.collisionNoBlock || model.isInvisibleTrap || model.isSpellEffect) continue;
        if (instance.skipCollision) continue;
        if (instance.scale <= 0.001f) continue;

        // --- Mesh-based wall collision: closest-point push ---
        if (model.collision.valid()) {
            glm::vec3 localFrom = glm::vec3(instance.invModelMatrix * glm::vec4(from, 1.0f));
            glm::vec3 localPos  = glm::vec3(instance.invModelMatrix * glm::vec4(adjustedPos, 1.0f));
            float localRadius = playerRadius / instance.scale;

            model.collision.getWallTrisInRange(
                std::min(localFrom.x, localPos.x) - localRadius - 1.0f,
                std::min(localFrom.y, localPos.y) - localRadius - 1.0f,
                std::max(localFrom.x, localPos.x) + localRadius + 1.0f,
                std::max(localFrom.y, localPos.y) + localRadius + 1.0f,
                tl_m2_collisionTriScratch);

            constexpr float PLAYER_HEIGHT = 2.0f;
            constexpr float MAX_TOTAL_PUSH = 0.02f; // Cap total push per instance
            bool pushed = false;
            float totalPushX = 0.0f, totalPushY = 0.0f;

            for (uint32_t ti : tl_m2_collisionTriScratch) {
                if (ti >= model.collision.triCount) continue;
                if (localPos.z + PLAYER_HEIGHT < model.collision.triBounds[ti].minZ ||
                    localPos.z > model.collision.triBounds[ti].maxZ) continue;

                // Step-up: only skip wall when player is rising (jumping over it)
                constexpr float MAX_STEP_UP = 1.2f;
                bool rising = (localPos.z > localFrom.z + 0.05f);
                if (rising && localPos.z + MAX_STEP_UP >= model.collision.triBounds[ti].maxZ) continue;

                // Early out if we already pushed enough this instance
                float totalPushSoFar = std::sqrt(totalPushX * totalPushX + totalPushY * totalPushY);
                if (totalPushSoFar >= MAX_TOTAL_PUSH) break;

                const auto& verts = model.collision.vertices;
                const auto& idx   = model.collision.indices;
                const auto& v0 = verts[idx[ti * 3]];
                const auto& v1 = verts[idx[ti * 3 + 1]];
                const auto& v2 = verts[idx[ti * 3 + 2]];

                glm::vec3 closest = closestPointOnTriangle(localPos, v0, v1, v2);
                glm::vec3 diff = localPos - closest;
                float distXY = std::sqrt(diff.x * diff.x + diff.y * diff.y);

                if (distXY < localRadius && distXY > 1e-4f) {
                    // Gentle push — very small fraction of penetration
                    float penetration = localRadius - distXY;
                    float pushDist = std::clamp(penetration * 0.08f, 0.001f, 0.015f);
                    float dx = (diff.x / distXY) * pushDist;
                    float dy = (diff.y / distXY) * pushDist;
                    localPos.x += dx;
                    localPos.y += dy;
                    totalPushX += dx;
                    totalPushY += dy;
                    pushed = true;
                } else if (distXY < 1e-4f) {
                    // On the plane — soft push along triangle normal XY
                    glm::vec3 n = glm::cross(v1 - v0, v2 - v0);
                    float nxyLen = std::sqrt(n.x * n.x + n.y * n.y);
                    if (nxyLen > 1e-4f) {
                        float pushDist = std::min(localRadius, 0.015f);
                        float dx = (n.x / nxyLen) * pushDist;
                        float dy = (n.y / nxyLen) * pushDist;
                        localPos.x += dx;
                        localPos.y += dy;
                        totalPushX += dx;
                        totalPushY += dy;
                        pushed = true;
                    }
                }
            }

            if (pushed) {
                glm::vec3 worldPos = glm::vec3(instance.modelMatrix * glm::vec4(localPos, 1.0f));
                adjustedPos.x = worldPos.x;
                adjustedPos.y = worldPos.y;
                collided = true;
            }
            continue;
        }

        glm::vec3 localFrom = glm::vec3(instance.invModelMatrix * glm::vec4(from, 1.0f));
        glm::vec3 localPos = glm::vec3(instance.invModelMatrix * glm::vec4(adjustedPos, 1.0f));
        float radiusScale = model.collisionNarrowVerticalProp ? 0.45f : 1.0f;
        float localRadius = (playerRadius * radiusScale) / instance.scale;

        glm::vec3 rawMin, rawMax;
        getTightCollisionBounds(model, rawMin, rawMax);
        glm::vec3 localMin = rawMin - glm::vec3(localRadius);
        glm::vec3 localMax = rawMax + glm::vec3(localRadius);
        float effectiveTop = getEffectiveCollisionTopLocal(model, localPos, rawMin, rawMax) + localRadius;
        glm::vec2 localCenter((localMin.x + localMax.x) * 0.5f, (localMin.y + localMax.y) * 0.5f);
        float fromR = glm::length(glm::vec2(localFrom.x, localFrom.y) - localCenter);
        float toR = glm::length(glm::vec2(localPos.x, localPos.y) - localCenter);

        // Feet-based vertical overlap test: ignore objects fully above/below us.
        constexpr float PLAYER_HEIGHT = 2.0f;
        if (localPos.z + PLAYER_HEIGHT < localMin.z || localPos.z > effectiveTop) {
            continue;
        }

        bool fromInsideXY =
            (localFrom.x >= localMin.x && localFrom.x <= localMax.x &&
             localFrom.y >= localMin.y && localFrom.y <= localMax.y);
        bool fromInsideZ = (localFrom.z + PLAYER_HEIGHT >= localMin.z && localFrom.z <= effectiveTop);
        bool escapingOverlap = (fromInsideXY && fromInsideZ && (toR > fromR + 1e-4f));
        bool allowEscapeRelax = escapingOverlap && !model.collisionSmallSolidProp;

        // Swept hard clamp for taller blockers only.
        // Low/stepable objects should be climbable and not "shove" the player off.
        float maxStepUp = 1.20f;
        if (model.collisionStatue) {
            maxStepUp = 2.5f;
        } else if (model.collisionSmallSolidProp) {
            // Keep box/crate-class props hard-solid to prevent phase-through.
            maxStepUp = 0.75f;
        } else if (model.collisionSteppedFountain) {
            maxStepUp = 2.5f;
        } else if (model.collisionSteppedLowPlatform) {
            maxStepUp = model.collisionPlanter ? 2.8f : 2.4f;
            if (model.collisionBridge) {
                maxStepUp = 25.0f;
            }
        }
        bool stepableLowObject = (effectiveTop <= localFrom.z + maxStepUp);
        bool climbingAttempt = (localPos.z > localFrom.z + 0.18f);
        bool nearTop = (localFrom.z >= effectiveTop - 0.30f);
        float climbAllowance = model.collisionPlanter ? 0.95f : 0.60f;
        if (model.collisionSteppedLowPlatform && !model.collisionPlanter) {
            // Let low curb/planter blocks be stepable without sticky side shoves.
            climbAllowance = 1.00f;
        }
        if (model.collisionBridge) {
            climbAllowance = 3.0f;
        }
        if (model.collisionSmallSolidProp) {
            climbAllowance = 1.05f;
        }
        bool climbingTowardTop = climbingAttempt && (localFrom.z + climbAllowance >= effectiveTop);
        bool forceHardLateral =
            model.collisionSmallSolidProp &&
            !nearTop && !climbingTowardTop;
        if ((!stepableLowObject || forceHardLateral) && !allowEscapeRelax) {
            float tEnter = 0.0f;
            glm::vec3 sweepMax = localMax;
            sweepMax.z = std::min(sweepMax.z, effectiveTop);
            if (segmentIntersectsAABB(localFrom, localPos, localMin, sweepMax, tEnter)) {
                float tSafe = std::clamp(tEnter - 0.03f, 0.0f, 1.0f);
                glm::vec3 localSafe = localFrom + (localPos - localFrom) * tSafe;
                glm::vec3 worldSafe = glm::vec3(instance.modelMatrix * glm::vec4(localSafe, 1.0f));
                adjustedPos.x = worldSafe.x;
                adjustedPos.y = worldSafe.y;
                collided = true;
                continue;
            }
        }

        if (localPos.x < localMin.x || localPos.x > localMax.x ||
            localPos.y < localMin.y || localPos.y > localMax.y) {
            continue;
        }

        float pushLeft  = localPos.x - localMin.x;
        float pushRight = localMax.x - localPos.x;
        float pushBack  = localPos.y - localMin.y;
        float pushFront = localMax.y - localPos.y;

        float minPush = std::min({pushLeft, pushRight, pushBack, pushFront});
        if (allowEscapeRelax) {
            continue;
        }
        if (stepableLowObject && localFrom.z >= effectiveTop - 0.35f) {
            // Already on/near top surface: don't apply lateral push that ejects
            // the player from the object (carpets, platforms, etc).
            continue;
        }
        // Gentle fallback push for overlapping cases.
        float pushAmount;
        if (model.collisionNarrowVerticalProp) {
            pushAmount = std::clamp(minPush * 0.10f, 0.001f, 0.010f);
        } else if (model.collisionSteppedLowPlatform) {
            if (model.collisionPlanter && stepableLowObject) {
                pushAmount = std::clamp(minPush * 0.06f, 0.001f, 0.006f);
            } else {
            pushAmount = std::clamp(minPush * 0.12f, 0.003f, 0.012f);
            }
        } else if (stepableLowObject) {
            pushAmount = std::clamp(minPush * 0.12f, 0.002f, 0.015f);
        } else {
            pushAmount = std::clamp(minPush * 0.28f, 0.010f, 0.045f);
        }
        glm::vec3 localPush(0.0f);
        if (minPush == pushLeft) {
            localPush.x = -pushAmount;
        } else if (minPush == pushRight) {
            localPush.x = pushAmount;
        } else if (minPush == pushBack) {
            localPush.y = -pushAmount;
        } else {
            localPush.y = pushAmount;
        }

        glm::vec3 worldPush = glm::vec3(instance.modelMatrix * glm::vec4(localPush, 0.0f));
        adjustedPos.x += worldPush.x;
        adjustedPos.y += worldPush.y;
        collided = true;
    }

    return collided;
}

float M2Renderer::raycastBoundingBoxes(const glm::vec3& origin, const glm::vec3& direction, float maxDistance) const {
    QueryTimer timer(&queryTimeMs, &queryCallCount);
    float closestHit = maxDistance;

    glm::vec3 rayEnd = origin + direction * maxDistance;
    glm::vec3 queryMin = glm::min(origin, rayEnd) - glm::vec3(1.0f);
    glm::vec3 queryMax = glm::max(origin, rayEnd) + glm::vec3(1.0f);
    gatherCandidates(queryMin, queryMax, tl_m2_candidateScratch);

    for (size_t idx : tl_m2_candidateScratch) {
        const auto& instance = instances[idx];
        if (collisionFocusEnabled &&
            pointAABBDistanceSq(collisionFocusPos, instance.worldBoundsMin, instance.worldBoundsMax) > collisionFocusRadiusSq) {
            continue;
        }

        // Cheap world-space broad-phase.
        float tEnter = 0.0f;
        glm::vec3 worldMin = instance.worldBoundsMin - glm::vec3(0.35f);
        glm::vec3 worldMax = instance.worldBoundsMax + glm::vec3(0.35f);
        if (!segmentIntersectsAABB(origin, origin + direction * maxDistance, worldMin, worldMax, tEnter)) {
            continue;
        }

        if (!instance.cachedModel) continue;

        const M2ModelGPU& model = *instance.cachedModel;
        if (model.collisionNoBlock || model.isInvisibleTrap || model.isSpellEffect) continue;
        glm::vec3 localMin, localMax;
        getTightCollisionBounds(model, localMin, localMax);
        // Skip tiny doodads for camera occlusion; they cause jitter and false hits.
        glm::vec3 extents = (localMax - localMin) * instance.scale;
        if (glm::dot(extents, extents) < 0.5625f) continue;

        glm::vec3 localOrigin = glm::vec3(instance.invModelMatrix * glm::vec4(origin, 1.0f));
        glm::vec3 localDir = glm::normalize(glm::vec3(instance.invModelMatrix * glm::vec4(direction, 0.0f)));
        if (!std::isfinite(localDir.x) || !std::isfinite(localDir.y) || !std::isfinite(localDir.z)) {
            continue;
        }

        // Local-space AABB slab intersection.
        glm::vec3 invDir = 1.0f / localDir;
        glm::vec3 tMin = (localMin - localOrigin) * invDir;
        glm::vec3 tMax = (localMax - localOrigin) * invDir;
        glm::vec3 t1 = glm::min(tMin, tMax);
        glm::vec3 t2 = glm::max(tMin, tMax);

        float tNear = std::max({t1.x, t1.y, t1.z});
        float tFar = std::min({t2.x, t2.y, t2.z});
        if (tNear > tFar || tFar <= 0.0f) continue;

        float tHit = tNear > 0.0f ? tNear : tFar;
        glm::vec3 localHit = localOrigin + localDir * tHit;
        glm::vec3 worldHit = glm::vec3(instance.modelMatrix * glm::vec4(localHit, 1.0f));
        float worldDist = glm::length(worldHit - origin);
        if (worldDist > 0.0f && worldDist < closestHit) {
            closestHit = worldDist;
        }
    }

    return closestHit;
}

void M2Renderer::recreatePipelines() {
    if (!vkCtx_) return;
    VkDevice device = vkCtx_->getDevice();

    // Destroy old main-pass pipelines (NOT shadow, NOT pipeline layouts)
    if (opaquePipeline_)            { vkDestroyPipeline(device, opaquePipeline_, nullptr); opaquePipeline_ = VK_NULL_HANDLE; }
    if (alphaTestPipeline_)         { vkDestroyPipeline(device, alphaTestPipeline_, nullptr); alphaTestPipeline_ = VK_NULL_HANDLE; }
    if (alphaPipeline_)             { vkDestroyPipeline(device, alphaPipeline_, nullptr); alphaPipeline_ = VK_NULL_HANDLE; }
    if (additivePipeline_)          { vkDestroyPipeline(device, additivePipeline_, nullptr); additivePipeline_ = VK_NULL_HANDLE; }
    if (particlePipeline_)          { vkDestroyPipeline(device, particlePipeline_, nullptr); particlePipeline_ = VK_NULL_HANDLE; }
    if (particleAdditivePipeline_)  { vkDestroyPipeline(device, particleAdditivePipeline_, nullptr); particleAdditivePipeline_ = VK_NULL_HANDLE; }
    if (smokePipeline_)             { vkDestroyPipeline(device, smokePipeline_, nullptr); smokePipeline_ = VK_NULL_HANDLE; }
    if (ribbonPipeline_)            { vkDestroyPipeline(device, ribbonPipeline_, nullptr); ribbonPipeline_ = VK_NULL_HANDLE; }
    if (ribbonAdditivePipeline_)    { vkDestroyPipeline(device, ribbonAdditivePipeline_, nullptr); ribbonAdditivePipeline_ = VK_NULL_HANDLE; }

    // --- Load shaders ---
    rendering::VkShaderModule m2Vert, m2Frag;
    rendering::VkShaderModule particleVert, particleFrag;
    rendering::VkShaderModule smokeVert, smokeFrag;

    (void)m2Vert.loadFromFile(device, "assets/shaders/m2.vert.spv");
    (void)m2Frag.loadFromFile(device, "assets/shaders/m2.frag.spv");
    (void)particleVert.loadFromFile(device, "assets/shaders/m2_particle.vert.spv");
    (void)particleFrag.loadFromFile(device, "assets/shaders/m2_particle.frag.spv");
    (void)smokeVert.loadFromFile(device, "assets/shaders/m2_smoke.vert.spv");
    (void)smokeFrag.loadFromFile(device, "assets/shaders/m2_smoke.frag.spv");

    if (!m2Vert.isValid() || !m2Frag.isValid()) {
        LOG_ERROR("M2Renderer::recreatePipelines: missing required shaders");
        return;
    }

    VkRenderPass mainPass = vkCtx_->getImGuiRenderPass();

    // --- M2 model vertex input ---
    VkVertexInputBindingDescription m2Binding{};
    m2Binding.binding = 0;
    m2Binding.stride = 18 * sizeof(float);
    m2Binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::vector<VkVertexInputAttributeDescription> m2Attrs = {
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},                     // position
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 3 * sizeof(float)},     // normal
        {2, 0, VK_FORMAT_R32G32_SFLOAT, 6 * sizeof(float)},        // texCoord0
        {5, 0, VK_FORMAT_R32G32_SFLOAT, 8 * sizeof(float)},        // texCoord1
        {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 10 * sizeof(float)}, // boneWeights
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 14 * sizeof(float)}, // boneIndices (float)
    };

    // Pipeline derivatives — opaque is the base, others derive from it for shared state optimization
    auto buildM2Pipeline = [&](VkPipelineColorBlendAttachmentState blendState, bool depthWrite,
                               VkPipelineCreateFlags flags = 0, VkPipeline basePipeline = VK_NULL_HANDLE) -> VkPipeline {
        return PipelineBuilder()
            .setShaders(m2Vert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                        m2Frag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({m2Binding}, m2Attrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setDepthTest(true, depthWrite, VK_COMPARE_OP_LESS_OR_EQUAL)
            .setColorBlendAttachment(blendState)
            .setMultisample(vkCtx_->getMsaaSamples())
            .setLayout(pipelineLayout_)
            .setRenderPass(mainPass)
            .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
            .setFlags(flags)
            .setBasePipeline(basePipeline)
            .build(device, vkCtx_->getPipelineCache());
    };

    opaquePipeline_ = buildM2Pipeline(PipelineBuilder::blendDisabled(), true,
                                      VK_PIPELINE_CREATE_ALLOW_DERIVATIVES_BIT);
    alphaTestPipeline_ = buildM2Pipeline(PipelineBuilder::blendAlpha(), true,
                                         VK_PIPELINE_CREATE_DERIVATIVE_BIT, opaquePipeline_);
    alphaPipeline_ = buildM2Pipeline(PipelineBuilder::blendAlpha(), false,
                                     VK_PIPELINE_CREATE_DERIVATIVE_BIT, opaquePipeline_);
    additivePipeline_ = buildM2Pipeline(PipelineBuilder::blendAdditive(), false,
                                        VK_PIPELINE_CREATE_DERIVATIVE_BIT, opaquePipeline_);

    // --- Particle pipelines ---
    if (particleVert.isValid() && particleFrag.isValid()) {
        VkVertexInputBindingDescription pBind{};
        pBind.binding = 0;
        pBind.stride = 9 * sizeof(float); // pos3 + color4 + size1 + tile1
        pBind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::vector<VkVertexInputAttributeDescription> pAttrs = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},                    // position
            {1, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 3 * sizeof(float)}, // color
            {2, 0, VK_FORMAT_R32_SFLOAT, 7 * sizeof(float)},          // size
            {3, 0, VK_FORMAT_R32_SFLOAT, 8 * sizeof(float)},          // tile
        };

        auto buildParticlePipeline = [&](VkPipelineColorBlendAttachmentState blend) -> VkPipeline {
            return PipelineBuilder()
                .setShaders(particleVert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                            particleFrag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
                .setVertexInput({pBind}, pAttrs)
                .setTopology(VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
                .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
                .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
                .setColorBlendAttachment(blend)
                .setMultisample(vkCtx_->getMsaaSamples())
                .setLayout(particlePipelineLayout_)
                .setRenderPass(mainPass)
                .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
                .build(device, vkCtx_->getPipelineCache());
        };

        particlePipeline_ = buildParticlePipeline(PipelineBuilder::blendAlpha());
        particleAdditivePipeline_ = buildParticlePipeline(PipelineBuilder::blendAdditive());
    }

    // --- Smoke pipeline ---
    if (smokeVert.isValid() && smokeFrag.isValid()) {
        VkVertexInputBindingDescription sBind{};
        sBind.binding = 0;
        sBind.stride = 6 * sizeof(float); // pos3 + lifeRatio1 + size1 + isSpark1
        sBind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::vector<VkVertexInputAttributeDescription> sAttrs = {
            {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},           // position
            {1, 0, VK_FORMAT_R32_SFLOAT, 3 * sizeof(float)}, // lifeRatio
            {2, 0, VK_FORMAT_R32_SFLOAT, 4 * sizeof(float)}, // size
            {3, 0, VK_FORMAT_R32_SFLOAT, 5 * sizeof(float)}, // isSpark
        };

        smokePipeline_ = PipelineBuilder()
            .setShaders(smokeVert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                        smokeFrag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
            .setVertexInput({sBind}, sAttrs)
            .setTopology(VK_PRIMITIVE_TOPOLOGY_POINT_LIST)
            .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
            .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
            .setColorBlendAttachment(PipelineBuilder::blendAlpha())
            .setMultisample(vkCtx_->getMsaaSamples())
            .setLayout(smokePipelineLayout_)
            .setRenderPass(mainPass)
            .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
            .build(device, vkCtx_->getPipelineCache());
    }

    // --- Ribbon pipelines ---
    {
        rendering::VkShaderModule ribVert, ribFrag;
        (void)ribVert.loadFromFile(device, "assets/shaders/m2_ribbon.vert.spv");
        (void)ribFrag.loadFromFile(device, "assets/shaders/m2_ribbon.frag.spv");
        if (ribVert.isValid() && ribFrag.isValid()) {
            VkVertexInputBindingDescription rBind{};
            rBind.binding = 0;
            rBind.stride = 9 * sizeof(float);
            rBind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

            std::vector<VkVertexInputAttributeDescription> rAttrs = {
                {0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 3 * sizeof(float)},
                {2, 0, VK_FORMAT_R32_SFLOAT,       6 * sizeof(float)},
                {3, 0, VK_FORMAT_R32G32_SFLOAT,    7 * sizeof(float)},
            };

            auto buildRibbonPipeline = [&](VkPipelineColorBlendAttachmentState blend) -> VkPipeline {
                return PipelineBuilder()
                    .setShaders(ribVert.stageInfo(VK_SHADER_STAGE_VERTEX_BIT),
                                ribFrag.stageInfo(VK_SHADER_STAGE_FRAGMENT_BIT))
                    .setVertexInput({rBind}, rAttrs)
                    .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP)
                    .setRasterization(VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE)
                    .setDepthTest(true, false, VK_COMPARE_OP_LESS_OR_EQUAL)
                    .setColorBlendAttachment(blend)
                    .setMultisample(vkCtx_->getMsaaSamples())
                    .setLayout(ribbonPipelineLayout_)
                    .setRenderPass(mainPass)
                    .setDynamicStates({VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR})
                    .build(device, vkCtx_->getPipelineCache());
            };

            ribbonPipeline_         = buildRibbonPipeline(PipelineBuilder::blendAlpha());
            ribbonAdditivePipeline_ = buildRibbonPipeline(PipelineBuilder::blendAdditive());
        }
        ribVert.destroy(); ribFrag.destroy();
    }

    m2Vert.destroy(); m2Frag.destroy();
    particleVert.destroy(); particleFrag.destroy();
    smokeVert.destroy(); smokeFrag.destroy();

    core::Logger::getInstance().info("M2Renderer: pipelines recreated");
}

} // namespace rendering
} // namespace wowee
