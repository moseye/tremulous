"""Verify benchmark evidence handling without launching a server."""
import copy
import contextlib
import importlib.util
import json
import pathlib
import unittest
import uuid


spec = importlib.util.spec_from_file_location("bot_benchmark", pathlib.Path(__file__).with_name("bot_benchmark.py"))
benchmark = importlib.util.module_from_spec(spec)
spec.loader.exec_module(benchmark)


@contextlib.contextmanager
def test_directory():
    # Inherit workspace ACLs; Python's private Windows temp ACL can exclude a
    # sandbox identity even when that identity created the directory itself.
    root = pathlib.Path(__file__).resolve().parent.parent / "build-bot-benchmark-unit"
    root.mkdir(exist_ok=True)
    directory = root / uuid.uuid4().hex
    directory.mkdir()
    try:
        yield directory
    finally:
        for path in directory.iterdir():
            path.unlink()
        directory.rmdir()


def event(kind="start", elapsed=0, winner="running", wall=0):
    team = {"bots": 16, "skill_histogram": [0, 0, 1, 3, 4, 4, 3, 1, 0, 0],
            "kills": 0, "queued_player_seconds": 120, "peak_queue": 16,
            "core_health": 750}
    return {"schema": 1, "event": kind, "map": "arachnid2", "seed": 17,
            "elapsed_ms": elapsed, "wall_elapsed_ms": wall,
            "winner": winner, "reason": "time_limit" if kind == "finish" else "",
            "min_step_ms": 50, "max_step_ms": 50,
            "humans": copy.deepcopy(team), "aliens": copy.deepcopy(team),
            "nav": {"nodes": 1000, "routes": 2}}


def run(seed=17, winner="draw", status="finished", profile="candidate"):
    terminal = event("finish", 30000, winner, 600)
    terminal["seed"] = seed
    return {"profile": profile, "map": "arachnid2", "seed": seed, "repeat": 0,
            "bots_per_team": 16, "requested_seconds": 30, "sv_fps": 20, "vm": 0,
            "status": status, "winner": winner if status == "finished" else None,
            "reason": "time_limit", "terminal": terminal, "simulated_seconds": 30,
            "simulation_speedup": 50, "speedup_including_startup": 10,
            "telemetry_sha256": "a"}


