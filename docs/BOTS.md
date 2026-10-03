# Tremulous bots

This patch adds server-controlled human and alien players to Tremulous. Bots use
real client slots, spawn queues, movement commands, weapons, credits and build
points. All controls are server console commands or rcon commands; there is no
new menu interface.

Use the patched engine together with the patched game module. The engine adds
bot client allocation and command syscalls. Copying only `game.qvm` or `game.dll`
into an old Tremulous installation is insufficient.

## Play the portable Windows build

Extract the complete portable archive into a writable folder. Keep its
executables, `SDL2.dll`, renderer DLLs, `base` directory, and accompanying license
and credit files together. The `base` directory contains the game modules and
the original game and map PK3 archives.

Run `Play with bots.cmd` to launch ATCS with four bots on each team, then join a
team through the usual game menu. `Dedicated bot server.cmd` starts the same bot
match as a separate server. All supplied bot launchers use varied skills with
the `bell` setting. The commands below show how to launch manually.

From PowerShell in the extracted folder, start a local game:

```powershell
$botGameDir = (Get-Location).Path
.\tremulous.exe +set fs_basepath "$botGameDir" +set fs_homepath "$botGameDir\user" +set sv_maxclients 16 +map atcs
```

Open the game console with the console key, usually `~`, and enter:

```text
bot fill humans 4 bell
bot fill aliens 4 bell
bot list
```

Join either team through the normal game menu. Each bot occupies one client
slot, so leave room for yourself and other players. Set `sv_maxclients` before
loading a map; increasing it while playing requires another map load. A team
still needs a surviving, usable spawn building for queued bots to spawn.

### Larger matches

Run `Play 16 vs 16 bots.cmd` for 16 bots on each team, varied skills in a bell
curve centered at 5.5, with 40 client
slots so human players can join. `Dedicated 16 vs 16 bots.cmd` starts the same
match on a separate server. Both execute `bots16.cfg`, which loads the larger
Arachnid2 map and enables coordinated attacks, queue-scaled spawn building,
navigation tuning and the measured human aim profile.

To expand a running local match, enter these commands in order:

```text
exec bots16.cfg
bot list
```

Increasing `sv_maxclients` takes effect on a full map load. `map arachnid2` restarts
the match; `map_restart 0` also reloads fully when that limit changes. Each bot and
human player uses a slot. The engine limit is 64 total clients, so 31 bots on
each team leave two human slots (`set sv_maxclients 64`). Spawn queues still
apply, so a larger team takes longer to enter the map. If AI causes server
hitches, try `set g_botThink 150` or `200`; movement still runs each server frame.

### Spectate the match

After launching, choose **Spectators** in the team menu or enter `team spectator`
in the game console (`~`). Use the mouse and movement keys for free flight.
`follow` toggles following a bot and free flight; `follownext` and `followprev`
cycle through bots. `bot list` shows IDs, and `follow <id>` follows a particular
alive bot. Let the normal spawn queues fill before expecting a full army in play.

The separate `user` home path keeps this build's configuration and saved
navigation seeds in the portable folder. It also helps avoid loading a different
game module from an existing installation's home directory.

To run a dedicated server instead:

```powershell
$botGameDir = (Get-Location).Path
.\tremded.exe +set fs_basepath "$botGameDir" +set fs_homepath "$botGameDir\user" +set dedicated 1 +set sv_maxclients 16 +map atcs
```

Enter the bot commands in that server's console. Start the client in another
terminal and use `connect localhost:30720` in its game console. For remote
administration, use the normal Tremulous rcon setup and prefix commands with
`rcon`, for example `rcon bot list`.

## Console commands

Team names `humans`, `human`, and `h` are equivalent; likewise `aliens`, `alien`,
and `a`. Skills are integers from 1 through 10, or `bell` for a bell-shaped
spread. IDs come from `bot list`; a
case-insensitive full bot name also works. Quote names containing spaces.

