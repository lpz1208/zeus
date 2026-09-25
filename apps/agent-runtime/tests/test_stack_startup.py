"""Exercise the real supervisor with small local child services."""
import os
from pathlib import Path
import select
import shutil
import socket
import subprocess
import time

ROOT = Path(__file__).resolve().parents[3]


def free_port():
    with socket.socket() as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def fixture_stack(tmp_path):
    (tmp_path / 'scripts').mkdir()
    (tmp_path / 'build').mkdir()
    (tmp_path / 'bin').mkdir()
    shutil.copy(ROOT / 'scripts/run-stack.sh', tmp_path / 'scripts/run-stack.sh')
    server = tmp_path / 'build/zeus-server'
    server.write_text('''#!/usr/bin/env python3
import http.server, sys, os
address = sys.argv[sys.argv.index('--addr')+1]
host, port = address.rsplit(':', 1)
class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        self.send_response(200)
        self.end_headers()
        self.wfile.write(b'{"ok":true,"service":"zeus-control-server"}' if self.path == '/api/live' else b'{}')
    def log_message(self, *args): pass
http.server.HTTPServer((host or '0.0.0.0', int(port)), Handler).serve_forever()
''')
    server.chmod(0o755)
    uv = tmp_path / 'bin/uv'
    uv.write_text('#!/usr/bin/env python3\nimport time\ntime.sleep(60)\n')
    uv.chmod(0o755)
    env = os.environ.copy()
    env.update(PATH=str(tmp_path / 'bin') + os.pathsep + env['PATH'],
               ZEUS_ADDR=f'0.0.0.0:{free_port()}', ZEUS_STARTUP_TIMEOUT='2',
               HTTP_PROXY='http://127.0.0.1:1', http_proxy='http://127.0.0.1:1',
               ALL_PROXY='http://127.0.0.1:1', all_proxy='http://127.0.0.1:1',
               NO_PROXY='', no_proxy='')
    for key in ('ZEUS_CONTROL_BASE_URL', 'ZEUS_DATA_DIR', 'ZEUS_UV_CACHE_DIR'):
        env.pop(key, None)
    return env


def test_custom_port_starts_even_with_broken_proxy(tmp_path):
    env = fixture_stack(tmp_path)
    process = subprocess.Popen(['bash', 'scripts/run-stack.sh'], cwd=tmp_path, env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    output = b''
    try:
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline and b'Zeus stack starting:' not in output:
            ready, _, _ = select.select([process.stdout], [], [], 0.2)
            if ready:
                chunk = os.read(process.stdout.fileno(), 4096)
                if not chunk:
                    break
                output += chunk
        assert b'Zeus stack starting:' in output, output.decode()
        port = env['ZEUS_ADDR'].rsplit(':', 1)[1]
        assert f'web=http://127.0.0.1:{port}'.encode() in output
    finally:
        process.terminate()
        process.communicate(timeout=5)


def test_failed_probe_reports_address_and_curl_error(tmp_path):
    env = fixture_stack(tmp_path)
    env['ZEUS_CONTROL_BASE_URL'] = f'http://127.0.0.1:{free_port()}'
    result = subprocess.run(['bash', 'scripts/run-stack.sh'], cwd=tmp_path, env=env,
                            capture_output=True, text=True, timeout=8)
    assert result.returncode == 1
    assert env['ZEUS_CONTROL_BASE_URL'] + '/api/live' in result.stderr
    assert 'curl=7' in result.stderr and 'HTTP=000' in result.stderr
