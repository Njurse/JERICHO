# Arena files

The `.cca` arena files live **here**, in the mod's own folder, so an arena ships
with Caine's Crossfire. The game reads and writes this folder directly when it
runs from a dev build, so it and `tools/arenaedit.py` edit the **same file** —
and both watch it, so a save on either side shows up on the other
(pseudo-realtime). See [`../ARENAS.md`](../ARENAS.md).

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
spawn: -18449 -60129 0 30      # x z heading [y]
spawn: -16000 -58000 1024 30   # y = height: without it a car on a hill drops through the world
pickup: weapon missile 5000 2000 5
pickup: health -5000 1000 2500
```

The in-game editor records the car's height when you place a spawn, so authoring
by driving always gets `y` right.