| Command | Effect |
| --- | --- |
| `bot` or `bot help` | Print command help. |
| `bot add <humans\|aliens> [skill\|bell] [name] [attack\|defend\|build]` | Add one bot; the default role is `attack`. `bell` samples its skill from the distribution. |
| `bot fill <humans\|aliens> <bot count> [skill\|bell]` | Adjust that team's number of bots. `bell` distributes skills across the resulting team, including existing bots. Human players do not count. |
| `bot remove <id\|name\|all>` | Remove matching bots and free their slots. |
| `bot tactics [id]` | Inspect group changes and regrouping; an optional bot ID shows current membership, sensed numbers and tactical goal. |
| `bot skill <id\|name\|all> <1-10\|bell>` | Change skill immediately; `all bell` spreads skills across all bots. |
| `bot role <id\|name\|all> <attack\|defend\|build>` | Change the requested role. |
| `bot team <id\|name> <humans\|aliens\|spectator>` | Move one bot to a team, or leave it spectating. |
| `bot list` | List bots and navigation status. |
| `bot buildings` | List living structures, health, power, completion and position. |

The optional arguments to `bot add` are positional. Supply a skill and name
before a role:

```text
bot add humans 6 "Ada" build
bot add aliens 7 "Dretch Patrol" attack
bot skill Ada 8
bot role "Dretch Patrol" defend
bot team Ada spectator
bot team Ada humans
bot remove all
```

With teamwork enabled, filling a team reserves builders and defenders in
proportion to its size. A new 16-bot team has two builders, two defenders and
twelve attackers. Expanding a team adds any missing support roles among new
bots, preserving existing assignments. With teamwork disabled, the first bot
on an empty team is a builder and later bots are attackers. Use `bot role` to
make explicit changes.

For `bell`, evenly spaced normal-distribution quantiles create a balanced spread
and assignments are shuffled so bot IDs and roles do not determine strength.
The distribution has mean 5.5 and approximate standard deviation 1.7, bounded
to levels 1–10. With 16 bots on a team, levels 2 and 9 have one bot each, levels
3 and 8 have one each, levels 4 and 7 have two each, and levels 5 and 6 have four
each. Single-bot `add` or `skill` commands sample the same distribution randomly.
Numeric skill settings still work. Skills persist across map restarts and loads.

| Cvar | Default | Effect |
| --- | --- | --- |
| `g_botThink` | `100` | AI decision interval in milliseconds, clamped to 25–250. Movement still runs on server frames. |
| `g_botSkill` | `5` | Default skill for subsequently added bots. |
| `g_botBuild` | `1` | Enable or disable bot construction and repair behavior. |
| `g_botDebug` | `0` | Add health, class, weapon, credits, position and target details to `bot list`. |
| `g_botCombatTuning` | `0` | Enable human angular aim, finite turning, firing bursts, healing/evolution retreats and corrected alien pounces. |
| `g_botHumanAimCone` | `3` | Base angular aim-error radius in degrees, scaled by skill; `bots16.cfg` uses `6`. |
| `g_botHumanReaction` | `300` | Base target acquisition delay in milliseconds, scaled by skill; the 16v16 profile uses `600`. |
| `g_botHumanTurnSpeed` | `240` | Base aim turning speed in degrees per second, scaled by skill; the 16v16 profile uses `160`. |
| `g_botHumanFireDelay` | `160` | Base pause between firing bursts in milliseconds, scaled by skill; the 16v16 profile uses `300`. |
| `g_botTeamwork` | `0` | Shared observed contacts, dynamic nearby groups, local retreats, swarm approaches and home defense; the 16v16 profile enables it. |
| `g_botTaunt` | `1` | Occasional normal gesture animations after an enemy kill or during quiet moments near teammates; disable with `set g_botTaunt 0`. |
| `g_botSpawnScale` | `0` | Scale spawn targets with team size and queues, prioritize capacity and check spacing/exits; the 16v16 profile enables it. |
| `g_botNavTuning` | `0` | Enable the improved graph, anchoring and partial-route behavior; the 16v16 profile enables it. |
| `g_botNavNodes` | `4096` | Maximum floor nodes, clamped to 1,024–8,192; takes effect on map load. The 16v16 profile uses `8192`. |

For example, `set g_botDebug 1` followed by `bot list` is useful when investigating
an idle or stuck bot. Changing the default skill does not change existing bots;
use `bot skill all 7` for that.

## Roles and game rules

**Attack** bots move toward enemy structures and engage visible opponents.
**Defend** bots stay near friendly structures between fights. **Build** bots
maintain the base and defend themselves against nearby enemies. Urgent builders
limit combat attention to close threats so distant fights do not continually
interrupt spawn construction.

