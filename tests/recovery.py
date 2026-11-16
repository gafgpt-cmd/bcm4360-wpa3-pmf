import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

repo = Path(__file__).resolve().parent.parent
root = Path(tempfile.mkdtemp(prefix="wl-recovery-tests-", dir=os.environ.get("TMPDIR", "/var/tmp")))
stub = root / "stubs"
stub.mkdir()
dispatcher = stub / "dispatch"
dispatcher.write_text("#!" + sys.executable + "\n" + r'''
import json, os, sys
from pathlib import Path
name = Path(sys.argv[0]).name
args = sys.argv[1:]
state = Path(os.environ["TEST_STATE"])
version = state / "sys/module/wl/srcversion"
with (state / "calls").open("a", encoding="utf-8") as log:
    log.write(json.dumps([name] + args) + "\n")
if name == "id": print("0")
elif name == "uname": print("test-kernel")
elif name == "modinfo": print("test-kernel SMP " if "vermagic" in args else "stock-version")
elif name == "findmnt": print(state / "sys")
elif name == "systemd-path": print(state / "run")
elif name == "systemd-run":
    if os.environ.get("FAIL_TIMER"): sys.exit(1)
    (state / "timer").write_text("armed", encoding="utf-8")
    (state / "timer_args").write_text(json.dumps(args), encoding="utf-8")
elif name == "systemctl": sys.exit(0 if (state / "timer").exists() else 3)
elif name == "modprobe":
    if args == ["-r", "wl"]:
        if os.environ.get("FAIL_UNLOAD"): sys.exit(1)
        version.write_text("", encoding="utf-8")
    elif args == ["wl"]:
        if os.environ.get("FAIL_STOCK"): sys.exit(1)
        version.write_text("stock-version\n", encoding="utf-8")
elif name == "insmod":
    candidate = "candidate" in Path(args[0]).parts
    if candidate and os.environ.get("FAIL_LOAD"): sys.exit(1)
    version.write_text("candidate-version\n" if candidate else "stock-version\n", encoding="utf-8")
else: raise AssertionError(name)
''', encoding="utf-8")
dispatcher.chmod(0o700)
for name in ("id", "uname", "modinfo", "findmnt", "systemd-path", "systemd-run", "systemctl", "modprobe", "insmod"):
    (stub / name).symlink_to(dispatcher)

def manifest(directory, names):
    text = "".join(hashlib.sha256((directory / n).read_bytes()).hexdigest() + "  " + n + "\n" for n in names)
    (directory / "SHA256SUMS").write_text(text, encoding="utf-8")

def case(name):
    state = root / name
    bundle = state / "bundle with spaces"
    for p in (bundle / "backup", bundle / "candidate", state / "sys/module/wl", state / "run", state / "home"):
        p.mkdir(parents=True)
    (state / "calls").write_text("", encoding="utf-8")
    for script in ("trial.sh", "revert.sh"):
        shutil.copy2(repo / "recovery" / script, bundle / script)
    (bundle / "backup/wl.ko").write_bytes(b"stock module")
    (bundle / "candidate/wl.ko").write_bytes(b"candidate module")
    candidate_hash = hashlib.sha256(b"candidate module").hexdigest()
    (bundle / "READY_FOR_TRIAL").write_text(candidate_hash + "  candidate/wl.ko\n", encoding="utf-8")
    (bundle / "backup/KERNEL").write_text("test-kernel\n", encoding="utf-8")
    (bundle / "backup/SRCVERSION").write_text("stock-version\n", encoding="utf-8")
    (state / "sys/module/wl/srcversion").write_text("stock-version\n", encoding="utf-8")
    manifest(bundle / "backup", ["wl.ko", "KERNEL", "SRCVERSION"])
    manifest(bundle, ["trial.sh", "revert.sh", "backup/wl.ko", "backup/KERNEL", "backup/SRCVERSION", "backup/SHA256SUMS", "candidate/wl.ko"])
    env = dict(os.environ, PATH=str(stub) + os.pathsep + os.environ["PATH"], TEST_STATE=str(state), HOME=str(state / "home"))
    for key in ("FAIL_TIMER", "FAIL_LOAD", "FAIL_STOCK", "FAIL_UNLOAD"):
        env.pop(key, None)
    return state, bundle, env

def run(bundle, env, script="trial.sh", *args):
    return subprocess.run(["bash", str(bundle / script), *args], env=env, text=True, capture_output=True, timeout=30)

def calls(state):
    return [json.loads(line) for line in (state / "calls").read_text(encoding="utf-8").splitlines()]

state, bundle, env = case("check")
result = run(bundle, env, "trial.sh", "--check")
assert result.returncode == 0, result.stderr
assert not any(c[0] in ("modprobe", "insmod", "systemd-run") for c in calls(state))

