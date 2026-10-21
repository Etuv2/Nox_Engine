#!/usr/bin/env python3
"""End-to-end test of the automation server: launches Nox_Engine.exe and drives it over TCP.

    python tools/nox_mcp/test_e2e.py            (needs build/vs2022-x64/Release/Nox_Engine.exe and a desktop session)
    NOX_E2E_PORT=47871 python tools/nox_mcp/test_e2e.py

Launches the engine on scenes/automation_e2e.json in editor mode, then checks engine.info, scene.tree, node.get,
node.set (verified with node.get), node.create/delete, render.screenshot, render.pass_timings, log.tail,
lua.exec, input.inject into the action map, render.reload_shaders (clean, broken and restored shader) and Ctrl+Shift+R
through input.inject target "editor", engine.pause/step/resume, scene.load, protocol errors, and engine.quit. Exit status 0 when every check
passes. Screenshots and the engine log go under captures/automation/.
"""
import json
import os
import shutil
import socket

import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from nox_client import NoxClient, NoxError  # noqa: E402

ROOT = HERE.parents[1]
EXE = ROOT / "build" / "vs2022-x64" / "Release" / "Nox_Engine.exe"
SCENE = "scenes/automation_e2e.json"
PORT = int(os.environ.get("NOX_E2E_PORT", "47871"))
LOG_DIR = ROOT / "captures" / "automation"
READY_TIMEOUT_S = 180

results = []


def check(name, condition, detail=""):
    results.append((name, bool(condition)))
    status = "PASS" if condition else "FAIL"
    print(f"[{status}] {name}" + (f" -- {detail}" if detail else ""), flush=True)
    return bool(condition)


def find_node(tree, name):
    if tree.get("name") == name:
        return tree
    for child in tree.get("children", []):
        found = find_node(child, name)
        if found:
            return found
    return None


def raw_line_roundtrip(payload):
    """Sends raw bytes as one line and returns the decoded response (for protocol-level checks)."""
    with socket.create_connection(("127.0.0.1", PORT), timeout=30) as sock:
        sock.sendall(payload + b"\n")
        buffer = b""
        while b"\n" not in buffer:
            chunk = sock.recv(65536)
            if not chunk:
                break
            buffer += chunk
    return json.loads(buffer.split(b"\n", 1)[0].decode("utf-8"))


def scene_ready(info):
    """True when no scene load is running and a scene is loaded."""
    return not info.get("loading") and bool(info.get("scene")) and not info.get("load_error")


def image_not_black(path):
    """(ok, detail): the PNG has visible pixels. Uses PIL when it is installed, else a file-size floor."""
    if not path.exists():
        return False, "missing file"
    try:
        from PIL import Image, ImageStat
    except ImportError:
        return path.stat().st_size > 20000, f"PIL not installed; {path.stat().st_size} bytes"
    mean = ImageStat.Stat(Image.open(path).convert("RGB")).mean
    return max(mean) > 2.0, f"mean RGB {[round(v, 1) for v in mean]}"


def wait_ready(nox, proc):
    deadline = time.monotonic() + READY_TIMEOUT_S
    last = None
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError(f"engine exited early with code {proc.returncode}")
        try:
            info = nox.call("engine.info")
            if info.get("load_error"):
                raise RuntimeError(f"scene load failed: {info['load_error']}")
            if scene_ready(info):
                return info
        except (OSError, NoxError) as exc:
            last = exc
        time.sleep(0.5)
    raise RuntimeError(f"engine not ready after {READY_TIMEOUT_S} s (last error: {last})")


