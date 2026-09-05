# Linux managed-session architecture

## Decision

Use one launcher-owned local room hub. Do not expose every Dos.Zone room as a
synthetic UDP server and do not make Wine/Q2PRO-X responsible for WebRTC.

The coordinator owns one Wine/Q2PRO-X process and one shared WebRTC-NET
gateway. Each room target gets its own random loopback UDP endpoint inside that
gateway. The ordinary Q2PRO-X shortcut remains independent and failure cleanup
stays deterministic.

## State machine

```text
  Idle
  → ValidatingSnapshot
  → ValidatingContent
  → StartingGateways
  → WaitingForGatewaysReady
  → StartingWine
  → Active
  → Closing
  → Closed

any nonterminal state → Failed → Closing → Closed
```

`TRANSPORT_READY` means a genuine Quake II OOB `ping` traversed the
corresponding WebRTC-NET route and an OOB `ack` returned. Signaling success,
ICE connected, or DataChannel open alone is insufficient.

## Ownership

The coordinator and its children use a unique non-secret session marker. Each
managed gateway and Wine/Q2PRO-X receive an independent process group. A
separate guardian receives every group id over a pipe. If the owner disappears,
the guardian terminates all groups, searches only same-UID processes carrying
the exact marker, and removes only a marked session directory under the
configured runtime root.

The guardian does not terminate a shared `wineserver`, because that could break
unrelated Wine applications. The live matrix must nevertheless prove that no
Q2PRO-X, gateway, WebView/Chromium helper owned by this session, UDP socket, or
session directory remains.

## Secret flow

```text
fresh room JSON
  → coordinator stdin
  → private pipe
  → managed gateway memory
  → parsed ICE array
  → sealed anonymous memfd inherited only by the corresponding legacy gateway
```

No room JSON, ICE/TURN credential, SDP, candidate, token or secret is accepted
as a command-line argument, environment variable, persistent config item or
control-protocol field.

The memfd compatibility boundary is deliberate: it preserves the tested
WebRTC-NET gateway while removing disk persistence. It is not a general file
interface and is closed with the child process. A future direct library
integration may replace only this adapter without changing coordinator,
lifecycle or user workflow.

## UDP rules

- bind only to `127.0.0.1`;
- choose a random high port for every session;
- keep complete datagrams; never stream, coalesce or split them;
- reject oversized datagrams rather than truncate;
- lock to the first loopback Q2 source after supervised Wine launch;
- reject a second source;
- preserve WebRTC-NET logical channel 0;
- do not parse gameplay packets.

## Live catalog and content bound

The launcher starts the coordinator and Q2PRO-X even when the initial catalog
has no compatible room. It polls the catalog approximately every 15 seconds.
After each successful poll it sends one full heartbeat to the shared gateway:
fresh ICE data, `PUT` commands for compatible rooms and `DROP` commands for
rooms that disappeared. Unchanged rooms keep their original configuration and
port. A new room is published after content and transport readiness checks. A
room absent from two consecutive successful polls is cleared; a catalog error
leaves the existing targets unchanged. A ready full room is retained, and
compatible new rooms are added up to the explicit `256` target limit.

The existing launcher keeps content checks bounded. Missing maps are skipped,
but an empty candidate set does not prevent
Q2PRO-X from starting with an empty `Custom` source.

The coordinator starts one stable loopback HTTP `Custom` source, one shared
gateway and one Q2PRO-X process. It accepts newline-delimited room updates while
both remain alive: later commands add, clear or replace individual targets
without replacing Q2PRO-X. The ordinary Q2PRO-X `Custom` `Refresh` requests the
current endpoint list. There is no direct-connect or alternate single-room
mode.

## Room closure

DataChannel close is decisive. Gameplay silence is not. During remote silence,
the gateway sends bounded genuine OOB probes. Only repeated probe failure
produces `ROOM_CLOSED`; there is no arbitrary gameplay idle timeout.

Legacy `q2` rooms keep the original tombstone policy. A `room.q2ded.*` binding
may start its gateway twice more after a failure or `ROOM_CLOSED`; successful
readiness does not reset that budget. After the second recovery start fails,
the binding remains tombstoned until it is cleared, replaced, or the
coordinator is restarted. Closing one room never closes Q2PRO-X or siblings.
