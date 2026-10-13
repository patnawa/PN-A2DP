/* Lane S: control-interface access decisions. Included by core_test.cpp.
 * SPDX-License-Identifier: MIT */
#include "control_access.h"

static void TestControlAccess()
{
    const ULONG user = PN_CTL_ROLE_USER, admin = PN_CTL_ROLE_ADMIN, sys = PN_CTL_ROLE_SYSTEM;

    /* Read-only status for everyone, including the unelevated panel. */
    const ULONG open[] = {IOCTL_PNA2DP_GET_VERSION, IOCTL_PNA2DP_GET_STATUS, IOCTL_PNA2DP_GET_CAPABILITIES,
        IOCTL_PNA2DP_GET_CONFIG, IOCTL_PNA2DP_WAIT_EVENT, IOCTL_PNA2DP_GET_STATS, IOCTL_PNA2DP_GET_QUALITY,
        /* the unelevated panel saves settings, reconnects and clears counters */
        IOCTL_PNA2DP_SET_CONFIG, IOCTL_PNA2DP_RECONNECT, IOCTL_PNA2DP_RESET_STATS};
    for (ULONG i = 0; i < PN_ARRAYSIZE(open); ++i) {
        CHECK(PnControlRoleAllows(user, open[i]));
        CHECK(PnControlRoleAllows(admin, open[i]));
        CHECK(PnControlRoleAllows(sys, open[i]));
    }

    /* Trace text (addresses, names) and the audio capture need an elevated caller. */
    const ULONG elevated[] = {IOCTL_PNA2DP_GET_TRACE, IOCTL_PNA2DP_SBC_SAMPLE};
    for (ULONG i = 0; i < PN_ARRAYSIZE(elevated); ++i) {
        CHECK(!PnControlRoleAllows(user, elevated[i]));
        CHECK(PnControlRoleAllows(admin, elevated[i]));
        CHECK(PnControlRoleAllows(sys, elevated[i]));
    }

    /* Only the LocalSystem codec host service may receive PCM or inject payloads. */
    const ULONG host[] = {IOCTL_PNA2DP_CODEC_REGISTER, IOCTL_PNA2DP_CODEC_UNREGISTER,
        IOCTL_PNA2DP_CODEC_GET_WORK, IOCTL_PNA2DP_CODEC_PUT_RESULT};
    for (ULONG i = 0; i < PN_ARRAYSIZE(host); ++i) {
        CHECK(!PnControlRoleAllows(user, host[i]));
        CHECK(!PnControlRoleAllows(admin, host[i]));
        CHECK(PnControlRoleAllows(sys, host[i]));
    }

    /* Unknown codes fail closed, including a known function with other access bits. */
    CHECK(!PnControlRoleAllows(admin, PNA2DP_CTL(0x8FF, FILE_READ_ACCESS)));
    CHECK(!PnControlRoleAllows(user, PNA2DP_CTL(0x800, FILE_ANY_ACCESS)));
    CHECK(PnControlRoleAllows(sys, PNA2DP_CTL(0x8FF, FILE_READ_ACCESS)));
    CHECK(!PnControlRoleAllows(user, 0));
    /* Out-of-range roles never widen access. */
    CHECK(!PnControlRoleAllows(7, IOCTL_PNA2DP_CODEC_REGISTER));
    CHECK(!PnControlRoleAllows(7, IOCTL_PNA2DP_GET_TRACE));

    /* Token -> role. Administrators counts only when enabled (not a UAC deny-only group). */
    static_assert(PN_SE_GROUP_ENABLED == SE_GROUP_ENABLED && PN_SE_GROUP_USE_FOR_DENY_ONLY == SE_GROUP_USE_FOR_DENY_ONLY, "winnt.h values");
    CHECK(PnControlRoleFromToken(TRUE, FALSE, 0) == sys);
    CHECK(PnControlRoleFromToken(TRUE, TRUE, SE_GROUP_ENABLED) == sys);
    CHECK(PnControlRoleFromToken(FALSE, TRUE, SE_GROUP_ENABLED | SE_GROUP_ENABLED_BY_DEFAULT | SE_GROUP_MANDATORY) == admin);
    CHECK(PnControlRoleFromToken(FALSE, TRUE, SE_GROUP_USE_FOR_DENY_ONLY) == user);
    CHECK(PnControlRoleFromToken(FALSE, TRUE, SE_GROUP_ENABLED | SE_GROUP_USE_FOR_DENY_ONLY) == user);
    CHECK(PnControlRoleFromToken(FALSE, TRUE, SE_GROUP_ENABLED_BY_DEFAULT) == user);
    CHECK(PnControlRoleFromToken(FALSE, FALSE, SE_GROUP_ENABLED) == user);
    CHECK(PnControlRoleFromToken(FALSE, FALSE, 0) == user);

    /* CODEC_REGISTER.HostPid must name the calling process. */
    CHECK(PnControlCodecHostPidValid(1234, 1234));
    CHECK(!PnControlCodecHostPidValid(1234, 4321));
    CHECK(!PnControlCodecHostPidValid(0, 0));
}
