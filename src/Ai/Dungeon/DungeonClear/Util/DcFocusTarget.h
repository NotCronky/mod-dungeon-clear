/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCFOCUSTARGET_H
#define _PLAYERBOT_DCFOCUSTARGET_H

#include "Define.h"

// The party's kill target in a trash fight, marked with the skull.
//
// Without a mark every DPS bot picks its own target (playerbots' stock `dps
// target`: the least-health mob in its own range, else the closest), so a pack's
// damage spreads over several mobs and the openers land on mobs the tank has not
// touched. A skull is the one thing every bot's picker puts first
// (`RtiTargetValue`, `FindTargetStrategy::IsHighPriority`), so marking one mob
// is enough to make the party focus it.
//
// Which mob: DungeonClearMath::PickFocusTarget — a fleeing mob, then healers, then
// casters (DcCreatureRole), then the rest; a mob on a tank before a loose one;
// then the least health. The mark is kept until something of a better rank shows
// up or it dies.
//
// What it leaves alone:
//   - boss fights (a boss among the enemies): encounter strategies mark their own
//     targets, and the module's exclusion rows decide what may be hit;
//   - a skull somebody else set (a player, an encounter strategy) on a live mob;
//   - mobs under breakable crowd control, and the module's never-target and
//     exclusion rows.
// It only ever clears a skull it set itself.
//
// World thread (the module's world tick), throttled: group icons are changed
// while the maps are idle. DungeonClear.FocusTarget switches it.
namespace DcFocusTarget
{
    void Tick(uint32 diff);
}

#endif  // _PLAYERBOT_DCFOCUSTARGET_H
