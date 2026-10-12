/* Bounded PCM conversion shared by the pump and user-mode regression tests.
 * SPDX-License-Identifier: MIT */
#pragma once
ULONG PnVolumeGainQ15(LONG Db16);
ULONG PnConvertPcm(const UCHAR *In, ULONG InBytes, ULONG Channels, ULONG ContainerBits,
                   ULONG ValidBits, BOOLEAN IsFloat, PUCHAR Out, ULONG OutCap,
                   ULONG Target, ULONG TargetChannels, ULONG GainQ15);
