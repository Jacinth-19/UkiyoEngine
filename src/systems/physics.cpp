#include "systems/physics.h"

#include <algorithm>
#include <optional>
#include <cstdarg>
#include <cstdio>
#include <thread>
#include <unordered_map>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/CastResult.h>
#include <Jolt/Physics/Collision/RayCast.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <set>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include "engine/water.h"



namespace ukiyo {
namespace {

// Two object layers: static geometry never collides with other static geometry.
namespace Layers {
constexpr JPH::ObjectLayer NON_MOVING = 0;
constexpr JPH::ObjectLayer MOVING = 1;
constexpr JPH::ObjectLayer NUM = 2;
} // namespace Layers

namespace BroadPhases {
const JPH::BroadPhaseLayer NON_MOVING(0);
const JPH::BroadPhaseLayer MOVING(1);
constexpr uint32_t NUM = 2;
} // namespace BroadPhases

class BroadPhaseLayers final : public JPH::BroadPhaseLayerInterface {
public:
    uint32_t GetNumBroadPhaseLayers() const override { return BroadPhases::NUM; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return layer == Layers::NON_MOVING ? BroadPhases::NON_MOVING : BroadPhases::MOVING;
    }
};

class ObjectVsBroadPhase final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer bp) const override {
        if (layer == Layers::NON_MOVING) return bp == BroadPhases::MOVING;
        return true;
    }
};

class ObjectPairs final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a == Layers::MOVING || b == Layers::MOVING;
    }
};

void traceImpl(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    std::vfprintf(stderr, fmt, args);
    std::fputc('\n', stderr);
    va_end(args);
}

// Jolt has process-wide state (type registry and factory). It is set up once.
void initJoltOnce() {
    static bool done = false;
    if (done) return;
    done = true;
    JPH::RegisterDefaultAllocator();
    JPH::Trace = traceImpl;
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
}

JPH::Vec3 toJolt(const glm::vec3& v) { return JPH::Vec3(v.x, v.y, v.z); }
glm::vec3 fromJolt(JPH::Vec3 v) { return glm::vec3(v.GetX(), v.GetY(), v.GetZ()); }

// An unordered body pair as a key: the smaller handle first. Returned by value (std::minmax would hand back
// references to temporaries here).
std::pair<uint32_t, uint32_t> pairKey(uint32_t a, uint32_t b) { return a < b ? std::make_pair(a, b) : std::make_pair(b, a); }

// Jolt calls these on its job threads during Update. They only copy the contact into a locked queue; nothing
// here touches Lua or changes a body.
class ContactQueue final : public JPH::ContactListener {
public:
    void OnContactAdded(const JPH::Body& b1, const JPH::Body& b2, const JPH::ContactManifold& m,
                        JPH::ContactSettings&) override {
        PhysicsWorld::Contact c;
        c.added = true;
        c.a = b1.GetID().GetIndexAndSequenceNumber();
        c.b = b2.GetID().GetIndexAndSequenceNumber();
        if (!m.mRelativeContactPointsOn1.empty())
            c.point = fromJolt(JPH::Vec3(m.GetWorldSpaceContactPointOn1(0)));
        c.normal = fromJolt(m.mWorldSpaceNormal);
        push(c);
    }
    void OnContactRemoved(const JPH::SubShapeIDPair& pair) override {
        PhysicsWorld::Contact c;
        c.added = false;
        c.a = pair.GetBody1ID().GetIndexAndSequenceNumber();
        c.b = pair.GetBody2ID().GetIndexAndSequenceNumber();
        push(c);
    }
    // Blocked pairs are changed on the main thread between steps and read by Jolt's validate callback.
    void setPairEnabled(uint32_t a, uint32_t b, bool enabled) {
        std::lock_guard<std::mutex> lock(mu_);
        const auto key = pairKey(a, b);
        if (enabled) blocked_.erase(key);
        else blocked_.insert(key);
    }
    JPH::ValidateResult OnContactValidate(const JPH::Body& b1, const JPH::Body& b2, JPH::RVec3Arg,
                                          const JPH::CollideShapeResult&) override {
        const auto key = pairKey(b1.GetID().GetIndexAndSequenceNumber(), b2.GetID().GetIndexAndSequenceNumber());
        std::lock_guard<std::mutex> lock(mu_);
        return blocked_.count(key) ? JPH::ValidateResult::RejectAllContactsForThisBodyPair
                                   : JPH::ValidateResult::AcceptAllContactsForThisBodyPair;
    }
    std::vector<PhysicsWorld::Contact> take() {
        std::lock_guard<std::mutex> lock(mu_);
        std::vector<PhysicsWorld::Contact> out;
        out.swap(events_);
        return out;
    }

private:
    void push(const PhysicsWorld::Contact& c) {
        std::lock_guard<std::mutex> lock(mu_);
        events_.push_back(c);
    }
    std::mutex mu_;
    std::vector<PhysicsWorld::Contact> events_;
    std::set<std::pair<uint32_t, uint32_t>> blocked_;
};

} // namespace

