"""Verify Player VOD using generated media; actual WASAPI output is explicit and silent."""
import argparse
import ctypes
import os
from pathlib import Path
import subprocess
import sys

parser = argparse.ArgumentParser()
parser.add_argument('--qt-root', default='C:/software/Qt/6.11.2/msvc2022_64')
parser.add_argument('--build-dir', default='OBS client/build-agent')
parser.add_argument('--media-file', help='Verify one explicitly selected local recording instead of generating the fixture suite.')
mode = parser.add_mutually_exclusive_group()
mode.add_argument('--audio-device', action='store_true', help='Use the default output device at volume zero; verify audio master and unequal track lengths.')
mode.add_argument('--mvc-only', action='store_true', help='Only verify buttons, window lifecycle and layout with audio output disabled.')
mode.add_argument('--adaptive-buffer', action='store_true', help='Verify adaptive playback buffer with synthetic PCM and silent real WASAPI output.')
args = parser.parse_args()
if os.name != 'nt':
    raise SystemExit('Player checks require Windows.')
sys.stdout.reconfigure(encoding='utf-8')
sys.stderr.reconfigure(encoding='utf-8')
root = Path(__file__).resolve().parents[1]
qt = Path(args.qt_root).resolve()
environment = os.environ.copy()
environment['PATH'] = str(qt / 'bin') + os.pathsep + environment.get('PATH', '')
environment['QT_PLUGIN_PATH'] = str(qt / 'plugins')
environment['QT_QPA_PLATFORM'] = 'windows'
environment.setdefault('OBS_UI_SCREENSHOTS', str(root / 'out'))
ctypes.WinDLL('kernel32').SetErrorMode(0x8003)
command = [str((root / args.build_dir / 'OBSPlaybackTests.exe').resolve())]
if args.audio_device:
    command.append('--audio-device')
elif args.mvc_only:
    command.append('--mvc-only')
elif args.adaptive_buffer:
    command.append('--adaptive-buffer')
if args.media_file:
    command.extend(['--media-file', str(Path(args.media_file).resolve())])
try:
    result = subprocess.run(command, env=environment, cwd=root, capture_output=True, timeout=60,
                            creationflags=subprocess.CREATE_NO_WINDOW)
except subprocess.TimeoutExpired as error:
    print((error.stderr or b'').decode('utf-8', errors='replace'))
    raise SystemExit('Playback checks timed out.')
print(result.stdout.decode('utf-8', errors='replace'))
print(result.stderr.decode('utf-8', errors='replace'))
raise SystemExit(result.returncode)
