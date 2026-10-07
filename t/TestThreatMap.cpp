/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "Ai/Dungeon/DungeonClear/Util/DcThreatMapKernel.h"

using namespace DcThreatMapKernel;

TEST(DcThreatMapPacks, LoneSpawnsArePacksOfOne)
{
    std::vector<SpawnPoint> const s = {{0, 0, 0, 0}, {50, 0, 0, 0}, {0, 50, 0, 0}};
    std::vector<uint32> const ids = GroupPacks(s, 10.0f);
    EXPECT_EQ(ids, (std::vector<uint32>{1, 2, 3}));
}

TEST(DcThreatMapPacks, AssistChainLinksTransitively)
{
    // 8yd apart in a line: each links to the next, so all three are one pack,
    // as the engine's assist chain would pull them.
    std::vector<SpawnPoint> const s = {{0, 0, 0, 0}, {8, 0, 0, 0}, {16, 0, 0, 0}, {40, 0, 0, 0}};
    std::vector<uint32> const ids = GroupPacks(s, 10.0f);
    EXPECT_EQ(ids[0], ids[1]);
    EXPECT_EQ(ids[1], ids[2]);
    EXPECT_NE(ids[2], ids[3]);
}

TEST(DcThreatMapPacks, FormationLinksAtAnyDistance)
{
    std::vector<SpawnPoint> const s = {{0, 0, 0, 77}, {60, 0, 0, 77}, {30, 30, 0, 0}};
    std::vector<uint32> const ids = GroupPacks(s, 10.0f);
    EXPECT_EQ(ids[0], ids[1]);
    EXPECT_NE(ids[0], ids[2]);
}

TEST(DcThreatMapPacks, LevelBandKeepsALedgeApart)
{
    // Garr's cave vs a ledge overhead: close in 2D, a floor apart.
    std::vector<SpawnPoint> const s = {{0, 0, 0, 0}, {4, 0, 20, 0}, {4, 0, 3, 0}};
    std::vector<uint32> const ids = GroupPacks(s, 10.0f);
    EXPECT_NE(ids[0], ids[1]);
    EXPECT_EQ(ids[0], ids[2]);
}

TEST(DcThreatMapPacks, IdsAreDenseInFirstSeenOrder)
{
    std::vector<SpawnPoint> const s = {{100, 0, 0, 0}, {0, 0, 0, 0}, {101, 0, 0, 0}, {1, 0, 0, 0}};
    std::vector<uint32> const ids = GroupPacks(s, 10.0f);
    EXPECT_EQ(ids, (std::vector<uint32>{1, 2, 1, 2}));
}

TEST(DcThreatMapPacks, Empty)
{
    EXPECT_TRUE(GroupPacks({}, 10.0f).empty());
}

TEST(DcThreatMapVelocity, YardsPerSecond)
{
    Velocity const v = EstimateVelocity(0.0f, 0.0f, 3.5f, -1.75f, 500);
    EXPECT_FLOAT_EQ(v.vx, 7.0f);
    EXPECT_FLOAT_EQ(v.vy, -3.5f);
}

TEST(DcThreatMapVelocity, NoSampleOrStaleSampleIsStill)
{
    Velocity const none = EstimateVelocity(0.0f, 0.0f, 10.0f, 0.0f, 0);
    EXPECT_FLOAT_EQ(none.vx, 0.0f);
    Velocity const stale = EstimateVelocity(0.0f, 0.0f, 10.0f, 0.0f, kMaxVelocitySampleMs + 1);
    EXPECT_FLOAT_EQ(stale.vx, 0.0f);
}

TEST(DcThreatMapRefresh, FirstRefreshIsAlwaysDue)
{
    EXPECT_TRUE(RefreshDue(0, 12345, 500));
}

TEST(DcThreatMapRefresh, ThrottledUntilTheInterval)
{
    EXPECT_FALSE(RefreshDue(1000, 1499, 500));
    EXPECT_TRUE(RefreshDue(1000, 1500, 500));
}

