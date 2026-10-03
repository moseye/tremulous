#!/usr/bin/env python3
"""Actual offline-engine bot behavior scenarios; fixtures are not balance matches.

Generate isolated fixtures by default. Add --run only for a frozen build.
No third-party dependencies, RCON, master server, or network listeners.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import time
import uuid
import zipfile

import bot_benchmark


MAP = "bot_behavior_fixtures"
PROFILE = {
    "g_botCombatTuning": "1", "g_botTeamwork": "1", "g_botSpawnScale": "1",
    "g_botNavTuning": "1", "g_botNavNodes": "8192", "g_botHumanAimCone": "6",
    "g_botHumanReaction": "600", "g_botHumanTurnSpeed": "160",
    "g_botHumanFireDelay": "300", "g_botTaunt": "1", "g_botProbe": "1",
    "g_botBuild": "0", "g_doWarmup": "0",
}


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def generate_map(path):
    """Minimal valid IBSP46 collision map, with axial ordering from cm_load.c."""
    planes = [(1, 0, 0, 0), (-1, 0, 0, 0)]
    sides, brushes = [], []
    def box(lo, hi):
        first = len(sides)
        for axis in range(3):
            for sign, distance in ((-1, -lo[axis]), (1, hi[axis])):
                normal = [0, 0, 0]
                normal[axis] = sign
                index = len(planes)
                planes.extend([(*normal, distance), (*[-v for v in normal], -distance)])
                sides.append((index, 0))
        brushes.append((first, 6, 0))
    box([-4096, -4096, -64], [4096, 4096, 0])
    box([-320, 2388, 0], [320, 2404, 160])
    # The 16-unit gap beside this obstacle is narrower than a Dretch hull;
    # the wall lane above its top remains clear. Other floor routes still exist.
    box([0, 2420, 0], [48, 2596, 48])
    entities = ('{\n"classname" "worldspawn"\n"message" "Bot behavior fixtures"\n}\n'
                '{\n"classname" "info_player_intermission"\n"origin" "-512 0 128"\n}\n'
                '{\n"classname" "team_alien_spawn"\n"origin" "-3000 3000 16"\n}\n'
                '{\n"classname" "team_human_spawn"\n"origin" "3000 -3000 16"\n}\n\0')
    lumps = [b""] * 17
    lumps[0] = entities.encode("ascii")
    lumps[1] = struct.pack("<64sii", b"textures/common/caulk", 0, 1)
    lumps[2] = b"".join(struct.pack("<4f", *p) for p in planes)
    lumps[3] = struct.pack("<9i", 0, -1, -1, -8192, -8192, -8192, 8192, 8192, 8192)
    lumps[4] = struct.pack("<12i", 0, 0, -8192, -8192, -8192, 8192, 8192, 8192,
                           0, 0, 0, len(brushes))
    lumps[6] = b"".join(struct.pack("<i", i) for i in range(len(brushes)))
    lumps[7] = struct.pack("<6f4i", -8192, -8192, -8192, 8192, 8192, 8192,
                           0, 0, 0, len(brushes))
    lumps[8] = b"".join(struct.pack("<3i", *b) for b in brushes)
    lumps[9] = b"".join(struct.pack("<2i", *s) for s in sides)
    offset, header, body = 144, [], bytearray()
    for lump in lumps:
        padding = (-offset) % 4
        body.extend(b"\0" * padding)
        offset += padding
        header.extend((offset, len(lump)))
        body.extend(lump)
        offset += len(lump)
    bsp = struct.pack("<4si34i", b"IBSP", 46, *header) + body
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("maps/" + MAP + ".bsp", bsp)
    return {"pk3_sha256": sha(path), "bsp_bytes": len(bsp), "world_brushes": len(brushes)}


def start(humans=6, aliens=6):
    lines = ["set " + k + " " + v for k, v in PROFILE.items()]
    lines += ["devmap " + MAP, "wait 4", "vminfo", "cvarlist g_bot*", "g_botProbe", "sv_cheats",
              f"bot fill humans {humans} bell", f"bot fill aliens {aliens} bell",
              "bot role all attack"]
    for ident in range(humans + aliens):
        cls = 11 if ident < humans else 3
        z = 24.125 if cls == 11 else 15.125
        # Staged away from other actors and the wall fixture until placed.
        lines += [f"botprobe spawn {ident} {cls} {-2400 + ident * 80} -2400 {z} 0",
                  f"botprobe pause {ident} 1", f"botprobe god {ident} 1"]
    lines += ["wait 4"]
    return lines


def sample(lines, name, ticks=20):
    lines += [f"wait {ticks}", "botprobe snapshot " + name]


def place(lines, ident, x, y, human=True, yaw=0):
    z = 24.125 if human else 15.125
    lines.append(f"botprobe place {ident} {x} {y} {z} {yaw}")


def group_scenario():
    lines = start(4, 4)
    for base, y, human in ((0, -1024, True), (4, 1024, False)):
        for n, (x, dy) in enumerate(((-1400, 0), (-1350, 60), (1400, 0), (1450, 60))):
            place(lines, base+n, x, y+dy, human)
    sample(lines, "separate")
    for base, y, human in ((0, -1024, True), (4, 1024, False)):
        place(lines, base+2, -1250, y, human)
        place(lines, base+3, -1200, y+60, human)
    sample(lines, "merged")
    lines += ["botprobe kill 1 6", "botprobe kill 5 2"]
    sample(lines, "member_died", ticks=10)
    lines += ["botprobe spawn 1 11 -1350 -964 24.125 0",
              "botprobe spawn 5 3 -1350 1084 15.125 0",
              "botprobe god 1 1", "botprobe god 5 1"]
    sample(lines, "reinforced")
    for base, y, human in ((0, -1024, True), (4, 1024, False)):
        place(lines, base+2, 1400, y, human)
        place(lines, base+3, 1450, y+60, human)
    sample(lines, "split")
    return lines + ["quit"]


def retreat_scenario(team, role="attack", paired=False):
    lines = start()
    actor, friends, enemies, y = ((0, range(1, 6), range(6, 11), -1024) if team == "humans"
                                 else (6, range(7, 12), range(0, 5), 1024))
    lines += [f"bot role {actor} {role}"]
    place(lines, actor, 0, y, actor < 6)
    for n, ident in enumerate(enemies):
        if paired:
            x, dy = ((220, -220), (280, -110), (320, 0), (280, 110), (220, 220))[n]
        else:
            x, dy = 220+n*48, (n % 2)*64
        place(lines, ident, x, y+dy, ident < 6, 180)
    for n, ident in enumerate(friends):
        place(lines, ident, -1800+n*48, y+800, ident < 6)
    participants = [actor]
    if paired:
        partner = next(iter(friends))
        place(lines, partner, -64, y-96, partner < 6)
        lines += [f"bot role {partner} {role}"]
        participants.append(partner)
    # Let the ordinary perception pass observe the staged threat while the
    # actor remains paused; the first movement then has a known disadvantage.
    sample(lines, "disadvantaged_start", 12)
    lines += [f"botprobe pause {ident} 0" for ident in participants]
    # Keep the original visible threat in range at reinforcement; a longer
    # successful withdrawal can legitimately leave the 850-unit sight range.
    for i in range(4):
        sample(lines, "disadvantaged_" + str(i), 5)
    for n, ident in enumerate(friends):
        if ident in participants:
            continue
        lines.append(f"botprobe near {ident} {actor} {-48-n*48} -96 0 0")
    lines.append("botprobe snapshot reinforcement_arrived")
    for i in range(16):
        sample(lines, "reinforced_" + str(i), 5)
    return lines + ["quit"]


def swarm_scenario():
    lines = start()
    place(lines, 0, 300, 0)
    for ident in range(1, 6):
        place(lines, ident, 2200+ident*48, -1700)
    place(lines, 6, 0, 0, False)
    place(lines, 7, -700, 0, False)
    for ident in range(8, 12):
        place(lines, ident, 2200+(ident-8)*48, 1700, False)
    sample(lines, "swarm_start", 12)
    lines += ["botprobe pause 6 0"]
    for i in range(4):
        sample(lines, "join_" + str(i), 5)
    lines += ["botprobe snapshot ally_released", "botprobe pause 7 0"]
    for i in range(48):
        sample(lines, "commit_" + str(i), 5)
    return lines + ["quit"]


def taunt_scenario():
    lines = start()
    # A direct gesture asks Pmove to accept it; it does not count as an AI taunt.
    sample(lines, "gesture_before", 2)
    lines += ["botprobe gesture 0 100"]
    sample(lines, "gesture_accepted", 2)
    lines += ["botprobe gesture 0 500"]
    sample(lines, "gesture_timer_blocks_repeat", 2)
    sample(lines, "gesture_finished", 60)
    # Activate a healthy actor with allies, kill a staged enemy via ordinary
    # server damage, then leave enemies far away so safe celebration can occur.
    place(lines, 1, -1000, -1024)
    place(lines, 0, -1100, -960)
    place(lines, 2, 600, -1024)
    place(lines, 3, 520, -960)
    for ident in range(6, 12):
        place(lines, ident, 2400+(ident-6)*48, 1500, False)
    lines += ["botprobe pause 1 0", "botprobe pause 2 0"]
    sample(lines, "ai_initial_eligibility", 4)
    lines += ["botprobe pause 2 1"]
    # Keep a live threat present while the initially observed cooldown passes,
    # so an incidental idle celebration cannot reset it before the kill.
    place(lines, 6, -740, -1024, False, 180)
    sample(lines, "ai_initial_deadline_passed_in_combat", 420)
    lines += ["botprobe health 6 1", "botprobe god 6 0", "botprobe snapshot safe_kill"]
    for i in range(24):
        sample(lines, "safe_kill_" + str(i), 5)
    lines += ["botprobe kill 7 1"]
    for i in range(8):
        sample(lines, "cooldown_" + str(i), 5)
    # A different actor has no previous accepted taunt. Nearby active threats
    # must suppress its celebration, even after a valid enemy kill.
    place(lines, 2, 0, 0)
    for ident, x in ((8, 180), (9, 228), (10, 276)):
        place(lines, ident, x, 0, False, 180)
    lines += ["botprobe kill 11 2", "botprobe pause 2 0", "botprobe snapshot unsafe_kill"]
    for i in range(12):
        sample(lines, "unsafe_kill_" + str(i), 5)
    return lines + ["quit"]


def wall_scenario():
    lines = start()
    place(lines, 0, 220, 2500)
    for ident, x, y in ((6, -96, 2436), (7, -160, 2460), (8, -224, 2436), (9, -288, 2460)):
        place(lines, ident, x, y, False)
    sample(lines, "wall_start", 12)
    lines += ["botprobe pause 6 0"]
    for i in range(48):
        sample(lines, "wall_" + str(i), 5)
    return lines + ["quit"]


def disabled_taunts_scenario():
    lines = start(4, 4) + ["set g_botTaunt 0"]
    place(lines, 0, -600, -1200)
    place(lines, 1, -650, -1140)
    place(lines, 4, -600, 1200, False)
    place(lines, 5, -650, 1260, False)
    lines += ["botprobe pause 0 0", "botprobe pause 4 0"]
    for i in range(10):
        sample(lines, "disabled_" + str(i), 100)
    return lines + ["quit"]


SCENARIOS = {"dynamic_groups": group_scenario, "retreat_humans": lambda: retreat_scenario("humans"),
             "retreat_aliens": lambda: retreat_scenario("aliens"), "accepted_taunts": taunt_scenario,
             "defender_humans": lambda: retreat_scenario("humans", "defend"),
             "defender_aliens": lambda: retreat_scenario("aliens", "defend"),
             "group_retreat_humans": lambda: retreat_scenario("humans", paired=True),
             "group_retreat_aliens": lambda: retreat_scenario("aliens", paired=True),
             "group_defender_humans": lambda: retreat_scenario("humans", "defend", paired=True),
             "group_defender_aliens": lambda: retreat_scenario("aliens", "defend", paired=True),
             "organic_swarm": swarm_scenario,
             "disabled_taunts": disabled_taunts_scenario, "autonomous_wall": wall_scenario}


def bot(snapshot, ident):
    return next(b for b in snapshot["bots"] if b["id"] == ident)


def group_id(b):
    return b["tactics"]["group_id"]


def retreating(b):
    return bool(b["tactics"]["retreating"])


def assertions(name, snapshots):
    by_phase = {s["phase"]: s for s in snapshots}
    checks = []
    def check(label, passed, evidence):
        checks.append({"name": label, "passed": bool(passed), "evidence": evidence})
    if name == "dynamic_groups":
        for base, team in ((0, "humans"), (4, "aliens")):
            ids = [base+i for i in range(4)]
            for phase in ("separate", "merged", "split"):
                members = [bot(by_phase[phase], i) for i in ids]
                groups = [group_id(b) for b in members]
                if phase == "merged":
                    ok = len(set(groups)) == 1 and all(g > 0 for g in groups)
                else:
                    ok = groups[0] == groups[1] and groups[2] == groups[3] and groups[0] != groups[2] and min(groups) > 0
                check(team+"_"+phase, ok, {"ids": ids, "group_ids": groups,
                      "positions": [b["position"] for b in members]})
            dead = bot(by_phase["member_died"], base+1)
            living = [bot(by_phase["member_died"], i) for i in (base, base+2, base+3)]
            member_ids = living[0]["tactics"]["member_ids"]
            check(team+"_dead_member_removed", dead["health"] <= 0 and base+1 not in member_ids,
                  {"dead_health": dead["health"], "living_member_ids": member_ids})
            reinforced = [bot(by_phase["reinforced"], i) for i in ids]
            check(team+"_reinforcement_joins", len({group_id(b) for b in reinforced}) == 1 and
                  all(group_id(b) > 0 for b in reinforced) and
                  bot(by_phase["reinforced"], base+1)["fixture_spawn_generation"] > dead["fixture_spawn_generation"],
                  {"group_ids": [group_id(b) for b in reinforced]})
    elif name.startswith(("retreat_", "defender_", "group_retreat_", "group_defender_")):
        actor = 0 if name.endswith("humans") else 6
        initial = bot(by_phase["disadvantaged_start"], actor)
        disadvantaged = [bot(s, actor) for s in snapshots if s["phase"].startswith("disadvantaged_") and s["phase"] != "disadvantaged_start"]
        reinforced = [bot(s, actor) for s in snapshots if s["phase"].startswith("reinforced_")]
        enemies = [bot(by_phase["disadvantaged_start"], i) for i in (range(6, 11) if actor == 0 else range(0, 5))]
        threat = [sum(b["position"][k] for b in enemies)/len(enemies) for k in range(3)]
        initial_distance = math.dist(initial["position"], threat)
        distances = [math.dist(b["position"], threat) for b in disadvantaged]
        check("disadvantage_causes_physical_retreat", any(retreating(b) for b in disadvantaged) and
              max(distances) > initial_distance+64,
              {"initial_distance": initial_distance, "observed_distances": distances,
               "retreating": [retreating(b) for b in disadvantaged]})
        before = bot(by_phase["reinforcement_arrived"], actor)
        after_distances = [math.dist(b["position"], threat) for b in reinforced]
        check("reinforcements_resume_physical_attack", any(not retreating(b) for b in reinforced) and
              min(after_distances) < math.dist(before["position"], threat)-32,
              {"before_distance": math.dist(before["position"], threat), "observed_distances": after_distances,
               "orders": [b["tactics"] for b in reinforced]})
        generations = {(b["fixture_spawn_generation"], b["fixture_placement_generation"]) for b in
                       [initial, *disadvantaged, before, *reinforced]}
        check("actor_moves_without_fixture_reposition", len(generations) == 1, sorted(generations))
        if "defender_" in name:
            check("actor_retains_defender_role", all(b["role"] == 1 for b in
                  [initial, *disadvantaged, before, *reinforced]),
                  [b["role"] for b in [initial, *disadvantaged, before, *reinforced]])
        if name.startswith("group_"):
            partner = actor+1
            pair_start = [initial, bot(by_phase["disadvantaged_start"], partner)]
            check("actual_multi_member_group_is_outnumbered", group_id(pair_start[0]) == group_id(pair_start[1]) and
                  all(b["tactics"]["group_size"] >= 2 and b["tactics"]["nearby_enemies"] > b["tactics"]["nearby_allies"]
                      for b in pair_start), [b["tactics"] for b in pair_start])
            observations = [bot(s, partner) for s in snapshots if s["phase"].startswith("disadvantaged_") and
                            s["phase"] != "disadvantaged_start"]
            partner_distance = math.dist(pair_start[1]["position"], threat)
            check("second_group_member_physically_withdraws", any(retreating(b) for b in observations) and
                  max(math.dist(b["position"], threat) for b in observations) > partner_distance+64,
                  {"initial_distance": partner_distance, "positions": [b["position"] for b in observations]})
            partner_before = bot(by_phase["reinforcement_arrived"], partner)
            partner_after = [bot(s, partner) for s in snapshots if s["phase"].startswith("reinforced_")]
            check("second_member_resumes_physical_attack", any(not retreating(b) for b in partner_after) and
                  min(math.dist(b["position"], threat) for b in partner_after) <
                  math.dist(partner_before["position"], threat)-32,
                  {"before_distance": math.dist(partner_before["position"], threat),
                   "positions": [b["position"] for b in partner_after]})
            all_partner = [bot(s, partner) for s in snapshots]
            check("second_group_member_is_same_life_without_reposition", len({
                  (b["fixture_spawn_generation"], b["fixture_placement_generation"]) for b in all_partner}) == 1,
                  [b["position"] for b in all_partner])
            if "defender_" in name:
                check("second_member_retains_defender_role", all(b["role"] == 1 for b in all_partner),
                      [b["role"] for b in all_partner])
    elif name == "organic_swarm":
        initial = by_phase["swarm_start"]
        actor, ally = bot(initial, 6), bot(initial, 7)
        joins = [s for s in snapshots if s["phase"].startswith("join_")]
        check("isolated_alien_initially_lacks_local_support", actor["tactics"]["group_size"] == 1 and
              actor["tactics"]["nearby_allies"] == 1 and math.dist(actor["position"], ally["position"]) >= 650,
              {"actor": actor["tactics"], "initial_ally_distance": math.dist(actor["position"], ally["position"])})
        approaching = [bot(s, 6) for s in joins]
        check("solitary_alien_physically_converges_on_actual_ally", any(
              math.dist(b["position"], ally["position"]) < math.dist(actor["position"], ally["position"])-64
              for b in approaching), [b["position"] for b in approaching])
        commits = [s for s in snapshots if s["phase"].startswith("commit_")]
        merged = next((s for s in commits if group_id(bot(s, 6)) == group_id(bot(s, 7)) and
                       min(bot(s, i)["tactics"]["group_size"] for i in (6, 7)) >= 2 and
                       math.dist(bot(s, 6)["position"], bot(s, 7)["position"]) <= 450), None)
        pair = [bot(merged or by_phase["ally_released"], i) for i in (6, 7)]
        check("physical_neighbors_form_observed_group", merged is not None,
              {"phase": merged["phase"] if merged else None, "positions": [b["position"] for b in pair],
               "tactics": [b["tactics"] for b in pair]})
        if merged:
            commits = [s for s in commits if s["time_ms"] >= merged["time_ms"]]
        target_position = bot(merged or initial, 0)["position"]
        distances = [[math.dist(bot(s, i)["position"], target_position) for i in (6, 7)] for s in commits]
        check("merged_aliens_both_autonomously_approach_enemy", all(
              min(row[k] for row in distances) < math.dist(pair[k]["position"], target_position)-64
              for k in range(2)), {"positions_at_merge": [b["position"] for b in pair], "enemy_distances": distances})
        near_target = [s["phase"] for s, row in zip(commits, distances) if max(row) <= 128 and
                       all(bot(s, i)["target"] == 0 for i in (6, 7))]
        check("both_enter_near_melee_target_together", bool(near_target),
              {"qualified_phases": near_target, "distance_limit": 128,
               "note": "Near a stationary god-mode enemy with both actual combat targets; no damage or kill claim."})
        for ident in (6, 7):
            observations = [bot(s, ident) for s in snapshots]
            generations = {(b["fixture_spawn_generation"], b["fixture_placement_generation"]) for b in observations}
            check("alien_"+str(ident)+"_same_life_without_reposition", len(generations) == 1 and
                  all(not bot(s, ident)["paused"] for s in commits), sorted(generations))
    elif name == "accepted_taunts":
        before, accepted, repeated = [bot(by_phase[p], 0) for p in
                                     ("gesture_before", "gesture_accepted", "gesture_timer_blocks_repeat")]
        check("pmove_accepts_actual_taunt_event", accepted["taunts_accepted"] == before["taunts_accepted"]+1 and
              accepted["taunt_timer_ms"] > 0 and by_phase["gesture_accepted"]["event_taunt"] in accepted["event_ring"], accepted)
        check("pmove_timer_rejects_repeat_request", repeated["taunts_accepted"] == accepted["taunts_accepted"], repeated)
        finished = bot(by_phase["gesture_finished"], 0)
        check("gesture_timer_expires_normally", finished["taunt_timer_ms"] == 0 and
              finished["taunts_accepted"] == accepted["taunts_accepted"], finished)
        safe_start = bot(by_phase["safe_kill"], 1)
        safe = [bot(s, 1) for s in snapshots if s["phase"].startswith("safe_kill_")]
        victim_health = [bot(s, 6)["health"] for s in snapshots if s["phase"].startswith("safe_kill_")]
        check("safe_enemy_kill_triggers_ai_taunt", max(b["taunts_accepted"] for b in safe) > safe_start["taunts_accepted"],
              {"before": safe_start["taunts_accepted"], "counts": [b["taunts_accepted"] for b in safe],
               "victim_observed_health": victim_health})
        check("enemy_dies_from_autonomous_combat", any(h <= 0 for h in victim_health), victim_health)
        cooldown = [bot(s, 1) for s in snapshots if s["phase"].startswith("cooldown_")]
        check("ai_cooldown_survives_second_kill", max(b["taunts_accepted"] for b in cooldown) <= max(b["taunts_accepted"] for b in safe),
              [b["taunts_accepted"] for b in cooldown])
        unsafe_start = bot(by_phase["unsafe_kill"], 2)
        check("unsafe_actor_cooldown_is_expired", 0 < unsafe_start["next_taunt_time_ms"] <= by_phase["unsafe_kill"]["time_ms"] and
              unsafe_start["taunts_accepted"] == 0,
              {"next_taunt_time_ms": unsafe_start["next_taunt_time_ms"],
               "time_ms": by_phase["unsafe_kill"]["time_ms"], "accepted": unsafe_start["taunts_accepted"]})
        unsafe = [bot(s, 2) for s in snapshots if s["phase"].startswith("unsafe_kill_")]
        check("visible_combat_suppresses_ai_taunt", all(b["taunts_accepted"] == unsafe_start["taunts_accepted"] for b in unsafe),
              {"counts": [b["taunts_accepted"] for b in unsafe], "tactics": [b["tactics"] for b in unsafe]})
    elif name == "disabled_taunts":
        check("disabled_cvar_suppresses_accepted_events", all(s["taunts_enabled"] == 0 and
              all(b["taunts_accepted"] == 0 for b in s["bots"]) for s in snapshots),
              {"duration_ms": snapshots[-1]["time_ms"]-snapshots[0]["time_ms"],
               "flags": [s["taunts_enabled"] for s in snapshots]})
    elif name == "autonomous_wall":
        actors = [bot(s, 6) for s in snapshots]
        plans = {}
        for snapshot, b in zip(snapshots, actors):
            nav = b["navigation"]
            if abs(b["grapple_point"][2]) < .7 and b["ground_entity"] == 1022 and nav["wall_plan_age_ms"] >= 0:
                start_time = snapshot["time_ms"]-nav["wall_plan_age_ms"]
                plans.setdefault(start_time, []).append((snapshot["time_ms"], b))
        progress = [{"plan_start_ms": key, "first_ms": observations[0][0],
                     "last_ms": observations[-1][0], "samples": len(observations),
                     "max_displacement": max((math.dist(a[1]["position"], b[1]["position"])
                                              for a in observations for b in observations), default=0),
                     "completed_before": observations[0][1]["navigation"]["wall_completed"],
                     "approaches_before": observations[0][1]["navigation"]["wall_approaches"],
                     "aborted_before": observations[0][1]["navigation"]["wall_aborted"],
                     "positions": [b["position"] for _, b in observations]}
                    for key, observations in plans.items()]
        qualified = [p for p in progress if p["samples"] >= 2 and p["max_displacement"] >= 96]
        check("ai_wall_movement_has_physical_progress", bool(qualified), progress)
        landings = []
        for plan in qualified:
            for snapshot, b in zip(snapshots, actors):
                foot = b["position"][2]+b["mins"][2]
                if snapshot["time_ms"] > plan["last_ms"] and b["grapple_point"][2] >= .7 and b["ground_entity"] == 1022 and \
                        min(abs(foot-.125), abs(foot-48.125)) <= 2 and \
                        b["navigation"]["wall_approaches"] == plan["approaches_before"] and \
                        b["navigation"]["wall_aborted"] == plan["aborted_before"] and \
                        b["navigation"]["wall_completed"] > plan["completed_before"]:
                    landings.append({"plan_start_ms": plan["plan_start_ms"], "time_ms": snapshot["time_ms"],
                                     "position": b["position"], "foot_height": foot,
                                     "wall_completed": b["navigation"]["wall_completed"]})
                    break
        check("actual_wall_progress_returns_to_known_supported_floor", bool(landings), landings)
        generations = {(b["fixture_spawn_generation"], b["fixture_placement_generation"]) for b in actors}
        released = [bot(s, 6) for s in snapshots if s["phase"] != "wall_start"]
        check("wall_progress_is_autonomous_same_life", len(generations) == 1 and
              len({b["class"] for b in actors}) == 1 and all(not b["paused"] for b in released), sorted(generations))
        check("alien_attacks_with_local_group", any(b["tactics"]["group_size"] >= 3 and not retreating(b) for b in actors),
              [b["tactics"] for b in actors])
    return checks


def run_case(server, basepath, home, name, vm, fixture, timeout):
    (home / "base").mkdir(parents=True)
    with (home / "base" / fixture.name).open("wb") as output:
        output.write(fixture.read_bytes())
    lines = SCENARIOS[name]()
    (home / "base" / "scenario.cfg").write_text("\n".join(lines) + "\n")
    command = [str(server), "+set", "fs_basepath", str(basepath), "+set", "fs_homepath", str(home),
               "+set", "dedicated", "1", "+set", "net_enabled", "0", "+set", "sv_fastSim", "1",
               "+set", "sv_simulationSeed", "17", "+set", "sv_fps", "20", "+set", "sv_maxclients", "64",
               "+set", "vm_game", str(vm), "+exec", "scenario.cfg"]
    started = time.monotonic()
    timed_out = False
    with (home / "stdout.log").open("wb") as log:
        options = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
        process = subprocess.Popen(command, cwd=basepath, stdin=subprocess.DEVNULL, stdout=log,
                                   stderr=subprocess.STDOUT, **options)
        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            timed_out = True
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=5)
    text = (home / "stdout.log").read_text(errors="replace")
    modes = re.findall(r"^game\s*:\s*(native|compiled on load|interpreted)\s*$", text, re.M)
    expected_mode = {0: "native", 1: "interpreted", 2: "compiled on load"}[vm]
    path = home / "base" / "botprobe.jsonl"
    records = [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []
    snapshots = [r for r in records if r["kind"] == "snapshot"]
    errors = []
    if timed_out or process.returncode != 0:
        errors.append("process_timeout_or_failure")
    if modes != [expected_mode]:
        errors.append("actual_module_mode_not_verified")
    settings_evidence = bot_benchmark.bot_settings_evidence(PROFILE, text, require_all=True)
    if not settings_evidence["verified"]:
        errors.append("registered_fixture_profile_not_verified")
    if "rejected invalid or occupied" in text:
        errors.append("fixture_placement_rejected")
    if not snapshots:
        errors.append("missing_actual_snapshots")
    if any(s["fixture_mode"] != 1 or s["sv_cheats"] != 1 or s["net_enabled"] != 0 or s["sv_fps"] != 20 for s in snapshots):
        errors.append("fixture_guards_not_verified")
    if any(b["time_ms"]-a["time_ms"] != 50*(b["frame"]-a["frame"])
           for a, b in zip(snapshots, snapshots[1:])):
        errors.append("non_normal_50ms_ticks")
    checks = []
    if not errors:
        try:
            checks = assertions(name, snapshots)
        except (KeyError, StopIteration, ValueError) as error:
            errors.append("missing_or_invalid_observation:" + str(error))
    report = {"schema": 1, "scenario": name, "requested_vm": vm, "observed_modes": modes,
              "scope": "Offline cheat fixture with staged/paused/god actors; excluded from normal-rules balance and win-rate evidence.",
              "exit_code": process.returncode, "timed_out": timed_out,
              "wall_seconds": time.monotonic()-started, "errors": errors, "checks": checks,
              "bot_settings_verification": settings_evidence,
              "scenario_overrides": {"g_botTaunt": "0"} if name == "disabled_taunts" else {},
              "passed": not errors and bool(checks) and all(c["passed"] for c in checks),
              "records_sha256": sha(path) if path.exists() else None,
              "parsed_records_sha256": hashlib.sha256(json.dumps(records, sort_keys=True, separators=(",", ":")).encode()).hexdigest(),
              "snapshot_count": len(snapshots)}
    (home / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--server", type=Path, default=Path("build-bots-msvc/Release/tremded.exe"))
    parser.add_argument("--basepath", type=Path, default=Path("build-bots-msvc/Release"))
    parser.add_argument("--output", type=Path, default=Path("build-bot-benchmarks/behavior-scenarios"))
    parser.add_argument("--vm", type=int, choices=(0, 1, 2), action="append")
    parser.add_argument("--scenario", choices=tuple(SCENARIOS), action="append")
    parser.add_argument("--wall-timeout", type=float, default=60)
    parser.add_argument("--run", action="store_true", help="launch the explicitly frozen runtime")
    opt = parser.parse_args()
    output = opt.output.resolve() / ("suite-" + uuid.uuid4().hex[:10])
    output.mkdir(parents=True)
    fixture = output / (MAP + ".pk3")
    manifest = generate_map(fixture)
    manifest["scope"] = "Synthetic server-only fixtures; never shipped game maps or balance evidence."
    (output / "fixture.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print("Fixture:", output, flush=True)
    if not opt.run:
        for name in opt.scenario or SCENARIOS:
            (output / (name + ".cfg")).write_text("\n".join(SCENARIOS[name]()) + "\n")
        return
    server, basepath = opt.server.resolve(), opt.basepath.resolve()
    files = [server, basepath / "base/game.dll", basepath / "base/vm/game.qvm"]
    before = {str(p.relative_to(basepath)): sha(p) for p in files}
    reports = []
    for vm in opt.vm or [0]:
        for name in opt.scenario or SCENARIOS:
            if {str(p.relative_to(basepath)): sha(p) for p in files} != before:
                raise RuntimeError("Frozen runtime changed; no further scenario may launch")
            report = run_case(server, basepath, output / (name+"-vm"+str(vm)), name, vm, fixture, opt.wall_timeout)
            reports.append(report)
            print(name, vm, "PASS" if report["passed"] else "FAIL", report["errors"], flush=True)
    after = {str(p.relative_to(basepath)): sha(p) for p in files}
    aggregate = {"schema": 1, "scope": manifest["scope"], "fixture": manifest,
                 "runtime_sha256": before, "runtime_unchanged": before == after,
                 "profile": PROFILE, "runs": reports,
                 "all_owned_processes_terminal": True,
                 "passed": before == after and all(r["passed"] for r in reports)}
    (output / "aggregate.json").write_text(json.dumps(aggregate, indent=2) + "\n")
    print("Aggregate:", output / "aggregate.json", flush=True)
    raise SystemExit(0 if aggregate["passed"] else 1)


if __name__ == "__main__":
    main()
