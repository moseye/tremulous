"""Package compiled Tremulous bots, unchanged upstream media, and patched source.

Example: python misc/package_bots.py --build build-bots-msvc/Release
  --data build-bots-msvc/data --output dist-bots
"""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess
import zipfile


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
    manifest = json.loads((data / 'upstream-manifest.json').read_text(encoding='utf-8-sig'))
    media = list(data.glob('*.pk3'))
    if len(media) != 10:
        raise ValueError('expected two data archives and all eight stock maps')
    for file in media:
        upstream = next(item for item in manifest if item['name'] == file.name)
        content = file.read_bytes()
        git_hash = hashlib.sha1(f'blob {len(content)}\0'.encode() + content).hexdigest()
        if len(content) != upstream['size'] or git_hash != upstream['sha']:
            raise ValueError(f'upstream media integrity failure: {file}')
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    portable = output / 'tremulous-bots-windows-x64'
    portable.mkdir(exist_ok=True)
    (portable / 'base').mkdir(exist_ok=True)
    for name in ('tremulous.exe', 'tremded.exe', 'SDL2.dll', 'renderer_opengl1.dll', 'renderer_opengl2.dll'):
        shutil.copy2(build / name, portable / name)
    if args.vc_runtime:
        if args.vc_runtime.name.lower() != 'vcruntime140.dll':
            raise ValueError('expected the x64 Visual C++ vcruntime140.dll')
        shutil.copy2(args.vc_runtime, portable / 'vcruntime140.dll')
        (portable / 'MSVC-RUNTIME-NOTICE.txt').write_text(
            'This package includes the unmodified Microsoft Visual C++ x64 runtime '
            'vcruntime140.dll, supplied with the build compiler.\n'
            'Copyright Microsoft Corporation. All rights reserved.\n'
            'The Windows Universal C Runtime is supplied by Windows 10/11.\n'
            'Microsoft Visual C++ Redistributable information and downloads:\n'
            'https://learn.microsoft.com/en-us/cpp/windows/latest-supported-vc-redist\n',
            encoding='utf-8')
    for name in ('game.dll', 'cgame.dll', 'ui.dll'):
        shutil.copy2(build / 'base' / name, portable / 'base' / name)
    for file in media:
        shutil.copy2(file, portable / 'base' / file.name)
    # Keep upstream archives untouched. Our configs/UI/modules live in a new PK3.
    with zipfile.ZipFile(portable / 'base' / 'zz-tremulous-bots.pk3', 'w', zipfile.ZIP_DEFLATED) as pak:
        for file in sorted((source / 'assets').rglob('*')):
            if file.is_file():
                pak.write(file, file.relative_to(source / 'assets').as_posix())
        for file in sorted((build / 'base' / 'vm').glob('*.qvm')):
            pak.write(file, f'vm/{file.name}')
    for name in ('COPYING.txt', 'GPL', 'CC'):
        shutil.copy2(source / name, portable / name)
    for name in ('COPYING', 'CREDITS.md', 'sha256.txt', 'upstream-manifest.json'):
        if (data / name).exists():
            shutil.copy2(data / name, portable / ('UPSTREAM-' + name))
    # The bundled SDL distribution carries its zlib notice in the public headers.
    sdl_headers = list((source / 'code' / 'thirdparty').glob('SDL2-*/include/SDL.h'))
    if len(sdl_headers) != 1:
        raise ValueError('cannot identify the bundled SDL license header')
    sdl_notice = sdl_headers[0].read_text(encoding='utf-8').split('*/', 1)[0] + '*/\n'
    (portable / 'SDL2-LICENSE.txt').write_text(sdl_notice, encoding='utf-8')
    shutil.copytree(source / 'docs' / 'licenses', portable / 'licenses', dirs_exist_ok=True)
    shutil.copy2(source / 'docs' / 'BOTS.md', portable / 'BOTS.md')
    (portable / 'README.md').write_text(
        (source / 'README.md').read_text(encoding='utf-8').replace('(docs/BOTS.md', '(BOTS.md'),
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
        '+set vm_game 2 +set sv_maxclients 40 +map atcs '
        '+bot fill humans 16 bell +bot fill aliens 16 bell\n', encoding='ascii')
    (portable / 'Dedicated 16 vs 16 bots.cmd').write_text(
        '@echo off\ncd /d "%~dp0"\n'
        'tremded.exe +set fs_basepath "%CD%" +set fs_homepath "%CD%\\server-user" '
        '+set vm_game 2 +set dedicated 1 +set sv_master1 "" +set sv_master2 "" '
        '+set sv_maxclients 40 +map atcs +bot fill humans 16 bell +bot fill aliens 16 bell\n',
        encoding='ascii')
    (portable / 'START-HERE.txt').write_text(
        'Tremulous with bots - Windows x64\n\n'
        'Run "Play with bots.cmd", then join a team with the usual game menu.\n'
        'For a larger match, run "Play 16 vs 16 bots.cmd" (32 bots, 40 client slots).\n'
        'The larger launchers use a bell-shaped skill spread centered around 5-6.\n'
        'To watch, choose Spectators or enter "team spectator" in the game console.\n'
        '"follow" toggles a bot view; "follownext"/"followprev" change bots.\n'
        '"Dedicated 16 vs 16 bots.cmd" starts that match as a separate server.\n'
        'Open the console (~) to manage bots.\n\n'
        'set sv_maxclients 40\nmap atcs\n'
        'bot fill humans 16 bell\nbot fill aliens 16 bell\nbot list\n'
        'bot add humans 7 "Engineer" build\nbot remove all\n'
        'bot skill all 8\nbot role 1 defend\nbotnav status\n\n'
        'BOTS.md covers every command, building/equipment, navigation, and known limits.\n'
        'This build includes all eight stock maps and the original freely licensed media.\n'
        'Keep the patched engine and game module together. Source archive is supplied separately.\n', encoding='utf-8')
    if args.report:
        reports = portable / 'validation'
        reports.mkdir(exist_ok=True)
        for file in args.report:
            report = json.loads(file.read_text(encoding='utf-8'))
            if not report.get('ok'):
                raise ValueError(f'cannot package a failed validation report: {file}')
            suffix = '-economy' if report.get('economy') else ''
            if report.get('bots_per_team', 4) != 4:
                suffix += f'-{report["bots_per_team"]}v{report["bots_per_team"]}'
            if report.get('bell_skills'):
                suffix += '-bell'
            name = f'vm{report["vm"]}-{report["map"]}{suffix}.json' if 'vm' in report else 'client-startup.json'
            shutil.copy2(file, reports / name)
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
            if name and (source / name).is_file():
                pak.write(source / name, 'tremulous-bots-source/' + name)
    with zipfile.ZipFile(source_zip) as check:
        assert check.testzip() is None
    for file in (playable_zip, source_zip):
        print(f'{file}: {file.stat().st_size:,} bytes; SHA256 {hashlib.sha256(file.read_bytes()).hexdigest()}')


if __name__ == '__main__':
    main()
