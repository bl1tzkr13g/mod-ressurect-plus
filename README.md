# mod-resurrect-plus (development preview)

AzerothCore WotLK module based on the concept of dr1s/mod-corpse-respawn.

**Status: source package, NOT compiled or validated on your exact AzerothCore revision.**
Back up your server and test on a staging build before production.

## Installation (Windows)
1. Unzip folder `mod-resurrect-plus` into `azerothcore-wotlk/modules/`.
2. Remove/disable `mod-corpse-respawn` to avoid conflicting ghost hooks.
3. Re-run CMake configure/generate, then rebuild worldserver in Visual Studio.
4. Copy `conf/mod-resurrect-plus.conf.dist` to your worldserver configuration folder as `mod-resurrect-plus.conf`, and configure values.
5. Restart worldserver.

## Functional implementation
- Open-world ghost at corpse, normal graveyard routing for dungeon entrance behavior.
- Permanent XP subtraction on death or resurrection; no level loss.
- Extra durability loss once per death.
- Temporary XP-gain penalty via XP hooks.
- Resurrected health and mana settings.
- Optional spell-aura penalties (damage/healing/speed/max health), **only when you supply appropriate spell IDs**.

## Important limitations
- Reclaim delay and escalating reclaim delays are **not implemented**. AzerothCore owns the reclaim timer; a core hook/patch is required to override it reliably.
- `InstanceGhostMode=2` delegates to AzerothCore's normal graveyard routing; exact dungeon entrance coordinates depend on the core and world database.
- Aura `Amount` options do **not** dynamically change spell effects. Set effect magnitudes in Spell.dbc or implement custom aura mechanics.
- Repeated-death counts and temporary XP penalties are **in memory only**, cleared at logout/restart. Persistence is not implemented.
- PvP killer detection, separate raid/heroic settings, XP debt, penalty persistence, and special corpse-only resurrection detection are not implemented.
- OnPlayerResurrect triggers for resurrection methods beyond corpse reclaim.
- `IgnoreMaxLevel` uses next-level XP as a heuristic; verify for custom level caps.
- Do not run this simultaneously with the original corpse-respawn module.

## Attribution
Inspired by dr1s/mod-corpse-respawn (AzerothCore module). This implementation is independently written and includes compatible ghost-repop logic. AzerothCore and original module licenses apply.
