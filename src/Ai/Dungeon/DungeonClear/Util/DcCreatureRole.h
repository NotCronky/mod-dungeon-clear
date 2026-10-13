/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCCREATUREROLE_H
#define _PLAYERBOT_DCCREATUREROLE_H

#include <cstdint>

struct CreatureTemplate;

// What a creature can do to the party, read from its data rather than guessed
// from its name: the spells it casts (its SmartAI casts and its template spells)
// and its SmartAI flee-for-help.
//
//   Healer — casts a heal (it keeps its pack alive: kill it first).
//   Caster — casts a ranged harmful spell, or is a mage-class creature (it hits
//            from range, so it does not stand in the tank's pile).
//   Flees  — runs for help at low health (SMART_ACTION_FLEE_FOR_ASSIST) and
//            brings back the nearest creature it finds.
//
// C++-scripted creatures are invisible to this (their spells live in code), so a
// creature reads as none of these unless its data says otherwise.
//
// Cached per entry for the server's lifetime, behind a mutex: the pull planner
// asks from the map threads and the focus marker from the world thread.
namespace DcCreatureRole
{
    enum Flag : std::uint8_t
    {
        None   = 0,
        Healer = 1,
        Caster = 2,
        Flees  = 4,
    };

    std::uint8_t FlagsFor(CreatureTemplate const* tmpl);
}

#endif  // _PLAYERBOT_DCCREATUREROLE_H
