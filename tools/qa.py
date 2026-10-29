"""Nightwatch QA gate: runs the test layers a change needs and prints one PASS / FAIL table.

    python tools/qa.py plan  --level change            # what a change needs, from the touched files
    python tools/qa.py run   --level change            # ... and run it
    python tools/qa.py run   --level merge             # native + Python + launcher checks
    python tools/qa.py run   --level release --package <dir>   # + full build, smoke, co-op rows, privacy
    python tools/qa.py run   --only native,launcher    # named layers

Layers (QA_TESTING.md has the why of each):
  build     cmake out/gpu + build.sh (LTO default; re-hashes the party build ID)
  native    ninja seamless-tests + ctest -j8
  python    engine Python unittests (CLANG64 Python 3.12)
  launcher  dotnet SeamlessTests, LocalizationChecks, DisplayChecks
  smoke     tools/smoke-game.py: one headless, muted new game (party tick, new-game progression, no crash line)
  coop      tools/smoke-coop.py rows from tools/qa_scenarios.json (host + guest, headless, isolated saves)
  privacy   tools/privacy-scan.py on the package + no game-derived files in it

Every run writes work/qa/<run>/report.md, report.json and one log per layer. Exit 1 when a layer fails.
Native builds run from a subst drive mapped to this tree (exes built under C:\\Users embed the user name);
`release` refuses to build without one.
"""
import argparse
from concurrent.futures import ThreadPoolExecutor
from datetime import datetime
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parent.parent
BASH = Path(r'C:\msys64\usr\bin\bash.exe')
PYTHON = Path(os.environ.get('BB_QA_PYTHON', r'C:\msys64\clang64\bin\python.exe'))
LAYERS = ['build', 'native', 'python', 'launcher', 'smoke', 'coop', 'privacy']
LEVELS = {
    'merge': ['native', 'python', 'launcher'],
    'release': ['build', 'native', 'python', 'launcher', 'smoke', 'coop', 'privacy'],
}
RELEASE_SCENARIOS = ['join', 'core', 'deaths', 'boss', 'guestrest', 'loadgap']
# Touched path -> layers; the first matching rule wins (a party-only change needs the co-op rows, not the solo
# smoke). The order of LAYERS decides the run order.
RULES = [
    (r'^engine/gpu/shim/(party|net)/', ['native', 'coop']),
    (r'^engine/(tests/.*\.(c|cc|cpp|h)$|gpu/cmake/)', ['native']),
    (r'^engine/(gpu|src|start)/', ['native', 'smoke']),
    (r'^engine/(scripts/|tools/|tests/.*\.py$)', ['python']),
    (r'^launcher/', ['launcher']),
    (r'^tools/(package-seamless|release)\.ps1$', ['smoke']),
    (r'^tools/(smoke-|qa)', ['python']),
]
# Engine fault / stop lines, Vulkan errors and visy's common smoke negatives (Session lost, boot stall),
# as tools/smoke-coop.py.
CRASH_RX = r'(?i)(?:guest|host) fault|uncaught C\+\+ exception|C\+\+ exception, thread|VK_ERROR|STOP: guest raised|assertion failed|EXCEPTION_ACCESS|fatal error|Session lost|\[WATCHDOG\] Boot stalled'
GAME_FILES = ('eboot.elf', 'boot-linked.bin')


def parse_ctest(out):
    """(summary found, text) from ctest output: "100% tests passed out of 65", or "96% tests passed,
    2 tests failed out of 65" followed by the failed tests' list."""
    m = re.search(r'\d+% tests passed(?:, (\d+) tests? failed)? out of (\d+)', out)
    if not m:
        return False, 'no ctest summary'
    total, bad = int(m.group(2)), int(m.group(1) or 0)
    failed = re.findall(r'^\s*\d+ - (\S+) \(', out, re.M)
    return True, f'ctest {total - bad}/{total}' + (': ' + ', '.join(failed[:5]) if bad else '')


