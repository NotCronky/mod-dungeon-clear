/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "DcCreatureRole.h"

#include <mutex>
#include <unordered_map>
#include <vector>

#include "CreatureData.h"
#include "SmartScriptMgr.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

namespace
{
    std::mutex g_lock;
    std::unordered_map<std::uint32_t, std::uint8_t> g_flags;

    // A heal of any shape: direct, percent, or over time.
    bool Heals(SpellInfo const* info)
    {
        for (SpellEffectInfo const& effect : info->GetEffects())
        {
            switch (effect.Effect)
            {
                case SPELL_EFFECT_HEAL:
                case SPELL_EFFECT_HEAL_PCT:
                case SPELL_EFFECT_HEAL_MAX_HEALTH:
                    return true;
                case SPELL_EFFECT_APPLY_AURA:
                    if (effect.ApplyAuraName == SPELL_AURA_PERIODIC_HEAL)
                        return true;
                    break;
                default:
                    break;
            }
        }
        return false;
    }

    // Harmful and cast from beyond melee: a bolt, a curse, a ranged DoT.
    bool HitsFromRange(SpellInfo const* info)
    {
        constexpr float kMeleeRange = 10.0f;
        return !info->IsPositive() && info->GetMaxRange(false) > kMeleeRange;
    }

    std::uint8_t Compute(CreatureTemplate const* tmpl)
    {
        std::uint8_t flags = DcCreatureRole::None;
        std::vector<std::uint32_t> spells;
        for (std::uint32_t spell : tmpl->spells)
            if (spell)
                spells.push_back(spell);

        for (SmartScriptHolder const& holder :
             sSmartScriptMgr->GetScript(static_cast<int32>(tmpl->Entry), SMART_SCRIPT_TYPE_CREATURE))
        {
            switch (holder.action.type)
            {
                case SMART_ACTION_CAST:
                case SMART_ACTION_SELF_CAST:
                case SMART_ACTION_INVOKER_CAST:
                    spells.push_back(holder.action.cast.spell);
                    break;
                case SMART_ACTION_FLEE_FOR_ASSIST:
                    flags |= DcCreatureRole::Flees;
                    break;
                default:
                    break;
            }
        }

        for (std::uint32_t spell : spells)
        {
            SpellInfo const* info = sSpellMgr->GetSpellInfo(spell);
            if (!info)
                continue;
            if (Heals(info))
                flags |= DcCreatureRole::Healer;
            else if (HitsFromRange(info))
                flags |= DcCreatureRole::Caster;
        }
        if (tmpl->unit_class == CLASS_MAGE)
            flags |= DcCreatureRole::Caster;
        return flags;
    }
}

std::uint8_t DcCreatureRole::FlagsFor(CreatureTemplate const* tmpl)
{
    if (!tmpl)
        return None;

    std::lock_guard<std::mutex> lock(g_lock);
    auto const it = g_flags.find(tmpl->Entry);
    if (it != g_flags.end())
        return it->second;
    std::uint8_t const flags = Compute(tmpl);
    g_flags.emplace(tmpl->Entry, flags);
    return flags;
}
