#include "physics/physics_world.h"

#include "core/log.h"

#include <Jolt/Jolt.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

JPH_SUPPRESS_WARNINGS

namespace vox {
namespace {

namespace Layers {
constexpr JPH::ObjectLayer NON_MOVING = 0;
constexpr JPH::ObjectLayer MOVING = 1;
constexpr JPH::ObjectLayer NUM_LAYERS = 2;
} // namespace Layers

namespace BPLayers {
constexpr JPH::BroadPhaseLayer NON_MOVING(0);
constexpr JPH::BroadPhaseLayer MOVING(1);
constexpr JPH::uint NUM_LAYERS = 2;
} // namespace BPLayers

class BPLayerInterfaceImpl final : public JPH::BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        map_[Layers::NON_MOVING] = BPLayers::NON_MOVING;
        map_[Layers::MOVING] = BPLayers::MOVING;
    }

    JPH::uint GetNumBroadPhaseLayers() const override { return BPLayers::NUM_LAYERS; }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(JPH::ObjectLayer layer) const override {
        return map_[layer];
    }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    const char* GetBroadPhaseLayerName(JPH::BroadPhaseLayer layer) const override {
        return layer == BPLayers::NON_MOVING ? "NON_MOVING" : "MOVING";
    }
#endif

private:
    JPH::BroadPhaseLayer map_[Layers::NUM_LAYERS]{};
};

class ObjectVsBroadPhaseLayerFilterImpl final : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer, JPH::BroadPhaseLayer broad_phase) const override {
        return layer == Layers::MOVING || broad_phase == BPLayers::MOVING;
    }
};

class ObjectLayerPairFilterImpl final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer a, JPH::ObjectLayer b) const override {
        return a == Layers::MOVING || b == Layers::MOVING;
    }
};

void initialize_jolt_globally() {
    static std::once_flag flag;
    std::call_once(flag, [] {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    });
}

} // namespace

struct PhysicsWorld::Impl {
    Impl()
        : job_system(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
                     std::max(1u, std::thread::hardware_concurrency() > 1
                                      ? std::thread::hardware_concurrency() - 1
                                      : 1)) {
        physics.Init(65536, 0, 65536, 10240, bp_layer_interface,
                     object_vs_broadphase, object_pair_filter);
        physics.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
    }

    ~Impl() {
        characters.clear();
        JPH::BodyInterface& bodies = physics.GetBodyInterface();
        for (const auto& [coord, body] : static_chunks) {
            (void)coord;
            bodies.RemoveBody(body);
            bodies.DestroyBody(body);
        }
        for (const JPH::BodyID body : dynamic_bodies) {
            if (!body.IsInvalid()) {
                bodies.RemoveBody(body);
                bodies.DestroyBody(body);
            }
        }
    }

    BPLayerInterfaceImpl bp_layer_interface;
    ObjectVsBroadPhaseLayerFilterImpl object_vs_broadphase;
    ObjectLayerPairFilterImpl object_pair_filter;
    JPH::TempAllocatorImpl temp_allocator{10 * 1024 * 1024};
    JPH::JobSystemThreadPool job_system;
    JPH::PhysicsSystem physics;
    std::unordered_map<ChunkCoord, JPH::BodyID, ChunkCoordHash> static_chunks;
    std::vector<JPH::BodyID> dynamic_bodies;
    std::vector<JPH::Ref<JPH::CharacterVirtual>> characters;
    bool initialized = false;
};

PhysicsWorld::PhysicsWorld() {
    initialize_jolt_globally();
    impl_ = std::make_unique<Impl>();
}

PhysicsWorld::~PhysicsWorld() {
    shutdown();
}

bool PhysicsWorld::initialize() {
    initialize_jolt_globally();
    if (!impl_) {
        impl_ = std::make_unique<Impl>();
    }
    impl_->initialized = true;
    return true;
}

void PhysicsWorld::shutdown() {
    impl_.reset();
}

bool PhysicsWorld::running() const noexcept {
    return impl_ && impl_->initialized;
}

void PhysicsWorld::add_static_triangles(ChunkCoord coord,
                                        std::span<const float> positions_xyz,
                                        std::span<const std::uint32_t> indices) {
    if (!impl_ || positions_xyz.size() % 3 != 0 || indices.size() % 3 != 0 ||
        indices.empty()) {
        return;
    }

    remove_static_chunk(coord);

    const std::size_t vertex_count = positions_xyz.size() / 3;
    JPH::TriangleList triangles;
    triangles.reserve(indices.size() / 3);
    for (std::size_t i = 0; i + 2 < indices.size(); i += 3) {
        if (indices[i] >= vertex_count || indices[i + 1] >= vertex_count ||
            indices[i + 2] >= vertex_count) {
            return;
        }
        const auto vertex = [&](std::uint32_t index) {
            return JPH::Vec3(positions_xyz[index * 3 + 0],
                             positions_xyz[index * 3 + 1],
                             positions_xyz[index * 3 + 2]);
        };
        triangles.emplace_back(vertex(indices[i]), vertex(indices[i + 1]),
                               vertex(indices[i + 2]));
    }

    JPH::MeshShapeSettings settings(triangles);
    settings.SetEmbedded();
    JPH::ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) {
        VOX_LOG_ERROR("Jolt MeshShape failed for chunk {},{}: {}", coord.x, coord.z,
                      result.GetError().c_str());
        return;
    }

    JPH::BodyCreationSettings body_settings(result.Get(), JPH::RVec3::sZero(),
                                            JPH::Quat::sIdentity(),
                                            JPH::EMotionType::Static, Layers::NON_MOVING);
    body_settings.mFriction = 0.8f;
    JPH::BodyID body = impl_->physics.GetBodyInterface().CreateAndAddBody(
        body_settings, JPH::EActivation::DontActivate);
    if (!body.IsInvalid()) {
        impl_->static_chunks[coord] = body;
    }
}