class TelemetryTests(unittest.TestCase):
    def test_partial_records_are_not_parsed_early(self):
        with test_directory() as directory:
            path = pathlib.Path(directory) / "botbench.jsonl"
            reader = benchmark.TelemetryReader(path, 17)
            self.assertEqual(reader.poll(), [])
            text = json.dumps(event())
            path.write_text(text[:40], encoding="utf-8")
            self.assertEqual(reader.poll(), [])
            with path.open("a", encoding="utf-8") as output:
                output.write(text[40:] + "\n" + json.dumps(event("finish", 30000, "draw", 600)) + "\n")
            self.assertEqual([item["event"] for item in reader.poll()], ["start", "finish"])
            self.assertEqual(reader.poll(), [])

    def test_clock_seed_and_second_match_are_rejected(self):
        for invalid in (event("sample", -1), {**event("sample", 50), "seed": 18}, event()):
            reader = benchmark.TelemetryReader(pathlib.Path("unused"), 17)
            reader.events.append(event())
            with self.assertRaises(ValueError):
                reader.validate(invalid)
        reader = benchmark.TelemetryReader(pathlib.Path("unused"), 17)
        with self.assertRaises(ValueError):
            reader.validate(event("finish", 30000, "draw"))

    def test_replay_excludes_only_wall_timer(self):
        before = [event(), event("finish", 30000, "draw", 600)]
        after = copy.deepcopy(before)
        after[-1]["wall_elapsed_ms"] = 1000
        self.assertEqual(benchmark.telemetry_digest(before), benchmark.telemetry_digest(after))
        after[-1]["aliens"]["kills"] += 1
        self.assertNotEqual(benchmark.telemetry_digest(before), benchmark.telemetry_digest(after))

    def test_optional_snapshots_are_accepted_and_included_in_replay(self):
        before = event()
        before["snapshot"] = {"bots": [{"id": 0, "position": [1, 2, 3]}]}
        benchmark.TelemetryReader(pathlib.Path("unused"), 17).validate(before)
        after = copy.deepcopy(before)
        after["snapshot"]["bots"][0]["position"][0] += 1
        self.assertNotEqual(benchmark.telemetry_digest([before]), benchmark.telemetry_digest([after]))

    def test_accepted_taunts_preserve_events_and_same_tick_observations(self):
        reader = benchmark.TelemetryReader(pathlib.Path("unused"), 17)
        reader.events.append(event())
        accepted = event("taunt", 50)
        accepted["event_actor"] = 17
        accepted["aliens"]["taunts_accepted"] = 1
        reader.validate(accepted)
        reader.events.append(accepted)
        reader.validate(event("sample", 50))
        altered = copy.deepcopy(accepted)
        altered["event_actor"] = 18
        self.assertNotEqual(benchmark.telemetry_digest([accepted]), benchmark.telemetry_digest([altered]))
        for actor in (-1, 64, None, "17"):
            with self.assertRaises(ValueError):
                reader.validate({**accepted, "event_actor": actor})

    def test_probe_fixtures_are_rejected_but_legacy_telemetry_remains_readable(self):
        reader = benchmark.TelemetryReader(pathlib.Path("unused"), 17)
        reader.validate(event())
        reader.validate({**event(), "configuration": {"probe_enabled": 0}, "fixture_mode": 0})
        for fields in ({"configuration": {"probe_enabled": 1}}, {"fixture_mode": 1}):
            with self.assertRaises(benchmark.FixtureTelemetryError):
                reader.validate({**event(), **fields})

    def test_server_error_terminal_is_recognized_without_becoming_a_game_outcome(self):
        reader = benchmark.TelemetryReader(pathlib.Path("unused"), 17)
        reader.events.append(event())
        failed = event("finish", 500, "error")
        failed["reason"] = "map_shutdown"
        reader.validate(failed)
        self.assertNotIn(failed["winner"], benchmark.WINNERS)

    def test_connection_summary_preserves_early_connection_after_core_destruction(self):
        start, sample, finish = event(), event("sample", 60000), event("finish", 900000, "humans")
        start["nav"]["bases_connected"] = 0
        sample["nav"]["bases_connected"] = 1
        finish["nav"]["bases_connected"] = 0
        finish["humans"]["building_damage"] = 100
        summary = benchmark.telemetry_diagnostics([start, sample, finish])
        self.assertTrue(summary["base_connection_observed"])
        self.assertEqual(summary["first_base_connection_observed_seconds"], 60)
        self.assertEqual(summary["first_structure_damage_observed_seconds"]["humans"], 900)

    def test_rebuilt_hq_does_not_hide_sampled_core_loss(self):
        start, sample, finish = event(), event("sample", 60000), event("finish", 900000, "draw")
        sample["aliens"]["core_health"] = 0
        summary = benchmark.telemetry_diagnostics([start, sample, finish])
        self.assertEqual(summary["minimum_observed_core_health"], {"humans": 750, "aliens": 0})
        self.assertEqual(summary["first_core_missing_observed_seconds"], {"humans": None, "aliens": 60})
        for value in (start, sample, finish):
            del value["humans"]["core_health"]
        self.assertIsNone(benchmark.telemetry_diagnostics([start, sample, finish])[
            "minimum_observed_core_health"]["humans"])


