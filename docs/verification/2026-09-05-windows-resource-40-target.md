# Windows 40-target resource/churn gate, rc15

Purpose: hold the product-level Windows shared gateway path for 30 minutes
with 40 controlled room targets and check for process growth, memory runaway
and cleanup failures.

This is a fixture resource gate. It does not prove live Q2PRO-X gameplay.

## Inputs

```text
VM: Windows 11 25H2 + LTSC (x64) 28in1
VM UUID: 9a57e185-2b98-458b-b58f-c9e23e666ce9
portable: C:\Users\user\q2dz-release\rc15\portable
session: portable\q2dz-session.exe
gateway: portable\gateway\q2-doszone-webview2-gateway.exe
fake Q2: C:\Users\user\q2dz-release\rc15\coordinator-fixture-build\Release\q2dz-fake-q2.exe
host peer harness: tests/tools/controlled-room-set.py webrtc --count 40
hold duration after source40: 1800 seconds
sample interval: 30 seconds
launcher heartbeat interval: 15 seconds
```

Fresh ICE was passed through stdin and not recorded here.

## Result

Startup:

```text
controlled peers: 40/40 ready
HEARTBEAT_SENT 1
HEARTBEAT_SENT 2
HEARTBEAT_SENT 3
HEARTBEAT_SENT 4
SOURCE40_READY heartbeat_count=4
```

Resource hold:

```text
samples=60
heartbeat_count=122
processes per sample=10
initial total_mem_kib=369944
final total_mem_kib=382180
```

Max per-image working set reported by Windows `tasklist`:

```text
msedgewebview2.exe: 120228 KiB
q2-doszone-webview2-gateway.exe: 15240 KiB
q2dz-fake-q2.exe: 4344 KiB
q2dz-session.exe: 10048 KiB
```

Max per-image process counts:

```text
msedgewebview2.exe: 6
q2-doszone-webview2-gateway.exe: 1
q2dz-fake-q2.exe: 1
q2dz-session.exe: 2
```

Stop and cleanup:

```text
RESOURCE_RESULT samples=60 heartbeat_count=122 max_mem_kib={"msedgewebview2.exe": 120228, "q2-doszone-webview2-gateway.exe": 15240, "q2dz-fake-q2.exe": 4344, "q2dz-session.exe": 10048} max_counts={"msedgewebview2.exe": 6, "q2-doszone-webview2-gateway.exe": 1, "q2dz-fake-q2.exe": 1, "q2dz-session.exe": 2} exit=0
POST_STOP_PROCESSES 0
host controlled peer children_alive=0
guest NO_PRODUCT_OR_HARNESS_PROCESSES
guest NO_RESOURCE_TEMP_DIRS
```

## Verdict

`PASS` for the Windows 30-minute 40-target fixture resource/churn gate.

Proven:

- coordinator-level 40-target source remained active for 30 minutes;
- no extra gateway, fake Q2 or WebView2 process tree growth was observed;
- memory stayed bounded for this fixture workload;
- `q2dz-session.exe` exited with code `0`;
- no product or harness process remained after stop.

Not proven here:

- exact Q2PRO-X Custom source menu rendering;
- `status -> connect` source tuple;
- live map entry, movement/fire or room switching;
- real-player traffic memory profile.
