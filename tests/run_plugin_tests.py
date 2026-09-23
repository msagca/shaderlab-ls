"""Tests for the Neovim plugin: the filetypes it claims, and the provisioning that gets a server on disk.

Each case runs in its own headless Neovim against its own fixture checkout -- the repository's own lua/, lsp/ and
plugin/ files beside a fake src/, CMakeLists.txt and build script -- and reports what happened to a log that this
runner asserts on. The fake build stands in for cmake: it records that it ran, waits long enough to be observed,
and copies the server given on the command line into build/. So these tests exercise when a build is started, what
happens to a client while it runs and what happens to the buffer when it lands, without compiling anything.

Downloading a release is the one path left out: it wants the network and a published release. vim.g
shaderlab_ls_download is off in every case, which is also the switch a developer sets.

Usage: python tests/run_plugin_tests.py path/to/shaderlab-ls.exe
"""

import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
CASES = pathlib.Path(__file__).parent / "plugin_cases.lua"
HOUR = 3600

failures = []
fixtures = []  # kept when something failed, so the log and the checkout that produced it can be read


def check(condition, description):
    status = "ok  " if condition else "FAIL"
    print(f"  [{status}] {description}")
    if not condition:
        failures.append(description)


def nvim_version():
    """(major, minor) of the nvim on PATH, or None when there is none."""
    if not shutil.which("nvim"):
        return None
    reported = subprocess.run(["nvim", "--version"], capture_output=True, text=True).stdout
    found = re.search(r"NVIM v(\d+)\.(\d+)", reported)
    return (int(found.group(1)), int(found.group(2))) if found else None


BUILD_CMD = """@echo off
rem Stands in for build.cmd: records the run, waits to be caught at it, and installs the server.
echo build >> "%~dp0build-log"
if exist "%~dp0fail-build" (
  echo fake build refusing, as asked
  echo LNK1104 would be here in the real thing
  exit /b 1
)
ping -n 4 127.0.0.1 >nul
if not exist "%~dp0build" mkdir "%~dp0build"
copy /y "%~dp0server.exe" "%~dp0build\\shaderlab-ls.exe" >nul
"""

BUILD_SH = """#!/bin/sh
# Stands in for build.sh: records the run, waits to be caught at it, and installs the server.
here=$(dirname "$0")
echo build >> "$here/build-log"
if [ -f "$here/fail-build" ]; then
  echo "fake build refusing, as asked"
  exit 1
fi
sleep 3
mkdir -p "$here/build"
cp "$here/server" "$here/build/shaderlab-ls"
chmod +x "$here/build/shaderlab-ls"
"""

CMAKELISTS = """cmake_minimum_required(VERSION 3.25)
project(shaderlab_ls VERSION 9.9.9 LANGUAGES CXX)
"""

SHADERS = {
    "test.shader": 'Shader "Tests/Plugin" {\n  SubShader { Pass { } }\n}\n',
    "godot.shader": "shader_type canvas_item;\nvoid fragment() { }\n",
    "kernel.compute": "#pragma kernel CSMain\n[numthreads(1,1,1)]\nvoid CSMain() { }\n",
    "include.cginc": "float4 tint;\n",
    "unity.hlsl": "float3 identity(float3 v) { return v; }\n",
    "support.glslinc": "precision mediump float;\n",
    # One GLSL block, with `bad` inside it and outside it: tests/fake_glsl_server.py reports every such line it sees.
    "glsl.shader": (
        'Shader "Tests/Glsl" {\n'
        "  SubShader { Pass {\n"
        "    GLSLPROGRAM\n"
        "    void main() { }\n"
        "    // bad\n"
        "    ENDGLSL\n"
        '  } Pass { Name "bad" } }\n'
        "}\n"
    ),
}


