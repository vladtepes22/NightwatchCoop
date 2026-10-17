"""Save recovery commands used by the WPF launcher's Seamless page."""
import argparse
from pathlib import Path
import party_saves
import party_session


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['describe', 'copy', 'undo', 'check'])
    parser.add_argument('user', type=Path)
    args = parser.parse_args()
    if args.action == 'describe':
        print('\n'.join(party_session.describe(args.user)))
    elif args.action == 'copy':
        backup = party_saves.copy_solo_to_party(args.user, replace=True)
        print('Single-player saves copied to the party folder.' + (f' Previous party saves: {backup}' if backup else ''))
    elif args.action == 'undo':
        session, backup = party_session.undo(args.user)
        print(f'Restored {session.label}. Your saves before the restore: {backup}')
    else:
        problems = party_session.check_completed_session(args.user)
        print('\n'.join(f'{level}: {message}' for level, message in problems) if problems else 'Party save checks passed.')


if __name__ == '__main__':
    main()
