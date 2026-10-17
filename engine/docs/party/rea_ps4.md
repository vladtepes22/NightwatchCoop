# REA on the Bloodborne eboot (PS4 ELF)

REA (`https://github.com/morluto/rea`, local clone `E:\games\bbport\deps\rea`, branch `ps4-elf`, not
pushed) is an MCP/CLI front end over Ghidra. Our branch adds PS4 support: SCE ELF (`e_type`
0xFE00/0xFE04/0xFE0C/0xFE10/0xFE18) and plaintext SELF (`eboot.bin` from the dump) are admitted, the
SCE dynamic tables in `PT_SCE_DYNLIBDATA` are read, `R_X86_64_RELATIVE/_64/_GLOB_DAT/_JUMP_SLOT`
are applied, imports get NID-resolved names in their library namespace (`libc::memcpy`,
`libScePad::scePadReadState`), PLT stubs become thunks, and every `.eh_frame_hdr` FDE start seeds a
function (162,959 FDEs → ~167,700 functions). It also fixes why REA refused `eboot.elf` on Windows
before: the Windows Ghidra provider admitted **PE files only** (`target_format_unsupported: Windows
Ghidra P0 accepts PE targets only`), for any ELF, not just PS4 ones.

**Addresses are our offsets** (raw ELF VA, image base 0) - the same numbers as `tools/re/decomp.py`,
Ghidra project `deps/ghidra-proj`, and the rest of `docs/party/*`. `eboot.bin` is unwrapped into an
ELF byte-identical to `smoketest/out/eboot.elf` (SHA-256 `cec1b276…4f86`), so both inputs give
the same addresses and results.

## Build (once)

Prerequisites already on this machine: Node 22.23, MSYS2 mingw64 (for the Windows native addon),
Ghidra 12.1.4 (`deps/ghidra/ghidra_12.1.4_PUBLIC`), JDK 21 (`C:\Program Files\Java\jdk-21.0.10`).

```sh
cd /e/games/bbport/deps/rea
git checkout ps4-elf
HUSKY=0 npm ci
PATH=/c/msys64/mingw64/bin:$PATH node scripts/build-windows-native.mjs   # native/windows/build/*.node
npm run build:cached                                                      # dist/
```

The native addon (job objects, private DACL runtime) is required on Windows; without it the
Ghidra provider reports `unsupported_host`. `node scripts/rea.mjs doctor --json` should show
`ghidra-native_authority: healthy`.

## Environment

```sh
export GHIDRA_INSTALL_DIR='E:\games\bbport\deps\ghidra\ghidra_12.1.4_PUBLIC'
export JAVA_HOME='C:\Program Files\Java\jdk-21.0.10'
export REA_ANALYSIS_PROVIDER=ghidra
export GHIDRA_HEADLESS_MAXMEM=12G               # 2G default runs out of heap on this image
export REA_GHIDRA_STARTUP_TIMEOUT_MS=3600000    # import + analysis take ~13 min before the first answer
export REA_PS4_NID_DATABASE='E:\games\bbport\src\src\import_names.inc'
# optional: REA_GHIDRA_PS4_ANALYSIS=full  (Ghidra default analyzers, ~30 min, more stack/switch detail)
```

`REA_PS4_NID_DATABASE` takes one or more files (`;`-separated on Windows). It reads our
`import_names.inc`, shadPS4 `aerolib` (`STUB("nid", name)`), ps4libdoc JSON, `NID name` rows, and plain
symbol-name lists (REA hashes the name to its NID). With `import_names.inc` 687 of 701 imports
resolve; the other 14 show as `libc::nid_<NID>`. A names list fixes those, e.g. adding a file with
`sqrtf`, `floorf`, `ceilf`, `fabsf` resolves four of them.

## Use it from an agent (MCP, recommended)

Every REA session imports and analyzes the eboot from scratch (~13 min with the default `fast`
preset). The CLI pays that per command; the MCP server keeps one session alive, so register it once:

```sh
claude mcp add rea \
  -e GHIDRA_INSTALL_DIR='E:\games\bbport\deps\ghidra\ghidra_12.1.4_PUBLIC' \
  -e JAVA_HOME='C:\Program Files\Java\jdk-21.0.10' -e REA_ANALYSIS_PROVIDER=ghidra \
  -e GHIDRA_HEADLESS_MAXMEM=12G -e REA_GHIDRA_STARTUP_TIMEOUT_MS=3600000 \
  -e REA_PS4_NID_DATABASE='E:\games\bbport\src\src\import_names.inc' \
  -- node 'E:\games\bbport\deps\rea\scripts\rea.mjs' mcp
```

Then: `open_binary {path: "E:/games/bbport/smoketest/out/eboot.elf", provider_id: "ghidra"}` (or
`E:/games/bloodborne/eboot.bin`), wait for the first query (it blocks until analysis is done), then
use `procedure_pseudo_code`, `procedure_assembly`, `read_function_instructions`, `procedure_callers`,
`procedure_callees`, `xrefs`, `address_name`, `search_procedures` (e.g. `scePad`, `sceNpMatching2`),
`search_strings`, `list_segments`, `trace_native_values`. Answers after startup take milliseconds.
Results are Evidence JSON (`raw_result` holds the text).

## One-off CLI

```sh
node scripts/rea.mjs decompile 'E:\games\bbport\smoketest\out\eboot.elf' 0x13CDE30 --provider ghidra --format json
node scripts/rea.mjs instructions 'E:\games\bloodborne\eboot.bin' 0x1339870 --provider ghidra --format json
```

Each call costs a full ~13 min import. Use `--format json`; the `md` renderer prints nested
diagnostics as `[object Object]`.

## REA vs `tools/re/decomp.py`

| | `tools/re/decomp.py` | REA |
|---|---|---|
| Database | persistent `deps/ghidra-proj` (analyzed once, full Ghidra analysis) | ephemeral, rebuilt per session |
| First answer | one headless Ghidra start per call (`-noanalysis`, project already analyzed) | ~13 min per session, then milliseconds |
| Import names | none (calls through GOT show as `FUN_`/`PTR_`) | NID-resolved (`memcpy`, `scePadReadState`, `PTR___stack_chk_guard`) |
| Relocations / GOT | not applied | applied; GOT slots constant, PLT stubs are named thunks |
| Function starts | Ghidra heuristics | every `.eh_frame` FDE |
| Output | plain C text + callers/callees | Evidence JSON with digests/provenance; xrefs, CFG, p-code value flow |

Guidance for agents:

- Quick look at one or two functions, or anything scripted in a loop: `tools/re/decomp.py` (and
  `tools/re/refs.py`/`FindRefs.java`) on the persistent project.
- Questions that involve library calls (what a function does with `sceNp*`, `scePad*`, `sceKernel*`,
  libc), call-graph walks, or many functions in one sitting: REA over MCP; keep the session open.
- Cite addresses as our offsets either way; they agree. Function boundaries can differ slightly
  (REA uses FDE starts), so name functions by entry address.
- REA does not write to the eboot, to `deps/ghidra-proj`, or to the game folder; it copies the target
  into a private temp runtime (`%TEMP%\rea-ghidra-*`) that it deletes on close.

## Limits

- Import slots live in an artificial `EXTERNAL` block after the image (`0x56e0000+`); they are not PS4
  runtime addresses.
- Only the four relocation types above are applied (eboot has no others: 231,478 RELATIVE, 2,838
  _64, 661 JUMP_SLOT, 23 GLOB_DAT); TLS relocations would be recorded as unsupported.
- `fast` skips Ghidra's Stack, Decompiler Switch, Function ID, x86 Constant Reference, discovered
  Non-Returning, Call-Fixup, Shared Return and Embedded Media analyzers. Decompiles are still complete
  (the decompiler recovers locals and switches per function), but constant-only data references are
  sparser; use `REA_GHIDRA_PS4_ANALYSIS=full` when hunting data xrefs.
- Encrypted/compressed SELFs are refused; only plaintext (fake-signed/decrypted) ones are unwrapped.
  For `eboot.bin`, file offsets REA reports refer to the unwrapped ELF (= `eboot.elf`), not to SELF
  offsets; the `PT_SCE_VERSION` bytes are not in the SELF and stay zero.
- `inspect_native_load_image` on the eboot is >10 MB (every relocation) and exceeds REA's MCP
  response budget; use `export_evidence_bundle` or the CLI with `--format json` for it.
