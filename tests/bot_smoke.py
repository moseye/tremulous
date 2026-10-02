"""Run a real, isolated Tremulous server and exercise bot gameplay/administration.

Requires a built patched server, matching game module, and stock ATCS data.
Python standard library only. Output includes a JSON report and server logs.
"""
import argparse
import json
import os
import pathlib
import re
import socket
import subprocess
import time
import uuid

from bot_benchmark import VM_MODES, bot_settings_evidence, build_identity, observed_vm_mode


# Match assets/bots16.cfg without changing the legacy smoke-test defaults.
BALANCED_PROFILE = {
    'g_botCombatTuning': '1', 'g_botHumanAimCone': '6',
    'g_botHumanReaction': '600', 'g_botHumanTurnSpeed': '160',
    'g_botHumanFireDelay': '300', 'g_botTeamwork': '1',
    'g_botSpawnScale': '1', 'g_botNavTuning': '1', 'g_botNavNodes': '8192',
}
BOOT_CVARS = {
    'fs_basepath', 'fs_homepath', 'dedicated', 'net_ip', 'net_enabled',
    'net_port', 'sv_master1', 'sv_master2', 'sv_maxclients', 'vm_game',
    'rconPassword',
}


def prepare_command(server, home, settings, map_name):
    # The engine parses only 32 +commands. Keep network/RCON and early-init
    # settings in argv, then apply every gameplay/profile setting before map.
    command = [str(server)]
    lines = []
    for key, value in settings.items():
        if key in BOOT_CVARS:
            command += ['+set', key, value]
        else:
            lines.append(f'set {key} "{value}"')
    lines.append(f'map {map_name}')
    base = home / 'base'
    base.mkdir(exist_ok=True)
    config = base / 'botsmoke-run.cfg'
    config.write_text('\n'.join(lines) + '\n', encoding='utf-8')
    command += ['+exec', config.name]
    return command, config


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--server', required=True, type=pathlib.Path)
    parser.add_argument('--basepath', required=True, type=pathlib.Path)
    parser.add_argument('--vm', type=int, choices=(0, 1, 2), default=2)
    parser.add_argument('--duration', type=float, default=45)
    parser.add_argument('--map', default='atcs')
    parser.add_argument('--bots-per-team', type=int, default=4,
                        help='bots on each team, from 4 through 31 (default: 4)')
    parser.add_argument('--bell-skills', action='store_true',
                        help='verify bell-shaped skill assignment and persistence')
    parser.add_argument('--economy', action='store_true',
                        help='accelerate normal passive funds and unlock stages; verify buying/evolving')
    parser.add_argument('--balanced-profile', action='store_true',
                        help='use and verify the same bot settings as assets/bots16.cfg')
    parser.add_argument('--output', type=pathlib.Path, default=pathlib.Path('build-bot-tests'))
    args = parser.parse_args()
    if not 4 <= args.bots_per_team <= 31:
        parser.error('--bots-per-team must be between 4 and 31')
    count = args.bots_per_team
    total = count * 2
    args.output.mkdir(parents=True, exist_ok=True)
    home = args.output.resolve() / f'vm{args.vm}-{uuid.uuid4().hex[:8]}'
    home.mkdir()
    # Bind an ephemeral local port before launch; no public listener or master registration.
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as reserve:
        reserve.bind(('127.0.0.1', 0))
        port = reserve.getsockname()[1]
    password = uuid.uuid4().hex
    settings = {
        'fs_basepath': str(args.basepath.resolve()), 'fs_homepath': str(home),
        'dedicated': '1', 'net_ip': '127.0.0.1', 'net_enabled': '1', 'net_port': str(port),
        'sv_master1': '', 'sv_master2': '', 'sv_maxclients': str(min(64, total + 8)), 'sv_pure': '0',
        'g_doWarmup': '0', 'g_botDebug': '1', 'logfile': '2',
        'g_logFile': 'gameplay.log', 'g_logFileSync': '1', 'g_inactivity': '1',
        'sv_timeout': '2', 'vm_game': str(args.vm), 'rconPassword': password,
    }
    profile = dict(BALANCED_PROFILE) if args.balanced_profile else {}
    settings.update(profile)
    if args.economy:
        settings.update(g_freeFundPeriod='1', g_humanStage2Threshold='0',
                        g_humanStage3Threshold='0', g_alienStage2Threshold='0',
                        g_alienStage3Threshold='0')
    command, config = prepare_command(args.server.resolve(), home, settings, args.map)
    transcript = []
    report = {'vm': args.vm, 'map': args.map, 'economy': args.economy,
              'bots_per_team': count, 'duration_seconds': args.duration,
              'bell_skills': args.bell_skills,
              'profile': 'balanced' if args.balanced_profile else 'legacy',
              'bot_profile': profile, 'startup_config': str(config),
              'build': build_identity(args.server.resolve(), args.basepath.resolve(), args.vm),
              'module_verifications': [], 'profile_verifications': [],
              'output': str(home), 'checks': []}
    out = (home / 'stdout.log').open('w', encoding='utf-8')
    options = {'creationflags': subprocess.CREATE_NO_WINDOW} if os.name == 'nt' else {}
    process = subprocess.Popen(command, cwd=args.basepath.resolve(), stdout=out,
                               stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, **options)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.35)

    def rcon(text):
        if process.poll() is not None:
            raise RuntimeError(f'server exited {process.returncode}; see {home}')
        sock.sendto(b'\xff' * 4 + f'rcon {password} {text}'.encode(), ('127.0.0.1', port))
        chunks = []
        while True:
            try:
                chunks.append(sock.recv(65535)[4:].decode('utf-8', 'replace'))
            except (socket.timeout, ConnectionResetError):
                break
        response = ''.join(chunks)
        transcript.append({'command': text, 'response': response})
        # Respect rcon flood protection and keep responses attributable to a command.
        time.sleep(0.2)
        return response

    def check(condition, label):
        if not condition:
            raise AssertionError(f'{label}; see {home}')
        report['checks'].append(label)
        print(f'PASS: {label}', flush=True)

    def verify_runtime(stage):
        mode = observed_vm_mode(rcon('vminfo'))
        report['module_verifications'].append({
            'stage': stage, 'requested': VM_MODES[args.vm], 'observed': mode})
        check(mode == VM_MODES[args.vm], f'loaded game mode is {VM_MODES[args.vm]} ({stage})')
        if args.balanced_profile:
            evidence = bot_settings_evidence(profile, rcon('cvarlist g_bot*'), require_all=True)
            report['profile_verifications'].append({'stage': stage, **evidence})
            report['bot_settings_verification'] = evidence
            check(evidence['verified'],
                  f'balanced profile has nine registered matching bot cvars ({stage})'
                  + (f": {evidence['error']}" if evidence['error'] else ''))

    def bots(response):
        return re.findall(r'^\s*(\d+)\s+Bot-[HA]-\d+\s+(human|alien|spectator)\s+skill (\d+)\s+(attack|build|defend)\s+(alive|queued)', response, re.M)

    def skills(response):
        return {row[0]: int(row[2]) for row in bots(response)}

    try:
        deadline = time.monotonic() + 20
        while 'botnav:' not in rcon('bot list'):
            if time.monotonic() >= deadline:
                raise TimeoutError(f'server did not initialize; see {home}')
        verify_runtime('initial')
        skill_mode = 'bell' if args.bell_skills else '7'
        rcon(f'bot fill humans {count} {skill_mode}')
        rcon(f'bot fill aliens {count} {skill_mode}')
        initial = rcon('bot list')
        check(len(bots(initial)) == total, f'{total} bots added in real server slots')
        if args.bell_skills:
            report['initial_skill_histograms'] = {
                team: [sum(row[1] == team and int(row[2]) == skill for row in bots(initial))
                       for skill in range(1, 11)] for team in ('human', 'alien')}
            check(all(len({row[2] for row in bots(initial) if row[1] == team}) >= 4
                      and sum(histogram[3:7]) > count / 2
                      for team, histogram in report['initial_skill_histograms'].items()),
                  'each team has varied skills concentrated near the bell-curve center')
        rcon('bot skill all 8')
        rcon('bot role 3 defend')
        check('skill 8 defend' in rcon('bot list'), 'skill and role controls')
        if args.bell_skills:
            rcon('bot skill all bell')
            check(len(set(skills(rcon('bot list')).values())) >= 4,
                  'existing bots accept a bell-shaped skill reassignment')
        saved_skills = skills(rcon('bot list'))
        # Seed from the newly spawned builder before combat can leave a client queued.
        check('botnav: added' in rcon('botnav add 0'), 'manual navigation seed accepted')
        check('wrote' in rcon('botnav save'), 'navigation seeds persist to disk')
        snapshots = []
        deadline = time.monotonic() + args.duration
        while time.monotonic() < deadline:
            time.sleep(min(5, max(0, deadline - time.monotonic())))
            snapshots.append(rcon('bot list'))
        check(process.poll() is None and len(bots(snapshots[-1])) == total,
              'bots survive network timeout and inactivity settings')
        report['spawned_bots_per_team'] = {
            team: len({row[0] for snapshot in snapshots for row in bots(snapshot)
                       if row[1] == team and row[4] == 'alive'})
            for team in ('human', 'alien')}
        check(all(any(row[1] == team and row[4] == 'alive'
                      for snapshot in snapshots for row in bots(snapshot))
                  for team in ('human', 'alien')),
              'both teams use normal spawn queues')
        observed = re.findall(
            r'^\s*\d+\s+Bot-[HA]-\d+\s+(human|alien)\s+skill \d+\s+'
            r'(attack|build|defend)\s+alive\n\s+hp \d+ class (\d+) weapon (\d+) credits (\d+)',
            '\n'.join(snapshots), re.M)
        report['human_weapons'] = sorted({int(row[3]) for row in observed if row[0] == 'human'})
        report['alien_classes'] = sorted({int(row[2]) for row in observed if row[0] == 'alien'})
        if args.economy:
            check(any(row[0] == 'human' and 11 <= int(row[3]) <= 18 for row in observed),
                  'humans purchase an upgraded weapon at an armoury')
            check(any(row[0] == 'alien' and row[1] != 'build' and 4 <= int(row[2]) <= 10
                      for row in observed), 'alien fighters evolve using earned credits')
        points = re.findall(r'pos (-?\d+) (-?\d+) (-?\d+)', '\n'.join(snapshots))
        check(len(set(points)) > 12, 'bots move through real collision and player movement')
        nav = rcon('botnav status')
        rcon('botnav paths')
        check(re.search(r'botnav: [1-9]\d*/[1-9]\d* floor nodes, [1-9]\d* directed links', nav),
              'collision navigation graph generates nodes and links')
        rcon('bot team 2 spectator')
        time.sleep(1)
        check(any(row[0] == '2' and row[1] == 'spectator' for row in bots(rcon('bot list'))),
              'spectator assignment is retained')
        rcon('bot team 2 humans')
        rcon('bot add humans 99')
        check(len(bots(rcon('bot list'))) == total, 'invalid skill rejected without adding a client')
        rcon('map_restart 0')
        time.sleep(3)
        restarted = rcon('bot list')
        check(len(bots(restarted)) == total and skills(restarted) == saved_skills
              and any(row[0] == '3' and row[3] == 'defend' for row in bots(restarted)),
              'map restart reconnects bots and preserves settings')
        verify_runtime('map_restart')
        rcon(f'map {args.map}')
        time.sleep(3)
        reloaded = rcon('bot list')
        check(len(bots(reloaded)) == total and skills(reloaded) == saved_skills,
              'full map load reconnects bot clients and preserves skills')
        verify_runtime('map_load')
        rcon('bot remove 0')
        rcon('bot add humans bell' if args.bell_skills else 'bot add humans 8')
        check(len(bots(rcon('bot list'))) == total, 'removed bot slots can be reused')
        rcon('bot fill humans 2')
        check(len(bots(rcon('bot list'))) == count + 2, 'fill reduces the bot count')
        rcon('bot remove all')
        check(not bots(rcon('bot list')), 'remove all releases every bot client')
        log = home / 'base' / 'gameplay.log'
        text = log.read_text(encoding='utf-8', errors='replace') if log.exists() else ''
        report['construction_events'] = len(re.findall(r'Construct:', text))
        report['kill_events'] = len(re.findall(r'Die:', text))
        report['ok'] = True
    except Exception as error:
        report['ok'] = False
        report['error'] = str(error)
        raise
    finally:
        if process.poll() is None:
            try:
                rcon('quit')
                process.wait(timeout=5)
            except Exception:
                process.terminate()
                process.wait(timeout=5)
        out.close()
        sock.close()
        gameplay = home / 'base' / 'gameplay.log'
        if gameplay.exists():
            text = gameplay.read_text(encoding='utf-8', errors='replace')
            report['construction_events'] = len(re.findall(r'Construct:', text))
            report['kill_events'] = len(re.findall(r'Die:', text))
        (home / 'report.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        (home / 'rcon.json').write_text(json.dumps(transcript, indent=2), encoding='utf-8')
        print(json.dumps(report, indent=2), flush=True)


if __name__ == '__main__':
    main()
