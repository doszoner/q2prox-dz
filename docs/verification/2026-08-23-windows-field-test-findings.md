# Windows field-test findings after 1.0.0-rc6

Date: 2026-08-23
Status: **RESOLVED IN 1.0.0-rc8**

## Scope

This note records problems found when the `1.0.0-rc6` Windows portable package
was tried by community testers after the original clean-VM gates. It separates
observations from hypotheses so a reviewer can challenge the diagnosis.

Both Windows delivery forms share the same coordinator and WebView2 gateway.
Therefore a gateway or launch-argument defect affects both portable and setup.
The previously recorded Linux live gates are not invalidated by these Windows
observations.

## Findings

### 1. Transport-readiness timeout

One tester observed:

```text
q2dz-session: state=StartingGateway cause=none
q2dz-session: state=WaitingTransport cause=none
q2dz-session: state=Closing cause=timeout
Q2PRO-X - Dos.Zone ended with error 12.
```

This proves that the gateway did not report `TRANSPORT_READY` within the
coordinator deadline in that run. The available screenshot does not prove why.
It may be related to the WebView2 scheduling hypothesis below, but that link is
currently unconfirmed.

### 2. Q2PRO-X entered the remote session before selecting `3zb2`

Two Windows launch paths currently start Q2PRO-X with only:

```text
+connect 127.0.0.1:<session-port>
```

See:

- `windows/coordinator/src/windows/main.cpp`;
- `linux/src/session_main.cpp`.

On a tester's installation, `3zb2` was selected only after the connection had
already started. The captured Q2PRO-X log showed this order:

```text
Connected to 127.0.0.1:<port> (protocol 35).
----- FS_Restart -----
Current search path:
./3zb2/...
SpawnServer: catodm1
Serverdata has invalid playernum 3
ERROR: CL_ParseConfigstring: bad index: 0
```

The Dos.Zone owner saw the player connect and immediately disconnect. A local
`autoexec.cfg` workaround that selected `3zb2` before launch allowed the tester
to connect.

**Conclusion:** this is a confirmed launch-order defect, not a `maxclients`
problem. The product must launch the supported profile explicitly:

```text
+set game 3zb2 +connect 127.0.0.1:<session-port>
```

The fix belongs in both coordinators and needs an argument-order regression
test. User CFG files must not be modified as the permanent solution.

### 3. Windows gameplay advanced at approximately one update per second

Two independent Windows testers reported the same behavior after successfully
entering a room:

- Q2PRO-X rendered at about 90 FPS;
- the game world appeared to advance at roughly one update per second;
- the connection remained established;
- other participants in the same Dos.Zone environment did not have this
  slowdown.

The high local FPS makes a Q2 rendering bottleneck unlikely. Other unaffected
participants make room-owner Chrome throttling and a general server-side stall
unlikely. The common suspect is the Windows-only WebView2 transport path.

## Leading performance hypothesis: hidden WebView2 timer throttling

The Windows gateway creates a 1x1 WebView2 controller and explicitly marks it
invisible:

```cpp
RECT bounds{0, 0, 1, 1};
controller_->put_Bounds(bounds);
controller_->put_IsVisible(FALSE);
```

`windows/gateway/assets/transport.html` drives WebRTC-NET polling from a
JavaScript timer:

```javascript
const timer = setInterval(() => {
  // ...
  wait(0);
  // ...
```

Microsoft documents that a WebView whose `IsVisible` property is `FALSE` is in
background state, and that the default background timer wake interval is
normally 1000 ms. That documented interval closely matches the observed
one-update-per-second behavior:

- <https://learn.microsoft.com/en-us/microsoft-edge/webview2/reference/win32/icorewebview2experimentalsettings9?view=webview2-1.0.4071-prerelease>
- <https://learn.microsoft.com/en-us/dotnet/api/microsoft.web.webview2.core.corewebview2settings.preferredbackgroundtimerwakeinterval?view=webview2-dotnet-1.0.3796-prerelease>

Plausible causal chain:

