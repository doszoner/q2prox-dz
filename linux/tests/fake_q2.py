#!/usr/bin/env python3
import os, signal, socket, sys, time
if os.environ.get('Q2DZ_SYNTHETIC_CREDENTIAL'):
    raise SystemExit('credential-shaped parent environment reached fake Q2')
marker=os.environ.get('Q2DZ_TEST_MARKER')
if marker:
    open(marker+'.started','w').write(str(os.getpid()))
    starts_path=marker+'.q2-starts'
    try:
        with open(starts_path, 'r', encoding='ascii') as f:
            starts=int(f.read() or '0')
    except FileNotFoundError:
        starts=0
    with open(starts_path, 'w', encoding='ascii') as f:
        f.write(str(starts+1))
args=sys.argv[1:]
if len(args) < 5 or args[-5:-1] != ['+set', 'game', '3zb2', '+connect'] or '+connect' in args[:-5]:
    sys.exit(64)
try:
    host,port=args[-1].rsplit(':',1)
    port=int(port)
except (ValueError, IndexError):
    sys.exit(64)
if host != '127.0.0.1' or not 1024 <= port <= 65535:
    sys.exit(64)
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
s.bind(('127.0.0.1',0)); s.settimeout(2)
s.sendto(b'\xff\xff\xff\xffgetchallenge\n',(host,port))
try: data,_=s.recvfrom(65535)
except socket.timeout: sys.exit(5)
if data != b'\xff\xff\xff\xffgetchallenge\n': sys.exit(6)
mode=os.environ.get('Q2DZ_FAKE_Q2_MODE','normal')
def stop(sig,frame):
    if marker: open(marker+'.stopped','w').write(str(sig))
    sys.exit(0)
signal.signal(signal.SIGTERM,stop); signal.signal(signal.SIGINT,stop)
if mode == 'sleep':
    while True: time.sleep(1)
if marker: open(marker+'.finished','w').write('ok')
