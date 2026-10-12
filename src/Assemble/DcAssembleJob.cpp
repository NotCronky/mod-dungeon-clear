/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Assemble/DcAssembleJob.h"

#include <algorithm>
#include <array>
#include <ctime>
#include <functional>
#include <map>
#include <utility>

#include "Chat.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "World.h"

#include "PlayerbotAI.h"
#include "PlayerbotFactory.h"
#include "PlayerbotMgr.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"

#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "DungeonQueueFill/DcDungeonQueueFillPlanner.h"
#include "TestRun/DcTestGearTiers.h"
#include "Util/DcBotProvisioning.h"
#include "Util/DcDungeonAccess.h"
#include "Util/DcPlayerbotsConfig.h"
#include "Util/DcPoolBots.h"
#include "Util/DcProvisionBudget.h"

namespace
{
    constexpr char const* kTag = "ASSEMBLE";

    std::string MakeAssembleId()
    {
        static std::uint32_t counter = 0;
        std::time_t const now = std::time(nullptr);
        std::tm tmBuf{};
        localtime_r(&now, &tmBuf);
        char buf[32];
        std::strftime(buf, sizeof(buf), "as-%Y%m%d-%H%M%S", &tmBuf);
        return std::string(buf) + "-" + std::to_string(++counter);
    }

    // A member's role: a bot's from its strategies, a human's from their talents.
    char const* RoleOf(Player* member)
    {
        if (PlayerbotAI::IsTank(member))
            return "tank";
        if (PlayerbotAI::IsHeal(member))
            return "heal";
        return "dps";
    }

    void Count(DcAssemblePlanner::Counts& counts, std::string const& role)
    {
        if (role == "tank")
            ++counts.tanks;
        else if (role == "heal")
            ++counts.healers;
        else
            ++counts.dps;
    }

    using RoleCounts = std::array<std::pair<char const*, std::size_t>, 3>;

    RoleCounts ByRole(DcAssemblePlanner::Counts const& c)
    {
        return { { { "tank", c.tanks }, { "heal", c.healers }, { "dps", c.dps } } };
    }

    std::string CountsText(DcAssemblePlanner::Counts const& c)
    {
        return std::to_string(c.tanks) + (c.tanks == 1 ? " tank, " : " tanks, ") + std::to_string(c.healers) +
               (c.healers == 1 ? " healer, " : " healers, ") + std::to_string(c.dps) + " damage";
    }

    // Log a released bot out through whichever holder has it: the masterless
    // login files it under the random-bot manager, but the player scan that
    // makes the player its master can have re-filed it under the player's.
    void LogoutBot(ObjectGuid guid, Player* player)
    {
        if (sRandomPlayerbotMgr.GetPlayerBot(guid))
        {
            sRandomPlayerbotMgr.LogoutPlayerBot(guid);
            return;
        }
        if (player)
            if (PlayerbotMgr* mgr = GET_PLAYERBOT_MGR(player))
                if (mgr->GetPlayerBot(guid))
                {
                    mgr->LogoutPlayerBot(guid);
                    return;
                }
        if (ObjectAccessor::FindConnectedPlayer(guid))
            LOG_WARN("playerbots.dungeonclear", "{} bot {} is online but owned by no holder — left logged in",
                     kTag, guid.ToString());
    }
}

char const* DcAssembleJob::StageName(Stage s)
{
    switch (s)
    {
        case Stage::Claiming:     return "claiming";
        case Stage::LoggingIn:    return "logging_in";
        case Stage::Evicting:     return "evicting";
        case Stage::Provisioning: return "provisioning";
        case Stage::Sanitizing:   return "sanitizing";
        case Stage::Joining:      return "joining";
        case Stage::Ready:        return "ready";
        case Stage::Done:         return "done";
    }
    return "?";
}

std::unique_ptr<DcAssembleJob> DcAssembleJob::Create(Player* player)
{
    if (!player)
        return nullptr;

    std::unique_ptr<DcAssembleJob> job(new DcAssembleJob());
    job->_id = MakeAssembleId();
    job->_playerGuid = player->GetGUID();
    job->_playerName = player->GetName();
    job->_isAlliance = player->GetTeamId(true) == TEAM_ALLIANCE;
    job->_drawState = static_cast<std::uint32_t>(std::hash<std::string>{}(job->_id)) ^ 0x2545f491u;
    return job;
}

