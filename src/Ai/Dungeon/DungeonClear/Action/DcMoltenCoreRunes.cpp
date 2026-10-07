/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

// Molten Core (map 409) Firelord runes. With mod-individual-progression's
// IndividualProgression.MoltenCore.ManualRuneHandling = 1 (its default), a
// boss kill only makes the boss's rune (176951-176957, one beside each boss
// but Lucifron) selectable; Majordomo Executus is summoned once all seven are
// doused (its CheckFirelordRunes). A player douses one with Aqual Quintessence
// (17333), whose OPEN_LOCK clears the rune's lock 1459. Bots carry no
// quintessence, so the leader USES the rune instead: GameObject::Use runs the
// rune's go_firelord_rune OnGossipHello first, which is the whole douse
// (status latch, fire circle despawn, Majordomo check), the same seam the
// Hakkar flame douse rides.
//
// The trigger keys on the rune's own state, not a run-side latch: the script
// clears GO_FLAG_NOT_SELECTABLE and sets GO_STATE_ACTIVE on the kill, and sets
// the flag again on the douse. A rune that is active AND selectable is
// therefore exactly a killed boss's rune still waiting. Inert everywhere else:
// on another map, with ManualRuneHandling = 0 (the script douses on the kill
// and never clears the flag), under the core's own instance script (same), and
// on an instance reloaded after the kills (the runes load active, flagged).

#include "DcMoltenCoreRunes.h"
#include "DungeonClearActions.h"

#include <algorithm>
#include <iterator>

#include "Creature.h"
#include "GameObject.h"
#include "InstanceScript.h"
#include "Log.h"
#include "Map.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "SharedDefines.h"
#include "StringFormat.h"
#include "Ai/Dungeon/DungeonClear/Trigger/DungeonClearTriggers.h"
#include "Ai/Dungeon/DungeonClear/Util/DcCombatFlag.h"
#include "Ai/Dungeon/DungeonClear/Util/DcLeaderSignal.h"
#include "Ai/Dungeon/DungeonClear/Util/DcRun.h"
#include "Playerbots.h"

namespace
{
namespace DcMcRunes
{
    constexpr uint32 MAP_MOLTEN_CORE = 409;
    // Each rune sits within ~6yd of its boss's spawn, so the leader standing on
    // the corpse sees it; the scan reaches a little past a kiting pull's drift.
    constexpr float SCAN = 60.0f;
    constexpr float USE_RANGE = 5.0f;
    // Koro, Zeth, Mazj, Theri, Blaz, Kress, Mohn (Sulfuron, Geddon, Shazzrah,
    // Golemagg, Garr, Magmadar, Gehennas).
    constexpr uint32 RUNES[7] = {176951u, 176952u, 176953u, 176954u, 176955u, 176956u, 176957u};

    bool AwaitsDouse(GameObject const* rune)
    {
        return rune->isSpawned() && rune->GetGoState() == GO_STATE_ACTIVE &&
               !rune->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE);
    }

    GameObject* NearestUndousedRune(Player* bot)
    {
        if (!bot || bot->GetMapId() != MAP_MOLTEN_CORE)
            return nullptr;
        GameObject* best = nullptr;
        float bestDist = 0.0f;
        for (uint32 entry : RUNES)
        {
            GameObject* go = bot->FindNearestGameObject(entry, SCAN);
            if (!go || !AwaitsDouse(go))
                continue;
            float const d = bot->GetDistance(go);
            if (!best || d < bestDist)
            {
                best = go;
                bestDist = d;
            }
        }
        return best;
    }
}
}

bool DungeonClearMcDouseRuneTrigger::IsActive()
{
    if (!bot || bot->isDead() || bot->GetMapId() != DcMcRunes::MAP_MOLTEN_CORE)
        return false;
    DcRunState const& run = DcRun::Of(context);
    if (!run.enabled || run.paused || !DcLeaderSignal::IsDungeonClearLeader(bot))
        return false;
    if (!DcCombatFlag::MayDrive(bot, context))
        return false;
    return DcMcRunes::NearestUndousedRune(bot) != nullptr;
}

