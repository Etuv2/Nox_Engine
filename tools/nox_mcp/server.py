#!/usr/bin/env python3
"""MCP server for the NOX engine: lets Claude Code build, launch, inspect and drive the engine.

Implements the MCP stdio transport directly (newline-delimited JSON-RPC 2.0, protocol 2025-06-18) with the
standard library only. Registered for this repository by the project .mcp.json. Documentation: docs/automation.md.

Tools:
  engine_build, engine_launch, engine_wait_ready, engine_stop    manage the engine process
  engine_* / scene_* / node_* / render_* / log_tail / lua_exec /
  camera_* / input_inject / editor_select                         one tool per automation command
Every command tool forwards its arguments as the JSON-RPC params of the same-named command. render_screenshot
also returns the PNG as an MCP image block.

Logs go to stderr; stdout carries protocol messages only.
"""
import base64
import json
import os
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from nox_client import DEFAULT_PORT, NoxClient, NoxError  # noqa: E402

PROTOCOL_VERSION = "2025-06-18"
SERVER_NAME = "nox"
SERVER_VERSION = "0.1.0"

ROOT = Path(__file__).resolve().parents[2]
ENGINE_EXE = ROOT / "build" / "vs2022-x64" / "Release" / "Nox_Engine.exe"
CMAKE_CANDIDATES = [
    Path("C:/Program Files/Microsoft Visual Studio/2022/Community/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"),
    Path("C:/Program Files/Microsoft Visual Studio/2022/Professional/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe"),
]
LOG_DIR = ROOT / "captures" / "automation"

# The engine process this server launched (one at a time).
ENGINE = {"proc": None, "port": None, "log": None, "command": None}


def log(message):
    print(f"[nox-mcp] {message}", file=sys.stderr, flush=True)


class ToolError(Exception):
    """A failure reported to the model as a tool result with isError set."""


# ---- Engine process management -------------------------------------------------------------------

def engine_running():
    proc = ENGINE["proc"]
    return proc is not None and proc.poll() is None


def engine_port():
    return ENGINE["port"] or DEFAULT_PORT


def call_engine(method, params=None, port=None):
    with NoxClient(port=port or engine_port()) as nox:
        return nox.call(method, params or {})


def log_tail(path, lines=40):
    try:
        text = Path(path).read_text(encoding="utf-8", errors="replace").splitlines()
    except OSError:
        return "(no log)"
    return "\n".join(text[-lines:])


def find_cmake():
    for candidate in CMAKE_CANDIDATES:
        if candidate.exists():
            return str(candidate)
    return "cmake"


def tool_engine_build(args):
    cmake = find_cmake()
    steps = []
    if args.get("configure"):
        steps.append([cmake, "--preset", "vs2022-x64"])
    steps.append([cmake, "--build", "--preset", "vs2022-x64-release", "--parallel", "6"])
    output = []
    for step in steps:
        proc = subprocess.run(step, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, encoding="utf-8", errors="replace", timeout=3600)
        output.append(proc.stdout)
        if proc.returncode != 0:
            text = "\n".join(output).splitlines()
            return {"ok": False, "exit_code": proc.returncode, "command": " ".join(step),
                    "tail": "\n".join(text[-80:])}
    text = "\n".join(output).splitlines()
    return {"ok": True, "exit_code": 0, "exe": str(ENGINE_EXE), "exe_exists": ENGINE_EXE.exists(),
            "tail": "\n".join(text[-40:])}


def tool_engine_launch(args):
    if engine_running():
        raise ToolError(f"an engine is already running (pid {ENGINE['proc'].pid}); call engine_stop first")
    exe = Path(args["exe"]) if args.get("exe") else ENGINE_EXE
    if not exe.is_absolute():
        exe = ROOT / exe
    if not exe.exists():
        raise ToolError(f"engine executable not found: {exe} (build it first with engine_build)")
    port = int(args.get("port") or DEFAULT_PORT)
    command = [str(exe), f"--automation={port}"]
    if args.get("preset"):
        command += ["--preset", args["preset"]]
    elif args.get("mode") == "game":
        command += ["--game"]
    elif args.get("mode") == "editor":
        command += ["--editor"]
    if args.get("scene"):
        command += ["--scene", args["scene"]]
    if args.get("quality"):
        command += ["--quality", args["quality"]]
    command += [str(a) for a in args.get("extra_args", [])]

    env = dict(os.environ)
    for key, value in (args.get("env") or {}).items():
        env[str(key)] = str(value)

    LOG_DIR.mkdir(parents=True, exist_ok=True)
    log_path = LOG_DIR / f"engine_{port}.log"
    handle = open(log_path, "w", encoding="utf-8", errors="replace")
    proc = subprocess.Popen(command, cwd=ROOT, env=env, stdin=subprocess.DEVNULL,
                            stdout=handle, stderr=subprocess.STDOUT)
    ENGINE.update({"proc": proc, "port": port, "log": str(log_path), "command": command})
    log(f"launched pid {proc.pid}: {' '.join(command)}")
    return {"pid": proc.pid, "port": port, "log": str(log_path), "command": command}