Player* DcAssembleJob::FindPlayer() const
{
    // Connected, not in world: a player on a loading screen is still here.
    return ObjectAccessor::FindConnectedPlayer(_playerGuid);
}

void DcAssembleJob::Tell(std::string const& text) const
{
    if (Player* player = FindPlayer())
        ChatHandler(player->GetSession()).SendSysMessage("|cff00ccff[Assemble]|r " + text);
}

Group* DcAssembleJob::PlayerGroup(Player* player)
{
    // Inside a battleground GetGroup() is the battleground's raid and the
    // player's own group waits in GetOriginalGroup().
    if (Group* original = player->GetOriginalGroup())
        return original;
    return player->GetGroup();
}

std::uint32_t DcAssembleJob::SetupTimeoutMs() const
{
    return DcSettings::GetUInt(ObjectGuid::Empty, "Assemble.SetupTimeoutSec") * 1000;
}

void DcAssembleJob::EnterStage(Stage s)
{
    _stage = s;
    _stageMs = 0;
}

bool DcAssembleJob::HoldsBot(ObjectGuid guid) const
{
    for (Slot const& slot : _slots)
        if (slot.guid == guid)
            return true;
    return false;
}

DcAssemblePlanner::Counts DcAssembleJob::CountOthers(Player* player, std::string const& playerRole) const
{
    DcAssemblePlanner::Counts counts;
    Count(counts, !playerRole.empty() ? playerRole : RoleOf(player));

    Group* grp = PlayerGroup(player);
    if (!grp)
        return counts;
    for (Group::MemberSlot const& member : grp->GetMemberSlots())
    {
        if (member.guid == player->GetGUID() || HoldsBot(member.guid))
            continue;
        // An offline member keeps their seat; their role is unknown, so it
        // counts as damage, the role with the most seats.
        Player* const other = ObjectAccessor::FindConnectedPlayer(member.guid);
        Count(counts, other ? RoleOf(other) : "dps");
    }
    return counts;
}

DcAssemblePlanner::Counts DcAssembleJob::CountOurs() const
{
    DcAssemblePlanner::Counts counts;
    for (Slot const& slot : _slots)
        Count(counts, slot.plan.role);
    return counts;
}

