#include "systems/navigation.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "DetourCrowd.h"
#include "DetourNavMesh.h"
#include "DetourNavMeshBuilder.h"
#include "DetourNavMeshQuery.h"
#include "Recast.h"

namespace ukiyo {
namespace {

const float kExtents[3] = {2.0f, 4.0f, 2.0f};

using HeightfieldPtr = std::unique_ptr<rcHeightfield, decltype(&rcFreeHeightField)>;
using CompactPtr = std::unique_ptr<rcCompactHeightfield, decltype(&rcFreeCompactHeightfield)>;
using ContourPtr = std::unique_ptr<rcContourSet, decltype(&rcFreeContourSet)>;
using PolyMeshPtr = std::unique_ptr<rcPolyMesh, decltype(&rcFreePolyMesh)>;
using DetailPtr = std::unique_ptr<rcPolyMeshDetail, decltype(&rcFreePolyMeshDetail)>;

} // namespace

NavMesh::NavMesh()
    : nav_(nullptr, &dtFreeNavMesh), query_(nullptr, &dtFreeNavMeshQuery), crowd_(nullptr, &dtFreeCrowd) {}

NavMesh::~NavMesh() = default;

int NavMesh::polyCount() const { return polys_; }

bool NavMesh::build(const std::vector<glm::vec3>& vertices, const std::vector<uint32_t>& indices,
                    const Settings& s, std::atomic<int>* progress) {
    crowd_.reset();
    query_.reset();
    nav_.reset();
    agents_.clear();
    polys_ = 0;
    settings_ = s;
    if (vertices.empty() || indices.size() < 3) return false;

    std::vector<float> verts(vertices.size() * 3);
    for (size_t i = 0; i < vertices.size(); ++i) {
        verts[i * 3 + 0] = vertices[i].x;
        verts[i * 3 + 1] = vertices[i].y;
        verts[i * 3 + 2] = vertices[i].z;
    }
    const int ntris = int(indices.size() / 3);
    std::vector<int> tris(ntris * 3);
    for (int i = 0; i < ntris * 3; ++i) tris[i] = int(indices[i]);

    rcConfig cfg{};
    cfg.cs = s.cellSize;
    cfg.ch = s.cellHeight;
    cfg.walkableSlopeAngle = s.maxSlopeDeg;
    cfg.walkableHeight = int(std::ceil(s.agentHeight / cfg.ch));
    cfg.walkableClimb = int(std::floor(s.maxClimb / cfg.ch));
    cfg.walkableRadius = int(std::ceil(s.agentRadius / cfg.cs));
    cfg.maxEdgeLen = int(12.0f / cfg.cs);
    cfg.maxSimplificationError = 1.3f;
    cfg.minRegionArea = 8 * 8;
    cfg.mergeRegionArea = 20 * 20;
    cfg.maxVertsPerPoly = 6;
    cfg.detailSampleDist = cfg.cs * 6.0f;
    cfg.detailSampleMaxError = cfg.ch;
    cfg.bmin[0] = cfg.bmax[0] = vertices[0].x;
    cfg.bmin[1] = cfg.bmax[1] = vertices[0].y;
    cfg.bmin[2] = cfg.bmax[2] = vertices[0].z;
    for (const glm::vec3& v : vertices) {
        cfg.bmin[0] = std::min(cfg.bmin[0], v.x);
        cfg.bmin[1] = std::min(cfg.bmin[1], v.y);
        cfg.bmin[2] = std::min(cfg.bmin[2], v.z);
        cfg.bmax[0] = std::max(cfg.bmax[0], v.x);
        cfg.bmax[1] = std::max(cfg.bmax[1], v.y);
        cfg.bmax[2] = std::max(cfg.bmax[2], v.z);
    }
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

    rcContext ctx;
    HeightfieldPtr hf(rcAllocHeightfield(), &rcFreeHeightField);
    if (progress) progress->store(1);
    if (!hf || !rcCreateHeightfield(&ctx, *hf, cfg.width, cfg.height, cfg.bmin, cfg.bmax, cfg.cs, cfg.ch)) return false;

    std::vector<unsigned char> areas(ntris, 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle, verts.data(), int(vertices.size()), tris.data(), ntris,
                            areas.data());
    if (progress) progress->store(2);
    rcRasterizeTriangles(&ctx, verts.data(), int(vertices.size()), tris.data(), areas.data(), ntris, *hf,
                         cfg.walkableClimb);
    if (progress) progress->store(3);
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *hf);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *hf);

    CompactPtr chf(rcAllocCompactHeightfield(), &rcFreeCompactHeightfield);
    if (progress) progress->store(4);
    if (!chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *hf, *chf)) return false;
    if (progress) progress->store(5);
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf)) return false;
    if (!rcBuildDistanceField(&ctx, *chf)) return false;
    if (progress) progress->store(6);
    if (!rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea)) return false;

    ContourPtr cset(rcAllocContourSet(), &rcFreeContourSet);
    if (progress) progress->store(7);
    if (!cset || !rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset)) return false;
    PolyMeshPtr pmesh(rcAllocPolyMesh(), &rcFreePolyMesh);
    if (progress) progress->store(8);
    if (!pmesh || !rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh)) return false;
    DetailPtr dmesh(rcAllocPolyMeshDetail(), &rcFreePolyMeshDetail);
    if (!dmesh || !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf, cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh))
        return false;
    if (pmesh->npolys == 0) return false;

    for (int i = 0; i < pmesh->npolys; ++i) {
        if (pmesh->areas[i] == RC_WALKABLE_AREA) pmesh->areas[i] = 0;
        pmesh->flags[i] = 1;   // one flag: walkable
    }

    dtNavMeshCreateParams params;
    std::memset(&params, 0, sizeof(params));
    params.verts = pmesh->verts;
    params.vertCount = pmesh->nverts;
    params.polys = pmesh->polys;
    params.polyAreas = pmesh->areas;
    params.polyFlags = pmesh->flags;
    params.polyCount = pmesh->npolys;
    params.nvp = pmesh->nvp;
    params.detailMeshes = dmesh->meshes;
    params.detailVerts = dmesh->verts;
    params.detailVertsCount = dmesh->nverts;
    params.detailTris = dmesh->tris;
    params.detailTriCount = dmesh->ntris;
    params.walkableHeight = float(cfg.walkableHeight) * cfg.ch;
    params.walkableRadius = float(cfg.walkableRadius) * cfg.cs;
    params.walkableClimb = float(cfg.walkableClimb) * cfg.ch;
    std::memcpy(params.bmin, pmesh->bmin, sizeof(params.bmin));
    std::memcpy(params.bmax, pmesh->bmax, sizeof(params.bmax));
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;

    unsigned char* navData = nullptr;
    int navDataSize = 0;
    if (!dtCreateNavMeshData(&params, &navData, &navDataSize)) return false;

    dtNavMesh* nav = dtAllocNavMesh();
    if (!nav || dtStatusFailed(nav->init(navData, navDataSize, DT_TILE_FREE_DATA))) {
        dtFree(navData);
        dtFreeNavMesh(nav);
        return false;
    }
    nav_.reset(nav);
    polys_ = pmesh->npolys;

    dtNavMeshQuery* query = dtAllocNavMeshQuery();
    if (!query || dtStatusFailed(query->init(nav_.get(), 2048))) {
        dtFreeNavMeshQuery(query);
        nav_.reset();
        polys_ = 0;
        return false;
    }
    query_.reset(query);

    dtCrowd* crowd = dtAllocCrowd();
    if (!crowd || !crowd->init(256, s.agentRadius, nav_.get())) {
        dtFreeCrowd(crowd);
        query_.reset();
        nav_.reset();
        polys_ = 0;
        return false;
    }
    crowd_.reset(crowd);
    return true;
}

