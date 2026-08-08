# Over-the-Shoulder Camera — JERICHO example package

Demonstrates the new `JER_EVENT_CAMERA` hook (fired at the end of
`InitCamera` once `camera_position` is set).

When Tanner is **on foot**, the module shifts the chase camera:

- **closer** to him (`CAM_DIST = 1000` — the default chase distance is
  ~1500–2000), and
- **to the left** of his facing (`CAM_LATERAL = 450`).

The camera keeps aiming at the look target ahead of Tanner, so his view
direction stays lined up with the screen centre while his silhouette sits
about a third of the half-width off-centre — over-the-shoulder framing.
Flip `CAM_LEFT_SIGN` to `-1` for the other shoulder.

The offset math (world x/z plane, heading `h`):

```
facing  f = (RSIN h, RCOS h)
left    l = (RCOS h, -RSIN h)
offset  = CAM_DIST * f + CAM_LATERAL * CAM_LEFT_SIGN * l
```

Toggle the module off in the frontend JERICHO Config menu (or remove
`camera` from the `--with-mods` premake list and rebuild) to restore the
stock camera.