With teamwork enabled, groups form from living fighters who are currently
nearby and can see each other. Membership is recalculated as fighters arrive,
separate or die. There are no fixed squad rosters, designated squad leaders or
base rally launch gates. Each bot keeps its own movement and weapon decisions
while sharing current contacts and objectives with its neighbors.

Fighters compare nearby armed support with freshly sighted opponents. When
outnumbered, they retreat toward actual reinforcements or a checked fallback
position away from the threat, then reassess as the balance changes. Human
attackers and defenders both use this behavior. Aliens converge on nearby
fighters before committing against humans, so support can arrive together.
Brief local regrouping replaces indefinite waiting at the base. Healing,
equipment trips and close-range self-defense still use the ordinary game rules.

Teammates share verified player sightings. Current combat counts use fresh
reports; older last-seen positions are remembered for at most 20 seconds for
searching. Hidden players' live positions are not tracked. Enemy structures
remain shared strategic objectives, with spawns and cores given priority.
After infrastructure is gone, patrol assignments spread fighters across
reachable areas. Builders keep their construction reservations and support roles.

Bots occasionally use the game's normal taunt gesture after a confirmed enemy
kill or while resting near teammates. They avoid gesturing during attacks,
healing, tactical retreats, wall attachment or injury, and accepted gestures
have a 20–40 second cooldown. `set g_botTaunt 0` disables AI gestures. The game
plays the normal animation and taunt event; bots do not send chat messages.

A human builder spawns with a construction kit and uses its blaster in combat,
then restores the kit. A living human reassigned to building can exchange its
weapon at a functioning armoury. An alien builder starts as a granger or advanced
granger, according to stage and allowed classes. An alien reassigned from a combat
class to building may need its next respawn to receive a builder class; changing
roles does not grant a free class change. Queued bots update their requested
spawn loadout when their role changes.

Bots cannot create a free life when their spawn queue is blocked. Disabled spawn
items/classes are respected, with a legal fallback where available. Team and
role settings survive normal map changes and map restarts while the bot remains
connected. Administrators can keep a bot spectating with `bot team ... spectator`.

### Combat and equipment

Bots select visible enemy players and structures using collision traces. Skill
changes reaction delay, aim error and some weapon preferences. Hitscan weapons
aim at the target; projectile attacks lead moving opponents. Advanced dragoon
barbs and advanced granger blobs also receive gravity compensation. Basic
friendly-fire checks reject shots through teammates or friendly buildings;
they do not predict every possible collision or splash interaction. Tuned human
aim uses angular error held for short intervals, finite turning speed and pauses
between bursts. Skill affects all four controls. Lucifer cannon charging still
uses its normal hold/release behavior. Dragoon pounces charge on the ground and
retain their damage payload during flight; melee aims at the near face of large
structures.

Humans start with a rifle, unless assigned to building. Their equipment tree
prefers progressively stronger legal weapons as credits and stages permit:
shotgun/lasgun, then precision or heavier weapons, pulse rifle and, for stronger
bots, Lucifer cannon. It buys armour, helmets, energy battery packs and a
battlesuit when affordable and legal. Battlesuit room is checked before trading
conflicting equipment. Empty weapons reload normally; exhausted weapons fall
back to the blaster. Bots visit powered armouries for ammunition and equipment,
or reactors/repeaters for eligible energy ammunition. They activate carried
medkits when injured or poisoned and can seek medistations for healing and
normal medkit replenishment.

Aliens attempt affordable, stage-legal evolution through the existing evolution
tree. Builders remain grangers rather than spending their role on an attack
class. Evolution uses the original overmind, nearby-human, wall-climbing,
building-delay, credit and class-clearance checks. With combat tuning enabled,
attackers also check whether the prospective form has a complete route to the
enemy objective. Pending checks defer evolution; a known blocked route makes
the bot try the next affordable form. This avoids spending credits on an alien
that fits at home but cannot travel through the map. Combat supports automatic
dretch biting, other melee attacks, marauder jumping, advanced marauder zap,
advanced basilisk poison cloud, dragoon pouncing, advanced dragoon barbs,
tyrant trample and advanced granger blobs. Navigation can use wall climbing
locally for classes that support it.

