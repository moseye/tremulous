"""Run isolated, offline, real Tremulous bot matches in parallel.

Fast mode removes wall-clock waits while retaining the normal server tick. No
economy, stage, damage, spawn, or movement rules are accelerated. Each run keeps
its telemetry, gameplay log, stdout and report. Python standard library only.

Example:
  python tests/bot_benchmark.py --server build-bots-msvc/Release/tremded.exe
    --basepath build-bots-msvc/Release --seeds 17,29,41 --seconds 900 --jobs 3
  python tests/bot_benchmark.py --compare baseline/aggregate.json candidate/aggregate.json
"""
from __future__ import annotations

import argparse
import collections
import concurrent.futures
import hashlib
import json
import math
import os
import pathlib
import re
import statistics
import subprocess
import time
import uuid


SCHEMA = 1
TEAMS = ("humans", "aliens")
WINNERS = ("humans", "aliens", "draw")
RESERVED_CVARS = {
    "fs_basepath", "fs_homepath", "fs_game", "dedicated", "net_enabled",
    "net_ip", "sv_fastsim", "sv_simulationseed", "sv_fps", "sv_maxclients",
    "sv_privateclients", "vm_game", "timescale", "fixedtime", "rconpassword",
    "sv_master1", "sv_master2", "logfile", "g_logfile", "g_logfilesync",
}
POLL_SECONDS = 0.025
VM_MODES = {0: "native", 1: "interpreted_qvm", 2: "compiled_qvm"}
LONG_MATCH_SECONDS = 900
NEGLIGIBLE_BUILDING_DAMAGE_PER_SECOND = 0.25
BOOT_CVARS = {"fs_basepath", "fs_homepath", "dedicated", "net_enabled", "sv_fastsim",
              "sv_simulationseed", "sv_fps", "sv_maxclients", "vm_game"}


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def sha256_file(path):
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def build_identity(server, basepath, vm):
    """Record both loose modules and archives; precedence remains the engine's."""
    files = [server]
    base = basepath / "base"
    if vm == 0:
        files.extend(path for pattern in ("game*.dll", "game*.so", "game*.dylib",
                                          "qagame*.so", "qagame*.dylib")
                     for path in sorted(base.glob(pattern)))
    else:
        files.append(base / "vm" / "game.qvm")
    files.extend(sorted(base.glob("*.pk3")))
    return {
        "server": str(server), "basepath": str(basepath), "vm": vm,
        "files": [{"path": str(path), "name": path.name,
                   "bytes": path.stat().st_size, "sha256": sha256_file(path)}
                  for path in files if path.is_file()],
    }


def parse_settings(items):
    settings = {}
    for item in items:
        if "=" not in item:
            raise ValueError(f"expected KEY=VALUE, got {item!r}")
        key, value = item.split("=", 1)
        if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*", key):
            raise ValueError(f"invalid cvar name {key!r}")
        if key.lower() in RESERVED_CVARS:
            raise ValueError(f"{key} is controlled by the benchmark runner")
        if any(character in value for character in '\r\n;"'):
            raise ValueError(f"{key} contains a console command separator or quote")
        if any(existing.lower() == key.lower() for existing in settings):
            raise ValueError(f"duplicate cvar {key!r}")
        settings[key] = value
    return settings


def parse_seeds(text):
    try:
        values = [int(part.strip()) for part in text.split(",")]
    except ValueError as error:
        raise ValueError("--seeds must be comma-separated positive integers") from error
    if not values or any(value < 1 or value > 2147483647 for value in values):
        raise ValueError("seeds must be from 1 through 2147483647")
    if len(set(values)) != len(values):
        raise ValueError("duplicate seeds; use --repeats for deterministic replay")
    return values


class TelemetryReader:
    """Read complete JSONL records without interpreting an unfinished write."""

    def __init__(self, path, seed):
        self.path = path
        self.seed = seed
        self.offset = 0
        self.partial = b""
        self.events = []

    def poll(self):
        if not self.path.exists():
            return []
        with self.path.open("rb") as source:
            if source.seek(0, 2) < self.offset:
                raise ValueError("telemetry file was truncated (unexpected map reload)")
            source.seek(self.offset)
            data = source.read()
            self.offset = source.tell()
        lines = (self.partial + data).split(b"\n")
        self.partial = lines.pop()
        new_events = []
        for line in lines:
            if not line.strip():
                continue
            event = json.loads(line)
            self.validate(event)
            self.events.append(event)
            new_events.append(event)
        return new_events

    def validate(self, event):
        if not isinstance(event, dict) or event.get("schema") != SCHEMA:
            raise ValueError("unsupported botbench telemetry schema")
        kind = event.get("event")
        if kind not in ("start", "sample", "finish"):
            raise ValueError(f"unexpected telemetry event {kind!r}")
        elapsed = event.get("elapsed_ms")
        if type(elapsed) is not int or elapsed < 0:
            raise ValueError("elapsed_ms must be a nonnegative integer")
        if event.get("seed") != self.seed:
            raise ValueError("telemetry seed differs from the requested seed")
        if not self.events and kind != "start":
            raise ValueError("telemetry is missing its start record")
        if self.events:
            if kind == "start" or self.events[-1]["event"] == "finish":
                raise ValueError("unexpected second match in one process")
            if elapsed < self.events[-1]["elapsed_ms"]:
                raise ValueError("game clock moved backward")
        winner = event.get("winner")
        if kind == "finish":
            if winner not in (*WINNERS, "error") or not isinstance(event.get("reason"), str):
                raise ValueError("finish record requires a winner/draw/error and reason")
        elif winner != "running":
            raise ValueError("nonterminal record contains a winner")
        for key in (*TEAMS, "nav"):
            if not isinstance(event.get(key), dict):
                raise ValueError(f"telemetry is missing {key} metrics")


