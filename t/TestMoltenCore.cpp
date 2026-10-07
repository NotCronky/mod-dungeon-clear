/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "gtest/gtest.h"

#include "Ai/Dungeon/DungeonClear/Data/DungeonEventRegistry.h"
#include "Ai/Dungeon/DungeonClear/Overrides/BossRosterRegistry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcDifficulty.h"

// Molten Core (map 409) — raid-support Plan E1 data. The eight statics derive
// from BossSpawnIndex at runtime; these tests pin the authored additions: Baron
// Geddon's rune objective, the two script-summoned bosses and the Ragnaros
// summon event.

namespace
{
    constexpr uint32 kMap = 409;

    std::vector<DungeonBossInfo> Statics()
    {
        // Stand-ins for the eight derived kill-credit bosses (bits 0-7).
        std::vector<DungeonBossInfo> base;
        uint32 const entries[] = {12118, 11982, 12259, 12057, 12264, 12056, 12098, 11988};
        for (uint32 i = 0; i < 8; ++i)
        {
            DungeonBossInfo b;
            b.entry = entries[i];
            b.encounterIndex = i;
            b.mapId = kMap;
            base.push_back(b);
        }
        return base;
    }
}

TEST(DcMoltenCoreTest, RosterAppendsTheFinaleInOrder)
{
    auto const out =
        BossRosterRegistry::Apply(kMap, DcDiffKey::Raid(0), Statics());
    ASSERT_EQ(out.size(), 18u);

    // Lucifron..Baron Geddon keep their derived order...
    for (uint32 i = 0; i < 6; ++i)
        EXPECT_EQ(out[i].encounterIndex, i) << i;
    // ...Geddon re-anchored at his pull-back camp on the near side of his loop,
    // on his own kill-bit.
    EXPECT_EQ(out[5].entry, 12056u);
    EXPECT_NEAR(out[5].x, 682.0f, 1.0f);
    EXPECT_NEAR(out[5].y, -714.0f, 1.0f);

    // ...then Geddon's rune, which he can die ~240yd away from on his patrol...
    EXPECT_EQ(out[6].kind, DungeonAnchorKind::Objective);
    EXPECT_EQ(out[6].name, "Douse Rune of Zeth");
    EXPECT_EQ(out[6].doneGoEntry, 176952u);
    EXPECT_GT(BossOrderKey(out[6]), BossOrderKey(out[5]));

    // ...then Sulfuron and Golemagg, reordered past it on their own kill-bits...
    EXPECT_EQ(out[7].entry, 12098u);
    EXPECT_EQ(out[7].encounterIndex, 6u);
    EXPECT_EQ(out[8].entry, 11988u);
    EXPECT_EQ(out[8].encounterIndex, 7u);

    // ...then the sweep over the other six runes, each done once doused, so
    // Majordomo is never reached with one missed...
    uint32 const sweep[] = {176954, 176951, 176953, 176955, 176957, 176956};
    for (uint32 i = 0; i < 6; ++i)
    {
        EXPECT_EQ(out[9 + i].kind, DungeonAnchorKind::Objective) << i;
        EXPECT_EQ(out[9 + i].doneGoEntry, sweep[i]) << i;
        EXPECT_GT(BossOrderKey(out[9 + i]), BossOrderKey(out[8 + i])) << i;
    }

    // ...then Majordomo, the summon objective, and Ragnaros.
    EXPECT_EQ(out[15].entry, 12018u);
    EXPECT_EQ(out[15].kind, DungeonAnchorKind::Boss);
    EXPECT_EQ(out[15].doneBossStateIndex, 8);
    // Boss-state completion parks the encounterIndex out of mask range so no
    // static boss's bit can ever be misread as theirs.
    EXPECT_GE(out[15].encounterIndex, 32u);

    EXPECT_EQ(out[16].kind, DungeonAnchorKind::Objective);
    EXPECT_EQ(out[16].eventId, 1u);
    EXPECT_EQ(out[16].gateEntry, 11502u);  // a live Ragnaros satisfies it

    EXPECT_EQ(out[17].entry, 11502u);
    EXPECT_EQ(out[17].kind, DungeonAnchorKind::Boss);
    EXPECT_EQ(out[17].doneBossStateIndex, 9);
    EXPECT_GE(out[17].encounterIndex, 32u);
}

TEST(DcMoltenCoreTest, SummonRagnarosEventShape)
{
    DungeonEvent const* ev = DungeonEventRegistry::Find(kMap, 1);
    ASSERT_NE(ev, nullptr);
    EXPECT_EQ(ev->activation, EventActivation::Anchored);
    EXPECT_TRUE(ev->required);

    ASSERT_EQ(ev->steps.size(), 3u);
    EXPECT_EQ(ev->steps[0].kind, EventStepKind::Wait);

    EventStep const& gossip = ev->steps[1];
    EXPECT_EQ(gossip.kind, EventStepKind::Gossip);
    EXPECT_EQ(gossip.creatureEntry, 12018u);
    EXPECT_EQ(gossip.gossipOption, 0);
    // Re-entry with Ragnaros already summoned has no gossip Majordomo — skip.
    EXPECT_TRUE(gossip.skipIfMissing);

    EventStep const& wait = ev->steps[2];
    EXPECT_EQ(wait.kind, EventStepKind::WaitForSpawn);
    EXPECT_EQ(wait.creatureEntry, 11502u);
    EXPECT_TRUE(wait.wantAlive);
    // Must outlast the ~48s scripted intro.
    EXPECT_GE(wait.timeoutMs, 60000u);
}
