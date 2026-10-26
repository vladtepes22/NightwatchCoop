"""Checks a visy port for completeness: every hunk visy changed in FROM..TO must be in our tree.

    python tools/visy-port-check.py <visy from> <visy to> [<our ref, default HEAD>]

visy's tree maps onto our engine/ (gpu/... -> engine/gpu/...). For each file visy changed in the range
(minus what we never take: tools/discord, QA_TESTING.md, CHANGES.md, README.md, launcher.py and its
tests), the file is either identical to visy's at TO, or the range's patch for it must reverse-apply
onto ours (all its hunks are there, wherever our own glue moved them; a little context fuzz allowed).
A file where that fails is listed with the rejected hunks' first added lines, for a look by hand:
often our own glue (ours kept on purpose, e.g. party rows in bbport_game_menu_pages.cpp instead of
bbport_overlay.cpp), sometimes a missed piece. Exit 1 when a file is missing or a hunk is rejected.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
SKIP_PREFIXES = ('tools/discord/', 'QA_TESTING.md', 'CHANGES.md', 'README.md', 'launcher.py',
                 'tests/test_launcher_', 'tests/test_netdoctor_bot', 'tools/party/flag_tool.py', 'VERSION')
# visy files that are not under his gpu/ src/ tests/ docs/ tools/ scripts/ map to nothing in ours.
MAPPED = ('gpu/', 'src/', 'tests/', 'docs/', 'tools/', 'scripts/')


def git(*args, check=True):
    out = subprocess.run(['git', '-C', str(REPO), *args], capture_output=True)
    if check and out.returncode:
        raise SystemExit(out.stderr.decode(errors='replace').strip())
    return out.stdout


def blob(ref, path):
    out = subprocess.run(['git', '-C', str(REPO), 'rev-parse', '-q', '--verify', f'{ref}:{path}'],
                         capture_output=True, text=True)
    return out.stdout.strip() or None


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    sys.stdout.reconfigure(errors='replace')
    base, tip = sys.argv[1], sys.argv[2]
    ours = sys.argv[3] if len(sys.argv) > 3 else 'HEAD'
    files = [f for f in git('diff', '--name-only', base, tip).decode().split()
             if f.startswith(MAPPED) and not f.startswith(SKIP_PREFIXES)]
    same = missing = 0
    problems = []
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for f in files:
            theirs, mine = blob(tip, f), blob(ours, 'engine/' + f)
            if theirs == mine:
                same += 1
                continue
            if theirs is None:  # visy deleted it
                if mine is not None:
                    problems.append((f, 'visy deleted it; still in ours', []))
                continue
            if mine is None:
                missing += 1
                problems.append((f, 'missing in ours', []))
                continue
            target = root / f
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(git('show', f'{ours}:engine/{f}').replace(b'\r', b''))
            patch = root / 'p.patch'
            patch.write_bytes(git('diff', base, tip, '--', f).replace(b'\r', b''))
            rej = target.with_name(target.name + '.rej')
            rej.unlink(missing_ok=True)
            subprocess.run(['git', 'apply', '--reverse', '-C1', '--reject', str(patch)], cwd=root, capture_output=True)
            if rej.exists():
                text = rej.read_text(encoding='utf-8', errors='replace').splitlines()
                added = [l[1:].strip() for l in text if l.startswith('+') and not l.startswith('+++') and l[1:].strip()]
                hunks = sum(l.startswith('@@') for l in text)
                problems.append((f, f'{hunks or 1} hunk(s) not found as visy wrote them', added[:3]))
    print(f'visy {base[:10]}..{tip[:10]} vs our {ours}: {len(files)} file(s), {same} identical, '
          f'{len(files) - same - len(problems)} with all hunks present, {len(problems)} to look at')
    for f, why, lines in problems:
        print(f'  {f}: {why}')
        for line in lines:
            print(f'      {line[:140]}')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
