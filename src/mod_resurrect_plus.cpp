/*
 * mod-resurrect-plus - AzerothCore WotLK
 * GPL-3.0-or-later. Original ghost release behavior inspired by dr1s/mod-corpse-respawn.
 */
#include "Config.h"
#include "GameGraveyard.h"
#include "Opcodes.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "SpellAuras.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_map>

namespace RP
{
    template<typename T> T Option(char const* name, T def)
    {
        return sConfigMgr->GetOption<T>(name, def);
    }
    bool Enabled(char const* name) { return Option<bool>(name, false); }
    uint32 Percent(char const* name, uint32 def = 0)
    {
        return std::min<uint32>(100, Option<uint32>(name, def));
    }
    uint32 Seconds(char const* name, uint32 def = 0)
    {
        return std::min<uint32>(86400, Option<uint32>(name, def));
    }
    uint64 Now()
    {
        return static_cast<uint64>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    }
    struct State
    {
        uint64 lastDeath = 0;
        uint32 deaths = 0;
        bool pending = false;
        uint32 debt = 0;
        uint64 xpEnd = 0;
        uint32 xpReduction = 0;
    };
    std::unordered_map<uint32, State> states;
    State& Get(Player* p) { return states[p->GetGUID().GetCounter()]; }
    bool PvPExcluded(Player* p)
    {
        return (p->InBattleground() && Option<bool>("ResurrectPlus.IgnoreBattlegrounds", true))
            || (p->InArena() && Option<bool>("ResurrectPlus.IgnoreArenas", true));
    }
    bool Allowed(Player* p)
    {
        if (!Option<bool>("ResurrectPlus.Enable", true) || PvPExcluded(p))
            return false;
        return true;
    }
    bool PenaltyAllowed(Player* p)
    {
        return Allowed(p) && Option<bool>("ResurrectPlus.Penalties.Enable", true);
    }
    float InstanceFactor(Player* p)
    {
        if (!p->GetMap()->IsDungeon() || !Enabled("ResurrectPlus.InstancePenalties.Enable"))
            return 1.0f;
        return std::max(0.0f, std::min(10.0f, Option<float>("ResurrectPlus.InstancePenaltyMultiplier", 1.5f)));
    }
    uint32 Scaled(uint32 n, float factor)
    {
        return static_cast<uint32>(std::min<double>(UINT32_MAX, std::round(double(n) * factor)));
    }
    void AuraPenalty(Player* p, char const* prefix, float factor)
    {
        // Configure a valid spell with the desired effect in your server's Spell.dbc.
        // This module controls the aura duration; the spell controls its actual magnitude.
        std::string base(prefix);
        if (!Option<bool>((base + ".Enable").c_str(), false))
            return;
        uint32 spell = Option<uint32>((base + ".SpellId").c_str(), 0);
        uint32 seconds = Scaled(Seconds((base + ".Duration").c_str()), factor);
        if (!spell || !seconds)
            return;
        p->CastSpell(p, spell, true);
        if (Aura* aura = p->GetAura(spell))
        {
            aura->SetMaxDuration(static_cast<int32>(std::min<uint32>(seconds * 1000, INT32_MAX)));
            aura->SetDuration(static_cast<int32>(std::min<uint32>(seconds * 1000, INT32_MAX)));
        }
    }
    void XPLoss(Player* p, float factor)
    {
        if (!Enabled("ResurrectPlus.XPLoss.Enable"))
            return;
        if (p->GetLevel() < Option<uint32>("ResurrectPlus.XPLoss.MinLevel", 10))
            return;
        if (Enabled("ResurrectPlus.XPLoss.IgnoreMaxLevel") &&
            p->GetUInt32Value(PLAYER_NEXT_LEVEL_XP) == 0)
            return;
        uint32 current = p->GetUInt32Value(PLAYER_XP);
        uint32 next = p->GetUInt32Value(PLAYER_NEXT_LEVEL_XP);
        uint32 mode = Option<uint32>("ResurrectPlus.XPLoss.Mode", 1);
        double base = mode == 0 ? Option<uint32>("ResurrectPlus.XPLoss.FixedAmount", 500)
            : double(mode == 2 ? next : current) * Percent("ResurrectPlus.XPLoss.Percentage", 5) / 100.0;
        uint32 amount = Scaled(static_cast<uint32>(std::min<double>(base, UINT32_MAX)), factor);
        uint32 cap = Option<uint32>("ResurrectPlus.XPLoss.MaxAmount", 5000);
        if (cap) amount = std::min(amount, cap);
        // Deliberately never delevel; this is safe for spells, talents and equipment.
        p->SetUInt32Value(PLAYER_XP, current - std::min(current, amount));
    }
    void ApplyPenalty(Player* p)
    {
        if (!PenaltyAllowed(p))
            return;
        float factor = InstanceFactor(p);
        AuraPenalty(p, "ResurrectPlus.DamagePenalty", factor);
        AuraPenalty(p, "ResurrectPlus.HealingPenalty", factor);
        AuraPenalty(p, "ResurrectPlus.SpeedPenalty", factor);
        AuraPenalty(p, "ResurrectPlus.HealthPenalty", factor);
        if (Enabled("ResurrectPlus.XPPenalty.Enable"))
        {
            State& st = Get(p);
            st.xpReduction = std::min<uint32>(100, Scaled(Percent("ResurrectPlus.XPPenalty.Amount", 50), factor));
            st.xpEnd = Now() + Scaled(Seconds("ResurrectPlus.XPPenalty.Duration", 300), factor);
        }
        if (Enabled("ResurrectPlus.ResurrectStats.Enable"))
        {
            p->SetHealth(std::max<uint32>(1, p->GetMaxHealth() * Percent("ResurrectPlus.ResurrectHealthPct", 50) / 100));
            p->SetPower(POWER_MANA, p->GetMaxPower(POWER_MANA) * Percent("ResurrectPlus.ResurrectManaPct", 50) / 100);
        }
    }
}