bool NavMesh::findPath(const glm::vec3& from, const glm::vec3& to, std::vector<glm::vec3>* path) const {
    if (!query_ || !path) return false;
    dtQueryFilter filter;
    filter.setIncludeFlags(0xffff);
    filter.setExcludeFlags(0);
    dtPolyRef startRef = 0, endRef = 0;
    float start[3] = {from.x, from.y, from.z};
    float end[3] = {to.x, to.y, to.z};
    float nearestStart[3], nearestEnd[3];
    if (dtStatusFailed(query_->findNearestPoly(start, kExtents, &filter, &startRef, nearestStart))) return false;
    if (dtStatusFailed(query_->findNearestPoly(end, kExtents, &filter, &endRef, nearestEnd))) return false;
    if (!startRef || !endRef) return false;

    static constexpr int kMaxPoly = 256;
    dtPolyRef polys[kMaxPoly];
    int npolys = 0;
    if (dtStatusFailed(query_->findPath(startRef, endRef, nearestStart, nearestEnd, &filter, polys, &npolys, kMaxPoly)))
        return false;
    if (npolys == 0) return false;

    static constexpr int kMaxStraight = 256;
    float straight[kMaxStraight * 3];
    unsigned char flags[kMaxStraight];
    dtPolyRef refs[kMaxStraight];
    int nstraight = 0;
    if (dtStatusFailed(query_->findStraightPath(nearestStart, nearestEnd, polys, npolys, straight, flags, refs,
                                                &nstraight, kMaxStraight, 0)))
        return false;
    path->clear();
    for (int i = 0; i < nstraight; ++i)
        path->push_back(glm::vec3(straight[i * 3], straight[i * 3 + 1], straight[i * 3 + 2]));
    return !path->empty();
}