class AggregateTests(unittest.TestCase):
    def test_draws_and_faults_are_not_wins(self):
        matches = [run(17, "humans"), run(29, "aliens"), run(41),
                   run(53, status="wall_timeout"), run(59, status="crash"),
                   run(61, status="invalid_settings")]
        aggregate = benchmark.aggregate_runs(matches, 3)
        profile = aggregate["profiles"]["candidate"]
        self.assertEqual((profile["runs"], profile["finished"], profile["draws"]), (6, 3, 1))
        self.assertEqual(profile["human_win_fraction_decisive"], 0.5)
        self.assertEqual(profile["time_limit_draws"], 1)
        self.assertEqual(aggregate["total_simulated_seconds"], 90)
        self.assertEqual(profile["mean_final_metrics"]["humans"]["mean_queued_players"], 4)

    def test_comparisons_pair_exact_scenarios(self):
        old = [run(17, "humans"), run(29), run(41)]
        new = [run(17, "aliens"), run(29, status="crash"), run(53)]
        new[0]["terminal"]["aliens"]["kills"] = 9
        comparison = benchmark.compare_aggregates({"runs": old}, {"runs": new})
        self.assertEqual((comparison["paired_runs"], comparison["valid_paired_runs"]), (2, 1))
        self.assertEqual(comparison["unpaired_baseline"], 1)
        self.assertEqual(comparison["unpaired_candidate"], 1)
        self.assertEqual(comparison["outcome_transitions"], {"humans->aliens": 1})
        self.assertEqual(comparison["mean_candidate_minus_baseline"]["aliens"]["kills"], 9)

    def test_duplicate_profiles_cannot_be_mispaired(self):
        with self.assertRaises(ValueError):
            benchmark.compare_aggregates({"runs": [run(), run(profile="baseline")]}, {"runs": []})

    def test_replay_records_require_valid_matching_telemetry(self):
        one, two = run(), run()
        two["repeat"] = 1
        self.assertTrue(benchmark.replay_summary([one, two])[0]["telemetry_matches"])
        two["status"] = "invalid_tick"
        self.assertFalse(benchmark.replay_summary([one, two])[0]["telemetry_matches"])

    def test_stalemate_warning_does_not_reclassify_finished_draws(self):
        match = run()
        match.update(simulated_seconds=900, requested_seconds=900,
                     telemetry_diagnostics={"base_connection_observed": False})
        for team in benchmark.TEAMS:
            match["terminal"][team]["building_damage"] = 0
        aggregate = benchmark.aggregate_runs([match], 20)
        profile = aggregate["profiles"]["candidate"]
        self.assertEqual(profile["statuses"], {"finished": 1})
        self.assertEqual(profile["time_limit_draws"], 1)
        self.assertEqual({warning["code"] for warning in profile["warnings"]},
                         {"strategic_stalemate", "zero_structure_damage", "no_recorded_kills", "no_observed_base_connection"})
        self.assertEqual(match["status"], "finished")

    def test_meaningful_pressure_or_short_runs_do_not_warn_of_strategic_stalemate(self):
        match = run()
        match.update(simulated_seconds=900, requested_seconds=900)
        for team in benchmark.TEAMS:
            match["terminal"][team].update(building_damage=1000, kills=10)
        self.assertEqual(benchmark.diagnostic_warnings([match]), [])
        match["simulated_seconds"] = 30
        for team in benchmark.TEAMS:
            match["terminal"][team]["building_damage"] = 0
        self.assertEqual(benchmark.diagnostic_warnings([match]), [])

    def test_missing_pressure_counters_are_not_assumed_zero(self):
        match = run()
        match.update(simulated_seconds=900, requested_seconds=900)
        codes = {warning["code"] for warning in benchmark.diagnostic_warnings([match])}
        self.assertIn("missing_objective_metrics", codes)
        self.assertNotIn("strategic_stalemate", codes)

    def test_module_mode_mismatch_is_not_a_valid_gameplay_comparison(self):
        before, after = run(), run()
        before["observed_vm_mode"], after["observed_vm_mode"] = "native", "compiled_qvm"
        comparison = benchmark.compare_aggregates({"runs": [before]}, {"runs": [after]})
        self.assertEqual(comparison["valid_paired_runs"], 0)
        self.assertFalse(comparison["pairs"][0]["same_observed_vm_mode"])
        self.assertEqual(comparison["outcome_transitions"], {})


