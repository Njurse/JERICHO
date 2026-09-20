# tools/

Repo-wide maintenance tools. Mod-specific tooling lives next to its mod
(`JERICHO/MODS/<mod>/tools/`).

| Tool | What it is for |
| --- | --- |
| `publish_release.ps1` | Build, package and publish a release locally — the offline equivalent of the `windows` + `publish` jobs in `.github/workflows/build.yml` |
| `dmp_fault.py` | Read a minidump and print the exception plus the faulting module and RVA |
| `map_lookup.py` | Turn that RVA into a function name using a build's `.map` file |

## Publishing a release without Actions

`publish_release.ps1` exists because GitHub Actions can refuse to run for
reasons that have nothing to do with this repo — an account billing lock
means every job comes back with `steps: []`, `runner_id: 0` in about two
seconds and the annotation *"The job was not started because your account is
locked due to a billing issue."* When that happens no runner starts, so the
`publish` job never runs and no release is ever produced. This script does
the same work on your own machine.

### Prerequisites

- Visual Studio with the **C++ workload** (MSBuild + the v142/v143 toolset).
  The script finds MSBuild through `vswhere`.
- The dependencies premake expects under `src_rebuild/dependencies/`:
  `SDL2-2.30.2`, `openal-soft-1.23.1-bin`, `jpeg-9d`.
- A GitHub token with the **`repo`** scope, in `GH_TOKEN`:
  create one at https://github.com/settings/tokens

```powershell
$env:GH_TOKEN = 'ghp_...'
```

### Use

```powershell
# build + package only; print what would be published, publish nothing
powershell -ExecutionPolicy Bypass -File tools/publish_release.ps1 -DryRun

# refresh the rolling 'alpha' pre-release from the current build
powershell -ExecutionPolicy Bypass -File tools/publish_release.ps1 -Publish

# a permanent release from a tag
powershell -ExecutionPolicy Bypass -File tools/publish_release.ps1 -Tag v1.2.0 -Publish

# re-publish without rebuilding
powershell -ExecutionPolicy Bypass -File tools/publish_release.ps1 -Publish -SkipBuild
```

Artifacts land in `dist/` as `REDRIVER2_Release_win64.zip` and
`REDRIVER2_Release_dev_win64.zip`. Each holds the exe, the runtime DLLs
(`SDL2.dll`, `OpenAL32.dll`, `soft_oal.dll`), the `JERICHO/` module tree and
the `data/` runtime tree. **The FMV videos are deliberately not shipped** —
they are 1.5 GB of the 1.6 GB build and `-nofmv` is supported.

`-Publish` attaches the zips to the release, replacing any asset of the same
name, which is what the rolling `alpha` needs on every refresh.

### What it cannot do

Only the **Windows x64** half of a release. There is no Linux toolchain
here, so `REDRIVER2_*_linux-x64.tar.gz` can only come from CI. The two paths
are complementary — a release simply carries whatever it got, because the
`publish` job runs when *either* platform built.

### Note on `exports.def`

Both this script and CI link each configuration twice:

1. link against a throwaway empty `exports.def` — this is what writes the
   `.map` file;
2. regenerate `exports.def` from that map with `gen_exports`, then relink
   (incremental, only the exe relinks).

`exports.def` is generated from a linker map, so any committed copy is stale
by construction, and the linker is handed it for *every* configuration and
platform. Linking against a stale one fails with hundreds of `LNK2001`s —
that is the bug that used to make the Windows CI job fail every time.
