#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Philip Bos
"""The companion app on http://localhost (Web MIDI needs a secure page: localhost is one). No internet needed.
  python3 companion/serve.py [PORT]      then open the address it prints, in Chrome or Edge"""
import functools
import http.server
import sys
import webbrowser
from pathlib import Path

port = int(sys.argv[1]) if len(sys.argv) > 1 else 8747
class Handler(http.server.SimpleHTTPRequestHandler):
    extensions_map = {**http.server.SimpleHTTPRequestHandler.extensions_map, ".js": "text/javascript",
                      ".mjs": "text/javascript", ".efs": "application/octet-stream", ".fwsc": "application/octet-stream"}

    def end_headers(self):
        self.send_header("Cache-Control", "no-cache")   # an updated companion is always the one that runs
        super().end_headers()


class Server(http.server.ThreadingHTTPServer):
    request_queue_size = 64      # (Python's 5: Chrome asks for the page's modules all at once; macOS resets the rest)


handler = functools.partial(Handler, directory=str(Path(__file__).resolve().parent))
with Server(("127.0.0.1", port), handler) as httpd:
    url = f"http://localhost:{port}/"
    print(f"ECHOFIELD companion: {url}  (Chrome or Edge; Ctrl+C to stop)")
    if "--no-open" not in sys.argv:
        webbrowser.open(url)
    httpd.serve_forever()
