"""Run capture unit checks, or explicit native Windows hardware checks."""
import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--qt-root', default='C:/software/Qt/6.11.2/msvc2022_64')
parser.add_argument('--build-dir', default='OBS client/build-agent')
parser.add_argument('--hardware', action='store_true')
args = parser.parse_args()
if os.name != 'nt':
    raise SystemExit('Capture checks require Windows.')
sys.stdout.reconfigure(encoding='utf-8')
sys.stderr.reconfigure(encoding='utf-8')
root = Path(__file__).resolve().parents[1]
qt = Path(args.qt_root).resolve()
environment = os.environ.copy()
environment['PATH'] = str(qt / 'bin') + os.pathsep + environment.get('PATH', '')
environment['QT_PLUGIN_PATH'] = str(qt / 'plugins')
environment['QT_QPA_PLATFORM'] = 'windows'
(root / 'out').mkdir(exist_ok=True)
environment['OBS_CAPTURE_SCREENSHOT'] = str(root / 'out' / '推流端采集页面.png')
ctypes.WinDLL('kernel32').SetErrorMode(0x8003)
command = [str((root / args.build_dir / 'OBSCaptureTests.exe').resolve())]
if args.hardware:
    command.append('--hardware')
try:
    result = subprocess.run(command, env=environment, cwd=root, capture_output=True,
                            timeout=60, creationflags=subprocess.CREATE_NO_WINDOW)
except subprocess.TimeoutExpired as error:
    print((error.stderr or b'').decode('utf-8', errors='replace'))
    raise SystemExit('Capture checks timed out.')
print(result.stdout.decode('utf-8', errors='replace'))
print(result.stderr.decode('utf-8', errors='replace'))
raise SystemExit(result.returncode)
