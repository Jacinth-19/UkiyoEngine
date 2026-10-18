// Skinned glTF / VRM model: geometry, MToon materials, skeleton and a small
// procedural idle animation (breathing + head tracking the camera).
#pragma once

#include <array>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include "engine/gpu_types.h"
#include "rhi/rhi.h"
#include "engine/rig.h"

namespace ukiyo {

struct Material {
    std::string name;
    MaterialUBO params{};
    std::unique_ptr<rhi::Buffer> ubo;
    std::unique_ptr<rhi::DescriptorSet> set;   // SetType::Material
    bool doubleSided = false;
    bool blend = false;      // alpha blended, drawn after the opaque pass
    bool mask = false;       // alpha tested
    glm::vec4 outlineBase{0.0f};   // the outline as loaded (selection outline changes params.outline and restores this)
    bool outlined() const { return params.flags2.x > 0.5f && params.outline.w > 0.0f; }
};

// A simplified index buffer over the same vertices as the full primitive (item 15).
struct LodLevel {
    std::unique_ptr<rhi::Buffer> indices;
    uint32_t indexCount = 0;
};

struct Primitive {
    std::unique_ptr<rhi::Buffer> vertices;
    std::unique_ptr<rhi::Buffer> indices;
    uint32_t indexCount = 0;
    Material* material = nullptr;
    // Opt-in LOD levels built by buildModelLods. lods[k] is level k + 1; level 0 is `indices`.
    std::vector<LodLevel> lods;

    // Morph targets (facial expressions). Empty when the primitive has none.
    int mesh = -1;                                          // glTF mesh index
    std::vector<rhi::Vertex> morphBase;                     // rest vertices (CPU copy)
    std::vector<std::vector<glm::vec3>> morphPos, morphNrm; // per target, per vertex
    std::vector<float> morphW;                              // weights currently uploaded

