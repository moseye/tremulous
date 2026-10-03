"""Package compiled Tremulous bots, unchanged upstream media, and patched source.

Example: python misc/package_bots.py --build build-bots-msvc/Release
  --data build-bots-msvc/data --output dist-bots
"""
import argparse
import hashlib
import json
import math
import pathlib
import re
import shutil
import subprocess
import zipfile


BOT_CHECKPOINTS = {
    'bot-balance-checkpoint-2026-10-02.json': 'sanitized_bot_balance_checkpoint',
    'bot-progress-retention-2026-10-02.json': 'sanitized_bot_progress_retention_checkpoint',
}
RUNTIME_FILES = ('tremulous.exe', 'tremded.exe', 'SDL2.dll',
                 'renderer_opengl1.dll', 'renderer_opengl2.dll')
NATIVE_MODULES = ('game.dll', 'cgame.dll', 'ui.dll')
UPSTREAM_NOTICES = ('COPYING', 'CREDITS.md', 'sha256.txt', 'upstream-manifest.json')
BOT_PROFILE_FIELDS = ('g_botCombatTuning', 'g_botTeamwork', 'g_botSpawnScale',
                      'g_botNavTuning', 'g_botNavNodes', 'g_botHumanAimCone',
                      'g_botHumanReaction', 'g_botHumanTurnSpeed', 'g_botHumanFireDelay')
VM_MODES = ('native', 'interpreted_qvm', 'compiled_qvm')


def report_destination(report):
    if 'vm' not in report:
        return 'client-startup.json'
    if type(report['vm']) is not int or report['vm'] not in (0, 1, 2):
        raise ValueError('validation report has an invalid VM identifier')
    if not isinstance(report.get('map'), str) or not re.fullmatch(r'[A-Za-z0-9_-]+', report['map']):
        raise ValueError('validation report has an invalid map identifier')
    count = report.get('bots_per_team', 4)
    if type(count) is not int or count < 1:
        raise ValueError('validation report has an invalid team size')
    suffix = '-economy' if report.get('economy') is True else ''
    if count != 4:
        suffix += f'-{count}v{count}'
    if report.get('bell_skills') is True:
        suffix += '-bell'
    return f'vm{report["vm"]}-{report["map"]}{suffix}.json'


def public_profile(values):
    if not isinstance(values, dict):
        return {}
    return {name: str(values[name]) for name in BOT_PROFILE_FIELDS if name in values and
            re.fullmatch(r'-?\d+(?:\.\d+)?', str(values[name]))}


