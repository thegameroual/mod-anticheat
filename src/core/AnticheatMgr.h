/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence */
#pragma once

#include "Common.h"
#include <memory>
#include <string>

class Player;
class WorldSession;
class WorldPacket;
struct MovementInfo;

namespace anticheat
{
class AnticheatMgr
{
public:
    static AnticheatMgr& Instance();
    bool LoadConfig();
    void OnLogin(Player* player);
    void OnLogout(Player* player);
    void OnMovement(Player* player, MovementInfo const& movement, uint32 opcode);
    void OnTransition(Player* player);
    void OnServerPacket(WorldSession* session, WorldPacket const& packet);
    void OnWorldUpdate(uint32 diff);
    void OnShutdown();
    void ShowPlayerReport(Player* gm, Player* player);
    void ShowGlobalReport(Player* gm);
    void ResetSessionReports(Player* gm);
    void OnWardenAlert(Player* player, uint16 checkId, std::string const& reason);

private:
    AnticheatMgr();
    ~AnticheatMgr();
    AnticheatMgr(AnticheatMgr const&) = delete;
    AnticheatMgr& operator=(AnticheatMgr const&) = delete;
    struct Impl;
    std::unique_ptr<Impl> _impl;
};
}
