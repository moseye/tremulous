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

For physical movement diagnostics, add `--set g_botBenchmarkSampleMsec=500`
with details enabled. The diagnostic interval defaults to 30,000 ms and is
bounded to 250–30,000 ms; it changes observations, not game ticks. Detailed
samples include physical spawn generation, velocity, movement commands, nearby
group membership, sensed support/opponent counts, tactical regroup orders,
wall-flank phases and accepted taunt events. Compare movement only across continuous
living samples from the same physical generation and class. Different route
preferences alone do not prove different traversed corridors; use actual
positions and crossing gates. Finer samples increase output and analysis cost.

Use separate offline fixtures for controlled placement, group merge/split,
outnumbered retreats and wall movement. `botprobe` requires explicit
`g_botProbe 1`, `sv_cheats 1`, `dedicated 1` and `net_enabled 0`; ordinary matches
leave it disabled. Fixture snapshots mark forced class/spawn and placement
generations separately from natural physical spawns. A placement or manual
input demonstrates only the subsequent observed behavior, and is excluded
from fair match and economy evidence. Requested gesture buttons do not prove
taunting: accepted `EV_TAUNT` counts and normal animation timers do.

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
With `teamwork_mode: dynamic_nearby`, the legacy `attack_waves`, `rallied_players` and
`defensive_dispatches` fields describe cumulative local group formations, current
regrouping actors and cumulative tactical withdrawals. `launched_members`
counts membership changes, `peak_group` is the largest observed nearby
component, `advance_orders` counts changed tactical orders, and
`active_assault_members` counts living attackers.
`progress_renewals` and `timed_wave_recalls` remain zero:
fixed wave lifetimes were removed. Historical checkpoints retain their original
wave-based meanings. Use each actor's current `tactics` object to examine
membership and local numerical pressure.
Class-clearance counters include movement, rally/search and prospective evolution
checks used to validate actual hulls and their jump limits. Evolution queries do
not increment movement route-plan totals. A pending class check means that the bot is
waiting for a safe route; it is not counted as a completed route. Detailed
snapshots include the actual hull, next waypoint, collision results and the age
of the most recent movement safety decision to help diagnose crowded exits.

`class_ascent_checks`, `class_ascent_passed`, `class_ascent_rejected` and
`class_ascent_deferred` count uncached rising-edge queries whose total rise
exceeds that class's single-jump height. Accepted queries prove actual class
clearance and floor support along a continuous ramp. Deferred queries can recur
when the shared trace budget is exhausted; they are not cached as blocked.
These query totals do not count distinct ramps or completed journeys.

Moving-door diagnostics report `mover_pending`, `mover_retry_attempts`,
`mover_resolved_attempts`, `mover_rejected_attempts` and `mover_dropped`.
Generation retains up to 512 blocked node/direction pairs and retries up to four
per frame within the generation work allowance. A resolved attempt passed the
same supported collision checks; it does not necessarily add a new link if that
link already exists. A rejected attempt has a known failure. The queue does not
force doors open or prove a route through a closed door. Failed class-route
queries remain pending when their reached component contains a queued frontier.

## Recorded checkpoint

[The 2026-10-02 checkpoint](../tests/results/bot-balance-checkpoint-2026-10-02.json)
contains sanitized hashes, settings and results from the fair-planning build
before the moving-door retry fix. It records six native 40-minute 16v16 matches
on Arachnid2/Niveus, a compiled-QVM 15-minute Niveus match and twelve 15-minute
matches across four additional maps. The native long suite simulated 14,400
game seconds in 215.53 wall seconds across three workers. All six reached the
time limit; alien building damage remained very low. Eleven of the twelve
additional-map runs never connected their bases. These results identify work
still needed; they do not establish balanced play or certify a later build.

The checkpoint also records matching native real-time/fast/replayed telemetry
for a 15-second startup scenario. Those checks prove tick and replay behavior
for that scenario; the short equivalence run did not include combat.

[The progress-retention checkpoint](../tests/results/bot-progress-retention-2026-10-02.json)
records the later `b683f331` runtime, before the graph-ramp fix. A same-runtime
Arachnid2 comparison over three seeds reduced human player kills from 699 to
401 and increased alien player kills from 89 to 123 with the selected aim
settings. Alien building pressure remained weak. Six 40-minute native matches
on Arachnid2/Niveus produced one human win and five time-limit draws. Nexus6
connected its bases, but activity still stopped well before the time limit.
The checkpoint also records live smoke tests, within-mode replays and a local
portable spectator preview. It identifies tested hashes and limitations;
it does not certify the later ramp fix or a final release.

[The ramp-validation checkpoint](../tests/results/bot-ramp-validation-2026-10-02.json)
records the subsequent supported-ramp fix and its release-version checks.
Nine native 40-minute matches ended in draws. Nexus6 full-route success rose
to 14–18%, and two seeds continued alien combat beyond minute 38. Alien damage
to human buildings remained low: 137 total across the six Arachnid2/Niveus
matches, and zero across Nexus6. Some actors still stalled on world geometry or
teammates and structures. The file distinguishes the modules used for those
long matches from the later binaries with refreshed version labels.

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
