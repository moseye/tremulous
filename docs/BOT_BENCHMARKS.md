# Bot simulation and balance measurements

Use the patched dedicated server for repeatable matches without waiting for
real time. Fast mode advances the **same 50 ms physics frames** as a normal
20 Hz server and removes the wall-clock wait between frames. It does not
increase movement speed, weapon damage, credits, stages or spawn frequency.
Each process runs one map and seed in its own home directory. The Python runner
launches several processes and retains their logs and results.

Fast simulation is offline. To watch bots, use **Play 16 vs 16 bots.cmd** and
join Spectators; accelerated benchmark processes cannot accept spectators.

## Run a suite

Install Python 3.8 or newer and extract the complete Windows game package, or build the
matching dedicated server and game module and supply the stock media. From a
source checkout, run this in PowerShell, adjusting the two runtime paths:

```powershell
python tests/bot_benchmark.py `
  --server "C:\Games\Tremulous-Bots\tremded.exe" `
  --basepath "C:\Games\Tremulous-Bots" `
  --map arachnid2 --bots-per-team 16 --seeds 17,29,41 `
  --seconds 2400 --jobs 3 --vm 0 --wall-timeout 300 `
  --set g_botCombatTuning=1 --set g_botTeamwork=1 `
  --set g_botSpawnScale=1 --set g_botNavTuning=1 --set g_botNavNodes=8192 `
  --set g_botHumanAimCone=6 --set g_botHumanReaction=600 `
  --set g_botHumanTurnSpeed=160 --set g_botHumanFireDelay=300
```

This runs three 40-minute matches with normal game rules on Arachnid2. A match
ends when the game declares a winner or reaches the requested game-time limit.
The default map is Arachnid2. `--map` can be repeated for a suite of maps; each
must be installed in the runtime's `base` directory. `--bots-per-team` supports
1–31. Choose a worker count that your machine can sustain; the runner limits it
to the CPU count and 32 workers.

`--vm 0` uses the native game DLL and is fastest on Windows. `--vm 2` uses the
compiled game QVM and matches the play launchers. `--vm 1` uses the slower QVM
interpreter. Keep the engine and game modules from the same build together.

Reports appear in `build-bot-benchmarks/suite-<id>`:

- `aggregate.json`: outcomes, per-match summaries, throughput, normalized combat
  rates, class and weapon occupation, queues, spawn construction and navigation.
- One directory per map, seed, configuration and repetition: `report.json`,
  `stdout.log`, isolated `base/gameplay.log` and `base/botbench.jsonl`.
- `comparison.json` when running paired baseline and candidate configurations.

The runner records executable, module and map hashes, startup commands and
configuration files. Homes are unique and do not load the player's usual home
settings. Use a clean runtime: custom `autoexec.cfg` or other settings installed
in its `base` directory can still apply before the generated benchmark config.
Requested bot settings are checked against registered game cvars and their
loaded values. Misspelled or ignored settings fail the run as `invalid_settings`
instead of silently measuring a different configuration.

## Compare changes

Use the same map, seed list, match duration, VM and worker count for both sides.
An override is recorded in the report. For example, hold navigation, spawn
construction and teamwork fixed while comparing human combat settings:

```powershell
python tests/bot_benchmark.py `
  --server "C:\Games\Tremulous-Bots\tremded.exe" `
  --basepath "C:\Games\Tremulous-Bots" `
  --map arachnid2 --bots-per-team 16 --seeds 17,29,41 `
  --seconds 900 --jobs 3 --vm 0 --wall-timeout 300 `
  --set g_botCombatTuning=1 --set g_botTeamwork=1 `
  --set g_botSpawnScale=1 --set g_botNavTuning=1 --set g_botNavNodes=8192 `
  --baseline-set g_botHumanAimCone=3 --baseline-set g_botHumanReaction=300 `
  --baseline-set g_botHumanTurnSpeed=240 --baseline-set g_botHumanFireDelay=160 `
  --candidate-set g_botHumanAimCone=6 --candidate-set g_botHumanReaction=600 `
  --candidate-set g_botHumanTurnSpeed=160 --candidate-set g_botHumanFireDelay=300
```

To compare saved aggregates from separate binaries:

```powershell
python tests/bot_benchmark.py --compare baseline\aggregate.json candidate\aggregate.json --output comparison
```

Add `--repeats 2` to replay every seed and configuration. The runner compares
every telemetry record after removing only `wall_elapsed_ms`; a mismatch is
reported separately and causes a nonzero exit code. Repeatability is checked
within a build and VM. Native and QVM floating-point behavior may differ over
long matches, so use the same VM for an A/B experiment.

For additional diagnostics, add `--set g_botBenchmarkDetails=1`. Samples then
include bot positions, movement goals, classes, targets, credits and commands,
plus structure positions and health. These snapshots observe AI decisions
without supplying information to bots. Aggregate structure counts include all
buildings; the optional structure-position list is capped at 128 and reports
any omitted count.

## Interpret results

A draw at the game-time limit is distinct from a process timeout. Timeouts,
crashes, startup failures, missing terminal records and invalid frame steps are
errors and do not count as completed draws. The runner has independent startup
and process wall-time limits. Increase `--wall-timeout` for long matches,
interpreted QVM runs or slower hardware.

Evaluate decisive wins and base damage alongside combat, queues and class
occupation. Kills alone can reward spawn camping or endless fights in the
middle of a map. Building damage is cumulative actual health removed, so
regeneration and repairs can leave the final HQ at full health despite earlier
damage. Damage events include splash, pellets and poison; they are not a hit
percentage. Combat rates per alive player-second account for time in queues.

Navigation reports complete routes separately from partial routes, failed
routes and stuck escapes. Inspect early samples for connection between bases:
after a base is destroyed, its connectivity result no longer describes the
earlier route. Spawn statistics count physical spawns separately from evolution.
Construction statistics count buildings actually placed through normal rules.
Class-clearance counters describe the bounded checks used to validate larger
alien hulls and their jump limits. A pending class check means that the bot is
waiting for a safe route; it is not counted as a completed route. Detailed
snapshots include the actual hull, next waypoint, collision results and the age
of the most recent movement safety decision to help diagnose crowded exits.

Throughput depends on the CPU, VM, map, congestion and combat. Both time spent
inside the simulation and total process time including startup are reported.
Combined throughput divides completed game seconds by the entire suite's wall
time; it is not the speed of one server.

## Engine controls

The runner configures these automatically:

| Control | Meaning |
| --- | --- |
| `sv_fastSim 1` | Startup-only offline fixed-step acceleration. |
| `sv_simulationSeed <positive integer>` | Startup-only fixed seed passed to game initialization. |
| `net_enabled 0` | Required at startup for fast mode; disables network access. |
| `dedicated 1` | Run without a game client. |
| `sv_fps 20` | Normal 50 ms game frames. |
| `botbench start <humans> <aliens> <seconds>` | Start fresh bell-distributed teams, write telemetry and quit at the result. |

Fast mode rejects enabled networking and connected human clients. It ignores
`timescale` and `fixedtime` so those controls cannot change benchmark physics.
The default server behavior remains paced by wall time. `--speed realtime`
runs the same offline benchmark at normal speed for timing comparisons.

Startup commands have an engine limit of 32. The runner places boot-critical
settings on the command line and executes a generated configuration file for
the remaining settings, map and benchmark command, so large tuning profiles
are not silently truncated.
