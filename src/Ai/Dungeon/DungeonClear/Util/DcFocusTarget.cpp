/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcFocusTarget.h"

#include "Ai/Dungeon/DungeonClear/Data/DcNeverTargetRegistry.h"
#include "Ai/Dungeon/DungeonClear/Data/DcTargetExclusionRegistry.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Util/DcCreatureRole.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Ai/Dungeon/DungeonClear/Util/DcStatusPublisher.h"
#include "Ai/Dungeon/DungeonClear/Util/DungeonClearMath.h"

#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "CombatManager.h"
#include "Creature.h"
#include "Group.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "Playerbots.h"

namespace
{
    constexpr uint32 kIntervalMs = 300;
    constexpr float kFocusRange = 40.0f;  // from the tank: the fight, not the next room
    constexpr uint8 kSkull = 7;

    uint32 g_accumMs = 0;
    // Tank -> the mob it marked. World thread only.
    std::unordered_map<ObjectGuid, ObjectGuid> g_marked;

    void ClearOurs(ObjectGuid tankGuid, Group* group)
    {
        auto const it = g_marked.find(tankGuid);
        if (it == g_marked.end())
            return;
        if (group && group->GetTargetIcon(kSkull) == it->second)
            group->SetTargetIcon(kSkull, tankGuid, ObjectGuid::Empty);
        g_marked.erase(it);
    }

    bool HoldsOtherIcon(Group* group, ObjectGuid guid)
    {
        for (uint8 i = 0; i < TARGETICONCOUNT; ++i)
            if (i != kSkull && group->GetTargetIcon(i) == guid)
                return true;
        return false;
    }

    DungeonClearMath::FocusRank RankOf(Creature* c)
    {
        if (c->HasUnitState(UNIT_STATE_FLEEING))
            return DungeonClearMath::FocusRank::Fleeing;
        std::uint8_t const flags = DcCreatureRole::FlagsFor(c->GetCreatureTemplate());
        if (flags & DcCreatureRole::Healer)
            return DungeonClearMath::FocusRank::Healer;
        if (flags & DcCreatureRole::Caster)
            return DungeonClearMath::FocusRank::Caster;
        return DungeonClearMath::FocusRank::Other;
    }

    void Update(Player* tank)
    {
        Group* group = tank->GetGroup();
        PlayerbotAI* botAI = GET_PLAYERBOT_AI(tank);
        Map* map = tank->GetMap();
        if (!group || !botAI || !map || !map->IsDungeon())
        {
            ClearOurs(tank->GetGUID(), group);
            return;
        }
        DcRunState const& run = DcRun::Of(botAI);
        if (!run.enabled || run.paused || !DcSettings::GetBool(tank, "FocusTarget"))
        {
            ClearOurs(tank->GetGUID(), group);
            return;
        }

        // Every creature in combat with a member of the party, once.
        std::vector<Creature*> mobs;
        std::unordered_set<ObjectGuid> seen;
        bool bossFight = false;
        for (GroupReference* ref = group->GetFirstMember(); ref; ref = ref->next())
        {
            Player* member = ref->GetSource();
            if (!member || !member->IsAlive() || member->GetMap() != map)
                continue;
            for (auto const& [guid, combat] : member->GetCombatManager().GetPvECombatRefs())
            {
                Unit* other = combat->GetOther(member);
                Creature* c = other ? other->ToCreature() : nullptr;
                if (!c || !seen.insert(c->GetGUID()).second)
                    continue;
                if (!c->IsAlive() || !c->IsInWorld() || c->IsTotem() || c->IsCritter() || !tank->IsHostileTo(c))
                    continue;
                if (c->IsDungeonBoss() || c->isWorldBoss())
                    bossFight = true;
                if (c->IsInEvadeMode() || c->HasBreakableByDamageCrowdControlAura() ||
                    tank->GetDistance(c) > kFocusRange || HoldsOtherIcon(group, c->GetGUID()))
                    continue;
                uint32 const entry = c->GetEntry();
                if (DcNeverTargetRegistry::IsNeverTarget(map->GetId(), entry) ||
                    DcTargetExclusionRegistry::IsExcluded(tank, map->GetId(), entry))
                    continue;
                mobs.push_back(c);
            }
        }

        if (bossFight || mobs.empty())
        {
            ClearOurs(tank->GetGUID(), group);
            return;
        }

        ObjectGuid const current = group->GetTargetIcon(kSkull);
        auto const ours = g_marked.find(tank->GetGUID());
        bool const currentIsOurs = ours != g_marked.end() && ours->second == current;
        if (current && !currentIsOurs)
        {
            // Somebody else's skull on a live mob is their call, not ours.
            Unit* marked = ObjectAccessor::GetUnit(*tank, current);
            if (marked && marked->IsAlive())
            {
                g_marked.erase(tank->GetGUID());
                return;
            }
        }

        std::vector<DungeonClearMath::FocusCandidate> candidates;
        candidates.reserve(mobs.size());
        int currentIdx = -1;
        for (std::size_t i = 0; i < mobs.size(); ++i)
        {
            Creature* c = mobs[i];
            Unit* victim = c->GetVictim();
            Player* victimPlayer = victim ? victim->GetCharmerOrOwnerPlayerOrPlayerItself() : nullptr;
            DungeonClearMath::FocusCandidate candidate;
            candidate.rank = RankOf(c);
            candidate.onTank = victimPlayer && PlayerbotAI::IsTank(victimPlayer);
            candidate.health = c->GetHealth();
            candidates.push_back(candidate);
            if (currentIsOurs && c->GetGUID() == current)
                currentIdx = static_cast<int>(i);
        }

        int const pick = DungeonClearMath::PickFocusTarget(candidates, currentIdx);
        if (pick < 0)
            return;
        Creature* chosen = mobs[static_cast<std::size_t>(pick)];
        if (chosen->GetGUID() == current)
            return;
        group->SetTargetIcon(kSkull, tank->GetGUID(), chosen->GetGUID());
        g_marked[tank->GetGUID()] = chosen->GetGUID();
        LOG_DEBUG("playerbots.dungeonclear", "[DC:{}] focus: skull on {} ({}, rank {}, {})", tank->GetName(),
                  chosen->GetName(), chosen->GetGUID().ToString(),
                  static_cast<int>(candidates[static_cast<std::size_t>(pick)].rank),
                  candidates[static_cast<std::size_t>(pick)].onTank ? "on a tank" : "loose");
    }
}

void DcFocusTarget::Tick(uint32 diff)
{
    g_accumMs += diff;
    if (g_accumMs < kIntervalMs)
        return;
    g_accumMs = 0;

    std::vector<ObjectGuid> const tanks = DcStatusPublisher::ActiveTanks();
    std::unordered_set<ObjectGuid> const active(tanks.begin(), tanks.end());

    // A run that ended (dc off, the tank gone) leaves no skull behind.
    for (auto it = g_marked.begin(); it != g_marked.end();)
    {
        if (active.count(it->first))
        {
            ++it;
            continue;
        }
        Player* tank = ObjectAccessor::FindPlayer(it->first);
        Group* group = tank ? tank->GetGroup() : nullptr;
        if (group && group->GetTargetIcon(kSkull) == it->second)
            group->SetTargetIcon(kSkull, it->first, ObjectGuid::Empty);
        it = g_marked.erase(it);
    }

    for (ObjectGuid const& guid : tanks)
        if (Player* tank = ObjectAccessor::FindPlayer(guid))
            if (tank->IsInWorld())
                Update(tank);
}
