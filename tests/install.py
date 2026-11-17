import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

repo = Path(__file__).resolve().parent.parent
root = Path(tempfile.mkdtemp(prefix="wl-install-tests-", dir=os.environ.get("TMPDIR", "/var/tmp")))
stub = root / "stubs"
stub.mkdir()
dispatcher = stub / "dispatch"
dispatcher.write_text("#!" + sys.executable + "\n" + r'''
import hashlib, json, os, sys
from pathlib import Path
name = Path(sys.argv[0]).name
args = sys.argv[1:]
state = Path(os.environ["TEST_STATE"])
with (state / "calls").open("a", encoding="utf-8") as log:
    log.write(json.dumps([name] + args) + "\n")

def one(key, default=None):
    path = state / key
    return path.read_text(encoding="utf-8").strip() if path.exists() else default

def loaded_write(value):
    (state / "sys/module/wl/srcversion").write_text(value + "\n", encoding="utf-8")

def dependencies():
    path = state / "dependencies"
    return json.loads(path.read_text(encoding="utf-8")) if path.exists() else []

if name in ("mktemp", "install", "mv"):
    restoring = any(".wl-wpa3-restore." in arg for arg in args)
    failure = os.environ.get("FAIL_RESTORE_OP")
    if restoring and failure == name:
        sys.exit(1)
    if restoring and name == "mv" and failure == "silent-mv":
        sys.exit(0)
    os.execv(os.environ["REAL_" + name.upper()], [name] + args)
elif name == "id":
    print(os.environ.get("TEST_UID", "0"))
elif name == "uname":
    print(os.environ.get("TEST_KERNEL", "test-kernel"))
elif name == "modinfo":
    if args and args[0] == "-n":
        installed = one("installed_path")
        if not installed:
            sys.exit(1)
        print(installed)
    elif "-F" in args:
        field = args[args.index("-F") + 1]
        target = args[-1]
        candidate = "candidate" in Path(target).parts
        if field == "depends":
            print("cfg80211,aes_cbc_macs" if candidate else "cfg80211")
        elif field == "vermagic":
            kernel = "test-kernel" if candidate else one("installed_kernel", "test-kernel")
            print(kernel + " SMP preempt")
        elif field == "srcversion":
            print("candidate-src" if candidate else "stock-src")
        else:
            sys.exit(1)
    else:
        sys.exit(1)
elif name == "findmnt":
    print(state / "sys")
elif name == "systemd-path":
    print(state / "run")
elif name == "systemd-run":
    if os.environ.get("FAIL_TIMER"):
        sys.exit(1)
    installed = one("installed_path")
    if installed and Path(installed).is_file():
        (state / "timer_snapshot").write_text(
            hashlib.sha256(Path(installed).read_bytes()).hexdigest() + "\n", encoding="utf-8"
        )
    (state / "timer").write_text("armed", encoding="utf-8")
    (state / "timer_args").write_text(json.dumps(args), encoding="utf-8")
elif name == "systemctl":
    if args and args[0] == "is-active":
        unit = args[-1]
        if unit.startswith("wl-wpa3-restore."):
            sys.exit(0 if (state / "recovery_active").exists() else 3)
        if unit.startswith("wl-wpa3-install-restore."):
            sys.exit(0 if (state / "timer").exists() else 3)
        sys.exit(3)
    if args and args[0] == "stop":
        if any(unit.startswith("wl-wpa3-install-restore.") for unit in args[1:]):
            (state / "timer").unlink(missing_ok=True)
        sys.exit(0)
    sys.exit(0)
elif name == "depmod":
    counter = state / "depmod_count"
    count = int(counter.read_text(encoding="utf-8")) + 1 if counter.exists() else 1
    counter.write_text(str(count), encoding="utf-8")
    mode = os.environ.get("FAIL_DEPMOD")
    if mode == "always" or (mode == "first" and count == 1):
        sys.exit(1)
    temporary = state / "module-index.tmp"
    descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o666)
    os.close(descriptor)
    os.replace(temporary, state / "modules.dep")
elif name == "modprobe":
    if args == ["wl"]:
        loaded_write(os.environ.get("MODPROBE_VERSION", "stock-src"))
    elif args and args[0] != os.environ.get("FAIL_DEPENDENCY"):
        (state / "dependencies").write_text(json.dumps(dependencies() + args), encoding="utf-8")
    else:
        sys.exit(1)
elif name == "rmmod":
    if args != ["wl"]:
        sys.exit(1)
    if os.environ.get("FAIL_UNLOAD"):
        sys.exit(1)
    loaded_write("")
    (state / "dependencies").write_text("[]", encoding="utf-8")
elif name == "insmod":
    target = args[0]
    candidate = "candidate" in Path(target).parts
    if candidate and os.environ.get("FAIL_LOAD"):
        sys.exit(1)
    required = {"cfg80211", "aes_cbc_macs"} if candidate else {"cfg80211"}
    if not required.issubset(set(dependencies())):
        sys.exit(1)
    if candidate:
        loaded_write("candidate-other" if os.environ.get("WRONG_IDENTITY") else "candidate-src")
    else:
        loaded_write("wrong-version" if os.environ.get("WRONG_BACKUP") else "stock-src")
elif name == "nmcli":
    if args[:2] == ["-g", "GENERAL.STATE"] and args[2:4] == ["device", "show"]:
        states = json.loads(os.environ.get("DEVICE_READY_STATES", '["30"]'))
        counter = state / "device_queries"
        count = int(counter.read_text(encoding="utf-8")) if counter.exists() else 0
        counter.write_text(str(count + 1), encoding="utf-8")
        value = states[min(count, len(states) - 1)]
        if value == "missing":
            print("Error: device not found", file=sys.stderr)
            sys.exit(10)
        if value in ("30", "40", "50", "60", "70", "80", "90", "100", "120"):
            (state / "device_ready").write_text("1", encoding="utf-8")
        print(value + " (device state)")
    elif args[:2] == ["-g", "UUID,DEVICE"] and args[2:] == ["connection", "show", "--active"]:
        active = state / "active"
        if active.exists():
            print(active.read_text(encoding="utf-8").strip())
    elif args[:2] == ["-g", "connection.type"]:
        print(one("profile_type", "802-11-wireless"))
    elif args[:2] == ["-g", "connection.uuid"] and args[2:4] == ["connection", "show"]:
        print(one("profile_uuid", "uuid-selected"))
    elif args[:2] == ["connection", "up"]:
        if os.environ.get("DEVICE_READY_STATES") and not (state / "device_ready").exists():
            print("Error: device not found for connection", file=sys.stderr)
            sys.exit(10)
        if os.environ.get("FAIL_RECONNECT"):
            sys.exit(1)
        if os.environ.get("NO_CONNECT"):
            (state / "active").write_text("", encoding="utf-8")
        else:
            iface = args[args.index("ifname") + 1] if "ifname" in args else ""
            (state / "active").write_text(one("profile_uuid", "uuid-selected") + ":" + iface + "\n",
                                           encoding="utf-8")
    else:
        sys.exit(1)
else:
    raise AssertionError(name)
''', encoding="utf-8")
dispatcher.chmod(0o700)
for name in ("id", "uname", "modinfo", "findmnt", "systemd-path", "systemd-run",
             "systemctl", "depmod", "modprobe", "rmmod", "insmod", "nmcli",
             "mktemp", "install", "mv"):
    (stub / name).symlink_to(dispatcher)

