/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCMEMBERGUARD_H
#define _PLAYERBOT_DCMEMBERGUARD_H

#include "ObjectGuid.h"
#include "DcProximityGuard.h"

class AiObjectContext;
class Player;

// The member proximity guard, live side: a party member (never the leader tank)
// standing inside an idle pack's aggro steps out of it. Shared by
// DungeonClearMemberGuardTrigger and DungeonClearMemberGuardAction so the two
// agree on the same tick.
//
// Who: in combat, healers and ranged only (melee are pinned to their target;
// moving the fight is the tank's job). Out of combat, every follower: resting,
// looting and drinking are where a healer drifts into the next pack.
//
// What counts: living, loaded, hostile creatures from the threat map that are
// NOT in combat (a pack already fighting us is the fight, not a bystander), not
// evading, able to aggro, and in line of sight. Each one's reach is its aggro
// reach against this member (DcEngageGeometry::AggroReach).
//
// Gated by DungeonClear.ProximityGuard (raids on, dungeons off for now).
namespace DcMemberGuard
{
    struct Result
    {
        DcProximityGuard::Decision decision;
        ObjectGuid worst;           // the mob that triggered it
        float worstDist{0.0f};
        float worstReach{0.0f};
    };

    // True, with `out` filled, when `bot` should step out of an idle pack's aggro.
    bool Evaluate(Player* bot, AiObjectContext* ctx, Result& out);
}

#endif  // _PLAYERBOT_DCMEMBERGUARD_H
