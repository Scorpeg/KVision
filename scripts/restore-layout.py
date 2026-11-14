#!/usr/bin/env python3
"""Restore ONE layout from an older copy of KVision.conf, leaving everything else as it is.

  List the layouts in a copy:
      restore-layout.py list KVision.conf.daily-2026-11-13
  Compare every copy with the current file (shows which copies differ for a layout):
      restore-layout.py compare 5
  Restore layout number 5 from a copy:
      restore-layout.py restore 5 KVision.conf.daily-2026-11-13
  (Layouts are matched by position. It refuses when that position holds a different layout in the copy; --force overrides.)

Copy names without a folder are looked for in ~/.config/KVision. Close KVision first: it writes the settings
when it exits and would overwrite the change. The current file is copied to KVision.conf.before-layout-restore-<time>
before anything is written, and only the one layout is replaced (the other layouts, recorders and settings are not touched).
"""
import glob
import json
import os
import shutil
import subprocess
import sys
import time

CONFIG_DIR = os.environ.get("KVISION_CONFIG_DIR") or os.path.expanduser("~/.config/KVision")
CONFIG = os.path.join(CONFIG_DIR, "KVision.conf")
DEC = json.JSONDecoder()


def resolve(name):
    return name if os.path.dirname(name) else os.path.join(CONFIG_DIR, name)


def models_line(path):
    with open(path, errors="surrogateescape") as f:
        for line in f:
            if line.startswith("models="):
                return line.rstrip("\n")
    sys.exit("No layouts (models=) found in " + path)


def unescape(line):
    v = line[len("models="):]
    if len(v) < 2 or v[0] != '"' or v[-1] != '"':
        sys.exit("The models line is not in the expected form.")
    return v[1:-1].replace('\\"', '"').replace("\\\\", "\\")


def escape(s):
    return 'models="' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def spans(s):
    """The start and end of each layout's text inside the JSON list, so one can be swapped without rewriting the rest."""
    i, out = 1, []
    while True:
        _, e = DEC.raw_decode(s, i)
        out.append((i, e))
        i = e
        if s[i] == ",":
            i += 1
        else:
            return out


def load(path):
    return json.loads(unescape(models_line(path)))


def describe(i, l):
    used = sum(1 for x in l.get("items", []) if x.get("url"))
    name = l.get("name") or ("(no name, recorder %s)" % l.get("nvrIp") if l.get("nvrIp") else "(no name)")
    return "%2d  %-34s %dx%d  %d cameras" % (i, name, l["size"]["width"], l["size"]["height"], used)


def cmd_list(path):
    print(path)
    for i, l in enumerate(load(resolve(path))):
        print("  " + describe(i, l))


def cmd_compare(n):
    cur = load(CONFIG)
    if n >= len(cur):
        sys.exit("The current file has only %d layouts." % len(cur))
    print("Current:  " + describe(n, cur[n]))
    files = sorted(glob.glob(os.path.join(CONFIG_DIR, "KVision.conf*")), key=os.path.getmtime)
    for p in files:
        if p == CONFIG:
            continue
        try:
            m = load(p)
        except Exception:
            continue
        when = time.strftime("%d %b %Y %H:%M", time.localtime(os.path.getmtime(p)))
        if n >= len(m):
            print("  %-48s %s  (no layout %d)" % (os.path.basename(p), when, n))
        else:
            print("  %-48s %s  %s  %s" % (os.path.basename(p), when, "same as now" if m[n] == cur[n] else "DIFFERENT",
                                          describe(n, m[n]).split("  ", 1)[1].strip()))


def window_open():
    """True when a KVision window (not the background service) is running."""
    out = subprocess.run(["ps", "-eo", "comm,args"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        comm = line.split(None, 1)[0] if line.strip() else ""
        if comm.lower() == "kvision" and "--headless-events" not in line:
            return True
    return False


def same_kind(a, b):
    """A layout in an old copy is the same one when its name and recorder match (they are matched by position)."""
    return (a.get("name") or "") == (b.get("name") or "") and (a.get("nvrIp") or "") == (b.get("nvrIp") or "")


def cmd_restore(n, src, force=False):
    if window_open():
        sys.exit("KVision is open. Close it first (it would overwrite this change when it exits).")
    src = resolve(src)
    cur_line = models_line(CONFIG)
    cur_s = unescape(cur_line)
    src_s = unescape(models_line(src))
    cur_sp, src_sp = spans(cur_s), spans(src_s)
    if n >= len(cur_sp) or n >= len(src_sp):
        sys.exit("Layout %d does not exist in both files (current has %d, the copy has %d)." % (n, len(cur_sp), len(src_sp)))
    a, b = cur_sp[n]
    sa, sb = src_sp[n]
    new_s = cur_s[:a] + src_s[sa:sb] + cur_s[b:]
    cur, new = json.loads(cur_s), json.loads(new_s)
    changed = [i for i in range(len(cur)) if cur[i] != new[i]]
    if changed not in ([n], []):
        sys.exit("Safety stop: more than layout %d would change (%s). Nothing written." % (n, changed))
    if not force and not same_kind(cur[n], json.loads(src_s)[n]):
        sys.exit("Layout %d in that copy is a different layout (%s), so it is not the one you mean.\n"
                 "Use 'compare %d' to find a copy where it matches, or add --force to replace it anyway."
                 % (n, describe(n, json.loads(src_s)[n]).split("  ", 1)[1].strip(), n))
    print("Now:      " + describe(n, cur[n]))
    print("Restored: " + describe(n, new[n]))
    if changed == []:
        print("They are the same already. Nothing to do.")
        return
    keep = CONFIG + ".before-layout-restore-" + time.strftime("%Y-%m-%d_%H%M%S")
    shutil.copy2(CONFIG, keep)
    with open(CONFIG, errors="surrogateescape") as f:
        text = f.read()
    if text.count(cur_line) != 1:
        sys.exit("Could not find the layouts line exactly once. Nothing written.")
    with open(CONFIG, "w", errors="surrogateescape") as f:
        f.write(text.replace(cur_line, escape(new_s)))
    check = load(CONFIG)
    if check[n] != new[n] or len(check) != len(cur):
        shutil.copy2(keep, CONFIG)
        sys.exit("The result did not check out, the original file was put back.")
    print("Done. Only layout %d was replaced. The file as it was before is %s" % (n, keep))


def main(a):
    if len(a) == 3 and a[1] == "list":
        return cmd_list(a[2])
    if len(a) == 3 and a[1] == "compare" and a[2].isdigit():
        return cmd_compare(int(a[2]))
    if len(a) in (4, 5) and a[1] == "restore" and a[2].isdigit() and (len(a) == 4 or a[4] == "--force"):
        return cmd_restore(int(a[2]), a[3], len(a) == 5)
    print(__doc__)


main(sys.argv)