STOCK = b"stock module"
CANDIDATE = b"candidate module"


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def manifest(directory, names):
    text = "".join(sha(directory / name) + "  " + name + "\n" for name in names)
    (directory / "SHA256SUMS").write_text(text, encoding="utf-8")


def write_manifests(bundle):
    manifest(bundle / "backup", ["wl.ko", "KERNEL", "SRCVERSION"])
    manifest(bundle, ["trial.sh", "install.sh", "revert.sh", "backup/wl.ko", "backup/KERNEL",
                      "backup/SRCVERSION", "backup/SHA256SUMS", "candidate/wl.ko"])


def write_ready(bundle):
    text = "".join(sha(bundle / name) + "  " + name + "\n"
                   for name in ("install.sh", "candidate/wl.ko"))
    (bundle / "READY_FOR_INSTALL").write_text(text, encoding="utf-8")


def case(name, fmt="ko", interfaces=("wlan0",), active="uuid-selected:wlan0",
         profile_type="802-11-wireless"):
    state = root / name
    bundle = state / "bundle with spaces"
    for path in (bundle / "backup", bundle / "candidate", state / "sys/module/wl",
                 state / "sys/class/net", state / "drivers/wl", state / "run",
                 state / "home"):
        path.mkdir(parents=True)
    for interface in interfaces:
        driver = state / "sys/class/net" / interface / "device"
        driver.mkdir(parents=True)
        (driver / "driver").symlink_to(state / "drivers/wl")
    (state / "calls").write_text("", encoding="utf-8")
    (state / "dependencies").write_text("[]", encoding="utf-8")
    (state / "active").write_text(active + "\n" if active else "", encoding="utf-8")
    (state / "profile_type").write_text(profile_type + "\n", encoding="utf-8")
    (state / "profile_uuid").write_text("uuid-selected\n", encoding="utf-8")
    (state / "installed_kernel").write_text("test-kernel\n", encoding="utf-8")
    (state / "sys/module/wl/srcversion").write_text("stock-src\n", encoding="utf-8")
    for script in ("trial.sh", "install.sh", "revert.sh"):
        shutil.copy2(repo / "recovery" / script, bundle / script)
    (bundle / "backup/wl.ko").write_bytes(STOCK)
    (bundle / "candidate/wl.ko").write_bytes(CANDIDATE)
    (bundle / "backup/KERNEL").write_text("test-kernel\n", encoding="utf-8")
    (bundle / "backup/SRCVERSION").write_text("stock-src\n", encoding="utf-8")
    module_dir = state / "lib/modules/test-kernel/kernel/drivers/net/wireless"
    module_dir.mkdir(parents=True)
    if fmt == "zst":
        installed = module_dir / "wl.ko.zst"
        subprocess.run(["zstd", "-q", "-f", "-o", str(installed)], input=STOCK,
                       check=True, capture_output=True)
    elif fmt == "xz":
        installed = module_dir / "wl.ko.xz"
        installed.write_bytes(STOCK)
    else:
        installed = module_dir / "wl.ko"
        installed.write_bytes(STOCK)
    (state / "installed_path").write_text(str(installed) + "\n", encoding="utf-8")
    write_manifests(bundle)
    write_ready(bundle)
    env = dict(os.environ, PATH=str(stub) + os.pathsep + os.environ["PATH"],
               TEST_STATE=str(state), HOME=str(state / "home"), TEST_UID="0",
               TEST_KERNEL="test-kernel")
    for tool in ("mktemp", "install", "mv"):
        env["REAL_" + tool.upper()] = shutil.which(tool)
    for key in ("FAIL_TIMER", "FAIL_LOAD", "FAIL_UNLOAD", "FAIL_DEPENDENCY", "FAIL_DEPMOD",
                "FAIL_RECONNECT", "FAIL_MODPROBE", "WRONG_BACKUP", "WRONG_IDENTITY",
                "NO_CONNECT", "MODPROBE_VERSION", "INSTALLED_VERSION", "FAIL_RESTORE_OP",
                "DEVICE_READY_STATES"):
        env.pop(key, None)
    return state, bundle, env, installed, installed.read_bytes()


