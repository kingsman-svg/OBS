"""Render this project's small Mermaid UML subset to SVG and PNG.

Sources remain editable Mermaid; rendering uses Pillow and system fonts.
Supports one detailed class per file and the sequence constructs used here.
"""
from pathlib import Path
import math
import argparse
import re
import xml.etree.ElementTree as ET
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parents[1] / 'docs' / 'uml'
IMAGE_NAMES = {
    'ffmpeg-record': '本地编码录制时序图', 'ffmpeg-stop': '录制停止封存时序图',
    'media-sync': '音视频同步时序图',
    'gpu-face': 'GPU人脸检测时序图',
    'TrtLogger': '推理日志类图', 'EngineSession': '推理引擎会话类图',
    'ModelOptimizer': '模型优化器类图', 'model-optimize': '独立模型优化时序图',
    'engine-infer': '推理引擎执行时序图',
    'AudioResampler': '音频重采样类图', 'AudioMixer': '音频混音类图',
    'AudioMixTrack': '混音输入状态类图', 'audio-resample': '音频重采样时序图',
    'audio-mix': '双路音频混音时序图', 'audio-finish': '混音停止排空时序图',
    'VideoRenderer': '视频GPU渲染类图', 'gpu-preview': 'GPU预览渲染时序图',
    'PreviewWindow': '独立画面窗口类图', 'preview-window': '独立画面窗口时序图',
    'CaptureSource': '采集源数据类图', 'VideoFrame': '视频帧数据类图',
    'AudioPacket': '音频包数据类图', 'VideoCapture': '视频采集类图',
    'WasapiCapture': '音频采集类图', 'capture-devices': '采集设备枚举时序图',
    'capture-video': '视频采集预览时序图', 'capture-audio': '双路音频采集时序图',
    'capture-stop': '采集停止清理时序图',
    'SignalClient': '客户端信令类图', 'SessionModel': '工作台模型类图',
    'SessionController': '工作台控制器类图',
    'client-session': '客户端信令连接时序图', 'client-frames': '客户端信令收发时序图',
    'client-room': '双端房间操作时序图', 'client-mode': '播放模式切换时序图',
    'client-expiry': '客户端会话到期时序图',
    'HttpClient': '网络请求类图', 'LoginModel': '登录模型类图',
    'MainWindow': '主窗口类图', 'LoginController': '登录控制器类图',
    'LocalAuthServer': '本地认证服务类图',
    'http-request': '网络请求时序图', 'login': '登录时序图',
    'cancel-login': '取消登录时序图', 'logout': '退出登录时序图',
    'local-auth': '本地认证时序图',
    'docker-dev': '容器开发连接时序图',
    'UniqueFd': '文件描述符管理类图', 'Buffer': '字节缓冲类图',
    'Channel': '事件通道类图', 'Poller': '事件轮询类图',
    'EventLoop': '事件循环类图', 'Acceptor': '连接接收器类图',
    'TcpConnection': '连接管理类图', 'TcpServer': '服务入口类图',
    'tcp-accept': '连接接入时序图', 'tcp-io': '数据收发时序图',
    'tcp-close': '连接关闭时序图', 'reactor-task': '跨线程任务时序图',
    'TimerQueue': '定时器队列类图', 'HttpRequest': '请求数据类图',
    'HttpParser': '请求解析器类图', 'HttpServer': '公共请求服务类图',
    'HttpProbe': '健康探测类图', 'FrameCodec': '信令帧编解码类图',
    'AuthService': '认证签名类图', 'LoginServer': '登录服务器类图',
    'LoginNode': '登录节点类图', 'SchedulerServer': '负载调度服务器类图',
    'SignalSession': '信令会话类图', 'LiveRoom': '直播房间类图',
    'SignalServer': '信令服务器类图',
    'timer-schedule': '定时器调度时序图', 'timer-cancel': '定时器取消时序图',
    'http-server': '服务端请求处理时序图', 'login-service': '独立登录时序图',
    'login-schedule': '登录节点调度时序图', 'signal-auth': '信令认证时序图',
    'signal-room': '信令房间管理时序图', 'signal-live': '信令开播停播时序图',
    'signal-cleanup': '信令心跳清理时序图', 'server-stop': '服务器启停时序图',
}
FONT = Path('C:/Windows/Fonts/msyh.ttc')
NS = 'http://www.w3.org/2000/svg'
ET.register_namespace('', NS)

