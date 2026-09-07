#!/usr/bin/env python3
import http.client, json, os, signal, socket, sys, time
if os.environ.get('Q2DZ_SYNTHETIC_CREDENTIAL'):
    raise SystemExit('credential-shaped parent environment reached fake Q2')
marker=os.environ.get('Q2DZ_TEST_MARKER')
if marker:
    open(marker+'.started','w').write(str(os.getpid()))
    open(marker+'.q2-pid','w').write(str(os.getpid()))
    starts_path=marker+'.q2-starts'
    try:
        with open(starts_path, 'r', encoding='ascii') as f:
            starts=int(f.read() or '0')
    except FileNotFoundError:
        starts=0
    with open(starts_path, 'w', encoding='ascii') as f:
        f.write(str(starts+1))
args=sys.argv[1:]
if os.read(0, 1) != b'':
    sys.exit(64)
hub = 'cl_serverbrowser_custom_source' in args
if not hub:
    sys.exit(64)
expected = [
    '+set', 'game', '3zb2',
    '+set', 'cl_timeout', '20',
    '+set', 'cl_serverbrowser_custom_source',
]
if len(args) != len(expected) + 1 or args[:len(expected)] != expected:
    sys.exit(64)
source = args[len(expected)]
if not source.startswith('http://127.0.0.1:') or not source.endswith('/servers'):
    sys.exit(64)
host, source_port = source[len('http://'):].split(':', 1)
source_port = int(source_port.split('/', 1)[0])
if host != '127.0.0.1' or not 1024 <= source_port <= 65535:
    sys.exit(64)
if marker:
    open(marker + '.q2-source-url', 'w').write(source)
mode=os.environ.get('Q2DZ_FAKE_Q2_MODE','normal')

def read_raw(path, host_header):
    connection = socket.create_connection((host, source_port), timeout=2)
    request = f'GET {path} HTTP/1.1\r\nHost: {host_header}\r\nConnection: close\r\n\r\n'.encode()
    connection.sendall(request)
    chunks = []
    while True:
        data = connection.recv(4096)
        if not data:
            break
        chunks.append(data)
    connection.close()
    return b''.join(chunks)

def read_source():
    raw = read_raw('/servers', host)
    separator = raw.find(b'\r\n\r\n')
    if separator < 0 or not raw.startswith(b'HTTP/1.1 200 OK\r\n'):
        sys.exit(7)
    body = raw[separator + 4:]
    expected = (b'HTTP/1.1 200 OK\r\nContent-Type: text/plain; charset=utf-8\r\n'
                b'Cache-Control: no-store\r\nContent-Length: ' + str(len(body)).encode() +
                b'\r\nConnection: close\r\n\r\n' + body)
    if raw != expected:
        sys.exit(7)
    endpoints = [line.decode('ascii') for line in body.splitlines() if line]
    if len(endpoints) > 256 or any(not endpoint.startswith('127.0.0.1:') for endpoint in endpoints):
        sys.exit(8)
    return endpoints

def read_rooms():
    connection = http.client.HTTPConnection(host, source_port, timeout=2)
    connection.request('GET', '/q2dz/rooms', headers={'Host': f'{host}:{source_port}', 'Connection': 'close'})
    response = connection.getresponse()
    body = response.read()
    connection.close()
    if response.status != 200:
        if marker:
            open(marker + '.q2-rooms-failed', 'w').write(str(response.status))
        sys.exit(11)
    try:
        payload = json.loads(body)
    except json.JSONDecodeError:
        if marker:
            open(marker + '.q2-rooms-failed', 'w').write('invalid-json')
        sys.exit(12)
    if payload.get('version') != 1 or not isinstance(payload.get('session'), str) or payload.get('targets') != []:
        if marker:
            open(marker + '.q2-rooms-failed', 'w').write('invalid-empty-snapshot')
        sys.exit(13)
    if marker:
        open(marker + '.q2-rooms-empty', 'w').write(json.dumps(payload, sort_keys=True))
    return payload
endpoints = [] if mode == 'no-source-request' else read_source()
if mode != 'no-source-request':
    read_rooms()
if marker:
    open(marker + '.q2-source-empty', 'w').write('1' if not endpoints else '0')
ports = []
for endpoint in endpoints:
    try:
        ports.append(int(endpoint.rsplit(':', 1)[1]))
    except (ValueError, IndexError):
        sys.exit(9)
if any(not 1024 <= port <= 65535 for port in ports):
    sys.exit(10)
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
s.bind(('127.0.0.1',0)); s.settimeout(2)
for port in ports:
    s.sendto(b'\xff\xff\xff\xffgetchallenge\n', ('127.0.0.1', port))
    try: data,_=s.recvfrom(65535)
    except socket.timeout:
        if os.environ.get('Q2DZ_FAKE_Q2_MODE') != 'sleep-no-probe':
            sys.exit(5)
        continue
    if data != b'\xff\xff\xff\xffgetchallenge\n': sys.exit(6)
def stop(sig,frame):
    if marker: open(marker+'.stopped','w').write(str(sig))
    sys.exit(0)
signal.signal(signal.SIGTERM,stop); signal.signal(signal.SIGINT,stop)
if mode == 'normal-delay':
    time.sleep(0.5)
if mode in ('sleep', 'sleep-no-probe', 'no-source-request'):
    while True: time.sleep(1)
if marker: open(marker+'.finished','w').write('ok')