bool DungeonClearMcDouseRuneAction::Execute(Event /*event*/)
{
    GameObject* rune = DcMcRunes::NearestUndousedRune(bot);
    if (!rune)
        return false;
    if (!bot->IsWithinDistInMap(rune, DcMcRunes::USE_RANGE))
    {
        return DcMoveTo(bot->GetMapId(), rune->GetPositionX(), rune->GetPositionY(),
                        rune->GetPositionZ(), /*idle*/ false, /*react*/ false,
                        /*normal_only*/ false, /*exact_waypoint*/ false,
                        MovementPriority::MOVEMENT_NORMAL);
    }
    bot->SetFacingToObject(rune);
    rune->Use(bot);
    bool const doused = rune->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE);
    LOG_INFO("playerbots.dungeonclear", "[dungeon-clear] {} {} Firelord rune {} ({})",
             bot->GetName(), doused ? "doused" : "failed to douse", rune->GetName(), rune->GetEntry());
    return doused;
}

std::string DcMoltenCore::ForceRagnaros(Map* map, Player* user)
{
    InstanceMap* instMap = map ? map->ToInstanceMap() : nullptr;
    InstanceScript* inst = instMap ? instMap->GetInstanceScript() : nullptr;
    if (!inst || !user || map->GetId() != MAP_ID)
        return "not a Molten Core instance";

    // DATA_MAJORDOMO_EXECUTUS. Before the runes: once they are out the script
    // summons him, at his battle spot unless this slot is already DONE.
    if (inst->GetBossState(8) != DONE)
        inst->SetBossState(8, DONE);
    return ForceMajordomo(map, user);
}

std::string DcMoltenCore::ForceMajordomo(Map* map, Player* user)
{
    InstanceMap* instMap = map ? map->ToInstanceMap() : nullptr;
    InstanceScript* inst = instMap ? instMap->GetInstanceScript() : nullptr;
    if (!inst || !user || map->GetId() != MAP_ID)
        return "not a Molten Core instance";

    // DATA_LUCIFRON..DATA_GOLEMAGG (molten_core.h). SetBossState is a no-op for
    // a slot already DONE; for any other it runs the script's kill handling,
    // which is what readies that boss's rune.
    uint32 forced = 0;
    for (uint32 slot = 0; slot < 8; ++slot)
        if (inst->GetBossState(slot) != DONE && inst->SetBossState(slot, DONE))
            ++forced;

    uint32 doused = 0;
    uint32 missing = 0;
    for (auto const& [spawnId, data] : sObjectMgr->GetAllGOData())
    {
        if (data.mapid != MAP_ID ||
            std::find(std::begin(DcMcRunes::RUNES), std::end(DcMcRunes::RUNES), data.id) == std::end(DcMcRunes::RUNES))
            continue;
        map->LoadGrid(data.posX, data.posY);
        auto const bounds = map->GetGameObjectBySpawnIdStore().equal_range(spawnId);
        if (bounds.first == bounds.second)
        {
            ++missing;
            continue;
        }
        GameObject* rune = bounds.first->second;
        if (!DcMcRunes::AwaitsDouse(rune))
            continue;
        rune->Use(user);
        if (rune->HasGameObjectFlag(GO_FLAG_NOT_SELECTABLE))
            ++doused;
    }

    // The instance hands his GUID out under his boss slot (DATA_MAJORDOMO_EXECUTUS).
    Creature const* majordomo = map->GetCreature(inst->GetGuidData(8));
    bool const summoned = majordomo && majordomo->IsAlive();
    return Acore::StringFormat("Majordomo prep: {} boss slots forced, {} runes doused, {} not loaded; Majordomo {}",
                               forced, doused, missing, summoned ? "summoned" : "NOT summoned");
}
