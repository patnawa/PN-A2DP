/*
 * rtp.h - RTP (RFC 3550) packetisation for A2DP media packets.
 *
 * A2DP media packets are: RTP header (12 bytes, no CSRC, no extension) followed by a
 * codec specific media payload header (SBC/aptX LL/FastStream: 1 byte with frame
 * count and fragmentation flags; AAC: LATM; aptX/aptX HD: none; LDAC: 1 byte frame
 * count) and the codec frames.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#define RTP_HEADER_SIZE   12
#define RTP_VERSION       2

typedef struct _RTP_STATE {
    USHORT  Sequence;
    ULONG   Timestamp;     /* in PCM sample units of the stream clock */
    ULONG   Ssrc;
    UCHAR   PayloadType;   /* 96 dynamic */
    BOOLEAN Marker;
} RTP_STATE, *PRTP_STATE;

VOID  RtpInit(_Out_ PRTP_STATE R, _In_ UCHAR PayloadType);
/* Writes a header at Out and advances sequence. TimestampIncrement is added after the header is written. */
VOID  RtpWriteHeader(_Inout_ PRTP_STATE R, _Out_writes_bytes_(RTP_HEADER_SIZE) PUCHAR Out, _In_ ULONG TimestampIncrement,
                     _In_ BOOLEAN Marker);
BOOLEAN RtpParseHeader(_In_reads_bytes_(Len) const UCHAR *In, _In_ ULONG Len, _Out_ PUSHORT Seq, _Out_ PULONG Timestamp,
                       _Out_ PULONG HeaderLen);
