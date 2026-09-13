import io

TB = chr(9)
NL = chr(10)


def T(n):
    return TB * n


P = "JERICHO/MODS/combatd2/ai/opponent.c"
raw = io.open(P, encoding="utf-8", newline="").read()
crlf = "\r\n" in raw
body = raw.replace("\r\n", "\n")


def ed(old, new, tag, count=1):
    global body
    assert body.count(old) == count, (tag, body.count(old), count)
    body = body.replace(old, new)
    print("ok:", tag)


ed(T(2) + "for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)" + NL +
   T(2) + "{" + NL +
   T(3) + "if (gCarCleanModelPtr[i] != NULL && gCarDamModelPtr[i] != NULL &&" + NL +
   T(4) + "    i != pcp->ap.model)" + NL +
   T(4) + "loaded[n++] = i;" + NL +
   T(2) + "}" + NL + NL +
   T(2) + "if (n == 0)" + NL +
   T(3) + "model = pcp->ap.model;" + T(2) + "// the only car this level loaded" + NL +
   T(2) + "else" + NL +
   T(3) + "model = loaded[cd2AiRandSalt(n, index + 1)];",
   T(2) + "// A slot is only usable if the level actually loaded all THREE models" + NL +
   T(2) + "// for it. Clean on its own is not enough: CreateDentableCar needs the" + NL +
   T(2) + "// low-detail one too, and it bails with 'gCarLowModelPtr is NULL' -" + NL +
   T(2) + "// after which the half-built car gets dereferenced and the game dies." + NL +
   T(2) + "// Cities differ here, so it has to be asked, never assumed." + NL +
   T(2) + "for (i = 0; i < MAX_CAR_RESIDENT_MODELS; i++)" + NL +
   T(2) + "{" + NL +
   T(3) + "if (gCarCleanModelPtr[i] != NULL && gCarDamModelPtr[i] != NULL &&" + NL +
   T(4) + "    gCarLowModelPtr[i] != NULL && i != pcp->ap.model)" + NL +
   T(4) + "loaded[n++] = i;" + NL +
   T(2) + "}" + NL + NL +
   T(2) + "// Name the pool once per level: this is what tells you which -car ids" + NL +
   T(2) + "// are even legal in the city being tested." + NL +
   T(2) + "if (!sPoolLogged)" + NL +
   T(2) + "{" + NL +
   T(3) + "char pool[64];" + NL +
   T(3) + "int pi, pl = 0;" + NL + NL +
   T(3) + "sPoolLogged = 1;" + NL +
   T(3) + "pool[0] = 0;" + NL + NL +
   T(3) + "for (pi = 0; pi < MAX_CAR_RESIDENT_MODELS; pi++)" + NL +
   T(3) + "{" + NL +
   T(4) + "if (gCarCleanModelPtr[pi] != NULL && gCarDamModelPtr[pi] != NULL &&" + NL +
   T(5) + "    gCarLowModelPtr[pi] != NULL)" + NL +
   T(4) + "{" + NL +
   T(5) + "pool[pl++] = (char)('0' + pi);" + NL +
   T(5) + "pool[pl] = 0;" + NL +
   T(4) + "}" + NL +
   T(3) + "}" + NL + NL +
   T(3) + "printInfo(\"[combatd2] car model pool this level: [%s] (player model clean=%d dam=%d low=%d, %d slots)\\n\"," + NL +
   T(4) + "pool, gCarCleanModelPtr[pcp->ap.model] != NULL, gCarDamModelPtr[pcp->ap.model] != NULL," + NL +
   T(4) + "gCarLowModelPtr[pcp->ap.model] != NULL, MAX_CAR_RESIDENT_MODELS);" + NL +
   T(2) + "}" + NL + NL +
   T(2) + "if (n == 0)" + NL +
   T(3) + "model = pcp->ap.model;" + T(2) + "// the only car this level loaded" + NL +
   T(2) + "else" + NL +
   T(3) + "model = loaded[cd2AiRandSalt(n, index + 1)];",
   "pool enumeration")

ed("static CD2_AI_CAR sAi[CD2_AI_MAX];",
   "static CD2_AI_CAR sAi[CD2_AI_MAX];" + NL +
   "static int sPoolLogged;\t\t// one-shot: log the level's usable car models",
   "pool flag")

if crlf:
    body = body.replace("\n", "\r\n")
io.open(P, "w", encoding="utf-8", newline="").write(body)
print("done")