struct PhysicsWorld::Impl {
    BroadPhaseLayers bpLayers;
    ObjectVsBroadPhase objVsBp;
    ObjectPairs pairs;
    JPH::TempAllocatorImpl temp{16 * 1024 * 1024};
    JPH::JobSystemThreadPool jobs{JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                                  std::max(1, int(std::thread::hardware_concurrency()) - 1)};
    ContactQueue contacts;   // declared before the system, which holds a pointer to it
    JPH::PhysicsSystem system;
    float accumulator = 0.0f;
    float fixedStep = 1.0f / 60.0f;
    std::optional<WaterVolume> water;   // item 17: buoyancy volume, none when empty
    std::unordered_map<uint32_t, float> mass;   // dynamic bodies: mass in kg, for buoyancy
    float waterTime = 0.0f;             // wave clock, seconds
    // Interpolation. `prev` holds the pose of each dynamic body before the last fixed step, and `alpha`
    // is how far the render time is between that step and the current one (0 to 1).
    struct Pose { glm::vec3 p; glm::quat r; };
    bool interpolate = true;
    float alpha = 1.0f;
    std::unordered_map<uint32_t, Pose> prev;
    // Body changes requested while `defer` is on. They run at the start of the next step().
    struct Deferred { enum class Op { SetPosition, AddImpulse, Remove } op; uint32_t h; glm::vec3 v; };
    std::vector<Deferred> deferred;
    bool defer = false;
    // Characters (item 4). CharacterVirtual is not a body, so it is not in the body map; the handle is ours.
    struct CharacterEntry {
        JPH::Ref<JPH::CharacterVirtual> character;
        float height = 1.8f;
        float radius = 0.3f;
        float stepHeight = 0.4f;
        float stickToFloor = 0.35f;
        float jumpSpeed = 5.0f;
        float yaw = 0.0f;
        float verticalVelocity = 0.0f;
        float airTime = 0.0f;   // seconds since the character was last on the ground; a jump is allowed briefly after
    };
    std::unordered_map<uint32_t, CharacterEntry> characters;
    uint32_t nextCharacter = 1;
};

static PhysicsWorld::Impl::Pose readPose(const JPH::BodyInterface& bi, uint32_t h) {
    const JPH::BodyID id(h);
    const JPH::Vec3 p = JPH::Vec3(bi.GetPosition(id));
    const JPH::Quat q = bi.GetRotation(id);
    return {glm::vec3(p.GetX(), p.GetY(), p.GetZ()), glm::quat(q.GetW(), q.GetX(), q.GetY(), q.GetZ())};
}

// Jolt's allocators are set up by initJoltOnce(), so it has to run before any Impl is built.
static PhysicsWorld::Impl* newImpl() {
    initJoltOnce();
    return new PhysicsWorld::Impl;
}

