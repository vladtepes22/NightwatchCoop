"""Bounded, headless, muted game instances (a host and one to four guests) with isolated copies of smoke-test saves."""
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
ARTIFACTS = Path(os.environ.get('BB_ARTIFACT_ROOT', r'D:\project-artifacts\BloodborneSeamless'))
if not ARTIFACTS.anchor or not Path(ARTIFACTS.anchor).exists():
    ARTIFACTS = REPO / 'work' / 'artifacts'  # as tools/smoke-game.py


def read_status(path):
    try:
        return json.loads(path.read_text(encoding='utf-8-sig'))
    except (OSError, ValueError):
        return {}


def joined(status, need=1):
    """In the session with at least `need` remote players connected and in the world."""
    return status.get('state') == 'joined' and sum(
        1 for member in status.get('members', [])
        if not member.get('local') and member.get('connected') and member.get('in_world')) >= need


# Roles: the host, then guest, guest2, guest3, guest4 (a party of up to five).
ROLES = ('host', 'guest', 'guest2', 'guest3', 'guest4')


def guest_roles(count):
    return ['guest'] + [f'guest{n}' for n in range(2, count + 1)]


def load_scenario(name):
    """A named row from tools/qa_scenarios.json: per-role director steps, the log lines each role must
    show (in order) and optional extra environment switches for both games."""
    scenarios = json.loads((REPO / 'tools' / 'qa_scenarios.json').read_text(encoding='utf-8'))
    if name not in scenarios:
        raise SystemExit(f'Unknown scenario {name}; known: {", ".join(sorted(scenarios))}')
    return scenarios[name]


def check_expects(expects, text):
    """Each (role, regex) must match that role's log after the previous match of the same role."""
    results, cursor = [], {}
    for role, pattern in expects:
        found = re.compile(pattern).search(text.get(role, ''), cursor.get(role, 0))
        if found:
            cursor[role] = found.end()
            line_start = text[role].rfind('\n', 0, found.start()) + 1
            line = text[role][line_start:text[role].find('\n', found.end())].strip()
        results.append({'role': role, 'expect': pattern, 'passed': bool(found), 'line': line[:200] if found else None,
                         'at': found.end() if found else None})
    return results


def check_forbids(forbids, text):
    """Each (role, regex) must match nowhere in that role's log ('any' = every log)."""
    results = []
    for role, pattern in forbids:
        logs = [log for seed, log in text.items() if role in ('any', seed)]
        found = next((m for m in (re.search(pattern, log) for log in logs) if m), None)
        line = None
        if found:
            log = found.string
            line = log[log.rfind('\n', 0, found.start()) + 1:log.find('\n', found.end())].strip()
        results.append({'role': role, 'forbid': pattern, 'passed': not found, 'line': line[:200] if line else None})
    return results


# The party's own story lines (state changes, director steps), without the per-packet net trace, the perf
# tables and the periodic dumps (B2 status, remote chrs, title polling) that make up most of a session log.
STORY_RX = re.compile(r'^\[coop\] Party(?! perf| B2: (?:status|remote chrs)| seamless: talk NPC| ?: \[[\d:.]+\] title: FROM)|^Boot: ')


def last_distinct(lines, count):
    """The last `count` distinct lines (time stamps ignored), in order, a repeated one marked (xN): periodic
    dumps such as the roster would otherwise fill the note."""
    seen, kept = {}, []
    for line in reversed(lines):
        key = re.sub(r'\[\d\d:\d\d:\d\d\.\d+\] ', '', line)
        if key in seen:
            seen[key] += 1
        elif len(kept) < count:
            seen[key] = 1
            kept.append(key)
    return [l + (f' (x{seen[l]})' if seen[l] > 1 else '') for l in reversed(kept)]


def failure_note(rows, text, crashes, joined_seconds, harness, ended='', tail=12):
    """A short note an agent reads instead of the session logs (each ~450 KB): per role, the first expect
    it never showed, the last line that did match, the party lines after that point and its crash lines."""
    note = [f'run ended: {ended}'] if ended else []
    if joined_seconds < 15:
        note.append(f'joined only {joined_seconds:.0f} s (needs 15)')
    for role, log in text.items():
        missing = next((r for r in rows if r['role'] == role and not r['passed']), None)
        crash = [l.strip()[:200] for l in log.splitlines() if re.search(CRASH_RX, l)]
        if not (missing or crash or joined_seconds < 15):
            continue
        note.append(f'== {role}' + (f' ({crashes.get(role, 0)} crash lines)' if crash else ''))
        if not log:
            note += ['  empty session log; harness output:'] + ['  ' + l[:200] for l in harness.get(role, '').splitlines()[-5:]]
            continue
        start = 0
        if missing:
            matched = [r for r in rows[:rows.index(missing)] if r['role'] == role and r['passed']]
            start = matched[-1]['at'] if matched else 0
            note += [f'  missing: {missing["expect"]}', f'  last match: {matched[-1]["line"] if matched else "(none)"}']
        story = [l.strip()[:200] for l in log[start:].splitlines() if STORY_RX.match(l)]
        note += [f'  party lines after it (last {tail} distinct of {len(story)}):'] + ['    ' + l for l in last_distinct(story, tail)]
        note += ['  crash: ' + l for l in crash[:5]]
    return note


