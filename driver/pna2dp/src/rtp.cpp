/*
 * rtp.cpp - RTP header writer/parser for A2DP media packets.
 * SPDX-License-Identifier: MIT
 */
#include "pna2dp.h"

VOID RtpInit(_Out_ PRTP_STATE R, _In_ UCHAR PayloadType)
{
    LARGE_INTEGER t = KeQueryPerformanceCounter(NULL);
    RtlZeroMemory(R, sizeof(*R));
    R->PayloadType = PayloadType;
    R->Sequence = (USHORT)(t.LowPart & 0xFFFF);
    R->Timestamp = t.LowPart ^ 0x5A5A5A5A;
    R->Ssrc = (ULONG)(t.QuadPart >> 7) | 1;
}

VOID RtpWriteHeader(_Inout_ PRTP_STATE R, _Out_writes_bytes_(RTP_HEADER_SIZE) PUCHAR Out, _In_ ULONG TimestampIncrement,
                    _In_ BOOLEAN Marker)
{
    Out[0] = (UCHAR)(RTP_VERSION << 6);                     /* V=2, P=0, X=0, CC=0 */
    Out[1] = (UCHAR)((Marker ? 0x80 : 0x00) | (R->PayloadType & 0x7F));
    Out[2] = (UCHAR)(R->Sequence >> 8);
    Out[3] = (UCHAR)(R->Sequence & 0xFF);
    Out[4] = (UCHAR)(R->Timestamp >> 24);
    Out[5] = (UCHAR)(R->Timestamp >> 16);
    Out[6] = (UCHAR)(R->Timestamp >> 8);
    Out[7] = (UCHAR)(R->Timestamp);
    Out[8] = (UCHAR)(R->Ssrc >> 24);
    Out[9] = (UCHAR)(R->Ssrc >> 16);
    Out[10] = (UCHAR)(R->Ssrc >> 8);
    Out[11] = (UCHAR)(R->Ssrc);
    R->Sequence++;
    R->Timestamp += TimestampIncrement;
}

BOOLEAN RtpParseHeader(_In_reads_bytes_(Len) const UCHAR *In, _In_ ULONG Len, _Out_ PUSHORT Seq, _Out_ PULONG Timestamp,
                       _Out_ PULONG HeaderLen)
{
    if (Len < RTP_HEADER_SIZE) return FALSE;
    if ((In[0] >> 6) != RTP_VERSION) return FALSE;
    ULONG cc = In[0] & 0x0F;
    BOOLEAN ext = (In[0] & 0x10) != 0;
    ULONG hdr = RTP_HEADER_SIZE + cc * 4;
    if (Len < hdr) return FALSE;
    if (ext) {
        if (Len < hdr + 4) return FALSE;
        ULONG extWords = ((ULONG)In[hdr + 2] << 8) | In[hdr + 3];
        hdr += 4 + extWords * 4;
        if (Len < hdr) return FALSE;
    }
    *Seq = (USHORT)(((USHORT)In[2] << 8) | In[3]);
    *Timestamp = ((ULONG)In[4] << 24) | ((ULONG)In[5] << 16) | ((ULONG)In[6] << 8) | In[7];
    *HeaderLen = hdr;
    return TRUE;
}
