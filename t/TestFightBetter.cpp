/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Phase 1 of the clear-success plan: the kill-target skull (PickFocusTarget), the
// threat cap (ShouldHoldForThreat) and flee-for-help in the pull estimate.

#include "gtest/gtest.h"

#include <vector>

#include "DungeonClearMath.h"

using namespace DungeonClearMath;

namespace
{
    ThreatHoldInput Base()
    {
        ThreatHoldInput in;
        in.tankThreat = 1000.0f;
        in.botThreat = 0.0f;
        in.capPct = 90;
        in.combatSinceMs = 10000;
        in.now = 11000;
        in.noTankThreatCapMs = 4000;
        in.tankHealthPct = 100.0f;
        in.panicHpPct = 60.0f;
        return in;
    }

    FocusCandidate F(FocusRank rank, bool onTank, std::uint32_t health)
    {
        FocusCandidate c;
        c.rank = rank;
        c.onTank = onTank;
        c.health = health;
        return c;
    }

    DynPullMob M(float x, std::uint32_t packId = 0, bool canFlee = false)
    {
        return { x, 0.0f, 0.0f, /*chainEligible*/ true, packId, /*aggroReach*/ 10.0f, /*patroller*/ false,
                 /*elite*/ true, canFlee };
    }
}

// ---- threat cap ----

TEST(DcFightBetter, ThreatUnderTheCapIsReleased)
{
    ThreatHoldInput in = Base();
    in.botThreat = 899.0f;
    EXPECT_FALSE(ShouldHoldForThreat(in));
}

TEST(DcFightBetter, ThreatAtTheCapIsHeld)
{
    ThreatHoldInput in = Base();
    in.botThreat = 900.0f;
    EXPECT_TRUE(ShouldHoldForThreat(in));
}

TEST(DcFightBetter, NoTankThreatHoldsUntilTheCap)
{
    ThreatHoldInput in = Base();
    in.tankThreat = 0.0f;
    in.now = in.combatSinceMs + 3999;
    EXPECT_TRUE(ShouldHoldForThreat(in));
    in.now = in.combatSinceMs + 4000;
    EXPECT_FALSE(ShouldHoldForThreat(in));
}

TEST(DcFightBetter, NoTankThreatBeforeTheFightIsHeld)
{
    ThreatHoldInput in = Base();
    in.tankThreat = 0.0f;
    in.combatSinceMs = 0;
    EXPECT_TRUE(ShouldHoldForThreat(in));
}

TEST(DcFightBetter, ThreatCapBypasses)
{
    ThreatHoldInput in = Base();
    in.botThreat = 5000.0f;
    ASSERT_TRUE(ShouldHoldForThreat(in));

    ThreatHoldInput exempt = in;
    exempt.exempt = true;
    EXPECT_FALSE(ShouldHoldForThreat(exempt));

    ThreatHoldInput attacked = in;
    attacked.beingAttacked = true;
    EXPECT_FALSE(ShouldHoldForThreat(attacked));

    ThreatHoldInput peel = in;
    peel.peel = true;
    EXPECT_FALSE(ShouldHoldForThreat(peel));

    ThreatHoldInput off = in;
    off.capPct = 0;
    EXPECT_FALSE(ShouldHoldForThreat(off));

    ThreatHoldInput panic = in;
    panic.tankHealthPct = 59.0f;
    EXPECT_FALSE(ShouldHoldForThreat(panic));
    panic.panicHpPct = 0.0f;
    EXPECT_TRUE(ShouldHoldForThreat(panic));
}

// ---- kill-target skull ----

TEST(DcFightBetter, FocusNothingToMark)
{
    EXPECT_EQ(PickFocusTarget({}, -1), -1);
}

TEST(DcFightBetter, FocusRanksFleeingThenHealerThenCaster)
{
    std::vector<FocusCandidate> const c{
        F(FocusRank::Other, true, 100), F(FocusRank::Caster, true, 900), F(FocusRank::Healer, false, 900),
        F(FocusRank::Fleeing, false, 900) };
    EXPECT_EQ(PickFocusTarget(c, -1), 3);
    EXPECT_EQ(PickFocusTarget({ c[0], c[1], c[2] }, -1), 2);
    EXPECT_EQ(PickFocusTarget({ c[0], c[1] }, -1), 1);
}

TEST(DcFightBetter, FocusPrefersAMobOnATankThenLeastHealth)
{
    std::vector<FocusCandidate> const c{
        F(FocusRank::Other, false, 10), F(FocusRank::Other, true, 500), F(FocusRank::Other, true, 300) };
    EXPECT_EQ(PickFocusTarget(c, -1), 2);
}

TEST(DcFightBetter, FocusKeepsTheMarkUnlessABetterRankAppears)
{
    std::vector<FocusCandidate> c{ F(FocusRank::Other, true, 900), F(FocusRank::Other, true, 100) };
    // Another mob with less health doesn't move the mark.
    EXPECT_EQ(PickFocusTarget(c, 0), 0);
    // A healer does.
    c.push_back(F(FocusRank::Healer, true, 800));
    EXPECT_EQ(PickFocusTarget(c, 0), 2);
    // A marked healer stays over a lower-health healer.
    c.push_back(F(FocusRank::Healer, true, 50));
    EXPECT_EQ(PickFocusTarget(c, 2), 2);
    // A mob that starts fleeing takes it.
    c.push_back(F(FocusRank::Fleeing, false, 700));
    EXPECT_EQ(PickFocusTarget(c, 2), 4);
}

// ---- flee for help in the pull estimate ----

TEST(DcFightBetter, FleeingMobBringsTheNearestOne)
{
    // The target at 0, a lone mob 20yd away: outside its 10yd reach and the 5yd
    // assist hop, inside a 30yd flee run.
    std::vector<DynPullMob> mobs{ M(0.0f, 0, /*canFlee*/ true), M(20.0f) };
    EXPECT_EQ(EstimateAggroCount(mobs, 0, 0.0f, 5.0f, 5.0f), 1u);
    EXPECT_EQ(EstimateAggroCount(mobs, 0, 0.0f, 5.0f, 5.0f, false, nullptr, nullptr, 30.0f), 2u);

    // A mob that doesn't flee brings nobody.
    mobs[0].canFlee = false;
    EXPECT_EQ(EstimateAggroCount(mobs, 0, 0.0f, 5.0f, 5.0f, false, nullptr, nullptr, 30.0f), 1u);
}

TEST(DcFightBetter, FleeBringsTheWholePackAndOnlyTheNearest)
{
    // The target flees to the nearest mob (20yd, pack 7, whose other member is at
    // 60yd); the mob at 25yd is further and stays.
    std::vector<DynPullMob> mobs{ M(0.0f, 0, true), M(20.0f, 7), M(25.0f), M(60.0f, 7) };
    std::uint32_t weight = 0;
    EXPECT_EQ(EstimateAggroCount(mobs, 0, 0.0f, 0.0f, 5.0f, false, nullptr, &weight, 30.0f), 3u);
    EXPECT_EQ(weight, 9u);
}

TEST(DcFightBetter, FleeIsOneRing)
{
    // A mob reached by a flee does not flee on: 0 -> 20, never 20 -> 40.
    std::vector<DynPullMob> mobs{ M(0.0f, 0, true), M(20.0f, 0, true), M(40.0f) };
    EXPECT_EQ(EstimateAggroCount(mobs, 0, 0.0f, 0.0f, 5.0f, false, nullptr, nullptr, 30.0f), 2u);
}