# The engine's own fault / stop lines (probe.c, runtime_kernel.c), Vulkan errors and visy's common smoke
# negatives (smoke_party.py check_common: Session lost, boot stall); not the party's
# benign "host gone ... its crash restart" notice.
CRASH_RX = r'(?i)(?:guest|host) fault|uncaught C\+\+ exception|C\+\+ exception, thread|VK_ERROR|STOP: guest raised|assertion failed|EXCEPTION_ACCESS|fatal error|Session lost|\[WATCHDOG\] Boot stalled'
CRASH_BYTES_RX = re.compile(CRASH_RX.encode())


def read_logs(paths):
    return {seed: path.read_text(encoding='utf-8', errors='replace') if path.exists() else ''
            for seed, path in paths.items()}


def parse_expect(value):
    role, sep, pattern = value.partition(':')
    if not sep or role not in ROLES:
        raise argparse.ArgumentTypeError('--expect takes ROLE:REGEX, ROLE one of ' + ', '.join(ROLES))
    return role, pattern


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--label', required=True)
    parser.add_argument('--probe', type=Path)
    parser.add_argument('--seconds', type=int)
    parser.add_argument('--steps', default='', help='Director chain for both games (BB_PARTY_DIRECTOR_TEST).')
    parser.add_argument('--host-steps', help='Director chain for the host only (overrides --steps).')
    parser.add_argument('--guest-steps', help='Director chain for the guest only (overrides --steps).')
    parser.add_argument('--expect', type=parse_expect, action='append', default=[],
                        help='host:REGEX or guest:REGEX that role\'s log must show, in order; repeatable.')
    parser.add_argument('--scenario', help='A named row from tools/qa_scenarios.json (steps, expects, seconds).')
    parser.add_argument('--require-boundary', action='store_true')
    parser.add_argument('--seed-host', default='host', help='Existing smoke-game save fixture name.')
    parser.add_argument('--seed-guest', default='guest', help='Existing smoke-game save fixture name.')
    parser.add_argument('--game', help='Game folder (CUSA03173), passed to smoke-game.py.')
    parser.add_argument('--guests', type=int, choices=[1, 2, 3, 4],
                        help='Guests to run (default: the scenario\'s "guests", else 1); all copy --seed-guest.')
    args = parser.parse_args()
    scenario = load_scenario(args.scenario) if args.scenario else {}
    guests = guest_roles(args.guests or scenario.get('guests', 1))
    steps = {'host': args.host_steps if args.host_steps is not None else scenario.get('host', args.steps),
             'guest': args.guest_steps if args.guest_steps is not None else scenario.get('guest', args.steps)}
    for role in guests[1:]:  # extra guests: their own row key, else no director chain
        steps[role] = scenario.get(role, '')
    expects = [tuple(e) for e in scenario.get('expect', [])] + args.expect
    forbids = [tuple(e) for e in scenario.get('forbid', [])]
    no_join = scenario.get('no_join', False)  # the row proves a refusal: nobody may join
    args.seconds = args.seconds or scenario.get('seconds', 240)
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
               BB_PARTY_TEST_INVINCIBLE='1')
    env.update(scenario.get('env', {}))  # a row's own switches (e.g. a progress clear for a boss fight)
    try:
        players = [('host', 'host', 47912)] + [('join', seed, 47913 + i) for i, seed in enumerate(guests)]
        for role, seed, port in players:
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
                       '--role', role, '--port', str(port), '--code-file', str(code),
                       # This harness ends every game at once (its own deadline below); a game's own deadline is only
                       # a safety net. With it at args.seconds the host, started first, stopped first, and the guests'
                       # last lines read "the host left" as if the link had broken.
                       '--seconds', str(args.seconds + 180),
                       '--party-max', str(scenario.get('guest_party_max', len(players)) if role != 'host' else len(players)),
                       '--party-name', 'HostSmoke' if role == 'host' else 'GuestSmoke' + seed[len('guest'):]]
            if args.game:
                command += ['--game', args.game]
            if args.probe:
                command += ['--probe', str(args.probe.resolve())]
            child_env = dict(env, BB_PAD_FILE=str(artifacts / f'{seed}-pad.txt'), BB_PARTY_DIRECTOR_TEST=steps[seed])
            child_env.update(scenario.get(f'{seed}_env', {}))  # one game's own switches (visy's ienv)
            child = subprocess.Popen(command, env=child_env, cwd=REPO, stdout=output, stderr=output,
                                     creationflags=subprocess.CREATE_NO_WINDOW)
            children.append(child)
            if role == 'host':
                deadline = time.monotonic() + 90
                while not code.exists() and child.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.5)
                if not code.exists():
                    raise RuntimeError('The host did not publish a connection code.')
            elif seed != guests[-1]:
                # The next guest waits for this one's party tick: starts share engine/out (patches.bin).
                deadline = time.monotonic() + 90
                while not states[seed].exists() and child.poll() is None and time.monotonic() < deadline:
                    time.sleep(0.5)
        print(f'{len(players)} headless, muted instances running; evidence: {artifacts}', flush=True)
        was_joined = False
        stable_joined_at = None
        stable_seconds = 0
        boundary_at = None
        joined_after_boundary_at = None
        joined_after_boundary_seconds = 0
        positions = {seed: 0 for seed in offsets}
        stopped_early = False
        crash_at = None  # a crash line fails the row: stop a few seconds later instead of at the deadline
        next_expect_check = 0
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
                    if crash_at is None and CRASH_BYTES_RX.search(chunk):
                        crash_at = time.monotonic()
                        print(f'Crash line in the {seed} log: ending the run in 5 s.', flush=True)
            if crash_at is not None and time.monotonic() - crash_at >= 5:
                break
            snapshot = {seed: read_status(path) for seed, path in states.items()}
            # The host must see every guest; a guest needs the host (guest-to-guest shows in the logs).
            both_joined = all(joined(status, len(guests) if seed == 'host' else 1) for seed, status in snapshot.items())
            if both_joined != was_joined:
                evidence.append({'time': datetime.now().isoformat(), 'joined': both_joined, **snapshot})
                print(f'Every instance in the same multiplayer session: {both_joined}', flush=True)
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
            # A scenario ends as soon as every expected line is there (no idle time to the deadline).
            if expects and (stable_seconds >= 15 or no_join) and not args.require_boundary and time.monotonic() >= next_expect_check:
                next_expect_check = time.monotonic() + 5
                if all(r['passed'] for r in check_expects(expects, read_logs(offsets))):
                    print('Every expected line seen: ending the run.', flush=True)
                    stopped_early = True
                    break
            time.sleep(0.5)
        at_deadline = all(child.poll() is None for child in children)  # the run's end, not a game's exit
        if stopped_early or crash_at is not None or at_deadline:
            for child in children:
                subprocess.run(['taskkill', '/PID', str(child.pid), '/T', '/F'], capture_output=True)
        exits = [child.wait(timeout=15) for child in children]
        text = read_logs(offsets)
        boundary = any(re.search(r'K3: stage-record change with a session[^\n]*, [1-4] member chr', log) for log in text.values())
        cleared = {seed: log.count('stale map hit cleared') for seed, log in text.items()}
        rows = check_expects(expects, text)
        for row in rows:
            print(f"{'PASS' if row['passed'] else 'FAIL'} {row['role']}: {row['expect']}", flush=True)
        forbid_rows = check_forbids(forbids, text)
        for row in forbid_rows:
            print(f"{'PASS' if row['passed'] else 'FAIL'} {row['role']}: never {row['forbid']}"
                  + (f" (seen: {row['line']})" if row['line'] else ''), flush=True)
        crashes = {seed: len(re.findall(CRASH_RX, log))
                   for seed, log in text.items()}
        success = ((stopped_early or at_deadline or all(code == 0 for code in exits)) and (stable_seconds == 0 if no_join else stable_seconds >= 15) and
                   all(r['passed'] for r in rows + forbid_rows) and not any(crashes.values()) and
                   (not args.require_boundary or boundary and joined_after_boundary_seconds >= 15))
        result = {'passed': success, 'scenario': args.scenario, 'steps': steps, 'expects': rows, 'forbids': forbid_rows,
                  'crash_lines': crashes, 'stopped_early': stopped_early, 'stopped_on_crash': crash_at is not None,
                  'exits': exits, 'longest_continuous_join_seconds': stable_seconds,
                  'boundary_observed': boundary, 'stale_collision_clear_reports': cleared,
                  'longest_join_after_boundary_seconds': joined_after_boundary_seconds,
                  'logs': {seed: str(path) for seed, path in offsets.items()}, 'sessions': evidence}
        (artifacts / 'result.json').write_text(json.dumps(result, indent=2), encoding='utf-8')
        # One verdict line; the details stay in result.json, and a failure gets a short note (failure.txt).
        print(f"{'PASS' if success else 'FAIL'} {args.scenario or args.label}: rows {sum(r['passed'] for r in rows)}/{len(rows)}, "
              f"joined {stable_seconds:.0f} s, crash lines {sum(crashes.values())}, exits {exits}", flush=True)
        if not success:
            for log in logs:
                log.flush()
            harness = {seed: (artifacts / f'{seed}-harness.log').read_text(encoding='utf-8', errors='replace')
                       for seed in offsets}
            ended = ('a crash line (stopped 5 s later)' if crash_at is not None else
                     f'the {args.seconds} s deadline: the harness stopped every game, so a last link line is that stop'
                     if at_deadline else f'a game exited by itself (exits {exits})')
            note = failure_note(rows, text, crashes, stable_seconds, harness, ended)
            note += [f"{r['role']}: forbidden line seen: {r['line']}" for r in forbid_rows if not r['passed']]
            (artifacts / 'failure.txt').write_text('\n'.join(note) + '\n', encoding='utf-8', newline='\n')
            print('\n'.join(note), flush=True)
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
