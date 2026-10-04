# tools/

Repo-wide maintenance tools. Mod-specific tooling lives next to its mod
(`JERICHO/MODS/<mod>/tools/`).

| Tool | What it is for |
| --- | --- |
| `publish_release.ps1` | Build, package and publish a release locally — the offline equivalent of the `windows` + `publish` jobs in `.github/workflows/build.yml` |
| `dmp_fault.py` | Read a minidump and print the exception plus the faulting module and RVA |
| `map_lookup.py` | Turn that RVA into a function name using a build's `.map` file |
| `doccheck.py` | Check the documentation: links resolve, every engine event is in `events.md`, every doc is reachable from `docs/README.md`, and the counted claims match the code |

## Checking the documentation

```bash
python tools/doccheck.py
```

It exits non-zero on any failure and prints every one it finds, not just the first.

Documentation here drifts in three ways, and all three have had to be caught by hand at
least once: a relative link to a file that moved or never existed; a claim that is really a
**number** whose source of truth is code (a slot count, a guest-city ceiling, the size of
the roster) parting company with it; and a doc that nothing links to. So it checks:

- every relative link in every tracked `.md` resolves;
- every `JER_EVENT_*` the engine declares appears in the SDK's `events.md`;
- every doc under a mod's folder is reachable from `docs/README.md`, by resolving the
  index's links rather than by matching names - matching names would pass every `README.md`
  in the repo and mean nothing;
- the counted claims agree with the code they describe: `MAX_CAR_RESIDENT_MODELS` in
  `dr2limits.h` vs the number `PROFILES.md` states (and that `AI.md` does not restate it),
  the guest-city ceiling derived from `cars.h` vs `PROFILES.md`, and the length of the
  profile manifest `gVehRows[]` vs the count the docs advertise.

**Only tracked files are read.** That is deliberate: `bin/Release` and `bin/Release_dev`
carry untracked mirror copies of the docs from the last build, so anything walking the
filesystem matches stale text and reports a corrected claim as uncorrected.

When a counted claim changes *intentionally*, change the code's value and the doc together
- that is the point of the check. `doccheck.py` has been verified in both directions: it
passes on the tree, and reverting a claim (say `MAX_CAR_RESIDENT_MODELS` in `PROFILES.md`)
makes it fail by name.

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

Artifacts land in `dist/` as `JERICHO_Release_win64.zip` and
`JERICHO_Release_dev_win64.zip`. Each holds the exe, the runtime DLLs
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

## The CI build path

`.github/workflows/build.yml` builds both configurations for `windows-2022` and
`ubuntu-22.04`, uploads them as workflow artifacts, refreshes the rolling
`alpha` pre-release on every push to `main`, and publishes a permanent release
for any `v*` tag.

Three things about it are deliberate, and each is easy to undo by accident.

### Submodules are an explicit allow-list, not `--recursive`

`actions/checkout` runs with `submodules: false`, and a following step clones
each path the build actually needs:

```bash
for path in src_rebuild/PsyCross; do
  git submodule update --init --depth=1 "$path"
done
```

`submodules: recursive` walks *every* entry in `.gitmodules`, and a single
unfetchable one fails the whole checkout before any build step runs. That is
exactly how both platforms spent their entire history failing at step 2, with
`fatal: repository 'https://github.com/Njurse/gailredriver2.git/' not found`
on whichever submodule sorts first.

`JERICHO/MODS/gaildrv2` is deliberately **out of the picture for builds** — its
repository is not published — so it stays registered in `.gitmodules` for local
work but is never fetched here. The rule to keep: adding a submodule to
`.gitmodules` must not be able to break the build by itself. Add a path to that
loop only when a build genuinely needs it.

### Each configuration links twice

See the note on `exports.def` above. This is why the Windows job can produce an
exe but a naive `msbuild` cannot.

### The Windows job builds x64

A Win32 link cannot work with the `exports.def` mechanism at all — x86 decorates
every C++ name differently, leaving ~981 unresolved `LNK2001`s — and every dev
build target is x64. The job's name said `(Win32)` until 0.9.0; it was only ever
a label.