The shop and evolution code share the same checked functions used by player
console commands. Bots pay prices and evolution costs, receive ordinary sale
refunds, respect equipment slots, and obey stages and disabled items/classes.
They do not receive artificial credits, ammunition, health or build points.

### Base construction

Builders prioritize the headquarters and spawns, then utility and defenses.
Humans aim to maintain a reactor, two telenodes, an armoury and medistation,
followed by turrets, a defense computer and eligible teslas. Alien priorities
include the overmind, two eggs, acid tubes, booster, trapper and eligible hive
and barricade structures. These are bounded plans, rather than an unlimited
attempt to consume every available build point.

Utility positions are sampled near the headquarters. Defenses favor a wider
base perimeter. Sideways and forward collision traces score nearby narrow
passages as possible choke points. A surviving spawn or other friendly
structure can anchor headquarters rebuilding.

Placement is performed through the builder's real weapon and the game's
`G_CanBuild` checks. Build points, power, creep, stage, clearance, construction
time, build delays, surrender and build privileges still apply. Bots do not
consume teammates' deconstruction marks to make space or reclaim build points.
Human builders aim their construction kits to repair existing structures through
the ordinary repair code. Alien structures retain their ordinary regeneration.

## Navigation and manual map seeds

Navigation uses the map's collision model rather than extracting rendered level
triangles. It grows a floor graph incrementally from base structures, player
positions and saved manual seeds. Floor support, slopes, steps, clearance and
hazards matter to traversability. A* routes guide local, class-sized movement
traces; short routes can operate before graph generation completes. Stuck
recovery, jump attempts and wall-climbing escape behavior supplement routing.

Tuned navigation follows stairs as a sequence of ordinary full-hull 18-unit
steps. Local steering checks supported landings along a short lookahead instead
of requiring one raised sweep across the whole staircase. Nearby teammates
remain solid obstacles: bots try a supported side or back step, with brief
right-of-way pauses, and do not blacklist a stair waypoint solely for a crowd.
Local steering also checks supported downward drops up to 96 units with the
whole standing hull and fresh hazard checks, so the stair guard does not stop
bots before a legal low ledge. A missing landing still stops movement.
Bots stranded on teammates or their own buildings try checked lateral steps
to a nearby floor. A wall walker without a floor anchor can use short, checked
surface crawls during stuck recovery, preferring downward travel toward a floor.
Wall walkers check movement in their current surface plane and can release an
underside attachment when stalled above a verified nearby floor.

Route costs include corridor preferences for each current nearby group and
bounded congestion penalties for other groups' upcoming paths. Preferences
change with group membership, lives and stuck recovery, while each actual route still
passes the class's collision checks. These costs encourage alternatives when
the map offers them; a passage with only one legal exit still needs queuing.
Wall-capable alien fighters can choose a short surface flank during a supported
approach. The entry, crawl and floor exit require actual hull collision and
hazard checks. This supplements the floor graph with local wall travel; it does
not assume that every visible wall leads to a usable route.

Graph generation is bounded: up to 8,192 floor nodes, 12 directed links per node,
256 manual seeds and a generation trace budget per server frame. `botnav status`
reports whether the graph is still growing. Graph nodes themselves are rebuilt
on map initialization; only explicit manual seeds are saved.

With navigation tuning enabled, floor connectors use the bot's actual footprint
and standing height, including small aliens beneath overhangs. Wider classes can
adjust a graph point's feet by at most an ordinary 18-unit step to fit ramps.
An ascending graph edge above the class's single-jump height requires full
class-sized floor and clearance checks along the ramp. Its total rise can exceed
a jump only when continuous supported movement passes those checks.
Static connector proofs are cached briefly for the exact class and endpoints;
hazards are checked again on reuse. Unknown checks and mover collisions are
deferred. Directions blocked by moving doors are retained for bounded retries.
Bots approach an already verified door-frontier node more closely to touch
normal door triggers; the graph never creates a link across a closed door.
Bots receive the first planning opportunity in turn so one client's
queries cannot continually consume the shared class-check budget.