bool DcAssembleJob::Adjust(Player* player, std::size_t size, std::string const& playerRole, std::string* msg)
{
    if (SettingUp())
    {
        *msg = "Still setting up the last request (" + std::string(StageName(_stage)) + ").";
        return false;
    }
    if (player->InBattleground() || player->InArena())
    {
        *msg = "Not inside a battleground or arena.";
        return false;
    }
    Group* grp = PlayerGroup(player);
    if (grp && grp->isLFGGroup())
    {
        *msg = "Not in a Dungeon Finder group.";
        return false;
    }
    if (grp && grp->GetLeaderGUID() != player->GetGUID())
    {
        *msg = "Only the group leader can assemble bots.";
        return false;
    }

    // The bots match the player as they are now.
    _level = player->GetLevel();
    _roster = DcDungeonQueueFillPlanner::RosterFor(_level, sWorld->getIntConfig(CONFIG_EXPANSION));
    _size = std::clamp(size, DcAssemblePlanner::kMinSize, DcAssemblePlanner::kMaxSize);
    _playerRole = !playerRole.empty() ? playerRole : RoleOf(player);

    DcTestGearTiers::Spec spec;
    spec.ilvl = static_cast<std::int32_t>(DcSettings::GetUInt(ObjectGuid::Empty, "Assemble.GearIlvl"));
    spec.quality = DcSettings::GetUInt(ObjectGuid::Empty, "Assemble.GearQuality");
    DcTestGearTiers::Resolved const gear = DcTestGearTiers::Resolve(
        spec, DC_PB_CONFIG(AutoGearScoreLimit, autoGearScoreLimit),
        DC_PB_CONFIG(AutoGearQualityLimit, autoGearQualityLimit));
    _gearIlvl = gear.ilvl;
    _gearQuality = gear.quality;
    _gearScoreLimit = gear.ilvl == 0 ? 0 : PlayerbotFactory::CalcMixedGearScore(gear.ilvl, gear.quality);

    DcAssemblePlanner::Counts const others = CountOthers(player, _playerRole);
    DcAssemblePlanner::Plan const plan = DcAssemblePlanner::Compute(_size, others, CountOurs());

    // Surplus first, newest bots first.
    std::size_t removed = 0;
    for (auto const& [role, count] : ByRole(plan.remove))
        for (std::size_t n = 0; n < count; ++n)
            for (std::size_t i = _slots.size(); i-- > 0;)
                if (std::string(_slots[i].plan.role) == role)
                {
                    ReleaseSlot(i, "surplus");
                    ++removed;
                    break;
                }

    // Missing bots, spread over the classes the group has least of.
    std::map<std::uint8_t, std::size_t> classUse;
    if (grp)
        for (Group::MemberSlot const& member : grp->GetMemberSlots())
            if (Player* const other = ObjectAccessor::FindConnectedPlayer(member.guid))
                ++classUse[other->getClass()];
    if (!grp)
        ++classUse[player->getClass()];

    std::size_t added = 0;
    for (auto const& [role, count] : ByRole(plan.add))
        for (DcTestComp::Slot const& draw : DcAssemblePlanner::DrawRole(role, count, _roster, classUse, _drawState))
        {
            Slot slot;
            slot.plan = draw;
            _slots.push_back(slot);
            ++added;
        }

    std::string const playing = _playerRole == "heal" ? "healer" : _playerRole == "tank" ? "tank" : "damage";
    LOG_INFO("playerbots.dungeonclear",
             "{} {} {} (level {}, {}) asks for a group of {} as {}: others {}, adding {}, removing {}", kTag, _id,
             _playerName, _level, _isAlliance ? "Alliance" : "Horde", _size, playing, CountsText(others),
             CountsText(plan.add), CountsText(plan.remove));

    if (!added)
    {
        *msg = "Group of " + std::to_string(_size) + " (" + CountsText(plan.quota) + "), you as " + playing + ": " +
               (removed ? "removed " + std::to_string(removed) + " bot(s), nothing to add." : "nothing to change.");
        if (_slots.empty())
            EnterStage(Stage::Done);
        return true;
    }

    std::uint32_t const free =
        DcPoolBots::CountFree(_isAlliance, [this](ObjectGuid guid) { return HoldsBot(guid); });
    *msg = "Group of " + std::to_string(_size) + " (" + CountsText(plan.quota) + "), you as " + playing +
           ": adding " + CountsText(plan.add) + (removed ? ", removed " + std::to_string(removed) : "") +
           ". Setting them up...";
    if (free < added)
        *msg += " Only " + std::to_string(free) + " pool characters are free; seed more with "
                "`.playerbots bot addclass`.";

    _dropped.clear();
    EnterStage(Stage::Claiming);
    return true;
}

void DcAssembleJob::ReleaseSlot(std::size_t i, char const* why)
{
    Slot const slot = _slots[i];
    _slots.erase(_slots.begin() + static_cast<std::ptrdiff_t>(i));
    if (!slot.guid)
        return;

    Player* const bot = ObjectAccessor::FindPlayer(slot.guid);
    DcPoolBots::ReleaseBot(slot.guid, bot, /*logout*/ false, kTag, _id);
    LogoutBot(slot.guid, FindPlayer());
    LOG_INFO("playerbots.dungeonclear", "{} {} released {} ({} {}) — {}", kTag, _id,
             slot.name.empty() ? slot.guid.ToString() : slot.name, DcBotProvisioning::ClassToken(slot.plan.classId),
             slot.plan.role, why);
}

template <class Pred>
void DcAssembleJob::DropPending(Pred refuse, char const* why)
{
    for (std::size_t i = _slots.size(); i-- > 0;)
    {
        Slot const& slot = _slots[i];
        if (!Pending(slot) || !refuse(slot))
            continue;
        _dropped.push_back(std::string(DcBotProvisioning::ClassToken(slot.plan.classId)) + " " + slot.plan.role +
                           " (" + why + ")");
        ReleaseSlot(i, why);
    }
}

