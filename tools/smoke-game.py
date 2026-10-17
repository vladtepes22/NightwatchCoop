"""Bounded local game startup with isolated saves and all captures on the artifact drive."""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parent.parent


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--seconds', type=int, default=60)
    parser.add_argument('--name', default='host')
    parser.add_argument('--game', default=r'E:\games\bloodborne seamless\gamedir\CUSA03173')
    parser.add_argument('--new-game', action='store_true')
    parser.add_argument('--manual-title', action='store_true')
    parser.add_argument('--role', choices=['host', 'join'], default='host')
    parser.add_argument('--port', type=int, default=47892)
    parser.add_argument('--code-file', type=Path)
    parser.add_argument('--engine', type=Path, default=REPO / 'engine')
    parser.add_argument('--without-development-runtime', action='store_true')
    parser.add_argument('--max-party-tick-ms', type=float,
                        help='Require the last three in-world 5-second party tick p99 measurements below this limit.')
    args = parser.parse_args()
    engine = args.engine.resolve()
    if b'Headless: hidden, unfocusable surface' not in (engine / 'out/bb-probe.exe').read_bytes():
        raise RuntimeError('Build the engine with headless support before running this test.')
    data = REPO / 'work' / f'smoke-{args.name}'
    artifacts = Path(r'D:\project-artifacts\BloodborneSeamless') / f'smoke-{args.name}'
    artifacts.mkdir(parents=True, exist_ok=True)
    data.mkdir(parents=True, exist_ok=True)
    user = data / 'user'
    user.mkdir(exist_ok=True)
    config = data / 'bbport.ini'
    if not config.exists():
        config.write_text('upscaler=off\noutput_res=1280x720\nlive_resolution=0\nfullscreen=0\nskip_intro=1\nshow_fps=1\nparty_hud=1\n')
    env = dict(os.environ)
    env.update(BB_HEADLESS='1', BB_AUDIO='none', BB_PREBUILT='1', BB_DATA_DIR=str(data), BB_CONFIG=str(config), BB_USER_DIR=str(user),
               BB_GAME_DIR=args.game, BB_PROBE=str(engine / 'out/bb-probe.exe'),
               BB_PARTY=args.role, BB_PARTY_NAME='HostSmoke' if args.role=='host' else 'GuestSmoke',
               BB_PARTY_MAX='2', BB_PARTY_PORT=str(args.port),
               BB_PARTY_UPNP='0', BB_PARTY_STUN='off', BB_PARTY_LOOPBACK='1', BB_PARTY_LOCAL_IP='127.0.0.1',
               BB_PARTY_RESTART='0', BB_FULLSCREEN='0', BB_FPS='60', BB_GPU_CACHE_DIR=str(user / 'gpu-cache'),
               BB_LOG_FILE=str(artifacts / 'session.log'), BB_CAPTURE_DIR=str(artifacts), BB_DUMP_DIR=str(artifacts),
               BB_FINAL_DUMP_TRIGGER=str(artifacts / 'capture.trigger'), BB_LOG_CXX_EXCEPTIONS='1', PYTHONUTF8='1')
    if args.without_development_runtime:
        env['BB_MSYS2'] = str(data / 'no-development-runtime')
        env['PATH'] = os.path.join(os.environ.get('SystemRoot', r'C:\Windows'), 'System32')
    if args.max_party_tick_ms is not None:
        if args.max_party_tick_ms <= 0:
            parser.error('--max-party-tick-ms must be positive')
        env.update(BB_PARTY_PERF='1', BB_PARTY_PERF_SECONDS='5')
    if args.new_game:
        env.update(BB_TEST_NEWGAME='1', BB_TEST_NAME='Smoke', BB_IME_TEXT='Smoke', BB_TEST_PROLOGUE_SKIP='1')
    if args.manual_title:
        if args.new_game:
            parser.error('--manual-title cannot be combined with --new-game')
        env.update(BB_PARTY_AUTOCONTINUE='0', BB_PARTY_RESTARTED='0')
        (user / 'party_state.json').write_text(json.dumps({'role': args.role, 'secret': '0011223344556677', 'members': []}))
    if args.code_file:
        env['BB_PARTY_CODE_FILE'] = str(args.code_file.resolve())
    status_path = user / 'party_status.json'
    status_path.unlink(missing_ok=True)
    (user / 'party_command.json').unlink(missing_ok=True)
    session_log = artifacts / 'session.log'
    log_offset = session_log.stat().st_size if session_log.exists() else 0
    log = (artifacts / 'launcher.log').open('w', encoding='utf-8')
    python = engine / 'tools/python/python.exe'
    game = subprocess.Popen([str(python) if python.exists() else sys.executable, '-u', str(engine / 'scripts/run_windows.py')], env=env,
                            stdout=log, stderr=log, cwd=engine, creationflags=subprocess.CREATE_NO_WINDOW)
    print(f'Launcher PID {game.pid}; isolated user {user}; log {artifacts / "session.log"}', flush=True)
    ready = False
    try:
        end = time.monotonic() + args.seconds
        while game.poll() is None and time.monotonic() < end:
            if status_path.exists() and not ready:
                ready = True
                print('Party frame tick and launcher status channel are live.', flush=True)
                print('Headless, muted test: no display presentation or audio device.', flush=True)
            time.sleep(0.5)
        if game.poll() is None:
            (user / 'party_command.json').write_text(json.dumps({'action': 'quit'}))
            try:
                game.wait(10)
            except subprocess.TimeoutExpired:
                print('Test deadline: stopping this test process tree.', flush=True)
                subprocess.run(['taskkill', '/PID', str(game.pid), '/T', '/F'], capture_output=True)
                game.wait(10)
        print(f'Exit {game.returncode}; party tick seen: {ready}', flush=True)
        text = session_log.read_bytes()[log_offset:].decode('utf-8', 'replace')
        headless = 'Headless: hidden, unfocusable surface' in text
        muted = 'Runtime: audio backend timer (silent)' in text
        progressed = not args.new_game or (any(marker in text for marker in
            ['prologue skip: done; after the warp:', 'prologue skip: already past the prologue']) and 'GAVE UP' not in text)
        print(f'New-game progression verified: {progressed}' if args.new_game else 'Startup check complete.', flush=True)
        print(f'Silent audio backend verified: {muted}', flush=True)
        manual = not args.manual_title or ('restarted after an unclean exit' in text and 'auto-continue: Cross #' not in text)
        if args.manual_title:
            print(f'Stale recovery marker leaves title under manual control: {manual}', flush=True)
        performance = True
        if args.max_party_tick_ms is not None:
            reports = re.split(r'Party perf: [\d.]+ s,', text)[1:]
            measurements = []
            for report in reports[-3:]:
                tick = re.search(r'Party perf:\s+tick\s+per frame p50\s+([\d.]+) p99\s+([\d.]+)', report)
                world = 'Party perf:   b2.work ' in report and 'Party perf:   balance ' in report
                if tick and world:
                    measurements.append((float(tick[1]), float(tick[2])))
            performance = len(measurements) == 3 and all(p99 <= args.max_party_tick_ms for _, p99 in measurements)
            print(f'Party tick p50/p99 ms (last three in-world windows): {measurements}; '
                  f'p99 <= {args.max_party_tick_ms}: {performance}', flush=True)
        return 0 if headless and muted and ready and progressed and manual and performance and game.returncode == 0 else 1
    finally:
        log.close()


if __name__ == '__main__':
    sys.exit(main())