void PhysicsWorld::add_static_chunk(ChunkCoord coord, const ChunkMesh& mesh) {
    if (!impl_) {
        return;
    }
    if (mesh.empty() || mesh.vertices.empty() || mesh.indices.empty()) {
        remove_static_chunk(coord);
        return;
    }

    std::vector<float> positions;
    positions.reserve(mesh.vertices.size() * 3);
    for (const MeshVertex& vertex : mesh.vertices) {
        positions.push_back(vertex.x);
        positions.push_back(vertex.y);
        positions.push_back(vertex.z);
    }
    add_static_triangles(coord, positions, mesh.indices);
}

void PhysicsWorld::optimize_broadphase() {
    if (impl_) {
        impl_->physics.OptimizeBroadPhase();
    }
}

void PhysicsWorld::remove_static_chunk(ChunkCoord coord) {
    if (!impl_) {
        return;
    }
    const auto it = impl_->static_chunks.find(coord);
    if (it == impl_->static_chunks.end()) {
        return;
    }
    JPH::BodyInterface& bodies = impl_->physics.GetBodyInterface();
    bodies.RemoveBody(it->second);
    bodies.DestroyBody(it->second);
    impl_->static_chunks.erase(it);
}

PhysicsBody PhysicsWorld::add_box(Vec3 position, Vec3 half_extents, bool dynamic) {
    if (!impl_) {
        return kInvalidPhysicsBody;
    }

    JPH::BoxShapeSettings shape_settings(
        JPH::Vec3(half_extents.x, half_extents.y, half_extents.z));
    shape_settings.SetEmbedded();
    JPH::ShapeSettings::ShapeResult result = shape_settings.Create();
    if (result.HasError()) {
        return kInvalidPhysicsBody;
    }

    JPH::BodyCreationSettings body_settings(
        result.Get(), JPH::RVec3(position.x, position.y, position.z),
        JPH::Quat::sIdentity(),
        dynamic ? JPH::EMotionType::Dynamic : JPH::EMotionType::Static,
        dynamic ? Layers::MOVING : Layers::NON_MOVING);
    body_settings.mFriction = 0.6f;
    body_settings.mRestitution = 0.1f;

    JPH::BodyID body = impl_->physics.GetBodyInterface().CreateAndAddBody(
        body_settings, JPH::EActivation::Activate);
    if (body.IsInvalid()) {
        return kInvalidPhysicsBody;
    }
    impl_->dynamic_bodies.push_back(body);
    return static_cast<PhysicsBody>(impl_->dynamic_bodies.size());
}

void PhysicsWorld::remove_body(PhysicsBody body_handle) {
    if (!impl_ || body_handle == kInvalidPhysicsBody ||
        body_handle > impl_->dynamic_bodies.size()) {
        return;
    }
    JPH::BodyID& body = impl_->dynamic_bodies[body_handle - 1];
    if (body.IsInvalid()) {
        return;
    }
    JPH::BodyInterface& bodies = impl_->physics.GetBodyInterface();
    bodies.RemoveBody(body);
    bodies.DestroyBody(body);
    body = JPH::BodyID();
}

Vec3 PhysicsWorld::body_position(PhysicsBody body_handle) const {
    if (!impl_ || body_handle == kInvalidPhysicsBody ||
        body_handle > impl_->dynamic_bodies.size()) {
        return {};
    }
    const JPH::BodyID body = impl_->dynamic_bodies[body_handle - 1];
    if (body.IsInvalid()) {
        return {};
    }
    const JPH::RVec3 position = impl_->physics.GetBodyInterface().GetPosition(body);
    return Vec3(static_cast<float>(position.GetX()),
                static_cast<float>(position.GetY()),
                static_cast<float>(position.GetZ()));
}