state, bundle, env = case("timer-failed")
env["FAIL_TIMER"] = "1"
assert run(bundle, env).returncode != 0
assert not any(c[0] in ("modprobe", "insmod") for c in calls(state))

state, bundle, env = case("successful-trial")
result = run(bundle, env)
assert result.returncode == 0, result.stderr
events = calls(state)
assert next(i for i,c in enumerate(events) if c[0] == "systemd-run") < next(i for i,c in enumerate(events) if c[:2] == ["modprobe", "-r"])
assert (state / "sys/module/wl/srcversion").read_text(encoding="utf-8").strip() == "candidate-version"
timer_args = json.loads((state / "timer_args").read_text(encoding="utf-8"))
assert "--on-active=120s" in timer_args
root_copy = Path(timer_args[-1])
assert root_copy.parent.parent == state / "run"
result = subprocess.run(timer_args[-2:], env=env, text=True, capture_output=True, timeout=30)
assert result.returncode == 0, result.stderr
assert (state / "sys/module/wl/srcversion").read_text(encoding="utf-8").strip() == "stock-version"
before = len(calls(state))
assert run(bundle, env, "revert.sh").returncode == 0
assert not any(c[0] in ("modprobe", "insmod") for c in calls(state)[before:])

state, bundle, env = case("load-failed")
env["FAIL_LOAD"] = "1"
assert run(bundle, env).returncode != 0
assert (state / "sys/module/wl/srcversion").read_text(encoding="utf-8").strip() == "stock-version"

state, bundle, env = case("backup-fallback")
(state / "sys/module/wl/srcversion").write_text("candidate-version\n", encoding="utf-8")
env["FAIL_STOCK"] = "1"
assert run(bundle, env, "revert.sh").returncode == 0
assert any(c[0] == "insmod" and "backup" in c[-1] for c in calls(state))

state, bundle, env = case("tampered-module")
(bundle / "candidate/wl.ko").write_bytes(b"corrupt")
assert run(bundle, env).returncode != 0
assert not any(c[0] in ("modprobe", "insmod", "systemd-run") for c in calls(state))

state, bundle, env = case("kernel-mismatch")
(bundle / "backup/KERNEL").write_text("other-kernel\n", encoding="utf-8")
manifest(bundle / "backup", ["wl.ko", "KERNEL", "SRCVERSION"])
manifest(bundle, ["trial.sh", "revert.sh", "backup/wl.ko", "backup/KERNEL", "backup/SRCVERSION", "backup/SHA256SUMS", "candidate/wl.ko"])
assert run(bundle, env).returncode != 0
assert not any(c[0] in ("modprobe", "insmod", "systemd-run") for c in calls(state))
print("Recovery: dry run, timer-before-unload, timer failure, load failure, automatic/manual restore, backup fallback, tamper and kernel mismatch passed")

state, bundle, env = case("unload-failed")
(state / "sys/module/wl/srcversion").write_text("candidate-version\n", encoding="utf-8")
env["FAIL_UNLOAD"] = "1"
result = run(bundle, env, "revert.sh")
assert result.returncode != 0 and "Reboot" in result.stderr
assert (state / "sys/module/wl/srcversion").read_text(encoding="utf-8").strip() == "candidate-version"
assert not any(c[0] == "insmod" for c in calls(state))

state, bundle, env = case("review-gate")
(bundle / "READY_FOR_TRIAL").rename(bundle / "REVIEWS_PENDING")
result = run(bundle, env)
assert result.returncode != 0 and "Trial disabled" in result.stderr
assert not any(c[0] in ("modprobe", "insmod", "systemd-run") for c in calls(state))
print("Recovery: busy unload gives reboot instructions; incomplete reviews disable trial")

scan = root / "scanner-control"
scan.mkdir()
control = scan / "control.c"
control.write_text("key.algo = CRYPTO_ALGO_WEP128;\n", encoding="utf-8")
scan_cmd = ["gitleaks", "dir", "--redact", "--no-banner", "--config", str(repo / ".gitleaks.toml"), str(scan)]
clean = subprocess.run(scan_cmd, capture_output=True, text=True, timeout=30)
assert clean.returncode == 0, "Public enum constant should not be a credential finding"
synthetic = hashlib.sha256(b"synthetic scanner negative control").hexdigest()
control.write_text("api_key = '" + synthetic + "'\n", encoding="utf-8")
detected = subprocess.run(scan_cmd, capture_output=True, text=True, timeout=30)
assert detected.returncode == 1, "Scanner must still detect a synthetic credential-shaped value"
print("Gitleaks configuration: public C constants allowed; synthetic credential control still detected")
