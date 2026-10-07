/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCPROXIMITYGUARD_H
#define _PLAYERBOT_DCPROXIMITYGUARD_H

#include <cmath>
#include <vector>

// The member proximity guard's geometry: is a party member standing inside an
// idle pack's aggro, and where should it step to? Pure arithmetic, kernel tested
// (t/TestProximityGuard.cpp); the trigger and action in
// DungeonClearTriggers/DcFollowerActions feed it from the threat map.
//
// A member is THREATENED when it stands within `reach + margin` of any idle
// hostile, reach being that mob's aggro reach against it
// (DcEngageGeometry::AggroReach). It steps out to `reach + margin + hysteresis`
// from the worst one, so the next tick finds it clear by a few yards rather than
// on the edge, and does not jitter.
//
// Where it steps: straight away from the worst mob, unless that lands it inside
// another pack's reach; then toward the anchor (the tank), the first point along
// that line clear of every mob. Healers and ranged stay useful that way, and the
// party never fans out into the next room. Falls back to the away point when
// nothing on the anchor line is clear either.
namespace DcProximityGuard
{
    struct Threat
    {
        float x{0.0f};
        float y{0.0f};
        float z{0.0f};
        float reach{0.0f};  // the mob's aggro reach against this member, yd
    };

    struct Decision
    {
        bool threatened{false};
        std::size_t worst{0};  // index into the threats of the deepest one
        float destX{0.0f};
        float destY{0.0f};
        float destZ{0.0f};
    };

    // Step along the anchor line when looking for clear ground, yards.
    inline constexpr float kAnchorStepYd = 2.0f;

    inline float Dist3(float ax, float ay, float az, float bx, float by, float bz)
    {
        float const dx = ax - bx;
        float const dy = ay - by;
        float const dz = az - bz;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    inline bool ClearOfAll(std::vector<Threat> const& threats, float x, float y, float z, float margin)
    {
        for (Threat const& t : threats)
            if (Dist3(x, y, z, t.x, t.y, t.z) < t.reach + margin)
                return false;
        return true;
    }

    inline Decision Decide(float bx, float by, float bz, std::vector<Threat> const& threats,
                           float ax, float ay, float az, float margin, float hysteresis)
    {
        Decision d;
        float deepest = 0.0f;
        for (std::size_t i = 0; i < threats.size(); ++i)
        {
            Threat const& t = threats[i];
            float const depth = t.reach + margin - Dist3(bx, by, bz, t.x, t.y, t.z);
            if (depth > deepest)
            {
                deepest = depth;
                d.worst = i;
                d.threatened = true;
            }
        }
        if (!d.threatened)
            return d;

        // Away from the worst mob, out past its reach by the hysteresis.
        Threat const& w = threats[d.worst];
        float ux = bx - w.x;
        float uy = by - w.y;
        float len = std::sqrt(ux * ux + uy * uy);
        if (len < 0.01f)
        {
            // Standing on it: away means toward the anchor.
            ux = ax - w.x;
            uy = ay - w.y;
            len = std::sqrt(ux * ux + uy * uy);
        }
        if (len < 0.01f)
        {
            ux = 1.0f;
            uy = 0.0f;
            len = 1.0f;
        }
        float const out = w.reach + margin + hysteresis;
        float const awayX = w.x + ux / len * out;
        float const awayY = w.y + uy / len * out;
        if (ClearOfAll(threats, awayX, awayY, bz, margin + hysteresis))
        {
            d.destX = awayX;
            d.destY = awayY;
            d.destZ = bz;
            return d;
        }

        // Toward the anchor: the first point on the way clear of every mob.
        float const toAnchor = Dist3(bx, by, bz, ax, ay, az);
        for (float s = kAnchorStepYd; s <= toAnchor; s += kAnchorStepYd)
        {
            float const f = s / toAnchor;
            float const px = bx + (ax - bx) * f;
            float const py = by + (ay - by) * f;
            float const pz = bz + (az - bz) * f;
            if (ClearOfAll(threats, px, py, pz, margin + hysteresis))
            {
                d.destX = px;
                d.destY = py;
                d.destZ = pz;
                return d;
            }
        }

        d.destX = awayX;
        d.destY = awayY;
        d.destZ = bz;
        return d;
    }
}

#endif  // _PLAYERBOT_DCPROXIMITYGUARD_H
