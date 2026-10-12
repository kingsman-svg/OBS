"""Run the explicit SRS/OBS integration checks with generated media and silent device output."""
import ctypes
import os
from pathlib import Path
import subprocess
import sys

root = Path(__file__).resolve().parents[1]
qt = Path(os.environ.get('CSN_QT_ROOT', 'C:/software/Qt/6.11.2/msvc2022_64'))
env = os.environ.copy()
env['PATH'] = str(qt / 'bin') + os.pathsep + env.get('PATH', '')
env['QT_PLUGIN_PATH'] = str(qt / 'plugins')
env['QT_QPA_PLATFORM'] = 'windows'
if os.name == 'nt':
    ctypes.WinDLL('kernel32').SetErrorMode(0x8003)
sys.stdout.reconfigure(encoding='utf-8')
try:
    result = subprocess.run([str(root / 'OBS client/build-agent/OBSStreamingTests.exe')],
                            env=env, cwd=root, capture_output=True, timeout=45,
                            creationflags=subprocess.CREATE_NO_WINDOW if os.name == 'nt' else 0)
except subprocess.TimeoutExpired as error:
    print((error.stderr or b'').decode('utf-8', errors='replace'))
    raise SystemExit('SRS integration checks timed out.')
print(result.stdout.decode('utf-8', errors='replace'))
print(result.stderr.decode('utf-8', errors='replace'))
raise SystemExit(result.returncode)
