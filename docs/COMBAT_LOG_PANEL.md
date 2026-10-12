# Plan: WoW combat-log panel on NX Remote

**Status:** planned, not started.

## Goal

While the Lite is used as the controller, show WoW combat stats on its screen:
damage per second (current fight and session), healing per second, damage taken,
own health %, time in combat, encounter name, kills and deaths.

Display only. The controller path (Switch → USB → nice!nano → Bluetooth) must not
change and must keep working if the stats stop.

## Why this design

| Option | Verdict |
|--------|---------|
| WoW addon sends data out | **Not possible live.** Addons have no network or file access during play; their saved data is only written on `/reload` or logout. |
| Read WoW's combat log file | **Chosen.** WoW writes it when `/combatlog` is on; Warcraft Logs works the same way, so it is an accepted use. Catch: outside raids the file is written in batches, so solo play may lag (to be measured). |
| Addon draws stats as pixels, Mac reads the screen | **Rejected.** Fully live, but bots use the same technique and there is no clear Blizzard ruling: ban risk. |
| Stats through the nice!nano (Mac → BLE → nice!nano → USB → Switch) | **Later, maybe.** No Wi-Fi needed, but needs a custom Bluetooth channel next to the HID one. |

Transport for v1 is Wi-Fi (UDP): the Switch already has it, and stats are not
latency-critical.

## Architecture

```
WoW ──/combatlog──▶ Logs/WoWCombatLog*.txt ──follow──▶ Mac: companion/wow_stats.py ──UDP over Wi-Fi──▶ NX Remote "WoW" panel
                                                         (parse + aggregate)                              (Switch Lite)
```

The log lives in the `Logs/` folder of the WoW install (for retail:
`World of Warcraft/_retail_/Logs`); the install location varies, so the path is
configurable. Advanced Combat Logging is off by default.

## Components

### 1. Mac companion: `companion/wow_stats.py` (new)

- Finds the newest combat log in `<WoW>/_retail_/Logs` (path configurable) and
  follows it like `tail -F`, including when WoW starts a new file.
- Identifies the player: `--player Name-Realm`, or automatically from events whose
  source flags mark "mine + player" (to verify on a real log).
- Aggregates, per fight and per session:
  - damage done: `SWING_DAMAGE`, `RANGE_DAMAGE`, `SPELL_DAMAGE`,
    `SPELL_PERIODIC_DAMAGE`, including pets (owner learned from `SPELL_SUMMON`);
  - healing done: `SPELL_HEAL`, `SPELL_PERIODIC_HEAL`, minus overhealing;
  - damage taken (same damage events with the player as destination);
  - kills (`PARTY_KILL` by the player) and deaths (`UNIT_DIED` for the player);
  - fights: from `ENCOUNTER_START`/`ENCOUNTER_END` in instances, otherwise
    "in combat while events keep arriving" with a few seconds of quiet ending it.
- Own health %: Advanced Combat Logging adds unit info (current and max health)
  to events; read it from events where the player is that unit. Field positions
  to be confirmed on a real log.
- Sends a stats packet 2–4 times per second to the Switch.

### 2. Wire format (Mac → Switch)

UDP port **8194**, one datagram per update, plain text so it is easy to debug:

```
NXWOW1 combat=1 t=83.2 enc=Boss_Name dps=12345 sdps=9876 hps=0 dtps=2100 hp=76 kills=12 deaths=0
```

- Version prefix `NXWOW1`; unknown keys are ignored (room to grow).
- Spaces in values encoded as `_`; at most 512 bytes.
- No acknowledgement. The panel greys out after 5 s without packets.

### 3. NX Remote changes

- Start the network service and a third thread that listens on UDP 8194
  (non-blocking), parses packets into shared state.
- Show the Switch's Wi-Fi IP in the link panel, so the companion knows where to
  send (`wow_stats.py --switch <switch-ip>`).
- WoW panel on the free rows between the controller drawing and the footer:
  ```
  WoW  IN COMBAT 01:23  Boss_Name
       DPS 12.3k (session 9.9k)   HPS 0   taken 2.1k/s   HP 76%   kills 12  deaths 0
  ```
- The controller loop is untouched; a missing or broken companion only greys out
  the panel.

### 4. Optional helper addon

`/combatlog` has to be typed every session (it turns off on logout). An addon can
switch it on automatically with `LoggingCombat(true)`; existing addons do this
(to verify which one is current before writing our own).

## Steps

0. **Capture a real log (needs you, about 10 minutes in game).** Enable Advanced
   Combat Logging (Options → Network), type `/combatlog`, fight a few mobs solo
   and one dungeon pull, type `/combatlog` again. Then verify from the file:
   name pattern, line and timestamp format, position of the health fields, how
   to recognise the player, and how fast the file updates solo vs in a dungeon.
   Keep a trimmed sample for tests (other players' names anonymised).
1. **Parser and aggregator** as pure functions, unit-tested on the sample.
2. **Replay tool:** feeds a recorded log into the companion at real speed, so the
   whole chain can be tested without playing.
3. **Sender** with `--switch IP`.
4. **NX Remote:** network, listener thread, panel, Switch IP display; tested with
   the replay tool.
5. **Live test** in game, solo and in a dungeon; measure how far behind the panel is.
6. **Docs:** README usage, update `docs/FINDINGS.md` with the measured latency.

## Acceptance

- In a dungeon, the panel shows fight DPS, HP %, timer and encounter name, and
  updates within a few seconds of the action (exact number decided after step 0).
- The panel shows "stale" when the companion stops; the controller keeps working.
- No change to controller latency (checked with `check_gamecontroller`).

## Risks and open questions

- **Solo lag:** if the file only updates every many seconds outside instances,
  the panel is useful mainly in dungeons and raids. Step 0 decides.
- **Wi-Fi back on the Switch:** only for stats; costs some battery.
- **Format changes between patches:** parse by event name and tolerate extra
  fields.
- **Privacy:** combat logs contain other players' names; keep raw logs local.
- **Accuracy:** v1 may skip absorbs and other edge cases, so numbers can differ
  slightly from Details!/Warcraft Logs.
