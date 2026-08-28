#!/usr/bin/env python3
import argparse, os, re, signal, socket, sys, time
if os.environ.get('Q2DZ_SYNTHETIC_CREDENTIAL'):
    raise SystemExit('credential-shaped parent environment reached fake gateway')
p=argparse.ArgumentParser()
p.add_argument('--bind', required=True)
p.add_argument('--signal', required=True)
p.add_argument('--alias', required=True)
p.add_argument('--ice-file', required=True)
p.add_argument('--max-datagram', required=True, type=int)
a=p.parse_args()
if a.signal != 'wss://net.js-dos.com:444' or a.max_datagram != 1400:
    print('bad fixed transport contract', file=sys.stderr); sys.exit(4)
# The test validates that the inherited anonymous file is readable and contains no disk path.
with open(a.ice_file, 'rb', buffering=0) as f:
    data=f.read()
if b'iceServers' in data or not data.startswith(b'['):
    print('bad ICE payload', file=sys.stderr); sys.exit(4)
alias_key = re.sub(r'[^A-Za-z0-9]', '_', a.alias).upper()
mode_spec = os.environ.get('Q2DZ_FAKE_LEGACY_MODE', 'normal')
mode = 'normal'
for entry in mode_spec.split(';'):
    name, separator, candidate = entry.partition('=')
    if separator and name == a.alias:
        mode = candidate
        break
if not separator:
    mode = mode_spec
marker=os.environ.get('Q2DZ_TEST_MARKER')
attempts_path=marker+'.legacy-attempts' if marker else None
endpoints_path=marker+'.legacy-endpoints' if marker else None
pid_path=marker+'.legacy-'+alias_key+'.pid' if marker else None
attempt=0
if attempts_path:
    try:
        with open(attempts_path, 'r', encoding='ascii') as f:
            attempt=int(f.read() or '0')
    except FileNotFoundError:
        pass
    attempt += 1
    with open(attempts_path, 'w', encoding='ascii') as f:
        f.write(str(attempt))
host,port=a.bind.rsplit(':',1)
if endpoints_path:
    with open(endpoints_path, 'a', encoding='ascii') as f:
        f.write(a.bind+'\n')
if pid_path:
    with open(pid_path, 'w', encoding='ascii') as f:
        f.write(str(os.getpid()))
if mode == 'early-exit-once' and attempt == 1:
    print('early startup exit', file=sys.stderr)
    sys.exit(11)
if mode == 'early-exit-always':
    print('early startup exit', file=sys.stderr)
    sys.exit(12)
s=socket.socket(socket.AF_INET,socket.SOCK_DGRAM)
s.bind((host,int(port)))
s.settimeout(0.2)
if mode == 'exit': sys.exit(9)
if mode == 'no-ack-ignore-term':
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
while True:
    try: data,addr=s.recvfrom(65535)
    except socket.timeout: continue
    if data == b'\xff\xff\xff\xffping\n':
        if mode not in ('no-ack', 'no-ack-ignore-term'): s.sendto(b'\xff\xff\xff\xffack\n',addr)
        if mode == 'exit-after-ack': time.sleep(1.0); sys.exit(8)
        if mode == 'room-close-after-ack': time.sleep(1.0); sys.exit(20)
    elif mode == 'datagram-boundary' and data == b'remote-oversize':
        s.sendto(b'R'*1401,addr)
    else: s.sendto(data,addr)