def run(bundle, env, *args, script="install.sh"):
    return subprocess.run(["bash", str(bundle / script), *args], env=env, text=True,
                          capture_output=True, timeout=30)


def calls(state):
    return [json.loads(line) for line in
            (state / "calls").read_text(encoding="utf-8").splitlines()]


def untouched(state, bundle, installed, original):
    assert installed.read_bytes() == original
    assert not (bundle / "backup/installed").exists()
    assert not (state / "timer").exists()
    assert not any(c[0] in ("systemd-run", "depmod", "modprobe", "rmmod", "insmod")
                   for c in calls(state))


state, bundle, env, installed, original = case("check")
result = run(bundle, env, "--check")
assert result.returncode == 0, result.stderr
assert installed.read_bytes() == original
assert not (bundle / "backup/installed").exists()
assert not (state / "timer").exists()
assert not any(c[0] in ("systemd-run", "depmod", "modprobe", "rmmod", "insmod")
               for c in calls(state))

for args in (("--bogus",), ("--check", "extra"), ("--connection",),
             ("--check", "--check"), ("--connection", "a", "--connection", "b")):
    state, bundle, env, installed, original = case("invalid-" + "-".join(a.strip("-") for a in args))
    result = run(bundle, env, *args)
    assert result.returncode == 2, args
    assert "Usage" in result.stderr, args
    untouched(state, bundle, installed, original)

