# Reliability improvements, application 0.1.0.30 / driver 0.1.0.18

Implemented and installed, 12 October 2026. The six requested areas are tracked here;
implementation does not by itself establish hardware validation.

1. Protected service locations: one conservative owner/DACL/path policy checks
   setup, both service-registration paths and service startup. Ordinary users
   cannot own or modify the selected tree. Ancestor replacement rights, reparse
   points, hard-linked files, remote paths and ambiguous names are rejected.
   Files are not silently granted new permissions. Portable, unelevated use is
   separate from installing a privileged service.
2. Graduated quality recovery: 30 seconds of continuous healthy traffic before
   the first upgrade, then 10, then 5 seconds per step. Seven steps take 65
   seconds in deterministic tests. Downgrading within 10 seconds of an upgrade
   requires 60 clean seconds before retrying. Idle/gaps reset fast recovery.
3. Connection recovery: retries continue at a capped 30-second interval while
   audio is requested; returning ACL connections reset the retry budget. Power
   callbacks stop pumping asynchronously and request a fresh session on resume.
   Codec-host discovery errors retain workers, and repeated worker failures use
   a bounded retry delay rather than restarting every poll.
4. Quality events: 32 observed events, retained across interrupted observations
   for the same selected receiver. Changing device clears the history. Driver
   reasons and recovery intervals use former reserved ABI words; older drivers
   show unavailable causes and retain the original 30-second countdown.
5. Codec validation: invalid-open contract checks and repeated encoder flush /
   quality changes complement the existing matrix and independent decoding.
   aptX invalid rates and aptX/LDAC oversized codec-info lengths are rejected.
6. Optional recovery: after persistent fixed-rate aptX trouble, **Quality >
   Events > Use adaptive SBC** requests SBC using the saved SBC settings and
   enables adaptive bitrate. It saves SBC as first preference, may interrupt
   playback briefly, requires an explicit click, and is unavailable with pending
   edits, stale observations, unsupported SBC or an unavailable encoder. A
   60-second retry cooldown prevents repeated requests. It does not promise a
   lower initial bitrate or force settings outside the receiver/user limits.

## Validation completed on this machine

- Full package build and test signing succeeded. `verify.ps1 -SkipBuild -Sanitize
  -Interop` passed **29 groups**: 26,199 core checks plus 50,000 malformed inputs,
  54 health checks, 24 protected-path checks, 21 service-failure checks, codec
  matrix/flush/reconfiguration coverage, independent SBC/AAC/aptX-family decoding,
  AddressSanitizer, payload/license consistency and startup. Windows-mode packaging
  passed **7 groups**; signing-policy tests passed **7**. LDAC independent decoding
  is not part of that result.
- Reproduced failures before fixes: unsupported aptX rates, oversized aptX/LDAC
  codec information, too-slow multi-step recovery, exhausted reconnect retries,
  unprotected service registration and fallback offers suppressed by normal timer
  jitter. Focused regressions now pass.
- Visually inspected application 0.1.0.30 on this machine: readable Quality buttons
  and Events dialog, correct version, and SBC fallback disabled for an idle SBC
  receiver. The optional fallback's physical aptX-to-SBC transition remains untested.
  The installed panel's **Check my audio** also completed successfully, reporting
  the real endpoint, driver, receiver and host; idle transport remained informational.
- Full upgrade returned **0**, installed app **0.1.0.30** and driver **0.1.0.18**.
  The live driver query reports **0x00010012**, ABI 1; installed host hash matches
  the tested host. Its service is running. Exact saved preferences, Windows default
  selection, volume and mute were retained. Windows assigned a new endpoint ID,
  which the test rediscovered before playback.
- An elevated installation attempt into a user-writable workspace returned
  **access denied (5)**, created no destination and preserved the running service
  and its path. This directly tests rejection before installation side effects;
  it is not a complete adversarial filesystem/security audit.
- Two 30-second silent WASAPI streams submitted and drained 1,440,000 frames each.
  The receiver observation passed at **SBC 48 kHz / 672 kbps**, with **1,929 packets**
  over 15 seconds and no new drops, write failures or latency trims.
- Three software reconnects recovered streaming plus receiver absolute-volume
  integration in **2.184 / 2.599 / 2.558 seconds**, at **672 kbps**. Each following
  three-second window advanced 377–379 packets without transport errors. Saved
  preferences and the endpoint were retained.
- **31 normal-user KS checks passed**, including both 44.1/48 kHz pin allocation,
  control permissions, event-listener cleanup and buffer/latency bounds.
- The user reported a Sony power cycle while monitoring was active. The observation
  did not capture a disconnected state, so physical power-cycle recovery is **not
  counted as verified**.
- **Listening check passed, user-confirmed on 12 October 2026:** usual music on
  the Sony with app 0.1.0.30 / driver 0.1.0.18 sounded clear at normal loudness,
  and Windows volume and mute worked. This is subjective confirmation for this
  receiver, separate from the silent transport measurements above.

Local evidence: `out/improvements/results.json`, `out/improvements/`,
`out/verification/20261012-194902`, `out/verification/windows-mode-20261012-195129`,
`out/verification/receiver-codec-20261012-195247`, and
`out/verification/reconnect-20261012-195320-621`. No boot/security policy was changed.

## Remaining receiver and power coverage

Only the Sony CMT-SBT40D is available on 12 October. The user expects an MR5 on
13 October; its codecs must be discovered
from the receiver, not inferred from its name. Physical AAC/aptX/LDAC and actual
sleep/wake verification remain pending until exercised. Hardware codec-host crash
recovery and aptX-to-SBC fallback require a host-encoded receiver path. LDAC
independent decoder validation is also separate from encoder tests. Driver Verifier,
HLK and a clean retail/production-signed installation remain release qualification work.

After selecting a supported codec and starting playback, observe the exact
receiver without changing settings:

```powershell
.\build\test-receiver-codec.ps1 -InstanceId '<exact PnP instance ID>' -ExpectedCodec SBC -Seconds 15
```

Repeat for each **advertised and successfully negotiated** codec on the new
receiver. A passing transport observation is not an acoustic or latency result;
confirm sound, volume/mute and reconnects separately. Local raw reports can
contain device identifiers; use the panel's allowlisted diagnostic export for
public issue reports.