def gameplay_metrics(path):
    """Independent counts from ordinary gameplay events, retaining world deaths."""
    text = path.read_text(encoding="utf-8", errors="replace") if path.exists() else ""
    clients = {}
    kills = collections.Counter()
    deaths = collections.Counter()
    methods = collections.Counter()
    builds = collections.Counter()
    spawn_builds = collections.Counter()
    events = sorted(
        [(match.start(), "team", match.groups()) for match in re.finditer(
            r"ChangeTeam: (\d+) (human|alien|spectator):", text)]
        + [(match.start(), "die", match.groups()) for match in re.finditer(
            r"Die: (\d+) (\d+) (MOD_[A-Z0-9_]+):", text)]
        + [(match.start(), "build", match.groups()) for match in re.finditer(
            r"Construct: (\d+) \d+ ([a-zA-Z0-9_]+)", text)],
        key=lambda item: item[0],
    )
    for _, kind, values in events:
        if kind == "team":
            client, team = values
            clients[int(client)] = {"human": "humans", "alien": "aliens"}.get(team)
        elif kind == "die":
            killer, victim, method = values
            methods[method] += 1
            deaths[clients.get(int(victim)) or "other"] += 1
            attacker_team = clients.get(int(killer))
            if attacker_team:
                kills[attacker_team] += 1
            else:
                kills["world_or_building"] += 1
        else:
            builder, buildable = values
            team = clients.get(int(builder)) or "other"
            builds[team] += 1
            if buildable in ("eggpod", "egg", "telenode"):
                spawn_builds[team] += 1
    return {"kills_by_team": dict(kills), "deaths_by_team": dict(deaths),
            "kills_by_method": dict(methods), "builds_by_team": dict(builds),
            "spawn_builds_by_team": dict(spawn_builds),
            "exit_reasons": re.findall(r"Exit: ([^\r\n]+)", text)}


def stable_telemetry(events):
    """Exclude the one nondeterministic engine wall timer from replay evidence."""
    return [{key: value for key, value in event.items() if key != "wall_elapsed_ms"}
            for event in events]


