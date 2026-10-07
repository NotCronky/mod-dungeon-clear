/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCTHREATMAPKERNEL_H
#define _PLAYERBOT_DCTHREATMAPKERNEL_H

#include <algorithm>
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

    struct PathPoint
    {
        float x{0.0f};
        float y{0.0f};
        float z{0.0f};
    };

    inline constexpr float kNever = -1.0f;
    // Step along the path when sampling it, yards.
    inline constexpr float kPathSampleYd = 2.0f;

    // Seconds until a patroller walking its CYCLIC waypoint loop `path` from
    // (x,y,z) at `speed` yd/s first comes within `radius` (2D, inside
    // `heightBand`) of the target point; 0 if it already is; kNever if not within
    // `horizonSec`. The patroller is placed on its nearest path segment and walks
    // forward (waypoint order), which is how a MovementType 2 creature loops.
    inline float PatrolEtaSec(std::vector<PathPoint> const& path, float x, float y, float z, float speed,
                              float tx, float ty, float tz, float radius, float heightBand, float horizonSec)
    {
        auto within = [&](float px, float py, float pz)
        {
            float const dx = px - tx;
            float const dy = py - ty;
            return dx * dx + dy * dy <= radius * radius && std::fabs(pz - tz) <= heightBand;
        };
        if (within(x, y, z))
            return 0.0f;
        std::size_t const n = path.size();
        if (n < 2 || speed <= 0.0f || horizonSec <= 0.0f)
            return kNever;

        // Nearest segment (i -> i+1, wrapping) and the projection onto it.
        std::size_t seg = 0;
        float segT = 0.0f;
        float best = -1.0f;
        for (std::size_t i = 0; i < n; ++i)
        {
            PathPoint const& a = path[i];
            PathPoint const& b = path[(i + 1) % n];
            float const ex = b.x - a.x;
            float const ey = b.y - a.y;
            float const len2 = ex * ex + ey * ey;
            float t = len2 > 0.0f ? ((x - a.x) * ex + (y - a.y) * ey) / len2 : 0.0f;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            float const px = a.x + ex * t - x;
            float const py = a.y + ey * t - y;
            float const pz = a.z + (b.z - a.z) * t - z;
            float const d2 = px * px + py * py + pz * pz;
            if (best < 0.0f || d2 < best)
            {
                best = d2;
                seg = i;
                segT = t;
            }
        }

        float const maxDist = speed * horizonSec;
        float walked = 0.0f;
        float t = segT;
        std::size_t i = seg;
        // A loop longer than the horizon ends the walk at maxDist; a short loop is
        // walked round at most twice.
        for (std::size_t steps = 0; steps < 2 * n + 1 && walked <= maxDist; ++steps)
        {
            PathPoint const& a = path[i];
            PathPoint const& b = path[(i + 1) % n];
            float const ex = b.x - a.x;
            float const ey = b.y - a.y;
            float const ez = b.z - a.z;
            float const len = std::sqrt(ex * ex + ey * ey + ez * ez);
            float d = t * len;
            while (d <= len && walked <= maxDist)
            {
                float const f = len > 0.0f ? d / len : 0.0f;
                if (within(a.x + ex * f, a.y + ey * f, a.z + ez * f))
                    return walked / speed;
                d += kPathSampleYd;
                walked += kPathSampleYd;
            }
            walked -= d - len;  // the overshoot past this segment's end
            t = 0.0f;
            i = (i + 1) % n;
        }
        return kNever;
    }

    // The longest stretch of the loop, in seconds at `speed`, the patroller
    // spends OUTSIDE `radius` (2D, inside `heightBand`) of the target: the
    // biggest window a fight there can fit between two of its passes. A loop
    // that never comes near returns kNever (no window needed). Shorter than the
    // fight means waiting never helps: Lucifron walks a small loop around his
    // own Core Hounds, and a hold for him only ran out its cap
    // (tr-20261007-222010-1).
    inline float PatrolClearWindowSec(std::vector<PathPoint> const& path, float speed, float tx, float ty, float tz,
                                      float radius, float heightBand)
    {
        std::size_t const n = path.size();
        if (n < 2 || speed <= 0.0f)
            return kNever;
        std::vector<bool> outside;
        for (std::size_t i = 0; i < n; ++i)
        {
            PathPoint const& a = path[i];
            PathPoint const& b = path[(i + 1) % n];
            float const ex = b.x - a.x;
            float const ey = b.y - a.y;
            float const ez = b.z - a.z;
            float const len = std::sqrt(ex * ex + ey * ey + ez * ez);
            for (float d = 0.0f; d < len || (len <= 0.0f && d == 0.0f); d += kPathSampleYd)
            {
                float const f = len > 0.0f ? d / len : 0.0f;
                float const px = a.x + ex * f - tx;
                float const py = a.y + ey * f - ty;
                float const pz = a.z + ez * f;
                outside.push_back(px * px + py * py > radius * radius || std::fabs(pz - tz) > heightBand);
                if (len <= 0.0f)
                    break;
            }
        }
        std::size_t const m = outside.size();
        std::size_t insideCount = 0;
        for (bool o : outside)
            insideCount += o ? 0 : 1;
        if (insideCount == 0)
            return kNever;
        // Longest cyclic run of `outside`, starting the scan just after an inside sample.
        std::size_t start = 0;
        while (outside[start])
            ++start;
        std::size_t best = 0;
        std::size_t run = 0;
        for (std::size_t k = 1; k <= m; ++k)
        {
            if (outside[(start + k) % m])
                best = std::max(best, ++run);
            else
                run = 0;
        }
        return static_cast<float>(best) * kPathSampleYd / speed;
    }

    // Is a refresh due? `lastMs` is 0 before the first one. Wrap-safe on the
    // 32-bit millisecond clock.
    inline bool RefreshDue(uint32 lastMs, uint32 nowMs, uint32 intervalMs)
    {
        return lastMs == 0 || static_cast<uint32>(nowMs - lastMs) >= intervalMs;
    }
}

#endif  // _PLAYERBOT_DCTHREATMAPKERNEL_H
