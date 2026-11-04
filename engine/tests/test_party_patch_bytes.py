"""patches/Bloodborne.xml: no other patch writes bytes of the party patches, and no patch writes the game code
the engine's and the party's hooks take over (their byte checks would refuse them, and the hook with them)."""
from paths import ROOT
import unittest
import xml.etree.ElementTree as ET

from patches import EBOOT_BASE

XML = ROOT / 'patches/Bloodborne.xml'

# Prologues hooked by guest offset (llvm-objdump on out/eboot.elf); the XML addresses are EBOOT_BASE higher.
HOOKED = {
    'SprjFlipper::Update (engine/features.cpp frame hook, party_director.cpp main-thread tick)': (0x2034770, 17),
    'SprjStep constructor (engine/features.cpp menu steps)': (0x1c1cce0, 16),
}


def patch_ranges():
    root = ET.parse(XML).getroot()
    out = {}
    for meta in root.iter('Metadata'):
        name = meta.get('Name')
        if not name:
            continue
        spans = out.setdefault(name, [])
        for line in meta.iter('Line'):
            address = line.get('Address')
            if not address:
                continue
            value = line.get('Value') or ''
            size = len(value) // 2 if line.get('Type') == 'bytes' else 4
            spans.append((int(address, 16), int(address, 16) + size))
    return out


def overlaps(a, b):
    return any(a0 < b1 and b0 < a1 for a0, a1 in a for b0, b1 in b)


class PartyPatchBytesTests(unittest.TestCase):
    def test_no_patch_writes_bytes_of_a_party_patch(self):
        patches = patch_ranges()
        party = {n: r for n, r in patches.items() if n.startswith('Party: ')}
        self.assertGreater(len(party), 5)
        clashes = sorted(f'{n} -> {p}' for n, r in patches.items() if not n.startswith('Party: ')
                         for p, pr in party.items() if overlaps(r, pr))
        self.assertEqual(clashes, [])

    def test_the_live_fps_base_is_there(self):
        self.assertIn('60 FPS++ (Deltatime)', patch_ranges())

    def test_no_patch_writes_a_hooked_prologue(self):
        hooked = {what: [(EBOOT_BASE + at, EBOOT_BASE + at + size)] for what, (at, size) in HOOKED.items()}
        clashes = sorted(f'{n} -> {what}' for n, r in patch_ranges().items() for what, h in hooked.items()
                         if overlaps(r, h))
        self.assertEqual(clashes, [])


if __name__ == '__main__':
    unittest.main()
