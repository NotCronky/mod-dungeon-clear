/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Assemble/DcAssemblePlanner.h"

#include <algorithm>
#include <cstdint>

#include "DungeonQueueFill/DcDungeonQueueFillPlanner.h"

namespace
{
    std::size_t Short(std::size_t quota, std::size_t have)
    {
        return quota > have ? quota - have : 0;
    }

    // Take up to `excess` seats off `n`; returns what is left of the excess.
    std::size_t Trim(std::size_t& n, std::size_t excess)
    {
        std::size_t const cut = std::min(n, excess);
        n -= cut;
        return excess - cut;
    }
}

namespace DcAssemblePlanner
{
    Plan Compute(std::size_t size, Counts const& others, Counts const& ours)
    {
        size = std::clamp(size, kMinSize, kMaxSize);

        Plan plan;
        DcTestComp::Quota const q = DcTestComp::RoleQuota(size);
        plan.quota = { q.tanks, q.healers, q.dps };

        Counts wanted;
        wanted.tanks = Short(q.tanks, others.tanks);
        wanted.healers = Short(q.healers, others.healers);
        wanted.dps = Short(q.dps, others.dps);

        std::size_t const room = size > others.Total() ? size - others.Total() : 0;
        if (wanted.Total() > room)
        {
            std::size_t excess = wanted.Total() - room;
            excess = Trim(wanted.dps, excess);
            excess = Trim(wanted.healers, excess);
            Trim(wanted.tanks, excess);
        }
        plan.wanted = wanted;

        plan.add = { Short(wanted.tanks, ours.tanks), Short(wanted.healers, ours.healers),
                     Short(wanted.dps, ours.dps) };
        plan.remove = { Short(ours.tanks, wanted.tanks), Short(ours.healers, wanted.healers),
                        Short(ours.dps, wanted.dps) };
        return plan;
    }

    std::vector<DcTestComp::Slot> DrawRole(char const* role, std::size_t count, DcTestComp::Roster roster,
                                           std::map<std::uint8_t, std::size_t>& classUse, std::uint32_t& state)
    {
        std::vector<DcTestComp::Slot> out;
        std::vector<DcTestComp::Slot> const pool = DcTestComp::RolePool(role, roster);
        if (pool.empty())
            return out;

        for (std::size_t n = 0; n < count; ++n)
        {
            std::size_t fewest = SIZE_MAX;
            for (DcTestComp::Slot const& slot : pool)
            {
                auto const it = classUse.find(slot.classId);
                fewest = std::min(fewest, it == classUse.end() ? std::size_t(0) : it->second);
            }

            std::vector<DcTestComp::Slot> candidates;
            for (DcTestComp::Slot const& slot : pool)
            {
                auto const it = classUse.find(slot.classId);
                if ((it == classUse.end() ? 0 : it->second) == fewest)
                    candidates.push_back(slot);
            }

            DcTestComp::Slot const pick =
                candidates[DcDungeonQueueFillPlanner::NextRand(state) % candidates.size()];
            ++classUse[pick.classId];
            out.push_back(pick);
        }
        return out;
    }
}
