# Local integration testing

- Launch Bloodborne only with `BB_HEADLESS=1` and `BB_AUDIO=none`. These are explicit user constraints for every agent-run instance.
- Use `tools/smoke-game.py` for bounded game checks. It verifies hidden presentation and the silent audio backend.
- Keep test saves separate from the user's existing installations. Store captures, dumps and large traces under `D:\project-artifacts\BloodborneSeamless`.
- Use PowerShell and Windows paths. Preserve the CLANG64 toolchain recorded in the existing build tree.
