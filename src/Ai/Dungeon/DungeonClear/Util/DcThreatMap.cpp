/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcThreatMap.h"

#include <unordered_set>

#include "Creature.h"
#include "CreatureData.h"
#include "CreatureGroups.h"
#include "GameTime.h"
#include "Map.h"
#include "Player.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "Timer.h"
#include "WaypointMgr.h"
#include "World.h"
#include "Ai/Dungeon/DungeonClear/Data/BossSpawnIndex.h"

std::mutex DcThreatMap::_lock;
std::unordered_map<uint64, std::unique_ptr<DcThreatMap>> DcThreatMap::_maps;
uint32 DcThreatMap::_lastSweepMs = 0;

DcThreatMap* DcThreatMap::Get(Map* map)
{
    if (!map || !map->IsDungeon())
        return nullptr;

    uint32 const now = getMSTime();
    uint64 const key = (static_cast<uint64>(map->GetId()) << 32) | map->GetInstanceId();
    uintptr_t const identity = reinterpret_cast<uintptr_t>(map);

    DcThreatMap* tm = nullptr;
    {
        std::lock_guard<std::mutex> guard(_lock);

        if (DcThreatMapKernel::RefreshDue(_lastSweepMs, now, 60 * 1000))
        {
            _lastSweepMs = now ? now : 1;
            for (auto it = _maps.begin(); it != _maps.end();)
            {
                if (it->first != key && static_cast<uint32>(now - it->second->_lastUsedMs) >= IdleDropMs)
                    it = _maps.erase(it);
                else
                    ++it;
            }
        }

        std::unique_ptr<DcThreatMap>& slot = _maps[key];
        if (!slot || slot->_mapIdentity != identity)
        {
            slot = std::make_unique<DcThreatMap>();
            slot->_mapIdentity = identity;
            slot->Build(map);
        }
        // Stamped under the lock: a sweep on another map's thread reads it.
        slot->_lastUsedMs = now ? now : 1;
        tm = slot.get();
    }

    // The entry itself is only ever touched from this instance's map thread.
    if (DcThreatMapKernel::RefreshDue(tm->_lastRefreshMs, now, RefreshIntervalMs))
        tm->Refresh(map, now);
    return tm;
}

void DcThreatMap::Build(Map* map)
{
    _entries.clear();
    _bySpawnId.clear();

    uint32 const mapId = map->GetId();
    uint32 const difficultyBit = 1u << map->GetSpawnMode();

    std::unordered_set<uint32> bossEntries;
    for (DungeonBossInfo const& b : BossSpawnIndex::Get(mapId, map->GetDifficulty()))
        bossEntries.insert(b.entry);

    std::vector<DcThreatMapKernel::SpawnPoint> points;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
    {
        if (data.mapid != mapId || !(data.spawnMask & difficultyBit))
            continue;

        DcThreatEntry e;
        e.spawnId = spawnId;
        e.entry = data.id;
        e.spawnX = e.x = data.posX;
        e.spawnY = e.y = data.posY;
        e.spawnZ = e.z = data.posZ;
        e.patrols = data.movementType == WAYPOINT_MOTION_TYPE;
        e.boss = bossEntries.count(data.id) != 0;
        if (e.patrols)
        {
            // creature_addon's path_id, or the spawn-id convention (as
            // BossSpawnIndex::PatrolThreats reads it).
            uint32 pathId = spawnId * 10;
            if (CreatureAddon const* addon = sObjectMgr->GetCreatureAddon(spawnId))
                if (addon->path_id)
                    pathId = addon->path_id;
            if (WaypointPath const* path = sWaypointMgr->GetPath(pathId))
                for (WaypointNode const& node : path->Nodes)
                    e.path.push_back({node.X, node.Y, node.Z});
            if (CreatureTemplate const* ct = sObjectMgr->GetCreatureTemplate(data.id))
                e.walkSpeed = ct->speed_walk * baseMoveSpeed[MOVE_WALK];
        }

        DcThreatMapKernel::SpawnPoint p{data.posX, data.posY, data.posZ, 0u};
        auto const formation = sFormationMgr->CreatureGroupMap.find(spawnId);
        if (formation != sFormationMgr->CreatureGroupMap.end())
            p.formationId = formation->second.leaderGUID;

        _bySpawnId[spawnId] = _entries.size();
        _entries.push_back(e);
        points.push_back(p);
    }

    float const linkYd = sWorld->getFloatConfig(CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS);
    std::vector<uint32> const packs = DcThreatMapKernel::GroupPacks(points, linkYd);
    for (std::size_t i = 0; i < _entries.size(); ++i)
        _entries[i].packId = packs[i];
}

