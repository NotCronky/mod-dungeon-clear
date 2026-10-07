/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCMOLTENCORERUNES_H
#define _PLAYERBOT_DCMOLTENCORERUNES_H

#include <string>

#include "Define.h"

class Map;
class Player;

// Molten Core's Firelord runes, for callers outside the leader's douse rung
// (the test harness). See DcMoltenCoreRunes.cpp.
namespace DcMoltenCore
{
    constexpr uint32 MAP_ID = 409;
    constexpr uint32 NPC_MAJORDOMO = 12018;

    // Ready a Majordomo-only test run on `map`, whose bosses the caller has
    // already killed: mark the eight boss slots DONE (a backstop for a boss
    // whose death did not), then douse every rune still waiting, by `user`.
    // The instance script summons Majordomo and his adds once all seven are
    // out. Returns a short summary for the run log.
    std::string ForceMajordomo(Map* map, Player* user);
}

#endif