void DcAssembleJob::Release(std::string const& reason)
{
    if (_stage == Stage::Done)
        return;
    std::size_t const count = _slots.size();
    while (!_slots.empty())
        ReleaseSlot(_slots.size() - 1, reason.c_str());
    LOG_INFO("playerbots.dungeonclear", "{} {} released {} bot(s) — {}", kTag, _id, count, reason);
    EnterStage(Stage::Done);
}

void DcAssembleJob::Tick(std::uint32_t diff)
{
    if (_stage == Stage::Done)
        return;
    _stageMs += diff;

    if (!FindPlayer())
    {
        Release("player logged out");
        return;
    }

    switch (_stage)
    {
        case Stage::Claiming:     TickClaiming();     break;
        case Stage::LoggingIn:    TickLoggingIn();    break;
        case Stage::Evicting:     TickEvicting();     break;
        case Stage::Provisioning: TickProvisioning(); break;
        case Stage::Sanitizing:   TickSanitizing();   break;
        case Stage::Joining:      TickJoining();      break;
        case Stage::Ready:        TickReady();        break;
        case Stage::Done:         break;
    }
}

// One pool character per new slot. The drawn class is a preference: a class
// with no free character (vanilla has no Alliance shaman) gives way to another
// of the same role, and only a role with nothing free drops the slot.
void DcAssembleJob::TickClaiming()
{
    auto const held = [this](ObjectGuid guid) { return HoldsBot(guid); };
    for (Slot& slot : _slots)
    {
        if (!Pending(slot) || slot.guid)
            continue;
        slot.guid = DcPoolBots::ClaimPoolCharacter(_isAlliance, slot.plan.classId, _drawState, {}, held);
        if (slot.guid)
            continue;

        std::vector<DcTestComp::Slot> pool = DcTestComp::RolePool(slot.plan.role, _roster);
        for (std::size_t i = pool.size(); i > 1; --i)
            std::swap(pool[i - 1], pool[DcDungeonQueueFillPlanner::NextRand(_drawState) % i]);
        for (DcTestComp::Slot const& alt : pool)
        {
            ObjectGuid const guid = DcPoolBots::ClaimPoolCharacter(_isAlliance, alt.classId, _drawState, {}, held);
            if (!guid)
                continue;
            slot.plan = alt;
            slot.guid = guid;
            break;
        }
    }
    DropPending([](Slot const& slot) { return !slot.guid; }, "no free pool character");

    // Masterless, as the queue fill does: AddPlayerBot's ownership gate only
    // clears for the master's own characters, and masterAccountId 0 skips it.
    // The bots land in the random-bot manager without joining its rotation.
    for (Slot const& slot : _slots)
        if (Pending(slot) && !ObjectAccessor::FindConnectedPlayer(slot.guid))
            sRandomPlayerbotMgr.AddPlayerBot(slot.guid, 0);

    EnterStage(Stage::LoggingIn);
}

void DcAssembleJob::TickLoggingIn()
{
    bool allIn = true;
    for (Slot& slot : _slots)
    {
        if (!Pending(slot))
            continue;
        Player* const bot = ObjectAccessor::FindPlayer(slot.guid);
        if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
        {
            allIn = false;
            continue;
        }
        if (slot.name.empty())
            slot.name = bot->GetName();
    }

    if (!allIn && _stageMs < SetupTimeoutMs())
        return;
    DropPending([](Slot const& slot) { return slot.name.empty(); }, "did not log in");
    EnterStage(Stage::Evicting);
}

// A pool character logs in where it was saved, often inside a dungeon; a trip
// to the capital first clears what that leaves on it (see
// DcDungeonQueueFillJob::TickEvicting).
void DcAssembleJob::TickEvicting()
{
    bool allPlaced = true;
    for (Slot& slot : _slots)
    {
        if (!Pending(slot) || slot.relocated)
            continue;
        Player* const bot = ObjectAccessor::FindPlayer(slot.guid);
        if (bot && bot->IsInWorld() && GET_PLAYERBOT_AI(bot) &&
            DcPoolBots::EvictToCapital(bot, _isAlliance, kTag, _id))
            slot.relocated = true;
        else
            allPlaced = false;
    }

    if (!allPlaced && _stageMs < SetupTimeoutMs())
        return;
    DropPending([](Slot const& slot) { return !slot.relocated; }, "stuck teleporting");
    EnterStage(Stage::Provisioning);
}