def scene_ready(info):
    """True when no scene load is running and a scene is loaded (engine.info loading/scene/load_error)."""
    return not info.get("loading") and bool(info.get("scene")) and not info.get("load_error")


def tool_engine_wait_ready(args):
    timeout = float(args.get("timeout_s", 180))
    deadline = time.monotonic() + timeout
    last_error = None
    while time.monotonic() < deadline:
        if ENGINE["proc"] is not None and ENGINE["proc"].poll() is not None:
            raise ToolError(f"engine exited with code {ENGINE['proc'].returncode} before becoming ready. "
                            f"Log tail:\n{log_tail(ENGINE['log'])}")
        try:
            info = call_engine("engine.info")
            if info.get("load_error"):
                raise ToolError(f"scene load failed: {info['load_error']}")
            if scene_ready(info):
                return info
        except (OSError, NoxError) as exc:
            last_error = exc
        time.sleep(0.5)
    raise ToolError(f"engine not ready after {timeout:.0f} s (last error: {last_error}). "
                    f"Log tail:\n{log_tail(ENGINE['log']) if ENGINE['log'] else '(no engine launched by this server)'}")


def tool_engine_stop(args):
    proc = ENGINE["proc"]
    if proc is None:
        return {"stopped": False, "reason": "no engine launched by this server"}
    if proc.poll() is None:
        try:
            call_engine("engine.quit")
        except (OSError, NoxError) as exc:
            log(f"engine.quit failed ({exc}); waiting for exit anyway")
        try:
            proc.wait(timeout=20)
        except subprocess.TimeoutExpired:
            if args.get("force", True):
                proc.kill()
                proc.wait(timeout=10)
            else:
                raise ToolError("engine did not exit within 20 s (force=false)")
    code = proc.returncode
    ENGINE["proc"] = None
    return {"stopped": True, "exit_code": code}


# ---- Command tools -------------------------------------------------------------------------------

def passthrough(method, render_image=False):
    def run(args):
        result = call_engine(method, args)
        if not render_image:
            return result
        path = ROOT / result["path"]
        data = base64.b64encode(path.read_bytes()).decode("ascii")
        return ("image", result, data)
    return run


def obj(properties=None, required=(), description=None):
    schema = {"type": "object", "properties": properties or {}, "additionalProperties": False}
    if required:
        schema["required"] = list(required)
    if description:
        schema["description"] = description
    return schema


INT = {"type": "integer"}
NUM = {"type": "number"}
STR = {"type": "string"}
BOOL = {"type": "boolean"}
NODE_ID = {"type": "integer", "description": "node id from scene_tree / node_find (session-scoped)"}
VEC3 = {"type": "array", "items": NUM, "minItems": 3, "maxItems": 3}

