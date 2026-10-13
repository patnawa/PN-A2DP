/*
 * audio_notify.h - device -> audio endpoint notifications (contract C6).
 *
 * Both functions are callable at IRQL <= DISPATCH_LEVEL. They only record the request and
 * queue a PASSIVE work item owned by the PortCls adapter, which then rebuilds the wave
 * format table / raises the jack event. They are no-ops once the device is stopping
 * (D->Removing) or the audio filters are removed, so a late call during teardown is safe.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

/* The sink capabilities, the negotiated codec/rate or the configuration changed: rebuild
 * the render pin's format table and, if it changed, raise KSEVENT_PINCAPS_FORMATCHANGE. */
VOID PnAudioFormatsChanged(_In_ PPNA2DP_DEVICE D);

/* Report whether the receiver is connected (Bluetooth inbox semantics: the jack is
 * connected while the remote device is connected). Updates KSJACK_DESCRIPTION.IsConnected
 * and raises KSEVENT_PINCAPS_JACKINFOCHANGE on the topology bridge pin when it changes. */
VOID PnAudioSetJackConnected(_In_ PPNA2DP_DEVICE D, _In_ BOOLEAN Connected);
