# Continuous integration and downloads

Prebuilt binaries are produced by GitHub Actions (`.github/workflows/build.yml`)
and published as downloadable artifacts and releases. This is the fork's
replacement for the upstream [AppVeyor](#relationship-to-appveyor) pipeline.

## What gets built

| Job | Runner | Toolchain | Configurations |
|---|---|---|---|
| `windows` | `windows-2022` | premake5 `vs2022` → MSBuild, **x64** | `Release` |
| `linux` | `ubuntu-22.04` | premake5 `gmake2` → `make`, **x86_64** | `release_x64` |

Each build is packaged with everything needed to run:

- the game executable (`JERICHO.exe` on Windows; `jericho` on Linux — lowercase, because
  the `JERICHO/` tree below has to share a directory with it and a file and a directory
  cannot share a name on a case-sensitive filesystem. Naming the Linux binary exactly
  `JERICHO` made the post-build mirror's `mkdir -p .../JERICHO/MODS` fail, which is how a
  Linux release came to be published with no `MODS`, `CONFIG` or `CORE` at all. The dev
  builds — `JERICHO_dev`, `JERICHO_dbg` — never collided and keep their names, which the
  rigs and launchers use),
- the runtime libraries (`SDL2.dll`, `OpenAL32.dll` on Windows; system SDL2/OpenAL on Linux),
- the `data/` tree, and
- the `JERICHO/` tree (**`MODS`**, **`CONFIG`**) that the runtime reads, and **`CORE`** --
  the custom frontend art: the menu background the runtime loads and its source PNG. It is
  tracked deliberately, because a build that arrives without it has no frontend background
  at all (`jer_texture: cannot read .../CORE/jericho_background.tga`, then the menu draws
  on `texture 0`), and a CI checkout is exactly where an untracked file goes missing.

Resulting archives:

```
JERICHO_Release_win64.zip      JERICHO_Release_linux-x64.tar.gz
SHA256SUMS
```

Only `Release` is built and published: the clean shipping build, which
pre-includes just the release mods (`JERICHO_RELEASE_MODS` in `premake5.lua` —
carhacks, crumple, d1cars, mp). The `Release_dev` configuration — every
installed module plus the debug options / console (`DEBUG_OPTIONS`,
`COLLISION_DEBUG`, `CUTSCENE_RECORDER`) — is **not** built here and stays local.
`SHA256SUMS` lists the archives' SHA256, published beside them so a downloader --
the mp remote agent -- can verify an archive against something that did not come
out of it.

A third job, `checks`, runs on every push and takes seconds; it covers what a
build cannot see: `tools/doccheck.py` (docs consistent with the code), a
"no debug switch changes behaviour" test, and the mp delivery gate — which
**skips** where the 1.6 GB of game assets are absent rather than passing. It is
deliberately *not* a dependency of the publish job, so a docs typo cannot stop a
downloadable build; a red check is the signal.

Dependencies are pinned to the versions upstream used:
premake `5.0.0-beta1`, SDL2 `2.30.2`, OpenAL-soft `1.23.1`, and libjpeg `jpeg-9d`
(whose `jconfig.vc` is renamed to `jconfig.h` before generation).

## When it runs

| Trigger | Effect |
|---|---|
| push to `main` | build both platforms, upload the two archives as **workflow artifacts**, and refresh the rolling **`alpha`** pre-release |
| push a `v*` tag | build both platforms and publish a normal **GitHub Release** with the two archives and `SHA256SUMS` attached |
| manual run (Actions → Build → *Run workflow*) | same as a push to `main` — or, with the **`release_tag`** input set, publish this ref's build as a release under that tag (see [Installing a CI build on a test machine](#installing-a-ci-build-on-a-test-machine)) |

Superseded runs on the same ref are cancelled automatically.

## Getting a binary

- **Latest tagged release:** the repo's *Releases* page. Pushing a tag is how a
  stable version is cut:

  ```sh
  git tag v1.0
  git push origin v1.0
  ```

- **Rolling alpha (any push to `main`):** the pre-release tagged `alpha`. Its
  assets are overwritten on every push, so the download link is always current.
  The `alpha` tag is created and updated automatically — it is not a real version.
  Once tagged releases are the norm, delete the *Update rolling alpha pre-release*
  step from the workflow and the `alpha` release.

- **Per-commit artifacts:** open the run under *Actions* and download from the
  *Artifacts* section. Artifacts require being signed in to GitHub and expire
  after 90 days; releases do not.

## Installing a CI build on a test machine

The mp remote agent (`JERICHO/MODS/mp/tools/remote/mp_agent.ps1`) installs a binary
only from a **published release** — it has no way to accept a file pushed to it — so a
build has to reach GitHub before it can reach another PC. From a working branch, where
neither of the automatic triggers applies:

1. push the branch;
2. **Actions → Build → Run workflow**, pick the branch, set `release_tag` (for example
   `pr15`), and run it. Re-running with the same tag replaces the assets, which is the
   point: the tag names a line of work, not a moment;
3. from a machine that can reach the test PC:

   ```sh
   python JERICHO/MODS/mp/tools/remote/mp_remote.py update \
       --peer 192.168.50.244 --port 1401 --tag pr15
   ```

The rules that matter on this path, and why:

- the tag must match the agent's rule `[A-Za-z0-9][A-Za-z0-9._-]{0,63}`, and must not be
  `alpha` or a `v*` tag — those belong to the push-to-main and tag steps;
- the agent accepts exactly one asset name, `JERICHO_Release_win64.zip`, and
  verifies it against GitHub's per-asset digest, falling back to the `SHA256SUMS`
  asset beside it;
- it installs the game exe, its `.pdb`/`.map`, `SDL2.dll`, `OpenAL32.dll` and
  `JERICHO/` (the exact set is `$InstallSet` in `mp_agent.ps1`). The game content
  the archive also carries is downloaded and staged but **not** installed, so
  shipping it costs time and disk, not correctness;
- the on-demand release is a pre-release and never the *latest*, so it cannot disturb the
  download anyone else sees;
- both ends must run the **same** build to join a session, so publishing through CI is
  what makes "build here, test there" work at all: the version a CI run embeds comes from
  `git describe` on that commit, and a session refuses a peer whose build differs.

## Hardening: `_FORTIFY_SOURCE` on the Linux builds

The Linux Release and `Release_dev` configurations are compiled with
`_FORTIFY_SOURCE=2` (`premake5.lua`, in a `filter { "system:Linux",
"configurations:Release or Release_dev" }` — Debug is left out because fortify
needs `-O1` or better and would only warn at `-O0`). The Windows build does not
get it; MSVC has no equivalent.

This matters because the published Linux binary and a locally built one were not
being checked the same way. Debian 13 and Ubuntu 24.10+ enable `_FORTIFY_SOURCE`
in a normal `gcc` build, but the `ubuntu-22.04` image the `linux` job uses does
not — so a fixed-buffer defect could pass CI for months and abort the moment
somebody built the same commit on a current distribution. That is exactly what
happened: `jer_texture.c` handed a 512-byte buffer to `realpath()`, whose fortify
wrapper requires `PATH_MAX` and calls `__chk_fail()` *before* resolving anything,
so the game died at the frontend menu background with

```
[jer_console] console ON (from CONFIG/hud.ini)
*** buffer overflow detected ***: terminated
Aborted
```

while CI stayed green. Turning fortify on for the CI Linux job means the build
the project publishes is the build it checked.

The trade-off is deliberate: a remaining overflow now **aborts** on Linux instead
of corrupting memory quietly. If that is ever unwanted, scope the filter to a
CI-only configuration rather than deleting it — the diagnostics are the point.

Two local checks worth knowing when a Linux-only abort like this shows up:

```sh
gcc -dM -E - </dev/null | grep FORTIFY_SOURCE   # is the toolchain fortifying?
make config=release_x64 2>&1 | grep -i "path_max\|stringop-overflow\|fortify"
```

The first answers "why only on my machine"; the second catches the calls whose
*declared* size is wrong (the `realpath` case, which aborts whatever the path),
as opposed to the ones that only abort when the data happens to be too long.

## Local equivalents

The CI mirrors the existing local helpers, which remain the fastest way to build:

- Windows: `windows_dev_prepare.ps1` (fetch deps + `premake5 vs2022`), then
  `src_rebuild/build_jericho.bat` (Release x64) or `src_rebuild/gen_vc2019.bat`.
- Linux: `linux_dev_prepare.sh` (fetch premake + `premake5 gmake2`), then
  `make config=release_x64` in `src_rebuild/build/`.
- Multiarch Docker build: `Dockerfile` + `dockerbuild.sh`.

## Relationship to AppVeyor

`appveyor.yml` and `.appveyor/` still describe the upstream build (Visual Studio
2019, Win32 + `debug_x64`/`release_x64`/`release_dev_x64`) and are left in place
unchanged. The GitHub Actions workflow is the fork's active pipeline; AppVeyor can
be retired whenever it is no longer wanted.
