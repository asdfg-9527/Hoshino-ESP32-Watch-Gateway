#!/usr/bin/env python3
"""Phone-side mock for the Hoshino ESP32 SD storage API.

Uses only Python stdlib so it can run inside Termux. It mirrors the read-only
ESP32 endpoints and is useful for QuickApp/API testing before SD hardware is
connected.
"""

from __future__ import annotations

import argparse
import base64
import json
import os
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

MAX_PATH = 160
MAX_CHUNK = 2048


class StorageApi:
    def __init__(self, root: Path, token: str):
        self.root = root.resolve()
        self.root.mkdir(parents=True, exist_ok=True)
        self.token = token

    def resolve(self, relative: str, allow_root: bool = True) -> Path:
        text = (relative or "").strip().lstrip("/")
        if len(text.encode("utf-8")) > MAX_PATH:
            raise ValueError("path_too_long")
        if not text:
            if allow_root:
                return self.root
            raise ValueError("path_required")
        parts = text.split("/")
        if any((not p) or p in {".", ".."} or len(p.encode("utf-8")) > 64 for p in parts):
            raise ValueError("invalid_path_segment")
        if any(any(ord(ch) < 0x20 or ch in "\\:?#" for ch in p) for p in parts):
            raise ValueError("invalid_path_character")
        candidate = (self.root / Path(*parts)).resolve()
        try:
            candidate.relative_to(self.root)
        except ValueError as exc:
            raise ValueError("invalid_path") from exc
        return candidate


class Handler(BaseHTTPRequestHandler):
    server_version = "HoshinoStorageMock/1.0"

    @property
    def api(self) -> StorageApi:
        return self.server.api  # type: ignore[attr-defined]

    def log_message(self, fmt: str, *args) -> None:
        print("[storage-mock] " + fmt % args, flush=True)

    def send_json(self, code: int, payload: dict) -> None:
        raw = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(raw)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(raw)

    def fail(self, code: int, error: str, detail: str = "") -> None:
        self.send_json(code, {"ok": False, "error": error, "detail": detail})

    def authorized(self) -> bool:
        token = self.headers.get("X-Hoshino-Token", "")
        if token != self.api.token:
            self.fail(401, "unauthorized", "X-Hoshino-Token required")
            return False
        return True

    def do_GET(self) -> None:
        if not self.authorized():
            return
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query, keep_blank_values=True)
        try:
            if parsed.path == "/api/v1/storage/status":
                usage = os.statvfs(self.api.root)
                total = usage.f_frsize * usage.f_blocks
                free = usage.f_frsize * usage.f_bavail
                self.send_json(200, {
                    "ok": True,
                    "mounted": True,
                    "root": str(self.api.root),
                    "cardType": "phone-mock",
                    "cardSizeBytes": total,
                    "totalBytes": total,
                    "usedBytes": total - free,
                    "freeBytes": free,
                })
                return

            if parsed.path == "/api/v1/storage/list":
                rel = query.get("path", [""])[0]
                target = self.api.resolve(rel, allow_root=True)
                if not target.exists():
                    self.fail(404, "storage_list_failed", "not_found")
                    return
                if not target.is_dir():
                    self.fail(400, "not_a_directory", rel)
                    return
                entries = []
                truncated = False
                for index, child in enumerate(sorted(target.iterdir(), key=lambda p: p.name.lower())):
                    if index >= 48:
                        truncated = True
                        break
                    entries.append({
                        "name": child.name,
                        "directory": child.is_dir(),
                        "size": 0 if child.is_dir() else child.stat().st_size,
                    })
                self.send_json(200, {"ok": True, "path": rel, "items": entries, "count": len(entries), "truncated": truncated})
                return

            if parsed.path in {"/api/v1/storage/file", "/api/v1/storage/chunk"}:
                rel = query.get("path", [""])[0]
                if not rel:
                    self.fail(400, "path_required", "use ?path=relative/file.bin")
                    return
                target = self.api.resolve(rel, allow_root=False)
                if not target.exists():
                    self.fail(404, "storage_read_failed", "not_found")
                    return
                if target.is_dir():
                    self.fail(400, "is_a_directory", rel)
                    return

                if parsed.path.endswith("/file"):
                    size = target.stat().st_size
                    self.send_response(200)
                    self.send_header("Content-Type", "application/octet-stream")
                    self.send_header("Content-Length", str(size))
                    self.send_header("X-Hoshino-Storage", "phone-mock")
                    self.end_headers()
                    with target.open("rb") as fp:
                        while True:
                            chunk = fp.read(64 * 1024)
                            if not chunk:
                                break
                            self.wfile.write(chunk)
                    return

                offset = int(query.get("offset", ["0"])[0] or 0)
                length = int(query.get("length", ["1024"])[0] or 1024)
                if offset < 0 or length <= 0 or length > MAX_CHUNK:
                    self.fail(400, "invalid_range", "offset>=0 and 1<=length<=2048 required")
                    return
                size = target.stat().st_size
                if offset > size:
                    self.fail(416, "range_not_satisfiable", "offset exceeds file size")
                    return
                with target.open("rb") as fp:
                    fp.seek(offset)
                    data = fp.read(min(length, size - offset))
                next_offset = offset + len(data)
                self.send_json(200, {
                    "ok": True,
                    "encoding": "base64",
                    "offset": offset,
                    "bytes": len(data),
                    "nextOffset": next_offset,
                    "size": size,
                    "eof": next_offset >= size,
                    "data": base64.b64encode(data).decode("ascii"),
                })
                return

            self.fail(404, "not_found", parsed.path)
        except ValueError as exc:
            self.fail(400, str(exc))
        except Exception as exc:  # test harness: surface detail for debugging
            self.fail(500, "internal_error", repr(exc))


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", default="/sdcard/HoshinoBridge/storage")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8088)
    parser.add_argument("--token", default=os.environ.get("HOSHINO_TOKEN", "hoshino-local"))
    args = parser.parse_args()

    api = StorageApi(Path(args.root), args.token)
    server = ThreadingHTTPServer((args.host, args.port), Handler)
    server.api = api  # type: ignore[attr-defined]
    print(f"HOSHINO_STORAGE_MOCK_READY host={args.host} port={args.port} root={api.root}", flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