| Command | Effect |
| --- | --- |
| `botnav` or `botnav status` | Print nodes, links, progress, manual seeds and hurt-volume counts. |
| `botnav check <class number> <from feet x y z> <to feet x y z>` | Read-only class connector diagnostic: 1 verified, 0 blocked, -1 deferred; coordinates describe feet rather than player origins. |
| `botnav add <client number>` | Add a seed at an active player's feet. |
| `botnav add <x> <y> <z>` | Add a seed using map coordinates. |
| `botnav save` | Save current manual seeds to `botnav/<mapname>.nav`. |
| `botnav load` or `botnav rebuild` | Regenerate the graph and reload saved manual seeds. |
| `botnav clear` | Clear the in-memory manual seed list; save afterward to replace the file. |
| `botnav help` | Print available navigation commands. |

To help a disconnected room or route, walk a living player to useful points and
add that player's client number from the server console or rcon:

```text
botnav add 0
botnav save
botnav rebuild
botnav status
```

Seeds should lie on accessible floor space near corridor turns, ramps, doorways
and otherwise disconnected floor regions. They do not create edges through
walls or invent support over a pit. For coordinate entry, use feet/floor
coordinates rather than the player's eye position.

The saved file contains one whitespace-separated `x y z` triple per seed:

```text
128.000 -256.000 16.000
256.000 -256.000 16.000
```

With the portable launch commands above, files are written under
`user/base/botnav/`. Tremulous's normal virtual filesystem determines the write
location when different `fs_homepath` or `fs_game` settings are used. Distribute
map-specific seed files in the corresponding `base/botnav/` directory or inside
a PK3. Rebuilding reloads saved seeds and discards unsaved manual changes.

## Fast parallel matches

The patched dedicated server can run fixed-seed, offline matches without
wall-clock pacing. The [benchmark guide](BOT_BENCHMARKS.md) covers parallel
suites, paired configurations, actual VM verification, deterministic replay and
base-pressure diagnostics. It uses normal physics and economy. Fast mode is
offline; use the play launcher to spectate.

## Build from source

Use this patched source checkout, CMake 3.25 or newer, and a supported C compiler.
The game modules and engine must come from the same build. This patch adds no
external bot framework or navigation library dependency.

On Windows, install Visual Studio's C++ desktop build tools and CMake, then run:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DBUILD_STANDALONE=ON -DBUILD_GAME_LIBRARIES=ON -DBUILD_GAME_QVMS=ON
cmake --build build --config Release --parallel
```

For a single-configuration generator on Linux or another supported platform:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_STANDALONE=ON -DBUILD_GAME_LIBRARIES=ON -DBUILD_GAME_QVMS=ON
cmake --build build --parallel
```

Install the platform's normal client dependencies, including SDL2 development
files where an internal SDL binary is unavailable. Existing CMake options control
renderers, codecs and other libraries. For a server-only build, add
`-DBUILD_CLIENT=OFF -DBUILD_RENDERER_GL1=OFF -DBUILD_RENDERER_GL2=OFF`.

Release output is placed in `build/Release/`. Keep its executable, renderer and
runtime libraries together. Native modules are under `base/` and QVMs under
`base/vm/`. CMake builds the bundled QVM compiler tools automatically; no separate
QVM SDK installation is needed.

Compilation does not produce the complete game media. Add the original
Tremulous data and map PK3 archives to the runtime `base` directory and retain
the source checkout's `assets` overrides in the final runtime package. Ensure
the patched game module has priority over older modules embedded in data PK3s.
The portable package already assembles these pieces.

`vm_game` selects the server game module implementation:

| Value | Mode |
| --- | --- |
| `0` | Native game library, such as `game.dll` on Windows. |
| `1` | Interpreted game QVM. |
| `2` | Compiled game QVM. |

Set the cvar before loading the map. Client `cgame` and `ui` module settings are
separate. All three server modes need the patched engine's bot syscalls.

## Reproduce verification

The smoke-test script runs a dedicated server using the supplied runtime base
path. From the source checkout, use Python 3 and an absolute path to the patched
server and complete runtime directory:

```powershell
python tests/bot_smoke.py --server "C:\Games\Tremulous-Bots\tremded.exe" --basepath "C:\Games\Tremulous-Bots" --vm 0 --duration 45
python tests/bot_smoke.py --server "C:\Games\Tremulous-Bots\tremded.exe" --basepath "C:\Games\Tremulous-Bots" --vm 1 --duration 45
python tests/bot_smoke.py --server "C:\Games\Tremulous-Bots\tremded.exe" --basepath "C:\Games\Tremulous-Bots" --vm 2 --duration 45
python tests/bot_smoke.py --server "C:\Games\Tremulous-Bots\tremded.exe" --basepath "C:\Games\Tremulous-Bots" --vm 2 --duration 45 --economy
python tests/bot_smoke.py --server "C:\Games\Tremulous-Bots\tremded.exe" --basepath "C:\Games\Tremulous-Bots" --map arachnid2 --vm 2 --duration 90 --bots-per-team 16 --bell-skills --balanced-profile
```

