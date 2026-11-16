import os
from pathlib import Path
import shlex
import subprocess
import tempfile

repo = Path(__file__).resolve().parent.parent
source = (repo / "wl-src/src/wl/sys/wl_cfg80211_hybrid.c").read_text(encoding="utf-8")
protocol = (repo / "wl-src/src/wl/sys/wl_pmf.h").read_text(encoding="utf-8")
functions = [
    (protocol, "static int wl_pmf_frame_offset("),
    (protocol, "static bool wl_pmf_replay_accept("),
    (source, "static bool wl_mgmt_replay_accept("),
    (source, "static void wl_re_ccmp_blocks("),
    (source, "static inline u64 wl_re_ccmp_pn("),
    (source, "static int wl_re_mgmt_rx("),
    (source, "static int wl_re_ba_rx("),
    (source, "static void wl_re_saq_work(struct work_struct *w)\n{"),
    (source, "static void wl_re_dd_work(struct work_struct *w)\n{"),
    (source, "void __wrap_wlc_recv("),
]
root = Path(tempfile.mkdtemp(prefix="wl-rx-tests-", dir=os.environ.get("TMPDIR", "/var/tmp")))
bodies = []
for text, signature in functions:
    start = text.index(signature)
    bodies.append(text[start:text.index("\n}\n", start) + 3])
(root / "rx-under-test.h").write_text("\n".join(bodies), encoding="utf-8")
flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "openssl"], text=True))
subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter", "-fsanitize=address,undefined", "-g",
                "-I", str(root), str(repo / "tests/rx.c"), "-o", str(root / "rx"), *flags], check=True)
subprocess.run([str(root / "rx")], check=True)