PhysicsWorld::PhysicsWorld() : impl_(newImpl()) {
    impl_->system.Init(4096, 0, 4096, 10240, impl_->bpLayers, impl_->objVsBp, impl_->pairs);
    impl_->system.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
    impl_->system.SetContactListener(&impl_->contacts);
}

PhysicsWorld::~PhysicsWorld() {
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    JPH::BodyIDVector ids;
    impl_->system.GetBodies(ids);
    for (const JPH::BodyID& id : ids) {
        bi.RemoveBody(id);
        bi.DestroyBody(id);
    }
}

void PhysicsWorld::setGravity(const glm::vec3& g) { impl_->system.SetGravity(toJolt(g)); }
glm::vec3 PhysicsWorld::gravity() const { return fromJolt(impl_->system.GetGravity()); }

void PhysicsWorld::setFixedStep(float seconds) {
    if (!(seconds >= 1.0f / 120.0f)) seconds = 1.0f / 120.0f;
    if (seconds > 1.0f / 20.0f) seconds = 1.0f / 20.0f;
    impl_->fixedStep = seconds;
}
float PhysicsWorld::fixedStep() const { return impl_->fixedStep; }

void PhysicsWorld::setSolverSteps(int velocity, int position) {
    JPH::PhysicsSettings ps = impl_->system.GetPhysicsSettings();
    ps.mNumVelocitySteps = static_cast<JPH::uint>(std::min(std::max(velocity, 1), 32));
    ps.mNumPositionSteps = static_cast<JPH::uint>(std::min(std::max(position, 1), 32));
    impl_->system.SetPhysicsSettings(ps);
}
int PhysicsWorld::velocitySolverSteps() const { return static_cast<int>(impl_->system.GetPhysicsSettings().mNumVelocitySteps); }
int PhysicsWorld::positionSolverSteps() const { return static_cast<int>(impl_->system.GetPhysicsSettings().mNumPositionSteps); }

PhysicsWorld::Handle PhysicsWorld::addStaticBox(const glm::vec3& c, const glm::vec3& h) {
    JPH::BoxShapeSettings settings(toJolt(h));
    JPH::ShapeRefC shape = settings.Create().Get();
    JPH::BodyCreationSettings bcs(shape, JPH::RVec3(c.x, c.y, c.z), JPH::Quat::sIdentity(), JPH::EMotionType::Static,
                                  Layers::NON_MOVING);
    JPH::BodyID id = impl_->system.GetBodyInterface().CreateAndAddBody(bcs, JPH::EActivation::DontActivate);
    ++bodies_;
    return id.GetIndexAndSequenceNumber();
}

PhysicsWorld::Handle PhysicsWorld::addStaticMesh(const std::vector<glm::vec3>& vertices,
                                                 const std::vector<uint32_t>& indices) {
    if (vertices.empty() || indices.size() < 3) return kNone;
    JPH::MeshShapeSettings settings;
    settings.mTriangleVertices.reserve(vertices.size());
    for (const glm::vec3& v : vertices) settings.mTriangleVertices.push_back(JPH::Float3(v.x, v.y, v.z));
    settings.mIndexedTriangles.reserve(indices.size() / 3);
    for (size_t i = 0; i + 2 < indices.size(); i += 3)
        settings.mIndexedTriangles.push_back(JPH::IndexedTriangle(indices[i], indices[i + 1], indices[i + 2]));
    JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) return kNone;
    JPH::BodyCreationSettings bcs(result.Get(), JPH::RVec3(0, 0, 0), JPH::Quat::sIdentity(),
                                  JPH::EMotionType::Static, Layers::NON_MOVING);
    JPH::BodyID id = impl_->system.GetBodyInterface().CreateAndAddBody(bcs, JPH::EActivation::DontActivate);
    ++bodies_;
    return id.GetIndexAndSequenceNumber();
}

