/* Pure SDP parsing of the remote A2DP (AudioSink) record, shared with core_test.
 * The response comes from the remote device, so every length is checked against the
 * bytes that remain. Data element header: Bluetooth Core, Vol 3, Part B, 3.3.
 * SPDX-License-Identifier: MIT */
#pragma once

#define PN_SDP_TYPE_UINT      1
#define PN_SDP_TYPE_UUID      3
#define PN_SDP_TYPE_SEQUENCE  6

#define PN_SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST  0x0004
#define PN_SDP_ATTR_PROFILE_DESCRIPTOR_LIST   0x0009
#define PN_SDP_ATTR_SUPPORTED_FEATURES        0x0311
#define PN_SDP_UUID_L2CAP                     0x0100
#define PN_SDP_UUID_AVDTP                     0x0019
#define PN_SDP_UUID_ADVANCED_AUDIO            0x110D
#define PN_SDP_PSM_AVDTP                      0x0019

static inline BOOLEAN PnSdpElement(const UCHAR *P, ULONG Len, UCHAR *Type, const UCHAR **Data, ULONG *DataLen, ULONG *Total)
{
    *Type = 0; *Data = NULL; *DataLen = 0; *Total = 0;
    if (Len < 1) return FALSE;
    UCHAR type = (UCHAR)(P[0] >> 3), si = (UCHAR)(P[0] & 7);
    ULONG hdr = 1, dl;
    if (type == 0) dl = 0;
    else if (si <= 4) dl = 1u << si;
    else if (si == 5) { if (Len < 2) return FALSE; dl = P[1]; hdr = 2; }
    else if (si == 6) { if (Len < 3) return FALSE; dl = ((ULONG)P[1] << 8) | P[2]; hdr = 3; }
    else { if (Len < 5) return FALSE; dl = ((ULONG)P[1] << 24) | ((ULONG)P[2] << 16) | ((ULONG)P[3] << 8) | P[4]; hdr = 5; }
    if (dl > Len - hdr) return FALSE;
    *Type = type; *Data = P + hdr; *DataLen = dl; *Total = hdr + dl;
    return TRUE;
}

/* uint16, or a 16-bit UUID */
static inline BOOLEAN PnSdpUint16(UCHAR Type, const UCHAR *D, ULONG Len, USHORT *Value)
{
    *Value = 0;
    if ((Type != PN_SDP_TYPE_UINT && Type != PN_SDP_TYPE_UUID) || Len != 2) return FALSE;
    *Value = (USHORT)(((USHORT)D[0] << 8) | D[1]);
    return TRUE;
}

/* A UUID in its 16-, 32- or 128-bit (Bluetooth base) form, reduced to 16 bits. */
static inline BOOLEAN PnSdpUuid16(UCHAR Type, const UCHAR *D, ULONG Len, USHORT *Value)
{
    static const UCHAR base[12] = { 0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB };
    *Value = 0;
    if (Type != PN_SDP_TYPE_UUID) return FALSE;
    if (Len == 2) { *Value = (USHORT)(((USHORT)D[0] << 8) | D[1]); return TRUE; }
    if ((Len == 4 || Len == 16) && D[0] == 0 && D[1] == 0) {
        for (ULONG i = 4; i < Len; i++) if (D[i] != base[i - 4]) return FALSE;
        *Value = (USHORT)(((USHORT)D[2] << 8) | D[3]);
        return TRUE;
    }
    return FALSE;
}

typedef struct _PN_SDP_A2DP_INFO {
    BOOLEAN ProfileVersionFound;   /* BluetoothProfileDescriptorList: A2DP version */
    USHORT  ProfileVersion;
    BOOLEAN FeaturesSeen;          /* SupportedFeatures attribute present (0 if malformed) */
    USHORT  SupportedFeatures;
    BOOLEAN AvdtpVersionFound;     /* ProtocolDescriptorList: AVDTP version */
    USHORT  AvdtpVersion;
    BOOLEAN L2capPsmAvdtp;         /* ProtocolDescriptorList: L2CAP with PSM 0x0019 */
} PN_SDP_A2DP_INFO;

/* Profile descriptor list: seq( seq( uuid16 0x110D, uint16 version ) ... ); first match */
static inline VOID PnSdpParseProfileList(const UCHAR *P, ULONG Len, PN_SDP_A2DP_INFO *Info)
{
    while (Len) {
        UCHAR t; const UCHAR *d; ULONG dl, tot;
        if (!PnSdpElement(P, Len, &t, &d, &dl, &tot)) return;
        if (t == PN_SDP_TYPE_SEQUENCE) {
            UCHAR t1, t2; const UCHAR *d1, *d2; ULONG l1, l2, n1, n2;
            USHORT uuid, ver;
            if (PnSdpElement(d, dl, &t1, &d1, &l1, &n1) && PnSdpUint16(t1, d1, l1, &uuid) && uuid == PN_SDP_UUID_ADVANCED_AUDIO &&
                PnSdpElement(d + n1, dl - n1, &t2, &d2, &l2, &n2) && PnSdpUint16(t2, d2, l2, &ver)) {
                Info->ProfileVersionFound = TRUE;
                Info->ProfileVersion = ver;
                return;
            }
        }
        P += tot; Len -= tot;
    }
}

