# SPDX-License-Identifier: GPL-3.0-or-later
"""Install, rollback and diagnostics of the KEDAVRA Gamescope runtime, run from the
Decky backend (as root). Everything comes from the plugin zip: runtime/ holds the
patched gamescope, gamescopectl and their bundled libraries.

Install never modifies the read-only system image:
  * the runtime is copied to /var/lib/kedavra/runtime (writable, not nosuid);
  * a boot unit bind-mounts runtime/bin/gamescope over /usr/bin/gamescope before the
    Gaming Mode session starts; the mount lives in memory only;
  * boot watchdog: a boot that mounts KEDAVRA starts a "trial". If the patched
    Gamescope never writes its status file during that boot, the next boot skips
    the mount and records an automatic rollback. So a Gaming Mode that does not come
    up is fixed by holding the power button and rebooting.
"""
import json
import os
import shutil
import subprocess
import time
import zipfile
from pathlib import Path

UNIT_NAME = "kedavra-gamescope.service"
LOADER = "ld-linux-x86-64.so.2"
SYS_NICE_BIT = 23

BOOT_SH = """#!/bin/sh
# KEDAVRA boot step (written by the Decky plugin). Bind-mounts the patched Gamescope
# over /usr/bin/gamescope for this boot only, with an automatic rollback guard.
S={state}
BIN={runtime}/bin/gamescope
LOADER={runtime}/lib/{loader}
mkdir -p "$S"
[ -f "$S/disabled" ] && exit 0
if [ -f "$S/trial" ] && [ ! -f "$S/healthy" ]; then
	date -u +%Y-%m-%dT%H:%M:%SZ > "$S/auto_rollback"
	echo "previous boot with KEDAVRA never became healthy" >> "$S/auto_rollback"
	touch "$S/disabled"
	rm -f "$S/trial"
	exit 0
fi
rm -f "$S/healthy"
[ -x "$BIN" ] && [ -x "$LOADER" ] || exit 0
if ! "$LOADER" --list "$BIN" > "$S/ldd" 2>&1 || grep -q 'not found' "$S/ldd"; then
	mv "$S/ldd" "$S/ldd_failed"
	exit 0
fi
"$BIN" --version > "$S/version" 2>&1 || exit 0
findmnt -n /usr/bin/gamescope >/dev/null 2>&1 || mount --bind "$BIN" /usr/bin/gamescope
touch "$S/trial"
"""

UNIT = """[Unit]
Description=KEDAVRA frame generation: patched Gamescope for this boot
Before=display-manager.service graphical.target
After=local-fs.target

[Service]
Type=oneshot
ExecStart=/bin/sh {boot}
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
"""


# Decky's own runtime sets these for its bundled libraries (an older libstdc++ among
# them). Inherited by our tools they override the runtime's RUNPATH: gamescopectl then
# fails with "undefined symbol ... GLIBCXX_3.4.35".
LOADER_VARS = ("LD_LIBRARY_PATH", "LD_PRELOAD", "LD_AUDIT", "PYTHONHOME", "PYTHONPATH")


def clean_env(env=None):
    env = dict(os.environ if env is None else env)
    for k in LOADER_VARS:
        env.pop(k, None)
    return env


def _run(argv, timeout=20, env=None, **kw):
    try:
        r = subprocess.run(argv, capture_output=True, text=True, timeout=timeout, env=clean_env(env), **kw)
        return r.returncode, (r.stdout or "") + (r.stderr or "")
    except (OSError, subprocess.TimeoutExpired) as e:
        return -1, str(e)


