"""Test input for test.c: readings of N8 slots 1-4 from the BattBench database (read only) -> n8.csv."""
import os, sqlite3, sys

db = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.environ['LOCALAPPDATA'], 'BattBench', 'battbench.db')
c = sqlite3.connect('file:' + db + '?mode=ro', uri=True)
rows = c.execute("SELECT t, slot, mode, mv, ma, mah, res, temp, secs FROM samples "
                 "WHERE dev=1 AND slot<4 AND raw LIKE 'df%' ORDER BY t").fetchall()
with open('n8.csv', 'w') as f:
    for t, slot, mode, mv, ma, mah, res, temp, secs in rows:
        st = 0 if mode == 0 else 5 if mode in (4, 6, 8, 10, 12, 14) else (3 if ma < 0 else 2)
        f.write(f"{t:.1f},{slot},{st},{max(mv, 0)},{abs(ma)},{abs(mah)},{res},{temp},{secs}\n")
print(len(rows), 'readings')