void DcThreatMap::Refresh(Map* map, uint32 nowMs)
{
    uint32 const dtMs = _lastRefreshMs ? static_cast<uint32>(nowMs - _lastRefreshMs) : 0;
    _lastRefreshMs = nowMs ? nowMs : 1;
    time_t const gameNow = GameTime::GetGameTime().count();

    auto const& store = map->GetCreatureBySpawnIdStore();
    for (DcThreatEntry& e : _entries)
    {
        Creature* live = nullptr;
        auto const range = store.equal_range(e.spawnId);
        for (auto it = range.first; it != range.second; ++it)
        {
            // A respawning spawn can briefly have its corpse and the new body;
            // prefer the living one.
            if (!live || (!live->IsAlive() && it->second->IsAlive()))
                live = it->second;
        }

        if (!live)
        {
            e.loaded = false;
            e.vx = e.vy = 0.0f;
            e.inCombat = e.evading = false;
            // Unloaded, or killed and its corpse gone: the instance's respawn
            // timer tells them apart. A never-seen spawn in an unloaded grid
            // keeps its last known state (alive, at its spawn).
            time_t const respawnAt = map->GetCreatureRespawnTime(e.spawnId);
            if (respawnAt > gameNow)
                e.alive = false;
            else if (map->IsGridLoaded(e.spawnX, e.spawnY))
                e.alive = false;  // grid loaded, spawn absent: despawned or scripted out
            continue;
        }

        float const nx = live->GetPositionX();
        float const ny = live->GetPositionY();
        DcThreatMapKernel::Velocity const v =
            e.loaded ? DcThreatMapKernel::EstimateVelocity(e.x, e.y, nx, ny, dtMs) : DcThreatMapKernel::Velocity{};
        e.vx = v.vx;
        e.vy = v.vy;
        e.x = nx;
        e.y = ny;
        e.z = live->GetPositionZ();
        e.guid = live->GetGUID();
        e.loaded = true;
        e.alive = live->IsAlive();
        e.inCombat = live->IsInCombat();
        e.evading = live->IsInEvadeMode();
        if (e.patrols)
            e.walkSpeed = live->GetSpeed(MOVE_WALK);
    }
}

DcThreatEntry const* DcThreatMap::FindBySpawnId(ObjectGuid::LowType spawnId) const
{
    auto const it = _bySpawnId.find(spawnId);
    return it == _bySpawnId.end() ? nullptr : &_entries[it->second];
}

DcThreatEntry const* DcThreatMap::FindByGuid(ObjectGuid const& guid) const
{
    if (guid.IsEmpty())
        return nullptr;
    for (DcThreatEntry const& e : _entries)
        if (e.guid == guid)
            return &e;
    return nullptr;
}

std::vector<DcThreatEntry const*> DcThreatMap::AliveWithin(float x, float y, float z, float radius) const
{
    std::vector<DcThreatEntry const*> out;
    float const r2 = radius * radius;
    for (DcThreatEntry const& e : _entries)
    {
        if (!e.alive)
            continue;
        float const dx = e.x - x;
        float const dy = e.y - y;
        float const dz = e.z - z;
        if (dx * dx + dy * dy + dz * dz <= r2)
            out.push_back(&e);
    }
    return out;
}

std::vector<DcThreatEntry const*> DcThreatMap::PackMembers(uint32 packId) const
{
    std::vector<DcThreatEntry const*> out;
    if (!packId)
        return out;
    for (DcThreatEntry const& e : _entries)
        if (e.alive && e.packId == packId)
            out.push_back(&e);
    return out;
}

std::optional<DcPatrolArrival> DcThreatMap::NextPatrollingBoss(float x, float y, float z, float radius,
                                                                float horizonSec) const
{
    std::optional<DcPatrolArrival> soonest;
    for (DcThreatEntry const& e : _entries)
    {
        if (!e.alive || !e.boss || !e.patrols || e.inCombat || e.path.empty())
            continue;
        float const eta = DcThreatMapKernel::PatrolEtaSec(e.path, e.x, e.y, e.z, e.walkSpeed, x, y, z, radius,
                                                          BossSpawnIndex::PatrolThreatHeightBand, horizonSec,
                                                          e.vx, e.vy);
        if (eta == DcThreatMapKernel::kNever)
            continue;
        // Waiting only helps when the loop leaves a gap the fight fits in.
        float const window = DcThreatMapKernel::PatrolClearWindowSec(e.path, e.walkSpeed, x, y, z, radius,
                                                                    BossSpawnIndex::PatrolThreatHeightBand);
        if (window != DcThreatMapKernel::kNever && window < horizonSec)
            continue;
        if (!soonest || eta < soonest->etaSec)
            soonest = DcPatrolArrival{&e, eta};
    }
    return soonest;
}

DcThreatEntry const* DcThreatMap::NearestIdleHostile(Map* map, Player* bot, float x, float y, float z, float radius,
                                                     float heightBand, ObjectGuid const& exclude) const
{
    DcThreatEntry const* nearest = nullptr;
    float best = radius * radius;
    for (DcThreatEntry const& e : _entries)
    {
        if (!e.alive || e.inCombat || e.evading || (!exclude.IsEmpty() && e.guid == exclude))
            continue;
        if (std::fabs(e.z - z) > heightBand)
            continue;
        float const dx = e.x - x;
        float const dy = e.y - y;
        float const d2 = dx * dx + dy * dy;
        if (d2 > best)
            continue;
        if (e.loaded)
        {
            Creature const* c = map ? map->GetCreature(e.guid) : nullptr;
            if (!c || !c->IsAlive() || !bot || !c->IsHostileTo(bot))
                continue;
        }
        best = d2;
        nearest = &e;
    }
    return nearest;
}