class Installer:
    def __init__(self, plugin_dir, home, uid=1000, root=Path("/"), run=_run):
        self.plugin_dir = Path(plugin_dir)
        self.bundle = self.plugin_dir / "runtime"
        self.home = Path(home)
        self.uid = uid
        self.root = Path(root)
        self.var = self.root / "var/lib/kedavra"
        self.runtime = self.var / "runtime"
        self.state = self.var / "state"
        self.unit = self.root / "etc/systemd/system" / UNIT_NAME
        self.boot = self.var / "boot.sh"
        self.target = self.root / "usr/bin/gamescope"
        self.status_file = self.root / f"run/user/{uid}/kedavra-status.json"
        self.run = run

    # ------------------------------------------------------------------ helpers

    def bundled_version(self):
        p = self.bundle / "VERSION"
        return p.read_text().strip() if p.exists() else "unknown"

    def installed_version(self):
        p = self.runtime / "VERSION"
        return p.read_text().strip() if p.exists() else None

    @staticmethod
    def _loader(runtime, *args):
        # The runtime carries its own glibc. The system ldd would check it against the
        # system glibc, so the bundled loader resolves it instead, from the runtime's own
        # lib/ (its interpreter and RUNPATH name the installed location only).
        lib = Path(runtime) / "lib"
        return [str(lib / LOADER), "--library-path", str(lib), *map(str, args)]

    @staticmethod
    def _fix_modes(runtime):
        # Decky unpacks the zip without file modes; the loader must be executable.
        for d in ("bin", "lib"):
            for f in (Path(runtime) / d).iterdir():
                f.chmod(0o755)

    def _missing_libs(self, runtime):
        runtime = Path(runtime)
        if not (runtime / "lib" / LOADER).exists():
            return [f"runtime has no lib/{LOADER}: the zip was built by an old version"]
        rc, out = self.run(self._loader(runtime, "--list", runtime / "bin/gamescope"))
        missing = [l.strip() for l in out.splitlines() if "not found" in l]
        if rc != 0 and not missing:
            missing = [out.strip()[-400:] or f"the loader exited with code {rc}"]
        return missing

    def _mounted(self):
        rc, out = self.run(["findmnt", "-n", "-o", "SOURCE", str(self.target)])
        return rc == 0 and out.strip() != ""

    def _gamescope_procs(self):
        procs = []
        proc = self.root / "proc"
        try:
            entries = list(proc.iterdir())
        except OSError:
            return procs
        for d in entries:
            if not d.name.isdigit():
                continue
            try:
                if (d / "comm").read_text().strip() not in ("gamescope", "gamescope-wl"):
                    continue
                st = os.stat(d / "exe")
                cap = 0
                for line in (d / "status").read_text().splitlines():
                    if line.startswith("CapEff:"):
                        cap = int(line.split()[1], 16)
                procs.append({"pid": int(d.name), "dev": st.st_dev, "ino": st.st_ino,
                              "sys_nice": bool(cap >> SYS_NICE_BIT & 1)})
            except (OSError, ValueError):
                continue
        return procs

    def _ours(self, p):
        try:
            st = os.stat(self.runtime / "bin/gamescope")
        except OSError:
            return False
        return p["dev"] == st.st_dev and p["ino"] == st.st_ino

    def status_age(self):
        try:
            return time.time() - self.status_file.stat().st_mtime
        except OSError:
            return None

    def mark_healthy(self):
        """Called periodically by the plugin: the patched Gamescope is writing status."""
        age = self.status_age()
        if age is not None and age < 5 and (self.state / "trial").exists():
            (self.state / "healthy").touch()
            return True
        return False

    # ------------------------------------------------------------------ actions

    def install(self):
        if not (self.bundle / "bin/gamescope").exists():
            raise RuntimeError("the plugin zip has no runtime/bin/gamescope")
        if not self.target.exists():
            raise RuntimeError(f"{self.target} not found: is this SteamOS Gaming Mode?")
        self.var.mkdir(parents=True, exist_ok=True)
        tmp = self.var / "runtime.new"
        if tmp.exists():
            shutil.rmtree(tmp)
        shutil.copytree(self.bundle, tmp)
        self._fix_modes(tmp)
        missing = self._missing_libs(tmp)
        if missing:
            shutil.rmtree(tmp)
            raise RuntimeError("Gamescope KEDAVRA would not start, missing libraries: " + "; ".join(missing[:6]))
        rc, out = self.run(self._loader(tmp, tmp / "bin/gamescope", "--version"))
        if rc != 0:
            shutil.rmtree(tmp)
            raise RuntimeError("Gamescope KEDAVRA --version failed: " + out.strip()[-400:])
        # A running KEDAVRA session keeps its open (unlinked) files; the new copy is
        # used from the next boot.
        if self.runtime.exists():
            shutil.rmtree(self.runtime)
        tmp.rename(self.runtime)
        rc, out = self.run(["setcap", "cap_sys_nice=eip", str(self.runtime / "bin/gamescope")])
        if rc != 0:
            raise RuntimeError("setcap: " + out.strip()[-300:])
        self.state.mkdir(parents=True, exist_ok=True)
        for f in ("disabled", "trial", "healthy", "auto_rollback", "ldd_failed"):
            (self.state / f).unlink(missing_ok=True)
        self.boot.write_text(BOOT_SH.format(state=self.state, runtime=self.runtime, loader=LOADER))
        self.unit.parent.mkdir(parents=True, exist_ok=True)
        self.unit.write_text(UNIT.format(boot=self.boot))
        self.run(["systemctl", "daemon-reload"])
        rc, out = self.run(["systemctl", "enable", UNIT_NAME])
        if rc != 0:
            raise RuntimeError("systemctl enable: " + out.strip()[-300:])
        return {"installed": self.bundled_version(), "needs_reboot": True,
                "message": "Installed. Reboot the Deck: Gaming Mode will start with Gamescope KEDAVRA."}

    def uninstall(self):
        self.run(["systemctl", "disable", UNIT_NAME])
        self.unit.unlink(missing_ok=True)
        self.run(["systemctl", "daemon-reload"])
        self.state.mkdir(parents=True, exist_ok=True)
        (self.state / "disabled").touch()
        mounted = self._mounted()
        return {"needs_reboot": mounted,
                "message": "Removed. Reboot the Deck to return to the stock Gamescope." if mounted
                else "Removed. The stock Gamescope is active."}

    def reboot(self):
        self.run(["systemctl", "reboot"])
        return {"rebooting": True}

    # ------------------------------------------------------------------ diagnostics

    def diagnose(self):
        checks = []

        def add(key, ok, text, detail=""):
            checks.append({"key": key, "ok": ok, "text": text, "detail": detail})

        rc, osr = self.run(["cat", str(self.root / "etc/os-release")])
        osv = {k: v.strip('"') for k, _, v in (l.partition("=") for l in osr.splitlines()) if k}
        add("os", osv.get("ID") == "steamos", f"OS: {osv.get('PRETTY_NAME', '?')} {osv.get('VERSION_ID', '')} {osv.get('BUILD_ID', '')}")
        rc, pkg = self.run(["pacman", "-Q", "gamescope", "mesa", "vulkan-radeon"])
        add("packages", rc == 0, "Packages: " + " · ".join(pkg.split("\n")[:3]))

        bundled = self.bundle / "bin/gamescope"
        try:
            self._fix_modes(self.bundle)
        except OSError:
            pass
        add("bundle", bundled.exists(), f"Plugin carries Gamescope KEDAVRA {self.bundled_version()}"
            if bundled.exists() else "The plugin zip has no runtime/: build the full zip")
        if bundled.exists():
            missing = self._missing_libs(self.bundle)
            add("libs", not missing, "Libraries on this SteamOS: all found" if not missing
                else "Missing libraries", "\n".join(missing))

        inst = self.installed_version()
        add("installed", inst is not None, f"Installed: {inst}" if inst else "Not installed")
        if inst and inst != self.bundled_version():
            add("outdated", False, f"Installed Gamescope {inst}, plugin carries {self.bundled_version()}: "
                "press Reinstall and reboot")
        if inst:
            missing = self._missing_libs(self.runtime)
            add("installed_libs", not missing, "Installed Gamescope finds its libraries" if not missing
                else "Installed Gamescope is missing libraries", "\n".join(missing))
        rc, en = self.run(["systemctl", "is-enabled", UNIT_NAME])
        add("unit", en.strip() == "enabled", f"Boot step: {en.strip() or 'none'}")
        for f, text in (("auto_rollback", "An automatic rollback happened"), ("ldd_failed", "Libraries were missing at boot")):
            p = self.state / f
            if p.exists():
                add(f, False, text, p.read_text()[-600:])
        if (self.state / "disabled").exists():
            add("disabled", False, "KEDAVRA is disabled until reinstalled")

        mounted = self._mounted()
        add("mounted", mounted, "Gamescope KEDAVRA is mounted for this boot" if mounted
            else "The stock Gamescope is active now (reboot after installing)")
        procs = self._gamescope_procs()
        if not procs:
            add("running", False, "No gamescope process found")
        for p in procs:
            ours = self._ours(p)
            add("running", ours, f"gamescope pid {p['pid']}: " + ("KEDAVRA" if ours else "stock"))
            add("sys_nice", p["sys_nice"], "CAP_SYS_NICE present (realtime GPU queue)" if p["sys_nice"]
                else "No CAP_SYS_NICE: the GPU queue is not realtime")
        age = self.status_age()
        add("status", age is not None and age < 3, "Engine status is fresh" if age is not None and age < 3
            else ("Engine status is %.0f s old" % age if age is not None else "No engine status file"))
        return {"checks": checks, "ok": all(c["ok"] for c in checks if c["key"] in ("libs", "mounted", "running", "status"))}

    def report(self, out_dir, dest_dir=None):
        """Diagnostics text in out_dir, plus one zip for sending on the Desktop (dest_dir):
        the diagnostics, the latest pacing traces with their analyses and the benchmark."""
        d = self.diagnose()
        out_dir = Path(out_dir)
        out_dir.mkdir(parents=True, exist_ok=True)
        stamp = time.strftime("%Y%m%d-%H%M%S", time.gmtime())
        path = out_dir / f"diagnostics-{stamp}.txt"
        lines = [("OK  " if c["ok"] else "FAIL ") + c["text"] + (("\n     " + c["detail"].replace("\n", "\n     ")) if c["detail"] else "")
                 for c in d["checks"]]
        for argv in (["findmnt", "-n", str(self.target)], ["journalctl", "-b", "-u", UNIT_NAME, "--no-pager"],
                     ["journalctl", "-b", "--no-pager", "-n", "120", "-g", "gamescope|kfg"]):
            rc, out = self.run(argv)
            lines.append(f"\n$ {' '.join(argv)}\n{out[-6000:]}")
        st = self.status_file
        if st.exists():
            lines.append("\nstatus: " + st.read_text())
        path.write_text("\n".join(lines) + "\n")
        self._own(out_dir, path)
        dest = Path(dest_dir) if dest_dir else self.home / "Desktop"
        try:
            dest.mkdir(parents=True, exist_ok=True)
            bundle = dest / f"GSF-report-{stamp}.zip"
            traces = sorted((out_dir / "captures").glob("*.csv"), reverse=True)[:3]
            with zipfile.ZipFile(bundle, "w", zipfile.ZIP_DEFLATED) as z:
                z.write(path, path.name)
                for t in traces:
                    z.write(t, "captures/" + t.name)
                    if t.with_suffix(".json").exists():
                        z.write(t.with_suffix(".json"), "captures/" + t.with_suffix(".json").name)
                if (out_dir / "bench.json").exists():
                    z.write(out_dir / "bench.json", "bench.json")
            self._own(dest, bundle)
        except OSError:
            # No Desktop folder we can write: the text report in out_dir still exists.
            return {"path": str(path), "diagnose": d}
        return {"path": str(bundle), "diagnose": d}

    def _own(self, *paths):
        for p in paths:
            try:
                os.chown(p, self.uid, self.uid)
            except OSError:
                pass


def bench(plugin_dir, out_dir, run=_run):
    b = Path(plugin_dir) / "bench"
    out = Path(out_dir) / "bench.json"
    Path(out_dir).mkdir(parents=True, exist_ok=True)
    try:
        (b / "kfg_vktest").chmod(0o755)  # Decky unpacks the zip without file modes
    except OSError:
        return {"ok": False, "log": "the plugin zip has no bench/kfg_vktest", "json": ""}
    rc, text = run([str(b / "kfg_vktest"), "--spv", str(b), "--bench", "200", "--json", str(out)], timeout=600)
    summary = {"ok": rc == 0, "log": text[-3000:], "json": str(out)}
    for line in text.splitlines():
        if line.startswith("analysis "):
            parts = line.split()
            try:
                summary["analysis_ms"] = float(parts[1])
                summary["two_outputs_ms"] = float(parts[4])
            except (IndexError, ValueError):
                pass
        if line.startswith("PASS:"):
            summary["correct"] = "0 failure" in line
    try:
        summary["device"] = json.loads(out.read_text()).get("device")
    except (OSError, ValueError):
        pass
    return summary