```text
IsVisible(FALSE)
  -> WebView2 background state
  -> setInterval throttled toward 1000 ms
  -> wait(0) and WebRTC-NET packet pumping run around once per second
  -> Q2 renders normally but receives game-state updates around once per second
```

This hypothesis was later confirmed by field and instrumented A/B/C evidence;
see the resolution below.

## Historical diagnostic comparison

The affected testers were asked to close the current session, open `cmd.exe` in
the portable directory and run:

```cmd
set WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS=--disable-background-timer-throttling --disable-renderer-backgrounding --disable-backgrounding-occluded-windows
Run.cmd
```

WebView2 supports `WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS`; the three Chromium
switches were used only as a short diagnostic. They are not shipped by rc8.

Interpretation:

- smooth gameplay with the switches strongly confirms background throttling;
- unchanged one-second updates falsify or substantially weaken the hypothesis;
- a mixed result requires measuring timer and packet cadence before changing
  transport architecture.

## Minimal proposed correction after confirmation

First test the native WebView2 contract by changing only:

```cpp
controller_->put_IsVisible(TRUE);
```

The controller can remain 1x1 and the application does not need to expose a
browser UI. Rebuild the exact Windows gateway and repeat:

1. startup to `TRANSPORT_READY`;
2. real room join;
3. continuous movement and firing;
4. normal exit and owner-close cleanup;
5. portable and installed-copy smoke tests.

Do not redesign the transport or permanently ship undocumented browser flags
unless this smallest supported change fails.

## Alternative explanations to retain until the comparison finishes

- WebView2 message delivery or `PostWebMessageAsJson` cadence is slow for a
  reason other than timer throttling;
- the JavaScript/WebAssembly polling loop stalls internally;
- a Windows-only network, ICE or DataChannel path introduces the cadence;
- the two affected clients share an environmental factor not present on the
  clean test VM.

General Dos.Zone server lag, the owner's Chrome tab and Q2 rendering performance
currently fit the observations poorly because other participants were normal
and the affected Q2 clients still rendered at about 90 FPS.

## Reviewer questions

1. Is `IsVisible(TRUE)` sufficient to keep this 1x1 controller in foreground
   scheduling state when its parent window is not shown?
2. Is there a stable supported WebView2 API preferable to browser switches if
   the controller must remain backgrounded?
3. Is `setInterval(... wait(0) ...)` safe as the transport pump, or should the
   existing pump be scheduled through another already-available mechanism?
4. What is the smallest useful instrumentation for distinguishing JS timer
   cadence, UDP ingress and WebRTC egress without logging packet contents?
5. Could any other shared Windows-only path explain the near-exact one-second
   cadence more directly?

## Resolution on 2026-08-24

Both affected community testers reported that the diagnostic Chromium switches
removed the one-second update cadence. An instrumented Windows VM then compared:

- A: `IsVisible(FALSE)`, no flags — stable timer and inbound-packet gaps around
  1000 ms;
- B: `IsVisible(FALSE)`, only `--disable-background-timer-throttling` — normal
  timer cadence and no sustained one-second gaps;
- C: `IsVisible(TRUE)`, no flags — normal timer cadence and no sustained
  one-second gaps.

The VM entered `q2dm1`, moved and fired in all applicable runs. Control-plane
ICMP checks had no loss and roughly 12–13 ms average RTT; guest memory stayed
clear of pressure. Q2 used the Microsoft Basic Render Driver and could saturate
guest CPU, but that did not correlate with transport cadence: B and C remained
smooth under the same VM constraint.

The selected production correction is C:

```cpp
controller_->put_IsVisible(TRUE);
```

No Chromium flags are shipped. The 1x1 controller remains non-interactive; the
change only keeps its transport timer in foreground scheduling state. Exact
counters, package hashes and remaining gates are recorded in
[1.0.0-rc8.md](1.0.0-rc8.md).

## Release consequence

The original rc6 packages remain blocked and must not be redistributed as the
current product. The argv and cadence corrections enter rc8; only exact rc8
archives that pass [the rc8 verification record](1.0.0-rc8.md) are candidates
for closed-group distribution.
