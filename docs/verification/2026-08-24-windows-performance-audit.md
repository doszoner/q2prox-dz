# Windows performance and latency audit

Date: 2026-08-24

Candidate: `q2prox-dz 1.0.0-rc8`

Source commit: `690393c8555decd480a41b32e939d8680ed5b2ec`

Verdict: **GO for the closed alpha; no additional transport optimization is justified by the measurements.**

## Question and conclusion

This audit looked specifically for Windows-side causes of high ping, one-second
world updates, stalls, packet loss, CPU contention, memory pressure, leaks and
avoidable transport overhead.

The previously observed one-second cadence had one confirmed cause: the
WebView2 controller was marked invisible while the WebRTC receive pump ran from
a JavaScript timer. `1.0.0-rc8` keeps the 1x1 controller in WebView2 foreground
scheduling state with `put_IsVisible(TRUE)` and ships no Chromium diagnostic
flags. The exact rc8 session did not reproduce the one-second transport stall.

No second gameplay bottleneck was found. During the measured session:

- the selected ICE pair stayed connected over UDP;
- selected ICE-pair RTT was stable;
- no WebRTC send discard or gateway datagram drop was recorded;
- packet counts matched at every complete transport boundary;
- the gateway used little CPU compared with Q2PRO-X;
- memory, handles and threads showed no failure pattern in the bounded window;
- room-close cleanup left zero managed processes.

The correct production action is therefore **no transport rewrite and no
speculative optimization**. The remaining candidates below have explicit
measurement triggers.

## Exact scope

The measured user artifact was a fresh extraction of:

```text
q2prox-dz-1.0.0-rc8-windows-x64-portable.zip
SHA-256 0f1ce46b47edc3e646149f9d6c8a9ebcd45ec0b0fcba1cf1964e62051196ad83
```

The release gateway and Q2PRO-X were exact packaged/user binaries. A diagnostic
build of the coordinator was used only to retain the gateway's already-bounded
stderr counters; it did not replace the gateway or alter the packet path.
WebView2 remote debugging was enabled only for a redacted `getStats()` sampler.

Guest environment:

| Item | Value |
|---|---|
| OS | Windows 11 Pro, build 26100 |
| VM | VirtualBox, 4 logical processors, 8 GiB RAM |
| Graphics | VirtualBox WDDM 7.2.14; software-constrained VM rendering |
| WebView2 Runtime | 151.0.4129.101 |
| Power plan | Balanced |
| Q2PRO-X | 1.4 by ly, 2026-07-05 |
| Room | Real Dos.Zone `q2dm1`, `game=3zb2` |

Room identifiers, ICE servers, candidates, IP addresses, credentials, SDP,
tokens and packet payloads were not retained. RTC evidence contains only state,
candidate types, protocol, RTT, aggregate bytes/messages and discard counters.

## Method

The audit followed the data path rather than treating render FPS or ICMP as a
proxy for gameplay latency:

```text
Q2PRO-X UDP
  -> native gateway / WebView2 IPC
  -> JavaScript + WebRTC-NET
  -> WebRTC DataChannel / selected ICE pair
  -> Dos.Zone browser server
```

The following evidence was collected in one real session:

1. 540 one-second gateway cadence snapshots over about nine minutes;
2. 60 sanitized `RTCPeerConnection.getStats()` samples over 119 seconds;
3. 120 seconds of system sampling, including 89.7–94.8 seconds of PID-scoped
   session-process data and a 71-sample common steady process window;
4. one Windows Performance Recorder CPU trace, with zero lost events;
5. the final gateway size/drop summary and coordinator cleanup result.

The resource sample came from the same diagnostic session, but did not overlap
the retained `getStats()` window. It is useful for process attribution, not a
strict production baseline. The WPR trace crossed the session shutdown
boundary; without WPA Exporter it is retained as raw evidence and its
`tracerpt` integrity summary, not used to invent stack-level attribution.

## Gameplay-path network measurements

The earlier 12–13 ms ICMP result measured control-plane endpoints. It did not
measure the encrypted peer path. This run measured the selected ICE pair
directly:

| Metric | Result |
|---|---:|
| Samples / duration | 60 / 119.1 s |
| Candidate path | local `srflx` -> remote `relay`, UDP |
| ICE / connection state | `connected` throughout |
| Candidate-pair state | `succeeded` throughout |
| RTT minimum / median / p95 / maximum | 58 / 60 / 62 / 81 ms |
| `packetsDiscardedOnSend` | 0 throughout |
| DataChannel state | `open` throughout |
| DataChannel messages sent | 47.4/s |
| DataChannel messages received | 10.0/s |
| DataChannel payload sent | 1.06 KiB/s |
| DataChannel payload received | 0.23 KiB/s |

