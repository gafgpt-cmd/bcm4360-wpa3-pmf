import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

repo = Path(__file__).resolve().parent.parent
root = Path(tempfile.mkdtemp(prefix="wl-dkms-tests-", dir=os.environ.get("TMPDIR", "/var/tmp")))
KERNELS = ["7.0.1-1-test", "7.0.2-1-test"]
STOCK = "broadcom-wl/6.30.223.271"
OURS = "wl-wpa3/1.2"
STOCK_CONF = 'PACKAGE_NAME="broadcom-wl"\nPACKAGE_VERSION="6.30.223.271"\nAUTOINSTALL="yes"\n' \
    'MAKE[0]="make KVER=${kernelver}"\nBUILT_MODULE_NAME[0]="wl"\n' \
    'DEST_MODULE_LOCATION[0]="/kernel/drivers/net/wireless"\n'

stub = root / "stubs"
stub.mkdir()
dispatcher = stub / "dispatch"
dispatcher.write_text("#!" + sys.executable + "\n" + r'''
import json, os, sys
from pathlib import Path
name = Path(sys.argv[0]).name
args = sys.argv[1:]
state = Path(os.environ["TEST_STATE"])
with (state / "calls").open("a", encoding="utf-8") as log:
    log.write(json.dumps([name] + args) + "\n")
if name == "id":
    print(os.environ.get("TEST_UID", "0"))
elif name == "sha256sum":
    sys.exit(0)
elif name == "modinfo":
    print("/stub/" + args[1] + "/wl.ko.zst" if "filename" in args else "STUBSRCVERSION")
elif name == "dkms":
    db_path = state / "dkms.json"
    db = json.loads(db_path.read_text(encoding="utf-8"))
    action = args[0]
    kver = args[args.index("-k") + 1] if "-k" in args else None
    if action == "status":
        only = args[args.index("-m") + 1] if "-m" in args else None
        for nv, kernels in sorted(db.items()):
            if only and nv.split("/")[0] != only:
                continue
            if not kernels:
                print(nv + ": added")
            for k, st in sorted(kernels.items()):
                print("%s, %s, x86_64: %s" % (nv, k, st))
        sys.exit(0)
    nv = args[1]
    if action == "add":
        db.setdefault(nv, {})
    elif action == "build":
        if kver == os.environ.get("FAIL_BUILD"):
            sys.exit(10)
        db.setdefault(nv, {})[kver] = "built"
    elif action == "install":
        db.setdefault(nv, {})[kver] = "installed"
    elif action == "remove":
        db.pop(nv, None)
    db_path.write_text(json.dumps(db), encoding="utf-8")
''', encoding="utf-8")
dispatcher.chmod(0o755)
for tool in ["id", "sha256sum", "modinfo", "dkms"]:
    (stub / tool).symlink_to(dispatcher)


def case(name, stock_installed=True):
    base = root / name
    tree = base / "repo"
    for part in ["dkms", "patches", "wl-src"]:
        shutil.copytree(repo / part, tree / part, ignore=shutil.ignore_patterns("lib"))
    (tree / "wl-src/src/stale.o").write_bytes(b"old object")
    blob = base / "blob"
    blob.write_bytes(b"pristine blob stand-in")
    sysroot = base / "sysroot"
    (sysroot / "usr/src/broadcom-wl-6.30.223.271").mkdir(parents=True)
    (sysroot / "usr/src/broadcom-wl-6.30.223.271/dkms.conf").write_text(STOCK_CONF, encoding="utf-8")
    (sysroot / "usr/src/facetimehd-0.7.2").mkdir()
    (sysroot / "usr/src/facetimehd-0.7.2/dkms.conf").write_text(
        'PACKAGE_NAME="facetimehd"\nPACKAGE_VERSION="0.7.2"\nBUILT_MODULE_NAME[0]="facetimehd"\n', encoding="utf-8")
    for k in KERNELS:
        (sysroot / "lib/modules" / k / "build/include").mkdir(parents=True)
    (sysroot / "lib/modules/6.0.0-headerless/kernel").mkdir(parents=True)
    state = base / "state"
    state.mkdir()
    db = {STOCK: {k: "installed" for k in KERNELS}} if stock_installed else {}
    (state / "dkms.json").write_text(json.dumps(db), encoding="utf-8")
    env = dict(os.environ, PATH=str(stub) + ":" + os.environ["PATH"], TEST_STATE=str(state),
               WL_WPA3_SYSROOT=str(sysroot))
    return tree, blob, sysroot, state, env


def run(tree, env, *args):
    return subprocess.run(["bash", str(tree / "dkms/install.sh"), *args], env=env,
                          capture_output=True, text=True)


def calls(state):
    path = state / "calls"
    lines = path.read_text(encoding="utf-8").splitlines() if path.exists() else []
    return [json.loads(line) for line in lines if json.loads(line)[0] == "dkms" and json.loads(line)[1] != "status"]


def db(state):
    return json.loads((state / "dkms.json").read_text(encoding="utf-8"))


