"""Explicit D3D11VA decoding/GPU conversion checks using only generated media; audio volume zero."""
import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--qt-root', default='C:/software/Qt/6.11.2/msvc2022_64')
parser.add_argument('--build-dir', default='OBS client/build-agent')
parser.add_argument('--player-only', action='store_true', help='Only check the hardware player lifecycle and MVC.')
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
qt = Path(args.qt_root)
env = os.environ.copy()
env['PATH'] = str(qt / 'bin') + os.pathsep + env.get('PATH', '')
env['QT_PLUGIN_PATH'] = str(qt / 'plugins')
env['QT_QPA_PLATFORM'] = 'windows'
sys.stdout.reconfigure(encoding='utf-8')
if os.name == 'nt':
    ctypes.WinDLL('kernel32').SetErrorMode(0x8003)
try:
    command = [str(root / args.build_dir / 'OBSHardwareDecodeTests.exe')]
    if args.player_only:
        command.append('--player-only')
    result = subprocess.run(command, env=env, cwd=root,
                            capture_output=True, timeout=90,
                            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
except subprocess.TimeoutExpired as error:
    print((error.stderr or b'').decode('utf-8', errors='replace'))
    raise SystemExit('Hardware decode checks timed out.')
print(result.stdout.decode('utf-8', errors='replace'))
print(result.stderr.decode('utf-8', errors='replace'))
raise SystemExit(result.returncode)
