"""Uploads one file to Gofile anonymously (pre-releases for testers) and prints its download page.

    python tools/gofile-upload.py <file>

Tries the global upload endpoint, then the per-server one from the server list (Gofile has moved its
upload host before). Uses curl.exe (ships with Windows) so a 300 MB zip streams instead of loading into
memory. Prints `downloadPage: <url>` and writes the full JSON reply beside the file as
<file>.gofile.json (it holds the guest token that can manage the upload). Exit 1 when every endpoint fails.
"""
import json
import subprocess
import sys
from pathlib import Path


def curl(*args):
    out = subprocess.run(['curl.exe', '-sS', '--fail-with-body', *args], capture_output=True, text=True)
    try:
        return json.loads(out.stdout)
    except ValueError:
        return {'status': 'error', 'curl': out.returncode, 'body': (out.stdout or out.stderr)[:300]}


def endpoints():
    yield 'https://upload.gofile.io/uploadfile'
    servers = curl('https://api.gofile.io/servers')
    for server in (servers.get('data') or {}).get('servers', [])[:2]:
        yield f"https://{server['name']}.gofile.io/contents/uploadfile"


def main():
    if len(sys.argv) != 2 or not Path(sys.argv[1]).is_file():
        print(__doc__)
        return 2
    path = Path(sys.argv[1]).resolve()
    print(f'Uploading {path.name} ({path.stat().st_size / 2**20:.0f} MiB)', flush=True)
    for url in endpoints():
        reply = curl('-F', f'file=@{path}', url)
        if reply.get('status') == 'ok' and (reply.get('data') or {}).get('downloadPage'):
            path.with_name(path.name + '.gofile.json').write_text(json.dumps(reply, indent=2), encoding='utf-8')
            print(f"downloadPage: {reply['data']['downloadPage']}")
            return 0
        print(f'{url}: {json.dumps(reply)[:200]}', flush=True)
    return 1


if __name__ == '__main__':
    sys.exit(main())