class InputAndLogTests(unittest.TestCase):
    def test_many_profile_cvars_do_not_exceed_engine_startup_command_limit(self):
        with test_directory() as home:
            settings = {name: "1" for name in benchmark.BOOT_CVARS}
            settings.update({f"g_tuning{number}": str(number) for number in range(50)})
            command, config = benchmark.prepare_command(pathlib.Path("server.exe"), home,
                                                         settings, "arachnid2", 16, 900)
            self.assertLess(sum(value.startswith("+") for value in command), 32)
            self.assertIn("+exec", command)
            self.assertIn("+set", command)
            text = config.read_text(encoding="utf-8")
            self.assertIn('set g_tuning49 "49"', text)
            self.assertTrue(text.endswith("map arachnid2\nvminfo\ncvarlist g_bot*\nbotbench start 16 16 900\n"))
            config.unlink()
            config.parent.rmdir()

    def test_controlled_network_and_tick_settings_cannot_be_overridden(self):
        for value in ("net_enabled=1", "sv_fps=5", "g_botBuild=1;quit", "g_botBuild=1\nquit",
                      "not a cvar=1", "g_botBuild"):
            with self.assertRaises(ValueError):
                benchmark.parse_settings([value])
        self.assertEqual(benchmark.parse_settings(["g_botBuild=0"]), {"g_botBuild": "0"})
        with self.assertRaises(ValueError):
            benchmark.parse_settings(["g_botBuild=0", "G_BOTBUILD=1"])

    def test_registered_bot_settings_require_engine_flags_and_actual_values(self):
        settings = {"g_botHumanReaction": "600", "G_BOTHUMANAIMCONE": "6.0", "g_customRule": "1"}
        stdout = ('     A    g_botHumanReaction "600"\n'
                  '     A    g_botHumanAimCone "6"\n')
        evidence = benchmark.bot_settings_evidence(settings, stdout, require_all=True)
        self.assertTrue(evidence["verified"])
        self.assertFalse(evidence["invalid"])
        self.assertEqual(set(evidence["observed"]), {"g_botHumanReaction", "G_BOTHUMANAIMCONE"})
        self.assertTrue(evidence["observed"]["g_botHumanReaction"]["registered"])

    def test_user_created_bot_typos_are_rejected_as_invalid_experiments(self):
        stdout = '        ? g_botHumanReactionTime "600"\n'
        evidence = benchmark.bot_settings_evidence({"g_botHumanReactionTime": "600"}, stdout)
        self.assertFalse(evidence["verified"])
        self.assertTrue(evidence["invalid"])
        self.assertEqual(evidence["unregistered"], ["g_botHumanReactionTime"])
        self.assertIn("unregistered", evidence["error"])

    def test_missing_partial_output_waits_but_missing_final_evidence_fails(self):
        settings = {"g_botTeamwork": "1"}
        self.assertFalse(benchmark.bot_settings_evidence(settings, "")["invalid"])
        final = benchmark.bot_settings_evidence(settings, "", require_all=True)
        self.assertTrue(final["invalid"])
        self.assertEqual(final["missing"], ["g_botTeamwork"])
        self.assertTrue(benchmark.bot_settings_evidence({"g_generalCustomCvar": "1"}, "")["verified"])

    def test_registered_but_ignored_requested_values_are_rejected(self):
        stdout = '     A    g_botTeamwork "0"\n'
        evidence = benchmark.bot_settings_evidence({"g_botTeamwork": "1"}, stdout, require_all=True)
        self.assertTrue(evidence["invalid"])
        self.assertEqual(evidence["mismatched_values"], ["g_botTeamwork"])

    def test_seed_ranges_and_duplicates(self):
        self.assertEqual(benchmark.parse_seeds("17,29,41"), [17, 29, 41])
        for value in ("0", "17,17", "a", "2147483648", ""):
            with self.assertRaises(ValueError):
                benchmark.parse_seeds(value)

    def test_module_mode_is_observed_in_engine_output(self):
        for text, expected in (("game : native\n", "native"),
                               ("game : compiled on load\n", "compiled_qvm"),
                               ("game : interpreted\n", "interpreted_qvm"),
                               (r"Sys_LoadGameDll(C:\Game\base\game.dll) found vmMain function at 123", "native")):
            self.assertEqual(benchmark.observed_vm_mode(text), expected)
        self.assertIsNone(benchmark.observed_vm_mode("Try loading dll file game.dll\nFailed loading dll"))

    def test_legacy_comparison_recovers_actual_mode_without_modifying_saved_report(self):
        with test_directory() as directory:
            path = directory / "aggregate.json"
            fixture = run()
            fixture["home"] = str(directory)
            path.write_text(json.dumps({"schema": 1, "runs": [fixture]}), encoding="utf-8")
            (directory / "stdout.log").write_text(
                r"Sys_LoadGameDll(C:\Game\base\game.dll) found vmMain function at 123", encoding="utf-8")
            loaded = benchmark.load_aggregate(path)
            self.assertEqual(loaded["runs"][0]["observed_vm_mode"], "native")
            self.assertNotIn("observed_vm_mode", json.loads(path.read_text())["runs"][0])

    def test_gameplay_counts_keep_world_deaths_separate(self):
        with test_directory() as directory:
            path = pathlib.Path(directory) / "gameplay.log"
            path.write_text("""  0:00 ChangeTeam: 0 human: Bot-H-1 switched teams
  0:00 ChangeTeam: 16 alien: Bot-A-17 switched teams
  0:04 Construct: 0 155 telenode: Bot-H-1 is building Telenode
  0:10 Die: 0 16 MOD_MACHINEGUN: Bot-H-1 killed Bot-A-17
  0:30 Die: 132 16 MOD_MGTURRET: <world> killed Bot-A-17
  0:40 Exit: Humans win.
""", encoding="utf-8")
            metrics = benchmark.gameplay_metrics(path)
            self.assertEqual(metrics["kills_by_team"], {"humans": 1, "world_or_building": 1})
            self.assertEqual(metrics["deaths_by_team"], {"aliens": 2})
            self.assertEqual(metrics["spawn_builds_by_team"], {"humans": 1})
            self.assertEqual(metrics["exit_reasons"], ["Humans win."])


if __name__ == "__main__":
    unittest.main()
