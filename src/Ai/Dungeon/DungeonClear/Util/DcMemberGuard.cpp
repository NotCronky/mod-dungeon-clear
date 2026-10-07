/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcMemberGuard.h"

#include <vector>

#include "Creature.h"
#include "Map.h"
#include "Player.h"
#include "Playerbots.h"
#include "Ai/Dungeon/DungeonClear/DcValueKeys.h"
#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Ai/Dungeon/DungeonClear/Util/DcEngageGeometry.h"
#include "Ai/Dungeon/DungeonClear/Util/DcThreatMap.h"

namespace
{
    // Mobs farther than this are never a threat and never block a step-out:
    // the largest aggro reach plus the step-out distance, with room to spare.
    constexpr float kScanYd = 60.0f;
}

bool DcMemberGuard::Evaluate(Player* bot, AiObjectContext* ctx, Result& out)
{
    if (!bot || !ctx || bot->isDead() || !DcSettings::GetBool(bot, "ProximityGuard"))
        return false;

    // A follower of an active clear: the elected tank is non-null only then, and
    // the tank itself never steps out (moving the fight is its own guard's job).
    Player* tank = ctx->GetValue<Player*>(DcKey::PartyTank)->Get();
    if (!tank || tank == bot || tank->GetMapId() != bot->GetMapId())
        return false;

    if (bot->HasUnitState(UNIT_STATE_STUNNED | UNIT_STATE_FLEEING | UNIT_STATE_CONFUSED | UNIT_STATE_ROOT))
        return false;

    // In combat, melee are pinned to their target.
    if (bot->IsInCombat() && !PlayerbotAI::IsHeal(bot) && !PlayerbotAI::IsRanged(bot))
        return false;

    Map* map = bot->GetMap();
    DcThreatMap const* tm = DcThreatMap::Get(map);
    if (!tm)
        return false;

    float const margin = DcSettings::GetFloat(bot, "ProximityGuardMargin");
    float const hysteresis = DcSettings::GetFloat(bot, "ProximityGuardHysteresis");

    std::vector<DcProximityGuard::Threat> threats;
    std::vector<ObjectGuid> guids;
    for (DcThreatEntry const* e : tm->AliveWithin(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), kScanYd))
    {
        if (!e->loaded || e->inCombat || e->evading || e->guid.IsEmpty())
            continue;
        Creature* c = map->GetCreature(e->guid);
        if (!c || !c->IsAlive() || c->IsInCombat() || c->IsCivilian() || c->HasReactState(REACT_PASSIVE) ||
            !c->IsHostileTo(bot) ||
            c->HasUnitFlag(UNIT_FLAG_NON_ATTACKABLE | UNIT_FLAG_IMMUNE_TO_PC | UNIT_FLAG_NOT_SELECTABLE))
            continue;
        float const reach = DcEngageGeometry::AggroReach(bot, c, 0.0f);
        // Aggro needs line of sight, so a pack behind a wall is not a threat. Only
        // tested for a mob close enough to matter (VMAP LOS per mob per bot per
        // tick is the cost here); a farther one still counts against the step-out
        // destination, conservatively.
        if (bot->GetDistance(c) < reach + margin && !c->IsWithinLOSInMap(bot))
            continue;
        threats.push_back({c->GetPositionX(), c->GetPositionY(), c->GetPositionZ(), reach});
        guids.push_back(e->guid);
    }
    if (threats.empty())
        return false;

    out.decision = DcProximityGuard::Decide(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), threats,
                                            tank->GetPositionX(), tank->GetPositionY(), tank->GetPositionZ(), margin,
                                            hysteresis);
    if (!out.decision.threatened)
        return false;

    DcProximityGuard::Threat const& w = threats[out.decision.worst];
    out.worst = guids[out.decision.worst];
    out.worstReach = w.reach;
    out.worstDist = DcProximityGuard::Dist3(bot->GetPositionX(), bot->GetPositionY(), bot->GetPositionZ(), w.x, w.y,
                                            w.z);
    return true;
}
