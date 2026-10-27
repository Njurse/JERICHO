# Pedestrian Hacks (pedhacks)

`pedhacks` is the compiled-in JMPPED pedestrian asset module. It sits in the
mandatory core order between BAKE Toolkit and mp:

```
carhacks -> bake -> pedhacks -> mp
```

It owns the archive lifecycle and the interface-3 `jer_ped_archive.h` contract
(archive/entry/model/player state, availability, palette selection and install
entry points). `mp` consumes that contract for character selection and sync and
falls back to Tanner when the archive is unavailable.

The engine still owns the low-level v3/v5 parser, model/page installation and
VRAM state; the JMPPED authoring tools and `JMPPED.LEV` remain under
`JERICHO/MODS/mp/tools`. Moving those into pedhacks is a separate change.
