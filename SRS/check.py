"""Run the explicit SRS/OBS integration checks with generated media and silent device output."""
import ctypes
import argparse
import os
from pathlib import Path
import subprocess
import sys
import json
import time

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser()
parser.add_argument('--beauty', action='store_true', help='Verify beauty using the existing local example image and engine.')
parser.add_argument('--latency', action='store_true', help='Compare original and low-latency budgets using generated pixel frame IDs.')
parser.add_argument('--gateway', action='store_true', help='Verify the custom Linux gateway with the real Publisher and Edge WebRTC decoder.')
args = parser.parse_args()
qt = Path(os.environ.get('CSN_QT_ROOT', 'C:/software/Qt/6.11.2/msvc2022_64'))
env = os.environ.copy()
env['PATH'] = str(qt / 'bin') + os.pathsep + env.get('PATH', '')
env['QT_PLUGIN_PATH'] = str(qt / 'plugins')
env['QT_QPA_PLATFORM'] = 'windows'
if os.name == 'nt':
    ctypes.WinDLL('kernel32').SetErrorMode(0x8003)
sys.stdout.reconfigure(encoding='utf-8')


def gateway_check():
    # 复用已有集成入口，浏览器依赖仅此显式模式加载，不影响默认 7 组 CTest。
    sys.path.insert(0, str(root / 'out/media-check-deps'))
    from playwright.sync_api import sync_playwright
    output = root / 'out/媒体网关验证'
    output.mkdir(parents=True, exist_ok=True)
    stream = f'gateway_check_{os.getpid()}'
    stop_file = output / f'停止推流_{os.getpid()}.flag'
    stop_file.unlink(missing_ok=True)
    command = [str(root / 'OBS client/build-agent/OBSStreamingTests.exe'), '--gateway-source',
               '--stream', stream, '--stop-file', str(stop_file)]
    log = (output / 'Publisher网关验收.log').open('wb')
    publisher = subprocess.Popen(command, env=env, cwd=root, stdout=log, stderr=log,
                                 creationflags=subprocess.CREATE_NO_WINDOW)
    report = {'scope': 'H264+Opus; common RTP/SR clock; generated-signal browser sync check; local Docker only'}
    try:
        with sync_playwright() as automation:
            browser = automation.chromium.launch(channel='msedge', headless=True)
            context = browser.new_context(viewport={'width': 1100, 'height': 900})
            page = context.new_page()
            page.goto('http://127.0.0.1:8090/')
            assert context.request.get('http://127.0.0.1:8090/api/status').status == 401
            existing_login = context.request.post('http://127.0.0.1:8081/auth/login',
                data=json.dumps({'account': 'root', 'password': os.environ.get('OBS_LOGIN_PASSWORD', 'root')}),
                headers={'Content-Type': 'application/json'})
            assert existing_login.status == 200
            existing_token = existing_login.json()['accessToken']
            assert context.request.get('http://127.0.0.1:8090/api/status',
                headers={'Authorization': 'Bearer ' + existing_token}).status == 200
            del existing_token
            report['existingLoginTokenAccepted'] = True

            def login(target):
                target.locator('#account').fill('root')
                target.locator('#password').fill(os.environ.get('OBS_LOGIN_PASSWORD', 'root'))
                target.locator('#loginButton').click()
                target.wait_for_function("!document.getElementById('start').disabled")

            login(page)
            # 页面 api 函数使用内存令牌；不导出、打印或保存密码/SDP/令牌。
            rejected = page.evaluate("""async()=>{const r=await fetch('/api/source',{method:'POST',headers:{Authorization:'Bearer '+token,'Content-Type':'application/json'},body:JSON.stringify({stream:'../invalid'})});return r.status;}""")
            assert rejected == 400
            page.evaluate("name=>api('/api/source','POST',{stream:name})", 'missing_' + stream)
            started = time.monotonic()
            assert context.request.get('http://127.0.0.1:8090/health').status == 200
            report['healthDuringMissingUpstreamMs'] = round((time.monotonic() - started) * 1000, 1)
            assert report['healthDuringMissingUpstreamMs'] < 1500

            def wait_frames(target, minimum, timeout=25):
                deadline = time.monotonic() + timeout
                stats = {}
                while time.monotonic() < deadline:
                    stats = target.evaluate('window.gatewayStats()')
                    if stats.get('framesDecoded', 0) >= minimum:
                        return stats
                    target.wait_for_timeout(200)
                raise AssertionError('browser decoding did not reach the expected frame count')

            def watch(target):
                target.locator('#stream').fill(stream)
                target.locator('#start').click()
                try:
                    return wait_frames(target, 30)
                except Exception:
                    # 仅记录去敏后的业务状态与接收统计，避免 Playwright 日志携带协商正文。
                    report['pageMessage'] = target.locator('#message').inner_text()
                    report['gateway'] = target.evaluate("api('/api/status')")
                    raise

            report['firstViewer'] = watch(page)
            assert report['firstViewer']['connected'] and report['firstViewer']['width'] == 320
            status = page.evaluate("api('/api/status')")
            report['inputProfile'] = status['profile']
            incompatible = page.evaluate("""async()=>{const r=await fetch('/api/sessions',{method:'POST',headers:{Authorization:'Bearer '+token,'Content-Type':'application/json'},body:JSON.stringify({sdp:'v=0\\r\\n'})});return r.status;}""")
            assert incompatible == 406
            report['incompatibleOfferRejected'] = True
            def start_probe(target):
                # 实际消费音频，元素音量和 WebAudio 增益为零，不向用户设备播放测试音。
                target.evaluate("document.getElementById('video').volume=0")
                if target.evaluate("document.getElementById('video').muted"):
                    target.locator('#sound').click()
                target.evaluate("""async()=>{
                const video=document.getElementById('video');
                const audio=video.srcObject.getAudioTracks();if(audio.length!==1)throw Error('missing audio track');
                const context=new AudioContext({sampleRate:48000,latencyHint:'interactive'});
                const source=context.createMediaStreamSource(new MediaStream(audio)),analyser=context.createAnalyser(),gain=context.createGain();
                analyser.fftSize=1024;gain.gain.value=0;source.connect(analyser);analyser.connect(gain);gain.connect(context.destination);await context.resume();
                const canvas=document.createElement('canvas');canvas.width=320;canvas.height=180;const draw=canvas.getContext('2d',{willReadFrequently:true});
                const pcm=new Float32Array(1024);window.syncProbe={context,rows:[],running:true};
                const next=(time,metadata)=>{if(!window.syncProbe.running)return;draw.drawImage(video,0,0,320,180);let marker=0;
                    for(let bit=0;bit<32;bit++){const p=draw.getImageData(20+bit*8,24,1,1).data;if(p[0]+p[1]+p[2]>384)marker=(marker|(1<<bit))>>>0;}
                    analyser.getFloatTimeDomainData(pcm);let energy=0;for(const sample of pcm)energy+=sample*sample;
                    const patch=draw.getImageData(296,146,1,1).data;
                    window.syncProbe.rows.push({id:marker&65535,check:marker>>>16,rms:Math.sqrt(energy/pcm.length),on:patch[0]+patch[1]+patch[2]>384,timeMs:metadata.expectedDisplayTime});
                    video.requestVideoFrameCallback(next);
                };video.requestVideoFrameCallback(next);
                }""")

            def finish_probe(target, label, duration_ms=8200):
                target.wait_for_timeout(1800)
                target.evaluate('window.syncProbe.rows=[]')
                target.wait_for_timeout(duration_ms)
                rows = target.evaluate('window.syncProbe.rows')
                target.evaluate('window.syncProbe.running=false;window.syncProbe.context.close()')
                assert len(rows) >= 140
                valid = [row for row in rows if row['id'] and row['check'] == (0xa500 | ((row['id'] & 255) ^ (row['id'] >> 8) ^ 0x5a))]
                assert len(valid) >= len(rows) * .95
                def signal_on(milliseconds):
                    return bool((0x6d5a4e37 >> (int(milliseconds // 200) % 31)) & 1)
                assert all(row['on'] == signal_on((row['id']-1)*1000/30) for row in valid)
                assert max(row['rms'] for row in valid) > .04 and min(row['rms'] for row in valid) < .005
                # 包络给出两个浏览器消费分支的相对偏移；不等同物理声卡/显示器嘴型测量。
                candidates = []
                for lag in range(-500, 501, 10):
                    mismatch = sum((row['rms'] > .025) != signal_on((row['id']-1)*1000/30+lag) for row in valid)
                    candidates.append((mismatch, abs(lag), lag))
                mismatch, _, lag = min(candidates)
                report[label] = {'rows': len(valid), 'audioRelativeVideoMs': lag,
                    'matchRatio': round(1-mismatch/len(valid), 4), 'maxRms': round(max(row['rms'] for row in valid), 5),
                    'scope': 'rendered video pixels vs silent WebAudio decoded envelope; 10ms search step; not physical output latency'}
                assert abs(lag) <= 150 and mismatch / len(valid) < .12

            start_probe(page)
            finish_probe(page, 'audioVideoPatternCheck')
            report['audioAfterWarmup'] = page.evaluate('window.gatewayStats()')
            assert report['audioAfterWarmup']['audioSamples'] > 96000
            status = page.evaluate("api('/api/status')")
            assert status['audioAvailable'] and status['audioInputRate'] == 48000 and status['audioPackets'] > 200
            assert status['viewers'][0]['audioSenderReports'] >= 5 and status['viewers'][0]['videoSenderReports'] >= 5
            page.screenshot(path=str(output / '自建网关浏览器播放.png'), full_page=True)
            # 编号像素直接经过 NVENC→RTMP→网关→浏览器，检查收到的画面内容。
            def read_marker(target):
                return target.evaluate("""()=>{const v=document.getElementById('video'),c=document.createElement('canvas');c.width=v.videoWidth;c.height=v.videoHeight;const x=c.getContext('2d');x.drawImage(v,0,0);let marker=0;for(let bit=0;bit<32;bit++){const p=x.getImageData(20+bit*8,24,1,1).data;if(p[0]+p[1]+p[2]>384)marker=(marker|(1<<bit))>>>0;}return marker;}""")

            marker = read_marker(page)
            sequence = marker & 65535
            assert marker >> 24 == 0xa5 and (marker >> 16) & 255 == ((sequence & 255) ^ (sequence >> 8) ^ 0x5a)
            report['validPixelFrameId'] = sequence

            late = context.new_page()
            late.goto('http://127.0.0.1:8090/')
            login(late)
            report['lateViewer'] = watch(late)
            assert report['lateViewer']['audioPackets'] >= 10
            busy = page.evaluate("""async()=>{const r=await fetch('/api/source',{method:'POST',headers:{Authorization:'Bearer '+token,'Content-Type':'application/json'},body:JSON.stringify({stream:'another_stream'})});return r.status;}""")
            assert busy == 409
            before = late.evaluate('window.gatewayStats()')['framesDecoded']
            page.locator('#stop').click()
            wait_frames(late, before + 20)
            report['remainingViewer'] = late.evaluate('window.gatewayStats()')
            start_probe(late) # 保持同一个音频消费者跨过上游停止/重推，不能重建消费者掩盖恢复问题。

            # 上游停止并重新推同名流：新 PTS 从零开始，原浏览器会话应在新 IDR 后恢复。
            old_epoch = late.evaluate("api('/api/status')")['epoch']
            old_sequence = read_marker(late) & 65535
            stop_file.touch(); assert publisher.wait(timeout=8) == 0
            time.sleep(1)
            before = late.evaluate('window.gatewayStats()')['framesDecoded']
            stop_file.unlink()
            publisher = subprocess.Popen(command, env=env, cwd=root, stdout=log, stderr=log,
                                         creationflags=subprocess.CREATE_NO_WINDOW)
            # SRS 可能保留 RTMP 订阅连接并修正新主播时间戳；不强求读取端必须重开。
            # 用视频中的编号归零证明播放的是新 Publisher 的真实画面。
            deadline = time.monotonic() + 15
            new_sequence = old_sequence
            while time.monotonic() < deadline:
                candidate = read_marker(late)
                new_sequence = candidate & 65535
                if candidate >> 24 == 0xa5 and 0 < new_sequence < old_sequence:
                    break
                late.wait_for_timeout(100)
            assert 0 < new_sequence < old_sequence
            wait_frames(late, before + 30, timeout=20)
            status = late.evaluate("api('/api/status')")
            report['upstreamRestartFrameIds'] = [old_sequence, new_sequence]
            report['originalEpoch'] = old_epoch
            report['reconnectedEpoch'] = status['epoch']
            report['afterUpstreamRestart'] = late.evaluate('window.gatewayStats()')
            audio_before = report['afterUpstreamRestart']['audioPackets']
            late.wait_for_timeout(700)
            assert late.evaluate('window.gatewayStats()')['audioPackets'] > audio_before + 15
            finish_probe(late, 'audioVideoAfterUpstreamRestart', 6200)
            late.locator('#stop').click()
            for _ in range(30):
                if not page.evaluate("api('/api/status')")['viewers']:
                    break
                page.wait_for_timeout(100)
            assert not page.evaluate("api('/api/status')")['viewers']
            report['restartedViewer'] = watch(page)
            page.locator('#stop').click()
            for _ in range(30):
                if not page.evaluate("api('/api/status')")['viewers']:
                    break
                page.wait_for_timeout(100)
            assert not page.evaluate("api('/api/status')")['viewers']
            report['finalStatus'] = page.evaluate("api('/api/status')")
            assert report['finalStatus']['queuedFrames'] <= 32 and report['finalStatus']['queuedBytes'] <= 8*1024*1024
            # 可选音频不能破坏原来的纯视频输入；换新流后浏览器应只协商一条视频轨。
            stop_file.touch(); assert publisher.wait(timeout=8) == 0
            stop_file.unlink()
            video_stream = stream + '_video'
            video_command = command.copy(); video_command[video_command.index('--stream')+1] = video_stream
            video_command.append('--video-only')
            publisher = subprocess.Popen(video_command, env=env, cwd=root, stdout=log, stderr=log,
                                         creationflags=subprocess.CREATE_NO_WINDOW)
            stream = video_stream
            report['videoOnlyFallback'] = watch(page)
            assert report['videoOnlyFallback']['audioSamples'] == 0
            assert page.evaluate("api('/api/status')")['videoOnly'] is True
            page.locator('#stop').click()
            # 小窗口只验排版：表单可折行，没有横向溢出。
            page.set_viewport_size({'width': 400, 'height': 850})
            assert page.evaluate('document.documentElement.scrollWidth<=window.innerWidth')
            page.screenshot(path=str(output / '自建网关窄屏布局.png'), full_page=True)
            browser.close()
        report['passed'] = True
    finally:
        stop_file.touch()
        try:
            publisher.wait(timeout=8)
        except subprocess.TimeoutExpired:
            publisher.terminate(); publisher.wait(timeout=5)
        log.close(); stop_file.unlink(missing_ok=True)
        (output / '媒体网关验收.json').write_text(json.dumps(report, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    assert publisher.returncode == 0
    print(json.dumps(report, ensure_ascii=False, indent=2))
    print('PASS custom AV gateway: real H264/AAC -> H264/Opus -> Edge; audio content and patterned AV sync; common SR clock; late join, source restart, stop/restart, narrow layout')
    return 0


if args.gateway:
    if args.beauty or args.latency:
        parser.error('--gateway is a separate functional check')
    try:
        raise SystemExit(gateway_check())
    except Exception as error:
        print('Gateway check failed:', type(error).__name__, '(sanitized details: out/媒体网关验证/媒体网关验收.json)')
        raise SystemExit(1)

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
