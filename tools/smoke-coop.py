"""Two bounded, headless, muted game instances with isolated copies of smoke-test saves."""
import argparse
from datetime import datetime
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parent.parent
ARTIFACTS = Path(r'D:\project-artifacts\BloodborneSeamless')


def read_status(path):
    try:
        return json.loads(path.read_text(encoding='utf-8-sig'))
    except (OSError, ValueError):
        return {}


def joined(status):
    return status.get('state') == 'joined' and any(
        not member.get('local') and member.get('connected') and member.get('in_world')
        for member in status.get('members', []))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--label', required=True)
    parser.add_argument('--probe', type=Path)
    parser.add_argument('--seconds', type=int, default=240)
    parser.add_argument('--steps', default='')
    parser.add_argument('--require-boundary', action='store_true')
    parser.add_argument('--seed-host', default='host', help='Existing smoke-game save fixture name.')
    parser.add_argument('--seed-guest', default='guest', help='Existing smoke-game save fixture name.')
    args = parser.parse_args()
    run = f'coop-{args.label}-{datetime.now():%Y%m%d-%H%M%S}'
    artifacts = ARTIFACTS / run
    artifacts.mkdir(parents=True)
    code = artifacts / 'host-code.txt'
    children = []
    logs = []
    states = {}
    offsets = {}
    evidence = []
    env = dict(os.environ, BB_PARTY_AUTOCONTINUE='1', BB_PARTY_PERF='1', BB_PARTY_PERF_SECONDS='5',
               BB_PARTY_TEST_INVINCIBLE='1', BB_PARTY_DIRECTOR_TEST=args.steps)
    try:
        for role, seed, port in [('host', 'host', 47912), ('join', 'guest', 47913)]:
            name = f'{run}-{seed}'
            user = REPO / 'work' / f'smoke-{name}' / 'user'
            fixture_name = args.seed_host if role == 'host' else args.seed_guest
            source = REPO / 'work' / f'smoke-{fixture_name}' / 'user'
            if not (source / 'savedata_party').is_dir():
                raise RuntimeError(f'Create the isolated {seed} character with smoke-game.py first.')
            user.mkdir(parents=True)
            shutil.copytree(source / 'savedata_party', user / 'savedata_party')
            for fixture in ['party_prologue.txt', 'party_rot.txt']:
                if (source / fixture).exists():
                    shutil.copy2(source / fixture, user / fixture)
            states[seed] = user / 'party_status.json'
            native_log = ARTIFACTS / f'smoke-{name}' / 'session.log'
            offsets[seed] = native_log
            output = (artifacts / f'{seed}-harness.log').open('w', encoding='utf-8')
            logs.append(output)
            command = [sys.executable, '-u', str(REPO / 'tools/smoke-game.py'), '--name', name,
                       '--role', role, '--port', str(port), '--code-file', str(code), '--seconds', str(args.seconds)]
            if args.probe:
                command += ['--probe', str(args.probe.resolve())]
            child_env = dict(env, BB_PAD_FILE=str(artifacts / f'{seed}-pad.txt'))
            child = subprocess.Popen(command, env=child_env, cwd=REPO, stdout=output, stderr=output,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
            children.append(child)
            if role == 'host':
                deadline = time.monotonic() + 90
                while not code.exists() and child.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.5)
                if not code.exists():
                    raise RuntimeError('The host did not publish a connection code.')
        print(f'Two headless, muted instances running; evidence: {artifacts}', flush=True)
        was_joined = False
        stable_joined_at = None
        stable_seconds = 0
        boundary_at = None
        joined_after_boundary_at = None
        joined_after_boundary_seconds = 0
        positions = {seed: 0 for seed in offsets}
        deadline = time.monotonic() + args.seconds + 20
        while time.monotonic() < deadline and any(child.poll() is None for child in children):
            for seed, path in offsets.items():
                if path.exists():
                    with path.open('rb') as native:
                        native.seek(positions[seed])
                        chunk = native.read()
                        positions[seed] = native.tell()
                    if boundary_at is None and re.search(rb'K3: stage-record change with a session[^\n]*, [1-4] member chr', chunk):
                        boundary_at = time.monotonic()
                        print('Streaming stage boundary observed with the session active.', flush=True)
            snapshot = {seed: read_status(path) for seed, path in states.items()}
            both_joined = all(joined(status) for status in snapshot.values())
            if both_joined != was_joined:
                evidence.append({'time': datetime.now().isoformat(), 'joined': both_joined, **snapshot})
                print(f'Both instances in the same multiplayer session: {both_joined}', flush=True)
                was_joined = both_joined
            if both_joined:
                if stable_joined_at is None:
                    stable_joined_at = time.monotonic()
                stable_seconds = max(stable_seconds, time.monotonic() - stable_joined_at)
                if boundary_at is not None:
                    if joined_after_boundary_at is None:
                        joined_after_boundary_at = time.monotonic()
                    joined_after_boundary_seconds = max(joined_after_boundary_seconds, time.monotonic() - joined_after_boundary_at)
            else:
                stable_joined_at = None
                joined_after_boundary_at = None
            time.sleep(0.5)
        exits = [child.wait(timeout=15) for child in children]
        text = {seed: path.read_text(encoding='utf-8', errors='replace') for seed, path in offsets.items()}
        boundary = any(re.search(r'K3: stage-record change with a session[^\n]*, [1-4] member chr', log) for log in text.values())
        cleared = {seed: log.count('stale map hit cleared') for seed, log in text.items()}
        success = (all(code == 0 for code in exits) and stable_seconds >= 15 and
                   (not args.require_boundary or boundary and joined_after_boundary_seconds >= 15))
        result = {'passed': success, 'exits': exits, 'longest_continuous_join_seconds': stable_seconds,
                  'boundary_observed': boundary, 'stale_collision_clear_reports': cleared,
                  'longest_join_after_boundary_seconds': joined_after_boundary_seconds,
                  'logs': {seed: str(path) for seed, path in offsets.items()}, 'sessions': evidence}
        (artifacts / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        print(json.dumps({key: value for key, value in result.items() if key != 'sessions'}, indent=2), flush=True)
        return 0 if success else 1
    finally:
        for child in children:
            if child.poll() is None:
                subprocess.run(['taskkill', '/PID', str(child.pid), '/T', '/F'], capture_output=True)
                child.wait(timeout=15)
        for output in logs:
            output.close()


if __name__ == '__main__':
    sys.exit(main())
