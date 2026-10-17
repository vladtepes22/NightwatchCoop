"""Party process restarts and role changes for the native Windows launcher."""
import os
from pathlib import Path
import re
import time

from party_env import party_enabled, party_switch_on


def apply_relaunch(path, env):
    """Read the party flow's generated SET assignments without executing a shell."""
    path = Path(path)
    if not path.is_file():
        return False
    updates = {}
    for line in path.read_text(encoding='utf-8-sig').splitlines():
        line = line.strip()
        if not line or line.lower().startswith(('rem ', '@rem ', '@echo ')):
            continue
        match = re.fullmatch(r'set "([A-Z][A-Z0-9_]*)=([A-Za-z0-9_.:@-]*)"', line, re.I)
        if not match:
            raise ValueError('Invalid party relaunch settings; no commands were executed')
        key, value = match.groups()
        if not key.startswith(('BB_PARTY', 'BB_SKIP_NETWORK', 'BB_SAVE_', 'BB_AUTO_CONTINUE')):
            raise ValueError(f'Unexpected party relaunch setting: {key}')
        updates[key] = value
    for key, value in updates.items():
        if value:
            env[key] = value
        else:
            env.pop(key, None)
    path.unlink()
    return True


def run_loop(start, env=os.environ, sleep=time.sleep, clock=time.monotonic):
    """Keep the launcher attached across role changes and bounded crash recovery."""
    env['BB_RELAUNCH_LOOP'] = '1'
    attempts, window = 0, clock()
    while True:
        status = start()
        if status == 75:
            user = Path(env.get('BB_USER_DIR', Path(env.get('BB_DATA_DIR', '.')) / 'user'))
            apply_relaunch(user / 'party_relaunch.bat', env)
            env.pop('BB_PARTY_RESTARTED', None)
            continue
        if not status or not party_enabled(env) or not party_switch_on(env, 'BB_PARTY_RESTART'):
            return status
        if clock() - window > 300:
            attempts, window = 0, clock()
        attempts += 1
        limit = int(env.get('BB_PARTY_RESTART_MAX', '3'))
        if attempts > limit:
            print(f'Party: stopped after {limit} crash restarts in five minutes', flush=True)
            return status
        print(f'Party: game exited with {status}; restart {attempts}/{limit} to rejoin', flush=True)
        env['BB_PARTY_RESTARTED'] = '1'
        sleep(1)
