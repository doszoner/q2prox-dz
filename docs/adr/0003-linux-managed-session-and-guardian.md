# ADR 0003: Linux launcher-owned Wine session and guardian

- **Status:** Accepted
- **Date:** 2026-08-22

## Context

Historical maintainer observations showed UDP↔WebRTC-NET transport in the
earlier Linux prototype, but it was not a shippable product boundary: it
persisted ICE through a file, treated transport startup as readiness, and did
not own Wine/Q2PRO-X or clean the session after a hard owner failure.

## Decision

Implement a native Linux coordinator, managed gateway adapter and guardian.
Fresh room configuration enters only through stdin. The coordinator starts
Wine/Q2PRO-X only after a genuine Quake II OOB round trip through the selected
browser host. All session processes share a process group and a random marker;
a guardian cleans them and a marked runtime directory when the owner dies.

Retain the pinned WebRTC-NET transport, historically observed at runtime,
behind a narrow adapter.
Use a sealed anonymous memfd only to satisfy its existing ICE-file input without
persistent credentials. The add-on does not directly modify Q2PRO-X, its Wine
prefix, CFG or game data; Q2PRO-X may perform its normal config autosave.

## Consequences

The launcher owns bounded HTTPS room lookup, selection and fresh RTC retrieval;
the user does not open the portal to join. Historical maintainer observations
reported that an installed package reached a real map through Wine and passed
lifecycle, movement/fire and room-close gates. The exact rc6 package result is
preserved as a historical PASS in
[`docs/verification/1.0.0-rc6.md`](../verification/1.0.0-rc6.md). The corrected
rc8 package has fresh package, real-room join, movement/fire input, chat
round-trip and managed-cleanup evidence in
[`docs/verification/1.0.0-rc8.md`](../verification/1.0.0-rc8.md). Room creation
and multi-client hosting remain separate work.