PhysicsWorld::Handle PhysicsWorld::addDynamicSphere(const glm::vec3& p, float radius, float mass) {
    JPH::SphereShapeSettings settings(radius);
    JPH::BodyCreationSettings bcs(settings.Create().Get(), JPH::RVec3(p.x, p.y, p.z), JPH::Quat::sIdentity(),
                                  JPH::EMotionType::Dynamic, Layers::MOVING);
    bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    bcs.mMassPropertiesOverride.mMass = mass;
    JPH::BodyID id = impl_->system.GetBodyInterface().CreateAndAddBody(bcs, JPH::EActivation::Activate);
    ++bodies_;
    impl_->prev[id.GetIndexAndSequenceNumber()] = readPose(impl_->system.GetBodyInterface(), id.GetIndexAndSequenceNumber());
    impl_->mass[id.GetIndexAndSequenceNumber()] = mass;
    return id.GetIndexAndSequenceNumber();
}

PhysicsWorld::Handle PhysicsWorld::addDynamicBox(const glm::vec3& p, const glm::vec3& h, float mass) {
    JPH::BoxShapeSettings settings(toJolt(h));
    JPH::BodyCreationSettings bcs(settings.Create().Get(), JPH::RVec3(p.x, p.y, p.z), JPH::Quat::sIdentity(),
                                  JPH::EMotionType::Dynamic, Layers::MOVING);
    bcs.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
    bcs.mMassPropertiesOverride.mMass = mass;
    JPH::BodyID id = impl_->system.GetBodyInterface().CreateAndAddBody(bcs, JPH::EActivation::Activate);
    ++bodies_;
    impl_->prev[id.GetIndexAndSequenceNumber()] = readPose(impl_->system.GetBodyInterface(), id.GetIndexAndSequenceNumber());
    impl_->mass[id.GetIndexAndSequenceNumber()] = mass;
    return id.GetIndexAndSequenceNumber();
}

void PhysicsWorld::removeBody(Handle h) {
    if (h == kNone) return;
    if (impl_->defer) {
        impl_->deferred.push_back({Impl::Deferred::Op::Remove, h, glm::vec3(0.0f)});
        return;
    }
    JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
    JPH::BodyID id(h);
    if (!bi.IsAdded(id)) return;
    bi.RemoveBody(id);
    bi.DestroyBody(id);
    impl_->prev.erase(h);
    impl_->mass.erase(h);
    --bodies_;
}

CharacterHandle PhysicsWorld::addCharacter(const glm::vec3& feet, const CharacterSettings& settings) {
    CharacterSettings s = settings;
    s.radius = std::clamp(s.radius, 0.15f, 2.0f);
    s.height = std::max(s.height, 2.0f * s.radius + 0.05f);
    s.maxSlopeDeg = std::clamp(s.maxSlopeDeg, 5.0f, 89.0f);
    s.stepHeight = std::clamp(s.stepHeight, 0.0f, s.height * 0.5f);
    s.stickToFloor = std::clamp(s.stickToFloor, 0.0f, 2.0f);
    s.jumpSpeed = std::clamp(s.jumpSpeed, 0.0f, 30.0f);

    // The capsule's cylinder half height: `height` counts the two hemispherical caps.
    const float halfCylinder = std::max((s.height - 2.0f * s.radius) * 0.5f, 0.01f);
    JPH::RefConst<JPH::Shape> shape = new JPH::CapsuleShape(halfCylinder, s.radius);

    JPH::CharacterVirtualSettings vc;
    vc.mShape = shape;
    vc.mMaxSlopeAngle = JPH::DegreesToRadians(s.maxSlopeDeg);
    // The supporting plane sits at the bottom of the capsule, so a step is measured from the feet.
    vc.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -s.radius);
    vc.mMass = s.mass;

    const uint32_t h = impl_->nextCharacter++;
    JPH::CharacterVirtual* ch = new JPH::CharacterVirtual(
        &vc, JPH::RVec3(feet.x, feet.y + s.height * 0.5f, feet.z), JPH::Quat::sIdentity(), &impl_->system);
    Impl::CharacterEntry e;
    e.character = ch;
    e.height = s.height;
    e.radius = s.radius;
    e.stepHeight = s.stepHeight;
    e.stickToFloor = s.stickToFloor;
    e.jumpSpeed = s.jumpSpeed;
    impl_->characters.emplace(h, e);
    return h;
}

