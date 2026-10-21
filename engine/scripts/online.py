# SPDX-License-Identifier: GPL-2.0-or-later
"""The online profile: always offline in Nightwatch Co-op (no shadNet, no The Hunter's Dream).
Co-op goes through the party's own network layer (party_env)."""
from pathlib import Path

ONLINE_ENV = ('BB_ONLINE', 'BB_SHADNET_SERVER', 'BB_SHADNET_WEBAPI', 'BB_SHADNET_NPID',
              'BB_SHADNET_PASSWORD', 'BB_SHADNET_TOKEN', 'BB_UPNP', 'BB_P2P_PORT',
              'SHADPS4_HTTP_HOST_OVERRIDES_JSON', 'BB_SIGN_TIMEOUT', 'BB_NET_BACKEND', 'BB_THD_SERVER',
              'BB_THD_AUTH', 'BB_THD_TOKEN', 'BB_THD_ONLINE_ID', 'BB_THD_RULESET')

def read_settings(path):
    settings = {}
    for line in Path(path).read_text(encoding='utf-8-sig').splitlines():
        if line.lstrip().startswith(('#', ';')):
            continue
        key, sep, value = line.partition('=')
        if sep:
            settings[key.strip()] = value.strip()
    return settings

def configure_online(settings, data, env):
    """Clears the online environment whatever bbport.ini says; None: offline."""
    for key in ONLINE_ENV:
        env.pop(key, None)
    env['BB_ONLINE'] = '0'
    return None