def main():
    if not EXE.exists():
        print(f"engine not built: {EXE} (run the Release build first)")
        return 1
    LOG_DIR.mkdir(parents=True, exist_ok=True)
    log_path = LOG_DIR / f"e2e_engine_{PORT}.log"
    env = dict(os.environ)
    env["NOX_FIRST_SCENE"] = SCENE
    command = [str(EXE), f"--automation={PORT}", "--preset", "editor"]
    print("launching:", " ".join(command), flush=True)
    with open(log_path, "w", encoding="utf-8", errors="replace") as log_file:
        proc = subprocess.Popen(command, cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                                stdout=log_file, stderr=subprocess.STDOUT)
        try:
            run_checks(proc)
        finally:
            if proc.poll() is None:
                print("engine still running at the end: killing it", flush=True)
                proc.kill()
                proc.wait(timeout=20)
    check_without_project()
    failed = [name for name, ok in results if not ok]
    print(f"\n{len(results) - len(failed)} passed, {len(failed)} failed", flush=True)
    if failed:
        print("failed: " + ", ".join(failed))
    return 0 if not failed else 1


def check_without_project():
    """A scene with no Lua project: lua.exec must say so, and still work with isolated=true."""
    port = PORT + 1
    env = dict(os.environ)
    env["NOX_FIRST_SCENE"] = "scenes/cornell_box.json"
    command = [str(EXE), f"--automation={port}", "--preset", "editor"]
    log_path = LOG_DIR / f"e2e_noproject_{port}.log"
    with open(log_path, "w", encoding="utf-8", errors="replace") as log_file:
        proc = subprocess.Popen(command, cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                                stdout=log_file, stderr=subprocess.STDOUT)
        try:
            with NoxClient(port=port, timeout=150) as nox:
                wait_ready(nox, proc)
                try:
                    nox.call("lua.exec", {"code": "return 1"})
                    check("lua.exec without a project world reports it", False, "no error returned")
                except NoxError as exc:
                    check("lua.exec without a project world reports it",
                          exc.code == -32000 and "no Lua project" in exc.message, f"message={exc.message!r}")
                check("lua.exec isolated works without a project world",
                      nox.call("lua.exec", {"code": "return 1 + 2", "isolated": True}) == 3)
                nox.call("engine.quit")
            proc.wait(timeout=60)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait(timeout=20)