void PhysicsWorld::removeCharacter(CharacterHandle h) { impl_->characters.erase(h); }

bool PhysicsWorld::hasCharacter(CharacterHandle h) const { return impl_->characters.count(h) != 0; }

void PhysicsWorld::setCharacterYaw(CharacterHandle h, float yawRadians) {
    const auto it = impl_->characters.find(h);
    if (it == impl_->characters.end()) return;
    it->second.yaw = yawRadians;
    it->second.character->SetRotation(JPH::Quat::sRotation(JPH::Vec3::sAxisY(), yawRadians));
}

bool PhysicsWorld::moveCharacter(CharacterHandle h, const glm::vec3& planar, bool jump, float dt, CharacterState* out) {
    const auto it = impl_->characters.find(h);
    if (it == impl_->characters.end()) return false;
    Impl::CharacterEntry& e = it->second;
    dt = std::clamp(dt, 1.0f / 240.0f, 0.1f);

    JPH::CharacterVirtual* ch = e.character;
    const bool grounded = ch->GetGroundState() == JPH::CharacterBase::EGroundState::OnGround;
    e.airTime = grounded ? 0.0f : e.airTime + dt;
    // Coyote time: a jump pressed within 0.1 s of walking off an edge still fires, which is what players expect.
    const bool canJump = grounded || e.airTime <= 0.1f;
    if (!grounded) e.verticalVelocity += impl_->system.GetGravity().GetY() * dt;
    if (canJump && jump) {
        e.verticalVelocity = e.jumpSpeed;
        e.airTime = 1.0f;   // one jump per airtime; the coyote window is spent
    } else if (grounded) {
        e.verticalVelocity = std::min(e.verticalVelocity, 0.0f);   // landing clears the fall
    }

    JPH::Vec3 v(planar.x, e.verticalVelocity, planar.z);
    // A moving platform carries the character; add its velocity so standing still does not slide off.
    if (grounded) v += ch->GetGroundVelocity();
    ch->SetLinearVelocity(v);

    JPH::CharacterVirtual::ExtendedUpdateSettings us;
    us.mStickToFloorStepDown = JPH::Vec3(0.0f, -e.stickToFloor, 0.0f);
    us.mWalkStairsStepUp = JPH::Vec3(0.0f, e.stepHeight, 0.0f);
    ch->ExtendedUpdate(dt, impl_->system.GetGravity(), us, JPH::BroadPhaseLayerFilter(), JPH::ObjectLayerFilter(),
                       JPH::BodyFilter(), JPH::ShapeFilter(), impl_->temp);
    e.verticalVelocity = ch->GetLinearVelocity().GetY();

    if (out) *out = CharacterState{};
    if (out) characterState(h, out);
    return true;
}

bool PhysicsWorld::characterState(CharacterHandle h, CharacterState* out) const {
    const auto it = impl_->characters.find(h);
    if (it == impl_->characters.end() || !out) return false;
    const JPH::CharacterVirtual* ch = it->second.character;
    const JPH::RVec3 p = ch->GetPosition();
    out->position = glm::vec3(p.GetX(), p.GetY() - it->second.height * 0.5f, p.GetZ());
    const JPH::Vec3 v = ch->GetLinearVelocity();
    out->velocity = glm::vec3(v.GetX(), v.GetY(), v.GetZ());
    const auto state = ch->GetGroundState();
    out->grounded = state == JPH::CharacterBase::EGroundState::OnGround;
    out->onSteepGround = state == JPH::CharacterBase::EGroundState::OnSteepGround;
    const JPH::Vec3 n = ch->GetGroundNormal();
    out->groundNormal = glm::vec3(n.GetX(), n.GetY(), n.GetZ());
    return true;
}

