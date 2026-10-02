# Tremulous with bots

This fork adds human and alien bots with combat, equipment purchases, evolution,
base building, repair and navigation derived from map collision. Bots follow the
normal credit, stage, build-point and spawn rules. Manage them through the game
or server console.

## Fresh Windows install: 16 vs 16

1. On Windows 10/11 **x64**, open the
   [bot release](https://github.com/moseye/tremulous/releases/tag/v0.1.0-bots)
   and download **tremulous-bots-windows-x64.zip**.
2. Extract the entire ZIP into a writable folder, such as
   `C:\Games\Tremulous-Bots`. Open the extracted folder containing
   `tremulous.exe`; keep its `base` folder, DLLs and license files together.
3. Double-click **Play 16 vs 16 bots.cmd**.
4. Join humans or aliens through the normal game menu, or stay in spectator
   mode to watch.

The archive includes the patched engine, game modules, stock game media, eight
maps and required Windows runtimes. You do not need an existing Tremulous
installation, Python or a compiler to play. The downloadable binary is for
Windows x64; other platforms require a source build.

The launcher loads **ATCS**, adds **16 human and 16 alien bots**, and allocates
**40 client slots**, leaving eight slots for people. The first bot added to each
empty team is a builder. ATCS is compact and gets crowded at this size.

Each team's skill spread follows evenly spaced normal-distribution quantiles,
centered at **5.5** with approximate standard deviation **1.7**, rounded and
bounded to levels 1–10. Assignments are shuffled across bot IDs and roles. For
16 bots per team, the distribution is:

| Skill | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Bots per team | 0 | 1 | 1 | 2 | 4 | 4 | 2 | 1 | 1 | 0 |

Bots enter the normal spawn queue. A spawn building has a **10-second spawn
interval**, so a full team can take several minutes to enter when only one
spawn is available. Queued bots need a surviving, usable team spawn. Navigation
also builds gradually while the match runs.

## Inspect or change the match

Press **~** to open the console. Use:

```text
set g_botDebug 1
bot list
bot buildings
botnav status
```

`bot list` shows IDs, roles, skills and whether bots are alive or queued. Debug
mode adds health, class, weapon, credits, position and target information.
`bot buildings` lists structures; `botnav status` reports graph generation.

To turn an existing local match into the same 16 vs 16 setup, enter these
commands in order. **Loading the map restarts the match** and applies the larger
client limit.

```text
set sv_maxclients 40
map atcs
bot fill humans 16 bell
bot fill aliens 16 bell
bot list
```

`bell` distributes skills across the entire resulting bot team, including bots
already present. Human players do not count toward the fill target. Other
useful controls are `bot remove all`, `bot skill all bell` and
`bot role <id> defend`. See [all commands and tuning options](docs/BOTS.md).

## Run a separate server

Double-click **Dedicated 16 vs 16 bots.cmd** in the extracted game folder. Keep
its console open. It runs the same map, bot count and skill distribution.

To join it on the same computer, open PowerShell in that folder and run:

```powershell
$botGameDir = (Get-Location).Path
.\tremulous.exe +set fs_basepath "$botGameDir" +set fs_homepath "$botGameDir\user" +connect localhost:30720
```

Enter management commands in the dedicated server's console. Other computers
can connect to the server's address using port 30720 when UDP access to that
port is allowed.

## Source checkout and repeatable test

For a fresh checkout of the bot branch:

```powershell
git clone --branch codex/bot-support-16v16 https://github.com/moseye/tremulous.git
cd tremulous
```

The [bot guide](docs/BOTS.md#build-from-source) describes compiler requirements,
building the matching engine and game modules, and adding the original media.
The release ZIP already provides a complete runtime for testing the source.

To reproduce a 16 vs 16 live test, install Python 3 and run this from the source
checkout. Adjust the paths to the extracted folder containing `tremded.exe`:

```powershell
python tests/bot_smoke.py `
  --server "C:\Games\Tremulous-Bots\tremded.exe" `
  --basepath "C:\Games\Tremulous-Bots" `
  --bots-per-team 16 --bell-skills --vm 2 --duration 90
```

The script starts an isolated local server, checks bot lifecycle and commands,
spawning, movement, navigation and skill distribution, then shuts it down.
Reports and logs are saved under `build-bot-tests`. Add `--economy` to accelerate
the existing passive-credit and stage settings for a separate purchase and
evolution check. Those accelerated settings are absent from the play launchers.

## Current limits

Base placement uses sampled positions and local choke-point traces, so cramped
bases can produce awkward layouts. The global navigation graph covers floors;
wall walking is local steering and recovery. Complex movers, teleporters and
wall/ceiling routes lack specialized planning. Larger matches can need slower
AI decision intervals (`set g_botThink 150` or `200`) on slower computers.

See [BOTS.md](docs/BOTS.md) for detailed behavior, navigation seeds, validation
and licenses. Preserve the bundled code, media and third-party license notices
when redistributing the game.
