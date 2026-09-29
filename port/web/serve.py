#!/usr/bin/env python3
"""Serve the web build for local testing (docs/wasm-spike.md).

  python3 port/web/serve.py --maps /path/to/maps [--port 8000]

Serves port/web/shell and build/web at /, and the maps folder at /maps/
(with Range requests and a generated /maps/index.json), with the
cross-origin isolation headers (COOP/COEP) that SharedArrayBuffer and so
WebAssembly threads need. Laravel's /play and /maps must send the same.
"""

import argparse
import http.server
import json
import os
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map,
                      ".js": "text/javascript", ".wasm": "application/wasm", ".map": "application/octet-stream"}
    maps_dir: Path = Path(".")

    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        self.send_header("Cross-Origin-Resource-Policy", "same-origin")
        self.send_header("Cache-Control", "no-store")
        if self.path.startswith("/maps/"):
            self.send_header("Accept-Ranges", "bytes")
        super().end_headers()

    def translate_path(self, path):
        path = path.split("?", 1)[0].split("#", 1)[0]
        if path.startswith("/maps/"):
            return str(self.maps_dir / os.path.basename(path))
        name = os.path.basename(path) or "play.html"
        for base in (ROOT / "port/web/shell", ROOT / "build/web"):
            if (base / name).is_file():
                return str(base / name)
        return str(ROOT / "build/web" / name)

    def maps_index(self):
        maps = [{"name": p.name, "size": p.stat().st_size} for p in sorted(self.maps_dir.iterdir())
                if p.suffix.lower() == ".map"]
        body = json.dumps(maps).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        return body

    def do_HEAD(self):
        self.handle_get(head=True)

    def do_GET(self):
        self.handle_get(head=False)

    def handle_get(self, head):
        if self.path.split("?")[0] == "/maps/index.json":
            body = self.maps_index()
            if not head:
                self.wfile.write(body)
            return
        path = Path(self.translate_path(self.path))
        match = re.match(r"bytes=(\d+)-(\d*)", self.headers.get("Range", ""))
        if not path.is_file() or not match:
            return super().do_HEAD() if head else super().do_GET()
        size = path.stat().st_size
        start = int(match.group(1))
        end = min(int(match.group(2)) if match.group(2) else size - 1, size - 1)
        self.send_response(206)
        self.send_header("Content-Type", self.guess_type(str(path)))
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.send_header("Content-Length", str(end - start + 1))
        self.end_headers()
        if not head:
            with open(path, "rb") as f:
                f.seek(start)
                self.wfile.write(f.read(end - start + 1))

    def log_message(self, fmt, *args):
        if os.environ.get("HALO_SERVE_QUIET") != "1":
            super().log_message(fmt, *args)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8000)
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--maps", type=Path, required=True)
    args = parser.parse_args()
    Handler.maps_dir = args.maps.resolve()
    with http.server.ThreadingHTTPServer((args.bind, args.port), Handler) as server:
        print(f"http://{args.bind}:{args.port}/ (maps from {Handler.maps_dir})")
        server.serve_forever()


if __name__ == "__main__":
    main()
