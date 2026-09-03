# Q2DZ/1 managed gateway protocol

The gateway reads exactly one bounded room JSON document from stdin. Stdout is
reserved for line-oriented control events. Diagnostics go to stderr and must be
redacted.

## Events

```text
Q2DZ/1 TRANSPORT_READY udp=127.0.0.1:<high-port> peer=<numeric-peer-id>
Q2DZ/1 HEARTBEAT state=ok
Q2DZ/1 ROOM_CLOSED reason=<stable-reason>
Q2DZ/1 FATAL stage=<stable-stage> code=<stable-code>
```

Unknown event types, duplicate fields, malformed values, non-loopback
endpoints, privileged ports and sensitive-looking field names are protocol
errors. Sensitive values are never permitted in stdout.

## Readiness rule

The gateway may emit `TRANSPORT_READY` only after all of the following:

1. fresh ICE was applied before P2P startup;
2. alias resolution selected the requested `q2.<id>` or `room.q2ded.<id>` peer;
3. WebRTC DataChannel is open;
4. a Quake II OOB `ping\n` datagram sent over logical channel 0 returned an
   OOB `ack` from the browser listen-server;
5. the public front UDP socket is bound to the advertised loopback endpoint.

## Owner failure

EOF on the owner pipe is an unconditional shutdown request. Normal shutdown and
owner loss must release the DataChannel, legacy transport child, front UDP
socket, anonymous ICE memfd and session-owned runtime state.
