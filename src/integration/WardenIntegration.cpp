/* Copyright (C) 2016+ AzerothCore <www.azerothcore.org>
 * Released under GNU GPL v2 licence */
#include "core/AnticheatMgr.h"
#include "ScriptMgr.h"

#ifdef AC_MOD_ANTICHEAT_WARDEN_HOOK
class AnticheatWardenScript : public PlayerScript
{
public:
    AnticheatWardenScript() : PlayerScript("AnticheatWardenScript", {PLAYERHOOK_ON_WARDEN_PENALTY}) { }
    void OnWardenPenalty(Player* player, uint16 checkId, std::string const& reason) override
    {
        anticheat::AnticheatMgr::Instance().OnWardenAlert(player, checkId, reason);
    }
};
#endif

void AddAnticheatWardenIntegration()
{
#ifdef AC_MOD_ANTICHEAT_WARDEN_HOOK
    new AnticheatWardenScript();
#endif
}