def telemetry_digest(events):
    payload = json.dumps(stable_telemetry(events), sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(payload.encode()).hexdigest()


def telemetry_diagnostics(events):
    """Sampled evidence describes coverage, independently of process success."""
    connections = [event for event in events if "bases_connected" in event["nav"]]
    connected = [event for event in connections if event["nav"]["bases_connected"] == 1]
    return {
        "base_connection_observed": bool(connected) if connections else None,
        "first_base_connection_observed_seconds": connected[0]["elapsed_ms"] / 1000 if connected else None,
        "first_structure_damage_observed_seconds": {
            team: next((event["elapsed_ms"] / 1000 for event in events
                        if event[team].get("building_damage", 0) > 0), None)
            for team in TEAMS},
        "minimum_observed_core_health": {
            team: min((event[team]["core_health"] for event in events
                       if type(event[team].get("core_health")) in (int, float)), default=None)
            for team in TEAMS},
        "first_core_missing_observed_seconds": {
            team: next((event["elapsed_ms"] / 1000 for event in events
                        if event[team].get("core_health") == 0), None)
            for team in TEAMS},
        "observation_note": "Connections, structure damage and core health are observed at telemetry sample times, not exact event times. A rebuilt core can hide earlier HQ destruction in terminal health alone.",
    }


def observed_vm_mode(stdout):
    match = re.search(r"^game\s*:\s*(native|compiled on load|interpreted)\s*$", stdout, re.M)
    if match:
        return {"native": "native", "compiled on load": "compiled_qvm",
                "interpreted": "interpreted_qvm"}[match.group(1)]
    # Older reports predate explicit vminfo. Native loader success is still
    # authoritative when the log identifies the game module's entry point.
    if re.search(r"Sys_LoadGameDll\([^\r\n]*[/\\](?:qagame|game)[^/\\)]*\) found vmMain", stdout):
        return "native"
    return None


def comparability_summary(runs):
    requested = sorted({VM_MODES[run["vm"]] for run in runs})
    observed = sorted({run.get("observed_vm_mode") for run in runs if run.get("observed_vm_mode")})
    return {
        "requested_vm_modes": requested, "observed_vm_modes": observed,
        "all_module_modes_observed": bool(runs) and all(run.get("observed_vm_mode") for run in runs),
        "all_requested_modes_match_observed": bool(runs) and all(
            run.get("observed_vm_mode") == VM_MODES[run["vm"]] for run in runs),
        "all_requested_vms_match_play_launcher": bool(runs) and all(run["vm"] == 2 for run in runs),
        "all_observed_modules_are_compiled_qvm": bool(runs) and all(
            run.get("observed_vm_mode") == "compiled_qvm" for run in runs),
        "note": "Use the same actual VM mode and server tick for A/B comparisons. Native results do not prove identical compiled-QVM play outcomes.",
    }


def diagnostic_warnings(runs):
    """Warn about weak strategic evidence without reclassifying valid draws."""
    warnings = []
    long_runs = [run for run in runs if run["status"] == "finished"
                 and run.get("simulated_seconds", 0) >= LONG_MATCH_SECONDS]
    if long_runs:
        seconds = sum(run["simulated_seconds"] for run in long_runs)
        damage_known = all(type(run["terminal"][team].get("building_damage")) in (int, float)
                           for run in long_runs for team in TEAMS)
        pressure = (sum(run["terminal"][team]["building_damage"]
                        for run in long_runs for team in TEAMS) / seconds if damage_known else None)
        if (all(run["winner"] == "draw" and run["reason"] == "time_limit" for run in long_runs)
                and pressure is not None and pressure < NEGLIGIBLE_BUILDING_DAMAGE_PER_SECOND):
            warnings.append({
                "code": "strategic_stalemate",
                "message": f"All {len(long_runs)} long matches reached the game-time limit with negligible structure pressure. These completed runs do not establish effective objective attacks or balanced wins.",
                "long_match_minimum_seconds": LONG_MATCH_SECONDS,
                "combined_building_damage_per_game_second": pressure,
                "negligible_pressure_threshold": NEGLIGIBLE_BUILDING_DAMAGE_PER_SECOND,
            })
        if not damage_known:
            warnings.append({"code": "missing_objective_metrics",
                             "message": "Long-match reports lack building-damage counters. Objective pressure cannot be established from these results."})
        for team in TEAMS:
            no_structure_damage = [run for run in long_runs
                                   if run["terminal"][team].get("building_damage") == 0]
            if no_structure_damage:
                warnings.append({
                    "code": "zero_structure_damage", "team": team,
                    "message": f"{team.capitalize()} recorded zero cumulative structure damage in {len(no_structure_damage)} long matches. Combat kills do not establish that this team attacks objectives.",
                    "scenarios": [{"map": run["map"], "seed": run["seed"], "repeat": run.get("repeat", 0)}
                                  for run in no_structure_damage],
                })
        no_kills = [run for run in long_runs if all(
            type(run["terminal"][team].get("kills")) in (int, float) for team in TEAMS) and sum(
            run["terminal"][team].get("kills", 0) + run["terminal"][team].get("defense_kills", 0)
            for team in TEAMS) == 0]
        if no_kills:
            warnings.append({
                "code": "no_recorded_kills",
                "message": f"{len(no_kills)} long matches recorded zero kills in the available player/defense counters. Inspect movement and combat before treating their draws as balance evidence.",
                "scenarios": [{"map": run["map"], "seed": run["seed"], "repeat": run.get("repeat", 0)}
                              for run in no_kills],
            })
        disconnected = [run for run in long_runs
                        if run.get("telemetry_diagnostics", {}).get("base_connection_observed") is False]
        if disconnected:
            warnings.append({
                "code": "no_observed_base_connection",
                "message": f"{len(disconnected)} long matches never showed a navigation connection between bases in their sampled telemetry. Inspect routes; disconnected bots can produce inactive draws.",
                "scenarios": [{"map": run["map"], "seed": run["seed"], "repeat": run.get("repeat", 0)}
                              for run in disconnected],
            })
    fallbacks = [run for run in runs if run.get("observed_vm_mode")
                 and run["observed_vm_mode"] != VM_MODES[run["vm"]]]
    if fallbacks:
        warnings.append({"code": "module_fallback",
                         "message": f"{len(fallbacks)} runs loaded a different VM mode from the requested one. Compare actual module modes before interpreting performance or gameplay differences."})
    return warnings


def load_aggregate(path):
    """Read legacy evidence from its retained logs without modifying reports."""
    document = json.loads(path.read_text(encoding="utf-8"))
    if document.get("schema") != SCHEMA or not isinstance(document.get("runs"), list):
        raise ValueError(f"{path} is not a supported aggregate report")
    for run in document["runs"]:
        if not run.get("home"):
            continue
        home = pathlib.Path(run["home"])
        stdout = home / "stdout.log"
        if not run.get("observed_vm_mode") and stdout.is_file():
            mode = observed_vm_mode(stdout.read_text(encoding="utf-8", errors="replace"))
            if mode:
                run["observed_vm_mode"] = mode
                run["observed_vm_mode_source"] = "retained_stdout_log"
        telemetry = home / "base" / "botbench.jsonl"
        if ("minimum_observed_core_health" not in run.get("telemetry_diagnostics", {})
                and telemetry.is_file()):
            reader = TelemetryReader(telemetry, run["seed"])
            try:
                reader.poll()
                run["telemetry_diagnostics"] = telemetry_diagnostics(reader.events)
            except (ValueError, OSError) as error:
                run["legacy_diagnostic_evidence_error"] = str(error)
    return document


def stop_process(process):
    if process.poll() is None:
        process.terminate()
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            process.kill()
            process.wait(timeout=5)


def bot_settings_evidence(settings, stdout, require_all=False):
    """Verify requested bot cvars against the loaded game's cvarlist output.

    Quake accepts unknown `set` names as user-created cvars. The ninth flag
    printed by cvarlist is `?` for those names; successful game registration
    removes that flag. General game cvars remain flexible and are not checked.
    """
    requested = {key: value for key, value in settings.items()
                 if key.lower().startswith("g_bot")}
    listed = {}
    for flags, name, value in re.findall(
            r'^([SsURIALC? ]{9}) (g_bot[A-Za-z0-9_]*) "([^"\r\n]*)"\r?$',
            stdout, re.MULTILINE | re.IGNORECASE):
        listed[name.lower()] = {"name": name, "flags": flags, "value": value,
                                "registered": "?" not in flags}
    observed = {name: listed[name.lower()] for name in requested
                if name.lower() in listed}
    missing = sorted(set(requested) - set(observed))
    unknown = sorted(name for name, evidence in observed.items()
                     if not evidence["registered"])

    def same_value(actual, expected):
        if actual == expected:
            return True
        try:
            return math.isfinite(float(actual)) and float(actual) == float(expected)
        except ValueError:
            return False

    mismatched = sorted(name for name, evidence in observed.items()
                        if evidence["registered"]
                        and not same_value(evidence["value"], requested[name]))
    problems = []
    if unknown:
        problems.append("unregistered bot cvars: " + ", ".join(unknown))
    if mismatched:
        problems.append("bot cvar values differ from the requested profile: " + ", ".join(mismatched))
    if require_all and missing:
        problems.append("missing engine registration evidence for bot cvars: " + ", ".join(missing))
    return {"source": "engine_cvarlist_after_map", "requested": requested,
            "observed": observed, "missing": missing, "unregistered": unknown,
            "mismatched_values": mismatched,
            "verified": not (missing or unknown or mismatched),
            "invalid": bool(problems), "error": "; ".join(problems) or None}


def prepare_command(server, home, settings, map_name, count, seconds):
    # Com_ParseCommandLine accepts at most 32 +commands. Put only early-init
    # settings in argv, then execute all remaining settings and match startup
    # from a local config. Profiles can add many tuning cvars without silently
    # dropping +map or +botbench off the end of the engine's command array.
    command = [str(server)]
    lines = []
    for key, value in settings.items():
        if key.lower() in BOOT_CVARS:
            command += ["+set", key, value]
        else:
            lines.append(f'set {key} "{value}"')
    lines += [f"map {map_name}", "vminfo", "cvarlist g_bot*",
              f"botbench start {count} {count} {seconds}"]
    base = home / "base"
    base.mkdir(exist_ok=True)
    config = base / "botbench-run.cfg"
    config.write_text("\n".join(lines) + "\n", encoding="utf-8")
    command += ["+exec", config.name]
    return command, config


def run_match(job):
    home = job["home"]
    home.mkdir(parents=True)
    count = job["bots_per_team"]
    settings = {
        "fs_basepath": str(job["basepath"]), "fs_homepath": str(home),
        "dedicated": "1", "net_enabled": "0", "net_ip": "127.0.0.1",
        "sv_master1": "", "sv_master2": "", "sv_maxclients": str(min(64, count * 2 + 8)),
        "sv_privateClients": "0", "sv_pure": "0", "sv_fastSim": str(int(job["speed"] == "fast")),
        "sv_simulationSeed": str(job["seed"]), "sv_fps": str(job["sv_fps"]),
        "vm_game": str(job["vm"]), "timescale": "1", "fixedtime": "0",
        "g_doWarmup": "0", "g_botDebug": "0", "logfile": "1",
        "g_logFile": "gameplay.log", "g_logFileSync": "0", "rconPassword": "",
    }
    settings.update(job["settings"])
    command, config = prepare_command(job["server"], home, settings, job["map"], count, job["seconds"])
    report = {
        "schema": SCHEMA, "profile": job["profile"], "map": job["map"],
        "seed": job["seed"], "repeat": job["repeat"], "bots_per_team": count,
        "requested_seconds": job["seconds"], "speed": job["speed"],
        "sv_fps": job["sv_fps"], "vm": job["vm"], "settings": settings,
        "home": str(home), "build": job["build"], "command": command,
        "startup_config": str(config),
        "wall_timeout_seconds": job["wall_timeout"], "status": "error",
        "winner": None, "reason": None,
    }
    reader = TelemetryReader(home / "base" / "botbench.jsonl", job["seed"])
    process = None
    started = time.monotonic()
    observed_start = None
    observed_finish = None

    def collect_events(events, observed_time):
        nonlocal observed_start, observed_finish
        for event in events:
            if event.get("map") != job["map"]:
                raise ValueError("telemetry map differs from the requested map")
            if event["event"] == "start":
                observed_start = observed_time
                for team in TEAMS:
                    if event[team].get("bots") != count:
                        raise ValueError(f"{team} has the wrong bot count in start telemetry")
                    histogram = event[team].get("skill_histogram")
                    if (not isinstance(histogram, list) or len(histogram) != 10
                            or any(type(value) is not int or value < 0 for value in histogram)
                            or sum(histogram) != count):
                        raise ValueError(f"{team} is missing its complete skill histogram")
                    if count >= 4 and (sum(value > 0 for value in histogram) < 4
                                       or sum(histogram[3:7]) <= count / 2):
                        raise ValueError(f"{team} does not have the expected varied bell-shaped skills")
            elif event["event"] == "finish":
                if event["elapsed_ms"] > job["seconds"] * 1000 + 1000 // job["sv_fps"]:
                    raise ValueError("terminal telemetry exceeds the requested game-time limit")
                observed_finish = observed_time
                report["terminal"] = event

    try:
        with (home / "stdout.log").open("wb") as output:
            options = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
            process = subprocess.Popen(command, cwd=job["basepath"], stdout=output,
                                       stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, **options)
            report["pid"] = process.pid
            while True:
                now = time.monotonic()
                collect_events(reader.poll(), now)
                if not report.get("bot_settings_verification", {}).get("verified"):
                    stdout = (home / "stdout.log").read_text(encoding="utf-8", errors="replace")
                    evidence = bot_settings_evidence(job["settings"], stdout)
                    report["bot_settings_verification"] = evidence
                    if evidence["invalid"]:
                        report["status"] = "invalid_settings"
                        report["error"] = evidence["error"]
                        break
                if observed_finish is not None:
                    try:
                        process.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        report["status"] = "shutdown_timeout"
                        report["error"] = "server emitted finish but did not exit within 5 seconds"
                        break
                    if process.returncode != 0:
                        report["status"] = "crash"
                        report["error"] = f"server exited {process.returncode} after emitting finish"
                        break
                    terminal = report["terminal"]
                    if terminal["winner"] == "error" or terminal["reason"] in ("error", "setup_error"):
                        report["status"] = "error"
                        report["error"] = f"server reported a benchmark error: {terminal['reason']}"
                    else:
                        report["status"] = "finished"
                        report["winner"] = terminal["winner"]
                        report["reason"] = terminal["reason"]
                    break
                if process.poll() is not None:
                    # The final record may have been written after this poll.
                    collect_events(reader.poll(), time.monotonic())
                    if observed_finish is not None:
                        terminal = report["terminal"]
                        report["status"] = "finished" if process.returncode == 0 else "crash"
                        if terminal["winner"] == "error" or terminal["reason"] in ("error", "setup_error"):
                            report["status"] = "error"
                            report["error"] = f"server reported a benchmark error: {terminal['reason']}"
                        if report["status"] == "finished":
                            report["winner"], report["reason"] = terminal["winner"], terminal["reason"]
                        elif report["status"] == "crash":
                            report["error"] = f"server exited {process.returncode} after emitting finish"
                        break
                    report["status"] = "crash"
                    report["error"] = f"server exited {process.returncode} without terminal telemetry"
                    break
                if now - started >= job["wall_timeout"]:
                    report["status"] = "wall_timeout"
                    report["error"] = "wall-time safety limit reached before match finished"
                    break
                if observed_start is None and now - started >= job["startup_timeout"]:
                    report["status"] = "startup_timeout"
                    report["error"] = "server did not produce start telemetry before startup safety limit"
                    break
                time.sleep(POLL_SECONDS)
    except Exception as error:
        report["error"] = f"{type(error).__name__}: {error}"
    finally:
        if process is not None:
            stop_process(process)
            report["exit_code"] = process.returncode
        wall_seconds = time.monotonic() - started
        report["wall_seconds_including_startup"] = wall_seconds
        report["observed_startup_seconds"] = (observed_start - started) if observed_start else None
        report["observer_resolution_seconds"] = POLL_SECONDS
        report["telemetry_records"] = len(reader.events)
        report["telemetry_sha256"] = telemetry_digest(reader.events)
        report["telemetry_diagnostics"] = telemetry_diagnostics(reader.events)
        if reader.events:
            report["last_sample"] = reader.events[-1]
        terminal = report.get("terminal")
        if terminal:
            report["simulated_seconds"] = terminal["elapsed_ms"] / 1000
            report["speedup_including_startup"] = report["simulated_seconds"] / wall_seconds
            engine_wall = terminal.get("wall_elapsed_ms")
            if type(engine_wall) is int and engine_wall > 0:
                report["simulation_wall_seconds"] = engine_wall / 1000
                report["simulation_speedup"] = terminal["elapsed_ms"] / engine_wall
            else:
                report["simulation_wall_seconds"] = None
                report["simulation_speedup"] = None
            expected_step = 1000 // job["sv_fps"]
            report["normal_tick_verified"] = (
                terminal.get("min_step_ms") == expected_step
                and terminal.get("max_step_ms") == expected_step)
            if report["status"] == "finished" and not report["normal_tick_verified"]:
                report["status"] = "invalid_tick"
                report["winner"] = None
                report["error"] = "simulation did not preserve the configured normal server tick"
        report["gameplay"] = gameplay_metrics(home / "base" / "gameplay.log")
        stdout_path = home / "stdout.log"
        stdout = stdout_path.read_text(encoding="utf-8", errors="replace") if stdout_path.exists() else ""
        evidence = bot_settings_evidence(job["settings"], stdout, require_all=True)
        report["bot_settings_verification"] = evidence
        if report["status"] == "finished" and not evidence["verified"]:
            report["status"] = "invalid_settings"
            report["winner"] = report["reason"] = None
            report["error"] = evidence["error"]
        report["requested_vm_mode"] = VM_MODES[job["vm"]]
        report["observed_vm_mode"] = observed_vm_mode(stdout)
        report["comparability"] = comparability_summary([report])
        report["warnings"] = diagnostic_warnings([report])
        write_json(home / "report.json", report)
    return report


def average(values):
    return statistics.fmean(values) if values else None


def numeric_final_metrics(runs):
    """Average terminal measurements; cumulative occupancy remains exact."""
    metrics = {}
    for group in (*TEAMS, "nav"):
        fields = sorted({field for run in runs for field in run["terminal"][group]
                         if type(run["terminal"][group][field]) in (int, float)})
        metrics[group] = {field: average([run["terminal"][group][field] for run in runs
                                        if type(run["terminal"][group].get(field)) in (int, float)
                                        and (field != "first_spawn_build_ms"
                                             or run["terminal"][group][field] >= 0)])
                          for field in fields}
        if group in TEAMS:
            for field, name in (("queued_player_seconds", "mean_queued_players"),
                                ("alive_player_seconds", "mean_alive_players")):
                metrics[group][name] = average([
                    run["terminal"][group][field] / run["simulated_seconds"]
                    for run in runs if run["simulated_seconds"] > 0
                    and field in run["terminal"][group]])
            for field in ("kills", "player_damage", "building_damage", "builds", "spawn_builds"):
                metrics[group][f"{field}_per_game_minute"] = average([
                    run["terminal"][group][field] * 60 / run["simulated_seconds"]
                    for run in runs if run["simulated_seconds"] > 0
                    and field in run["terminal"][group]])
            for field in ("kills", "player_damage", "building_damage"):
                metrics[group][f"{field}_per_alive_player_second"] = average([
                    run["terminal"][group][field] / run["terminal"][group]["alive_player_seconds"]
                    for run in runs if run["terminal"][group].get("alive_player_seconds", 0) > 0
                    and field in run["terminal"][group]])
            metrics[group]["spawn_build_observed_fraction"] = (
                sum(run["terminal"][group].get("spawn_builds", 0) > 0 for run in runs) / len(runs)
                if runs else None)
            for field in ("skill_histogram", "class_player_seconds", "weapon_player_seconds"):
                arrays = [run["terminal"][group][field] for run in runs
                          if isinstance(run["terminal"][group].get(field), list)]
                if arrays and len({len(array) for array in arrays}) == 1:
                    metrics[group][field] = [average([array[index] for array in arrays])
                                             for index in range(len(arrays[0]))]
    return metrics


def replay_summary(runs):
    groups = collections.defaultdict(list)
    for run in runs:
        groups[(run["profile"], run["map"], run["seed"])].append(run)
    summaries = []
    for (profile, map_name, seed), matches in sorted(groups.items()):
        if len(matches) < 2:
            continue
        finished = all(run["status"] == "finished" for run in matches)
        summaries.append({
            "profile": profile, "map": map_name, "seed": seed, "runs": len(matches),
            "all_finished": finished,
            "outcome_matches": finished and len({(run["winner"], run["reason"])
                                                   for run in matches}) == 1,
            "telemetry_matches": finished and len({run["telemetry_sha256"] for run in matches}) == 1,
        })
    return summaries


def aggregate_runs(runs, suite_wall_seconds):
    profiles = {}
    for profile in sorted({run["profile"] for run in runs}):
        all_runs = [run for run in runs if run["profile"] == profile]
        finished = [run for run in all_runs if run["status"] == "finished"]
        decisive = [run for run in finished if run["winner"] in TEAMS]
        counts = collections.Counter(run["winner"] for run in finished)
        profiles[profile] = {
            "runs": len(all_runs), "finished": len(finished),
            "statuses": dict(collections.Counter(run["status"] for run in all_runs)),
            "humans_wins": counts["humans"], "aliens_wins": counts["aliens"],
            "draws": counts["draw"],
            "time_limit_draws": sum(run["winner"] == "draw" and run["reason"] == "time_limit"
                                     for run in finished),
            "human_win_fraction_decisive": counts["humans"] / len(decisive) if decisive else None,
            "mean_simulated_seconds": average([run["simulated_seconds"] for run in finished]),
            "mean_simulation_speedup": average([run["simulation_speedup"] for run in finished
                                                if run.get("simulation_speedup") is not None]),
            "mean_speedup_including_startup": average([run["speedup_including_startup"]
                                                       for run in finished]),
            "mean_final_metrics": numeric_final_metrics(finished),
            "configurations": [json.loads(configuration) for configuration in sorted({
                json.dumps(run["terminal"]["configuration"], sort_keys=True)
                for run in finished if "configuration" in run["terminal"]})],
            "comparability": comparability_summary(all_runs),
            "warnings": diagnostic_warnings(all_runs),
        }
    simulated_seconds = sum(run.get("simulated_seconds", 0) for run in runs
                            if run["status"] == "finished")
    return {
        "schema": SCHEMA, "suite_wall_seconds": suite_wall_seconds,
        "total_simulated_seconds": simulated_seconds,
        "observed_simulated_seconds_all_runs": sum(run.get("simulated_seconds", 0) for run in runs),
        "parallel_throughput_speedup": simulated_seconds / suite_wall_seconds if suite_wall_seconds else None,
        "profiles": profiles, "replays": replay_summary(runs), "runs": runs,
        "interpretation": "Draws, crashes, invalid ticks and wall timeouts are separate. Completed runs validate simulation execution, not strategic competence. Small samples do not establish a win rate.",
    }


def comparison_key(run):
    return (run["map"], run["seed"], run.get("repeat", 0), run["bots_per_team"],
            run["requested_seconds"], run["sv_fps"], run["vm"])


def compare_aggregates(baseline, candidate):
    """Match exact scenarios; report unpaired/faulty runs without inventing wins."""
    def unique_runs(document):
        index = {}
        for run in document["runs"]:
            key = comparison_key(run)
            if key in index:
                raise ValueError("comparison input contains multiple profiles per scenario; select a profile first")
            index[key] = run
        return index

    before, after = unique_runs(baseline), unique_runs(candidate)
    pairs = []
    for key in sorted(before.keys() & after.keys()):
        old, new = before[key], after[key]
        valid = old["status"] == new["status"] == "finished"
        old_mode, new_mode = old.get("observed_vm_mode"), new.get("observed_vm_mode")
        if old_mode and new_mode and old_mode != new_mode:
            valid = False
        pair = {
            "map": key[0], "seed": key[1], "repeat": key[2], "valid": valid,
            "baseline_status": old["status"], "candidate_status": new["status"],
            "baseline_winner": old.get("winner"), "candidate_winner": new.get("winner"),
            "same_observed_vm_mode": bool(old_mode and new_mode and old_mode == new_mode),
            "baseline_observed_vm_mode": old_mode, "candidate_observed_vm_mode": new_mode,
        }
        if valid:
            differences = {}
            for group in (*TEAMS, "nav"):
                old_group, new_group = old["terminal"][group], new["terminal"][group]
                differences[group] = {
                    field: new_group[field] - old_group[field]
                    for field in sorted(old_group.keys() & new_group.keys())
                    if type(old_group[field]) in (int, float) and type(new_group[field]) in (int, float)
                    and field != "focus_target"
                    and (field != "first_spawn_build_ms" or min(old_group[field], new_group[field]) >= 0)}
            pair["candidate_minus_baseline"] = differences
        pairs.append(pair)
    valid_pairs = [pair for pair in pairs if pair["valid"]]
    deltas = {}
    for group in (*TEAMS, "nav"):
        fields = {field for pair in valid_pairs for field in pair["candidate_minus_baseline"][group]}
        deltas[group] = {field: average([pair["candidate_minus_baseline"][group][field]
                                        for pair in valid_pairs
                                        if field in pair["candidate_minus_baseline"][group]])
                         for field in sorted(fields)}
    return {
        "schema": SCHEMA, "paired_runs": len(pairs), "valid_paired_runs": len(valid_pairs),
        "unpaired_baseline": len(before.keys() - after.keys()),
        "unpaired_candidate": len(after.keys() - before.keys()),
        "outcome_transitions": dict(collections.Counter(
            f"{pair['baseline_winner']}->{pair['candidate_winner']}" for pair in valid_pairs)),
        "mean_candidate_minus_baseline": deltas, "pairs": pairs,
        "comparability": {"baseline": comparability_summary(baseline["runs"]),
                          "candidate": comparability_summary(candidate["runs"])},
    }


def parse_arguments():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--server", type=pathlib.Path)
    parser.add_argument("--basepath", type=pathlib.Path)
    parser.add_argument("--vm", type=int, choices=(0, 1, 2), default=0)
    parser.add_argument("--map", action="append", dest="maps", help="repeat to benchmark several maps (default: arachnid2)")
    parser.add_argument("--seeds", default="17,29,41")
    parser.add_argument("--seconds", type=int, default=900, help="normal game seconds, not wall seconds")
    parser.add_argument("--bots-per-team", type=int, default=16)
    parser.add_argument("--jobs", type=int, default=min(3, os.cpu_count() or 1))
    parser.add_argument("--repeats", type=int, default=1, help="repeat identical scenarios to verify deterministic replay")
    parser.add_argument("--speed", choices=("fast", "realtime"), default="fast")
    parser.add_argument("--sv-fps", type=int, choices=(10, 20, 25, 40, 50, 100), default=20)
    parser.add_argument("--wall-timeout", type=float, help="per-process safety limit (default: 180 fast, game seconds + 60 realtime)")
    parser.add_argument("--startup-timeout", type=float, default=20, help="wall seconds allowed for loading and match setup")
    parser.add_argument("--set", action="append", default=[], metavar="KEY=VALUE", help="recorded cvar override; no overrides by default")
    parser.add_argument("--baseline-set", action="append", default=[], metavar="KEY=VALUE", help="run baseline and candidate profiles paired by map/seed")
    parser.add_argument("--candidate-set", action="append", default=[], metavar="KEY=VALUE")
    parser.add_argument("--output", type=pathlib.Path, default=pathlib.Path("build-bot-benchmarks"))
    parser.add_argument("--compare", nargs=2, type=pathlib.Path, metavar=("BASELINE", "CANDIDATE"), help="compare two saved aggregates without launching servers")
    args = parser.parse_args()
    if args.compare:
        return parser, args
    if args.server is None or args.basepath is None:
        parser.error("--server and --basepath are required when launching matches")
    args.server, args.basepath = args.server.resolve(), args.basepath.resolve()
    if not args.server.is_file() or not args.basepath.is_dir():
        parser.error("server executable or basepath does not exist")
    if not 1 <= args.bots_per_team <= 31 or args.seconds < 5 or args.seconds > 86400:
        parser.error("--bots-per-team must be 1..31 and --seconds must be 5..86400")
    if not 1 <= args.jobs <= min(32, os.cpu_count() or 1):
        parser.error("--jobs must be between 1 and the CPU count (maximum 32)")
    if not 1 <= args.repeats <= 100:
        parser.error("--repeats must be 1..100")
    if args.wall_timeout is None:
        args.wall_timeout = 180 if args.speed == "fast" else args.seconds + 60
    if not math.isfinite(args.wall_timeout) or args.wall_timeout <= 0:
        parser.error("--wall-timeout must be positive and finite")
    if not math.isfinite(args.startup_timeout) or args.startup_timeout <= 0:
        parser.error("--startup-timeout must be positive and finite")
    args.maps = args.maps or ["arachnid2"]
    if len(set(args.maps)) != len(args.maps) or any(not re.fullmatch(r"[A-Za-z0-9_-]+", name) for name in args.maps):
        parser.error("map names must be distinct letters, numbers, underscores or hyphens")
    try:
        args.seeds = parse_seeds(args.seeds)
        args.settings = parse_settings(args.set)
        args.baseline_settings = parse_settings(args.baseline_set)
        args.candidate_settings = parse_settings(args.candidate_set)
    except ValueError as error:
        parser.error(str(error))
    return parser, args


def main():
    parser, args = parse_arguments()
    if args.compare:
        try:
            baseline, candidate = [load_aggregate(path) for path in args.compare]
        except (ValueError, OSError) as error:
            parser.error(str(error))
        result = compare_aggregates(baseline, candidate)
        args.output.mkdir(parents=True, exist_ok=True)
        write_json(args.output / "comparison.json", result)
        print(json.dumps(result, indent=2))
        return 0
    suite = args.output.resolve() / f"suite-{uuid.uuid4().hex[:10]}"
    suite.mkdir(parents=True)
    identity = build_identity(args.server, args.basepath, args.vm)
    profiles = {"candidate": args.settings}
    if args.baseline_set or args.candidate_set:
        profiles = {
            "baseline": {**args.settings, **args.baseline_settings},
            "candidate": {**args.settings, **args.candidate_settings},
        }
    jobs = []
    # Interleave profiles/repeats so A/B measurements share similar load conditions.
    for map_name in args.maps:
        for seed in args.seeds:
            for repeat in range(args.repeats):
                for profile, settings in profiles.items():
                    jobs.append({
                        "server": args.server, "basepath": args.basepath, "build": identity,
                        "map": map_name, "seed": seed, "repeat": repeat, "profile": profile,
                        "settings": settings, "bots_per_team": args.bots_per_team,
                        "seconds": args.seconds, "speed": args.speed, "vm": args.vm,
                        "sv_fps": args.sv_fps, "wall_timeout": args.wall_timeout,
                        "startup_timeout": args.startup_timeout,
                        "home": suite / f"{profile}-{map_name}-s{seed}-r{repeat}",
                    })
    print(f"Running {len(jobs)} offline matches, {args.jobs} at a time. Reports: {suite}", flush=True)
    started = time.monotonic()
    reports = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
        pending = {pool.submit(run_match, job): job for job in jobs}
        for future in concurrent.futures.as_completed(pending):
            report = future.result()
            reports.append(report)
            speedup = report.get("simulation_speedup")
            speed_text = f"{speedup:.1f}x" if speedup is not None else "n/a"
            print(f"{report['profile']} {report['map']} seed {report['seed']} r{report['repeat']}: "
                  f"{report['status']} {report.get('winner') or ''} "
                  f"{report.get('simulated_seconds', 0):.1f}s game, "
                  f"{report['wall_seconds_including_startup']:.2f}s wall, simulation {speed_text}", flush=True)
            if report.get("error"):
                print(f"  {report['error']}; logs: {report['home']}", flush=True)
    reports.sort(key=lambda run: (run["map"], run["seed"], run["repeat"], run["profile"]))
    aggregate = aggregate_runs(reports, time.monotonic() - started)
    aggregate["jobs"] = args.jobs
    aggregate["output"] = str(suite)
    write_json(suite / "aggregate.json", aggregate)
    if len(profiles) == 2:
        documents = [{"runs": [run for run in reports if run["profile"] == profile]}
                     for profile in ("baseline", "candidate")]
        write_json(suite / "comparison.json", compare_aggregates(*documents))
    print(f"Aggregate: {suite / 'aggregate.json'}", flush=True)
    for profile, summary in aggregate["profiles"].items():
        for warning in summary["warnings"]:
            print(f"WARNING [{profile}/{warning['code']}]: {warning['message']}", flush=True)
    if any(run["status"] != "finished" for run in reports):
        return 1
    if any(not replay["telemetry_matches"] for replay in aggregate["replays"]):
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