`--balanced-profile` applies and verifies the nine settings used by `bots16.cfg`
before adding bots and after both kinds of map reload. Without it, the script
uses the legacy defaults. Add it to the native/interpreted and economy commands
above when checking the current 16v16 profile.

`--economy` sets the existing passive-credit period to one second and stage
thresholds to zero for that isolated test server. Shopping and evolution still
use normal player checks. It additionally asserts a purchased human weapon and
an evolved alien fighter. These accelerated settings are absent from the
playable launchers.

Read the script's result and server logs for each invocation. A successful build
alone does not establish successful runtime behavior, and a short smoke test
does not establish competent play on every map.

The earlier **v0.1.0-bots** Windows x64 release was verified on ATCS using the native module,
the QVM interpreter and compiled QVM. Live checks cover both teams spawning,
movement, collision-graph generation, saved navigation seeds, timeouts/inactivity,
skill/role/team commands, invalid input, map restart/full reload, slot reuse and
removal. The accelerated economy run also observed pulse rifles/Lucifer cannons,
battlesuits, advanced grangers and evolved fighters through tyrants, plus legal
construction and combat. Passed JSON reports accompany the portable package in
`validation/`. These are bounded gameplay checks, not a guarantee of competent
play on every map.

That release's graphical client also passed an ATCS startup check with the OpenGL renderer,
UI/game/cgame QVMs, a local player and eight bots, followed by a clean quit. The
original 1.1/GPP media lacks newer optional first-person weapon animation
configuration files; those produce nonfatal log messages and use the available
static weapon models.

Its mixed-skill 16-vs-16 run passed a 90-second live server check: 32 bot slots,
the expected per-team skill histograms, movement, construction, combat, console
controls and skill persistence on both map restarts and full reloads. Normal
spawn queues and blocked spawns meant 12 humans and all 16 aliens had been
observed alive during that interval; a larger roster does not bypass spawn
cooldowns. Native and interpreted QVM checks also passed with `bell` skills.

To assemble a portable archive from a matching Windows Release build and the
upstream media, use:

```powershell
python misc/package_bots.py --build build/Release --data path/to/tremulous-data --output dist-bots
```

The data directory needs its original two data PK3s, all eight map PK3s, licenses,
credits and a GitHub contents manifest named `upstream-manifest.json` identifying
their original sizes and Git blob hashes. The packager checks media integrity,
includes current QVMs and assets in a new PK3, retains the original archives,
generates SHA256 hashes and verifies both output ZIPs. Add `--report path/to/report.json`
for each passed smoke-test report to include it. The separate source ZIP contains
the full patched source checkout without build outputs or local test homes.
Use `--vc-runtime path/to/x64/vcruntime140.dll` to include the unmodified compiler
redistributable runtime. Otherwise the target machine needs the corresponding
Microsoft Visual C++ x64 runtime installed. The supplied portable archive
includes it and targets Windows 10/11 x64.

For manual gameplay checks, start `atcs`, fill both teams, enable `g_botDebug`,
and observe `bot list`, `bot buildings` and `botnav status`. Check that bots
spawn, move, take damage, earn and spend credits, and rejoin normal spawn queues
after dying. Change skills and roles, move one bot to spectator and back, then
test both `map_restart` and a full map change. Also test an empty spawn queue
when its team's last spawn is gone, restricted stages/items/classes, insufficient
credits, missing or unpowered armouries, restricted evolution clearance, and
construction without sufficient build points. Compare disabled bot construction
with `set g_botBuild 0`.

Longer play sessions should examine combat at each stage, base recovery, map
hazards, and paths far from the starting bases. Test native and QVM execution
separately when modifying shared code. Keep QVM-facing code compatible with the
bundled compiler's C dialect.

## Developer map