state, bundle, env, installed, original = case("missing-approval")
(bundle / "READY_FOR_INSTALL").unlink()
for args in (("--check",), ()):
    result = run(bundle, env, *args)
    assert result.returncode != 0
    assert "READY_FOR_INSTALL" in result.stderr
    untouched(state, bundle, installed, original)

state, bundle, env, installed, original = case("approval-mismatch")
(bundle / "READY_FOR_INSTALL").write_text(
    "0" * 64 + "  install.sh\n" + sha(bundle / "candidate/wl.ko") + "  candidate/wl.ko\n",
    encoding="utf-8")
result = run(bundle, env, "--check")
assert result.returncode != 0 and "approval" in result.stderr
untouched(state, bundle, installed, original)

state, bundle, env, installed, original = case("approval-coverage")
(bundle / "READY_FOR_INSTALL").write_text(
    sha(bundle / "candidate/wl.ko") + "  candidate/wl.ko\n", encoding="utf-8")
result = run(bundle, env, "--check")
assert result.returncode != 0 and "install.sh" in result.stderr
untouched(state, bundle, installed, original)

state, bundle, env, installed, original = case("tampered-candidate")
(bundle / "candidate/wl.ko").write_bytes(b"tampered candidate")
result = run(bundle, env, "--check")
assert result.returncode != 0 and "integrity" in result.stderr
untouched(state, bundle, installed, original)

state, bundle, env, installed, original = case("delayed-device")
env["DEVICE_READY_STATES"] = '["missing", "20", "30"]'
result = run(bundle, env)
assert result.returncode == 0, ("device rediscovery must precede activation", result.stderr)
assert installed.read_bytes() == CANDIDATE
assert int((state / "device_queries").read_text(encoding="utf-8")) == 3
assert (state / "modules.dep").stat().st_mode & 0o777 == 0o644
assert (bundle / "backup/installed/module").stat().st_mode & 0o777 == 0o600
assert not (state / "timer").exists()

state, bundle, env, installed, original = case("device-never-ready")
env["DEVICE_READY_STATES"] = '["20"]'
result = run(bundle, env)
assert result.returncode != 0
assert installed.read_bytes() == original
assert (state / "sys/module/wl/srcversion").read_text(encoding="utf-8").strip() == "stock-src"
assert not any(c[:3] == ["nmcli", "connection", "up"] for c in calls(state))
assert (state / "modules.dep").stat().st_mode & 0o777 == 0o644
assert not (state / "timer").exists()

state, bundle, env, installed, original = case("install-ko")
result = run(bundle, env)
assert result.returncode == 0, result.stderr
events = calls(state)
index = lambda tool: next(i for i, c in enumerate(events) if c[0] == tool)
assert index("systemd-run") < index("rmmod") < index("insmod")
timer_args = json.loads((state / "timer_args").read_text(encoding="utf-8"))
assert "--on-active=120s" in timer_args
assert Path(timer_args[-1]).parent.parent == state / "run"
assert (state / "timer_snapshot").read_text(encoding="utf-8").strip() == \
    hashlib.sha256(original).hexdigest()
assert installed.read_bytes() == CANDIDATE
assert (state / "sys/module/wl/srcversion").read_text(encoding="utf-8").strip() == "candidate-src"
assert (bundle / "backup/installed/module").read_bytes() == original
assert not (state / "timer").exists()
reconnect = [c for c in events if c[:3] == ["nmcli", "connection", "up"]]
assert reconnect and "uuid-selected" in reconnect[0] and "ifname" in reconnect[0]
assert "wlan0" in reconnect[0]
assert any(c[0] == "depmod" for c in events)
assert any(c[0] == "systemctl" and c[1] == "stop" for c in events)
assert "Confirm actual Wi-Fi internet" in result.stdout
rollback = subprocess.run(timer_args[-2:], env=env, text=True, capture_output=True, timeout=30)
assert rollback.returncode == 0, rollback.stderr
assert installed.read_bytes() == original
assert (state / "sys/module/wl/srcversion").read_text(encoding="utf-8").strip() == "stock-src"
assert (state / "modules.dep").stat().st_mode & 0o777 == 0o644

