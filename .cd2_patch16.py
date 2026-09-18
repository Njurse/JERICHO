import io

TB = chr(9)
NL = chr(10)


def T(n):
    return TB * n


def load(p):
    raw = io.open(p, encoding="utf-8", newline="").read()
    return raw, ("\r\n" in raw)


def save(p, body, crlf):
    if crlf:
        body = body.replace("\n", "\r\n")
    io.open(p, "w", encoding="utf-8", newline="").write(body)


P = "JERICHO/MODS/cainescrossfire/ai/opponent.c"
raw, crlf = load(P)
body = raw.replace("\r\n", "\n")


def ed(old, new, tag, count=1):
    global body
    assert body.count(old) == count, (tag, body.count(old), count)
    body = body.replace(old, new)
    print("ok:", tag)


# --- 1. engage range: less sticky, and drop the duplicated define ---------
ed("#define CD2_AI_ENGAGE_RANGE" + T(1) + "12000" + T(1) + "// close to this and it commits to a fight" + NL +
   T(5) + "//    (was 3000 - so little of the map counted as" + NL +
   T(5) + "//    'a target' that they never actually engaged)" + NL +
   "#define CD2_AI_ENGAGE_KEEP" + T(2) + "18000" + T(1) + "// ...and stays committed out to here (hysteresis)" + NL +
   "#define CD2_AI_ENGAGE_KEEP" + T(2) + "18000" + T(1) + "// ...and stays committed out to here (hysteresis)",
   "#define CD2_AI_ENGAGE_RANGE" + T(1) + "7000" + T(1) + "// close to this and it commits to a fight" + NL +
   "#define CD2_AI_ENGAGE_KEEP" + T(2) + "11000" + T(1) + "// ...and stays committed out to here (hysteresis)." + NL +
   T(5) + "//    These were 12000/18000, which on a city map" + NL +
   T(5) + "//    means a target is always in range, so the" + NL +
   T(5) + "//    contest never left the area it spawned in.",
   "engage range")

# --- 2. longer roam legs, plus a regroup distance for fleeing -------------
ed("#define CD2_AI_ROAM_MIN" + T(3) + "15000" + T(1) + "// roam goal: nearest acceptable road node" + NL +
   "#define CD2_AI_ROAM_MAX" + T(3) + "70000" + T(1) + "// roam goal: furthest acceptable road node" + NL +
   "#define CD2_AI_GOAL_TICKS" + T(2) + "1200" + T(1) + "// frames before a roam goal is re-picked",
   "#define CD2_AI_ROAM_MIN" + T(3) + "30000" + T(1) + "// roam goal: nearest acceptable road node" + NL +
   "#define CD2_AI_ROAM_MAX" + T(3) + "150000" + T(1) + "// roam goal: furthest acceptable road node -" + NL +
   T(5) + "//    big enough to reach across a level" + NL +
   "#define CD2_AI_FLEE_RUN_MIN" + T(2) + "30000" + T(1) + "// fleeing: nearest regroup node" + NL +
   "#define CD2_AI_FLEE_RUN_MAX" + T(2) + "150000" + T(1) + "// fleeing: furthest regroup node" + NL +
   "#define CD2_AI_GOAL_TICKS" + T(2) + "1800" + T(1) + "// frames before a roam goal is re-picked",
   "roam ranges")

# --- 3. aggression burst: shorter, so it breaks off and travels ----------
ed("#define CD2_AI_ENGAGE_TICKS" + T(1) + "1620" + T(1) + "// frames of sustained aggression before breaking off",
   "#define CD2_AI_ENGAGE_TICKS" + T(1) + "900" + T(1) + "// frames of sustained aggression before breaking off," + NL +
   T(5) + "//    after which it goes travelling for ROAM_TICKS.",
   "engage ticks")

# --- 4. per-frame behaviour clocks ---------------------------------------
ed(T(1) + "// opening spread counts down in real frames" + NL +
   T(1) + "if (sDisperseTicks > 0)" + NL +
   T(2) + "sDisperseTicks--;",
   T(1) + "// opening spread counts down in real frames" + NL +
   T(1) + "if (sDisperseTicks > 0)" + NL +
   T(2) + "sDisperseTicks--;" + NL + NL +
   T(1) + "// Same for the behaviour clocks. These two are what stop a contest" + NL +
   T(1) + "// spending its whole life in one firefight: a fight that drags on is" + NL +
   T(1) + "// broken off, and the car goes and drives somewhere else for a while." + NL +
   T(1) + "if (sRoamTicks > 0)" + NL +
   T(2) + "sRoamTicks--;" + NL + NL +
   T(1) + "if (sState == CD2_AI_ATTACK)" + NL +
   T(1) + "{" + NL +
   T(2) + "if (++sEngageTicks > CD2_AI_ENGAGE_TICKS)" + NL +
   T(2) + "{" + NL +
   T(3) + "sEngageTicks = 0;" + NL +
   T(3) + "sRoamTicks = CD2_AI_ROAM_TICKS + cd2AiRand(CD2_AI_ROAM_JITTER);" + NL +
   T(3) + "sGoalTimer = 0;\t\t// pick somewhere new to go" + NL +
   T(2) + "}" + NL +
   T(1) + "}" + NL +
   T(1) + "else" + NL +
   T(1) + "{" + NL +
   T(2) + "// measures CONSECUTIVE frames of aggression" + NL +
   T(2) + "sEngageTicks = 0;" + NL +
   T(1) + "}",
   "behaviour clocks")

