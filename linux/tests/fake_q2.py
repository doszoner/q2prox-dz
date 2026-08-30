#!/usr/bin/env python3
import http.client, os, signal, socket, sys, time
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
expected = ['+set', 'game', '3zb2', '+set', 'cl_serverbrowser_custom_source']
if len(args) < len(expected) + 2 or args[:len(expected)] != expected:
    sys.exit(64)
source = args[len(expected)]
if not source.startswith('http://127.0.0.1:') or not source.endswith('/servers'):
    sys.exit(64)
if args[len(expected) + 1:] != ['+serverbrowser_open_modern']:
    sys.exit(64)
host, source_port = source[len('http://'):].split(':', 1)
source_port = int(source_port.split('/', 1)[0])
if host != '127.0.0.1' or not 1024 <= source_port <= 65535:
    sys.exit(64)
if marker:
    open(marker + '.q2-source-url', 'w').write(source)
mode=os.environ.get('Q2DZ_FAKE_Q2_MODE','normal')
def read_source():
    connection = http.client.HTTPConnection(host, source_port, timeout=2)
    connection.request('GET', '/servers', headers={'Host': f'{host}:{source_port}', 'Connection': 'close'})
    response = connection.getresponse()
    body = response.read()
    connection.close()
    if response.status != 200:
        sys.exit(7)
    endpoints = [line.decode('ascii') for line in body.splitlines() if line]
    if len(endpoints) > 2 or any(not endpoint.startswith('127.0.0.1:') for endpoint in endpoints):
        sys.exit(8)
    return endpoints
endpoints = [] if mode == 'no-source-request' else read_source()
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
