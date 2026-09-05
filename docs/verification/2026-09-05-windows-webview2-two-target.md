# Windows WebView2 two-target gate, rc15

Purpose: verify that the packaged rc15 Windows WebView2 gateway can run one
shared transport process and bring two independent room targets to `READY`.

This is a transport/runtime gate. It is not a Q2PRO-X menu or gameplay gate.

## Inputs

```text
VM: Windows 11 25H2 + LTSC (x64) 28in1
VM UUID: 9a57e185-2b98-458b-b58f-c9e23e666ce9
snapshot before run: q2prox-rc15-staged-20260905
portable: C:\Users\user\q2dz-release\rc15\portable
gateway: portable\gateway\q2-doszone-webview2-gateway.exe
host peer harness: tests/tools/controlled-room-set.py webrtc --count 2
aliases: room.q2ded.testserver01, room.q2ded.testserver02
```

The host peer harness received fresh ICE through stdin. Raw ICE/TURN values
were not written to this document.

## Result

```text
Q2DZ/HUB/2 HEARTBEAT state=ok targets=2 ready=0
Q2DZ/HUB/2 READY id=1 udp=127.0.0.1:61622 peer=567490928
Q2DZ/HUB/2 READY id=2 udp=127.0.0.1:61623 peer=405118896
GATE_STATUS PASS ready=2 heartbeats=1 exit=0
WEBVIEW2_RUNTIME version=152.0.4191.62 controller_visible=true diagnostic_background_timer_flag_exact=false
Q2DZ_CADENCE timerTicks=1 timerMaxGapMs=0 waitCalls=1 aliasPollIterations=1 probeSendCount=1 probeAckCount=0 dcToNativeDatagrams=0 dcToNativeBytes=0 dcToNativeMaxGapMs=0 nativeToDcDatagrams=0 nativeToDcBytes=0 nativeToDcMaxGapMs=0 udpFromQ2Datagrams=0 udpFromQ2Bytes=0 udpToQ2Datagrams=0 udpToQ2Bytes=0 postToWebDatagrams=0 webToNativeDatagrams=0 udpToQ2MaxGapMs=0 postToWebMaxGapMs=0
GATEWAY_STOP udp_to_web=0 web_to_udp=0 targets=2 max_udp_to_transport_datagram=0 max_transport_to_udp_datagram=0 oversized_udp_drops=0 oversized_transport_drops=0 unknown_peer_drops=0 second_source_drops=0 transport_not_ready_drops=0 invalid_cadence_drops=0
```

Host peer cleanup:

```text
children_alive=0
cleanup=complete
```

Guest process cleanup:

```text
NO_PRODUCT_PROCESSES
```

## Verdict

`PASS` for the Windows packaged WebView2 two-target transport/runtime gate.

Proven:

- one packaged `q2-doszone-webview2-gateway.exe`;
- two independent HUB/2 targets;
- both targets reached `READY`;
- gateway accepted `STOP` and exited with code `0`;
- WebView2 runtime was present;
- no gateway, WebView2, coordinator or Q2PRO-X process remained afterward.

Not proven here:

- Q2PRO-X Custom source menu behavior;
- `status -> connect` source tuple;
- map entry, movement/fire or room switching;
- 30-minute Windows WebView2 resource/churn behavior.
