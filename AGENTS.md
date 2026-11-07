# Repository instructions

- Use this repository root as the working directory for all future ThinkPad
  700/700C development.
- The codebase is based on upstream 86Box v6.0 (`4fef696a4`). Keep `origin` for
  this fork and `upstream` for the official 86Box repository.
- Never commit `local-assets/` or `build/`. They contain local ROMs, disk
  images, VM state, toolchains, and generated output.
- Do not add firmware, operating-system files, reference/diagnostic disks, or
  derived copies unless redistribution rights have been verified.
- Keep `README.md` (English, canonical) and `README.zh-CN.md` synchronized when
  changing user-facing setup or behavior.
- Store only selected, useful screenshots under `docs/images/`; do not retain
  raw XWD/frame sequences in the repository.
- Before handing off source changes, run `git diff --check`, build the relevant
  target, and perform a focused ThinkPad 700 boot/firmware regression when the
  local assets are available.
