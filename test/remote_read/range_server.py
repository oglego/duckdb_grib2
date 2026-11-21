"""A small HTTP file server for the remote-read tests: HEAD, Range requests, a request log, and a choice of how a missing
object is reported.

http.server's own handler ignores Range, and httpfs relies on it. This one serves the files under ``root``, optionally
below a URL prefix (so it can stand in for an S3 bucket in path style: ``/bucket/key``).

``missing_status`` is what a request for an object that does not exist gets:
  404  what a web server and S3 (with s3:ListBucket) answer
  403  what S3 answers for a missing key when the caller may not list the bucket
  401  an authentication failure, which the extension must NOT treat as "no such file"
``hidden`` names objects that exist on disk but are reported as missing, so one directory can serve a file with and
without its .idx.

Standard library only.
"""

import re
import threading
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

S3_ACCESS_DENIED = (
    b'<?xml version="1.0" encoding="UTF-8"?>\n<Error><Code>AccessDenied</Code><Message>Access Denied</Message></Error>'
)


class RangeServer:
    def __init__(self, root, prefix="", missing_status=404, hidden=()):
        self.root = Path(root)
        self.prefix = prefix.rstrip("/")
        self.missing_status = missing_status
        self.hidden = set(hidden)
        self.log = []  # (method, object name, range header or None, status, bytes sent)
        self._lock = threading.Lock()
        server = self

        class Handler(BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def log_message(self, *args):  # keep the test output quiet
                pass

            def _object(self):
                path = self.path.split("?", 1)[0]
                if self.server_ref.prefix:
                    if not path.startswith(self.server_ref.prefix + "/"):
                        return None
                    path = path[len(self.server_ref.prefix) :]
                name = path.lstrip("/")
                if not name or "/" in name or name in self.server_ref.hidden:
                    return None
                file = self.server_ref.root / name
                return (name, file) if file.is_file() else None

            def _send(self, status, body=b"", headers=(), head=False, name="", range_header=None):
                self.send_response(status)
                for key, value in headers:
                    self.send_header(key, value)
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                if not head and body:
                    self.wfile.write(body)
                with self.server_ref._lock:
                    self.server_ref.log.append((self.command, name, range_header, status, 0 if head else len(body)))

            def _serve(self, head):
                found = self._object()
                range_header = self.headers.get("Range")
                name = self.path.split("?", 1)[0].rsplit("/", 1)[-1]
                if found is None:
                    body = S3_ACCESS_DENIED if self.server_ref.missing_status == 403 else b""
                    self._send(self.server_ref.missing_status, body, head=head, name=name, range_header=range_header)
                    return
                name, file = found
                data = file.read_bytes()
                common = [
                    ("Accept-Ranges", "bytes"),
                    ("ETag", '"%x"' % len(data)),
                    ("Last-Modified", "Wed, 21 Oct 2026 07:28:00 GMT"),
                ]
                if head:
                    # Content-Length of the whole object, no body
                    self.send_response(200)
                    for key, value in common:
                        self.send_header(key, value)
                    self.send_header("Content-Length", str(len(data)))
                    self.end_headers()
                    with self.server_ref._lock:
                        self.server_ref.log.append(("HEAD", name, range_header, 200, 0))
                    return
                match = re.fullmatch(r"bytes=(\d*)-(\d*)", range_header or "")
                if match and (match.group(1) or match.group(2)):
                    if match.group(1):
                        start = int(match.group(1))
                        end = int(match.group(2)) if match.group(2) else len(data) - 1
                    else:  # suffix range: the last N bytes
                        start = max(0, len(data) - int(match.group(2)))
                        end = len(data) - 1
                    end = min(end, len(data) - 1)
                    if start >= len(data) or start > end:
                        self._send(
                            416,
                            b"",
                            [("Content-Range", "bytes */%d" % len(data))],
                            name=name,
                            range_header=range_header,
                        )
                        return
                    self._send(
                        206,
                        data[start : end + 1],
                        common + [("Content-Range", "bytes %d-%d/%d" % (start, end, len(data)))],
                        name=name,
                        range_header=range_header,
                    )
                else:
                    self._send(200, data, common, name=name, range_header=range_header)

            def do_GET(self):
                self._serve(head=False)

            def do_HEAD(self):
                self._serve(head=True)

        Handler.server_ref = server
        self.httpd = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.httpd.daemon_threads = True
        self.port = self.httpd.server_address[1]
        self._thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    def __enter__(self):
        self._thread.start()
        return self

    def __exit__(self, *exc):
        self.httpd.shutdown()
        self.httpd.server_close()

    @property
    def url(self):
        return "http://127.0.0.1:%d%s" % (self.port, self.prefix)

    def data_bytes_served(self, name):
        """Body bytes sent for GET requests of one object (the .idx and the headers are not counted)."""
        with self._lock:
            return sum(sent for method, obj, _, _, sent in self.log if method == "GET" and obj == name)

    def requests_for(self, name):
        with self._lock:
            return [entry for entry in self.log if entry[1] == name]
