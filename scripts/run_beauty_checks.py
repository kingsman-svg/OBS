"""Verify local beauty; explicit GPU mode uses the existing example image/engine and a generated window."""
import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--gpu', action='store_true')
parser.add_argument('--engine', default='cpp/out/scrfd_final_smoke.engine')
parser.add_argument('--image', default='cpp/out/assets/示例人脸.jpg')
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
command = [str(root / args.build_dir / 'OBSBeautyTests.exe')]
if args.gpu:
    command += ['--gpu', '--engine', str((root / args.engine).resolve()), '--image', str((root / args.image).resolve())]
result = subprocess.run(command, cwd=root, env=environment, capture_output=True, timeout=90,
                        creationflags=subprocess.CREATE_NO_WINDOW)
print(result.stdout.decode('utf-8', errors='replace'))
print(result.stderr.decode('utf-8', errors='replace'))
raise SystemExit(result.returncode)