def public_validation_summary(file, report):
    """Copy semantic values explicitly; raw paths, commands and logs never enter the package."""
    summary = {'schema': 1, 'kind': 'sanitized_bot_validation_summary', 'ok': True,
               'source_report_sha256': hashlib.sha256(file.read_bytes()).hexdigest(),
               'scope': 'Public semantic summary of a passed smoke/client report. '
                        'The source report hash preserves traceability; this is not '
                        'competitive balance certification or proof of source-to-binary equivalence.',
               'passed_check_count': len(report['checks']) if isinstance(report.get('checks'), list) else None}
    for name in ('economy', 'bell_skills', 'warmup_disabled', 'renderer_observed',
                 'bell_skills_observed', 'package_files_unchanged'):
        if type(report.get(name)) is bool:
            summary[name] = report[name]
    for name in ('vm', 'bots_per_team', 'duration_seconds', 'construction_events', 'kill_events', 'exit_code'):
        value = report.get(name)
        if type(value) in (int, float) and math.isfinite(value):
            summary[name] = value
    for name in ('map', 'profile'):
        value = report.get(name)
        if isinstance(value, str) and re.fullmatch(r'[A-Za-z0-9_-]+', value):
            summary[name] = value
    for name in ('human_weapons', 'alien_classes'):
        value = report.get(name)
        if isinstance(value, list) and all(type(item) is int for item in value):
            summary[name] = value
    for name in ('spawned_bots_per_team', 'roster_counts', 'initial_skill_histograms', 'skill_histograms'):
        values = report.get(name)
        if not isinstance(values, dict):
            continue
        clean = {}
        for team in ('human', 'alien'):
            value = values.get(team)
            if type(value) is int or (isinstance(value, list) and all(type(item) is int for item in value)):
                clean[team] = value
        if clean:
            summary[name] = clean
    summary['bot_profile'] = public_profile(report.get('bot_profile'))
    stages = report.get('profile_verifications', [])
    if isinstance(report.get('profile_verification'), dict):
        stages = [{'stage': 'after_map', **report['profile_verification']}]
    summary['profile_verifications'] = []
    for stage in stages if isinstance(stages, list) else []:
        if not isinstance(stage, dict) or stage.get('stage') not in ('initial', 'map_restart', 'map_load', 'after_map'):
            continue
        observed = stage.get('observed', {})
        values = {name: value.get('value') for name, value in observed.items()
                  if name in BOT_PROFILE_FIELDS and isinstance(value, dict)} if isinstance(observed, dict) else {}
        clean = {'stage': stage['stage'], 'verified': stage.get('verified') is True,
                 'requested': public_profile(stage.get('requested')),
                 'observed': {name: {'value': value, 'registered': observed[name].get('registered') is True}
                              for name, value in public_profile(values).items()}}
        clean['verified'] = (clean['verified'] and set(clean['requested']) == set(BOT_PROFILE_FIELDS) and
                             set(clean['observed']) == set(BOT_PROFILE_FIELDS) and
                             all(value['registered'] and value['value'] == clean['requested'][name]
                                 for name, value in clean['observed'].items()))
        summary['profile_verifications'].append(clean)
        if not summary['bot_profile']:
            summary['bot_profile'] = clean['requested']
    summary['module_verifications'] = []
    for stage in report.get('module_verifications', []):
        if isinstance(stage, dict) and stage.get('stage') in ('initial', 'map_restart', 'map_load'):
            summary['module_verifications'].append({key: stage[key] for key in ('stage', 'requested', 'observed')
                                                    if key in stage and (key == 'stage' or stage[key] in VM_MODES)})
    mode = report.get('observed_game_mode')
    if mode in VM_MODES:
        summary['observed_mode'] = mode
    elif summary['module_verifications']:
        summary['observed_mode'] = summary['module_verifications'][0].get('observed')
    build = report.get('build', {})
    summary['build_hashes'] = []
    for entry in build.get('files', []) if isinstance(build, dict) else []:
        if not isinstance(entry, dict):
            continue
        name, size, digest = entry.get('name'), entry.get('bytes'), entry.get('sha256')
        if (isinstance(name, str) and re.fullmatch(r'[A-Za-z0-9_.-]+', name) and
                type(size) is int and size >= 0 and isinstance(digest, str) and
                re.fullmatch(r'[A-Fa-f0-9]{64}', digest)):
            summary['build_hashes'].append({'name': name, 'bytes': size, 'sha256': digest.lower()})
    if isinstance(report.get('screenshots'), list):
        summary['screenshot_count'] = len(report['screenshots'])
    return summary


def packaging_build_info(source, build):
    def git(*args):
        return subprocess.check_output(['git', *args], cwd=source).decode('utf-8').strip()
    dirty = set(filter(None, git('diff', '--name-only', 'HEAD', '-z').split('\0')))
    dirty.update(filter(None, git('ls-files', '--others', '--exclude-standard', '-z').split('\0')))
    if any(pathlib.PureWindowsPath(name).is_absolute() or pathlib.PurePosixPath(name).is_absolute()
           or '..' in pathlib.PurePosixPath(name).parts for name in dirty):
        raise ValueError('expected only relative paths in packaging source provenance')
    version_file = build.parent / 'version.txt'
    version = version_file.read_text(encoding='utf-8').strip() if version_file.is_file() else '(unavailable)'
    return ('Packaging provenance; not proof of source-to-binary equivalence.\n'
            'Binary SHA256 values in SHA256.json identify the packaged files.\n\n'
            f'Git HEAD: {git("rev-parse", "HEAD")}\n'
            f'Git branch: {git("rev-parse", "--abbrev-ref", "HEAD")}\n'
            f'Configured product version (build-parent/version.txt): {version}\n'
            'Dirty source paths (relative to the source checkout):\n' +
            ('\n'.join(json.dumps(name.replace('\\', '/')) for name in sorted(dirty)) if dirty else '(none)') + '\n')


