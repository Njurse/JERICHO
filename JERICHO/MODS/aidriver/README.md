# AI Driver — general-purpose AI driver for the player's car

A JERICHO DLL addon that makes the engine's AI drivers behave the way a
player would when the sandbox hands the player's car to them
(`Player AI Mode: Traffic / Cop / Lead`).

Zero coupling: the module reads the AI mode from the game-global
`g_PlayerControlMode` (which the sandbox menu writes) and observes the
game state — no cross-module calls, no `gaildrv2` involvement.

## Behaviors

| Situation                                   | Behavior |
|---------------------------------------------|----------|
| Joyride (no threat)                         | Free-flowing: never parks, never stops at lights/yields (hard obstacles still brake), pace = road limit + `pace_boost_pct` |
| Pursuit / cops close (`cop_clearance`)      | Evade: `top_speed` flat-out, runs lights, no parking — all three AI modes where the engine exposes a knob |
| Chase mission (`Mission.ChaseTarget` valid) | Tail the target car, holding `pursuit_dist` (±`pursuit_margin`) behind it, steering lock `max_steer` |

Control-type coverage:

- **Traffic (`CIV_AI`)** — full override: `maxSpeed`, `thrustState` /
  `ctrlState` / `ctrlNode` un-sticking, direct throttle when the AI re-set
  a stop mid-frame.
- **Cop (`PURSUER_AI`)** — `desiredSpeed` pinned wide open when threatened
  (the cop AI's power already scales with the felony).
- **Lead (`LEAD_AI`)** — the road-follower drives its own route at mission
  pace; no engine knob to force speed, so it is left to drive.

## Tunables (`JERICHO/CONFIG/aidriver.ini`, read live)

| Key                | Default | Meaning |
|--------------------|---------|---------|
| `enabled`          | 1       | master switch |
| `pace_boost_pct`   | 25      | joyride pace above the road limit (%) |
| `top_speed`        | 255     | evade speed cap (civ `maxSpeed`, u_char 0..255) |
| `run_lights`       | 1       | free-flowing: skip parking / lights / yields |
| `pursuit_dist`     | 4000    | desired tail distance behind the chase target (world units) |
| `pursuit_margin`   | 800     | tail-distance deadband |
| `cop_clearance`    | 6000    | a cop closer than this triggers evade (world units) |
| `max_steer`        | 512     | tailing steering lock (game units) |

The sandbox menu can adjust these live with
`jer_config_set_int("aidriver", "<key>", value)` — the module re-reads
them every `AI_CONFIG_REFRESH` frames.

## Timing

- `JER_EVENT_PRE_SIM` (top of `StepSim`, before the AI control loop): state
  overrides the AI reads this frame (`maxSpeed`, stop-state clears).
- `JER_EVENT_FRAME` (inside `GlobalTimeStep`, after the AI computed
  thrust/steer, before `StepCars` integrates them): un-sticks stops the AI
  re-set mid-frame and applies the pursuit tailing controller directly.

## Build

```
JERICHO\build_mods.bat        (or the in-game "Compile Mods" button)
```

Enable the module from Options → JERICHO (mods menu), or set
`default-enabled = true` in `mod.toml`.