# install: patched built for every kernel with headers before stock is touched; stock excluded
tree, blob, sysroot, state, env = case("install")
result = run(tree, env, "--blob", str(blob))
assert result.returncode == 0, result.stderr
seq = calls(state)
builds = [i for i, c in enumerate(seq) if c[1:3] == ["build", OURS]]
stock_remove = seq.index(["dkms", "remove", STOCK, "--all"])
installs = [i for i, c in enumerate(seq) if c[1:3] == ["install", OURS]]
assert [seq[i][4] for i in builds] == KERNELS
assert max(builds) < stock_remove < min(installs)
assert all("--force" in seq[i] for i in installs) and [seq[i][4] for i in installs] == KERNELS
assert db(state) == {OURS: {k: "installed" for k in KERNELS}}
override = (sysroot / "etc/dkms/broadcom-wl.conf").read_text(encoding="utf-8").splitlines()
assert override[0].startswith("# wl-wpa3 dkms/install.sh") and override[-1] == 'BUILD_EXCLUSIVE_KERNEL="^$"'
assert not (sysroot / "etc/dkms/facetimehd.conf").exists()
assert "blacklist brcmsmac" in (sysroot / "etc/modprobe.d/wl-wpa3.conf").read_text(encoding="utf-8")
src = sysroot / "usr/src/wl-wpa3-1.2"
assert (src / "lib/wlc_hybrid.o_amd64.orig").read_bytes() == blob.read_bytes()
assert os.access(src / "dkms-prebuild.sh", os.X_OK) and (src / "dkms.conf").exists()
assert (src / "patches/wl_patch.py").exists() and (src / "src/wl/sys/wl_cfg80211_hybrid.c").exists()
assert not (src / "src/stale.o").exists()
assert "next boot" in result.stdout

# rerun is idempotent: same end state, own override accepted
result = run(tree, env, "--blob", str(blob))
assert result.returncode == 0, result.stderr
assert db(state) == {OURS: {k: "installed" for k in KERNELS}}
assert sorted(p.name for p in (sysroot / "usr/src").iterdir()) == \
    ["broadcom-wl-6.30.223.271", "facetimehd-0.7.2", "wl-wpa3-1.2"]

# remove: stock rebuilt for every kernel, every trace of ours gone
result = run(tree, env, "--remove")
assert result.returncode == 0, result.stderr
assert db(state) == {STOCK: {k: "installed" for k in KERNELS}}
assert not (sysroot / "etc/dkms/broadcom-wl.conf").exists()
assert not (sysroot / "etc/modprobe.d/wl-wpa3.conf").exists()
assert not src.exists()

# build failure: stock untouched, nothing of ours left behind
tree, blob, sysroot, state, env = case("build-fails")
env["FAIL_BUILD"] = KERNELS[1]
result = run(tree, env, "--blob", str(blob))
assert result.returncode != 0 and "stock driver not touched" in result.stderr
assert db(state) == {STOCK: {k: "installed" for k in KERNELS}}
assert not (sysroot / "etc/dkms").exists() and not (sysroot / "etc/modprobe.d").exists()
assert not (sysroot / "usr/src/wl-wpa3-1.2").exists()
assert ["dkms", "remove", STOCK, "--all"] not in calls(state)

# a foreign override for the stock package is never overwritten
tree, blob, sysroot, state, env = case("foreign-override")
(sysroot / "etc/dkms").mkdir(parents=True)
(sysroot / "etc/dkms/broadcom-wl.conf").write_text("# admin's own\n", encoding="utf-8")
result = run(tree, env, "--blob", str(blob))
assert result.returncode != 0 and "merge it by hand" in result.stderr
assert calls(state) == [] and db(state) == {STOCK: {k: "installed" for k in KERNELS}}

# refusals before any change: not root, missing blob, unknown argument
tree, blob, sysroot, state, env = case("refusals")
for args, extra, message in [(["--blob", str(blob)], {"TEST_UID": "1000"}, "sudo"),
                             (["--blob", str(blob) + ".missing"], {}, "not found"),
                             (["--bogus"], {}, "unknown argument")]:
    result = run(tree, dict(env, **extra), *args)
    assert result.returncode != 0 and message in result.stderr, (args, result.stderr)
assert calls(state) == [] and not (sysroot / "usr/src/wl-wpa3-1.2").exists()

# stage: no root, no DKMS calls, refuses an existing directory
tree, blob, sysroot, state, env = case("stage")
staged = root / "stage/wl-wpa3-1.2"
result = run(tree, dict(env, TEST_UID="1000"), "--stage", str(staged), "--blob", str(blob))
assert result.returncode == 0, result.stderr
assert (staged / "dkms.conf").exists() and not (staged / "src/stale.o").exists()
assert run(tree, env, "--stage", str(staged), "--blob", str(blob)).returncode != 0
assert calls(state) == []

print("DKMS: patched wl built for every kernel before stock is excluded; stock override written, rerun idempotent")
print("DKMS: --remove restores stock on every kernel; build failure and foreign override leave stock untouched")
print("DKMS: refusals and staging make no DKMS changes; staged tree has no stale objects")
