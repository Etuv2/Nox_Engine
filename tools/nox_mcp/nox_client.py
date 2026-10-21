#!/usr/bin/env python3
"""Client for the NOX automation server (docs/automation.md). Standard library only.

Shell use:
    python tools/nox_mcp/nox_client.py engine.info
    python tools/nox_mcp/nox_client.py scene.tree
    python tools/nox_mcp/nox_client.py node.get '{"id": 3}'
    python tools/nox_mcp/nox_client.py --port 47801 render.screenshot '{"path": "captures/automation/x.png"}'

Prints the result as JSON. An engine error prints the JSON-RPC error to stderr and exits with status 1; a
missing engine exits with status 2.

Library use:
    from nox_client import NoxClient
    with NoxClient(port=47800) as nox:
        print(nox.call("scene.tree", {"depth": 2}))
"""
import argparse
import json
import socket
import sys

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 47800
DEFAULT_TIMEOUT = 150.0   # the server gives up on a command after 120 s; leave headroom


class NoxError(Exception):
    """A JSON-RPC error answered by the engine (or a protocol failure)."""

    def __init__(self, code, message, data=None):
        super().__init__(f"{message} (code {code})")
        self.code = code
        self.message = message
        self.data = data


class NoxClient:
    def __init__(self, host=DEFAULT_HOST, port=DEFAULT_PORT, timeout=DEFAULT_TIMEOUT):
        self.host = host
        self.port = port
        self.timeout = timeout
        self._sock = None
        self._buffer = b""
        self._next_id = 1

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *exc_info):
        self.close()

    def connect(self):
        if self._sock is None:
            self._sock = socket.create_connection((self.host, self.port), timeout=self.timeout)

    def close(self):
        if self._sock is not None:
            try:
                self._sock.close()
            finally:
                self._sock = None
                self._buffer = b""

    def call(self, method, params=None):
        """Sends one request and returns its result. Raises NoxError on an error response."""
        self.connect()
        request_id = self._next_id
        self._next_id += 1
        request = {"jsonrpc": "2.0", "id": request_id, "method": method, "params": params or {}}
        self._sock.sendall((json.dumps(request) + "\n").encode("utf-8"))
        response = json.loads(self._read_line().decode("utf-8"))
        if "error" in response:
            error = response["error"]
            raise NoxError(error.get("code", 0), error.get("message", "error"), error.get("data"))
        return response.get("result")

    def _read_line(self):
        while b"\n" not in self._buffer:
            chunk = self._sock.recv(65536)
            if not chunk:
                self.close()
                raise NoxError(-1, "connection closed by the engine")
            self._buffer += chunk
        line, _, self._buffer = self._buffer.partition(b"\n")
        return line


def main(argv=None):
    parser = argparse.ArgumentParser(description="Call one NOX automation command and print its result.")
    parser.add_argument("method", help="command name, e.g. scene.tree")
    parser.add_argument("params", nargs="?", default="{}", help="parameters as a JSON object (default {})")
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    parser.add_argument("--timeout", type=float, default=DEFAULT_TIMEOUT)
    args = parser.parse_args(argv)

    try:
        params = json.loads(args.params)
    except json.JSONDecodeError as exc:
        print(f"params is not valid JSON: {exc}", file=sys.stderr)
        return 1
    if not isinstance(params, dict):
        print("params must be a JSON object", file=sys.stderr)
        return 1

    try:
        with NoxClient(args.host, args.port, args.timeout) as nox:
            result = nox.call(args.method, params)
    except ConnectionRefusedError:
        print(f"no engine is listening on {args.host}:{args.port} (start it with --automation)", file=sys.stderr)
        return 2
    except OSError as exc:
        print(f"could not talk to the engine on {args.host}:{args.port}: {exc}", file=sys.stderr)
        return 2
    except NoxError as exc:
        print(json.dumps({"error": {"code": exc.code, "message": exc.message, "data": exc.data}}, indent=2),
              file=sys.stderr)
        return 1
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