def git(*args):
    return subprocess.run(['git', '-C', str(REPO), *args], capture_output=True, text=True).stdout


def touched(base):
    names = set(git('diff', '--name-only', f'{base}...HEAD').split())
    names |= set(git('diff', '--name-only', 'HEAD').split())
    names |= set(git('ls-files', '--others', '--exclude-standard').split())
    return sorted(names)


def layers_for(paths):
    wanted = set()
    for path in paths:
        for pattern, layers in RULES:
            if re.search(pattern, path):
                wanted.update(layers)
                break
    return [layer for layer in LAYERS if layer in wanted]


def subst_drive():
    """The drive letter subst maps to this tree, e.g. 'N:' (None when there is none)."""
    out = subprocess.run(['cmd', '/c', 'subst'], capture_output=True, text=True).stdout
    for line in out.splitlines():
        m = re.match(r'^([A-Z]:)\\: => (.+)$', line.strip())
        if m and (REPO.drive.upper() == m.group(1) or Path(m.group(2)).resolve() == REPO.resolve()):
            return m.group(1)
    return None


def artifact_root():
    root = Path(os.environ.get('BB_ARTIFACT_ROOT', r'D:\project-artifacts\BloodborneSeamless'))
    return root if root.anchor and Path(root.anchor).exists() else REPO / 'work' / 'artifacts'


