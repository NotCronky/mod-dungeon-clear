/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "TestRun/DcRotationCensus.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#include "AllSpellScript.h"
#include "DBCStores.h"
#include "Player.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

namespace
{
    struct Counts
    {
        std::uint32_t casts = 0;
        std::uint32_t rejected = 0;
        std::map<std::uint32_t, std::uint32_t> rejects; // SpellCastResult -> count
    };

    // Map threads update different instances at the same time, so every touch
    // goes through the lock. The work under it is a map lookup and an increment.
    std::mutex sLock;
    std::unordered_map<ObjectGuid, std::unordered_map<std::uint32_t, Counts>> sTallies;

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
            : AllSpellScript("DcRotationCensusScript", { ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_SPELL_CHECK_CAST }) { }

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

        void OnSpellCheckCast(Spell* spell, bool /*strict*/, SpellCastResult& res) override
        {
            if (res == SPELL_CAST_OK)
                return;

            Player* player = TrackedCaster(spell);
            if (!player)
                return;

            std::lock_guard<std::mutex> guard(sLock);
            auto tally = sTallies.find(player->GetGUID());
            if (tally == sTallies.end())
                return;

            Counts& counts = tally->second[FirstRank(spell->GetSpellInfo()->Id)];
            ++counts.rejected;
            ++counts.rejects[static_cast<std::uint32_t>(res)];
        }
    };
}

void DcRotationCensus::Start(ObjectGuid guid)
{
    std::lock_guard<std::mutex> guard(sLock);
    sTallies[guid].clear();
}

void DcRotationCensus::Stop(ObjectGuid guid)
{
    std::lock_guard<std::mutex> guard(sLock);
    sTallies.erase(guid);
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
        line.rejected = counts.rejected;
        for (auto const& [reason, count] : counts.rejects)
        {
            if (count > line.topRejectCount)
            {
                line.topReject = reason;
                line.topRejectCount = count;
            }
        }
        lines.push_back(std::move(line));
    }

    std::sort(lines.begin(), lines.end(), [](SpellLine const& a, SpellLine const& b)
    {
        return a.casts != b.casts ? a.casts > b.casts : a.rejected > b.rejected;
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
}