state, bundle, env, installed, original = case("install-zst", fmt="zst")
result = run(bundle, env)
assert result.returncode == 0, result.stderr
decoded = subprocess.run(["zstd", "-dc", str(installed)], check=True,
                         capture_output=True).stdout
assert decoded == CANDIDATE
assert (bundle / "backup/installed/module").read_bytes() == original
assert (state / "sys/module/wl/srcversion").read_text(encoding="utf-8").strip() == "candidate-src"

state, bundle, env, installed, original = case("reject-xz", fmt="xz")
result = run(bundle, env)
assert result.returncode != 0 and "Unsupported installed module format" in result.stderr
untouched(state, bundle, installed, original)

state, bundle, env, installed, original = case("reject-relative-path")
(state / "installed_path").write_text("relative/wl.ko\n", encoding="utf-8")
result = run(bundle, env)
assert result.returncode != 0 and "not absolute" in result.stderr
untouched(state, bundle, installed, original)

state, bundle, env, installed, original = case("wrong-installed-identity")
installed.write_bytes(b"different stock module")
result = run(bundle, env)
assert result.returncode != 0 and "identity is unexpected" in result.stderr
untouched(state, bundle, installed, installed.read_bytes())

state, bundle, env, installed, original = case("install-kernel-mismatch")
(bundle / "backup/KERNEL").write_text("other-kernel\n", encoding="utf-8")
write_manifests(bundle)
result = run(bundle, env)
assert result.returncode != 0 and "Kernel changed" in result.stderr
untouched(state, bundle, installed, original)

state, bundle, env, installed, original = case("timer-failure")
env["FAIL_TIMER"] = "1"
result = run(bundle, env)
assert result.returncode != 0 and "rollback timer" in result.stderr
assert installed.read_bytes() == original
assert not (state / "timer").exists()
assert not any(c[0] in ("depmod", "modprobe", "rmmod", "insmod") for c in calls(state))

state, bundle, env, installed, original = case("load-failure")
env["FAIL_LOAD"] = "1"
result = run(bundle, env)
assert result.returncode != 0
assert installed.read_bytes() == original
assert (state / "sys/module/wl/srcversion").read_text(encoding="utf-8").strip() == "stock-src"
assert not (state / "timer").exists()

state, bundle, env, installed, original = case("unload-failure")
env["FAIL_UNLOAD"] = "1"
result = run(bundle, env)
assert result.returncode != 0 and "busy" in result.stderr
assert installed.read_bytes() == original
assert (state / "sys/module/wl/srcversion").read_text(encoding="utf-8").strip() == "stock-src"
assert not any(c[0] == "insmod" and "candidate" in c[-1] for c in calls(state))

state, bundle, env, installed, original = case("depmod-failure")
env["FAIL_DEPMOD"] = "first"
result = run(bundle, env)
assert result.returncode != 0 and "depmod" in result.stderr
assert installed.read_bytes() == original
assert (state / "sys/module/wl/srcversion").read_text(encoding="utf-8").strip() == "stock-src"
assert int((state / "depmod_count").read_text(encoding="utf-8")) >= 2

state, bundle, env, installed, original = case("reconnect-failure")
env["FAIL_RECONNECT"] = "1"
result = run(bundle, env)
assert result.returncode != 0 and "reconnect" in result.stderr
assert installed.read_bytes() == original
assert (state / "sys/module/wl/srcversion").read_text(encoding="utf-8").strip() == "stock-src"

state, bundle, env, installed, original = case("not-connected")
env["NO_CONNECT"] = "1"
result = run(bundle, env)
assert result.returncode != 0 and "not connected" in result.stderr
assert installed.read_bytes() == original
assert (state / "sys/module/wl/srcversion").read_text(encoding="utf-8").strip() == "stock-src"

