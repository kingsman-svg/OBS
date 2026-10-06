"""Run the existing CMake tests with an explicit, process-local Qt environment."""
import argparse
import ctypes
import os
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--qt-root', default=os.environ.get('CSN_QT_ROOT', 'C:/software/Qt/6.11.2/msvc2022_64'))
parser.add_argument('--build-dir', default='CSN/build-agent')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
qt = Path(args.qt_root).resolve()
build = (root / args.build_dir).resolve()
environment = os.environ.copy()
environment['PATH'] = str(qt / 'bin') + os.pathsep + environment.get('PATH', '')
environment['QT_PLUGIN_PATH'] = str(qt / 'plugins')
environment['QT_QPA_PLATFORM'] = 'offscreen'
(root / 'out').mkdir(exist_ok=True)
environment['CSN_SCREENSHOT_PATH'] = str(root / 'out' / 'login-ui.png')
if os.name == 'nt':
    ctypes.WinDLL('kernel32').SetErrorMode(0x8003)
ctest = shutil.which('ctest')
if not ctest:
    raise SystemExit('ctest was not found. Add CMake/bin to PATH.')
result = subprocess.run([ctest, '--test-dir', str(build), '--output-on-failure', '-V'],
                        env=environment, timeout=30, capture_output=True)
print(result.stdout.decode('utf-8', errors='replace'))
print(result.stderr.decode('utf-8', errors='replace'))
raise SystemExit(result.returncode)
