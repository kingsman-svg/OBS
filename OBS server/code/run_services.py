"""本地开发启动器：统一密钥、四个进程的启动/停止与失败收敛，不替代生产进程管理器。"""
import argparse
import fcntl
import http.client
import json
import os
from pathlib import Path
import secrets
import signal
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parent
parser = argparse.ArgumentParser()
parser.add_argument('action', choices=['start', 'stop', 'status', 'run'])
parser.add_argument('--build-dir', default=str(ROOT / 'build'))
parser.add_argument('--config', default=str(ROOT / 'services/config.json'))
args = parser.parse_args()
BUILD = Path(args.build_dir).resolve()
CONFIG = Path(args.config).resolve()
RUNTIME = BUILD / 'run'
STATE = RUNTIME / 'services.json'


def identity(pid):
    try:
        # starttime 防止 PID 复用后误停止其他进程；comm 字段可能包含空格。
        fields = Path(f'/proc/{pid}/stat').read_text().rsplit(')', 1)[1].split()
        return fields[19] if fields[0] != 'Z' else None
    except (OSError, IndexError):
        return None


def state():
    try:
        record = json.loads(STATE.read_text())
        if identity(record['pid']) == record['starttime']:
            return record
    except (OSError, ValueError, KeyError):
        pass
    return None


def environment():
    result = os.environ.copy()
    if not result.get('OBS_TOKEN_SECRET'):
        keyfile = Path.home() / '.config/obs/token-secret'
        keyfile.parent.mkdir(parents=True, exist_ok=True, mode=0o700)
        try:
            fd = os.open(keyfile, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        except FileExistsError:
            pass
        else:
            with os.fdopen(fd, 'w') as stream:
                stream.write(secrets.token_hex(32))
        result['OBS_TOKEN_SECRET'] = keyfile.read_text().strip()
    # 密钥不会进入配置 JSON、命令行参数和日志。root 卷保留它，进程重启不更换签名。
    if len(result['OBS_TOKEN_SECRET']) < 32:
        raise RuntimeError('signing key must contain at least 32 characters')
    return result


def health(port):
    connection = http.client.HTTPConnection('127.0.0.1', port, timeout=.5)
    try:
        connection.request('GET', '/health')
        response = connection.getresponse()
        data = json.loads(response.read())
        return response.status == 200 and data.get('ready') is True
    except (OSError, ValueError, http.client.HTTPException):
        return False
    finally:
        connection.close()


def run():
    RUNTIME.mkdir(parents=True, exist_ok=True)
    with (RUNTIME / 'services.lock').open('w') as lock:
        # 文件锁由内核随进程退出释放；两个启动器不能同时管理同一 build 目录。
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        config = json.loads(CONFIG.read_text())
        children = []
        stopping = False

        def request_stop(_number, _frame):
            nonlocal stopping
            stopping = True

        signal.signal(signal.SIGINT, request_stop)
        signal.signal(signal.SIGTERM, request_stop)
        record = {'pid': os.getpid(), 'starttime': identity(os.getpid()), 'ready': False}
        STATE.write_text(json.dumps(record))
        try:
            env = environment()
            jobs = [('login', node['port']) for node in config['loginNodes']]
            jobs += [('scheduler', config['schedulerPort']), ('signal', config['signalPort'])]
            for role, port in jobs:
                children.append(subprocess.Popen(
                    [str(BUILD / f'obs_{role}'), '--config', str(CONFIG), '--port', str(port)], env=env))
            deadline = time.monotonic() + 8
            while not stopping:
                if any(child.poll() is not None for child in children):
                    raise RuntimeError('one server exited; stopping the whole development group')
                if not record['ready']:
                    if all(health(node['port']) for node in config['loginNodes']) and health(config['schedulerPort']):
                        record['ready'] = True
                        STATE.write_text(json.dumps(record))
                        print('Development servers ready: login, scheduler, signal.', flush=True)
                    elif time.monotonic() >= deadline:
                        raise RuntimeError('development services did not become ready')
                time.sleep(.1)
        finally:
            for child in children:
                if child.poll() is None:
                    child.terminate()
            for child in children:
                try:
                    child.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait()
            STATE.unlink(missing_ok=True)


def main():
    record = state()
    if args.action == 'status':
        print('running' if record and record.get('ready') else 'stopped or starting')
        return 0 if record and record.get('ready') else 1
    if args.action == 'stop':
        if not record:
            print('already stopped')
            return 0
        os.kill(record['pid'], signal.SIGTERM)
        deadline = time.monotonic() + 8
        while identity(record['pid']) == record['starttime'] and time.monotonic() < deadline:
            time.sleep(.1)
        if identity(record['pid']) == record['starttime']:
            raise RuntimeError('supervisor did not stop before deadline')
        print('stopped')
        return 0
    if args.action == 'run':
        run()
        return 0
    if record:
        print('already running' if record.get('ready') else 'already starting')
        return 0
    RUNTIME.mkdir(parents=True, exist_ok=True)
    with (RUNTIME / 'services.log').open('ab') as log:
        supervisor = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), 'run',
            '--build-dir', str(BUILD), '--config', str(CONFIG)], stdout=log, stderr=log,
            stdin=subprocess.DEVNULL, start_new_session=True)
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if supervisor.poll() is not None:
            raise RuntimeError(f'start failed; inspect {RUNTIME / "services.log"}')
        record = state()
        if record and record.get('ready'):
            print('started: login 8081/8082, scheduler 8080, signal 9000 (default config)')
            return 0
        time.sleep(.1)
    raise RuntimeError(f'start timed out; inspect {RUNTIME / "services.log"}')


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, ValueError) as error:
        print(str(error), file=sys.stderr)
        raise SystemExit(1)