TEST(DcThreatMapRefresh, SurvivesClockWrap)
{
    EXPECT_FALSE(RefreshDue(0xFFFFFF00u, 0x00000010u, 500));
    EXPECT_TRUE(RefreshDue(0xFFFFFF00u, 0x00000200u, 500));
}

namespace
{
    // A 100yd square loop, walked counter-clockwise from (0,0).
    std::vector<PathPoint> Square()
    {
        return {{0, 0, 0}, {100, 0, 0}, {100, 100, 0}, {0, 100, 0}};
    }
}

TEST(DcThreatMapPatrol, AlreadyWithinIsZero)
{
    EXPECT_FLOAT_EQ(PatrolEtaSec(Square(), 50, 0, 0, 2.5f, 55, 5, 0, 10, 15, 60), 0.0f);
}

TEST(DcThreatMapPatrol, AheadOnTheLoopArrivesAtWalkSpeed)
{
    // From (0,0) heading east, the target at (100,50) with a 10yd radius is first
    // reached ~140yd along: 100 east + 40 north.
    float const eta = PatrolEtaSec(Square(), 0, 0, 0, 2.0f, 100, 50, 0, 10, 15, 200);
    EXPECT_NEAR(eta, 70.0f, 2.0f);
}

TEST(DcThreatMapPatrol, WalksForwardNotBack)
{
    // Just past (0,0) heading east: the target at (0,50) is behind it, reached
    // only round the loop (~340yd), not 40yd back the way it came.
    float const eta = PatrolEtaSec(Square(), 5, 0, 0, 2.0f, 0, 50, 0, 10, 15, 500);
    EXPECT_GT(eta, 150.0f);
}

TEST(DcThreatMapPatrol, BeyondTheHorizonIsNever)
{
    EXPECT_EQ(PatrolEtaSec(Square(), 0, 0, 0, 2.0f, 100, 50, 0, 10, 15, 30), kNever);
}

TEST(DcThreatMapPatrol, AFloorAwayNeverArrives)
{
    // Geddon's ledge loop passing over a cave a floor below: close in 2D only.
    EXPECT_EQ(PatrolEtaSec(Square(), 0, 0, 0, 2.0f, 50, 0, -30, 10, 15, 500), kNever);
}

TEST(DcThreatMapPatrol, NoPathOrNoSpeedIsNever)
{
    EXPECT_EQ(PatrolEtaSec({}, 0, 0, 0, 2.0f, 50, 50, 0, 10, 15, 500), kNever);
    EXPECT_EQ(PatrolEtaSec(Square(), 0, 0, 0, 0.0f, 50, 50, 0, 10, 15, 500), kNever);
}

TEST(DcThreatMapPatrol, ClearWindowIsTheLongestStretchAway)
{
    // The square loop is 400yd; a 10yd circle at (50,0) covers ~20yd of the
    // south edge, so ~380yd of the loop is clear: 190s at 2yd/s.
    float const w = PatrolClearWindowSec(Square(), 2.0f, 50, 0, 0, 10, 15);
    EXPECT_NEAR(w, 190.0f, 3.0f);
}

TEST(DcThreatMapPatrol, ALoopThatNeverLeavesHasNoWindow)
{
    // Lucifron's case: the radius swallows the whole loop.
    EXPECT_FLOAT_EQ(PatrolClearWindowSec(Square(), 2.0f, 50, 50, 0, 100, 15), 0.0f);
}

TEST(DcThreatMapPatrol, ALoopThatNeverComesNearNeedsNoWindow)
{
    EXPECT_EQ(PatrolClearWindowSec(Square(), 2.0f, 500, 500, 0, 10, 15), kNever);
}

TEST(DcThreatMapPatrol, TheWindowWrapsRoundTheLoop)
{
    // The circle sits on the loop's start point, so the clear stretch runs past
    // the end of the waypoint list and wraps: it must still read as one window.
    float const w = PatrolClearWindowSec(Square(), 2.0f, 0, 0, 0, 10, 15);
    EXPECT_NEAR(w, 190.0f, 3.0f);
}
