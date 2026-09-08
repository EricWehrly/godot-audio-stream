#!/usr/bin/env python
import json
import os
import subprocess

# Bind only the 4.6 API surface so one binary serves surfer (4.7) and the 4.6
# siblings. godot-cpp master ships an extension_api JSON per version, so this is
# enforced at generation time rather than left to chance. Override on the CLI.
ARGUMENTS.setdefault("api_version", "4.6")


def find_vcvars():
    """Locate a VS instance that actually has the x64 C++ toolset.

    SCons resolves MSVC "14.3" to the first *registered* instance, which may be
    a phantom -- present in the VS installer's records but carrying no C++
    workload, so it has neither cl.exe nor vcvars64.bat. SCons rejects it and
    does NOT fall through to a working instance, so msvc.exists() returns False
    and godot-cpp silently falls back to MinGW (tools/windows.py). If MinGW
    isn't installed either, every compile then dies with the thoroughly
    unhelpful "The system cannot find the file specified".

    Pointing MSVC_USE_SCRIPT at a batch file we verified on disk bypasses
    instance selection entirely. Override with `scons vcvars=<path>`.
    """
    if os.name != "nt":
        return None

    override = ARGUMENTS.get("vcvars", "")
    if override:
        return override if os.path.isfile(override) else None

    vswhere = None
    for base in (os.environ.get("ProgramFiles(x86)"), os.environ.get("ProgramFiles")):
        if not base:
            continue
        candidate = os.path.join(base, "Microsoft Visual Studio", "Installer", "vswhere.exe")
        if os.path.isfile(candidate):
            vswhere = candidate
            break
    if vswhere is None:
        return None

    try:
        raw = subprocess.run(
            [
                vswhere, "-all", "-products", "*",
                "-requires", "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                "-format", "json", "-utf8",
            ],
            capture_output=True, text=True, timeout=30,
        ).stdout
        installs = json.loads(raw or "[]")
    except Exception:
        return None

    # Newest first, but only trust an instance whose vcvars64.bat really exists.
    installs.sort(key=lambda i: i.get("installationVersion", ""), reverse=True)
    for install in installs:
        bat = os.path.join(
            install.get("installationPath", ""), "VC", "Auxiliary", "Build", "vcvars64.bat"
        )
        if os.path.isfile(bat):
            return bat, _msvs_version(install.get("installationVersion", ""))
    return None


# VS product major -> the MSVS_VERSION string SCons expects.
_MSVS_BY_MAJOR = {15: "14.1", 16: "14.2", 17: "14.3", 18: "14.4"}


def _msvs_version(installation_version):
    try:
        major = int(installation_version.split(".")[0])
    except (ValueError, IndexError):
        return "14.3"
    return _MSVS_BY_MAJOR.get(major, "14.3")


# Variables vcvars64.bat sets that the compiler and linker actually need.
_VCVARS_KEYS = (
    "PATH", "INCLUDE", "LIB", "LIBPATH",
    "VCINSTALLDIR", "VCTOOLSINSTALLDIR", "VCTOOLSVERSION",
    "WINDOWSSDKDIR", "WINDOWSSDKVERSION", "UNIVERSALCRTSDKDIR", "UCRTVERSION",
)


def capture_vcvars_env(bat):
    """Run vcvars64.bat and capture the environment it produces.

    MSVC_USE_SCRIPT is enough to make SCons *select* the MSVC toolchain, but on
    this setup it does not actually apply the script's environment -- so builds
    get as far as invoking `cl` and then die with "'cl' is not recognized".
    Running the batch file ourselves and merging PATH/INCLUDE/LIB into env["ENV"]
    is the part that was missing.
    """
    try:
        completed = subprocess.run(
            '"%s" && set' % bat, shell=True, capture_output=True, text=True, timeout=120
        )
    except Exception:
        return {}
    if completed.returncode != 0:
        return {}

    captured = {}
    for line in completed.stdout.splitlines():
        key, sep, value = line.partition("=")
        if sep and key.upper() in _VCVARS_KEYS:
            captured[key.upper()] = value
    return captured


env = Environment(tools=["default"], PLATFORM="")

detected = find_vcvars()
if detected:
    vcvars, msvs_version = detected
    print("Using MSVC environment script: %s" % vcvars)
    env["MSVC_USE_SCRIPT"] = vcvars

Export("env")
env = SConscript("godot-cpp/SConstruct")

# godot-cpp nulls MSVC_VERSION so SCons picks for itself, but nothing then fills
# in MSVS_VERSION -- and mslink regex-matches it unguarded, so a None here dies
# as "expected string or bytes-like object, got 'NoneType'" at link time.
if detected and not env.get("MSVS_VERSION"):
    env["MSVS_VERSION"] = msvs_version

if detected:
    for key, value in capture_vcvars_env(vcvars).items():
        env["ENV"][key] = value

env.Append(CPPPATH=["src/", "thirdparty/"])
sources = Glob("src/*.cpp")

library = env.SharedLibrary(
    "demo/bin/libgdradio{}{}".format(env["suffix"], env["SHLIBSUFFIX"]),
    source=sources,
)

Default(library)