state, bundle, env, installed, original = case("wrong-loaded-identity")
env["WRONG_IDENTITY"] = "1"
result = run(bundle, env)
assert result.returncode != 0 and "identity does not match" in result.stderr
assert installed.read_bytes() == original
assert (state / "sys/module/wl/srcversion").read_text(encoding="utf-8").strip() == "stock-src"

state, bundle, env, installed, original = case("ambiguous-profile",
                                               active="uuid-a:wlan0\nuuid-b:wlan0")
result = run(bundle, env)
assert result.returncode != 0 and "--connection" in result.stderr
untouched(state, bundle, installed, original)
state, bundle, env, installed, original = case("explicit-profile",
                                               active="uuid-a:wlan0\nuuid-b:wlan0")
result = run(bundle, env, "--connection", "saved-profile")
assert result.returncode == 0, result.stderr

state, bundle, env, installed, original = case("no-interface", interfaces=())
result = run(bundle, env)
assert result.returncode != 0 and "cannot identify" in result.stderr
untouched(state, bundle, installed, original)

state, bundle, env, installed, original = case("not-wireless", profile_type="802-3-ethernet")
result = run(bundle, env)
assert result.returncode != 0 and "not a saved Wi-Fi profile" in result.stderr
untouched(state, bundle, installed, original)

state, bundle, env, installed, original = case("not-root")
env["TEST_UID"] = "1000"
result = run(bundle, env)
assert result.returncode != 0 and "sudo bash" in result.stderr
untouched(state, bundle, installed, original)

state, bundle, env, installed, original = case("recovery-active")
(state / "recovery_active").write_text("1", encoding="utf-8")
result = run(bundle, env)
assert result.returncode != 0 and "Recovery is still armed" in result.stderr
assert installed.read_bytes() == original
assert not (state / "timer").exists()
assert not any(c[0] in ("systemd-run", "depmod", "modprobe", "rmmod", "insmod")
               for c in calls(state))

state, bundle, env, installed, original = case("another-install")
lock_fd = os.open(state / "run/wl-wpa3-install.lock", os.O_WRONLY | os.O_CREAT, 0o600)
fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
try:
    result = run(bundle, env)
finally:
    fcntl.flock(lock_fd, fcntl.LOCK_UN)
    os.close(lock_fd)
assert result.returncode != 0 and "Another installation" in result.stderr
assert installed.read_bytes() == original
assert not (state / "timer").exists()

state, bundle, env, installed, original = case("manual-restore")
assert run(bundle, env).returncode == 0
before = len(calls(state))
result = run(bundle, env, script="revert.sh")
assert result.returncode == 0, result.stderr
assert installed.read_bytes() == original
assert (state / "sys/module/wl/srcversion").read_text(encoding="utf-8").strip() == "stock-src"
assert any(c[0] == "depmod" for c in calls(state)[before:])

state, bundle, env, installed, original = case("already-loaded-stock")
assert run(bundle, env).returncode == 0
(state / "sys/module/wl/srcversion").write_text("stock-src\n", encoding="utf-8")
before = len(calls(state))
result = run(bundle, env, script="revert.sh")
assert result.returncode == 0, result.stderr
assert "already loaded" in result.stdout
assert installed.read_bytes() == original
assert not any(c[0] in ("rmmod", "insmod") for c in calls(state)[before:])

state, bundle, env, installed, original = case("revert-kernel-mismatch")
assert run(bundle, env).returncode == 0
env["TEST_KERNEL"] = "other-kernel"
before = len(calls(state))
result = run(bundle, env, script="revert.sh")
assert result.returncode == 0, result.stderr
assert installed.read_bytes() == original
assert not any(c[0] == "insmod" for c in calls(state)[before:])
assert any(c == ["modprobe", "wl"] for c in calls(state)[before:])

state, bundle, env, installed, original = case("busy-revert")
assert run(bundle, env).returncode == 0
env["FAIL_UNLOAD"] = "1"
result = run(bundle, env, script="revert.sh")
assert result.returncode != 0 and "Reboot" in result.stderr
assert installed.read_bytes() == original
assert not any(c[0] == "insmod" for c in calls(state)[-3:])