    // CPU copy of the geometry, for navmesh baking and other queries. Not uploaded again.
    std::vector<rhi::Vertex> cpuVertices;
    std::vector<uint32_t> cpuIndices;
};

// A morph target driven by an expression: target `target` of mesh `mesh` at `weight`.
struct MorphBind {
    int mesh = -1;
    int target = -1;
    float weight = 1.0f;
};

// One draw: a primitive placed by a node. skin == -1 for static geometry.
struct DrawItem {
    int node = -1;
    int primitive = -1;
    int skin = -1;
};

// Spring-bone collider (VRMC_springBone or VRM 0.x secondaryAnimation).
struct SpringCollider {
    int node = -1;
    bool capsule = false;
    glm::vec3 offset{0.0f};     // node-local
    glm::vec3 tail{0.0f};       // node-local, capsules only
    float radius = 0.0f;
};

// One simulated bone. Its child (or its own axis, for tips) defines the rest direction.
struct SpringJoint {
    int node = -1;
    glm::vec3 restDir{0.0f, -1.0f, 0.0f};   // rest direction to the child, joint-local
    float boneLength = 0.1f;
    float hitRadius = 0.02f;
    float stiffness = 1.0f;
    float gravityPower = 0.0f;
    glm::vec3 gravityDir{0.0f, -1.0f, 0.0f};
    float drag = 0.4f;
    std::vector<int> colliders;             // indices into Model::colliders
    glm::vec3 prevTail{0.0f}, currTail{0.0f};   // world-space verlet state
    bool init = false;
};

// Procedural reaction layer: damped springs on upper-body bones, driven by hit
// impulses and balance sway. Angles are small rotations in model space (radians).
struct ReactionBone {
    int node = -1;
    glm::vec3 angle{0.0f};
    glm::vec3 vel{0.0f};
    float stiffness = 60.0f;
    float damping = 8.0f;
    float weight = 1.0f;     // share of hit impulses
};

// Torso collision volume: a vertical, tapered elliptical capsule around the waist and
// lower chest. Measured from the skin at load time, so it fits any VRM body.
struct TorsoCollider {
    bool valid = false;
    static constexpr int kSamples = 5;
    float ys[kSamples] = {0, 0, 0, 0, 0};   // heights, ascending (model space)
    float cx[kSamples] = {0, 0, 0, 0, 0};   // axis x per height
    float cz[kSamples] = {0, 0, 0, 0, 0};   // axis z per height
    float ax[kSamples] = {0, 0, 0, 0, 0};   // half width (lateral) per height
    float az[kSamples] = {0, 0, 0, 0, 0};   // half depth (front/back) per height
    float goalY = 0.0f;                     // height of the hand-on-waist contact
};

// One arm. Solved by two-bone IK after the animation pose, then pushed out of the torso.
struct ArmIK {
    bool valid = false;
    int upper = -1, lower = -1, hand = -1;  // humanoid nodes
    float hipWeight = 0.0f;                 // 0..1 hand-on-waist request (set by AnimPlayer)
    glm::vec3 offset{0.0f};                 // damped push-out applied to the wrist target
    glm::vec3 offsetVel{0.0f};
    glm::vec3 offsetGoal{0.0f};             // accumulated clearance needed (decays when clear)
    glm::vec3 elbowBias{0.0f};              // last frame's elbow push (pole bias)
    float clearance = 1e9f;                 // min distance from the solved arm to the torso (m), last frame
    float penetration = 0.0f;               // deepest penetration before correction (m), last frame
};

struct Node {
    std::string name;
    int parent = -1;
    int springJoint = -1;                // index into Model::springJoints, or -1
    int mesh = -1;                       // glTF mesh index this node draws, or -1
    glm::vec3 translation{0.0f};
    glm::quat baseRotation{1.0f, 0.0f, 0.0f, 0.0f};
    glm::vec3 scale{1.0f};
    bool hasMatrix = false;
    glm::mat4 matrix{1.0f};
    glm::mat4 local{1.0f};
    glm::mat4 world{1.0f};
};

struct Skin {
    std::vector<int> joints;            // node indices
    std::vector<glm::mat4> inverseBind;
    std::unique_ptr<rhi::Buffer> ubo;   // JointUBO
    std::unique_ptr<rhi::DescriptorSet> set;  // SetType::Skin
};

struct Model {
    std::string name;
    std::string sourcePath;
    bool isVRM0 = false;
    bool isVRM = false;
    glm::mat4 root{1.0f};               // VRM0 faces -Z, so it gets a 180 degree turn; see applyPlacement
    glm::mat4 placement{1.0f};          // scene placement of this instance (translation and yaw)
    std::vector<Node> nodes;
    std::vector<int> updateOrder;       // parents before children
    std::vector<std::unique_ptr<rhi::Texture>> textures;
    std::vector<std::unique_ptr<Material>> materials;
    std::vector<Primitive> primitives;
    std::vector<DrawItem> draws;
    std::vector<Skin> skins;
    // Identity joint palette bound for non-skinned draws (shader ignores it).
    std::unique_ptr<rhi::Buffer> staticSkinUbo;
    std::unique_ptr<rhi::DescriptorSet> staticSkinSet;
    std::map<std::string, int> humanoid;   // humanoid bone name -> node index
    glm::vec3 boundsMin{0.0f}, boundsMax{0.0f};
    // Distance cull for the whole model (0 = off): nothing is drawn beyond cullDistance from the model's
    // centre. Between fadeDistance and cullDistance the model is dithered out.
    float cullDistance = 0.0f;
    float fadeDistance = 0.0f;
    // Opt-in mesh LOD (item 15): the distance from the model centre at which each level starts. Empty = off.
    std::vector<float> lodSwitch;

    // Procedural idle state.
    float headYaw = 0.0f;
    float headPitch = 0.0f;

    // Spring bones (hair, clothing). Simulated in updateModel.
    std::vector<SpringCollider> colliders;
    std::vector<SpringJoint> springJoints;
    bool physics = true;                 // toggle spring-bone simulation
    float lastSimTime = -1.0f;

    // Facial expressions. Canonical names: aa ih ou ee oh blink happy angry sad surprised
    // relaxed. Set expressionValues each frame; applyExpressions() blends the morphs.
    std::map<std::string, std::vector<MorphBind>> expressionBinds;
    std::map<std::string, float> expressionValues;
    // Head motion from the face track (radians): nod is positive forward, tilt positive toward +X.
    float faceNod = 0.0f;
    float faceTilt = 0.0f;

    // Reaction layer (hits and sway).
    std::vector<ReactionBone> reactions;
    bool reactionsEnabled = true;
    glm::vec3 swayForce{0.0f};           // model-space, consumed each update

    // Animation overlays written by AnimPlayer. When poseActive[i] is set, node i uses
    // poseRot/poseTrans instead of its base rotation/translation.
    std::vector<glm::quat> poseRot;
    std::vector<glm::vec3> poseTrans;
    std::vector<uint8_t> poseActive;
    // IK overlays for arm nodes (rebuilt every frame by updateModel; not fed back).
    std::vector<glm::quat> ikRot;
    std::vector<uint8_t> ikActive;

