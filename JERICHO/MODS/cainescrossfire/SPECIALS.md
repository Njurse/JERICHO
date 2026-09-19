# Vehicle specials

Each vehicle profile owns one unique **special** weapon. They live in
`weapons/special/`, one file each, and are driven through the ordinary weapon
framework: a `CD2_WEAPON_DEF` row plus a fire function, reachable by the same
`cd2WpnTryFire` the shared weapons use (the player's trigger, the AI, and the
debug driver all get them). What a special adds beyond a shared weapon is a
**status**: a per-car timed effect with its own hooks.

- Internal ids are `CD2_WID_SPECIAL_<CAR>` — internally named **`special_<car>`**
  (`special_hornet`), separate from the on-screen name in the def's `displayName`
  (`"Spike Storm"`).
- The **recharge**, the **ammo** and the **display name** all live in the
  special's own def (`refireCooldown`, `maxAmmo`, `displayName`) — the vehicle
  profile only **names** the weapon (`specialWeapon`), so weapon data is never
  duplicated in a profile.
- Ammo is **per car** (`weapons.c`, `gCarAmmo[MAX_CARS][CD2_WID_COUNT]`), so each
  contestant's special is its own. A car is handed its special when its profile
  is first assigned, and the **player starts on it**.

Firing one headlessly (the debug driver):

```
60:select:hornet        # arm the special (the ring comes up)
90:fire:hornet          # launch
150:fire:avalanche
180:fire:corvo
210:fire:bruxa
240:fire:highwayman
270:fire:deadstar
```

## The six

| internal | display | car | recharge | capacity |
|---|---|---|---|---|
| `special_hornet` | Spike Storm | Hornet | 600 (20s) | 3 |
| `special_avalanche` | Monster Crush | Avalanche | 900 (30s) | 2 |
| `special_corvo` | Siren's Wrath | Corvo | 540 (18s) | 3 |
| `special_bruxa` | Double Boom | Bruxa | 450 (15s) | 4 |
| `special_highwayman` | Breath of Fire | Highwayman | 660 (22s) | 3 |
| `special_deadstar` | Death Dash | Deadstar | 750 (25s) | 2 |

### Hornet — Spike Storm

The ring deploys while the special is **armed**: eight spikes stand up around
the car (one per compass direction — a bright vertical spike with a faint spoke
back to the car), and any other car inside the ring takes contact damage and a knockback
every 15 frames. Pressing **fire launches** the eight outward in their eight
facing directions — weakly homing, a narrow lock cone — exploding on whatever
they hit (world or car). The launch spends the charge and starts the recharge.

### Avalanche — Monster Crush

A 5-second surge of max normal speed and raised grip. During it, ramming another
car makes Avalanche **climb on top**: the attacker is held above the victim, the
victim's velocity is pinned (trapped), the attacker's tyres spin and it can still
be steered, then it shoves the victim off and drops back.

### Corvo — Siren's Wrath

A 4-second siren. The **police siren** (the engine's siren sample, `SOUND_BANK_VOICES`
0 — what its siren cars play) wails on a loop, and the **siren light is forced onto
the car** for the whole window: Corvo is the model-0 car, but the engine only draws
a siren light for cop/pursuer cars (`cars.c`, `CarHasSiren` + controlType), so the
special drives `AddCopCarLight` itself. A lightning bolt revolves around the car and,
every 20 frames, arcs onto the nearest other car in range: moderate damage, a shaky
twist (angular velocity) that reads as electrocution, and a small vertical jump.

### Bruxa — Double Boom

The repurposed double shotgun: **40 pellets** (twice the demo weapon's 20), a
hard recoil shoved back into the shooter, and a loud, pitched-down boom. Target
recoil rides the per-pellet knock, so more pellets landing = a harder shove.

### Highwayman — Breath of Fire

A 4-second narrow cone of flame pours from the nose (short-range raycast
particles). While it burns, tapping fire launches a **homing fireball** that
seeks a target and explodes for extra damage.

### Deadstar — Death Dash

Instant acceleration to turbo top speed for 2.5 seconds, with the horn blaring
twice (0.75s apart). While dashing, the collision damage Deadstar deals is
**x4**, and a near-90-degree hit into a car's side lands an extra 15%.

## What is a first cut

The mechanics are the described ones, built on the framework's primitives, but a
few are approximations to iterate on:

- Hornet's ring is a damage/knock aura drawn with the engine's line primitives,
  not a modelled spike rack (a real model is the later pass).
- Avalanche's crush holds positions directly (a rigged climb animation is later).
- Corvo's bolt, Highwayman's flame, and the ring are line/particle visuals.
- The flame/blob/pellet sounds are sample + pitch, not bespoke audio.
