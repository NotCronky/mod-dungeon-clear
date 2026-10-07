/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "Ai/Dungeon/DungeonClear/Util/DcProximityGuard.h"

using namespace DcProximityGuard;

namespace
{
    constexpr float kMargin = 2.0f;
    constexpr float kHyst = 3.0f;
}

TEST(DcProximityGuard, ClearOfEveryPackIsNotThreatened)
{
    std::vector<Threat> const t = {{30, 0, 0, 20}};
    Decision const d = Decide(0, 0, 0, t, -10, 0, 0, kMargin, kHyst);
    EXPECT_FALSE(d.threatened);
}

TEST(DcProximityGuard, InsideTheMarginIsThreatened)
{
    // 21yd from a 20yd reach: inside reach + 2 margin.
    std::vector<Threat> const t = {{21, 0, 0, 20}};
    Decision const d = Decide(0, 0, 0, t, -10, 0, 0, kMargin, kHyst);
    EXPECT_TRUE(d.threatened);
}

TEST(DcProximityGuard, StepsStraightAwayPastTheHysteresis)
{
    std::vector<Threat> const t = {{15, 0, 0, 20}};
    Decision const d = Decide(0, 0, 0, t, 0, -40, 0, kMargin, kHyst);
    ASSERT_TRUE(d.threatened);
    // Out to 20 + 2 + 3 = 25yd from the mob, on the far side from it.
    EXPECT_NEAR(d.destX, 15.0f - 25.0f, 0.01f);
    EXPECT_NEAR(d.destY, 0.0f, 0.01f);
}

TEST(DcProximityGuard, PicksTheDeepestThreat)
{
    std::vector<Threat> const t = {{0, 21, 0, 20}, {10, 0, 0, 20}};
    Decision const d = Decide(0, 0, 0, t, -50, -50, 0, kMargin, kHyst);
    ASSERT_TRUE(d.threatened);
    EXPECT_EQ(d.worst, 1u);
}

TEST(DcProximityGuard, AwayIntoAnotherPackGoesTowardTheAnchorInstead)
{
    // A corridor: packs east and west of the member, the tank south. Straight
    // away from the east pack walks into the west one.
    std::vector<Threat> const t = {{12, 0, 0, 15}, {-20, 0, 0, 15}};
    Decision const d = Decide(0, 0, 0, t, 0, -60, 0, kMargin, kHyst);
    ASSERT_TRUE(d.threatened);
    EXPECT_TRUE(ClearOfAll(t, d.destX, d.destY, d.destZ, kMargin));
    EXPECT_LT(d.destY, 0.0f);  // went south, toward the tank
}

TEST(DcProximityGuard, HeightKeepsALedgePackOutOfReach)
{
    // A pack on a ledge 25yd above: close in 2D, out of reach in 3D.
    std::vector<Threat> const t = {{5, 0, 25, 20}};
    Decision const d = Decide(0, 0, 0, t, -10, 0, 0, kMargin, kHyst);
    EXPECT_FALSE(d.threatened);
}

TEST(DcProximityGuard, StandingOnTheMobStepsTowardTheAnchor)
{
    std::vector<Threat> const t = {{0, 0, 0, 20}};
    Decision const d = Decide(0, 0, 0, t, 0, -50, 0, kMargin, kHyst);
    ASSERT_TRUE(d.threatened);
    EXPECT_LT(d.destY, -20.0f);
}
