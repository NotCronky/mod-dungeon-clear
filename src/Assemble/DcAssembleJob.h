/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#ifndef _PLAYERBOT_DCASSEMBLEJOB_H
#define _PLAYERBOT_DCASSEMBLEJOB_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "ObjectGuid.h"

#include "Assemble/DcAssemblePlanner.h"
#include "TestRun/DcTestComp.h"

class Group;
class Player;

// The bots `.dc assemble` added to one player's group.
//
// Each bot is an addclass-pool character, set up the way a dungeon queue fill
// sets up its bots (claimed through DcPoolBots, logged in masterless, put down
// in the faction capital, one DcBotProvisioning::Roll per world tick under the
// shared budget, sanitised), then added to the player's group and teleported to
// the player. Once there it is an ordinary party bot: the random-bot manager's
// player scan makes the player its master, and DcStrategyGate gives it the
// dungeon-clear strategies inside a dungeon.
//
// The job lives as long as the player keeps any of its bots. A later
// `.dc assemble` re-plans it (Adjust), `.dc dismiss` releases it, and a bot the
// player removes from the group is logged out. Setup stages are bounded by
// Assemble.SetupTimeoutSec; a bot that misses a stage is dropped, not waited on.
//
// World thread only: the command handlers and the world-tick hook.
class DcAssembleJob
{
public:
    enum class Stage : std::uint8_t
    {
        Claiming,      // draw an offline pool character per new slot, start the logins
        LoggingIn,     // wait for IsInWorld + bot AI
        Evicting,      // put each new bot down in its faction capital
        Provisioning,  // one PlayerbotFactory roll per world tick (shared budget)
        Sanitizing,    // out of any queue or group, alive, out of combat
        Joining,       // into the player's group, then teleported to the player
        Ready,         // the group holds them; watch it
        Done,
    };

    static char const* StageName(Stage s);

    struct Slot
    {
        DcTestComp::Slot plan{};
        ObjectGuid guid;
        std::string name;
        bool relocated = false;
        bool provisioned = false;
        bool lfgLeaveSent = false;
        bool sanitized = false;
        bool joined = false;
    };

    // A job for `player`, with nothing planned yet.
    static std::unique_ptr<DcAssembleJob> Create(Player* player);

    // Re-plan for a group of `size`. `playerRole` is the role the player plays
    // ("tank" | "heal" | "dps"), or empty to read it from their spec. Surplus
    // bots are released at once; missing ones start setting up. Refused (false,
    // with the reason in `msg`) while a setup is still running. `msg` says what
    // was planned.
    bool Adjust(Player* player, std::size_t size, std::string const& playerRole, std::string* msg);

    // Release every bot and finish. Idempotent.
    void Release(std::string const& reason);

    void Tick(std::uint32_t diff);

    bool Done() const { return _stage == Stage::Done; }
    bool SettingUp() const { return _stage != Stage::Ready && _stage != Stage::Done; }
    ObjectGuid PlayerGuid() const { return _playerGuid; }
    bool HoldsBot(ObjectGuid guid) const;
    std::string StatusLine() const;

private:
    DcAssembleJob() = default;

    void EnterStage(Stage s);
    void TickClaiming();
    void TickLoggingIn();
    void TickEvicting();
    void TickProvisioning();
    void TickSanitizing();
    void TickJoining();
    void TickReady();

    Player* FindPlayer() const;
    void Tell(std::string const& text) const;

    // The group the bots join: the player's own, not a battleground's.
    static Group* PlayerGroup(Player* player);

    // Members that are not ours, by role, and the bots that are.
    DcAssemblePlanner::Counts CountOthers(Player* player, std::string const& playerRole) const;
    DcAssemblePlanner::Counts CountOurs() const;

    // Release slot `i` (log the bot out) and drop it.
    void ReleaseSlot(std::size_t i, char const* why);

    // Release the slots still being set up that `refuse` matches.
    template <class Pred>
    void DropPending(Pred refuse, char const* why);

    // The setup is over: say what the group now holds.
    void FinishSetup();

    bool Pending(Slot const& slot) const { return !slot.joined; }
    std::uint32_t SetupTimeoutMs() const;

    std::string _id;
    ObjectGuid _playerGuid;
    std::string _playerName;
    std::uint32_t _level = 0;
    bool _isAlliance = true;
    std::string _playerRole;             // "tank" | "heal" | "dps": what the player plays
    DcTestComp::Roster _roster = DcTestComp::Roster::NoDeathKnights;

    std::uint32_t _gearQuality = 0;
    std::uint32_t _gearScoreLimit = 0;
    std::uint32_t _gearIlvl = 0;

    std::uint32_t _drawState = 0;
    std::size_t _size = 0;
    ObjectGuid _groupGuid;
    std::vector<Slot> _slots;
    std::vector<std::string> _dropped;   // this setup's bots that didn't make it, for the report

    Stage _stage = Stage::Ready;
    std::uint32_t _stageMs = 0;
};

#endif  // _PLAYERBOT_DCASSEMBLEJOB_H
