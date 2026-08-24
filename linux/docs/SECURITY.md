# Security and local trust boundary

## Protected values

The implementation treats room JSON, ICE/TURN credentials, SDP, ICE candidates,
portal/session tokens and any game secret as sensitive. They are allowed only
in bounded process memory and private pipes. Core dumps and ptrace-style dump
access are disabled for managed processes where supported.

## Explicit prohibitions

- no credentials in argv or environment;
- no room JSON or credential file on disk;
- no logging of room JSON, ICE URLs with userinfo, TURN username/credential,
  SDP or candidates;
- no listening on wildcard or non-loopback addresses;
- no shell interpolation for process creation;
- no portal DOM automation, cookie reuse or private write-API reverse
  engineering;
- no direct CFG, PAK or Q2PRO-X executable modification by the add-on (Q2PRO-X
  may perform its normal config autosave);
- no root service.

## Local attacker scope

The add-on is a per-user process and does not claim isolation from a fully
compromised account. It minimizes exposure against accidental persistence,
process-list disclosure and crash artifacts. A same-UID adversary with sufficient
local debugging privileges remains outside the guaranteed boundary.

## Safe cleanup

A directory may be removed only when all conditions hold:

- it is below the configured runtime base after canonicalization;
- it contains the exact add-on ownership marker;
- it is not the runtime base itself;
- cleanup does not cross filesystems or follow a replacement path outside the
  base.

The uninstaller similarly refuses to remove paths without its install marker.
It also compares the complete install tree with its owned-path manifest and
stops before deletion when an unknown file or directory is present.

Before `execve`, coordinator and managed gateway rebuild the child environment
from an explicit desktop, locale, graphics and Wine runtime allowlist. Proxy,
cloud/registry auth and arbitrary project variables are not inherited.
Synthetic credential, userinfo proxy and auth-config unit tests plus fake-child
tests enforce this boundary.
