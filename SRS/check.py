"""Run the explicit SRS/OBS integration checks with generated media and silent device output."""
import ctypes
import argparse
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--beauty', action='store_true', help='Verify beauty using the existing local example image and engine.')
parser.add_argument('--latency', action='store_true', help='Compare original and low-latency budgets using generated pixel frame IDs.')
args = parser.parse_args()
qt = Path(os.environ.get('CSN_QT_ROOT', 'C:/software/Qt/6.11.2/msvc2022_64'))
env = os.environ.copy()
env['PATH'] = str(qt / 'bin') + os.pathsep + env.get('PATH', '')
env['QT_PLUGIN_PATH'] = str(qt / 'plugins')
env['QT_QPA_PLATFORM'] = 'windows'
if os.name == 'nt':
    ctypes.WinDLL('kernel32').SetErrorMode(0x8003)
sys.stdout.reconfigure(encoding='utf-8')
try:
    command = [str(root / 'OBS client/build-agent/OBSStreamingTests.exe')]
    if args.latency:
        command.append('--latency')
    if args.beauty:
        command += ['--beauty', '--engine', str(root / 'cpp/out/scrfd_final_smoke.engine'),
                    '--image', str(root / 'cpp/out/assets/示例人脸.jpg')]
    result = subprocess.run(command,
                            env=env, cwd=root, capture_output=True, timeout=90 if args.latency else 45,
                            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
except subprocess.TimeoutExpired as error:
    print((error.stderr or b'').decode('utf-8', errors='replace'))
    raise SystemExit('SRS integration checks timed out.')
print(result.stdout.decode('utf-8', errors='replace'))
print(result.stderr.decode('utf-8', errors='replace'))
raise SystemExit(result.returncode)