void PhysicsWorld::setInterpolation(bool on) {
    impl_->interpolate = on;
    // Turning it on must not blend from poses that are old.
    for (auto& [h, pose] : impl_->prev) pose = readPose(impl_->system.GetBodyInterface(), h);
}

bool PhysicsWorld::interpolation() const { return impl_->interpolate; }

glm::vec3 PhysicsWorld::position(Handle h) const {
    const Impl::Pose cur = readPose(impl_->system.GetBodyInterface(), h);
    if (!impl_->interpolate) return cur.p;
    const auto it = impl_->prev.find(h);
    if (it == impl_->prev.end()) return cur.p;
    return glm::mix(it->second.p, cur.p, impl_->alpha);
}

glm::quat PhysicsWorld::rotation(Handle h) const {
    const Impl::Pose cur = readPose(impl_->system.GetBodyInterface(), h);
    if (!impl_->interpolate) return cur.r;
    const auto it = impl_->prev.find(h);
    if (it == impl_->prev.end()) return cur.r;
    return glm::normalize(glm::slerp(it->second.r, cur.r, impl_->alpha));
}

void PhysicsWorld::setPosition(Handle h, const glm::vec3& p) {
    if (impl_->defer) {
        impl_->deferred.push_back({Impl::Deferred::Op::SetPosition, h, p});
        return;
    }
    impl_->system.GetBodyInterface().SetPosition(JPH::BodyID(h), JPH::RVec3(p.x, p.y, p.z),
                                                 JPH::EActivation::Activate);
    // A moved body jumps. Its previous pose becomes the new one, so nothing is blended across the jump.
    const auto it = impl_->prev.find(h);
    if (it != impl_->prev.end()) it->second = readPose(impl_->system.GetBodyInterface(), h);
}

void PhysicsWorld::addImpulse(Handle h, const glm::vec3& impulse) {
    if (impl_->defer) {
        impl_->deferred.push_back({Impl::Deferred::Op::AddImpulse, h, impulse});
        return;
    }
    impl_->system.GetBodyInterface().AddImpulse(JPH::BodyID(h), toJolt(impulse));
}

bool PhysicsWorld::isDynamic(Handle h) const {
    return impl_->system.GetBodyInterface().GetMotionType(JPH::BodyID(h)) == JPH::EMotionType::Dynamic;
}

void PhysicsWorld::setDeferChanges(bool on) { impl_->defer = on; }

void PhysicsWorld::setPairEnabled(Handle a, Handle b, bool enabled) { impl_->contacts.setPairEnabled(a, b, enabled); }

std::vector<PhysicsWorld::Contact> PhysicsWorld::takeContacts() { return impl_->contacts.take(); }

// Runs the queued body changes. Deferral is off while they run, so they apply directly.
void PhysicsWorld::applyDeferred() {
    if (impl_->deferred.empty()) return;
    std::vector<Impl::Deferred> ops;
    ops.swap(impl_->deferred);
    const bool was = impl_->defer;
    impl_->defer = false;
    for (const Impl::Deferred& op : ops) {
        switch (op.op) {
        case Impl::Deferred::Op::SetPosition: setPosition(op.h, op.v); break;
        case Impl::Deferred::Op::AddImpulse: addImpulse(op.h, op.v); break;
        case Impl::Deferred::Op::Remove: removeBody(op.h); break;
        }
    }
    impl_->defer = was;
}

// Buoyancy (item 17). Half-speed drag against water at rest. The buoyancy factor comes from each body's density.
static constexpr float kLinearDrag = 0.5f;
static constexpr float kAngularDrag = 0.5f;

void PhysicsWorld::setWater(const WaterVolume* volume) {
    if (volume) impl_->water = *volume;
    else impl_->water.reset();
}

void PhysicsWorld::setWaterTime(float seconds) {
    impl_->waterTime = seconds;
}

