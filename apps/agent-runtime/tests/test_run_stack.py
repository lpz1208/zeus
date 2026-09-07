"""Supervisor ordering with isolated child processes and no network ports."""

import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import time

import pytest


@pytest.mark.parametrize("fail_control", [False, True])
def test_supervisor_waits_for_control_and_drains_benchmark_first(tmp_path, fail_control):
    scripts = tmp_path / "scripts"
    scripts.mkdir()
    source = Path(__file__).resolve().parents[3] / "scripts" / "run-stack.sh"
    shutil.copyfile(source, scripts / "run-stack.sh")
    (tmp_path / "build").mkdir()
    binaries = tmp_path / "bin"
    binaries.mkdir()

    def executable(path, body):
        path.write_text(f"#!{sys.executable}\n" + body)
        path.chmod(0o755)

    executable(tmp_path / "build" / "zeus-server", """
import os, signal, sys
from pathlib import Path
if os.getenv('FAIL_CONTROL') == '1':
    sys.exit(1)
def stop(*_):
    Path('control-stopped').write_text(str(Path('benchmark-stopped').exists()))
    sys.exit(0)
signal.signal(signal.SIGTERM, stop)
Path('control.pid').write_text(str(os.getpid()))
while True:
    signal.pause()
""")
    executable(binaries / "curl", """
import sys
from pathlib import Path
if not Path('control.pid').exists():
    sys.exit(1)
Path('health-checked').touch()
""")
    executable(binaries / "uv", """
import os, signal, sys
from pathlib import Path
assert Path('health-checked').exists(), 'benchmark started before control readiness'
def stop(*_):
    os.kill(int(Path('control.pid').read_text()), 0)
    Path('benchmark-stopped').touch()
    sys.exit(0)
signal.signal(signal.SIGTERM, stop)
Path('benchmark-started').touch()
while True:
    signal.pause()
""")
    env = {**os.environ, "PATH": str(binaries) + os.pathsep + os.environ["PATH"],
           "FAIL_CONTROL": str(int(fail_control)),
           "ZEUS_DATA_DIR": str(tmp_path / "data"),
           "ZEUS_UV_CACHE_DIR": str(tmp_path / "cache")}
    process = subprocess.Popen(["bash", str(scripts / "run-stack.sh")],
                               env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                               text=True, start_new_session=True)
    try:
        if fail_control:
            process.communicate(timeout=5)
            assert process.returncode != 0
            assert not (tmp_path / "benchmark-started").exists()
            return
        deadline = time.monotonic() + 5
        while not (tmp_path / "benchmark-started").exists():
            assert process.poll() is None
            assert time.monotonic() < deadline
            time.sleep(0.01)
        process.terminate()
        process.communicate(timeout=5)
        assert (tmp_path / "benchmark-stopped").exists()
        assert (tmp_path / "control-stopped").read_text() == "True"
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.communicate(timeout=5)