| File | Responsibility |
| --- | --- |
| `code/game/g_bot.c`, `g_bot.h` | Lifecycle, console commands, roles, command scheduling and shared bot state. |
| `code/game/g_bot_nav.c` | Collision graph, A*, saved seeds, local steering and final movement safety checks. |
| `code/game/g_bot_combat.c` | Targets, aiming, attacks, shop/evolution decisions and spawn loadouts. |
| `code/game/g_bot_build.c` | Construction plans, placement scoring, builder reservations and human repairs. |
| `code/game/g_bot_team.c` | Sighted contacts, current nearby groups, swarm convergence, tactical retreats and home defense. |
| `code/game/g_bot_probe.c` | Explicit offline fixture controls for testing groups and physical movement; disabled during ordinary play. |
| `code/game/g_bot_benchmark.c` | Offline match control, actual gameplay counters, queue/class occupation and optional position/collision snapshots. |
| `code/game/g_cmds.c` | Shared checked buy/sell/class-change operations used by players and bots. |
| `code/game/g_active.c`, `g_main.c`, `g_client.c` | Frame integration, bot movement, connection/disconnection and voting behavior. |
| `code/game/g_public.h`, `g_syscalls.c`, `g_syscalls.asm` | Engine/game bot syscall interface. |
| `code/server/sv_game.c` and related server files | Engine client-slot allocation, usercmds and network-free bot lifecycle. |
| `cmake/basegame.cmake` | Native and QVM inclusion of the new bot source files. |

The game AI supplies `usercmd_t` inputs before `G_RunClient`; Pmove and existing
weapon code execute those inputs. Shopping and evolution reuse checked player
operations. Bot identity comes from the engine's client flag, rather than a
client-controlled userinfo key. Bots do not inflate human vote denominators or
hold intermission open waiting for a network acknowledgement.

If extending bots, preserve the original gameplay validation and keep work
bounded per server frame. Extend navigation with explicit traversability rules
for special movers or surfaces rather than assuming visible geometry is walkable.

## Known limits

- The global graph represents floors. Wall walking uses local flanks and recovery;
  it is not a complete wall-and-ceiling surface graph or a guarantee of a route
  to every ceiling-mounted base.
- Complex moving platforms, timed jumps, teleporters and unusual doors lack
  specialized route/action planning. Manual seeds can improve ordinary floor
  coverage but cannot teach those mechanics by themselves.
- Graph size and trace budgets are bounded. Large maps, narrow passages and
  disconnected regions can need manual seeds or additional navigation work.
  Large evolved classes can be unable to use routes a smaller class can traverse.
- Pounce, trample and jump choices are heuristics; they can miss moving targets
  or be poorly suited to a particular room. Projectile prediction is approximate.
- Base placement samples a small number of floor positions and a local choke
  score. It can choose awkward locations, leave space unused or fail in cramped
  bases. It does not construct elaborate forward bases or ceiling layouts.
- Equipment and evolution trees are simple preferences. Coordinated groups can
  still disperse in combat, crowd chokepoints or fail to reach their objective.
  Bots do not identify every tactical threat, plan jetpack flights or optimize
  class composition.
- Friendly-fire checks are basic. Explosive splash and moving teammates can still
  cause accidental damage under the server's normal friendly-fire settings.

## Media, credits and licenses

The game and map data come from the original Tremulous releases preserved by
[wtfbbqhax/tremulous-data](https://github.com/wtfbbqhax/tremulous-data). The package
retains the original data/map PK3 files and this source checkout's existing
assets. The bot patch does not replace the game's artwork, models, audio or
maps.

Credit the Tremulous team, original media/map authors and the ioquake3/id
Software engine contributors. Preserve the upstream
[CREDITS.md](https://github.com/wtfbbqhax/tremulous-data/blob/master/CREDITS.md)
and license notices when redistributing. Tremulous's license overview separates
code under the GNU GPL from media under Creative Commons Attribution-ShareAlike
2.5; individual bundled components retain their stated exceptions and notices.
See this checkout's `COPYING.txt`, `GPL` and `CC`, and the data repository's
[COPYING](https://github.com/wtfbbqhax/tremulous-data/blob/master/COPYING),
[GPL](https://github.com/wtfbbqhax/tremulous-data/blob/master/GPL) and
[CC](https://github.com/wtfbbqhax/tremulous-data/blob/master/CC). New bot source
files follow the game's GPL-2.0-or-later terms.
