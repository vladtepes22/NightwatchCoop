# Local integration testing

- Launch Bloodborne only with `BB_HEADLESS=1` and `BB_AUDIO=none`. These are explicit user constraints for every agent-run instance.
- Use `tools/smoke-game.py` for bounded game checks. It verifies hidden presentation and the silent audio backend.
- Keep test saves separate from the user's existing installations. Store captures, dumps and large traces under `D:\project-artifacts\BloodborneSeamless`.
- Use PowerShell and Windows paths. Preserve the CLANG64 toolchain recorded in the existing build tree.

# QA gate (every agent, every change)

- Read [QA_TESTING.md](QA_TESTING.md) once. Before committing, run `python tools/qa.py plan --level change` and then `run` (CLANG64 Python: `C:\msys64\clang64\bin\python.exe`). Paste its `work/qa/<run>/report.md` table into your report, plus what stayed unproven.
- Co-op behaviour change: add or extend a row in `tools/qa_scenarios.json` (director steps per role + the log lines each role must show, in order) and run it with `python tools/qa.py run --only coop --scenario <row>`. A row that passed once on a timing-sensitive path is not proof: rerun it.
- Release: `python tools/qa.py run --level release --package <fresh package dir>` from a `subst` drive mapped to the tree. Headless layers draw nothing: anything visual goes on one numbered checklist for the user.
- When no D: drive exists, captures and smoke logs fall back to `work/artifacts` in the tree (git-ignored).
