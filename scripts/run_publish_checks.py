"""Check publisher telemetry/queue recovery; explicit GPU and isolated RTMP backpressure checks use generated media."""
import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--gpu', action='store_true')
parser.add_argument('--weak-network', action='store_true', help='Explicit NVIDIA/RTMP blocked-write recovery test using generated media and a local TCP proxy.')
parser.add_argument('--qt-root', default='C:/software/Qt/6.11.2/msvc2022_64')
parser.add_argument('--build-dir', default='OBS client/build-agent')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
qt = Path(args.qt_root)
environment = os.environ.copy()
environment['PATH'] = str(qt / 'bin') + os.pathsep + environment.get('PATH', '')
environment['QT_PLUGIN_PATH'] = str(qt / 'plugins')
environment['QT_QPA_PLATFORM'] = 'windows'
ctypes.WinDLL('kernel32').SetErrorMode(0x8003)
sys.stdout.reconfigure(encoding='utf-8')
command = [str(root / args.build_dir / 'OBSPublishControlTests.exe')]
if args.gpu:
    command.append('--gpu')
if args.weak_network:
    command.append('--weak-network')
result = subprocess.run(command, cwd=root, env=environment, capture_output=True, timeout=90,
                        creationflags=subprocess.CREATE_NO_WINDOW)
print(result.stdout.decode('utf-8', errors='replace'))
print(result.stderr.decode('utf-8', errors='replace'))
raise SystemExit(result.returncode)
