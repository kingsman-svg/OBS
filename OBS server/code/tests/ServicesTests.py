"""真实进程/套接字集成测试；随机端口，不占用开发服务的预留端口。"""
from http import client as http_client
import json
import os
from pathlib import Path
import secrets
import socket
import struct
import subprocess
import sys
import tempfile
import time
import unittest

BUILD = Path(sys.argv.pop(1)).resolve()


def free_ports(count):
    sockets = [socket.socket() for _ in range(count)]
    try:
        for sock in sockets:
            sock.bind(('127.0.0.1', 0))
        return [sock.getsockname()[1] for sock in sockets]
    finally:
        for sock in sockets:
            sock.close()


def http(port, path, body=None):
    connection = http_client.HTTPConnection('127.0.0.1', port, timeout=2)
    try:
        headers = {'Content-Type': 'application/json'} if body is not None else {}
        connection.request('POST' if body is not None else 'GET', path,
                           json.dumps(body) if body is not None else None, headers)
        response = connection.getresponse()
        return response.status, json.loads(response.read())
    finally:
        connection.close()


def eventually(predicate, timeout=6):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        try:
            if predicate():
                return
        except (OSError, http_client.HTTPException):
            pass
        time.sleep(.05)
    raise AssertionError('condition did not become true before deadline')


class SignalClient:
    def __init__(self, port):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=4)
        self.sequence = 0
        self.events = []

    def close(self):
        self.sock.close()

    @staticmethod
    def frame(message):
        body = json.dumps(message, ensure_ascii=False).encode()
        return struct.pack('!I', len(body)) + body

    def exact(self, count):
        result = bytearray()
        while len(result) < count:
            data = self.sock.recv(count - len(result))
            if not data:
                raise EOFError('signal connection closed')
            result.extend(data)
        return bytes(result)

    def receive(self):
        length, = struct.unpack('!I', self.exact(4))
        return json.loads(self.exact(length))

    def request(self, kind, **fields):
        self.sequence += 1
        request_id = str(self.sequence)
        self.sock.sendall(self.frame({'id': request_id, 'type': kind, **fields}))
        while True:
            response = self.receive()
            if response.get('type') == 'event':
                self.events.append(response)
            elif response.get('id') == request_id:
                return response
            else:
                raise AssertionError('unexpected signal response id')

    def event(self, name):
        for index, event in enumerate(self.events):
            if event.get('event') == name:
                return self.events.pop(index)
        while True:
            response = self.receive()
            if response.get('event') == name:
                return response
            self.events.append(response)


class ServiceTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='obs-services-')
        cls.login1, cls.login2, cls.scheduler, cls.signal = free_ports(4)
        cls.config = Path(cls.temp.name) / 'config.json'
        cls.config.write_text(json.dumps({
            'bindAddress': '127.0.0.1', 'loginPort': cls.login1,
            'schedulerPort': cls.scheduler, 'signalPort': cls.signal,
            'tokenTtlSeconds': 3600, 'signalIdleSeconds': 2, 'signalAuthSeconds': 1,
            'rtmpBase': 'rtmp://127.0.0.1:1935/live',
            'playbackBase': 'http://127.0.0.1:8088/live',
            'loginNodes': [
                {'id': 'one', 'address': '127.0.0.1', 'port': cls.login1,
                 'publicUrl': f'http://127.0.0.1:{cls.login1}/auth/login'},
                {'id': 'two', 'address': '127.0.0.1', 'port': cls.login2,
                 'publicUrl': f'http://127.0.0.1:{cls.login2}/auth/login'}]}))
        cls.environment = dict(os.environ, OBS_TOKEN_SECRET=secrets.token_hex(32))
        cls.processes = []
        cls.logs = []
        try:
            cls.first = cls.launch('login', cls.login1)
            cls.second = cls.launch('login', cls.login2)
            cls.launch('scheduler', cls.scheduler)
            cls.launch('signal', cls.signal)
            eventually(lambda: http(cls.scheduler, '/health')[1].get('healthyNodes') == 2)
            eventually(lambda: cls.signal_ready())
        except BaseException:
            cls.tearDownClass()
            raise

    @classmethod
    def signal_ready(cls):
        with socket.create_connection(('127.0.0.1', cls.signal), timeout=.2):
            return True

    @classmethod
    def launch(cls, role, port):
        log = tempfile.TemporaryFile()
        cls.logs.append(log)
        process = subprocess.Popen([str(BUILD / ('obs_' + role)), '--config', str(cls.config),
                                    '--port', str(port)], env=cls.environment,
                                   stdout=log, stderr=subprocess.STDOUT)
        cls.processes.append(process)
        return process

    @classmethod
    def tearDownClass(cls):
        failures = []
        for process in cls.processes:
            if process.poll() is None:
                process.terminate()
            try:
                code = process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
                failures.append('server required SIGKILL')
                continue
            if code != 0:
                failures.append(f'server exit status {code}')
        # ASan/UBSan 输出也检查；仅失败时输出进程日志，服务日志不含认证正文。
        for log in cls.logs:
            log.seek(0)
            content = log.read().decode(errors='replace')
            if 'Sanitizer' in content or 'runtime error:' in content:
                failures.append(content)
            log.close()
        cls.temp.cleanup()
        if failures:
            raise AssertionError('\n'.join(failures))

    def token(self, port=None):
        status, body = http(port or self.login1, '/auth/login', {'account': 'root', 'password': 'root'})
        self.assertEqual(status, 200)
        self.assertTrue(isinstance(body.get('accessToken'), str))
        return body['accessToken']

    def authenticated(self, port=None):
        client = SignalClient(self.signal)
        self.addCleanup(client.close)
        self.assertTrue(client.request('auth', token=self.token(port))['ok'])
        return client

    def test_01_login_and_http_errors(self):
        self.token()
        self.token(self.login2)
        self.assertEqual(http(self.login1, '/auth/login', {'account': 'root', 'password': 'wrong'})[0], 401)
        self.assertEqual(http(self.login1, '/auth/login', {'account': 5, 'password': 'root'})[0], 400)
        self.assertEqual(http(self.login1, '/missing')[0], 404)
        self.assertEqual(http(self.login1, '/auth/login')[0], 405)
        with socket.create_connection(('127.0.0.1', self.login1), timeout=2) as client:
            client.sendall(b'POST /auth/login HTTP/1.1\r\nHost: local\r\nContent-Length: 2\r\n'
                           b'Transfer-Encoding: chunked\r\n\r\n{}')
            self.assertTrue(client.recv(4096).startswith(b'HTTP/1.1 400'))

    def test_02_scheduler_round_robin(self):
        nodes = [http(self.scheduler, '/login/server')[1]['nodeId'] for _ in range(4)]
        self.assertEqual(nodes[0], nodes[2])
        self.assertNotEqual(nodes[0], nodes[1])
        self.assertTrue(http(self.scheduler, '/login/server')[1]['loginUrl'].endswith('/auth/login'))

    def test_03_signal_fragmented_coalesced_and_auth(self):
        client = SignalClient(self.signal)
        self.addCleanup(client.close)
        self.assertEqual(client.request('room.list')['error']['code'], 'UNAUTHORIZED')
        self.assertEqual(client.request('auth', token='invalid')['error']['code'], 'UNAUTHORIZED')
        frame = client.frame({'id': 'fragment', 'type': 'auth', 'token': self.token(self.login2)})
        for part in (frame[:2], frame[2:7], frame[7:]):
            client.sock.sendall(part)
        self.assertTrue(client.receive()['ok'])
        client.sock.sendall(client.frame({'id': 'a', 'type': 'heartbeat'}) +
                            client.frame({'id': 'b', 'type': 'room.list'}))
        self.assertEqual([client.receive()['id'], client.receive()['id']], ['a', 'b'])

    def test_04_live_owner_viewer_and_disconnect(self):
        owner, viewer = self.authenticated(), self.authenticated()
        room = owner.request('room.create', title='测试直播间')['data']['roomId']
        self.assertTrue(viewer.request('room.join', roomId=room)['ok'])
        self.assertEqual(viewer.request('live.start')['error']['code'], 'FORBIDDEN')
        started = owner.request('live.start')
        self.assertTrue(started['ok'] and started['data']['pushUrl'].startswith('rtmp://'))
        self.assertTrue(viewer.event('live.changed')['data']['streaming'])
        self.assertTrue(owner.request('live.stop')['ok'])
        self.assertFalse(viewer.event('live.changed')['data']['streaming'])
        owner.close()
        self.assertEqual(viewer.event('room.closed')['data']['roomId'], room)
        self.assertTrue(viewer.request('room.create', title='清理后可重新创建')['ok'])

    def test_05_leave_and_missing_room(self):
        owner, viewer = self.authenticated(), self.authenticated()
        room = owner.request('room.create', title='离开流程')['data']['roomId']
        self.assertEqual(viewer.request('room.join', roomId='missing')['error']['code'], 'ROOM_NOT_FOUND')
        viewer.request('room.join', roomId=room)
        self.assertTrue(viewer.request('room.leave')['ok'])
        self.assertTrue(viewer.request('room.join', roomId=room)['ok'])
        owner.request('room.leave')
        self.assertEqual(viewer.event('room.closed')['data']['roomId'], room)

    def test_06_bad_frame_and_json(self):
        client = SignalClient(self.signal)
        self.addCleanup(client.close)
        client.sock.sendall(client.frame({'id': 'bad'}))
        self.assertEqual(client.receive()['error']['code'], 'INVALID_REQUEST')
        client.sock.sendall(client.frame({'id': 'x' * 60000, 'type': 'heartbeat'}))
        response = client.receive()
        self.assertEqual(response['error']['code'], 'INVALID_ID')
        self.assertIsNone(response['id'])
        client.sock.sendall(struct.pack('!I', 65537))
        self.assertEqual(client.sock.recv(1), b'')

    def test_07_auth_and_idle_deadlines(self):
        client = SignalClient(self.signal)
        self.addCleanup(client.close)
        self.assertEqual(client.sock.recv(1), b'')
        client = self.authenticated()
        self.assertEqual(client.sock.recv(1), b'')

    def test_08_http_absolute_request_timeout(self):
        with socket.create_connection(('127.0.0.1', self.login1), timeout=7) as client:
            client.sendall(b'POST /auth/login HTTP/1.1\r\nHost: local\r\nContent-Length: 2\r\n\r\n{')
            self.assertEqual(client.recv(1), b'')

    def test_09_login_rate_limit(self):
        statuses = [http(self.login2, '/auth/login', {'account': 'root', 'password': 'wrong'})[0]
                    for _ in range(130)]
        self.assertIn(429, statuses)

    def test_10_scheduler_down_and_recovery(self):
        self.first.terminate()
        self.first.wait(timeout=5)
        eventually(lambda: http(self.scheduler, '/health')[1].get('healthyNodes') == 1)
        self.assertEqual(http(self.scheduler, '/login/server')[1]['nodeId'], 'two')
        self.second.terminate()
        self.second.wait(timeout=5)
        eventually(lambda: http(self.scheduler, '/login/server')[0] == 503)
        self.launch('login', self.login1)
        eventually(lambda: http(self.scheduler, '/login/server')[0] == 200)
        self.assertEqual(http(self.scheduler, '/login/server')[1]['nodeId'], 'one')


if __name__ == '__main__':
    unittest.main(verbosity=2)
