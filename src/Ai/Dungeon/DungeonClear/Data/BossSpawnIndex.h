/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_BOSSSPAWNINDEX_H
#define _PLAYERBOT_BOSSSPAWNINDEX_H

#include <mutex>
#include <unordered_map>
#include <vector>

#include "Common.h"
#include "DBCEnums.h"
#include "DungeonBossInfo.h"

class BossSpawnIndex
{
public:
    static std::vector<DungeonBossInfo> const& Get(uint32 mapId, Difficulty difficulty);

    // The bosses of `roster` whose waypoint patrol passes within
    // PatrolThreatRadius of `bossEntry`'s spot: alive, they walk into that
    // boss's fight, so the clear kills them first (Molten Core's Baron Geddon
    // patrols through Shazzrah's room). From spawn and waypoint data; cached
    // per (map, boss). Empty for a boss nothing patrols past.
    static std::vector<uint32> PatrolThreats(uint32 mapId, uint32 bossEntry,
                                             std::vector<DungeonBossInfo> const& roster);
    static constexpr float PatrolThreatRadius = 60.0f;

private:
    static void EnsureBuilt();
    static void Build();

    static std::unordered_map<uint64, std::vector<DungeonBossInfo>> _store;
    // Built on first Get(), which arrives from bot AI on map-update worker
    // threads: with MapUpdate.Threads > 1, bots in two instances can both be
    // first. call_once runs Build() exactly once and blocks the others until it
    // finishes; a bare bool let both threads fill _store at once (issue #34).
    static std::once_flag _once;

    static std::mutex _patrolLock;
    static std::unordered_map<uint64, std::vector<uint32>> _patrolThreats;

    static uint64 MakeKey(uint32 mapId, Difficulty difficulty)
    {
        return (static_cast<uint64>(mapId) << 32) | static_cast<uint32>(difficulty);
    }
};

#endif