PhysicsCharacter PhysicsWorld::create_character(Vec3 feet) {
    if (!impl_) {
        return kInvalidPhysicsCharacter;
    }

    JPH::CharacterVirtualSettings settings;
    settings.mShape = new JPH::CapsuleShape(0.6f, 0.3f);
    settings.mShapeOffset = JPH::Vec3(0.0f, 0.9f, 0.0f);
    settings.mMaxSlopeAngle = JPH::DegreesToRadians(50.0f);
    settings.mSupportingVolume = JPH::Plane(JPH::Vec3::sAxisY(), -0.01f);

    auto character = JPH::Ref<JPH::CharacterVirtual>(new JPH::CharacterVirtual(
        &settings, JPH::RVec3(feet.x, feet.y, feet.z), JPH::Quat::sIdentity(),
        &impl_->physics));
    impl_->characters.push_back(std::move(character));
    return static_cast<PhysicsCharacter>(impl_->characters.size());
}

void PhysicsWorld::remove_character(PhysicsCharacter character_handle) {
    if (!impl_ || character_handle == kInvalidPhysicsCharacter ||
        character_handle > impl_->characters.size()) {
        return;
    }
    impl_->characters[character_handle - 1] = nullptr;
}

void PhysicsWorld::step_character(PhysicsCharacter character_handle,
                                  Vec3 horizontal_velocity, bool jump,
                                  Vec3 gravity, float dt) {
    if (!impl_ || character_handle == kInvalidPhysicsCharacter ||
        character_handle > impl_->characters.size()) {
        return;
    }
    auto& character = impl_->characters[character_handle - 1];
    if (character == nullptr) {
        return;
    }

    JPH::Vec3 velocity = character->GetLinearVelocity();
    if (jump && character->IsSupported()) {
        velocity.SetY(8.2f);
    } else {
        velocity.SetY(velocity.GetY() + gravity.y * std::clamp(dt, 0.0f, 0.1f));
    }
    velocity.SetX(horizontal_velocity.x);
    velocity.SetZ(horizontal_velocity.z);
    character->SetLinearVelocity(velocity);

    const auto broadphase_filter =
        impl_->physics.GetDefaultBroadPhaseLayerFilter(Layers::MOVING);
    const auto layer_filter = impl_->physics.GetDefaultLayerFilter(Layers::MOVING);

    character->ExtendedUpdate(std::clamp(dt, 0.0f, 0.1f),
                              JPH::Vec3(gravity.x, gravity.y, gravity.z),
                              JPH::CharacterVirtual::ExtendedUpdateSettings{},
                              broadphase_filter, layer_filter, JPH::BodyFilter{},
                              JPH::ShapeFilter{}, impl_->temp_allocator);
}

void PhysicsWorld::set_character_position(PhysicsCharacter character_handle, Vec3 feet) {
    if (!impl_ || character_handle == kInvalidPhysicsCharacter ||
        character_handle > impl_->characters.size()) {
        return;
    }
    auto& character = impl_->characters[character_handle - 1];
    if (character != nullptr) {
        character->SetPosition(JPH::RVec3(feet.x, feet.y, feet.z));
        character->SetLinearVelocity(JPH::Vec3::sZero());
    }
}

void PhysicsWorld::set_character_velocity(PhysicsCharacter character_handle, Vec3 velocity) {
    if (!impl_ || character_handle == kInvalidPhysicsCharacter ||
        character_handle > impl_->characters.size()) {
        return;
    }
    auto& character = impl_->characters[character_handle - 1];
    if (character != nullptr) {
        character->SetLinearVelocity(JPH::Vec3(velocity.x, velocity.y, velocity.z));
    }
}

Vec3 PhysicsWorld::character_position(PhysicsCharacter character_handle) const {
    if (!impl_ || character_handle == kInvalidPhysicsCharacter ||
        character_handle > impl_->characters.size()) {
        return {};
    }
    const auto& character = impl_->characters[character_handle - 1];
    if (character == nullptr) {
        return {};
    }
    const JPH::RVec3 position = character->GetPosition();
    return Vec3(static_cast<float>(position.GetX()),
                static_cast<float>(position.GetY()),
                static_cast<float>(position.GetZ()));
}

Vec3 PhysicsWorld::character_velocity(PhysicsCharacter character_handle) const {
    if (!impl_ || character_handle == kInvalidPhysicsCharacter ||
        character_handle > impl_->characters.size()) {
        return {};
    }
    const auto& character = impl_->characters[character_handle - 1];
    if (character == nullptr) {
        return {};
    }
    const JPH::Vec3 velocity = character->GetLinearVelocity();
    return Vec3(velocity.GetX(), velocity.GetY(), velocity.GetZ());
}

bool PhysicsWorld::character_on_ground(PhysicsCharacter character_handle) const {
    if (!impl_ || character_handle == kInvalidPhysicsCharacter ||
        character_handle > impl_->characters.size()) {
        return false;
    }
    const auto& character = impl_->characters[character_handle - 1];
    return character != nullptr && character->IsSupported();
}

void PhysicsWorld::step(float dt) {
    if (!impl_ || !impl_->initialized) {
        return;
    }
    const float step = std::clamp(dt, 0.0f, 0.1f);
    impl_->physics.Update(step, 1, &impl_->temp_allocator, &impl_->job_system);
}

} // namespace vox
