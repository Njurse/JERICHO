# Example Module

A self-contained JERICHO smoke-test package. It only uses the public SDK
(`jericho.h` — no game headers), so it proves the whole pipeline: premake
links it in, the generated registry lists it, the runtime activates it at
boot, and its hooks fire every frame (logging at boot and firing a custom
event every 60 frames).

Build id: `example` (must match `mods/example/` in `--with-mods`).
