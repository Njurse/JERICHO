# Arena files

The `.cca` arena files live **here**, in the mod's own folder, so an arena ships
with Caine's Crossfire. The game reads them from its `MODS/cainescrossfire/arenas/`
mirror (the build keeps that copy in step with this one), and both editors write
here too — see [`../ARENAS.md`](../ARENAS.md).

- One `.cca` per arena. `chicago.cca` **replaces** the built-in Chicago arena;
  any other name adds a new one to the Deathmatch list.
- The C sources in this folder (`registry.c`, `arena.c`, `arenafile.c`,
  `editor.c`, `profile.h`) are the registry, the runtime and the editors.

Easiest way to make or open one: run [`../tools/arena_menu.bat`](../tools/arena_menu.bat).

```
arena: chicago_docks
name: Chicago Docks
city: CHICAGO
mp: 1 0
region: -8000 -70000 8000 -55000
spawn: -18449 -60129 0
spawn: -16000 -58000 1024
pickup: weapon missile 5000 2000 5
pickup: health -5000 1000 2500
```
