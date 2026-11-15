/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence */
#include "core/AnticheatMgr.h"
#include "Chat.h"
#include "Config.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldSession.h"

using anticheat::AnticheatMgr;

class AnticheatWorldScript : public WorldScript
{
public:
    AnticheatWorldScript() : WorldScript("AnticheatWorldScript", {
        WORLDHOOK_ON_BEFORE_WORLD_INITIALIZED, WORLDHOOK_ON_AFTER_CONFIG_LOAD,
        WORLDHOOK_ON_UPDATE, WORLDHOOK_ON_SHUTDOWN, WORLDHOOK_ON_AFTER_UNLOAD_ALL_MAPS}) { }
    void OnBeforeWorldInitialized() override { AnticheatMgr::Instance().LoadConfig(); }
    void OnAfterConfigLoad(bool reload) override
    {
        if (reload)
            AnticheatMgr::Instance().LoadConfig();
    }
    void OnUpdate(uint32 diff) override { AnticheatMgr::Instance().OnWorldUpdate(diff); }
    void OnShutdown() override { AnticheatMgr::Instance().OnShutdown(); }
    void OnAfterUnloadAllMaps() override { AnticheatMgr::Instance().OnShutdown(); }
};

class AnticheatPlayerScript : public PlayerScript
{
public:
    AnticheatPlayerScript() : PlayerScript("AnticheatPlayerScript", {
        PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT, PLAYERHOOK_ON_MAP_CHANGED,
        PLAYERHOOK_ON_BEFORE_TELEPORT}) { }
    void OnPlayerLogin(Player* player) override { AnticheatMgr::Instance().OnLogin(player); }
    void OnPlayerLogout(Player* player) override { AnticheatMgr::Instance().OnLogout(player); }
    void OnPlayerMapChanged(Player* player) override { AnticheatMgr::Instance().OnTransition(player); }
    bool OnPlayerBeforeTeleport(Player* player, uint32, float, float, float, float, uint32, Unit*) override
    {
        AnticheatMgr::Instance().OnTransition(player);
        return true;
    }
};

class AnticheatMovementScript : public MovementHandlerScript
{
public:
    AnticheatMovementScript() : MovementHandlerScript("AnticheatMovementScript", {MOVEMENTHOOK_ON_PLAYER_MOVE}) { }
    void OnPlayerMove(Player* player, MovementInfo movement, uint32 opcode) override
    {
        AnticheatMgr::Instance().OnMovement(player, movement, opcode);
    }
};

class AnticheatServerScript : public ServerScript
{
public:
    AnticheatServerScript() : ServerScript("AnticheatServerScript", {SERVERHOOK_ON_PACKET_SENT}) { }
    void OnPacketSent(WorldSession* session, WorldPacket const& packet) override
    {
        AnticheatMgr::Instance().OnServerPacket(session, packet);
    }
};

using namespace Acore::ChatCommands;
class AnticheatCommandScript : public CommandScript
{
public:
    AnticheatCommandScript() : CommandScript("AnticheatCommandScript") { }
    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable const commands = {
            {"player", PlayerReport, SEC_GAMEMASTER, Console::No},
            {"global", GlobalReport, SEC_GAMEMASTER, Console::No},
            {"reset", Reset, SEC_ADMINISTRATOR, Console::No},
            {"reload", Reload, SEC_ADMINISTRATOR, Console::No}
        };
        static ChatCommandTable const root = {{"anticheat", commands}};
        return root;
    }
    static bool PlayerReport(ChatHandler* handler, Optional<PlayerIdentifier> target)
    {
        Player* player = target ? target->GetConnectedPlayer() : handler->getSelectedPlayer();
        if (!player)
        {
            handler->SendSysMessage("Select or name a connected player.");
            return true;
        }
        AnticheatMgr::Instance().ShowPlayerReport(handler->GetSession()->GetPlayer(), player);
        return true;
    }
    static bool GlobalReport(ChatHandler* handler)
    {
        AnticheatMgr::Instance().ShowGlobalReport(handler->GetSession()->GetPlayer());
        return true;
    }
    static bool Reset(ChatHandler* handler)
    {
        AnticheatMgr::Instance().ResetSessionReports(handler->GetSession()->GetPlayer());
        return true;
    }
    static bool Reload(ChatHandler* handler)
    {
        if (!sConfigMgr->LoadModulesConfigs(true))
        {
            handler->SendSysMessage("Module configuration files could not be loaded; see server log.");
            return true;
        }
        bool const loaded = AnticheatMgr::Instance().LoadConfig();
        handler->SendSysMessage(loaded ? "Anticheat configuration reloaded." :
            "Invalid anticheat configuration; previous revision retained. See server log.");
        return true;
    }
};

void AddAnticheatWardenIntegration();
void AddAnticheatScripts()
{
    new AnticheatWorldScript();
    new AnticheatPlayerScript();
    new AnticheatMovementScript();
    new AnticheatServerScript();
    new AnticheatCommandScript();
    AddAnticheatWardenIntegration();
}
