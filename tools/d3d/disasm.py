"""Shared helpers: per-function disassembly comments from generated/default."""
import glob
import re
from functools import lru_cache
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


@lru_cache(maxsize=None)
def functions():
    funcs = {}
    cur = None
    for path in glob.glob(str(ROOT / "generated/default/skate1_recomp.*.cpp")):
        for line in open(path, errors="ignore"):
            m = re.match(r"DEFINE_REX_FUNC\(sub_([0-9A-F]+)\)", line)
            if m:
                cur = int(m.group(1), 16)
                funcs[cur] = []
                continue
            s = line.strip()
            if cur is not None and (s.startswith("// ") or s.startswith("loc_")):
                funcs[cur].append(s[3:] if s.startswith("// ") else s)
    return funcs


def registers():
    """Xenos register index -> name, from the SDK's register table."""
    names = {}
    table = ROOT / "third_party/rexglue-sdk/include/rex/graphics/register_table.inc"
    for line in open(table):
        m = re.match(r"XE_GPU_REGISTER\((0x[0-9A-Fa-f]+),\s*\w+,\s*(\w+)\)", line.strip())
        if m:
            names[int(m.group(1), 16)] = m.group(2)
    return names
