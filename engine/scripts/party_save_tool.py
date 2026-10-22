"""Save recovery commands used by the WPF launcher's Seamless page."""
import argparse
import json
from pathlib import Path
import party_saves
import party_session
import save_editor


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['describe', 'copy', 'undo', 'check',
                                           'editor-list', 'editor-export', 'editor-check', 'editor-import'])
    parser.add_argument('user', type=Path)
    # editor-export: kind slot folder; editor-check / editor-import: the edited file (scripts/save_editor.py).
    parser.add_argument('args', nargs='*')
    args = parser.parse_args()
    if args.action == 'describe':
        print('\n'.join(party_session.describe(args.user)))
    elif args.action == 'copy':
        backup = party_saves.copy_solo_to_party(args.user, replace=True)
        print('Single-player saves copied to the party folder.' + (f' Previous party saves: {backup}' if backup else ''))
    elif args.action == 'undo':
        session, backup = party_session.undo(args.user)
        print(f'Restored {session.label}. Your saves before the restore: {backup}')
    elif args.action == 'editor-list':
        print(json.dumps([{'kind': r['kind'], 'slot': r['slot'], 'text': save_editor.describe(r),
                           'guest_written': bool(r.get('guest_written'))} for r in save_editor.list_saves(args.user)]))
    elif args.action == 'editor-export':
        kind, slot, dest = args.args
        out = save_editor.export_save(args.user, kind, int(slot), dest)
        print(f'Exported to {out}. Open it in the save editor, save it there, then use Import from save editor. '
              f'Keep {save_editor.MANIFEST} next to it.')
    elif args.action == 'editor-check':
        plan = save_editor.check_import(args.user, args.args[0])
        print(json.dumps({'kind': plan['kind'], 'slot': plan['slot'], 'target': str(plan['target']),
                          'notes': plan['notes']}))
    elif args.action == 'editor-import':
        done = save_editor.import_save(args.user, args.args[0], force=True)
        print(f"Imported into slot {done['slot'] + 1}. Your saves before it: {done['backup']}")
    else:
        problems = party_session.check_completed_session(args.user)
        print('\n'.join(f'{level}: {message}' for level, message in problems) if problems else 'Party save checks passed.')


if __name__ == '__main__':
    main()
