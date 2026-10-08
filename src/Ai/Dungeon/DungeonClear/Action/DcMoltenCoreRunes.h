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
    constexpr uint32 NPC_RAGNAROS = 11502;

    // Ready a Majordomo-only test run on `map`, whose bosses the caller has
    // already killed: mark the eight boss slots DONE (a backstop for a boss
    // whose death did not), then douse every rune still waiting, by `user`.
    // The instance script summons Majordomo and his adds once all seven are
    // out. Returns a short summary for the run log.
    std::string ForceMajordomo(Map* map, Player* user);

    // Ready a Ragnaros-only test run the same way, with Majordomo's encounter
    // marked DONE first: the instance script then summons him friendly at
    // Ragnaros' lair, where the run's "Summon Ragnaros" objective talks to him.
    std::string ForceRagnaros(Map* map, Player* user);

    // Douse, by `user`, every rune whose boss is dead and that is still waiting
    // (the runes a `from=` test start leaves behind by killing the bosses before
    // it). Sets *missing to the runes whose spawn is not loaded. Returns how
    // many it doused.
    uint32 DouseReadyRunes(Map* map, Player* user, uint32* missing = nullptr);

    // Load the grid of every rune (and the fire circle beside it). The instance
    // script only readies a rune on its boss's kill if it has seen the rune
    // spawn, so a test start that kills bosses loads them first.
    void LoadRuneGrids(Map* map);

    // Runes still waiting to be doused (a killed boss's, active and selectable).
    uint32 RunesAwaitingDouse(Map* map);
}

#endif
