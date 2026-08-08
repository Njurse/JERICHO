# Over-the-Shoulder Camera — JERICHO example package

Demonstrates the new `JER_EVENT_CAMERA` hook (fired at the end of
`InitCamera` once `camera_position` is set).

When Tanner is **on foot**, the module shifts the chase camera:

- **closer** to him (`CAM_DIST = 900` — the offset subtracts along his facing,
  pulling the camera in), and
- **to the left** of his facing (`CAM_LATERAL = 400`).

The offset uses Tanner's **body direction** (`lp->dir`, the same angle the
game's chase cam uses via `baseDir`) — not the head/look angle — so it rotates
with his movement. The camera keeps aiming at the look target ahead of him,
so his view direction stays lined up with the screen centre while his
silhouette sits off-centre — over-the-shoulder framing. Flip
`CAM_LEFT_SIGN` to `-1` for the other shoulder.

The offset math (world x/z plane, body direction `h`):

```
facing  f = (RSIN h, RCOS h)          (the chase cam sits at +cameraDist*f,
                                       so pulling in is -DIST*f)
left    l = (RCOS h, -RSIN h)
offset  = -CAM_DIST * f + CAM_LATERAL * CAM_LEFT_SIGN * l
```

Toggle the module off in the frontend JERICHO Config menu (or remove
`camera` from the `--with-mods` premake list and rebuild) to restore the
stock camera.