// One factory roll per world tick, under the realm-wide ration the queue fills
// and the test harness share.
void DcAssembleJob::TickProvisioning()
{
    Slot* next = nullptr;
    for (Slot& slot : _slots)
        if (Pending(slot) && !slot.provisioned)
        {
            next = &slot;
            break;
        }
    if (!next)
    {
        EnterStage(Stage::Sanitizing);
        return;
    }
    if (_stageMs >= SetupTimeoutMs())
    {
        DropPending([](Slot const& slot) { return !slot.provisioned; }, "provisioning timed out");
        EnterStage(Stage::Sanitizing);
        return;
    }

    Player* const bot = ObjectAccessor::FindPlayer(next->guid);
    if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
        return;

    std::string pickedSpec;
    int const specNo =
        DcBotProvisioning::ResolveSpecNo(next->plan.classId, next->plan.specName, next->plan.fallbackSpec, &pickedSpec);
    // A random build is fine for damage, not for the group's tank or healer.
    if (specNo < 0 && std::string(next->plan.role) != "dps")
    {
        ObjectGuid const guid = next->guid;
        DropPending([guid](Slot const& slot) { return slot.guid == guid; }, "no premade spec for the role");
        return;
    }

    if (!DcProvisionBudget::Take())
        return;

    DcBotProvisioning::Roll(bot, _level, _gearQuality, _gearScoreLimit, specNo);
    next->provisioned = true;
    LOG_INFO("playerbots.dungeonclear", "{} {} provisioned {} ({} {} {}, level {}, gear <= ilvl {})", kTag, _id,
             bot->GetName(), DcBotProvisioning::ClassToken(next->plan.classId),
             specNo >= 0 ? pickedSpec : std::string("(random)"), next->plan.role, bot->GetLevel(),
             _gearIlvl ? std::to_string(_gearIlvl) : std::string("unlimited"));
}

void DcAssembleJob::TickSanitizing()
{
    bool allReady = true;
    for (Slot& slot : _slots)
    {
        if (!Pending(slot) || slot.sanitized)
            continue;
        Player* const bot = ObjectAccessor::FindPlayer(slot.guid);
        if (!bot || !bot->IsInWorld() || !GET_PLAYERBOT_AI(bot))
        {
            allReady = false;
            continue;
        }
        // A death knight who never left Acherus is locked out of every dungeon.
        DcPoolBots::CreditDeathKnightChain(bot);
        if (DcPoolBots::SanitizeCommon(bot, slot.lfgLeaveSent))
            slot.sanitized = true;
        else
            allReady = false;
    }

    if (!allReady && _stageMs < SetupTimeoutMs())
        return;
    DropPending([](Slot const& slot) { return !slot.sanitized; }, "could not be readied");
    EnterStage(Stage::Joining);
}