class Run:
    def __init__(self, args):
        self.args = args
        self.name = f'{datetime.now():%Y%m%d-%H%M%S}-{args.label or args.level or "only"}'
        self.dir = REPO / 'work' / 'qa' / self.name
        self.drive = subst_drive()
        self.results = {}

    def tree(self):
        """This tree as the MSYS2 shell sees it, through the subst drive when there is one."""
        if self.drive:
            return '/' + self.drive[0].lower()
        p = REPO.resolve().as_posix()
        return '/' + p[0].lower() + p[2:]

    def sh(self, layer, command, cwd=None, env=None, shell_bash=False, timeout=None):
        """Runs one step, appending its output to the layer's log; returns (exit code, output)."""
        log = self.dir / f'{layer}.log'
        full_env = dict(os.environ, **(env or {}))
        if shell_bash:
            full_env.update(MSYSTEM='CLANG64', CHERE_INVOKING='1')
            command = [str(BASH), '-lc', command]
        started = time.monotonic()
        try:
            proc = subprocess.run(command, cwd=cwd or REPO, env=full_env, capture_output=True, text=True,
                                  encoding='utf-8', errors='replace', timeout=timeout)
            code, out = proc.returncode, proc.stdout + proc.stderr
        except subprocess.TimeoutExpired as e:
            code, out = -1, f'{e.stdout or ""}{e.stderr or ""}\nTIMEOUT after {timeout} s'
        with log.open('a', encoding='utf-8') as f:
            f.write(f'$ {command if isinstance(command, str) else " ".join(map(str, command))}\n'
                    f'(exit {code}, {time.monotonic() - started:.0f} s)\n{out}\n')
        return code, out

    def done(self, layer, passed, summary, started):
        self.results[layer] = {'status': 'PASS' if passed else 'FAIL', 'summary': summary,
                               'seconds': round(time.monotonic() - started)}
        print(f"  {self.results[layer]['status']} {layer}: {summary}", flush=True)
        return passed

    # Layers ---------------------------------------------------------------------------------------
    def build(self):
        t = time.monotonic()
        if not self.drive:
            return self.done('build', False, f'no subst drive maps to {REPO.name}: cmd /c "subst N: <tree>" first', t)
        jobs = os.environ.get('BB_JOBS', str(max(2, (os.cpu_count() or 4) - 2)))
        code, out = self.sh('build', f'cd {self.tree()}/engine && cmake out/gpu >/dev/null && BB_JOBS={jobs} bash build.sh',
                            shell_bash=True)
        tail = [l for l in out.splitlines() if 'error' in l.lower()][:1]
        return self.done('build', code == 0, 'build.sh exit 0' if code == 0 else f'exit {code}: {tail[0][:120] if tail else "see build.log"}', t)

    def native(self):
        t = time.monotonic()
        code, out = self.sh('native', f'cd {self.tree()}/engine && ninja -C out/gpu seamless-tests', shell_bash=True)
        if code != 0:
            err = next((l for l in out.splitlines() if 'error:' in l), 'see native.log')
            return self.done('native', False, f'test build failed: {err[:140]}', t)
        code, out = self.sh('native', f'cd {self.tree()}/engine/out/gpu && ctest -j8 --output-on-failure', shell_bash=True)
        found, summary = parse_ctest(out)
        return self.done('native', code == 0 and found, summary, t)

    def python(self):
        t = time.monotonic()
        code, out = self.sh('python', [str(PYTHON), '-m', 'unittest', 'discover', '-s', 'tests'], cwd=REPO / 'engine')
        ran = re.search(r'Ran (\d+) tests?', out)
        verdict = re.findall(r'^(OK.*|FAILED.*)$', out, re.M)
        return self.done('python', code == 0, f'{ran.group(1) if ran else "?"} tests, {verdict[-1] if verdict else "no verdict"}', t)

    def launcher(self):
        t = time.monotonic()
        base = REPO / 'launcher' / 'windows'
        parts, ok = [], True
        for project in ['SeamlessTests', 'tests/LocalizationChecks', 'tests/DisplayChecks']:
            code, out = self.sh('launcher', ['dotnet', 'run', '--project', project], cwd=base)
            ok &= code == 0
            zh = re.search(r'zh-CN (\d+/\d+)', out)
            parts.append(f"{project.split('/')[-1]} {'ok' if code == 0 else f'exit {code}'}{f' (zh-CN {zh.group(1)})' if zh else ''}")
        fw_ok, fw = self.firewall(base)
        ok &= fw_ok
        parts.append(fw)
        return self.done('launcher', ok, ', '.join(parts), t)

    def firewall(self, base):
        """Fix firewall issues on the real firewall: a block rule for a dummy exe goes, the allow rules come.
        Needs an elevated shell; otherwise a skip with a note (not a failure)."""
        exe, port = r'C:\NightwatchQA\firewall-check\bb-probe.exe', '47977'
        code, out = self.sh('launcher', ['dotnet', 'run', '--project', 'SeamlessTests', '--', '--firewall-script', port, exe], cwd=base)
        if code != 0:
            return False, f'firewall script exit {code}'
        script = self.dir / 'firewall-script.ps1'
        lines = out.splitlines()
        first = next((i for i, l in enumerate(lines) if l.startswith('Get-NetFirewallApplicationFilter')), None)
        if first is None:
            return False, 'firewall script: no unblock line first'
        script.write_text('\n'.join(lines[first:first + 4]) + '\n', encoding='utf-8')
        code, out = self.sh('launcher', ['powershell', '-NoProfile', '-ExecutionPolicy', 'Bypass', '-File',
                                         str(REPO / 'tools' / 'firewall-block-check.ps1'),
                                         '-ScriptFile', str(script), '-Exe', exe, '-Port', port])
        line = (out.strip().splitlines() or ['no output'])[-1]
        if code == 2:
            return True, 'firewall block-rule check SKIPPED (not elevated)'
        return code == 0, f"firewall block-rule check {'ok' if code == 0 else 'FAILED: ' + line[:100]}"

    def smoke(self):
        t = time.monotonic()
        name = f'qa-{self.name}'
        code, out = self.sh('smoke', [str(PYTHON), str(REPO / 'tools' / 'smoke-game.py'), '--seconds', str(self.args.smoke_seconds),
                                      '--name', name, '--new-game', '--game', self.args.game], env=self.game_env())
        tick = 'party tick seen: True' in out
        progressed = 'New-game progression verified: True' in out
        log = artifact_root() / f'smoke-{name}' / 'session.log'
        crashes = len(re.findall(CRASH_RX, log.read_text(encoding='utf-8', errors='replace'))) if log.exists() else -1
        summary = f'tick {tick}, new-game progression {progressed}, crash lines {crashes} ({self.args.smoke_seconds} s)'
        return self.done('smoke', code == 0 and tick and progressed and crashes == 0, summary, t)

    def coop(self):
        t = time.monotonic()
        parts, ok = [], True
        for scenario in self.scenarios:
            code, out = self.sh('coop', [str(PYTHON), str(REPO / 'tools' / 'smoke-coop.py'), '--label', f'qa-{scenario}',
                                         '--scenario', scenario, '--game', self.args.game], env=self.game_env())
            m = re.search(r'evidence: (.+)$', out, re.M)
            result = {}
            if m and (Path(m.group(1).strip()) / 'result.json').exists():
                result = json.loads((Path(m.group(1).strip()) / 'result.json').read_text(encoding='utf-8'))
            rows = result.get('expects', [])
            rows += [dict(r, expect='never ' + r['forbid']) for r in result.get('forbids', [])]
            passed = code == 0 and result.get('passed') is True
            ok &= passed
            miss = next((r['role'] + ': ' + r['expect'] for r in rows if not r['passed']), '')
            parts.append(f"{scenario} {sum(r['passed'] for r in rows)}/{len(rows)} rows, "
                         f"joined {result.get('longest_continuous_join_seconds', 0):.0f} s" + (f' [missing {miss[:80]}]' if miss else '')
                         + ('' if passed else ' FAIL'))
        return self.done('coop', ok, '; '.join(parts), t)

    def privacy(self):
        t = time.monotonic()
        package = self.args.package
        if not package:
            return self.done('privacy', False, 'needs --package <package dir>', t)
        # The patterns (user name, friends' names, addresses) live outside the repo, as for tools/release.ps1.
        patterns_file = Path(os.environ.get('USERPROFILE', '~')) / '.nightwatch-privacy-patterns.txt'
        if not patterns_file.exists():
            return self.done('privacy', False, f'no patterns file {patterns_file.name} in the user profile', t)
        patterns = [p.strip() for p in patterns_file.read_text(encoding='utf-8-sig').splitlines() if p.strip()]
        code, out = self.sh('privacy', [str(PYTHON), str(REPO / 'tools' / 'privacy-scan.py'), str(package), *patterns])
        game = [str(p.relative_to(package)) for name in GAME_FILES for p in Path(package).rglob(name)]
        saves = [str(p.relative_to(package)) for p in Path(package).rglob('SPRJ0005')]
        bad = game + saves
        summary = f"scan exit {code}: {out.strip().splitlines()[0][:100] if out.strip() else 'no output'}"
        if bad:
            summary += f'; game-derived files: {", ".join(bad[:3])}'
        return self.done('privacy', code == 0 and not bad, summary, t)

    def game_env(self):
        return {'PATH': r'C:\msys64\clang64\bin;' + os.environ.get('PATH', ''), 'BB_ARTIFACT_ROOT': str(artifact_root())}

    # Driver ---------------------------------------------------------------------------------------
    def execute(self, layers):
        self.dir.mkdir(parents=True, exist_ok=True)
        head = git('log', '-1', '--format=%h %s').strip()
        print(f'QA run {self.name} on {head[:70]} (drive {self.drive or "none"}): {", ".join(layers)}', flush=True)
        started = time.monotonic()
        # The build comes first (smoke needs its exes); the checks that need no game run side by side;
        # games run one after another (a host and a guest already make two).
        if 'build' in layers and not self.build():
            layers = [l for l in layers if l in ('python', 'launcher')]
        side = [l for l in layers if l in ('native', 'python', 'launcher')]
        with ThreadPoolExecutor(max_workers=3) as pool:
            ok = dict(zip(side, pool.map(lambda l: getattr(self, l)(), side)))
        for layer in [l for l in layers if l in ('smoke', 'coop', 'privacy')]:
            if layer in ('smoke', 'coop') and ok.get('native') is False:
                self.results[layer] = {'status': 'SKIP', 'summary': 'native failed', 'seconds': 0}
                print(f'  SKIP {layer}: native failed', flush=True)
                continue
            getattr(self, layer)()
        self.report(layers, head, time.monotonic() - started)
        return all(r['status'] == 'PASS' for r in self.results.values())

    def report(self, layers, head, seconds):
        verdict = 'PASS' if all(r['status'] == 'PASS' for r in self.results.values()) else 'FAIL'
        data = {'run': self.name, 'commit': head, 'level': self.args.level, 'layers': layers, 'verdict': verdict,
                'seconds': round(seconds), 'results': self.results,
                'dirty': bool(git('status', '--porcelain', '--untracked-files=no').strip())}
        (self.dir / 'report.json').write_text(json.dumps(data, indent=2), encoding='utf-8')
        lines = [f'# QA {self.name}: {verdict}', '', f'Commit `{head}`' + (' (uncommitted changes)' if data['dirty'] else ''),
                 f'Level {self.args.level or "-"}, {round(seconds / 60)} min', '', '| Layer | Result | Time | Summary |', '|---|---|---|---|']
        for layer in layers:
            r = self.results.get(layer, {'status': 'SKIP', 'summary': '', 'seconds': 0})
            lines.append(f"| {layer} | {r['status']} | {r['seconds']} s | {r['summary']} |")
        lines += ['', 'Not covered by these layers: anything a player sees (headless runs draw nothing), real networks,',
                  'more than two players, long sessions. Those go on the visible / playtest checklist.']
        (self.dir / 'report.md').write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
        print(f'{verdict}: {self.dir / "report.md"}', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('action', choices=['plan', 'run'])
    parser.add_argument('--level', choices=['change', 'merge', 'release'])
    parser.add_argument('--only', help='Comma-separated layers instead of a level.')
    parser.add_argument('--base', default='fork/main', help='change level: diff base (default fork/main).')
    parser.add_argument('--scenario', help='coop rows to run (comma-separated; default join for a change, every release row for a release).')
    parser.add_argument('--package', type=Path, help='Package folder for the privacy layer.')
    parser.add_argument('--game', default=os.environ.get('BB_QA_GAME', r'C:\Games\CUSA03173'))
    parser.add_argument('--smoke-seconds', type=int, default=240)
    parser.add_argument('--label', help='Name part of the run folder.')
    args = parser.parse_args()
    if not args.level and not args.only:
        parser.error('give --level or --only')
    if args.only:
        layers = [l for l in LAYERS if l in args.only.split(',')]
        reason = 'named layers'
    elif args.level == 'change':
        paths = touched(args.base)
        layers = layers_for(paths)
        reason = f'{len(paths)} touched file(s) since {args.base}'
    else:
        layers = LEVELS[args.level]
        reason = f'{args.level} level'
    run = Run(args)
    run.scenarios = (args.scenario.split(',') if args.scenario else
                     RELEASE_SCENARIOS if args.level == 'release' else ['join'])
    print(f'Layers ({reason}): {", ".join(layers) or "none"}' + (f'; co-op rows: {", ".join(run.scenarios)}' if 'coop' in layers else ''))
    if args.action == 'plan' or not layers:
        if args.level == 'change' and args.action == 'plan':
            for path in touched(args.base)[:40]:
                print(f'  {path} -> {", ".join(layers_for([path])) or "-"}')
        return 0
    if args.level == 'release' and not run.drive:
        print('release level: map a subst drive to this tree first (exes built under C:\\Users embed the user name)')
        return 1
    return 0 if run.execute(layers) else 1


if __name__ == '__main__':
    sys.exit(main())
