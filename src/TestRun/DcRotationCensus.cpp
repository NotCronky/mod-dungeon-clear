/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "TestRun/DcRotationCensus.h"

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#include "AllSpellScript.h"
#include "DBCStores.h"
#include "Opcodes.h"
#include "Player.h"
#include "ServerScript.h"
#include "Spell.h"
#include "Timer.h"
#include "UnitScript.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
    struct Counts
    {
        std::uint32_t casts = 0;
        std::uint32_t failed = 0;
        std::map<std::uint32_t, std::uint32_t> fails; // SpellCastResult -> count
    };

    // Map threads update different instances at the same time, so every touch
    // goes through the lock. The work under it is a map lookup and an increment.
    std::mutex sLock;
    std::unordered_map<ObjectGuid, std::unordered_map<std::uint32_t, Counts>> sTallies;

    struct MeterTally
    {
        std::uint64_t damage = 0;
        std::uint64_t healing = 0;
        std::uint32_t activeMs = 0;
        std::uint32_t lastMs = 0;  // getMSTime() of the last event; 0 = none yet
    };
    std::unordered_map<ObjectGuid, MeterTally> sMeters;

    // The spell a hit on a tracked member is about to land with: the core asks
    // the spell / periodic / melee damage hooks first, then deals the damage
    // (OnDamage). Keyed by victim, matched on the attacker.
    struct PendingHit
    {
        ObjectGuid attacker;
        std::uint32_t spellId = 0;
    };
    std::unordered_map<ObjectGuid, PendingHit> sPending;
    std::unordered_map<ObjectGuid, DcRotationCensus::LastHit> sLastHits;

    // How many members are tracked: the damage and heal hooks fire for every
    // hit on the server, and with no test run going they return on this
    // without touching the lock.
    std::atomic<std::uint32_t> sTracked{0};

    void AddToMeter(Unit* source, std::uint64_t damage, std::uint64_t healing)
    {
        if (!sTracked.load(std::memory_order_relaxed) || !source)
            return;

        Player* player = source->GetCharmerOrOwnerPlayerOrPlayerItself();
        if (!player)
            return;

        std::uint32_t const now = getMSTime();
        std::lock_guard<std::mutex> guard(sLock);
        auto meter = sMeters.find(player->GetGUID());
        if (meter == sMeters.end())
            return;

        MeterTally& m = meter->second;
        m.damage += damage;
        m.healing += healing;
        if (m.lastMs)
            m.activeMs += std::min(getMSTimeDiff(m.lastMs, now), DcRotationCensus::ActiveGapMs);
        m.lastMs = now;
    }

    std::uint32_t FirstRank(std::uint32_t spellId)
    {
        std::uint32_t first = sSpellMgr->GetFirstSpellInChain(spellId);
        return first ? first : spellId;
    }

    // The tracked player that cast it, when the spell belongs in a rotation.
    Player* TrackedCaster(Spell const* spell)
    {
        if (!spell || spell->IsTriggered())
            return nullptr;

        Player* player = spell->GetCaster() ? spell->GetCaster()->ToPlayer() : nullptr;
        if (!player || !player->HasSpell(spell->GetSpellInfo()->Id))
            return nullptr;

        return player;
    }

    class DcRotationCensusScript : public AllSpellScript
    {
    public:
        DcRotationCensusScript()
            : AllSpellScript("DcRotationCensusScript", { ALLSPELLHOOK_ON_CAST }) { }

        void OnSpellCast(Spell* spell, Unit* /*caster*/, SpellInfo const* spellInfo, bool /*skipCheck*/) override
        {
            Player* player = TrackedCaster(spell);
            if (!player)
                return;

            std::lock_guard<std::mutex> guard(sLock);
            auto tally = sTallies.find(player->GetGUID());
            if (tally != sTallies.end())
                ++tally->second[FirstRank(spellInfo->Id)].casts;
        }

    };

    class DcRotationCensusMeterScript : public UnitScript
    {
    public:
        DcRotationCensusMeterScript()
            : UnitScript("DcRotationCensusMeterScript", true,
                         { UNITHOOK_ON_DAMAGE, UNITHOOK_ON_HEAL, UNITHOOK_MODIFY_SPELL_DAMAGE_TAKEN,
                           UNITHOOK_MODIFY_PERIODIC_DAMAGE_AURAS_TICK, UNITHOOK_MODIFY_MELEE_DAMAGE }) { }

        void ModifySpellDamageTaken(Unit* target, Unit* attacker, int32& /*damage*/, SpellInfo const* spellInfo) override
        {
            NoteSpell(target, attacker, spellInfo ? spellInfo->Id : 0);
        }

        void ModifyPeriodicDamageAurasTick(Unit* target, Unit* attacker, uint32& /*damage*/,
                                           SpellInfo const* spellInfo) override
        {
            NoteSpell(target, attacker, spellInfo ? spellInfo->Id : 0);
        }

        void ModifyMeleeDamage(Unit* target, Unit* attacker, uint32& /*damage*/) override
        {
            NoteSpell(target, attacker, 0);
        }

        void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
        {
            NoteHit(attacker, victim, damage);

            // Damage into enemies only: not Life Tap, Hellfire's self-burn or a mind-controlled party member.
            if (!attacker || !victim || !damage || attacker == victim || !attacker->IsHostileTo(victim))
                return;

            AddToMeter(attacker, damage, 0);
        }

        void OnHeal(Unit* healer, Unit* /*receiver*/, uint32& gain) override
        {
            if (gain)
                AddToMeter(healer, 0, gain);
        }

    private:
        static void NoteSpell(Unit* target, Unit* attacker, std::uint32_t spellId)
        {
            if (!sTracked.load(std::memory_order_relaxed) || !target || !attacker || !target->IsPlayer())
                return;
            std::lock_guard<std::mutex> guard(sLock);
            if (sMeters.count(target->GetGUID()))
                sPending[target->GetGUID()] = PendingHit{ attacker->GetGUID(), spellId };
        }

        static void NoteHit(Unit* attacker, Unit* victim, std::uint32_t damage)
        {
            if (!sTracked.load(std::memory_order_relaxed) || !victim || !damage || !victim->IsPlayer())
                return;
            std::lock_guard<std::mutex> guard(sLock);
            if (!sMeters.count(victim->GetGUID()))
                return;

            std::uint32_t spellId = 0;
            auto pending = sPending.find(victim->GetGUID());
            if (pending != sPending.end())
            {
                if (attacker && pending->second.attacker == attacker->GetGUID())
                    spellId = pending->second.spellId;
                sPending.erase(pending);
            }

            DcRotationCensus::LastHit& hit = sLastHits[victim->GetGUID()];
            hit.attacker = !attacker || attacker == victim ? "environment" : attacker->GetName();
            hit.entry = attacker && attacker->IsCreature() ? attacker->GetEntry() : 0;
            hit.spellId = spellId;
            SpellInfo const* info = spellId ? sSpellMgr->GetSpellInfo(spellId) : nullptr;
            hit.spell = info ? info->SpellName[0] : "";
            hit.damage = damage;
        }
    };

    // Failed casts, from the SMSG_CAST_FAILED the server sends the bot's
    // (socketless) session: uint8 cast count, uint32 spell id, uint8 result.
    class DcRotationCensusPacketScript : public ServerScript
    {
    public:
        DcRotationCensusPacketScript() : ServerScript("DcRotationCensusPacketScript", { SERVERHOOK_ON_PACKET_SENT }) { }

        void OnPacketSent(WorldSession* session, WorldPacket const& packet) override
        {
            if (packet.GetOpcode() != SMSG_CAST_FAILED || packet.size() < 6 || !session)
                return;

            Player* player = session->GetPlayer();
            if (!player)
                return;

            std::uint32_t spellId = packet.read<std::uint32_t>(1);
            std::uint8_t result = packet.read<std::uint8_t>(5);
            if (!player->HasSpell(spellId))
                return;

            std::lock_guard<std::mutex> guard(sLock);
            auto tally = sTallies.find(player->GetGUID());
            if (tally == sTallies.end())
                return;

            Counts& counts = tally->second[FirstRank(spellId)];
            ++counts.failed;
            ++counts.fails[result];
        }
    };
}

