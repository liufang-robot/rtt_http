#!/usr/bin/env python3
"""Exercise the native server through the documented Nginx raw-path config."""
import http.client
import json
from pathlib import Path
import socket
import subprocess
import sys
import tempfile
import time


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def request(port, method, path, value=None):
    client = http.client.HTTPConnection("127.0.0.1", port, timeout=2)
    try:
        body = None if value is None else json.dumps({"value": value})
        client.request(method, path, body, {"Content-Type": "application/json"})
        response = client.getresponse()
        return response.status, response.read()
    finally:
        client.close()


def main():
    backend_port, proxy_port = free_port(), free_port()
    with tempfile.TemporaryDirectory(prefix="rtt-http-nginx-") as directory:
        root = Path(directory)
        config = root / "nginx.conf"
        # $request_uri is the original encoded URI; $uri is normalized and must
        # not be substituted. No rewrite rules or URI replacement are used.
        config.write_text(f"""worker_processes 1;
pid {root / 'nginx.pid'};
error_log {root / 'error.log'};
events {{ worker_connections 32; }}
http {{
  access_log off;
  client_body_temp_path {root / 'body'};
  proxy_temp_path {root / 'proxy'};
  fastcgi_temp_path {root / 'fastcgi'};
  uwsgi_temp_path {root / 'uwsgi'};
  scgi_temp_path {root / 'scgi'};
  server {{
    listen 127.0.0.1:{proxy_port};
    location /api/ {{
      proxy_pass http://127.0.0.1:{backend_port}$request_uri;
      proxy_http_version 1.1;
      proxy_set_header Connection "";
    }}
  }}
}}
""", encoding="utf-8")
        backend = subprocess.Popen([sys.argv[1], str(backend_port)],
                                   stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True)
        proxy = None
        try:
            # Readiness is explicitly emitted after publication; EOF is failure.
            if backend.stdout.readline().strip() != "ready":
                raise RuntimeError("native proxy fixture did not become ready")
            proxy = subprocess.Popen([sys.argv[2], "-p", directory, "-c", str(config),
                                      "-g", "daemon off;"],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            deadline = time.monotonic() + 3
            while True:
                if proxy.poll() is not None:
                    raise RuntimeError(proxy.stderr.read().decode())
                try:
                    status, _ = request(proxy_port, "GET", "/api/v1/components")
                    if status == 200:
                        break
                except OSError:
                    pass
                if time.monotonic() >= deadline:
                    raise RuntimeError("Nginx readiness deadline expired")
                time.sleep(0.01)
            names = ["motion%2Fraw", "motion%252Fraw", "axes%2B%C3%A4", "state%23raw"]
            paths = [f"/api/v1/components/arm/services/{name}/properties/value" for name in names]
            for i, path in enumerate(paths):
                status, body = request(proxy_port, "GET", path)
                assert status == 200 and json.loads(body)["value"] == 11 * (i + 1)
                assert request(proxy_port, "PUT", path, 100 + i)[0] == 204
            for i, path in enumerate(paths):
                status, body = request(proxy_port, "GET", path)
                assert status == 200 and json.loads(body)["value"] == 100 + i
            assert request(proxy_port, "GET", "/api/v1/components/arm/services/motion/raw/properties/value")[0] == 404
            print("Nginx preserves distinct encoded names for reads and whole writes")
        finally:
            if proxy is not None:
                proxy.terminate()
                try:
                    proxy.communicate(timeout=3)
                except subprocess.TimeoutExpired:
                    proxy.kill()
                    proxy.communicate()
            try:
                backend.communicate("\n", timeout=3)
            except subprocess.TimeoutExpired:
                backend.kill()
                backend.communicate()
        if backend.returncode != 0:
            raise RuntimeError("native proxy fixture failed shutdown")


if __name__ == "__main__":
    main()
