/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCASSEMBLEMANAGER_H
#define _PLAYERBOT_DCASSEMBLEMANAGER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ObjectGuid.h"

class DcAssembleJob;
class Player;

// `.dc assemble` and `.dc dismiss`: one DcAssembleJob per player, ticked from
// the module's world tick. Its bots count as claimed for the queue fills and the
// test harness (DcPoolBots::IsClaimedByFill), so nothing else draws them.
//
// World thread only.
class DcAssembleManager
{
public:
    static DcAssembleManager& Instance();

    // Size the player's group to `size` (see DcAssembleJob::Adjust). `role` is
    // "tank" | "heal" | "dps", or empty to read it from the player's spec.
    bool Assemble(Player* player, std::size_t size, std::string const& role, std::string* msg);

    // Log out every bot `.dc assemble` added for the player.
    bool Dismiss(Player* player, std::string* msg);

    // The player's assemble, or a line saying there is none.
    std::string StatusFor(Player* player) const;

    void OnPlayerLogout(Player* player);
    void Tick(std::uint32_t diff);
    void ReleaseAll(std::string const& reason);
    bool IsClaimed(ObjectGuid guid) const;

private:
    DcAssembleManager() = default;

    DcAssembleJob* JobFor(ObjectGuid player) const;

    std::vector<std::unique_ptr<DcAssembleJob>> _jobs;
};

#endif  // _PLAYERBOT_DCASSEMBLEMANAGER_H
