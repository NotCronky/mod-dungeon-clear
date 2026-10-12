/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "Assemble/DcAssembleManager.h"

#include "Player.h"

#include "Ai/Dungeon/DungeonClear/Settings/DcSettings.h"
#include "Assemble/DcAssembleJob.h"
#include "DcModuleEnable.h"

DcAssembleManager& DcAssembleManager::Instance()
{
    static DcAssembleManager instance;
    return instance;
}

DcAssembleJob* DcAssembleManager::JobFor(ObjectGuid player) const
{
    for (auto const& job : _jobs)
        if (job->PlayerGuid() == player && !job->Done())
            return job.get();
    return nullptr;
}

bool DcAssembleManager::Assemble(Player* player, std::size_t size, std::string const& role, std::string* msg)
{
    if (!DcModule::IsEnabled() || !DcSettings::GetBool(ObjectGuid::Empty, "Assemble.Enable"))
    {
        *msg = "Assembling bots is turned off on this realm (DungeonClear.Assemble.Enable).";
        return false;
    }

    DcAssembleJob* job = JobFor(player->GetGUID());
    if (!job)
    {
        _jobs.push_back(DcAssembleJob::Create(player));
        job = _jobs.back().get();
    }
    return job->Adjust(player, size, role, msg);
}

bool DcAssembleManager::Dismiss(Player* player, std::string* msg)
{
    DcAssembleJob* job = JobFor(player->GetGUID());
    if (!job)
    {
        *msg = "You have no assembled bots.";
        return false;
    }
    job->Release("dismissed");
    *msg = "Assembled bots dismissed.";
    return true;
}

std::string DcAssembleManager::StatusFor(Player* player) const
{
    DcAssembleJob const* job = JobFor(player->GetGUID());
    return job ? job->StatusLine() : "You have no assembled bots.";
}

void DcAssembleManager::OnPlayerLogout(Player* player)
{
    if (DcAssembleJob* job = player ? JobFor(player->GetGUID()) : nullptr)
        job->Release("player logged out");
}

void DcAssembleManager::Tick(std::uint32_t diff)
{
    if (_jobs.empty())
        return;

    // Switched off with bots out: release them rather than strand them.
    if (!DcModule::IsEnabled() || !DcSettings::GetBool(ObjectGuid::Empty, "Assemble.Enable"))
    {
        ReleaseAll("feature turned off");
        return;
    }

    for (auto const& job : _jobs)
        job->Tick(diff);
    for (auto it = _jobs.begin(); it != _jobs.end();)
        it = (*it)->Done() ? _jobs.erase(it) : it + 1;
}

void DcAssembleManager::ReleaseAll(std::string const& reason)
{
    for (auto const& job : _jobs)
        job->Release(reason);
    _jobs.clear();
}

bool DcAssembleManager::IsClaimed(ObjectGuid guid) const
{
    for (auto const& job : _jobs)
        if (job->HoldsBot(guid))
            return true;
    return false;
}
