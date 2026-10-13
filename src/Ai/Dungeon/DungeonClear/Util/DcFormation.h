/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCFORMATION_H
#define _PLAYERBOT_DCFORMATION_H

#include <algorithm>
#include <cmath>

#include "Define.h"

// Shared formation geometry for parking a party/raid around an anchor (camp
// fan, follow fan, post-drop scatter). Pure arithmetic, header-only, kernel
// tested.
//
// The 5-man fans hardcoded a 1-2yd circle, which is exactly right for 4
// followers and a mosh pit for 39: the circumference must GROW with the
// population or everyone stacks. The primitives here keep the fan deterministic
// per bot (seeded by GUID counter — MoveTo dedups an unchanging destination, so
// each bot glides to one spot and parks) while sizing the ring from how many
// bodies actually stand on it, and optionally splitting roles onto concentric
// rings (melee in, ranged next, healers out) so a raid camp reads as a
// formation instead of a blob.
namespace DcFormation
{
    // Arc length each member needs on its ring. ~2.5yd keeps neighbors outside
    // the stock anti-stack shuffle's contact distance without spreading a small
    // party wider than the old fixed fan.
    inline constexpr float kSlotSpacingYd = 2.5f;

    // Radial gap between consecutive role rings.
    inline constexpr float kRingGapYd = 3.0f;

    // The golden angle (radians): consecutive seeds land maximally apart, so
    // any subset of bots fans evenly without coordination.
    inline constexpr float kGoldenAngle = 2.39996323f;

    // Radius of one ring holding `count` members at kSlotSpacingYd spacing,
    // never below `baseRadius` (the small-party floor — 5-man fans keep their
    // familiar tight circle through it).
    inline float RingRadius(uint32 count, float baseRadius)
    {
        float const need =
            static_cast<float>(count) * kSlotSpacingYd / (2.0f * 3.14159265f);
        return std::max(baseRadius, need);
    }

    // The three concentric role rings for the given role census. Melee hug the
    // anchor; ranged ring opens outside the melee ring; healers outside that.
    // Each ring is sized for ITS population, then pushed outside its inner
    // neighbor by kRingGapYd.
    struct RoleRings
    {
        float melee;
        float ranged;
        float healer;
    };

    inline RoleRings ComputeRoleRings(uint32 meleeCount, uint32 rangedCount,
                                      uint32 healerCount, float baseRadius = 1.5f)
    {
        RoleRings r{};
        r.melee  = RingRadius(meleeCount, baseRadius);
        r.ranged = std::max(RingRadius(rangedCount, baseRadius), r.melee + kRingGapYd);
        r.healer = std::max(RingRadius(healerCount, baseRadius), r.ranged + kRingGapYd);
        return r;
    }

    // Deterministic slot offset on a ring: golden-angle direction from the
    // seed, ring radius plus up to 1yd of seeded variance so a ring reads as a
    // loose arc, not a drill formation.
    struct Offset
    {
        float dx;
        float dy;
    };

    inline Offset SlotOffset(uint32 seed, float ringRadius)
    {
        float const angle = static_cast<float>(seed) * kGoldenAngle;
        float const radius =
            ringRadius + static_cast<float>(seed % 101) / 100.0f;  // [+0, +1]
        return { radius * std::cos(angle), radius * std::sin(angle) };
    }

    // ---- the follow column ----
    //
    // Walking, the party trails the tank as a column on the path the tank walked:
    // rows of `perRow` side by side, melee in the front rows, then ranged, then
    // healers at the back, so nobody walks ahead of the tank or out to the side
    // where other packs stand.

    // Front-to-back order of a follower's role in the column.
    enum class ColumnRole : uint8
    {
        Melee = 0,   // melee DPS and off-tanks
        Ranged = 1,
        Healer = 2,
    };

    // A follower's place in the column: how far back along the tank's path
    // (`lag`, yards) and how far to the side of it (`lateral`, yards; positive is
    // the left of the direction of travel).
    struct ColumnSlotPos
    {
        float lag;
        float lateral;
    };

    // Slot `index` (0 = the first follower) in a column `perRow` wide, the first
    // row `baseLag` behind the tank, rows `rowGap` apart and members `colGap`
    // apart. Each row is centred on the path.
    inline ColumnSlotPos ColumnSlot(uint32 index, uint32 perRow, float baseLag, float rowGap, float colGap)
    {
        perRow = std::max<uint32>(1, perRow);
        uint32 const row = index / perRow;
        uint32 const col = index % perRow;
        float const lateral = (static_cast<float>(col) - static_cast<float>(perRow - 1) / 2.0f) * colGap;
        return { baseLag + static_cast<float>(row) * rowGap, lateral };
    }

    // The same slot as a follow offset from the tank when there is no walked path
    // to trail (a fresh run): a distance and an angle relative to the tank's facing
    // (0 = in front, pi = behind), always behind it.
    inline float ColumnFollowAngle(ColumnSlotPos slot)
    {
        return std::atan2(slot.lateral, -slot.lag);
    }

    inline float ColumnFollowDist(ColumnSlotPos slot)
    {
        return std::sqrt(slot.lag * slot.lag + slot.lateral * slot.lateral);
    }

    // ---- the camp arc ----
    //
    // At a camp the pack comes from one side. Ranged and healers stand on an arc on
    // the OTHER side, so the pack reaches the tank and the melee first: `axis` is
    // the direction from the pack to the camp (radians, world frame), `arcWidth`
    // the arc's full width, and the seed spreads members along it deterministically.
    inline Offset ArcOffset(uint32 seed, float axis, float arcWidth, float radius)
    {
        constexpr float kGoldenFraction = 0.61803399f;
        float const t = std::fmod(static_cast<float>(seed) * kGoldenFraction, 1.0f);  // [0, 1)
        float const angle = axis + (t - 0.5f) * arcWidth;
        float const r = radius + static_cast<float>(seed % 101) / 100.0f;  // [+0, +1]
        return { r * std::cos(angle), r * std::sin(angle) };
    }
}

#endif  // _PLAYERBOT_DCFORMATION_H