/* Protocol descriptor list: seq( seq(uuid L2CAP, uint16 PSM), seq(uuid AVDTP, uint16 version) ) */
static inline VOID PnSdpParseProtocolList(const UCHAR *P, ULONG Len, PN_SDP_A2DP_INFO *Info)
{
    while (Len) {
        UCHAR t; const UCHAR *d; ULONG dl, tot;
        if (!PnSdpElement(P, Len, &t, &d, &dl, &tot)) return;
        if (t == PN_SDP_TYPE_SEQUENCE) {
            UCHAR t1, t2; const UCHAR *d1, *d2; ULONG l1, l2, n1, n2;
            USHORT uuid, value;
            if (PnSdpElement(d, dl, &t1, &d1, &l1, &n1) && PnSdpUuid16(t1, d1, l1, &uuid) &&
                PnSdpElement(d + n1, dl - n1, &t2, &d2, &l2, &n2) && t2 == PN_SDP_TYPE_UINT && PnSdpUint16(t2, d2, l2, &value)) {
                if (uuid == PN_SDP_UUID_L2CAP && value == PN_SDP_PSM_AVDTP) Info->L2capPsmAvdtp = TRUE;
                else if (uuid == PN_SDP_UUID_AVDTP && !Info->AvdtpVersionFound) {
                    Info->AvdtpVersionFound = TRUE;
                    Info->AvdtpVersion = value;
                }
            }
        }
        P += tot; Len -= tot;
    }
}

/* AttributeLists: seq( seq(attribute list of record 1) ... ); a bare list is accepted too.
 * Only the first record is read. FALSE: not an SDP sequence. */
static inline BOOLEAN PnSdpParseA2dpRecord(const UCHAR *P, ULONG Len, PN_SDP_A2DP_INFO *Info)
{
    UCHAR t; const UCHAR *d; ULONG dl, tot;
    Info->ProfileVersionFound = Info->FeaturesSeen = Info->AvdtpVersionFound = Info->L2capPsmAvdtp = FALSE;
    Info->ProfileVersion = Info->SupportedFeatures = Info->AvdtpVersion = 0;
    if (!PnSdpElement(P, Len, &t, &d, &dl, &tot) || t != PN_SDP_TYPE_SEQUENCE) return FALSE;
    {
        UCHAR t1; const UCHAR *d1; ULONG l1, n1;
        if (PnSdpElement(d, dl, &t1, &d1, &l1, &n1) && t1 == PN_SDP_TYPE_SEQUENCE) { d = d1; dl = l1; }
    }
    /* attribute list: uint16 id, value, uint16 id, value, ... */
    while (dl) {
        UCHAR ti, tv; const UCHAR *di, *dv; ULONG li, lv, ni, nv;
        USHORT id;
        if (!PnSdpElement(d, dl, &ti, &di, &li, &ni) || !PnSdpUint16(ti, di, li, &id)) break;
        if (!PnSdpElement(d + ni, dl - ni, &tv, &dv, &lv, &nv)) break;
        if (id == PN_SDP_ATTR_PROFILE_DESCRIPTOR_LIST && tv == PN_SDP_TYPE_SEQUENCE) PnSdpParseProfileList(dv, lv, Info);
        else if (id == PN_SDP_ATTR_PROTOCOL_DESCRIPTOR_LIST && tv == PN_SDP_TYPE_SEQUENCE) PnSdpParseProtocolList(dv, lv, Info);
        else if (id == PN_SDP_ATTR_SUPPORTED_FEATURES) {
            Info->FeaturesSeen = TRUE;
            PnSdpUint16(tv, dv, lv, &Info->SupportedFeatures);
        }
        d += ni + nv; dl -= ni + nv;
    }
    return TRUE;
}

/* The version AVDTP procedures are chosen by (GET_ALL_CAPABILITIES needs 1.3): the AVDTP
 * protocol version from the ProtocolDescriptorList, never the A2DP profile version (T5).
 * 0: unknown, keep the default. */
static inline USHORT PnSdpAvdtpVersionToUse(const PN_SDP_A2DP_INFO *Info)
{
    return Info->AvdtpVersionFound ? Info->AvdtpVersion : 0;
}
