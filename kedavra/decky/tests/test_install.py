# SPDX-License-Identifier: GPL-3.0-or-later
import os
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "py_modules"))
import kedavra_install  # noqa: E402


class FakeSystem:
    """Stands in for ldd, findmnt, systemctl and setcap."""

    def __init__(self):
        self.calls = []
        self.missing = []
        self.mounted = False
        self.version_rc = 0
        self.enabled = False

    def __call__(self, argv, timeout=20, **kw):
        self.calls.append(argv)
        cmd = Path(argv[0]).name
        if cmd == kedavra_install.LOADER and "--list" in argv:
            return 0, "".join(f"\t{m} => not found\n" for m in self.missing) + "\tlibc.so.6 => /usr/lib/libc.so.6\n"
        if cmd == "findmnt":
            return (0, "/dev/x[/var/lib/kedavra/runtime/bin/gamescope]\n") if self.mounted else (1, "")
        if cmd == "systemctl":
            if argv[1] == "enable":
                self.enabled = True
            if argv[1] == "disable":
                self.enabled = False
            if argv[1] == "is-enabled":
                return 0, "enabled\n" if self.enabled else "disabled\n"
            return 0, ""
        if cmd == kedavra_install.LOADER and "--version" in argv:
            return self.version_rc, "gamescope version kedavra\n"
        if cmd == "cat":
            try:
                return 0, Path(argv[1]).read_text()
            except OSError as e:
                return 1, str(e)
        return 0, ""


class InstallerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.root = self.tmp / "root"
        plugin = self.tmp / "plugin"
        for d in ("runtime/bin", "runtime/lib"):
            (plugin / d).mkdir(parents=True)
        (plugin / "runtime/bin/gamescope").write_text("binary")
        (plugin / "runtime/bin/gamescopectl").write_text("binary")
        (plugin / "runtime/lib" / kedavra_install.LOADER).write_text("loader")
        (plugin / "runtime/VERSION").write_text("kedavra-test\n")
        (self.root / "usr/bin").mkdir(parents=True)
        (self.root / "usr/bin/gamescope").write_text("stock")
        (self.root / "etc").mkdir()
        (self.root / "etc/os-release").write_text('ID=steamos\nPRETTY_NAME="SteamOS"\nVERSION_ID=3.7\n')
        (self.root / "proc").mkdir()
        self.sys = FakeSystem()
        self.inst = kedavra_install.Installer(plugin, self.tmp / "home", root=self.root, run=self.sys)

    def tearDown(self):
        shutil.rmtree(self.tmp)

    def test_install_writes_unit_and_runtime(self):
        r = self.inst.install()
        self.assertTrue(r["needs_reboot"])
        self.assertTrue((self.root / "var/lib/kedavra/runtime/bin/gamescope").exists())
        self.assertEqual(self.inst.installed_version(), "kedavra-test")
        unit = (self.root / "etc/systemd/system" / kedavra_install.UNIT_NAME).read_text()
        self.assertIn("Before=display-manager.service", unit)
        self.assertTrue(self.sys.enabled)
        self.assertTrue(any(c[0] == "setcap" for c in self.sys.calls))
        # The stock binary is never touched.
        self.assertEqual((self.root / "usr/bin/gamescope").read_text(), "stock")

    def test_install_refuses_missing_libraries(self):
        self.sys.missing = ["libstdc++.so.6"]
        with self.assertRaises(RuntimeError) as e:
            self.inst.install()
        self.assertIn("libstdc++", str(e.exception))
        self.assertFalse((self.root / "var/lib/kedavra/runtime").exists())
        self.assertFalse(self.sys.enabled)

    def test_install_checks_with_bundled_loader(self):
        self.inst.install()
        lists = [c for c in self.sys.calls if "--list" in c]
        self.assertTrue(lists and all(Path(c[0]).name == kedavra_install.LOADER for c in lists))
        self.assertFalse(any(Path(c[0]).name == "ldd" for c in self.sys.calls))
        boot = (self.root / "var/lib/kedavra/boot.sh").read_text()
        self.assertIn("runtime/lib/" + kedavra_install.LOADER, boot)
        self.assertNotIn("ldd \"$BIN\"", boot)

    def test_old_bundle_without_loader_is_refused(self):
        (self.inst.bundle / "lib" / kedavra_install.LOADER).unlink()
        with self.assertRaises(RuntimeError) as e:
            self.inst.install()
        self.assertIn(kedavra_install.LOADER, str(e.exception))

    def test_install_refuses_broken_binary(self):
        self.sys.version_rc = 1
        with self.assertRaises(RuntimeError):
            self.inst.install()
        self.assertFalse(self.sys.enabled)

    def test_uninstall(self):
        self.inst.install()
        self.sys.mounted = True
        r = self.inst.uninstall()
        self.assertTrue(r["needs_reboot"])
        self.assertFalse(self.sys.enabled)
        self.assertTrue((self.root / "var/lib/kedavra/state/disabled").exists())

    def test_healthy_only_with_fresh_status(self):
        self.inst.install()
        state = self.root / "var/lib/kedavra/state"
        (state / "trial").touch()
        self.assertFalse(self.inst.mark_healthy())
        st = self.root / "run/user/1000/kedavra-status.json"
        st.parent.mkdir(parents=True)
        st.write_text("{}")
        self.assertTrue(self.inst.mark_healthy())
        self.assertTrue((state / "healthy").exists())
        os.utime(st, (time.time() - 60, time.time() - 60))
        (state / "healthy").unlink()
        self.assertFalse(self.inst.mark_healthy())

    def test_report_zip_lands_on_desktop(self):
        data = self.tmp / "home/.local/share/kedavra"
        (data / "captures").mkdir(parents=True)
        (data / "captures/20261021-000000-x3.csv").write_text("flip_ns\n1\n")
        (data / "captures/20261021-000000-x3.json").write_text("{}")
        (data / "bench.json").write_text("{}")
        r = self.inst.report(data)
        bundle = Path(r["path"])
        self.assertEqual(bundle.parent, self.tmp / "home/Desktop")
        with zipfile.ZipFile(bundle) as z:
            names = set(z.namelist())
        self.assertIn("captures/20261021-000000-x3.csv", names)
        self.assertIn("captures/20261021-000000-x3.json", names)
        self.assertIn("bench.json", names)
        self.assertTrue(any(n.startswith("diagnostics-") for n in names))

    def test_diagnose_reports_stock_session(self):
        d = self.inst.diagnose()
        keys = {c["key"]: c for c in d["checks"]}
        self.assertTrue(keys["os"]["ok"])
        self.assertFalse(keys["installed"]["ok"])
        self.assertFalse(keys["mounted"]["ok"])
        self.assertFalse(d["ok"])

    def test_boot_script_rolls_back_unhealthy_boot(self):
        self.inst.install()
        script = (self.root / "var/lib/kedavra/boot.sh").read_text()
        state = self.root / "var/lib/kedavra/state"
        # Run the real boot script with stub ldd/findmnt/mount on PATH.
        stub = self.tmp / "stub"
        stub.mkdir()
        log = self.tmp / "mounts"
        for name, body in (("findmnt", "exit 1"), ("mount", f'echo "$@" >> {log}')):
            p = stub / name
            p.write_text(f"#!/bin/sh\n{body}\n")
            p.chmod(0o755)
        binary = self.root / "var/lib/kedavra/runtime/bin/gamescope"
        binary.write_text("#!/bin/sh\necho version\n")
        binary.chmod(0o755)
        loader = self.root / "var/lib/kedavra/runtime/lib" / kedavra_install.LOADER
        loader.write_text("#!/bin/sh\necho ok\n")
        loader.chmod(0o755)
        env = dict(os.environ, PATH=f"{stub}:{os.environ['PATH']}")
        script = script.replace("/usr/bin/gamescope", str(self.root / "usr/bin/gamescope"))
        boot = self.tmp / "boot.sh"
        boot.write_text(script)

        subprocess.run(["sh", str(boot)], env=env, check=True)  # boot 1: mounts, trial
        self.assertTrue((state / "trial").exists())
        self.assertEqual(len(log.read_text().splitlines()), 1)
        subprocess.run(["sh", str(boot)], env=env, check=True)  # boot 2: never healthy -> rollback
        self.assertTrue((state / "disabled").exists())
        self.assertTrue((state / "auto_rollback").exists())
        self.assertEqual(len(log.read_text().splitlines()), 1)


if __name__ == "__main__":
    unittest.main()