def canvas(width, height):
    image = Image.new('RGB', (width, height), '#ffffff')
    return {'svg': ET.Element(f'{{{NS}}}svg', width=str(width), height=str(height),
                             viewBox=f'0 0 {width} {height}'),
            'image': image, 'draw': ImageDraw.Draw(image)}

def rect(c, x, y, w, h, fill='#ffffff', stroke='#334155'):
    ET.SubElement(c['svg'], f'{{{NS}}}rect', x=str(x), y=str(y), width=str(w),
                  height=str(h), fill=fill, stroke=stroke, **{'stroke-width': '2'})
    c['draw'].rectangle((x, y, x+w, y+h), fill=fill, outline=stroke, width=2)

def text(c, x, y, value, size=17, color='#1e293b', centered=False):
    font = ImageFont.truetype(str(FONT), size)
    element = ET.SubElement(c['svg'], f'{{{NS}}}text', x=str(x), y=str(y+size), fill=color,
                            **{'font-family': 'Microsoft YaHei, sans-serif', 'font-size': str(size),
                               'text-anchor': 'middle' if centered else 'start'})
    element.text = value
    width = c['draw'].textlength(value, font=font)
    c['draw'].text((x-width/2 if centered else x, y), value, fill=color, font=font)

def line(c, x1, y1, x2, y2, dashed=False):
    attrs = {'stroke': '#64748b', 'stroke-width': '2'}
    if dashed: attrs['stroke-dasharray'] = '7 5'
    ET.SubElement(c['svg'], f'{{{NS}}}line', x1=str(x1), y1=str(y1), x2=str(x2), y2=str(y2), **attrs)
    if dashed:
        distance = math.hypot(x2-x1, y2-y1)
        for start in range(0, int(distance), 12):
            stop = min(start+7, distance)
            a, b = start/max(distance, 1), stop/max(distance, 1)
            c['draw'].line((x1+(x2-x1)*a, y1+(y2-y1)*a,
                            x1+(x2-x1)*b, y1+(y2-y1)*b), fill='#64748b', width=2)
    else: c['draw'].line((x1, y1, x2, y2), fill='#64748b', width=2)

def polygon(c, points, fill='#ffffff'):
    ET.SubElement(c['svg'], f'{{{NS}}}polygon', points=' '.join(f'{x},{y}' for x,y in points),
                  fill=fill, stroke='#64748b', **{'stroke-width':'2'})
    c['draw'].polygon(points, fill=fill, outline='#64748b')

def arrow(c, x1, y1, x2, y2, dashed=False, inheritance=False):
    line(c, x1, y1, x2, y2, dashed)
    angle = math.atan2(y2-y1, x2-x1)
    p = [(x2, y2)] + [(x2-13*math.cos(angle)+s*7*math.sin(angle),
                       y2-13*math.sin(angle)-s*7*math.cos(angle)) for s in (-1,1)]
    if inheritance: polygon(c, p)
    else:
        line(c, *p[0], *p[1]); line(c, *p[0], *p[2])

def save(c, path):
    path = path.with_name(IMAGE_NAMES[path.stem])
    # 写入字节以保持 LF，避免 Windows 文本包装器把 XML 换行改成 CRLF。
    path.with_suffix('.svg').write_bytes(ET.tostring(c['svg'], encoding='utf-8', xml_declaration=True))
    c['image'].save(path.with_suffix('.png'))

def render_class(path):
    source = path.read_text(encoding='utf-8')
    name = path.stem
    match = re.search(r'class '+re.escape(name)+r'\s*\{(.*?)\}', source, re.S)
    members = [s.strip() for s in match.group(1).splitlines() if s.strip()]
    relations = re.findall(r'^(\w+)\s+(<\|--|\*--|-->|\.\.>)\s+(\w+)(?:\s*:\s*(.*))?$', source, re.M)
    peers = list(dict.fromkeys(b if a == name else a for a, _, b, _ in relations))
    aliases = dict(re.findall(r'class (\w+)\["([^"]+)"\]', source))
    height = max(220 + len(members)*30, 170 + len(peers)*150)
    c = canvas(1220, height)
    text(c, 40, 22, f'{name} · UML 类图', 26)
    rect(c, 40, 85, 760, height-130)
    rect(c, 40, 85, 760, 54, '#e0f2fe')
    text(c, 420, 99, name, 23, centered=True)
    y = 154
    operations = False
    for member in members:
        if '(' in member and not operations:
            line(c, 40, y-6, 800, y-6); operations = True
        text(c, 58, y, member.replace('$', ' [static]'), 17)
        y += 30
    for i, peer in enumerate(peers):
        py = 110 + i*150
        rect(c, 950, py, 235, 54, '#f1f5f9')
        text(c, 1067, py+12, aliases.get(peer, peer), 17, centered=True)
    for a, symbol, b, label in relations:
        peer = b if a == name else a
        py = 137 + peers.index(peer)*150
        cy = min(180 + peers.index(peer)*95, height-65)
        if symbol == '<|--': arrow(c, 800, cy, 950, py, inheritance=True)
        else:
            arrow(c, 800, cy, 950, py, dashed=symbol == '..>')
            if symbol == '*--': polygon(c, [(800,cy),(808,cy-5),(816,cy),(808,cy+5)], '#64748b')
        if label: text(c, 818, min(cy, py)-25, label, 12)
    save(c, path)

