"""PlatformIO pre-build script: stamps the firmware with the git commit it was built from.

Defines SB_BUILD_REV, e.g. "f1d8a64" or "f1d8a64-dirty" when the working tree has uncommitted
changes. The controller prints it in the boot banner and reports it as "build" in
GET /api/v1/status, so `curl -s http://smartbrew.local/api/v1/status` shows which build runs.
No timestamp on purpose: it would change on every build and force a full rebuild.
"""
import subprocess
Import("env")  # noqa: F821  (provided by PlatformIO)


def _git(*args):
    try:
        return subprocess.check_output(["git", *args], cwd=env.subst("$PROJECT_DIR"),  # noqa: F821
                                       stderr=subprocess.DEVNULL, text=True).strip()
    except Exception:
        return ""


rev = _git("rev-parse", "--short=7", "HEAD") or "unknown"
# Generated files (web assets) are rebuilt by the other pre-script, so only tracked sources count.
dirty = _git("status", "--porcelain", "--untracked-files=no", "--", "src", "include", "data",
             "platformio.ini", ":!src/network/web_assets.h", ":!data/index.html.gz")
if dirty:
    rev += "-dirty"
env.Append(CPPDEFINES=[("SB_BUILD_REV", env.StringifyMacro(rev))])  # noqa: F821
print(f"gen_build_info: SB_BUILD_REV={rev}")