COMMAND_TOOLS = [
    ("engine_info", "engine.info", "Engine version, scene path, loading state, frame index, fps, quality tier, mode and pause state.",
     obj()),
    ("engine_quit", "engine.quit", "Ask the engine to exit cleanly after the current frame.", obj()),
    ("engine_pause", "engine.pause", "Pause the simulation. Rendering and automation commands keep running.", obj()),
    ("engine_resume", "engine.resume", "Resume the simulation (also cancels a step in progress).", obj()),
    ("engine_step", "engine.step",
     "Pause, then simulate exactly `frames` frames. Returns after the last frame has rendered. Waits for the window to be visible.",
     obj({"frames": {"type": "integer", "minimum": 1, "maximum": 100000, "default": 1}})),
    ("scene_list", "scene.list", "List the scene files under scenes/ (logical paths).", obj()),
    ("scene_load", "scene.load",
     "Switch to another scene file (applied at the start of the next frame; poll engine_info.loading).",
     obj({"path": {"type": "string", "description": "logical path, e.g. scenes/cornell_box.json"}}, ["path"])),
    ("scene_save", "scene.save",
     "Save the current scene. Without a path, writes the authored source file. With a path, writes the scene graph to that .json file.",
     obj({"path": STR})),
    ("scene_tree", "scene.tree", "Node hierarchy: id, name, type, components and children.",
     obj({"depth": {"type": "integer", "description": "levels below the root; -1 = all (default)"}})),
    ("node_get", "node.get", "All reflected properties of a node and of each of its components.",
     obj({"id": NODE_ID}, ["id"])),
    ("node_set", "node.set", "Set one reflected property of a node's component. Validated and range-checked by the engine.",
     obj({"id": NODE_ID, "component": STR, "property": STR, "value": {"description": "JSON value of the property's type"}},
         ["id", "component", "property", "value"])),
    ("node_create", "node.create",
     "Create a node from scene-file node JSON (e.g. {\"name\": \"Crate\", \"type\": \"node\", \"position\": [0,1,0]}) under parent.",
     obj({"parent": {"type": ["integer", "null"], "description": "parent id; null = scene root"},
          "node": {"type": "object", "description": "scene-file node object"}})),
    ("node_delete", "node.delete", "Delete a node and its subtree.", obj({"id": NODE_ID}, ["id"])),
    ("node_reparent", "node.reparent", "Move a node under another parent (null = scene root).",
     obj({"id": NODE_ID, "parent": {"type": ["integer", "null"]}, "index": INT,
          "keep_world": {"type": "boolean", "default": True}}, ["id"])),
    ("node_find", "node.find", "Find nodes by exact name and/or type (model, light, audio, camera, gui, folder, node ...).",
     obj({"name": STR, "type": STR})),
    ("render_settings_get", "render.settings.get",
     "Render settings: every render_settings key, plus surfel GI, debug views and quality_tier. Pass keys to filter.",
     obj({"keys": {"type": "array", "items": STR}})),
    ("render_settings_set", "render.settings.set",
     "Set one render setting ({key, value}) or several ({settings: {key: value}}). quality_tier applies a tier from the scene file.",
     obj({"key": STR, "value": {}, "settings": {"type": "object"}})),
    ("render_pass_timings", "render.pass_timings", "Per-pass CPU and GPU milliseconds of the last rendered frame.", obj()),
    ("log_tail", "log.tail", "Last engine log lines (stdout and stderr) with level info, warning or error.",
     obj({"lines": {"type": "integer", "minimum": 1, "maximum": 20000, "default": 200}})),
    ("lua_exec", "lua.exec",
     "Run a Lua chunk in the loaded project's Lua world (the VM the game scripts run in, with the nox API). "
     "Errors include the traceback. Set isolated=true to run in a separate VM instead. Returns the first return value.",
     obj({"code": STR, "chunk": {"type": "string", "default": "=automation"},
          "isolated": {"type": "boolean", "default": False,
                       "description": "true: run in the separate automation VM, not the project world"}}, ["code"])),
    ("camera_get", "camera.get", "Camera position, rotation [yaw, pitch] degrees, fov, near, far, aspect.", obj()),
    ("camera_set", "camera.set",
     "Move the camera. rotation = [yaw, pitch] degrees, or lookAt = point to face. fov in degrees.",
     obj({"position": VEC3, "rotation": {"type": "array", "items": NUM, "minItems": 2, "maxItems": 2},
          "lookAt": VEC3, "fov": NUM, "near": NUM, "far": NUM})),
    ("input_inject", "input.inject",
     "Inject a key or mouse button event through the normal input path (applies on the next frame).",
     obj({"key": {"type": "string", "description": "SDL key name, e.g. W, Space, Left"},
          "mouse": {"type": "string", "enum": ["left", "right", "middle"]},
          "state": {"type": "string", "enum": ["down", "up"]}, "x": INT, "y": INT}, ["state"])),
    ("editor_select", "editor.select", "Select a node in the editor (editor mode only). Omit id to clear.",
     obj({"id": {"type": ["integer", "null"]}})),
    ("render_screenshot", "render.screenshot",
     "Write a PNG of the backbuffer (UI included) and return it as an image.",
     obj({"path": {"type": "string", "description": "relative to the repository root; default captures/automation/screenshot_<frame>.png"}})),
]

LAUNCH_TOOLS = [
    ("engine_build", tool_engine_build,
     "Build Nox_Engine.exe (Release). Set configure=true the first time or after CMake file changes. Returns the output tail.",
     obj({"configure": {"type": "boolean", "default": False}})),
    ("engine_launch", tool_engine_launch,
     "Start Nox_Engine.exe with --automation. Choose a preset, or mode game/editor, plus scene, quality and env vars.",
     obj({"preset": STR, "mode": {"type": "string", "enum": ["game", "editor"]}, "scene": STR, "quality": STR,
          "port": {"type": "integer", "minimum": 1, "maximum": 65535},
          "env": {"type": "object", "additionalProperties": {"type": "string"}},
          "extra_args": {"type": "array", "items": STR}, "exe": STR})),
    ("engine_wait_ready", tool_engine_wait_ready,
     "Wait until the automation server answers and the scene has finished loading.",
     obj({"timeout_s": {"type": "number", "default": 120}})),
    ("engine_stop", tool_engine_stop,
     "Quit the engine launched by engine_launch (engine.quit, then kill after 20 s unless force=false).",
     obj({"force": {"type": "boolean", "default": True}})),
]

