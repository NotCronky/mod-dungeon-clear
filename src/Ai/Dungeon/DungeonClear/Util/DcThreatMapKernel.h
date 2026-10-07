/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCTHREATMAPKERNEL_H
#define _PLAYERBOT_DCTHREATMAPKERNEL_H

#include <cmath>
#include <numeric>
#include <vector>

#include "Define.h"

// Pure arithmetic behind DcThreatMap (Util/DcThreatMap.h): pack grouping over
// spawn positions and the live-velocity estimate. No game types, kernel tested
// (t/TestThreatMap.cpp).
namespace DcThreatMapKernel
{
    // Two spawns this close (and on about the same level) belong to one pack.
    // The engine's assist radius (CONFIG_CREATURE_FAMILY_ASSISTANCE_RADIUS, 10yd
    // stock) is what actually chains a pull from one mob to the next, so the
    // caller passes it in; the level band keeps a ledge pack apart from the floor
    // pack under it.
    inline constexpr float kPackLevelBandYd = 5.0f;

    // A velocity sample older than this is a teleport or a gap in refreshes, not
    // movement.
    inline constexpr uint32 kMaxVelocitySampleMs = 2000;

    struct SpawnPoint
    {
        float x{0.0f};
        float y{0.0f};
        float z{0.0f};
        uint32 formationId{0};  // creature_formations leader spawn id, 0 = none
    };

    // Pack id per spawn, 1-based and dense in first-seen order. Spawns share a
    // pack when they are in the same formation, or within `linkYd` of each other
    // (2D, inside the level band), transitively: three mobs in a line 8yd apart
    // are one pack, as the engine's assist chain would make them.
    inline std::vector<uint32> GroupPacks(std::vector<SpawnPoint> const& spawns, float linkYd)
    {
        std::size_t const n = spawns.size();
        std::vector<std::size_t> parent(n);
        std::iota(parent.begin(), parent.end(), std::size_t{0});
        auto find = [&parent](std::size_t i)
        {
            while (parent[i] != i)
            {
                parent[i] = parent[parent[i]];
                i = parent[i];
            }
            return i;
        };
        auto unite = [&](std::size_t a, std::size_t b)
        {
            a = find(a);
            b = find(b);
            if (a != b)
                parent[b < a ? a : b] = b < a ? b : a;
        };

        float const link2 = linkYd * linkYd;
        for (std::size_t i = 0; i < n; ++i)
        {
            for (std::size_t j = i + 1; j < n; ++j)
            {
                SpawnPoint const& a = spawns[i];
                SpawnPoint const& b = spawns[j];
                if (a.formationId && a.formationId == b.formationId)
                {
                    unite(i, j);
                    continue;
                }
                float const dx = a.x - b.x;
                float const dy = a.y - b.y;
                if (dx * dx + dy * dy <= link2 && std::fabs(a.z - b.z) <= kPackLevelBandYd)
                    unite(i, j);
            }
        }

        std::vector<uint32> ids(n, 0u);
        std::vector<uint32> rootId(n, 0u);
        uint32 next = 0;
        for (std::size_t i = 0; i < n; ++i)
        {
            std::size_t const r = find(i);
            if (!rootId[r])
                rootId[r] = ++next;
            ids[i] = rootId[r];
        }
        return ids;
    }

    struct Velocity
    {
        float vx{0.0f};
        float vy{0.0f};
    };

    // Yards per second between two position samples `dtMs` apart. Zero when the
    // gap is empty or too long to be movement.
    inline Velocity EstimateVelocity(float x0, float y0, float x1, float y1, uint32 dtMs)
    {
        if (dtMs == 0 || dtMs > kMaxVelocitySampleMs)
            return {};
        float const dt = static_cast<float>(dtMs) / 1000.0f;
        return {(x1 - x0) / dt, (y1 - y0) / dt};
    }

    // Is a refresh due? `lastMs` is 0 before the first one. Wrap-safe on the
    // 32-bit millisecond clock.
    inline bool RefreshDue(uint32 lastMs, uint32 nowMs, uint32 intervalMs)
    {
        return lastMs == 0 || static_cast<uint32>(nowMs - lastMs) >= intervalMs;
    }
}

#endif  // _PLAYERBOT_DCTHREATMAPKERNEL_H
