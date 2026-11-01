# PlatformIO pre-build hook (runs before every `pio run`).
#
# 1. Regenerates the embedded web assets from web/. PlatformIO compiles
#    ESP-IDF sources with SCons from the CMake code model, so CMake custom
#    commands do not run; this hook does the job instead.
# 2. Forces a CMake re-configure when VERSION changed. PlatformIO only
#    re-configures when CMakeLists.txt / sdkconfig change, so without this a
#    bumped VERSION would not reach PROJECT_VER and the app descriptor.
# 3. Recompiles the app descriptor on every build so its date/time fields are
#    the real build time (they come from __DATE__/__TIME__ in esp_app_desc.c).
import os
import subprocess
import sys

Import("env")  # noqa: F821  (provided by PlatformIO)

proj = env.subst("$PROJECT_DIR")  # noqa: F821
build = env.subst("$BUILD_DIR")  # noqa: F821

subprocess.check_call([
    sys.executable,
    os.path.join(proj, "tools", "pack_www.py"),
    os.path.join(proj, "web"),
    os.path.join(proj, "components", "webserver", "generated", "www_assets.c"),
])

with open(os.path.join(proj, "VERSION"), encoding="utf-8") as f:
    version = f.read().strip()
stamp = os.path.join(build, "bms_version.stamp")
old = None
if os.path.exists(stamp):
    with open(stamp, encoding="utf-8") as f:
        old = f.read().strip()
if old != version:
    ninja = os.path.join(build, "build.ninja")
    if os.path.exists(ninja):
        os.remove(ninja)  # makes the PlatformIO ESP-IDF builder re-run CMake
    os.makedirs(build, exist_ok=True)
    with open(stamp, "w", encoding="utf-8") as f:
        f.write(version)
    print(f"prebuild: VERSION is {version} (was {old}), re-configuring")

desc_obj = os.path.join(build, "esp_app_format", "esp_app_desc.c.o")
if os.path.exists(desc_obj):
    os.remove(desc_obj)