TOOLS = {}
for name, method, description, schema in COMMAND_TOOLS:
    TOOLS[name] = {
        "description": description,
        "schema": schema,
        "run": passthrough(method, render_image=(name == "render_screenshot")),
    }
for name, handler, description, schema in LAUNCH_TOOLS:
    TOOLS[name] = {"description": description, "schema": schema, "run": handler}


def tool_list():
    return [{"name": name, "description": spec["description"], "inputSchema": spec["schema"]}
            for name, spec in TOOLS.items()]


def text_block(value):
    return {"type": "text", "text": value if isinstance(value, str) else json.dumps(value, indent=2)}


def run_tool(name, args):
    spec = TOOLS[name]
    try:
        outcome = spec["run"](args)
    except ToolError as exc:
        return {"content": [text_block(str(exc))], "isError": True}
    except NoxError as exc:
        return {"content": [text_block(f"engine error {exc.code}: {exc.message}" +
                                       (f"\n{json.dumps(exc.data)}" if exc.data else ""))], "isError": True}
    except OSError as exc:
        return {"content": [text_block(f"cannot reach the engine on port {engine_port()} ({exc}). "
                                       f"Start it with engine_launch, or with --automation.")], "isError": True}
    except Exception as exc:  # noqa: BLE001 - every failure must come back as a tool result
        return {"content": [text_block(f"{type(exc).__name__}: {exc}")], "isError": True}
    if isinstance(outcome, tuple) and outcome and outcome[0] == "image":
        _, result, data = outcome
        return {"content": [text_block(result),
                            {"type": "image", "data": data, "mimeType": "image/png"}]}
    return {"content": [text_block(outcome if outcome is not None else {"ok": True})]}


# ---- JSON-RPC / MCP plumbing ---------------------------------------------------------------------

def send(message):
    sys.stdout.buffer.write((json.dumps(message) + "\n").encode("utf-8"))
    sys.stdout.buffer.flush()


def send_result(request_id, result):
    send({"jsonrpc": "2.0", "id": request_id, "result": result})


def send_error(request_id, code, message):
    send({"jsonrpc": "2.0", "id": request_id, "error": {"code": code, "message": message}})


def handle(message):
    if not isinstance(message, dict):
        send_error(None, -32600, "request must be a JSON object")
        return
    method = message.get("method")
    if method is None:
        return   # a response to a request of ours; none are sent
    is_notification = "id" not in message
    request_id = message.get("id")
    params = message.get("params") or {}

    if method == "initialize":
        requested = params.get("protocolVersion")
        send_result(request_id, {
            "protocolVersion": requested if requested == PROTOCOL_VERSION else PROTOCOL_VERSION,
            "capabilities": {"tools": {"listChanged": False}},
            "serverInfo": {"name": SERVER_NAME, "version": SERVER_VERSION},
            "instructions": ("Drive the NOX engine. Typical loop: engine_build (once), engine_launch, "
                             "engine_wait_ready, then scene_tree / node_get / node_set / render_screenshot / "
                             "engine_step / log_tail, and engine_stop when done."),
        })
    elif method.startswith("notifications/"):
        return
    elif method == "ping":
        if not is_notification:
            send_result(request_id, {})
    elif method == "tools/list":
        send_result(request_id, {"tools": tool_list()})
    elif method == "tools/call":
        name = params.get("name")
        arguments = params.get("arguments") or {}
        if name not in TOOLS:
            send_error(request_id, -32602, f"unknown tool: {name}")
            return
        if not isinstance(arguments, dict):
            send_error(request_id, -32602, "arguments must be an object")
            return
        send_result(request_id, run_tool(name, arguments))
    elif is_notification:
        return
    else:
        send_error(request_id, -32601, f"method not found: {method}")


def main():
    log(f"server starting (repo {ROOT})")
    for raw in sys.stdin.buffer:
        # utf-8-sig: a byte-order mark some pipes prepend to the first line is dropped.
        line = raw.decode("utf-8-sig", errors="replace").strip()
        if not line:
            continue
        try:
            message = json.loads(line)
        except json.JSONDecodeError:
            send_error(None, -32700, "parse error")
            continue
        try:
            handle(message)
        except Exception as exc:  # noqa: BLE001 - keep serving
            log(f"unhandled error: {exc!r}")
            if isinstance(message, dict) and "id" in message:
                send_error(message.get("id"), -32603, f"internal error: {exc}")
    if ENGINE["proc"] is not None and ENGINE["proc"].poll() is None:
        log("client disconnected; the launched engine is left running (call engine_stop to quit it)")


if __name__ == "__main__":
    main()
