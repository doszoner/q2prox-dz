# Managed gateway protocol v1

This contract is normative for the Windows WebView2 gateway when it is owned by `q2dz-session`.

## Startup

The coordinator starts the gateway directly, without `cmd.exe`, with only non-sensitive arguments:

```text
q2dz-gateway.exe
  --managed-session
  --udp-port 0
  --owner-handle <inherited-read-handle>
  --user-data-dir <per-session-directory>
```

The gateway must bind only `127.0.0.1` and must let the OS choose the port. Alias, ICE/TURN, game credentials, SDP, candidates and player tokens are forbidden in arguments, environment variables and persistent files.

## Configuration input

Gateway stdin contains exactly one UTF-8 JSON object and then EOF. Maximum accepted size is 256 KiB. The minimum schema is:

```json
{
  "version": 1,
  "roomId": "portal room identifier",
  "alias": "q2.<identifier>",
  "ordered": false,
  "iceServers": [
    {"urls": ["stun:…"]},
    {"urls": ["turn:…"], "username": "…", "credential": "…"}
  ]
}
```

`alias` accepts either `q2.<identifier>` for a browser-hosted room or the exact
`room.q2ded.<identifier>` alias for a dedicated server. The gateway must apply
`iceServers` before transport initialization. It must keep the object in memory
only and overwrite buffers after use. Player/statistics tokens are not part of
this object.

## Owner lifetime

`--owner-handle` is the decimal value of an inherited pipe read handle. EOF means that the coordinator has stopped. The gateway must begin graceful shutdown immediately. Job Object `KILL_ON_JOB_CLOSE` remains the hard guarantee for coordinator crash or timeout.

## Events

Stdout is reserved for one line per event:

```text
Q2DZ/1 TRANSPORT_READY udp=127.0.0.1:<port> peer=<peer-id>
Q2DZ/1 HEARTBEAT state=ok
Q2DZ/1 ROOM_CLOSED reason=<percent-encoded-reason>
Q2DZ/1 FATAL stage=<stage> code=<stable-code>
```

Event keys and values must never contain ICE, TURN credentials, token, secret, username, SDP or candidates. Values use percent encoding for spaces and non-token characters.

`TRANSPORT_READY` is emitted exactly once and only after all of the following are true:

1. signaling is authenticated;
2. the room alias has resolved;
3. the DataChannel is open;
4. the gateway sent the normal Quake II OOB `ping` packet to the room and received OOB `ack`;
5. the loopback UDP socket is bound.

A DataChannel-open callback alone is not readiness.

## Room liveness

Room closure is detected from DataChannel/peer close or bounded explicit Quake II OOB probes. Gameplay silence is never a timeout condition. The reference probe sends `0xff 0xff 0xff 0xff "ping\n"`, consumes only an awaited OOB `ack`, and reports `ROOM_CLOSED reason=probe_timeout` after three bounded failures.

## Exit

- `0`: owner-requested clean shutdown;
- non-zero: transport or internal failure after a corresponding `FATAL` event;
- on all exits, close DataChannel, WebView2 controller/environment, UDP socket and all threads before returning.
