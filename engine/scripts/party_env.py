"""The one rule for "is party mode on" (BB_PARTY), as gpu/party_env.h has it for the game:
unset, empty (or blanks only), 0, off, false, no (any case, surrounding blanks ignored) = off;
any other value (host, join, guest, 1, a party code) = on. run.bat clears an off value first."""
import os

OFF_VALUES = ('', '0', 'off', 'false', 'no')


def party_value_on(value):
    """True when `value` (BB_PARTY or another party on/off switch) means on."""
    return value is not None and str(value).strip().lower() not in OFF_VALUES


def party_enabled(env=os.environ):
    """True when the environment's BB_PARTY turns party mode on."""
    return party_value_on(env.get('BB_PARTY'))


def party_switch_on(env, name):
    """A party switch that is on by default (BB_PARTY_FAST_JOIN, ...): unset or blank = on,
    otherwise party_value_on (gpu/party_env.h bb_party_switch_on)."""
    value = env.get(name)
    return value is None or not str(value).strip() or party_value_on(value)