int NavMesh::addAgent(const glm::vec3& position, float maxSpeed) {
    if (!crowd_) return -1;
    dtCrowdAgentParams ap;
    std::memset(&ap, 0, sizeof(ap));
    ap.radius = settings_.agentRadius;
    ap.height = settings_.agentHeight;
    ap.maxAcceleration = 8.0f;
    ap.maxSpeed = maxSpeed;
    ap.collisionQueryRange = ap.radius * 12.0f;
    ap.pathOptimizationRange = ap.radius * 30.0f;
    ap.separationWeight = 2.0f;
    ap.updateFlags = DT_CROWD_ANTICIPATE_TURNS | DT_CROWD_OPTIMIZE_VIS | DT_CROWD_OPTIMIZE_TOPO |
                     DT_CROWD_OBSTACLE_AVOIDANCE | DT_CROWD_SEPARATION;
    float p[3] = {position.x, position.y, position.z};
    float nearest[3];
    dtPolyRef ref = 0;
    dtQueryFilter filter;
    filter.setIncludeFlags(0xffff);
    if (!query_ || dtStatusFailed(query_->findNearestPoly(p, kExtents, &filter, &ref, nearest)) || !ref) return -1;
    const int slot = crowd_->addAgent(nearest, &ap);
    if (slot < 0) return -1;
    agents_.push_back(slot);
    return int(agents_.size()) - 1;
}

bool NavMesh::moveAgent(int id, const glm::vec3& target) {
    if (!crowd_ || !query_ || id < 0 || id >= int(agents_.size()) || agents_[id] < 0) return false;
    float t[3] = {target.x, target.y, target.z};
    float nearest[3];
    dtPolyRef ref = 0;
    dtQueryFilter filter;
    filter.setIncludeFlags(0xffff);
    if (dtStatusFailed(query_->findNearestPoly(t, kExtents, &filter, &ref, nearest)) || !ref) return false;
    // This Detour version returns a plain bool from requestMoveTarget, not a dtStatus.
    return crowd_->requestMoveTarget(agents_[id], ref, nearest);
}

void NavMesh::removeAgent(int id) {
    if (!crowd_ || id < 0 || id >= int(agents_.size()) || agents_[id] < 0) return;
    crowd_->removeAgent(agents_[id]);
    agents_[id] = -1;
}

void NavMesh::update(float dt) {
    if (crowd_) crowd_->update(dt, nullptr);
}

glm::vec3 NavMesh::agentPosition(int id) const {
    if (!crowd_ || id < 0 || id >= int(agents_.size()) || agents_[id] < 0) return glm::vec3(0.0f);
    const dtCrowdAgent* a = crowd_->getAgent(agents_[id]);
    return a ? glm::vec3(a->npos[0], a->npos[1], a->npos[2]) : glm::vec3(0.0f);
}

glm::vec3 NavMesh::agentVelocity(int id) const {
    if (!crowd_ || id < 0 || id >= int(agents_.size()) || agents_[id] < 0) return glm::vec3(0.0f);
    const dtCrowdAgent* a = crowd_->getAgent(agents_[id]);
    return a ? glm::vec3(a->vel[0], a->vel[1], a->vel[2]) : glm::vec3(0.0f);
}

bool NavMesh::agentArrived(int id) const {
    if (!crowd_ || id < 0 || id >= int(agents_.size()) || agents_[id] < 0) return false;
    const dtCrowdAgent* a = crowd_->getAgent(agents_[id]);
    if (!a || a->targetState != DT_CROWDAGENT_TARGET_VALID) return false;
    // Detour has no "arrived" state. An agent with a valid target is at it once it is close enough.
    const float dx = a->npos[0] - a->targetPos[0];
    const float dz = a->npos[2] - a->targetPos[2];
    return std::sqrt(dx * dx + dz * dz) < 0.6f;
}

} // namespace ukiyo
