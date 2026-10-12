/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include <map>
#include <string>

#include "Assemble/DcAssemblePlanner.h"

using namespace DcAssemblePlanner;

namespace
{
    Counts C(std::size_t tanks, std::size_t healers, std::size_t dps)
    {
        Counts c;
        c.tanks = tanks;
        c.healers = healers;
        c.dps = dps;
        return c;
    }

    void ExpectCounts(Counts const& actual, Counts const& expected)
    {
        EXPECT_EQ(actual.tanks, expected.tanks);
        EXPECT_EQ(actual.healers, expected.healers);
        EXPECT_EQ(actual.dps, expected.dps);
    }
}

TEST(DcAssemblePlanner, SoloDamageDealerGetsAFullParty)
{
    Plan const plan = Compute(5, C(0, 0, 1), C(0, 0, 0));
    ExpectCounts(plan.add, C(1, 1, 2));
    ExpectCounts(plan.remove, C(0, 0, 0));
}

TEST(DcAssemblePlanner, SoloTankGetsNoTank)
{
    Plan const plan = Compute(5, C(1, 0, 0), C(0, 0, 0));
    ExpectCounts(plan.add, C(0, 1, 3));
}

TEST(DcAssemblePlanner, FortyFromADamageDealer)
{
    // 40: 4 tanks, 10 healers, 26 damage; the player is one of the damage.
    Plan const plan = Compute(40, C(0, 0, 1), C(0, 0, 0));
    ExpectCounts(plan.quota, C(4, 10, 26));
    ExpectCounts(plan.add, C(4, 10, 25));
    EXPECT_EQ(plan.add.Total(), 39u);
}

TEST(DcAssemblePlanner, FortyFromAHealer)
{
    Plan const plan = Compute(40, C(0, 1, 0), C(0, 0, 0));
    ExpectCounts(plan.add, C(4, 9, 26));
}

TEST(DcAssemblePlanner, OverfilledRoleTakesDamageSeats)
{
    // Two human tanks in a 5-man: no bot tank, and the extra tank's seat comes
    // out of the damage dealers.
    Plan const plan = Compute(5, C(2, 0, 0), C(0, 0, 0));
    ExpectCounts(plan.add, C(0, 1, 2));
    EXPECT_EQ(plan.add.Total() + 2, 5u);
}

TEST(DcAssemblePlanner, FullGroupGetsNothing)
{
    Plan const plan = Compute(5, C(1, 1, 3), C(0, 0, 0));
    EXPECT_EQ(plan.add.Total(), 0u);
}

TEST(DcAssemblePlanner, OthersOverTheSizeAddNothing)
{
    Plan const plan = Compute(5, C(0, 0, 7), C(0, 0, 0));
    EXPECT_EQ(plan.add.Total(), 0u);
    EXPECT_EQ(plan.wanted.Total(), 0u);
}

TEST(DcAssemblePlanner, ShrinkingRemovesTheSurplus)
{
    // A 40 assembled for a damage dealer, then cut to 10 (2/2/6).
    Plan const plan = Compute(10, C(0, 0, 1), C(4, 10, 25));
    ExpectCounts(plan.wanted, C(2, 2, 5));
    ExpectCounts(plan.remove, C(2, 8, 20));
    EXPECT_EQ(plan.add.Total(), 0u);
}

TEST(DcAssemblePlanner, GrowingAddsOnlyTheDifference)
{
    Plan const plan = Compute(10, C(0, 0, 1), C(1, 1, 2));
    ExpectCounts(plan.add, C(1, 1, 3));
    EXPECT_EQ(plan.remove.Total(), 0u);
}

TEST(DcAssemblePlanner, SameSizeAgainChangesNothing)
{
    Plan const plan = Compute(5, C(0, 0, 1), C(1, 1, 2));
    EXPECT_EQ(plan.add.Total(), 0u);
    EXPECT_EQ(plan.remove.Total(), 0u);
}

TEST(DcAssemblePlanner, SizeIsClamped)
{
    EXPECT_EQ(Compute(1, C(0, 0, 1), C(0, 0, 0)).quota.Total(), kMinSize);
    EXPECT_EQ(Compute(99, C(0, 0, 1), C(0, 0, 0)).quota.Total(), kMaxSize);
}

TEST(DcAssemblePlanner, DrawSpreadsClasses)
{
    std::map<std::uint8_t, std::size_t> classUse;
    std::uint32_t state = 12345;
    std::vector<DcTestComp::Slot> const healers =
        DrawRole("heal", 8, DcTestComp::Roster::NoDeathKnights, classUse, state);
    ASSERT_EQ(healers.size(), 8u);
    for (DcTestComp::Slot const& slot : healers)
        EXPECT_EQ(std::string(slot.role), "heal");
    // Four healing classes, eight healers: two of each.
    for (auto const& [classId, uses] : classUse)
        EXPECT_EQ(uses, 2u) << int(classId);
}

TEST(DcAssemblePlanner, DrawAvoidsTheGroupsClasses)
{
    // The group already has a warrior and a paladin: the tank is the druid.
    std::map<std::uint8_t, std::size_t> classUse{ { 1, 1 }, { 2, 1 } };
    std::uint32_t state = 7;
    std::vector<DcTestComp::Slot> const tanks =
        DrawRole("tank", 1, DcTestComp::Roster::NoDeathKnights, classUse, state);
    ASSERT_EQ(tanks.size(), 1u);
    EXPECT_EQ(tanks[0].classId, 11);
}

TEST(DcAssemblePlanner, NoDeathKnightsBeforeWrath)
{
    std::map<std::uint8_t, std::size_t> classUse;
    std::uint32_t state = 99;
    for (DcTestComp::Slot const& slot :
         DrawRole("dps", 30, DcTestComp::Roster::NoDeathKnights, classUse, state))
        EXPECT_NE(slot.classId, 6);
}