def portable_benchmark_text(text):
    for name in BOT_CHECKPOINTS:
        source_link = f'(../tests/results/{name})'
        if source_link not in text:
            raise ValueError(f'benchmark documentation is missing its checkpoint link: {name}')
        text = text.replace(source_link, f'(benchmark-checkpoints/{name})')
    title, newline, body = text.partition('\n')
    notice = ('The bundled checkpoint JSON files are historical evidence for the runtime hashes '
              'they record. They do not validate this portable package or certify balanced play.')
    return title + newline + '\n' + notice + '\n\n' + body.lstrip('\n')


def preflight(source, build, data, output, vc_runtime, reports):
    assets = source / 'assets'
    if output == source or output in source.parents:
        raise ValueError('package output cannot contain the source checkout')
    if output == assets or assets in output.parents:
        raise ValueError('package output cannot be inside the packaged assets')
    qvms = [build / 'base' / 'vm' / name for name in ('game.qvm', 'cgame.qvm', 'ui.qvm')]
    checkpoint_files = [source / 'tests' / 'results' / name for name in BOT_CHECKPOINTS]
    required = (qvms + [build / name for name in RUNTIME_FILES] +
                [build / 'base' / name for name in NATIVE_MODULES] +
                [data / name for name in UPSTREAM_NOTICES] +
                [source / name for name in ('COPYING.txt', 'GPL', 'CC', 'README.md',
                                           'docs/BOTS.md', 'docs/BOT_BENCHMARKS.md',
                                           'assets/bots16.cfg')] + checkpoint_files)
    missing = [str(file) for file in required if not file.is_file()]
    if missing:
        raise ValueError('missing required packaging files: ' + ', '.join(missing))
    if not (source / 'docs' / 'licenses').is_dir():
        raise ValueError('missing bundled component license directory')
    if vc_runtime:
        if vc_runtime.name.lower() != 'vcruntime140.dll':
            raise ValueError('expected the x64 Visual C++ vcruntime140.dll')
        if not vc_runtime.is_file():
            raise ValueError(f'Visual C++ runtime does not exist: {vc_runtime}')
    for checkpoint in checkpoint_files:
        evidence = json.loads(checkpoint.read_text(encoding='utf-8'))
        if evidence.get('kind') != BOT_CHECKPOINTS[checkpoint.name]:
            raise ValueError(f'expected sanitized historical bot checkpoint evidence: {checkpoint.name}')
    benchmark_text = portable_benchmark_text(
        (source / 'docs' / 'BOT_BENCHMARKS.md').read_text(encoding='utf-8'))
    sdl_headers = list((source / 'code' / 'thirdparty').glob('SDL2-*/include/SDL.h'))
    if len(sdl_headers) != 1:
        raise ValueError('cannot identify the bundled SDL license header')
    sdl_notice = sdl_headers[0].read_text(encoding='utf-8').split('*/', 1)[0] + '*/\n'
    checked_reports = []
    report_names = set()
    for file in reports:
        report = json.loads(file.read_text(encoding='utf-8'))
        if report.get('ok') is not True:
            raise ValueError(f'cannot package a failed validation report: {file}')
        name = report_destination(report)
        if name in report_names:
            raise ValueError(f'duplicate packaged validation report name: {name}')
        report_names.add(name)
        checked_reports.append((name, public_validation_summary(file, report)))
    manifest = json.loads((data / 'upstream-manifest.json').read_text(encoding='utf-8-sig'))
    media = sorted(data.glob('*.pk3'))
    if len(media) != 10:
        raise ValueError('expected two data archives and all eight stock maps')
    for file in media:
        upstream = next((item for item in manifest if item['name'] == file.name), None)
        if upstream is None:
            raise ValueError(f'upstream media is missing from the manifest: {file.name}')
        content = file.read_bytes()
        git_hash = hashlib.sha1(f'blob {len(content)}\0'.encode() + content).hexdigest()
        if len(content) != upstream['size'] or git_hash != upstream['sha']:
            raise ValueError(f'upstream media integrity failure: {file}')
    return qvms, checkpoint_files, benchmark_text, sdl_notice, checked_reports, media


def package_files(folder):
    for file in sorted(folder.rglob('*')):
        if file.is_file() and file.relative_to(folder).parts[0] not in ('user', 'server-user'):
            yield file