void DcRotationCensus::Start(ObjectGuid guid)
{
    std::lock_guard<std::mutex> guard(sLock);
    sTallies[guid].clear();
    sMeters[guid] = MeterTally{};
    sPending.erase(guid);
    sLastHits.erase(guid);
    sTracked.store(static_cast<std::uint32_t>(sMeters.size()), std::memory_order_relaxed);
}

void DcRotationCensus::Stop(ObjectGuid guid)
{
    std::lock_guard<std::mutex> guard(sLock);
    sTallies.erase(guid);
    sMeters.erase(guid);
    sPending.erase(guid);
    sLastHits.erase(guid);
    sTracked.store(static_cast<std::uint32_t>(sMeters.size()), std::memory_order_relaxed);
}

DcRotationCensus::MeterReading DcRotationCensus::Meter(ObjectGuid guid)
{
    std::lock_guard<std::mutex> guard(sLock);
    MeterReading reading;
    auto meter = sMeters.find(guid);
    if (meter != sMeters.end())
    {
        reading.damage = meter->second.damage;
        reading.healing = meter->second.healing;
        reading.activeS = meter->second.activeMs / 1000;
    }
    return reading;
}

bool DcRotationCensus::LastHitOn(ObjectGuid guid, LastHit& hit)
{
    std::lock_guard<std::mutex> guard(sLock);
    auto it = sLastHits.find(guid);
    if (it == sLastHits.end())
        return false;
    hit = it->second;
    return true;
}

