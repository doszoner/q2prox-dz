# Windows headless exact-client gate preflight, 2026-09-05

Purpose: avoid treating a non-interactive VirtualBox run as proof for the
exact Q2PRO-X Custom source/menu gate.

## Environment

```text
vm: Windows 11 25H2 + LTSC (x64) 28in1
uuid: 9a57e185-2b98-458b-b58f-c9e23e666ce9
VirtualBox Guest Additions: 7.2.16
Windows version: 10.0.26200.9168
memory: 8192 MiB
cpus: 4
snapshot: q2prox-rc15-fixture-ready-20260905
snapshot UUID: a8badfdb-6b06-4bf8-b37f-1cd7857c8229
```

The VM was started with:

```text
VBoxManage startvm 9a57e185-2b98-458b-b58f-c9e23e666ce9 --type headless
```

GuestControl authenticated as the prepared non-secret test user and executed
basic commands successfully.

## Evidence

```text
VBoxManage guestproperty get ... /VirtualBox/GuestAdd/Version
Value: 7.2.16

cmd.exe /c ver
Microsoft Windows [Version 10.0.26200.9168]

cmd.exe /c whoami
windows-ggfdko9\user

cmd.exe /c query user
No User exists for *
```

## Result

```text
result: NOT A VALID EXACT-CLIENT GATE ENVIRONMENT
```

There is no logged-in interactive Windows desktop session. Therefore this VM
state is valid for GuestControl build/package/process checks, but not for the
exact Q2PRO-X menu gate that requires visible user interaction:

```text
Q2PRO-X -> Join online -> Custom -> Refresh -> select row -> Enter
```

Do not close the exact-client gate from this state. Use either a visible,
logged-in Windows desktop session operated by a human, or an operator-owned
Linux GUI session.

## Cleanup

The VM was saved after the preflight:

```text
VMState="saved"
CurrentSnapshotName="q2prox-rc15-fixture-ready-20260905"
CurrentSnapshotUUID="a8badfdb-6b06-4bf8-b37f-1cd7857c8229"
running VMs: none
```
