"""
PlatformIO pre-build hook for [env:esp32dev] - keeps the web dashboard
(web/ -> data/) in sync with every firmware build/upload, so the ESP32
never gets flashed with a stale or missing dashboard.

Runs at SCons config-time, i.e. on every `pio run -e esp32dev ...`
invocation regardless of target (build, upload, uploadfs). Also chains
uploadfs onto the upload target, so `pio run -e esp32dev -t upload` alone
puts both the firmware and the dashboard on the device in one command.

Fails loudly if Node/npm are present but the web build itself errors (a
real problem worth stopping the build over). Fails *quietly* - a warning,
not a crash - if npm/node simply aren't installed at all, since tool
availability has varied session-to-session on this project; a firmware-only
build should still be possible on a machine without Node.
"""

import os
import shutil
import subprocess
import time

Import("env")

project_dir = env["PROJECT_DIR"]
web_dir = os.path.join(project_dir, "web")


def build_web_dashboard():
    if shutil.which("npm") is None:
        print("[build_web] npm not found on this machine - skipping web dashboard build "
              "(data/ will be whatever was there before, possibly stale or absent)")
        return

    print("[build_web] building web dashboard into data/ ...")
    node_modules = os.path.join(web_dir, "node_modules")
    if not os.path.isdir(node_modules):
        subprocess.run(["npm", "install"], cwd=web_dir, check=True)
    subprocess.run(["npm", "run", "build:device"], cwd=web_dir, check=True)
    print("[build_web] done")


build_web_dashboard()


def after_upload(source, target, env):
    if shutil.which("npm") is None:
        return  # already warned above; nothing to upload if it wasn't built
    print("[build_web] firmware uploaded - uploading filesystem (dashboard) image too...")
    # esptool's hard reset (RTS pin toggle) can momentarily drop/re-enumerate
    # the USB-serial adapter; opening the port again immediately can race
    # that. A short pause here has been enough in practice - if it's ever
    # not, the exit-code check below at least surfaces the failure instead
    # of silently leaving the device without a dashboard.
    time.sleep(2)
    rc = env.Execute("$PYTHONEXE -m platformio run -e esp32dev -t uploadfs")
    if rc != 0:
        print("[build_web] *** uploadfs FAILED (exit %s) - the dashboard was NOT "
              "updated on the device. Run `pio run -e esp32dev -t uploadfs` "
              "manually once the port is free. ***" % rc)
    else:
        print("[build_web] filesystem upload complete - dashboard is live.")


env.AddPostAction("upload", after_upload)