void DcRotationCensus::Collect(Player* player, std::vector<SpellLine>& lines, std::vector<std::string>& unused)
{
    lines.clear();
    unused.clear();
    if (!player)
        return;

    std::unordered_map<std::uint32_t, Counts> tally;
    {
        std::lock_guard<std::mutex> guard(sLock);
        auto found = sTallies.find(player->GetGUID());
        if (found != sTallies.end())
            tally = found->second;
    }

    for (auto const& [spellId, counts] : tally)
    {
        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        SpellLine line;
        line.spellId = spellId;
        line.name = info ? info->SpellName[0] : "";
        line.casts = counts.casts;
        line.failed = counts.failed;
        for (auto const& [reason, count] : counts.fails)
        {
            if (count > line.topFailCount)
            {
                line.topFail = reason;
                line.topFailCount = count;
            }
        }
        lines.push_back(std::move(line));
    }

    std::sort(lines.begin(), lines.end(), [](SpellLine const& a, SpellLine const& b)
    {
        return a.casts != b.casts ? a.casts > b.casts : a.failed > b.failed;
    });

    // Known class abilities never cast: active, of the class's spell family, and
    // the highest rank the player has (one name per ability).
    ChrClassesEntry const* classEntry = sChrClassesStore.LookupEntry(player->getClass());
    std::uint32_t family = classEntry ? classEntry->spellfamily : 0;
    std::set<std::string> names;
    for (auto const& [spellId, playerSpell] : player->GetSpellMap())
    {
        if (playerSpell->State == PLAYERSPELL_REMOVED || !playerSpell->Active)
            continue;

        SpellInfo const* info = sSpellMgr->GetSpellInfo(spellId);
        if (!info || info->IsPassive() || !family || info->SpellFamilyName != family)
            continue;

        if (std::uint32_t next = sSpellMgr->GetNextSpellInChain(spellId))
            if (player->HasSpell(next))
                continue;

        auto counted = tally.find(FirstRank(spellId));
        if (counted != tally.end() && counted->second.casts)
            continue;

        names.insert(info->SpellName[0]);
    }

    unused.assign(names.begin(), names.end());
}

void AddSC_dungeon_clear_rotation_census()
{
    new DcRotationCensusScript();
    new DcRotationCensusPacketScript();
    new DcRotationCensusMeterScript();
}