def archive(folder, destination):
    with zipfile.ZipFile(destination, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as output:
        for file in package_files(folder):
            output.write(file, (pathlib.Path(folder.name) / file.relative_to(folder)).as_posix())
    with zipfile.ZipFile(destination) as check:
        assert check.testzip() is None, f'archive CRC failure: {destination}'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build', required=True, type=pathlib.Path)
    parser.add_argument('--data', required=True, type=pathlib.Path)
    parser.add_argument('--output', type=pathlib.Path, default=pathlib.Path('dist-bots'))
    parser.add_argument('--vc-runtime', type=pathlib.Path,
                        help='unmodified x64 vcruntime140.dll from the compiler redistributable directory')
    parser.add_argument('--report', action='append', type=pathlib.Path, default=[])
    args = parser.parse_args()
    source = pathlib.Path(__file__).resolve().parents[1]
    build = args.build.resolve()
    data = args.data.resolve()
    output = args.output.resolve()
    qvms, checkpoint_files, benchmark_text, sdl_notice, checked_reports, media = preflight(
        source, build, data, output, args.vc_runtime, args.report)
    build_info = packaging_build_info(source, build)
    output.mkdir(parents=True, exist_ok=True)
    portable = output / 'tremulous-bots-windows-x64'
    portable.mkdir(exist_ok=True)
    (portable / 'base').mkdir(exist_ok=True)
    for name in RUNTIME_FILES:
        shutil.copy2(build / name, portable / name)
    if args.vc_runtime:
        shutil.copy2(args.vc_runtime, portable / 'vcruntime140.dll')
        (portable / 'MSVC-RUNTIME-NOTICE.txt').write_text(
            'This package includes the unmodified Microsoft Visual C++ x64 runtime '
            'vcruntime140.dll, supplied with the build compiler.\n'
            'Copyright Microsoft Corporation. All rights reserved.\n'
            'The Windows Universal C Runtime is supplied by Windows 10/11.\n'
            'Microsoft Visual C++ Redistributable information and downloads:\n'
            'https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist\n',
            encoding='utf-8')
    for name in NATIVE_MODULES:
        shutil.copy2(build / 'base' / name, portable / 'base' / name)
    for file in media:
        shutil.copy2(file, portable / 'base' / file.name)
    # Keep upstream archives untouched. Our configs/UI/modules live in a new PK3.
    with zipfile.ZipFile(portable / 'base' / 'zz-tremulous-bots.pk3', 'w', zipfile.ZIP_DEFLATED) as pak:
        for file in sorted((source / 'assets').rglob('*')):
            if file.is_file():
                pak.write(file, file.relative_to(source / 'assets').as_posix())
        for file in qvms:
            pak.write(file, f'vm/{file.name}')
    for name in ('COPYING.txt', 'GPL', 'CC'):
        shutil.copy2(source / name, portable / name)
    for name in UPSTREAM_NOTICES:
        shutil.copy2(data / name, portable / ('UPSTREAM-' + name))
    # The bundled SDL distribution carries its zlib notice in the public headers.
    (portable / 'SDL2-LICENSE.txt').write_text(sdl_notice, encoding='utf-8')
    shutil.copytree(source / 'docs' / 'licenses', portable / 'licenses', dirs_exist_ok=True)
    shutil.copy2(source / 'docs' / 'BOTS.md', portable / 'BOTS.md')
    (portable / 'BOT_BENCHMARKS.md').write_text(benchmark_text, encoding='utf-8')
    (portable / 'BUILD-INFO.txt').write_text(build_info, encoding='utf-8')
    checkpoints = portable / 'benchmark-checkpoints'
    checkpoints.mkdir(exist_ok=True)
    for checkpoint in checkpoint_files:
        shutil.copy2(checkpoint, checkpoints / checkpoint.name)
    (portable / 'README.md').write_text(
        (source / 'README.md').read_text(encoding='utf-8')
        .replace('(docs/BOTS.md', '(BOTS.md')
        .replace('(docs/BOT_BENCHMARKS.md', '(BOT_BENCHMARKS.md'),
        encoding='utf-8')
    (portable / 'Play with bots.cmd').write_text(
        '@echo off\ncd /d "%~dp0"\n'
        'tremulous.exe +set fs_basepath "%CD%" +set fs_homepath "%CD%\\user" '
        '+set vm_game 2 +set sv_maxclients 16 +map atcs '
        '+bot fill humans 4 bell +bot fill aliens 4 bell\n', encoding='ascii')
    (portable / 'Dedicated bot server.cmd').write_text(
        '@echo off\ncd /d "%~dp0"\n'
        'tremded.exe +set fs_basepath "%CD%" +set fs_homepath "%CD%\\server-user" '
        '+set vm_game 2 +set dedicated 1 +set sv_master1 "" +set sv_master2 "" '
        '+set sv_maxclients 16 +map atcs +bot fill humans 4 bell +bot fill aliens 4 bell\n', encoding='ascii')
    (portable / 'Play 16 vs 16 bots.cmd').write_text(
        '@echo off\ncd /d "%~dp0"\n'
        'tremulous.exe +set fs_basepath "%CD%" +set fs_homepath "%CD%\\user" '
        '+set vm_game 2 +exec bots16.cfg\n', encoding='ascii')
    (portable / 'Dedicated 16 vs 16 bots.cmd').write_text(
        '@echo off\ncd /d "%~dp0"\n'
        'tremded.exe +set fs_basepath "%CD%" +set fs_homepath "%CD%\\server-user" '
        '+set vm_game 2 +set dedicated 1 +set sv_master1 "" +set sv_master2 "" '
        '+exec bots16.cfg\n',
        encoding='ascii')
    (portable / 'START-HERE.txt').write_text(
        'Tremulous with bots - Windows x64\n\n'
        'Run "Play with bots.cmd", then join a team with the usual game menu.\n'
        'For a larger match, run "Play 16 vs 16 bots.cmd" (32 bots, 40 client slots).\n'
        'It uses Arachnid2, coordinated teams, safer extra spawns and human aim delays.\n'
        'The larger launchers use a bell-shaped skill spread centered around 5-6.\n'
        'To watch, choose Spectators or enter "team spectator" in the game console.\n'
        '"follow" toggles a bot view; "follownext"/"followprev" change bots.\n'
        '"Dedicated 16 vs 16 bots.cmd" starts that match as a separate server.\n'
        'Open the console (~) to manage bots.\n\n'
        'exec bots16.cfg\nbot list\nbot tactics\n'
        'bot add humans 7 "Engineer" build\nbot remove all\n'
        'bot skill all 8\nbot role 1 defend\nbotnav status\n\n'
        'BOTS.md covers every command, building/equipment, navigation, and known limits.\n'
        'BOT_BENCHMARKS.md explains fast offline seeded parallel matches.\n'
        'This build includes all eight stock maps and the original freely licensed media.\n'
        'Keep the patched engine and game module together. Source archive is supplied separately.\n', encoding='utf-8')
    if checked_reports:
        reports = portable / 'validation'
        reports.mkdir(exist_ok=True)
        for name, summary in checked_reports:
            (reports / name).write_text(json.dumps(summary, indent=2) + '\n', encoding='utf-8')
    hashes = {str(file.relative_to(portable)).replace('\\', '/'):
              hashlib.sha256(file.read_bytes()).hexdigest()
              for file in package_files(portable) if file != portable / 'SHA256.json'}
    (portable / 'SHA256.json').write_text(json.dumps(hashes, indent=2), encoding='utf-8')
    playable_zip = output / (portable.name + '.zip')
    archive(portable, playable_zip)
    source_zip = output / 'tremulous-bots-source.zip'
    names = subprocess.check_output(['git', 'ls-files', '-z', '--cached', '--others', '--exclude-standard'],
                                    cwd=source).decode('utf-8').split('\0')
    with zipfile.ZipFile(source_zip, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as pak:
        for name in sorted(set(names)):
            file = source / name
            # A custom version output may not be gitignored. Keep generated
            # archives, runtime files and portable user homes out of source.
            if name and file.is_file() and output not in file.resolve().parents:
                pak.write(file, 'tremulous-bots-source/' + name)
    with zipfile.ZipFile(source_zip) as check:
        assert check.testzip() is None
    archive_checksums = []
    for file in (playable_zip, source_zip):
        digest = hashlib.sha256(file.read_bytes()).hexdigest()
        archive_checksums.append(f'{digest}  {file.name}')
        print(f'{file}: {file.stat().st_size:,} bytes; SHA256 {digest}')
    with (output / 'SHA256SUMS.txt').open('w', encoding='ascii', newline='\n') as checksums:
        checksums.write('\n'.join(archive_checksums) + '\n')


if __name__ == '__main__':
    main()
