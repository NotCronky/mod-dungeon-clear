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
