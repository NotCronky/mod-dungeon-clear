/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "BossSpawnIndex.h"

#include <algorithm>
#include <cmath>

#include "CreatureData.h"
#include "CreatureSpawnEntry.h"
#include "DBCStores.h"
#include "DBCStructure.h"
#include "MotionMaster.h"
#include "ObjectMgr.h"
#include "WaypointMgr.h"

std::unordered_map<uint64, std::vector<DungeonBossInfo>> BossSpawnIndex::_store;
std::once_flag BossSpawnIndex::_once;
std::mutex BossSpawnIndex::_patrolLock;
std::unordered_map<uint64, std::vector<uint32>> BossSpawnIndex::_patrolThreats;

namespace
{
    // 2D distance from (px,py) to the segment a-b.
    float SegmentDistance(float px, float py, float ax, float ay, float bx, float by)
    {
        float const dx = bx - ax;
        float const dy = by - ay;
        float const len2 = dx * dx + dy * dy;
        float t = len2 > 0.0f ? ((px - ax) * dx + (py - ay) * dy) / len2 : 0.0f;
        t = std::clamp(t, 0.0f, 1.0f);
        float const cx = ax + t * dx - px;
        float const cy = ay + t * dy - py;
        return std::sqrt(cx * cx + cy * cy);
    }
}

std::vector<uint32> BossSpawnIndex::PatrolThreats(uint32 mapId, uint32 bossEntry,
                                                  std::vector<DungeonBossInfo> const& roster)
{
    uint64 const key = (static_cast<uint64>(mapId) << 32) | bossEntry;
    {
        std::lock_guard<std::mutex> guard(_patrolLock);
        auto it = _patrolThreats.find(key);
        if (it != _patrolThreats.end())
            return it->second;
    }

    DungeonBossInfo const* target = nullptr;
    for (DungeonBossInfo const& b : roster)
        if (b.entry == bossEntry)
            target = &b;

    std::vector<uint32> threats;
    if (target)
    {
        for (auto const& [spawnId, data] : sObjectMgr->GetAllCreatureData())
        {
            if (data.mapid != mapId || data.movementType != WAYPOINT_MOTION_TYPE || data.id == bossEntry)
                continue;
            bool const onRoster = std::any_of(roster.begin(), roster.end(),
                                              [&](DungeonBossInfo const& b) { return b.entry == data.id; });
            if (!onRoster)
                continue;

            // The patrol path: creature_addon's path_id, or the spawn-id convention.
            uint32 pathId = spawnId * 10;
            if (CreatureAddon const* addon = sObjectMgr->GetCreatureAddon(spawnId))
                if (addon->path_id)
                    pathId = addon->path_id;
            WaypointPath const* path = sWaypointMgr->GetPath(pathId);
            if (!path || path->Nodes.empty())
                continue;

            bool crosses = false;
            std::vector<WaypointNode> const& nodes = path->Nodes;
            for (std::size_t i = 0; i < nodes.size() && !crosses; ++i)
            {
                WaypointNode const& a = nodes[i];
                WaypointNode const& b = nodes[(i + 1) % nodes.size()];
                crosses = SegmentDistance(target->x, target->y, a.X, a.Y, b.X, b.Y) <= PatrolThreatRadius;
            }
            if (crosses && std::find(threats.begin(), threats.end(), data.id) == threats.end())
                threats.push_back(data.id);
        }
    }

    std::lock_guard<std::mutex> guard(_patrolLock);
    _patrolThreats[key] = threats;
    return threats;
}

std::vector<DungeonBossInfo> const& BossSpawnIndex::Get(uint32 mapId, Difficulty difficulty)
{
    EnsureBuilt();
    static std::vector<DungeonBossInfo> const empty;
    auto it = _store.find(MakeKey(mapId, difficulty));
    if (it == _store.end())
        return empty;
    return it->second;
}

void BossSpawnIndex::EnsureBuilt()
{
    std::call_once(_once, &BossSpawnIndex::Build);
}

void BossSpawnIndex::Build()
{
    // 1. Build creditEntry -> encounter list, keyed by (mapId, difficulty).
    //    Only kill-creature encounters are usable here.
    struct EncounterRow
    {
        uint32 mapId;
        Difficulty difficulty;
        uint32 encounterIndex;
        std::string name;
        uint32 creditEntry;
    };

    std::unordered_multimap<uint32, EncounterRow> byCreditEntry;

    for (uint32 i = 0; i < sDungeonEncounterStore.GetNumRows(); ++i)
    {
        DungeonEncounterEntry const* dbc = sDungeonEncounterStore.LookupEntry(i);
        if (!dbc)
            continue;

        DungeonEncounterList const* list =
            sObjectMgr->GetDungeonEncounterList(dbc->mapId, Difficulty(dbc->difficulty));
        if (!list)
            continue;

        for (DungeonEncounter const* enc : *list)
        {
            if (!enc || enc->dbcEntry != dbc)
                continue;
            if (enc->creditType != ENCOUNTER_CREDIT_KILL_CREATURE)
                continue;

            EncounterRow row;
            row.mapId = dbc->mapId;
            row.difficulty = Difficulty(dbc->difficulty);
            row.encounterIndex = dbc->encounterIndex;
            row.name = dbc->encounterName[0] ? dbc->encounterName[0] : "";
            row.creditEntry = enc->creditEntry;
            byCreditEntry.emplace(row.creditEntry, std::move(row));
        }
    }

    if (byCreditEntry.empty())
        return;

    // 2. Walk every creature spawn once. For each spawn whose entry matches a
    //    boss credit entry on its mapId, record it.
    CreatureDataContainer const& spawns = sObjectMgr->GetAllCreatureData();
    for (auto const& kv : spawns)
    {
        CreatureData const& data = kv.second;
        uint32 const entry = DungeonClear::SpawnEntry(data);
        if (!entry)
            continue;

        auto range = byCreditEntry.equal_range(entry);
        for (auto it = range.first; it != range.second; ++it)
        {
            EncounterRow const& row = it->second;
            if (row.mapId != data.mapid)
                continue;

            // A spawn belongs to a difficulty bucket only when its spawnMask
            // bit says it exists there. Most 5-man spawns are mask 3 (both);
            // a heroic-only placement (Shattered Halls' Porung, mask 2) must
            // not leak coords into the normal bucket, nor a normal-only spawn
            // into heroic. Mask 0 is treated as "no filter" (defensive — some
            // custom rows leave it unset).
            if (data.spawnMask && !(data.spawnMask & (1u << row.difficulty)))
                continue;

            DungeonBossInfo info;
            info.entry = entry;
            info.encounterIndex = row.encounterIndex;
            info.name = row.name;
            info.mapId = row.mapId;
            info.x = data.posX;
            info.y = data.posY;
            info.z = data.posZ;
            _store[MakeKey(row.mapId, row.difficulty)].push_back(info);
        }
    }

    // 3. Sort each bucket by encounter index; deduplicate by entry (keep the
    //    first spawn — there is usually only one per dungeon).
    for (auto& kv : _store)
    {
        auto& v = kv.second;
        std::sort(v.begin(), v.end(),
                  [](DungeonBossInfo const& a, DungeonBossInfo const& b)
                  { return a.encounterIndex < b.encounterIndex; });

        std::vector<DungeonBossInfo> deduped;
        deduped.reserve(v.size());
        for (auto const& info : v)
        {
            bool seen = false;
            for (auto const& kept : deduped)
            {
                if (kept.entry == info.entry)
                {
                    seen = true;
                    break;
                }
            }
            if (!seen)
                deduped.push_back(info);
        }
        v = std::move(deduped);
    }
}
