import os
from pathlib import Path
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
source = (repo / "wl-src/src/wl/sys/wl_cfg80211_hybrid.c").read_text(encoding="utf-8")
signature = "static s32\nwl_bss_connect_done("
start = source.index(signature)
root = Path(tempfile.mkdtemp(prefix="wl-connect-tests-", dir=os.environ.get("TMPDIR", "/var/tmp")))
(root / "connect-under-test.h").write_text(source[start:source.index("\n}\n", start) + 3], encoding="utf-8")
subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-fsanitize=address,undefined", "-g",
                "-I", str(root), str(repo / "tests/connect.c"), "-o", str(root / "connect")], check=True)
subprocess.run([str(root / "connect")], check=True)
