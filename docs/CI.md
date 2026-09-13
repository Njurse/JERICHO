# Continuous integration and downloads

Prebuilt binaries are produced by GitHub Actions (`.github/workflows/build.yml`)
and published as downloadable artifacts and releases. This is the fork's
replacement for the upstream [AppVeyor](#relationship-to-appveyor) pipeline.

## What gets built

| Job | Runner | Toolchain | Configurations |
|---|---|---|---|
| `windows` | `windows-2022` | premake5 `vs2022` → MSBuild, **Win32 / x86** | `Release`, `Release_dev` |
| `linux` | `ubuntu-22.04` | premake5 `gmake2` → `make`, **x86_64** | `release_x64`, `release_dev_x64` |

Each build is packaged with everything needed to run:

- the game executable (`REDRIVER2.exe` / `REDRIVER2_dev.exe`, or the Linux ELF),
- the runtime libraries (`SDL2.dll`, `OpenAL32.dll` on Windows; system SDL2/OpenAL on Linux),
- the `data/` tree, and
- the `JERICHO/` tree (**`MODS`** and **`CONFIG`**) that the runtime reads.

Resulting archives:

```
REDRIVER2_Release_win32.zip          REDRIVER2_Release_linux-x64.tar.gz
REDRIVER2_Release_dev_win32.zip      REDRIVER2_Release_dev_linux-x64.tar.gz
```

`Release` is the clean shipping build. `Release_dev` carries the debug-options /
console build (`DEBUG_OPTIONS`, `COLLISION_DEBUG`, `CUTSCENE_RECORDER`) that is
useful for testing.

Dependencies are pinned to the versions upstream used:
premake `5.0.0-beta1`, SDL2 `2.30.2`, OpenAL-soft `1.23.1`, and libjpeg `jpeg-9d`
(whose `jconfig.vc` is renamed to `jconfig.h` before generation).

## When it runs

| Trigger | Effect |
|---|---|
| push to `main` | build both platforms, upload the four archives as **workflow artifacts**, and refresh the rolling **`alpha`** pre-release |
| push a `v*` tag | build both platforms and publish a normal **GitHub Release** with the four archives attached |
| manual run (Actions → Build → *Run workflow*) | same as a push to `main` |

Superseded runs on the same ref are cancelled automatically.

## Getting a binary

- **Latest tagged release:** the repo's *Releases* page. Pushing a tag is how a
  stable version is cut:

  ```sh
  git tag v1.0
  git push origin v1.0
  ```

- **Rolling alpha (any push to `main`):** the pre-release tagged `alpha`. Its four
  assets are overwritten on every push, so the download link is always current.
  The `alpha` tag is created and updated automatically — it is not a real version.
  Once tagged releases are the norm, delete the *Update rolling alpha pre-release*
  step from the workflow and the `alpha` release.

- **Per-commit artifacts:** open the run under *Actions* and download from the
  *Artifacts* section. Artifacts require being signed in to GitHub and expire
  after 90 days; releases do not.

## Local equivalents

The CI mirrors the existing local helpers, which remain the fastest way to build:

- Windows: `windows_dev_prepare.ps1` (fetch deps + `premake5 vs2022`), then
  `src_rebuild/build_redriver2.bat` (Release x64) or `src_rebuild/gen_vc2019.bat`.
- Linux: `linux_dev_prepare.sh` (fetch premake + `premake5 gmake2`), then
  `make config=release_x64` in `src_rebuild/build/`.
- Multiarch Docker build: `Dockerfile` + `dockerbuild.sh`.

## Relationship to AppVeyor

`appveyor.yml` and `.appveyor/` still describe the upstream build (Visual Studio
2019, Win32 + `debug_x64`/`release_x64`/`release_dev_x64`) and are left in place
unchanged. The GitHub Actions workflow is the fork's active pipeline; AppVeyor can
be retired whenever it is no longer wanted.