class ResurrectPlusPlayer : public PlayerScript
{
public:
    ResurrectPlusPlayer() : PlayerScript("ResurrectPlusPlayer",
        { PLAYERHOOK_ON_PLAYER_JUST_DIED, PLAYERHOOK_ON_PLAYER_RELEASED_GHOST,
          PLAYERHOOK_CAN_REPOP_AT_GRAVEYARD, PLAYERHOOK_ON_PLAYER_RESURRECT,
          PLAYERHOOK_ON_GIVE_EXP, PLAYERHOOK_ON_QUEST_COMPUTE_EXP,
          PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_LOGOUT }) { }

    void OnPlayerJustDied(Player* p) override
    {
        if (!RP::Allowed(p)) return;
        RP::State& st = RP::Get(p);
        uint64 now = RP::Now();
        uint32 window = RP::Seconds("ResurrectPlus.RepeatedDeaths.Window", 600);
        if (!st.lastDeath || now - st.lastDeath > window) st.deaths = 0;
        st.lastDeath = now;
        ++st.deaths;
        st.pending = true;
        if (!RP::PenaltyAllowed(p)) return;
        float factor = RP::InstanceFactor(p);
        if (RP::Enabled("ResurrectPlus.XPLoss.Enable") &&
            RP::Option<uint32>("ResurrectPlus.XPLoss.ApplyOn", 0) == 0)
        {
            float repeat = RP::Option<float>("ResurrectPlus.XPLoss.RepeatedDeathMultiplier", 1.0f);
            RP::XPLoss(p, factor * std::pow(std::max(0.0f, repeat), std::min<uint32>(st.deaths - 1, 10)));
        }
        if (RP::Enabled("ResurrectPlus.DurabilityPenalty.Enable"))
        {
            double amount = RP::Percent("ResurrectPlus.DurabilityPenalty.Amount", 10) * factor / 100.0;
            p->DurabilityLossAll(std::min(1.0, amount), true);
        }
    }
    bool OnPlayerCanRepopAtGraveyard(Player* p) override
    {
        if (!RP::Allowed(p)) return true;
        uint32 mode = p->GetMap()->IsDungeon()
            ? RP::Option<uint32>("ResurrectPlus.InstanceGhostMode", 2)
            : RP::Option<uint32>("ResurrectPlus.WorldGhostMode", 1);
        // Mode 2 delegates to AzerothCore's built-in dungeon graveyard/entrance handling.
        if (mode != 1) return true;
        if (RP::Option<bool>("ResurrectPlus.SendGraveyardMarker", true))
        {
            if (GraveyardStruct const* gy = sGraveyard->GetClosestGraveyard(p, p->GetTeamId()))
            {
                WorldPacket data(SMSG_DEATH_RELEASE_LOC, 4 * 4);
                data << gy->Map << gy->x << gy->y << gy->z;
                p->SendDirectMessage(&data);
            }
        }
        return false;
    }
    void OnPlayerReleasedGhost(Player* /*p*/) override { }
    void OnPlayerResurrect(Player* p, float /*restore*/, bool& /*sickness*/) override
    {
        RP::State& st = RP::Get(p);
        if (!st.pending) return;
        st.pending = false;
        if (!RP::PenaltyAllowed(p)) return;
        if (RP::Enabled("ResurrectPlus.XPLoss.Enable") &&
            RP::Option<uint32>("ResurrectPlus.XPLoss.ApplyOn", 0) == 1)
            RP::XPLoss(p, RP::InstanceFactor(p));
        RP::ApplyPenalty(p);
    }
    void OnPlayerGiveXP(Player* p, uint32& amount, Unit* /*victim*/, uint8 /*source*/) override
    {
        RP::State& st = RP::Get(p);
        if (st.xpEnd > RP::Now() && st.xpReduction)
            amount = static_cast<uint32>(uint64(amount) * (100 - st.xpReduction) / 100);
    }
    void OnPlayerQuestComputeXP(Player* p, Quest const* /*quest*/, uint32& xpValue) override
    {
        RP::State& st = RP::Get(p);
        if (st.xpEnd > RP::Now() && st.xpReduction)
            xpValue = static_cast<uint32>(uint64(xpValue) * (100 - st.xpReduction) / 100);
    }
    void OnPlayerLogin(Player* p) override
    {
        // No transient state is persisted in this initial release.
        RP::states.erase(p->GetGUID().GetCounter());
    }
    void OnPlayerLogout(Player* p) override
    {
        RP::states.erase(p->GetGUID().GetCounter());
    }
};
void AddResurrectPlusScripts()
{
    new ResurrectPlusPlayer();
}
