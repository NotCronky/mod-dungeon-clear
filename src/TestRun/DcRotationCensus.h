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
//   casts   - Spell::cast went through (the spell left the bot's hands).
//   failed  - the bot started a cast and the server refused it, with the
//             SMSG_CAST_FAILED reason (the packet a client would have shown as
//             an error). The bot AI's own "can I cast this?" checks send
//             nothing and are not counted. A spell that fails again and again
//             for the same reason is a rotation reaching for something it
//             can't use: the wrong stance or form, out of range, no reagent.
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
        std::uint32_t failed = 0;
        std::uint32_t topFail = 0;      // SpellCastResult it failed with most often
        std::uint32_t topFailCount = 0;
    };

    void Start(ObjectGuid guid);
    void Stop(ObjectGuid guid);

    // The member's lines, most cast first, and the active class abilities it
    // knows (highest rank) but never cast while tracked.
    void Collect(Player* player, std::vector<SpellLine>& lines, std::vector<std::string>& unused);
}

void AddSC_dungeon_clear_rotation_census();

#endif