state, bundle, env, installed, original = case("repeat-noop")
assert run(bundle, env).returncode == 0
saved_state = (bundle / "backup/installed/module").read_bytes()
before = len(calls(state))
result = run(bundle, env)
assert result.returncode == 0, result.stderr
assert "already installed" in result.stdout
assert (bundle / "backup/installed/module").read_bytes() == saved_state
assert not any(c[0] in ("systemd-run", "rmmod", "insmod", "depmod")
               for c in calls(state)[before:])

state, bundle, env, installed, original = case("repeat-reinstall")
assert run(bundle, env).returncode == 0
saved_state = (bundle / "backup/installed/module").read_bytes()
(state / "sys/module/wl/srcversion").write_text("stock-src\n", encoding="utf-8")
before = len(calls(state))
result = run(bundle, env)
assert result.returncode == 0, result.stderr
assert (bundle / "backup/installed/module").read_bytes() == saved_state
assert any(c[0] == "rmmod" for c in calls(state)[before:])
assert installed.read_bytes() == CANDIDATE

for fmt in ("ko", "zst"):
    for failure in ("mktemp", "install", "mv", "silent-mv", "depmod", "directory"):
        state, bundle, env, installed, original = case("restore-failure-" + fmt + failure, fmt=fmt)
        assert run(bundle, env).returncode == 0
        candidate_encoded = installed.read_bytes()
        if failure == "depmod":
            env["FAIL_DEPMOD"] = "always"
        elif failure == "directory":
            installed.rename(state / "displaced-module")
            installed.parent.rename(state / "displaced-directory")
        else:
            env["FAIL_RESTORE_OP"] = failure
        before = len(calls(state))
        result = run(bundle, env, script="revert.sh")
        assert result.returncode != 0, (failure, result.stdout, result.stderr)
        assert "Installed module file restored" not in result.stdout
        assert "could not restore" in result.stderr
        assert not any(c[0] in ("rmmod", "insmod") for c in calls(state)[before:])
        if failure != "directory":
            assert installed.read_bytes() == (original if failure == "depmod" else candidate_encoded)

for failure in ("mktemp", "install", "mv", "silent-mv", "depmod"):
    state, bundle, env, installed, original = case("rollback-failure-" + failure)
    env["FAIL_LOAD"] = "1"
    if failure == "depmod":
        env["FAIL_DEPMOD"] = "always"
    else:
        env["FAIL_RESTORE_OP"] = failure
    result = run(bundle, env)
    assert result.returncode != 0
    assert "disk restoration is not confirmed" in result.stderr
    assert "Reboot uses" not in result.stderr
    assert (state / "timer").exists()
    assert installed.read_bytes() == (original if failure == "depmod" else CANDIDATE)

state, bundle, env, installed, original = case("revert-metadata-tamper")
assert run(bundle, env).returncode == 0
(bundle / "backup/installed/INSTALL_PATH").write_text(str(state / "other/wl.ko") + "\n",
                                                       encoding="utf-8")
result = run(bundle, env, script="revert.sh")
assert result.returncode != 0 and "checksum" in result.stderr
assert installed.read_bytes() == CANDIDATE

state, bundle, env, installed, original = case("revert-unsafe-path")
assert run(bundle, env).returncode == 0
(bundle / "backup/installed/INSTALL_PATH").write_text("relative/wl.ko\n", encoding="utf-8")
manifest(bundle / "backup/installed",
         ["module", "INSTALL_PATH", "FORMAT", "KERNEL", "SRCVERSION"])
result = run(bundle, env, script="revert.sh")
assert result.returncode != 0 and "not absolute" in result.stderr
assert installed.read_bytes() == CANDIDATE

print("Install: dry run and invalid arguments make no changes; approval covers installer and candidate")
print("Install: timer armed before file replacement and unload; .ko and .ko.zst handled; other formats rejected")
print("Install: checksum, kernel, format, path and stock-identity rejection; lock and active-recovery refusal")
print("Install: timer, depmod, unload, load, reconnect and acceptance failures restore the original file and driver")
print("Install: success requires candidate identity and selected Wi-Fi connection; timer cancelled after acceptance")
print("Install: manual restore, root runtime rollback, already-loaded disk restore, kernel-mismatch restore, busy rollback, repeat installs keep original backup")