// Into the player's group, then to the player. The group is the player's own:
// created with them as leader when they have none, a raid once it passes five.
void DcAssembleJob::TickJoining()
{
    Player* const player = FindPlayer();
    // A far teleport finishes on the player's next login screen; wait it out.
    if (!player->IsInWorld() || player->IsBeingTeleported())
    {
        if (_stageMs >= SetupTimeoutMs())
        {
            DropPending([](Slot const&) { return true; }, "you were teleporting");
            FinishSetup();
        }
        return;
    }

    if (player->InBattleground() || player->InArena())
    {
        DropPending([](Slot const&) { return true; }, "you are in a battleground");
        FinishSetup();
        return;
    }

    Group* grp = PlayerGroup(player);
    if (grp && (grp->isLFGGroup() || grp->GetLeaderGUID() != player->GetGUID()))
    {
        DropPending([](Slot const&) { return true; }, "you are no longer the group leader");
        FinishSetup();
        return;
    }

    std::size_t pending = 0;
    for (Slot const& slot : _slots)
        pending += Pending(slot);
    if (!pending)
    {
        FinishSetup();
        return;
    }

    if (!grp)
    {
        grp = new Group();
        if (!grp->Create(player))
        {
            delete grp;
            DropPending([](Slot const&) { return true; }, "group creation failed");
            FinishSetup();
            return;
        }
        sGroupMgr->AddGroup(grp);
    }
    // Convert before the sixth member: AddMember refuses on a full party.
    if (!grp->isRaidGroup() && grp->GetMembersCount() + pending > MAXGROUPSIZE)
        grp->ConvertToRaid();
    _groupGuid = grp->GetGUID();

    Map* const map = player->GetMap();
    for (Slot& slot : _slots)
    {
        if (!Pending(slot))
            continue;
        Player* const bot = ObjectAccessor::FindPlayer(slot.guid);
        if (!bot || grp->IsFull() || !grp->AddMember(bot))
            continue;  // dropped below
        slot.joined = true;

        // Into the player's instance as well: the bot is in the leader's group,
        // so the destination is the leader's instance; it must also meet the
        // map's entry requirements, which a pool character never has.
        if (map->Instanceable())
            DcDungeonAccess::GrantEntry(bot, map->GetId(), map->GetDifficulty());
        if (!bot->TeleportTo(player->GetMapId(), player->GetPositionX(), player->GetPositionY(),
                             player->GetPositionZ(), player->GetOrientation()))
            LOG_WARN("playerbots.dungeonclear", "{} {} could not teleport {} to {} (map {})", kTag, _id,
                     bot->GetName(), _playerName, player->GetMapId());
    }
    DropPending([](Slot const&) { return true; }, "could not join the group");

    // A raid needs a main tank for the bots' tank logic: the player when they
    // tank, else the first tank we added. An existing choice stands.
    if (grp->isRaidGroup())
    {
        bool hasMainTank = false;
        for (Group::MemberSlot const& member : grp->GetMemberSlots())
            hasMainTank = hasMainTank || (member.flags & MEMBER_FLAG_MAINTANK);
        if (!hasMainTank)
        {
            ObjectGuid mainTank = _playerRole == "tank" ? player->GetGUID() : ObjectGuid::Empty;
            for (Slot const& slot : _slots)
                if (!mainTank && std::string(slot.plan.role) == "tank")
                    mainTank = slot.guid;
            if (mainTank)
                grp->SetGroupMemberFlag(mainTank, true, MEMBER_FLAG_MAINTANK);
        }
    }

    FinishSetup();
}

void DcAssembleJob::FinishSetup()
{
    DcAssemblePlanner::Counts members;
    if (Player* const player = FindPlayer())
        if (Group* grp = PlayerGroup(player))
            for (Group::MemberSlot const& member : grp->GetMemberSlots())
            {
                Player* const other = ObjectAccessor::FindConnectedPlayer(member.guid);
                if (member.guid == _playerGuid)
                    Count(members, _playerRole);
                else
                    Count(members, other ? RoleOf(other) : "dps");
            }

    std::string text = "Group ready: " + std::to_string(members.Total()) + "/" + std::to_string(_size) + ", " +
                       CountsText(members) + ".";
    if (!_dropped.empty())
    {
        text += " Not added:";
        for (std::size_t i = 0; i < _dropped.size(); ++i)
            text += (i ? ", " : " ") + _dropped[i];
        text += ".";
    }
    Tell(text);
    LOG_INFO("playerbots.dungeonclear", "{} {} {}", kTag, _id, text);

    _dropped.clear();
    EnterStage(_slots.empty() ? Stage::Done : Stage::Ready);
}

// The bots stay while they're in the player's group. A bot the player removes
// is logged out; the player leaving or disbanding the group releases them all.
void DcAssembleJob::TickReady()
{
    if (_slots.empty())
    {
        EnterStage(Stage::Done);
        return;
    }

    Player* const player = FindPlayer();
    Group* const grp = PlayerGroup(player);
    if (!grp || grp->GetGUID() != _groupGuid)
    {
        Release("the player's group is gone");
        Tell("Your group was disbanded or you left it; the assembled bots logged out.");
        return;
    }

    for (std::size_t i = _slots.size(); i-- > 0;)
        if (!grp->IsMember(_slots[i].guid))
            ReleaseSlot(i, "removed from the group");

    if (_slots.empty())
        EnterStage(Stage::Done);
}

std::string DcAssembleJob::StatusLine() const
{
    DcAssemblePlanner::Counts const ours = CountOurs();
    std::string out = _id + " " + _playerName + " [" + StageName(_stage) + "] group of " + std::to_string(_size) +
                      ", " + std::to_string(_slots.size()) + " bot(s): " + CountsText(ours);
    return out;
}
