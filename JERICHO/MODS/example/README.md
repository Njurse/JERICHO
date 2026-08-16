# Example Module

A self-contained JERICHO smoke-test package. It only uses the public SDK
(`jericho.h` — no game headers), so it proves the whole pipeline: premake
links it in, the generated registry lists it, the runtime activates it at
boot, and its hooks fire every frame (logging at boot and firing a custom
event every 60 frames).

Build id: `example` (the folder `JERICHO/MODS/example/` is auto-discovered by premake).

All module output goes to **`REDRIVER2.log`** (via the JERICHO logger,
wired at boot); the boot inventory at startup lists this module and its
hooks, so a missing log line means it isn't compiled in or is disabled.