def run_checks(proc):
    with NoxClient(port=PORT, timeout=150) as nox:
        info = wait_ready(nox, proc)
        check("engine.info answers and reports protocol 2.0",
              info.get("protocol") == "2.0", json.dumps(info))
        check("engine.info reports editor mode and the e2e scene",
              info.get("mode") == "editor" and info.get("scene", "").endswith("automation_e2e.json"),
              f"mode={info.get('mode')} scene={info.get('scene')}")

        # Scene tree and node ids
        tree = nox.call("scene.tree")
        light = find_node(tree, "E2E_Light")
        box = find_node(tree, "E2E_Box")
        check("scene.tree lists the scene nodes", light is not None and box is not None,
              f"root children={len(tree.get('children', []))}")
        light_id = light["id"] if light else None

        node = nox.call("node.get", {"id": light_id})
        check("node.get exposes the Light component properties",
              "Light" in node.get("properties", {}) and "intensity" in node["properties"]["Light"],
              f"components={node.get('components')}")

        nox.call("node.set", {"id": light_id, "component": "Light", "property": "intensity", "value": 2.5})
        readback = nox.call("node.get", {"id": light_id})["properties"]["Light"]["intensity"]
        check("node.set changes the value (verified with node.get)", abs(readback - 2.5) < 1e-4,
              f"intensity={readback}")

        try:
            nox.call("node.set", {"id": light_id, "component": "Light", "property": "intensity", "value": "x"})
            check("node.set rejects a wrong value type with an error", False, "no error returned")
        except NoxError as exc:
            check("node.set rejects a wrong value type with an error", exc.code == -32602, f"code={exc.code}")

        created = nox.call("node.create", {"node": {"name": "E2E_Temp", "type": "node", "position": [0, 2, 0]}})
        found = nox.call("node.find", {"name": "E2E_Temp"})
        check("node.create adds a node that node.find returns", created["id"] in [n["id"] for n in found],
              f"id={created['id']}")
        nox.call("node.delete", {"id": created["id"]})
        try:
            nox.call("node.get", {"id": created["id"]})
            check("node.delete removes the node", False, "node still answers")
        except NoxError as exc:
            check("node.delete removes the node", exc.code == -32602, f"code={exc.code}")

        # Rendering and diagnostics
        shot_path = "captures/automation/e2e_shot.png"
        shot = nox.call("render.screenshot", {"path": shot_path})
        shot_file = ROOT / shot["path"]
        head = shot_file.read_bytes()[:8] if shot_file.exists() else b""
        check("render.screenshot writes a non-empty PNG",
              head == b"\x89PNG\r\n\x1a\n" and shot_file.stat().st_size > 0,
              f"{shot['path']} {shot_file.stat().st_size if shot_file.exists() else 0} bytes {shot['width']}x{shot['height']}")

        timings = nox.call("render.pass_timings")
        check("render.pass_timings reports per-pass timings", len(timings.get("passes", [])) > 0,
              f"{len(timings.get('passes', []))} passes, frame {timings.get('frame')}")

        logs = nox.call("log.tail", {"lines": 50})
        check("log.tail returns engine log lines", len(logs.get("lines", [])) > 0, f"total={logs.get('total')}")

        # Lua: the project world (the VM the sample project's scripts run in) and the isolated VM
        check("lua.exec evaluates Lua in the project world", nox.call("lua.exec", {"code": "return 1 + 2"}) == 3)
        check("project world has the nox API", nox.call("lua.exec", {"code": "return type(nox)"}) == "table")
        nox.call("lua.exec", {"code": "__e2e_value = 'set by an earlier call'"})
        readback = nox.call("lua.exec", {"code": "return __e2e_value"})
        check("a value set in one lua.exec is read back by the next (same project VM)",
              readback == "set by an earlier call", f"read {readback!r}")
        check("isolated VM does not see the project world's globals",
              nox.call("lua.exec", {"code": "return __e2e_value", "isolated": True}) is None)
        check("isolated VM evaluates Lua", nox.call("lua.exec", {"code": "return 1 + 2", "isolated": True}) == 3)
        check("isolated VM has no nox API", nox.call("lua.exec", {"code": "return type(nox)", "isolated": True}) == "nil")
        check("sandbox: dofile and loadfile are absent in the project VM",
              nox.call("lua.exec", {"code": "return tostring(dofile) .. ' ' .. tostring(loadfile)"}) == "nil nil")
        check("sandbox: dofile and loadfile are absent in the isolated VM",
              nox.call("lua.exec", {"code": "return tostring(dofile) .. ' ' .. tostring(loadfile)", "isolated": True}) == "nil nil")
        try:
            nox.call("lua.exec", {"code": "local function inner() error('e2e-traceback') end\ninner()"})
            check("lua.exec reports Lua errors as JSON-RPC errors", False, "no error returned")
        except NoxError as exc:
            traceback = (exc.data or {}).get("traceback", "")
            check("lua.exec error carries the message and a traceback",
                  exc.code == -32000 and "e2e-traceback" in exc.message and "stack traceback" in traceback,
                  f"code={exc.code} message={exc.message!r} traceback_lines={len(traceback.splitlines())}")

        # Injected input drives the action map: sample-arena binds "hud_toggle" to H and "fire" to pad X.
        nox.call("input.inject", {"key": "H", "state": "down"})
        time.sleep(0.3)
        key_held = nox.call("lua.exec", {"code": "return input.down('hud_toggle')"})
        nox.call("input.inject", {"key": "H", "state": "up"})
        time.sleep(0.3)
        key_released = nox.call("lua.exec", {"code": "return input.down('hud_toggle')"})
        check("input.inject key drives its action (held, then released)",
              key_held is True and key_released is False, f"held={key_held} released={key_released}")
        nox.call("input.inject", {"pad": 0, "button": "x", "state": "down"})
        time.sleep(0.3)
        pad_held = nox.call("lua.exec", {"code": "return input.down('fire')"})
        nox.call("input.inject", {"pad": 0, "button": "x", "state": "up"})
        time.sleep(0.3)
        check("input.inject pad button drives its action", pad_held is True, f"held={pad_held}")
        nox.call("input.inject", {"pad": 0, "axis": "leftx", "value": 0.8, "state": "down"})
        time.sleep(0.3)
        steer = nox.call("lua.exec", {"code": "return input.axis('steer')"})
        nox.call("input.inject", {"pad": 0, "axis": "leftx", "state": "up"})
        check("input.inject pad axis drives an axis action", isinstance(steer, (int, float)) and steer > 0.5,
              f"steer={steer}")

        # Frame control: exactly N simulation frames per step
        nox.call("engine.pause")
        before = nox.call("engine.info")["frame"]
        stepped = nox.call("engine.step", {"frames": 3})
        after = nox.call("engine.info")["frame"]
        check("engine.step {frames: 3} advances exactly 3 frames while paused",
              after - before == 3 and stepped["frame"] == after, f"before={before} after={after}")
        nox.call("engine.resume")
        time.sleep(0.3)
        check("engine.resume lets the frame counter run again",
              nox.call("engine.info")["frame"] > after and not nox.call("engine.info")["paused"])

        # Scene load through the normal swap
        nox.call("scene.load", {"path": SCENE})
        deadline = time.monotonic() + READY_TIMEOUT_S
        loaded = False
        while time.monotonic() < deadline:
            current = nox.call("engine.info")
            if scene_ready(current) and current.get("scene", "").endswith("automation_e2e.json"):
                loaded = True
                break
            time.sleep(0.25)
        check("scene.load requests a scene swap that completes", loaded)

        # Shader reload (render.reload_shaders): clean tree, a broken shader is kept and the frame still renders, then
        # the file is restored in a finally block and the next reload is clean again.
        clean = nox.call("render.reload_shaders")
        check("render.reload_shaders on the clean tree keeps nothing", clean.get("kept") == 0,
              f"reloaded={clean.get('reloaded')} kept={clean.get('kept')} errors={clean.get('errors')}")

        shader = ROOT / "shaders" / "fullscreen_vert.glsl"
        backup = shader.read_bytes()
        try:
            shader.write_bytes(backup + b"\nthis line is not glsl;\n")
            broken = nox.call("render.reload_shaders")
            check("a broken shader is kept, not swapped in", broken.get("kept", 0) >= 1,
                  f"kept={broken.get('kept')} first error={(broken.get('errors') or [''])[0]}")
            shot = nox.call("render.screenshot", {"path": "captures/automation/e2e_shader_broken.png"})
            ok, detail = image_not_black(ROOT / shot["path"])
            check("the frame still renders after a broken shader (not black)", ok, detail)
        finally:
            shader.write_bytes(backup)
        restored = nox.call("render.reload_shaders")
        check("restoring the shader reloads cleanly (kept == 0)", restored.get("kept") == 0,
              f"reloaded={restored.get('reloaded')} kept={restored.get('kept')}")

        # Ctrl+Shift+R through input.inject target "editor" (ImGui IO): the editor's shortcut runs the reload.
        for key, state in [("Left Ctrl", "down"), ("Left Shift", "down"), ("R", "down"), ("R", "up"),
                           ("Left Shift", "up"), ("Left Ctrl", "up")]:
            nox.call("input.inject", {"key": key, "state": state, "target": "editor"})
            time.sleep(0.15)
        time.sleep(0.5)
        log_lines = nox.call("log.tail", {"lines": 2000})["lines"]
        check("Ctrl+Shift+R through input.inject target editor runs the shader reload",
              any("[Shaders] reload:" in line.get("text", "") for line in log_lines))

        # Model hot reload (assets.reload and the editor's file watch). A copy of cube.glb under a scratch folder in the
        # project feeds a new node; the file is replaced by sphere.glb; the file watch (or assets.reload) reloads it,
        # the frame still renders, a broken file is kept, and 100 reloads leave the GPU object counts unchanged.
        # The folder and the node are removed in a finally block.
        hot_rel = "captures/automation/hot_reload_e2e"
        hot_dir = ROOT / hot_rel
        hot_file = hot_dir / "hot.glb"
        hot_logical = hot_rel + "/hot.glb"
        hot_node_id = None
        try:
            hot_dir.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(ROOT / "models" / "cube.glb", hot_file)
            created_hot = nox.call("node.create", {"node": {"name": "E2E_HotModel", "type": "model",
                                                             "position": [0, 0, -4], "path": hot_logical}})
            hot_node_id = created_hot["id"]
            hot_props = nox.call("node.get", {"id": hot_node_id}).get("properties", {})
            hot_model = hot_props.get("MeshRenderer", {}).get("model")
            check("a node created with a model path shows that model", hot_model == hot_logical, f"model={hot_model}")
            base_objects = nox.call("engine.info").get("gpu_objects", {})

            shutil.copyfile(ROOT / "models" / "sphere.glb", hot_file)
            watched = False
            deadline = time.time() + 8.0
            while time.time() < deadline:
                lines = nox.call("log.tail", {"lines": 2000})["lines"]
                if any(f"[HotReload] reloaded model {hot_logical}" in line.get("text", "") for line in lines):
                    watched = True
                    break
                time.sleep(0.25)
            check("the editor's file watch reloads a model whose file was replaced", watched)
            if not watched:
                forced = nox.call("assets.reload", {"path": hot_logical})
                check("assets.reload reloads the replaced model", len(forced.get("reloaded", [])) == 1,
                      f"reloaded={forced.get('reloaded')} failed={forced.get('failed')}")
            shot = nox.call("render.screenshot", {"path": "captures/automation/e2e_model_reload.png"})
            ok, detail = image_not_black(ROOT / shot["path"])
            check("the frame still renders after a model reload (not black)", ok, detail)

            # A broken file keeps the previous model and reports it; the same path reloads once the file is valid again.
            hot_file.write_bytes(b"this is not a glb file")
            broken = nox.call("assets.reload", {"path": hot_logical})
            check("a broken model file is kept, not swapped in",
                  len(broken.get("failed", [])) == 1 and not broken.get("reloaded"),
                  f"failed={broken.get('failed')}")
            shutil.copyfile(ROOT / "models" / "cube.glb", hot_file)
            restored_hot = nox.call("assets.reload", {"path": hot_logical})
            check("the same path reloads once the file is valid again", len(restored_hot.get("reloaded", [])) == 1,
                  f"reloaded={restored_hot.get('reloaded')} failed={restored_hot.get('failed')}")

            # Leak check: 100 reloads of the same model, then a settle period for the retired models to be freed.
            loops_reloaded = 0
            for _ in range(100):
                loops_reloaded += len(nox.call("assets.reload", {"path": hot_logical}).get("reloaded", []))
            check("100 forced reloads all succeed", loops_reloaded == 100, f"reloaded={loops_reloaded}")
            time.sleep(1.5)
            after_objects = nox.call("engine.info").get("gpu_objects", {})
            shot_after = nox.call("render.screenshot", {"path": "captures/automation/e2e_model_reload_after_100.png"})
            ok_after, detail_after = image_not_black(ROOT / shot_after["path"])
            check("the frame still renders after 100 model reloads (not black)", ok_after, detail_after)
            check("100 model reloads leave the GPU object counts unchanged",
                  after_objects == base_objects, f"before={base_objects} after={after_objects}")
        finally:
            if hot_node_id is not None:
                try:
                    nox.call("node.delete", {"id": hot_node_id})
                except NoxError:
                    pass
            shutil.rmtree(hot_dir, ignore_errors=True)

        # Protocol robustness
        bad = raw_line_roundtrip(b"{this is not json")
        check("malformed JSON gets a parse error (-32700)", bad.get("error", {}).get("code") == -32700)
        try:
            nox.call("no.such.command")
            check("unknown command gets -32601", False, "no error returned")
        except NoxError as exc:
            check("unknown command gets -32601", exc.code == -32601)
        check("server still answers after protocol errors", nox.call("engine.info").get("protocol") == "2.0")

        # Shutdown
        nox.call("engine.quit")
    try:
        code = proc.wait(timeout=60)
        check("engine.quit exits the process cleanly", code == 0, f"exit code {code}")
    except subprocess.TimeoutExpired:
        check("engine.quit exits the process cleanly", False, "still running after 60 s")


if __name__ == "__main__":
    sys.exit(main())
