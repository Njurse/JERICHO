# Contributing to JERICHO

- **Building:** follow "Building" in [README.md](README.md) (Windows: `windows_dev_prepare.ps1`;
  Linux: `linux_dev_prepare.sh`, then `make config=release_x64` in `src_rebuild/build`).
  CI ([docs/CI.md](docs/CI.md)) is the reference build.
- **Engine hooks and modules:** [src_rebuild/Game/C/JERICHO/docs/HOOKS.md](src_rebuild/Game/C/JERICHO/docs/HOOKS.md)
  and [events.md](src_rebuild/Game/C/JERICHO/docs/events.md).
- **Before a pull request:** `python tools/doccheck.py` must pass, and record user-facing
  changes in [CHANGELOG.md](CHANGELOG.md).
- Bugs and requests: https://github.com/Njurse/JERICHO/issues

The upstream REDRIVER2 project's guidelines still apply to stock engine code:
https://github.com/OpenDriver2/REDRIVER2/wiki/Contributing-to-project