// One buoyancy impulse per dynamic body inside the volume. Jolt takes one plane and computes the submerged part of
// the whole shape, so the four footprint samples only set that plane: their mean height and mean normal.
static void applyBuoyancy(PhysicsWorld::Impl& im, float dt) {
    if (!im.water) return;
    const WaterVolume& v = *im.water;
    JPH::BodyInterface& bi = im.system.GetBodyInterface();
    const JPH::Vec3 gravity = im.system.GetGravity();
    for (const auto& entry : im.prev) {
        const JPH::BodyID id(entry.first);
        if (!bi.IsAdded(id)) continue;
        const JPH::RVec3 c = bi.GetCenterOfMassPosition(id);
        if (!waterContainsXZ(v, float(c.GetX()), float(c.GetZ()))) continue;
        const JPH::Shape* shape = bi.GetShape(id).GetPtr();
        const JPH::AABox bounds = shape->GetLocalBounds();
        const JPH::Vec3 ext = bounds.GetExtent();
        // Buoyancy factor = water density / body density, with body density = mass / shape volume. Jolt uses this
        // factor as its 'buoyancy' (1 = neutral, above 1 floats). Clamped so a very light body cannot blow up.
        const auto mit = im.mass.find(entry.first);
        const float mass = mit != im.mass.end() && mit->second > 1e-6f ? mit->second : 1.0f;
        const float vol = shape->GetVolume();
        const float factor = vol > 1e-9f ? std::clamp(v.density * vol / mass, 0.0f, 4.0f) : 0.0f;
        float hSum = 0.0f;
        glm::vec3 nSum(0.0f);
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sz = -1; sz <= 1; sz += 2) {
                const float x = float(c.GetX()) + 0.5f * sx * ext.GetX();
                const float z = float(c.GetZ()) + 0.5f * sz * ext.GetZ();
                const WaterSurface s = waterSurface(v, x, z, im.waterTime);
                hSum += s.pos.y;
                nSum += s.normal;
            }
        const float surfaceY = 0.25f * hSum;
        const glm::vec3 n = glm::length(nSum) > 1e-6f ? glm::normalize(nSum) : glm::vec3(0.0f, 1.0f, 0.0f);
        bi.ApplyBuoyancyImpulse(id, JPH::RVec3(c.GetX(), surfaceY, c.GetZ()), JPH::Vec3(n.x, n.y, n.z), factor,
                                kLinearDrag, kAngularDrag, JPH::Vec3::sZero(), gravity, dt);
    }
}

void PhysicsWorld::step(float dt) {
    applyDeferred();
    const float kFixed = impl_->fixedStep;
    impl_->accumulator += dt;
    int guard = 0;
    while (impl_->accumulator >= kFixed && guard++ < 8) {
        // Record the pose the body had before this step. Only the last step's start is kept.
        JPH::BodyInterface& bi = impl_->system.GetBodyInterface();
        for (auto& [h, pose] : impl_->prev) pose = readPose(bi, h);
        applyBuoyancy(*impl_, kFixed);
        impl_->system.Update(kFixed, 1, &impl_->temp, &impl_->jobs);
        impl_->accumulator -= kFixed;
    }
    impl_->alpha = std::clamp(impl_->accumulator / kFixed, 0.0f, 1.0f);
}

bool PhysicsWorld::raycast(const glm::vec3& origin, const glm::vec3& direction, float maxDistance, glm::vec3* hit,
                           Handle* body) const {
    const glm::vec3 d = glm::normalize(direction) * maxDistance;
    JPH::RayCastResult result;
    JPH::RRayCast ray(JPH::RVec3(origin.x, origin.y, origin.z), toJolt(d));
    if (!impl_->system.GetNarrowPhaseQuery().CastRay(ray, result)) return false;
    if (hit) *hit = glm::vec3(origin) + glm::normalize(direction) * (result.mFraction * maxDistance);
    if (body) *body = result.mBodyID.GetIndexAndSequenceNumber();
    return true;
}

} // namespace ukiyo