def fixture_for(case, server, stale=None, fail=False):
    """A checkout to run `case` against: the real plugin files, a fake build, and optionally an executable in
    build/ already. `stale` says whether that executable is older than the sources or newer than them."""
    fixture = pathlib.Path(tempfile.mkdtemp(prefix=f"shaderlab-plugin-{case}-"))
    fixtures.append(fixture)
    windows = sys.platform == "win32"
    exe_name = "shaderlab-ls.exe" if windows else "shaderlab-ls"

    for part in ("lua/shaderlab-ls.lua", "lua/shaderlab-ls/glsl.lua", "lsp/shaderlab_ls.lua", "plugin/shaderlab-ls.lua"):
        target = fixture / part
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(ROOT / part, target)

    # The documented one-file install, for the case that tests it: the config on its own, no module beside it.
    standalone = fixture / "standalone" / "lsp"
    standalone.mkdir(parents=True)
    shutil.copy(ROOT / "lsp/shaderlab_ls.lua", standalone / "shaderlab_ls.lua")

    (fixture / "CMakeLists.txt").write_text(CMAKELISTS, encoding="utf-8")
    (fixture / "src").mkdir()
    (fixture / "src" / "main.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
    (fixture / "build.cmd").write_text(BUILD_CMD, encoding="utf-8")
    (fixture / "build.sh").write_text(BUILD_SH, encoding="utf-8")
    os.chmod(fixture / "build.sh", 0o755)
    shutil.copy(server, fixture / ("server.exe" if windows else "server"))

    # What the fake build installs, also for the PATH the standalone case looks on.
    on_path = fixture / "onpath"
    on_path.mkdir()
    shutil.copy(server, on_path / exe_name)

    project = fixture / "project"
    project.mkdir()
    for name, text in SHADERS.items():
        (project / name).write_text(text, encoding="utf-8")

    if fail:
        (fixture / "fail-build").write_text("", encoding="utf-8")

    if stale is not None:
        (fixture / "build").mkdir(exist_ok=True)
        shutil.copy(server, fixture / "build" / exe_name)
        # mtimes are the whole staleness test, so they are set rather than left to the order the files were copied
        # in. An hour is far more than the second the comparison works in.
        now = time.time()
        executable, sources = (now - HOUR, now) if stale else (now, now - HOUR)
        os.utime(fixture / "build" / exe_name, (executable, executable))
        for path in (fixture / "src" / "main.cpp", fixture / "CMakeLists.txt"):
            os.utime(path, (sources, sources))

    return fixture


def run(case, fixture, on_path=False, timeout=90):
    """Runs one case and returns its log as a list of lines, plus whatever Neovim said on stderr."""
    log = fixture / "log"
    environment = dict(os.environ, FIXTURE=fixture.as_posix(), CASE=case, LOG=str(log), PYTHON=sys.executable,
                       FAKE_GLSL=str(pathlib.Path(__file__).parent / "fake_glsl_server.py"))
    if on_path:
        environment["PATH"] = str(fixture / "onpath") + os.pathsep + environment.get("PATH", "")
    else:
        # A shaderlab-ls on the developer's own PATH would stand in for the one the fixture is meant to be missing.
        environment["PATH"] = os.pathsep.join(
            part for part in environment.get("PATH", "").split(os.pathsep)
            if not (pathlib.Path(part) / "shaderlab-ls").exists() and not (pathlib.Path(part) / "shaderlab-ls.exe").exists()
        )
    # -i NONE so the test sessions share no shada file, --headless so nothing waits for a UI. -u is the case file,
    # which puts the fixture on 'runtimepath' before the plugin and config files under it are loaded.
    command = ["nvim", "--headless", "-i", "NONE", "-u", str(CASES)]
    try:
        finished = subprocess.run(command, cwd=fixture, env=environment, capture_output=True, text=True, timeout=timeout)
        stderr = finished.stderr
    except subprocess.TimeoutExpired as expired:
        stderr = (expired.stderr or b"").decode(errors="replace") if isinstance(expired.stderr, bytes) else (expired.stderr or "")
        stderr += f"\ncase did not finish within {timeout}s"
    lines = log.read_text(encoding="utf-8").splitlines() if log.exists() else []
    return lines, stderr


# A headless Neovim writes vim.notify() to stderr, so an empty stderr is not the test: these are what a broken
# case looks like there, whether it broke in Lua or in the editor.
BROKEN = re.compile(r"E\d{3,}:|E5\d\d:|stack traceback|Error executing|Invalid 'event'|did not finish")


def ran_cleanly(lines, stderr, description):
    check("done" in lines, f"{description}: the case finished")
    broken = BROKEN.search(stderr)
    check(broken is None, f"{description}: no errors" + (f" ({stderr[max(0, broken.start() - 60):broken.end() + 60].strip()})" if broken else ""))


def value(lines, name, fallback=None):
    for line in lines:
        if line.startswith(name + " "):
            return line[len(name) + 1:]
    return fallback


def builds(lines):
    return int(value(lines, "builds", "-1"))


def attaches(lines):
    return [line for line in lines if line.startswith("attach ")]


def index(lines, pattern):
    for position, line in enumerate(lines):
        if re.search(pattern, line):
            return position
    return -1


def logged(lines, pattern):
    return index(lines, pattern) >= 0


def before(lines, earlier, later):
    """Both were logged, in this order. Two lines that are missing are not in order, they are absent."""
    first, second = index(lines, earlier), index(lines, later)
    return 0 <= first < second


def main():
    if len(sys.argv) < 2:
        print(__doc__.strip().splitlines()[-1])
        return 2
    server = pathlib.Path(sys.argv[1]).resolve()
    if not server.exists():
        print(f"no server at {server}")
        return 2

    # 77 rather than 2: nothing is wrong with the tests or the server, there is only nothing to run them with, and
    # ctest reports this exit code as a skip.
    version = nvim_version()
    if version is None:
        print("no nvim on PATH: these tests drive Neovim, so they need it installed")
        return 77
    if version < (0, 12):
        print(f"nvim {version[0]}.{version[1]} is too old: the plugin needs 0.12 or newer")
        return 77
    print(f"nvim {version[0]}.{version[1]}, server {server.name}")

    print("\nfiletypes")
    fixture = fixture_for("filetypes", server, stale=False)
    lines, stderr = run("filetypes", fixture)
    ran_cleanly(lines, stderr, "filetypes")
    for name, expected in (
        ("test.shader", "shaderlab"),
        ("godot.shader", "gdshader"),
        ("kernel.compute", "hlsl"),
        ("include.cginc", "hlsl"),
        ("unity.hlsl", "hlsl"),
        ("support.glslinc", "glsl"),
    ):
        check(f"filetype {name} {expected}" in lines, f"{name} is {expected}")

    print("\nan install, with nothing to run and no shader open")
    fixture = fixture_for("install", server)
    lines, stderr = run("install", fixture)
    ran_cleanly(lines, stderr, "install")
    check(builds(lines) == 1, "the executable is built without a shader being opened")
    check(value(lines, "built") == "true", "and it is there when the build finishes")
    check(attaches(lines) == [], "nothing is started, there being no buffer to start it for")

    print("\nan install with a shader already open")
    fixture = fixture_for("install_with_shader", server)
    lines, stderr = run("install_with_shader", fixture)
    ran_cleanly(lines, stderr, "install_with_shader")
    check(builds(lines) == 1, "the build runs")
    check(before(lines, r"^notify .*building in", r"^attach "), "no client before there is an executable")
    check(len(attaches(lines)) == 1 and value(lines, "clients") == "1", "the open buffer ends up with a client")
    check(not logged(lines, r"^notify .*not executable"), "no complaint about a missing language server")

    print("\na stale executable while its replacement builds")
    fixture = fixture_for("stale", server, stale=True)
    lines, stderr = run("stale", fixture)
    ran_cleanly(lines, stderr, "stale")
    check(builds(lines) == 1, "the build runs")
    check(before(lines, r"^attach buf=1 client=1", r"^notify .*shaderlab-ls built"), "the stale server serves meanwhile")
    check(before(lines, r"^notify .*shaderlab-ls built", r"^detach buf=1 client=1"), "the stale client is dropped when the build lands")
    check(before(lines, r"^detach buf=1 client=1", r"^attach buf=1 client=2"), "and the same buffer is moved onto the new server")

    print("\nan executable newer than the sources")
    fixture = fixture_for("current", server, stale=False)
    lines, stderr = run("current", fixture, timeout=60)
    ran_cleanly(lines, stderr, "current")
    check(builds(lines) == 0, "nothing is built for a checkout that is current")
    check(len(attaches(lines)) == 1, "the server starts straight away")
    check(not logged(lines, r"^notify"), "and says nothing about it")

    print("\na build that fails")
    fixture = fixture_for("failing_build", server, stale=True, fail=True)
    lines, stderr = run("failing_build", fixture)
    ran_cleanly(lines, stderr, "failing_build")
    check(builds(lines) == 1, "the build is attempted")
    check(logged(lines, r"notify level=4 .*build failed \(exit 1\)"), "the failure is reported with its exit code")
    check(value(lines, "built") == "true", "the executable that was working is put back")
    check(value(lines, "clients") == "1", "and a server is still serving the buffer")

    print("\nneither downloading nor building")
    fixture = fixture_for("no_provider", server, stale=True)
    lines, stderr = run("no_provider", fixture, timeout=60)
    ran_cleanly(lines, stderr, "no_provider")
    check(builds(lines) == 0, "no build is attempted")
    check(logged(lines, r"notify level=3 .*neither downloading nor building is enabled"), "the staleness is reported instead")
    check(value(lines, "clients") == "1", "the stale server goes on serving")

    print("\nprovisioning without the config being enabled")
    fixture = fixture_for("not_enabled", server)
    lines, stderr = run("not_enabled", fixture)
    ran_cleanly(lines, stderr, "not_enabled")
    check(builds(lines) == 1, "the executable is still provided")
    check(value(lines, "enabled") == "false", "the config is not enabled behind the user's back")
    check(attaches(lines) == [], "and no server attaches to anything")

    print("\nan update announced mid-session")
    fixture = fixture_for("pack_changed", server)
    lines, stderr = run("pack_changed", fixture, timeout=120)
    ran_cleanly(lines, stderr, "pack_changed")
    check(builds(lines) == 2, "the update is built, though the session had already built once")
    started = [position for position, line in enumerate(lines) if re.search(r"^notify .*building in", line)]
    first, second = (started + [-1, -1])[:2]
    check(0 <= index(lines, r"^announce other") < second, "another plugin's update is not this checkout's business")
    check(0 <= index(lines, r"^announce shaderlab-ls") < second, "this checkout's is")
    check(0 <= first < index(lines, r"^announce other"), "the first build was the one at startup")

    print("\nan update announced while a build is running")
    fixture = fixture_for("pack_changed_while_building", server)
    lines, stderr = run("pack_changed_while_building", fixture, timeout=120)
    ran_cleanly(lines, stderr, "pack_changed_while_building")
    check(len([line for line in lines if line.startswith("announce ")]) == 2, "both announcements arrive")
    check(builds(lines) == 1, "and are left to the build already running")

    print("\nthe config copied on its own, with the server on PATH")
    fixture = fixture_for("standalone", server)
    lines, stderr = run("standalone", fixture, on_path=True, timeout=60)
    ran_cleanly(lines, stderr, "standalone")
    check(builds(lines) == 0, "nothing is built: there is no checkout to build")
    check(len(attaches(lines)) == 1, "the server on PATH is what runs")
    check(not logged(lines, r"^notify"), "and nothing is reported")

    print("\na GLSL server on a shader's GLSL blocks")
    fixture = fixture_for("glsl", server, stale=False)
    lines, stderr = run("glsl", fixture, timeout=60)
    ran_cleanly(lines, stderr, "glsl")
    record = fixture / "glsl-record"
    sent = [json.loads(line) for line in record.read_text(encoding="utf-8").splitlines()] if record.exists() else []
    opened = [entry for entry in sent if entry["event"] == "open"]
    changed = [entry for entry in sent if entry["event"] == "change"]
    check(value(lines, "proxy plain") == "0", "a shader with no GLSL in it gets no GLSL server")
    check(value(lines, "proxy glsl") == "1", "one with a GLSL block does")
    check(value(lines, "glsl-only clients") == "0", "and the user's own config is not started for it")
    check(len(opened) == 1 and opened[0]["languageId"] == "glsl" and opened[0]["uri"].endswith("/glsl.shader.glsl"),
          "the server is shown the shader as a GLSL document")
    text = opened[0]["text"].splitlines() if opened else []
    check(len(text) == 8 and not "".join(text[:3] + text[5:]).strip() and text[3:5] == ["    void main() { }", "    // bad"],
          "with everything but the block blanked, and the block where it was")
    check(value(lines, "diagnostics") == "4", "its diagnostics reach the buffer only from inside the block")
    check(value(lines, "formats") == "false", "formatting is left to shaderlab-ls")
    check(value(lines, "hover inside") == "hover 3:6", "a request from inside the block is answered, at the same position")
    check(value(lines, "hover outside") == "nil", "one from outside it is not passed on")
    check(value(lines, "definition") == "true", "locations in the shader come back as the shader's")
    check(bool(changed) and all(entry["full"] for entry in changed), "edits are sent as whole documents")
    check(value(lines, "diagnostics after edit") == "3,5", "and the server follows them")

    print()
    if failures:
        print(f"{len(failures)} failure(s)")
        print("the fixtures are left behind, each with the log of its case:")
        for fixture in fixtures:
            print(f"  {fixture}")
        return 1
    for fixture in fixtures:
        shutil.rmtree(fixture, ignore_errors=True)
    print("all plugin tests passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
