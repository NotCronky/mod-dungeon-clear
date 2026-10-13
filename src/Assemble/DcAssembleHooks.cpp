/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license, you may redistribute it
 * and/or modify it under version 3 of the License, or (at your option), any later version.
 */

#include "ScriptMgr.h"

#include "Assemble/DcAssembleManager.h"

// The player leaving takes their assembled bots with them.
class DungeonClearAssemblePlayerScript : public PlayerScript
{
public:
    DungeonClearAssemblePlayerScript()
        : PlayerScript("DungeonClearAssemblePlayerScript", {
            PLAYERHOOK_ON_LOGOUT,
        }) {}

    void OnPlayerLogout(Player* player) override
    {
        DcAssembleManager::Instance().OnPlayerLogout(player);
    }
};

// Worldserver shutdown: log the bots out rather than save them in someone's group.
class DungeonClearAssembleWorldScript : public WorldScript
{
public:
    DungeonClearAssembleWorldScript()
        : WorldScript("DungeonClearAssembleWorldScript", {
            WORLDHOOK_ON_SHUTDOWN,
        }) {}

    void OnShutdown() override
    {
        DcAssembleManager::Instance().ReleaseAll("worldserver shutdown");
    }
};

void AddSC_dungeon_clear_assemble()
{
    new DungeonClearAssemblePlayerScript();
    new DungeonClearAssembleWorldScript();
}