The remote peer selected a relay candidate. The measured RTT belongs to the
selected WebRTC/ICE route and excludes the local UDP loopback bridge; it does
not isolate TURN, Internet and room-owner contributions. Different room owners
and networks can legitimately select a different route and RTT.

`candidate-pair.currentRoundTripTime` is estimated from ICE/STUN connectivity
checks. It is not a DataChannel application RTT and not an end-to-end game
command/response measurement. Here it is used to detect selected-route stalls
and changes, together with continuous DataChannel and gateway counters.

WebRTC DataChannel traffic is not RTP, so RTP jitter/loss fields would be the
wrong metric. Candidate-pair RTT, DataChannel progress, discard counters and
the gateway boundary counts are the relevant evidence here.

## Gateway cadence and packet accounting

Of 540 one-second snapshots, 520 contained traffic in both directions. Four
active outliers with application inter-arrival gaps above 250 ms were separated
from the 516 steady snapshots: one startup outlier, two isolated mid-session
gaps and the final room-close transition. None coincided with a repeated timer
stall.

| Metric | Steady median | p95 | Maximum |
|---|---:|---:|---:|
| WebView2 timer ticks per second | 200 | 200 | 200 |
| Timer maximum gap | 8 ms | 19 ms | 113 ms |
| WebRTC -> Q2 datagrams | 10/s | 10/s | 15/s |
| WebRTC -> Q2 inter-arrival gap | 106 ms | 118 ms | 206 ms |
| Q2 -> WebRTC datagrams | 47/s | 51/s | 55/s |
| Q2 -> WebRTC inter-arrival gap | 32 ms | 52 ms | 131 ms |

The single 113 ms timer maximum was not a recurring one-second cadence. Most
snapshots ran the requested 5 ms poll exactly 200 times per second. The roughly
10 Hz inbound update rate came from the remote game stream; the local timer ran
far more frequently and introduced no one-second batching.

Complete-session counts matched exactly:

```text
Q2 UDP received = native-to-DataChannel = posted-to-WebView2 = 24190 datagrams
WebRTC received  = WebView2-to-native   = UDP sent-to-Q2      =  5191 datagrams
```

Thirty-two individual snapshots differed by one outbound datagram because the
native and JavaScript counters crossed the one-second snapshot boundary at
slightly different instants. The differences were symmetric (`16 x -1`,
`16 x +1`) and the complete totals were identical.

Final bounds:

```text
max Q2 -> transport datagram: 126 bytes
max transport -> Q2 datagram: 1330 bytes
oversized UDP drops: 0
oversized transport drops: 0
invalid cadence drops: 0
```

The 1330-byte observation is below the 1400-byte product limit but leaves only
limited headroom. It is not evidence of a current performance failure. The
documented HumbleNet caller-buffer truncation blind spot still applies; the
limit must not be raised or redesigned without a separate protocol finding.

## CPU and scheduling

PID-scoped CPU is shown as percent of one logical processor. The guest had four
logical processors, so 100% in this column is approximately 25% of total guest
capacity.

| Process group | CPU, one logical processor | Approx. guest total |
|---|---:|---:|
| Q2PRO-X | 87.1% | 21.8% |
| all session WebView2 processes | 10.0% | 2.5% |
| native gateway | 1.8% | 0.4% |
| coordinator | below sampler resolution | below sampler resolution |

System-wide CPU median was 37.1%, p95 68.6% and maximum 80.7%. Processor queue
length median was 0, p95 2 and maximum 5. Available guest memory never fell
below 4272 MiB (4.17 GiB), and page reads were normally zero.

Q2PRO-X, not the gateway, dominated CPU. This VM used a software-constrained
renderer, so visual slowness inside the VM is not a valid reason to alter the
network bridge. Even under that load, WebRTC RTT, DataChannel progress and the
gateway timer remained stable.

The WPR trace contains 402276 events over 50 seconds with zero lost events.
Because the trace includes teardown and this VM lacks WPA Exporter, the
PID-scoped sampler above is the authoritative attribution for this audit. The
raw ETL is kept outside the public repository for optional WPA inspection.

## Memory, handles and cleanup

Stable-window medians compare the first and last ten samples inside a
71-sample common process window:

| Scope | Working set, first -> last | Private memory, first -> last | Threads | Handles |
|---|---:|---:|---:|---:|
| Add-on excluding Q2 | 347.1 -> 352.2 MiB | 139.2 -> 140.7 MiB | 137 -> 141 | 3219 -> 3200 |
| WebView2 portion | 321.1 -> 326.0 MiB | 135.1 -> 136.6 MiB | 130 -> 132 | 2831 -> 2812 |
| Native gateway | 16.0 -> 16.1 MiB | 2.57 -> 2.59 MiB | 5 -> 6 | 214 -> 214 |
| Q2PRO-X, separate | 378.9 -> 378.6 MiB | 501.0 -> 499.9 MiB | 23 -> 23 | 323 -> 323 |

