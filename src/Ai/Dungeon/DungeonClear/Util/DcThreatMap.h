/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCTHREATMAP_H
#define _PLAYERBOT_DCTHREATMAP_H

#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "Define.h"
#include "ObjectGuid.h"
#include "DcThreatMapKernel.h"

class Map;
class Player;

// Every DB-spawned creature of one instance, with its live state.
//
// The clear's other views of the world are scans centred on a bot: what the
// tank can see, inside its visibility range, minus anything the attack-target
// filter drops (an evading mob is damage-immune, so it vanishes from them while
// it evades). Pull planning, the room pre-clear and pack avoidance all asked
// those scans "what is near X?" for an X that is not the bot, and got answers
// that changed from one tick to the next with the tank's position and with the
// mobs' state rather than with the world.
//
// The threat map answers from the instance itself. It is built once per
// instance from the static spawn data (so a creature in an unloaded grid is
// still there, at its spawn) and refreshed from the map's spawn-id store at
// most every RefreshIntervalMs: live position, alive, in combat, evading,
// velocity. Pack ids come from formations plus spawn proximity
// (DcThreatMapKernel::GroupPacks), fixed for the instance's life.
//
// Threading: an instance's bots all update on that instance's map thread, so an
// entry is never touched by two threads at once; the registry itself is shared
// across maps and is locked. The world thread (chat commands, the test-run job)
// may call it too: it never runs while the map threads update. Never call it
// from the async path worker.
//
// Summoned creatures (no spawn id) are not tracked: they are scripted adds that
// are in combat when they matter, and the existing combat views cover them.
struct DcThreatEntry
{
    ObjectGuid::LowType spawnId{0};
    uint32 entry{0};
    float spawnX{0.0f};
    float spawnY{0.0f};
    float spawnZ{0.0f};
    // Live position while the grid is loaded, else the last known (initially
    // the spawn point).
    float x{0.0f};
    float y{0.0f};
    float z{0.0f};
    // Yards per second over the last refresh; 0 when not loaded.
    float vx{0.0f};
    float vy{0.0f};
    uint32 packId{0};
    ObjectGuid guid;          // empty until first seen loaded
    bool loaded{false};       // in a loaded grid at the last refresh
    bool alive{true};
    bool inCombat{false};
    bool evading{false};
    bool patrols{false};      // waypoint movement in its spawn data
    bool boss{false};         // an encounter boss of this instance
    // Patrollers only: the waypoint loop and walk speed (yd/s, live when loaded).
    std::vector<DcThreatMapKernel::PathPoint> path;
    float walkSpeed{0.0f};
};

struct DcPatrolArrival
{
    DcThreatEntry const* who{nullptr};
    float etaSec{0.0f};
};

class DcThreatMap
{
public:
    static constexpr uint32 RefreshIntervalMs = 500;

    // Maps no bot has asked for in this long are dropped (instance unloaded,
    // run over). Swept from Get, under the registry lock.
    static constexpr uint32 IdleDropMs = 10 * 60 * 1000;

    // The instance's threat map, built on first use and refreshed if due.
    // Null for a map that is not a dungeon or raid instance. The pointer is for
    // the current tick only.
    static DcThreatMap* Get(Map* map);

    std::vector<DcThreatEntry> const& Entries() const { return _entries; }
    DcThreatEntry const* FindBySpawnId(ObjectGuid::LowType spawnId) const;
    DcThreatEntry const* FindByGuid(ObjectGuid const& guid) const;

    // Living entries within `radius` (3D) of the point, in entry order.
    std::vector<DcThreatEntry const*> AliveWithin(float x, float y, float z, float radius) const;

    // Living entries sharing `packId`.
    std::vector<DcThreatEntry const*> PackMembers(uint32 packId) const;

    // The patrolling boss, not yet in combat, that its loop brings within
    // `radius` of the point soonest, if one does within `horizonSec`. A boss
    // whose loop never leaves a `horizonSec` gap outside that radius is left
    // out: there is no time to wait for.
    std::optional<DcPatrolArrival> NextPatrollingBoss(float x, float y, float z, float radius,
                                                       float horizonSec) const;

    // The nearest idle mob hostile to `bot` within `radius` (2D) and `heightBand`
    // of the point, other than `exclude`: alive, not in combat, not evading. A
    // creature in an unloaded grid counts, at its last known position (nobody
    // has killed it). Other bosses count too. Null when the ground is quiet.
    DcThreatEntry const* NearestIdleHostile(Map* map, Player* bot, float x, float y, float z, float radius,
                                            float heightBand, ObjectGuid const& exclude) const;

    uint32 LastRefreshMs() const { return _lastRefreshMs; }

private:
    void Build(Map* map);
    void Refresh(Map* map, uint32 nowMs);

    std::vector<DcThreatEntry> _entries;
    std::unordered_map<ObjectGuid::LowType, std::size_t> _bySpawnId;
    // Identity only, never dereferenced: an instance id reused by a new
    // instance must not inherit the old one's state.
    uintptr_t _mapIdentity{0};
    uint32 _lastRefreshMs{0};
    uint32 _lastUsedMs{0};

    static std::mutex _lock;
    static std::unordered_map<uint64, std::unique_ptr<DcThreatMap>> _maps;
    static uint32 _lastSweepMs;
};

#endif  // _PLAYERBOT_DCTHREATMAP_H