    // Collision and arm IK. arms[0] is the right arm, arms[1] the left (model space).
    TorsoCollider torso;
    std::array<ArmIK, 2> arms;
    glm::vec3 lateral{1.0f, 0.0f, 0.0f}; // model-space direction toward the left arm
    glm::vec3 facing{0.0f, 0.0f, 1.0f};  // model-space forward (pre-root)
    bool armCollision = true;            // torso collision and hand-on-waist IK

    // Animation rigging layers, evaluated after the animation and arm solver (rig.h).
    std::vector<Rig> rigs;
};

// Sets m.root from the VRM0 turn and the instance placement. Call after loading, and whenever
// m.placement changes. Every node, skin palette and spring simulation uses m.root.
// Per-character effects. Each character has its own material copies, so these change one character only.
struct CharacterFx {
    float dissolve = 0.0f;          // 0..1: the cut-out amount (noise threshold); 0 = off
    float dissolveEdge = 0.06f;     // width of the glow band above the cut (noise units)
    glm::vec3 edgeColor{1.0f, 0.5f, 0.1f};
    float darken = 0.0f;            // 0..1: darkens the character (target highlight)
    float hit = 0.0f;               // 0..1: flashes toward hitColor (get hit)
    glm::vec3 hitColor{1.0f, 0.2f, 0.2f};
    float upgradePulse = 0.0f;      // 0..1: pulsing glow in upgradeColor (upgrade)
    glm::vec3 upgradeColor{1.0f, 0.9f, 0.4f};
    bool selected = false;          // selection outline: recolours and widens the outline the material already has
    glm::vec3 selectColor{1.0f, 0.85f, 0.2f};
};
void setCharacterFx(Model& m, const CharacterFx& fx);
// Hair within radius of the face anchor (world point) is dithered out on the camera side. Radius 0 turns it off.
// Returns the number of hair materials (names containing "hair").
int setHairFace(Model& m, const glm::vec3& anchorWorld, float radius);
// A model that is not occludable keeps its surfaces when the occlusion target is set (use it for the target itself).
void setOccludable(Model& m, bool occludable);

void applyPlacement(Model& m);

// Load-time options. smoothNormals averages the normals of vertices that share a position within each
// primitive (RealToon smooth object normal): hard creases are lost, and the outline hull closes up.
struct ModelLoadOptions {
    bool smoothNormals = false;
    std::string faceSdfPath;               // PNG face shadow atlas (tools/make_face_sdf.py); empty = none
    std::string faceMaterial = "face";      // materials whose name contains this (any case) use the SDF
};

// Loads a .vrm (GLB) or .gltf. Throws std::runtime_error on failure.
std::unique_ptr<Model> loadModel(rhi::Device& device, const std::string& path, const ModelLoadOptions& opts = {});

// Hit impulse. worldDir is the direction the impact pushes the body; strength is in
// radians per second of initial angular velocity on the upper body.
void applyHitImpulse(Model& model, const glm::vec3& worldDir, float strength);

// Balance sway. worldAccel is the body's acceleration; the upper body leans against it.
void applySway(Model& model, const glm::vec3& worldAccel);

// Blends morph targets for the current expressionValues and re-uploads changed meshes.
// Called from updateModel; exposed for tests.
void applyExpressions(Model& model);

// Advances the procedural animation and recomputes all world and joint matrices.
// Call once per frame after beginFrame (it writes to GPU buffers).
void updateModel(Model& model, float time, const glm::vec3& lookAtWorld);

// Appends the drawn geometry of `model` in world space: every primitive of every node that draws a
// mesh, transformed by that node's world matrix. Indices are offset into *verts. Call after updateModel.
void worldTriangles(const Model& model, std::vector<glm::vec3>* verts, std::vector<uint32_t>* tris);

// Opt-in mesh LOD (item 15). Builds simplified index buffers for each static primitive from its CPU copy, with
// meshopt_simplify (meshoptimizer, MIT). Level k keeps about 0.5^k of the level-0 triangles and is dropped when
// simplification does not reduce the count. The border is locked so open edges stay put. `switches` sets the
// distance (from the model centre) at which each level starts; empty switches turn LOD off and free nothing.
// Returns the number of LOD levels built across the model.
int buildModelLods(rhi::Device& device, Model& model, const std::vector<float>& switches);

} // namespace ukiyo