# --- 5. honour the roam window in the state decision ---------------------
ed(T(2) + "else if (targetId >= 0 && targetD2 < (long long)engage * engage)",
   T(2) + "else if (sRoamTicks > 0)" + NL +
   T(2) + "{" + NL +
   T(3) + "// Just came off a fight. Travel - and deliberately do NOT re-acquire" + NL +
   T(3) + "// a target while this runs, or it locks straight back on and never" + NL +
   T(3) + "// goes anywhere." + NL +
   T(3) + "want = CD2_AI_ROAM;" + NL +
   T(2) + "}" + NL +
   T(2) + "else if (targetId >= 0 && targetD2 < (long long)engage * engage)",
   "roam window")

save(P, body, crlf)

# --- 6. nav.c: pick the FARTHEST acceptable node -------------------------
N = "JERICHO/MODS/cainescrossfire/ai/nav.c"
raw, crlf = load(N)
body = raw.replace("\r\n", "\n")


def ed2(old, new, tag, count=1):
    global body
    assert body.count(old) == count, (tag, body.count(old), count)
    body = body.replace(old, new)
    print("ok:", tag)


ed2(T(1) + "for (pass = 0; pass < sNodeCount; pass++)" + NL +
    T(1) + "{" + NL +
    T(2) + "int d;" + NL + NL +
    T(2) + "i = (sRotate + pass) % sNodeCount;" + NL + NL +
    T(2) + "if (!sNodes[i].hasPos)" + NL +
    T(3) + "continue;" + NL + NL +
    T(2) + "d = cd2NavDist2D(from->vx, from->vz, sNodes[i].x, sNodes[i].z);" + NL + NL +
    T(2) + "if (d >= minDist && d <= maxDist)" + NL +
    T(2) + "{" + NL +
    T(3) + "sRotate = i + 1;" + NL +
    T(3) + "out->vx = sNodes[i].x;" + NL +
    T(3) + "out->vy = from->vy;" + NL +
    T(3) + "out->vz = sNodes[i].z;" + NL +
    T(3) + "return 1;" + NL +
    T(2) + "}" + NL +
    T(1) + "}" + NL + NL +
    T(1) + "return 0;",
    T(1) + "// Take the FARTHEST acceptable node rather than the first one in index" + NL +
    T(1) + "// order. Index order follows the road layout, so scanning for the first" + NL +
    T(1) + "// match kept handing back destinations from the same corner of the map -" + NL +
    T(1) + "// which is exactly why the contest never left its own neighbourhood." + NL +
    T(1) + "for (pass = 0; pass < sNodeCount; pass++)" + NL +
    T(1) + "{" + NL +
    T(2) + "int d;" + NL + NL +
    T(2) + "i = (sRotate + pass) % sNodeCount;" + NL + NL +
    T(2) + "if (!sNodes[i].hasPos)" + NL +
    T(3) + "continue;" + NL + NL +
    T(2) + "d = cd2NavDist2D(from->vx, from->vz, sNodes[i].x, sNodes[i].z);" + NL + NL +
    T(2) + "if (d < minDist || d > maxDist)" + NL +
    T(3) + "continue;" + NL + NL +
    T(2) + "if (best < 0 || d > bestD)" + NL +
    T(2) + "{" + NL +
    T(3) + "best = i;" + NL +
    T(3) + "bestD = d;" + NL +
    T(2) + "}" + NL +
    T(1) + "}" + NL + NL +
    T(1) + "if (best < 0)" + NL +
    T(2) + "return 0;" + NL + NL +
    T(1) + "sRotate = best + 1;" + NL +
    T(1) + "out->vx = sNodes[best].x;" + NL +
    T(1) + "out->vy = from->vy;" + NL +
    T(1) + "out->vz = sNodes[best].z;" + NL +
    T(1) + "return 1;",
    "nav farthest")

ed2(T(1) + "static int sRotate = -1;" + T(1) + "// -1 = not yet seeded for this run" + NL +
    T(1) + "int pass, i, d;",
    T(1) + "static int sRotate = -1;" + T(1) + "// -1 = not yet seeded for this run" + NL +
    T(1) + "int pass, i, d;" + NL +
    T(1) + "int best = -1, bestD = 0;",
    "nav best vars")

save(N, body, crlf)
print("done")
