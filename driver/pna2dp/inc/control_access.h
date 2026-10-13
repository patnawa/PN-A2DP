/* Per-handle caller roles for the private control interface, shared with core_test.
 *
 * The role is captured from the caller's token when the control handle is opened and
 * checked on every IOCTL, on top of the FILE_READ/FILE_WRITE access bits the I/O manager
 * already enforces from the handle's granted access. No device-wide SDDL is used: the FDO
 * also serves PortCls/KS opens from audiodg and the audio service.
 *
 * SPDX-License-Identifier: MIT */
#pragma once

#define PN_CTL_ROLE_USER    0u   /* any other caller (interactive user, service account) */
#define PN_CTL_ROLE_ADMIN   1u   /* token with BUILTIN\Administrators enabled (elevated) */
#define PN_CTL_ROLE_SYSTEM  2u   /* token user is LocalSystem (S-1-5-18) */

/* winnt.h SE_GROUP_* token group attributes (the kernel headers do not define them). */
#define PN_SE_GROUP_ENABLED             0x00000004u
#define PN_SE_GROUP_USE_FOR_DENY_ONLY   0x00000010u

/* Lowest role that may issue IoControlCode. Every IOCTL added to pna2dp_public.h must be
 * listed here; unknown codes fail closed (LocalSystem only, and the dispatcher then rejects
 * them as invalid requests). */
static inline ULONG PnControlRequiredRole(ULONG IoControlCode)
{
    switch (IoControlCode) {
    case IOCTL_PNA2DP_GET_VERSION:
    case IOCTL_PNA2DP_GET_STATUS:
    case IOCTL_PNA2DP_GET_CAPABILITIES:
    case IOCTL_PNA2DP_GET_CONFIG:
    case IOCTL_PNA2DP_WAIT_EVENT:
    case IOCTL_PNA2DP_GET_STATS:
    case IOCTL_PNA2DP_GET_QUALITY:
    /* Deliberately available to interactive users: the control panel runs unelevated and
     * saves settings, reconnects and clears counters. These requests only affect the
     * caller's own Bluetooth audio session and still need a handle opened for write. */
    case IOCTL_PNA2DP_SET_CONFIG:
    case IOCTL_PNA2DP_RECONNECT:
    case IOCTL_PNA2DP_RESET_STATS:
        return PN_CTL_ROLE_USER;
    /* The trace ring holds Bluetooth addresses and device names; the SBC sample holds
     * audio. Diagnostic tools (pna2dpctl trace, sbc_capture, ks_test) run elevated. */
    case IOCTL_PNA2DP_GET_TRACE:
    case IOCTL_PNA2DP_SBC_SAMPLE:
        return PN_CTL_ROLE_ADMIN;
    /* The codec host receives all PCM and injects payloads sent to the receiver; only the
     * LocalSystem codec host service may take that role. */
    case IOCTL_PNA2DP_CODEC_REGISTER:
    case IOCTL_PNA2DP_CODEC_UNREGISTER:
    case IOCTL_PNA2DP_CODEC_GET_WORK:
    case IOCTL_PNA2DP_CODEC_PUT_RESULT:
    default:
        return PN_CTL_ROLE_SYSTEM;
    }
}

static inline BOOLEAN PnControlRoleAllows(ULONG Role, ULONG IoControlCode)
{
    return Role <= PN_CTL_ROLE_SYSTEM && Role >= PnControlRequiredRole(IoControlCode);
}

/* AdminsAttributes are the SE_GROUP_* attributes of BUILTIN\Administrators in the token.
 * A UAC-filtered token carries the group as deny-only, which must not count. */
static inline ULONG PnControlRoleFromToken(BOOLEAN UserIsLocalSystem, BOOLEAN HasAdminsGroup, ULONG AdminsAttributes)
{
    if (UserIsLocalSystem) return PN_CTL_ROLE_SYSTEM;
    if (HasAdminsGroup && (AdminsAttributes & PN_SE_GROUP_ENABLED) && !(AdminsAttributes & PN_SE_GROUP_USE_FOR_DENY_ONLY))
        return PN_CTL_ROLE_ADMIN;
    return PN_CTL_ROLE_USER;
}

/* CODEC_REGISTER.HostPid must name the process that sends the request. */
static inline BOOLEAN PnControlCodecHostPidValid(ULONG HostPid, ULONG RequestorPid)
{
    return HostPid != 0 && HostPid == RequestorPid;
}
