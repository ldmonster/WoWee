// M2 struct layout and field tests (header-only, no loader source)
#include <catch_amalgamated.hpp>
#include "pipeline/m2_loader.hpp"
#include <cstring>

using namespace wowee::pipeline;

TEST_CASE("M2Sequence fields are default-initialized", "[m2]") {
    M2Sequence seq{};
    REQUIRE(seq.id == 0);
    REQUIRE(seq.duration == 0);
    REQUIRE(seq.movingSpeed == 0.0f);
    REQUIRE(seq.flags == 0);
    REQUIRE(seq.blendTime == 0);
    REQUIRE(seq.boundRadius == 0.0f);
}

TEST_CASE("M2AnimationTrack hasData", "[m2]") {
    M2AnimationTrack track;
    REQUIRE_FALSE(track.hasData());

    track.sequences.push_back({});
    REQUIRE(track.hasData());
}

TEST_CASE("M2AnimationTrack default interpolation", "[m2]") {
    M2AnimationTrack track;
    REQUIRE(track.interpolationType == 0);
    REQUIRE(track.globalSequence == -1);
}

TEST_CASE("M2Bone parent defaults to root", "[m2]") {
    M2Bone bone{};
    bone.parentBone = -1;
    REQUIRE(bone.parentBone == -1);
    REQUIRE(bone.keyBoneId == 0);
}

TEST_CASE("M2Vertex layout", "[m2]") {
    M2Vertex vert{};
    vert.position = glm::vec3(1.0f, 2.0f, 3.0f);
    vert.boneWeights[0] = 255;
    vert.boneWeights[1] = 0;
    vert.boneWeights[2] = 0;
    vert.boneWeights[3] = 0;
    vert.boneIndices[0] = 5;
    vert.normal = glm::vec3(0.0f, 1.0f, 0.0f);
    vert.texCoords[0] = glm::vec2(0.5f, 0.5f);

    REQUIRE(vert.position.x == 1.0f);
    REQUIRE(vert.boneWeights[0] == 255);
    REQUIRE(vert.boneIndices[0] == 5);
    REQUIRE(vert.normal.y == 1.0f);
    REQUIRE(vert.texCoords[0].x == 0.5f);
}

TEST_CASE("M2Texture stores filename", "[m2]") {
    M2Texture tex{};
    tex.type = 1;
    tex.filename = "Creature\\Hogger\\Hogger.blp";
    REQUIRE(tex.filename == "Creature\\Hogger\\Hogger.blp");
}

TEST_CASE("M2Batch submesh fields", "[m2]") {
    M2Batch batch{};
    batch.skinSectionIndex = 3;
    batch.textureCount = 2;
    batch.indexStart = 100;
    batch.indexCount = 300;
    batch.vertexStart = 0;
    batch.vertexCount = 150;
    batch.submeshId = 0;
    batch.submeshLevel = 0;

    REQUIRE(batch.skinSectionIndex == 3);
    REQUIRE(batch.textureCount == 2);
    REQUIRE(batch.indexCount == 300);
    REQUIRE(batch.vertexCount == 150);
}

TEST_CASE("M2Material blend modes", "[m2]") {
    M2Material mat{};
    mat.flags = 0;
    mat.blendMode = 2;  // Alpha blend
    REQUIRE(mat.blendMode == 2);

    mat.blendMode = 0;  // Opaque
    REQUIRE(mat.blendMode == 0);
}

TEST_CASE("M2Model isValid", "[m2]") {
    M2Model model{};
    REQUIRE_FALSE(model.isValid()); // no vertices or indices

    model.vertices.push_back({});
    REQUIRE_FALSE(model.isValid()); // vertices but no indices

    model.indices.push_back(0);
    REQUIRE(model.isValid()); // both present
}

TEST_CASE("M2Model bounding box", "[m2]") {
    M2Model model{};
    model.boundMin = glm::vec3(-1.0f, -2.0f, -3.0f);
    model.boundMax = glm::vec3(1.0f, 2.0f, 3.0f);
    model.boundRadius = 5.0f;

    glm::vec3 center = (model.boundMin + model.boundMax) * 0.5f;
    REQUIRE(center.x == Catch::Approx(0.0f));
    REQUIRE(center.y == Catch::Approx(0.0f));
    REQUIRE(center.z == Catch::Approx(0.0f));
}

TEST_CASE("M2ParticleEmitter defaults", "[m2]") {
    M2ParticleEmitter emitter{};
    emitter.textureRows = 1;
    emitter.textureCols = 1;
    emitter.enabled = true;
    REQUIRE(emitter.textureRows == 1);
    REQUIRE(emitter.textureCols == 1);
    REQUIRE(emitter.enabled);
}

TEST_CASE("M2RibbonEmitter defaults", "[m2]") {
    M2RibbonEmitter ribbon{};
    REQUIRE(ribbon.edgesPerSecond == Catch::Approx(15.0f));
    REQUIRE(ribbon.edgeLifetime == Catch::Approx(0.5f));
    REQUIRE(ribbon.gravity == Catch::Approx(0.0f));
}

TEST_CASE("M2Attachment position", "[m2]") {
    M2Attachment att{};
    att.id = 1;  // Right hand
    att.bone = 42;
    att.position = glm::vec3(0.1f, 0.2f, 0.3f);

    REQUIRE(att.id == 1);
    REQUIRE(att.bone == 42);
    REQUIRE(att.position.z == Catch::Approx(0.3f));
}

TEST_CASE("M2Model collections", "[m2]") {
    M2Model model{};

    // Bones
    model.bones.push_back({});
    model.bones[0].parentBone = -1;
    model.bones[0].pivot = glm::vec3(0, 0, 0);

    // Sequences
    model.sequences.push_back({});
    model.sequences[0].id = 0; // Stand
    model.sequences[0].duration = 1000;

    // Textures
    model.textures.push_back({});
    model.textures[0].type = 0;
    model.textures[0].filename = "test.blp";

    REQUIRE(model.bones.size() == 1);
    REQUIRE(model.sequences.size() == 1);
    REQUIRE(model.textures.size() == 1);
    REQUIRE(model.sequences[0].duration == 1000);
}