### Switching architecture locally

CI builds x64 only, so it never meets this. Locally, `lib/%{cfg.buildcfg}` is
**shared between architectures**: `premake_libjpeg.lua` sends the jpeg static
library to `lib/<cfg>/jpeg.lib`, while the objects *are* separated
(`dependencies/jpeg-9d/obj/x64` beside `obj/x86`). So one Win32 build overwrites
the x64 library at that same path, and the next x64 link fails with:

```text
lib\Release\jpeg.lib : warning LNK4272: library machine type 'x86' conflicts with target machine type 'x64'
VideoPlayer.obj : error LNK2019: unresolved external symbol jpeg_std_error ...
..\bin\Release\JERICHO.exe : fatal error LNK1120: 8 unresolved externals
```

Delete the poisoned artifact and rebuild. `*.lib` is gitignored, so this is
always local-only and never something CI can hit:

```bash
rm -f src_rebuild/lib/Release/*.lib
```

It is also why a 0-byte `bin/<cfg>/JERICHO.exe` can sit in the tree looking
like a mystery: the link failed, and the empty file is what the linker left
behind.

## The release profile

What a release *runs* is decided by the shipped `JERICHO/CONFIG/modlist.ini`,
which the build mirrors next to the exe. Since 0.9.0 that file is a deliberate
release profile — **carhacks and mp on, everything else off**, including
`crumple`, `levelhacks` and `debugorbit` (the orbit camera seizes the camera at
level start and never hands it back). Local development is free to differ; the
frontend's Options → JERICHO rewrites the file on toggle.

### Compiling only carhacks and mp is deliberately NOT done

The profile above is about **activation**. The binaries still compile every deep
module under `JERICHO/MODS/` — gaildrv2 excepted, which CI never fetches — and
merely leave all but carhacks and mp switched off.

Restricting the compiled-in set is a real change to the build system: it means
teaching the mod scan (`premake_modules/jericho_mods.lua`) and its callers about
a release subset, and a filter bug there drops a module from the build *silently*
rather than failing. It is a deliberate non-goal for now, not an oversight.

The consequence worth knowing: a locally built release differs from a CI one in
exactly that respect. The exe in this tree has gaildrv2 and every other module
linked in (inactive); a CI exe does not contain gaildrv2 at all.

### Where a deep module's compiled output lives

Inside the module's own folder, not in `bin/<cfg>`:

```text
JERICHO/MODS/<id>/lib/<config>/<platform>/mod_<id>.lib
JERICHO/MODS/<id>/obj/<config>/<platform>/
```

Scoped by configuration *and* platform, because the file is called
`mod_<id>.lib` in every one of them: a single shared path lets one
configuration's library be linked into another's build — which it did, and it
only failed loudly because `Release_dev` and `Release` disagree about the C
runtime. `bin/<cfg>` never separated x86 from x64 either, which is the other half
of the same trap. The post-build mirror that copies `JERICHO/` next to the exe
drops `lib/`, `obj/` and the linker/debug artefacts on the way through, so a
shipped tree carries a module's source and its manifest but no build output. A
runtime addon's `<id>.dll` is game data and no rule there touches it.

## The version

`JERICHO_BUILD_VERSION` comes from `git describe --tags --always --dirty`
(`premake5.lua`), so a release is versioned by its tag. Release tags are spelled
`v0.9.0` because the workflow triggers on `v*` — the repo's older REDRIVER2 tags
carry no prefix at all — and premake strips that leading `v`, so the binary and
the boot log say `0.9.0`, not `v0.9.0`.

The string is hashed into mp's build identity (`gameBuild` in `mp_proto.h`), so
two peers on different builds refuse each other. That is the point of bumping it.

To cut one:

```bash
git tag -a v0.9.0 -m "JERICHO 0.9.0"
git push origin v0.9.0
```

Watch the tag land *after* the fixes it should contain: the tag triggers a
permanent, public release, not a rebuild of a previous one.

