/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCASSEMBLEPLANNER_H
#define _PLAYERBOT_DCASSEMBLEPLANNER_H

#include <cstddef>
#include <cstdint>
#include <map>
#include <vector>

#include "TestRun/DcTestComp.h"

// The role arithmetic behind `.dc assemble <size>`: given the group the player
// already has, how many bots of each role does it still need, and how many of
// the bots an earlier assemble added are now surplus?
//
// Engine-free, like DcDungeonQueueFillPlanner: the quota is DcTestComp's
// (1/1/3 for a 5-man, tanks stepwise and healers at ~N/4 for a raid), and the
// members who are not ours — the player, other humans, the player's own alt
// bots — fill their roles first. Turning the plan into logins, factory rolls
// and group joins belongs to DcAssembleJob.
namespace DcAssemblePlanner
{
    inline constexpr std::size_t kMinSize = DcTestComp::kMinPartySize;
    inline constexpr std::size_t kMaxSize = DcTestComp::kMaxPartySize;

    struct Counts
    {
        std::size_t tanks = 0;
        std::size_t healers = 0;
        std::size_t dps = 0;

        std::size_t Total() const { return tanks + healers + dps; }
    };

    struct Plan
    {
        Counts quota;   // the size's role quota
        Counts wanted;  // the bots of each role the group should hold
        Counts add;     // bots to add (wanted - ours, floored at 0)
        Counts remove;  // of ours, surplus (ours - wanted, floored at 0)
    };

    // `size` is clamped to [kMinSize, kMaxSize]. `others` are the members that
    // are not ours, by role; `ours` the bots an earlier assemble added.
    //
    // A role the others over-fill (two human tanks in a 5-man) needs no bots,
    // and the seats it takes come out of the damage dealers first, then the
    // healers, then the tanks: a group the others already fill gets no bots.
    Plan Compute(std::size_t size, Counts const& others, Counts const& ours);

    // `count` slots for `role` ("tank" | "heal" | "dps"), each on the class the
    // group has least of (`classUse`, updated as it draws), ties broken off
    // `state` (DcDungeonQueueFillPlanner::NextRand). A raid repeats classes;
    // this spreads them.
    std::vector<DcTestComp::Slot> DrawRole(char const* role, std::size_t count, DcTestComp::Roster roster,
                                           std::map<std::uint8_t, std::size_t>& classUse, std::uint32_t& state);
}

#endif  // _PLAYERBOT_DCASSEMBLEPLANNER_H