The small WebView2 warm-up growth over roughly one minute is not enough to call
a leak. There was no monotonic handle growth, no paging pressure and no orphan
process. The session ended on a bounded room-close path:

```text
exit=10 managedProcessesAfter=0
```

A long soak is warranted only if field reports show memory growth or a session
is expected to remain open for hours. The escalation threshold is private
memory growth above 2 MiB/min for at least 15 minutes, +128 MiB after warm-up,
handles growing by more than 100, or threads growing by more than 5 without
returning.

## Static bottleneck review

The exact Windows hot path still contains deliberate costs:

- a 5 ms JavaScript polling timer;
- base64 plus JSON conversion across WebView2 IPC;
- per-datagram byte-vector and string conversions;
- a multi-process WebView2 runtime.

At the observed 10 inbound and about 47 outbound datagrams per second, roughly
1.3 KiB/s of DataChannel payload, these costs are not a measured bottleneck.
The native gateway consumed about 1.8% of one core, the WebView2 tree about 10%
in the diagnostic session, and the local pump met its cadence.

Changing the 5 ms interval now would be counterproductive: a shorter interval
would spend more CPU without improving the remote 10 Hz game cadence; a longer
interval would add local latency. Replacing base64/JSON or WebView2 would be a
large correctness and lifecycle risk for no demonstrated gain.

## Escalation matrix

| Tool or change | Not used as a production change because | Trigger to revisit |
|---|---|---|
| Chromium background flags | Native `IsVisible(TRUE)` fixed the cause | Diagnostic A/B only if 1 Hz returns on exact rc8 |
| Packet capture / Wireshark | RTC stats and complete boundary totals agree; external payload is DTLS/SCTP encrypted and loopback capture exposes Q2 payload | Stable timer plus mismatched totals, route/RTT spikes, or unexplained disconnects |
| Pktmon | No Windows packet-drop evidence | `getStats` RTT spikes, candidate changes or boundary disagreement |
| ASan / Application Verifier | No crash, corruption or parser failure was observed; they do not measure ping | Native crash, malformed-input failure or lifecycle flake |
| Heap profiler / long soak | No pressure or orphan; bounded sample does not prove a multi-hour plateau | Growth thresholds listed above |
| Binary WebView2 bridge | Current packet rate and CPU are low | Gateway/WebView2 exceeds 80% of one core or boundary delay repeats above 50 ms under a representative busy room |
| Event-driven transport pump | Current 5 ms pump meets 200 ticks/s and adds bounded latency | WPR proves polling CPU is material or the existing pump misses its thresholds |

## Non-destructive optimization space

Ranked by present value:

1. **Ship rc8 unchanged.** This is the only optimization decision supported by
   the current measurements.
2. If a field latency report recurs, collect the same sanitized selected-pair
   and cadence counters. Keep this diagnostic opt-in and never retain candidate
   addresses, ICE configuration, SDP or payloads.
3. If a real 4 GiB machine shows pressure, profile the WebView2 process tree on
   that machine before changing runtime packaging. The current 8 GiB guest had
   at least 4272 MiB (4.17 GiB) available.
4. If a populated-room test raises packet rate enough to saturate a core, first
   profile the existing JSON/base64 bridge. Only then compare a smaller binary
   bridge; do not redesign WebRTC or the coordinator.
5. Launcher HTTP reuse can reduce join startup round trips, but it is outside
   gameplay and should be attempted only after startup timing proves it matters.

## Evidence boundary

Raw evidence is deliberately not tracked in the public repository because it
contains large ETL data and session-specific operational metadata. The local
evidence directory is:

```text
<local-evidence-root>/performance/20260824T072821Z-windows-rc8/evidence
```

Retained files include the sanitized RTC JSONL, gateway cadence log,
PID/system CSV, WPR ETL and `tracerpt` summary, inventory, session result and
tool result markers. The public conclusion is this redacted record.

## Final assessment

The Windows performance path is suitable for the intended small closed alpha.
The fixed background-timer defect explains the field symptom and the exact rc8
measurements show a healthy remaining path. The VM's visible rendering cost is
environmental and Q2-dominated; it does not justify changing gateway transport.

The audit does not claim that every Internet route has 60 ms RTT or that a
multi-hour leak is impossible. It establishes that this exact release, room
and VM have no remaining measured Windows transport bottleneck and provides
bounded triggers for deeper tools if a new symptom appears.
