#!/usr/bin/env python3

import json
import os
import socket
import subprocess
import sys
import tempfile
import threading


def serve_once(sock_path: str, ready: threading.Event) -> None:
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as server:
        server.bind(sock_path)
        server.listen(1)
        ready.set()
        with server.accept()[0]:
            pass
        with server.accept()[0] as client:
            request = b""
            while not request.endswith(b"\n"):
                chunk = client.recv(4096)
                if not chunk:
                    return
                request += chunk
            parsed = json.loads(request)
            response = {
                "id": parsed["id"],
                "ok": True,
                "protocol_version": 3,
                "result": {"alive": True, "protocol_version": 3},
            }
            client.sendall(json.dumps(response, separators=(",", ":")).encode() + b"\n")


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: TestAgentWaitReady.py <kyty_agent>", file=sys.stderr)
        return 125

    with tempfile.TemporaryDirectory(prefix="kyty_agent_wait_ready_") as temp_dir:
        sock_path = os.path.join(temp_dir, "agent.sock")
        ready = threading.Event()
        server = threading.Thread(target=serve_once, args=(sock_path, ready), daemon=True)
        server.start()
        if not ready.wait(timeout=5):
            print("fake agent server did not become ready", file=sys.stderr)
            return 1

        completed = subprocess.run(
            [sys.argv[1], "--sock", sock_path, "wait-ready", "--timeout-ms", "2000"],
            check=False,
            capture_output=True,
            text=True,
            timeout=5,
        )
        server.join(timeout=2)

    lines = [line for line in completed.stdout.splitlines() if line.strip()]
    if completed.returncode != 0:
        print(completed.stderr, file=sys.stderr)
        print(completed.stdout, file=sys.stderr)
        return 1
    if len(lines) != 1:
        print(f"expected one JSON object, got {len(lines)}", file=sys.stderr)
        return 1
    result = json.loads(lines[0])
    if result.get("protocol_version") != 3 or not result.get("ok"):
        print(f"unexpected wait-ready response: {result}", file=sys.stderr)
        return 1
    if not result.get("result", {}).get("ready"):
        print(f"wait-ready result is not ready: {result}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