def render_sequence(path):
    source = path.read_text(encoding='utf-8').splitlines()
    participants = []
    events = []
    for raw in source:
        row = raw.strip()
        p = re.match(r'(?:participant|actor) (\w+) as (.*)', row)
        if p: participants.append(p.groups())
        elif row and row != 'sequenceDiagram': events.append(row)
    width = max(1100, len(participants)*260)
    height = 210 + len(events)*57
    c = canvas(width, height)
    text(c, 35, 20, IMAGE_NAMES[path.stem], 26)
    xs = {}
    for i, (key, label) in enumerate(participants):
        x = (i+0.5)*width/len(participants); xs[key] = x
        rect(c, x-112, 80, 224, 55, '#e0f2fe')
        text(c, x, 96, label, 17, centered=True)
        line(c, x, 135, x, height-35, True)
    y = 175
    groups = []
    for row in events:
        message = re.match(r'(\w+)(-->>|->>)(\w+):\s*(.*)', row)
        note = re.match(r'Note over (\w+)(?:,(\w+))?:\s*(.*)', row, re.I)
        if message:
            a, style, b, label = message.groups()
            x1, x2 = xs[a], xs[b]
            if a == b:
                line(c, x1, y, x1+90, y); line(c, x1+90, y, x1+90, y+18)
                arrow(c, x1+90, y+18, x1, y+18, style == '-->>')
                label_width = c['draw'].textlength(label, font=ImageFont.truetype(str(FONT), 14))
                label_x = x1+8 if x1+8+label_width < width-15 else x1-label_width-12
                text(c, label_x, y-25, label, 14)
            else:
                arrow(c, x1, y, x2, y, style == '-->>')
                text(c, (x1+x2)/2, y-25, label, 14, centered=True)
        elif row.startswith('alt '):
            left, top = 8 + len(groups)*12, y-28
            groups.append((left, top))
            text(c, left+6, y-23, '[alt] '+row[4:], 14)
            line(c, left, top, width-left, top)
        elif row.startswith('else '):
            left, _ = groups[-1]
            line(c, left, y-28, width-left, y-28, True)
            text(c, left+6, y-23, '[else] '+row[5:], 14)
        elif row == 'end' and groups:
            left, top = groups.pop()
            line(c, left, top, left, y-22); line(c, width-left, top, width-left, y-22)
            line(c, left, y-22, width-left, y-22)
        elif note:
            a, b, label = note.groups()
            note_width = max(210, c['draw'].textlength(label, font=ImageFont.truetype(str(FONT), 13))+30)
            note_width = min(width-40, note_width)
            middle = (xs[a]+xs[b or a])/2
            left = max(20, min(middle-note_width/2, width-note_width-20))
            right = left+note_width
            rect(c, left, y-27, right-left, 40, '#fef9c3')
            text(c, (left+right)/2, y-18, label, 13, centered=True)
        else: raise ValueError(f'Unsupported Mermaid line: {row}')
        y += 57
    save(c, path)

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--classes', action='store_true', help='Also regenerate historical class diagrams (default: sequences only).')
args = parser.parse_args()
sources = sorted(ROOT.glob('*.mmd'))
class_sources = [p for p in sources if p.read_text(encoding='utf-8').startswith('classDiagram')]
sequence_sources = [p for p in sources if p.read_text(encoding='utf-8').startswith('sequenceDiagram')]
if args.classes:
    for source in class_sources: render_class(source)
for source in sequence_sources: render_sequence(source)
print(f'Rendered {len(class_sources) if args.classes else 0} class diagrams and {len(sequence_sources)} sequence diagrams (SVG + PNG).')
