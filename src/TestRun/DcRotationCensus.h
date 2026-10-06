/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCROTATIONCENSUS_H
#define _PLAYERBOT_DCROTATIONCENSUS_H

#include <cstdint>
#include <string>
#include <vector>

#include "ObjectGuid.h"

class Player;

// What each party member of a test run actually did with its spells, so a run
// shows how the class rotations play out and not just whether the party won.
//
// Two counts per spell, keyed by the first rank of its chain (every rank of
// Frostbolt is one line):
//   casts     - Spell::cast went through (the spell left the bot's hands).
//   rejected  - Spell::CheckCast said no. The bot AI asks CheckCast before it
//               casts (can I cast this now?), so these are mostly the AI's own
//               probes, not failed casts. A spell rejected hundreds of times for
//               the same reason with few casts is a rotation that keeps wanting
//               something it can't have: the wrong stance or form, a missing
//               reagent, a spell that needs a talent.
// Only spells in the bot's spellbook count: procs, item spells and triggered
// spells are not part of a rotation.
//
// Tracking runs from the Monitoring stage (the party starts clearing) to
// teardown, so the gearing, levelling and pre-run buffing of setup stay out.
namespace DcRotationCensus
{
    struct SpellLine
    {
        std::uint32_t spellId = 0;      // first rank of the chain
        std::string name;
        std::uint32_t casts = 0;
        std::uint32_t rejected = 0;
        std::uint32_t topReject = 0;    // SpellCastResult rejected with most often
        std::uint32_t topRejectCount = 0;
    };

    void Start(ObjectGuid guid);
    void Stop(ObjectGuid guid);

    // The member's lines, most cast first, and the active class abilities it
    // knows (highest rank) but never cast while tracked.
    void Collect(Player* player, std::vector<SpellLine>& lines, std::vector<std::string>& unused);
}

void AddSC_dungeon_clear_rotation_census();

#endif
