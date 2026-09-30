import importlib.util
spec = importlib.util.spec_from_file_location('ae', 'arenaedit.py')
ae = importlib.util.module_from_spec(spec); spec.loader.exec_module(ae)
D = "C:/Users/Jaret/Documents/Projects/REDRIVER2/DriverLevelTool/"
print("%-8s %-20s %-20s %-20s" % ("city", "400", "800", "2000"), flush=True)
for c in ("CHICAGO", "HAVANA", "RIO", "VEGAS"):
    row = []
    for s in (400, 800, 2000):
        _img, _r, st = ae._build_textured(c, D + "%s_LEVELMODEL.obj" % c, (s, s), 1, False)
        row.append("mirror=%-5s %.3f" % (st['u_mirrored'], st['uv_agree']))
    print("%-8s %-20s %-20s %-20s" % tuple([c] + row), flush=True)
print("DONE", flush=True)
