/* Pure AVDTP wire helpers. SPDX-License-Identifier: MIT */
#pragma once

/* Message types (signal header, bits 1..0 of the first octet) */
#define AVDTP_MSG_COMMAND          0x00
#define AVDTP_MSG_GENERAL_REJECT   0x01
#define AVDTP_MSG_RESPONSE_ACCEPT  0x02
#define AVDTP_MSG_RESPONSE_REJECT  0x03

/* Packet types (bits 3..2) */
#define AVDTP_PKT_SINGLE           0x00
#define AVDTP_PKT_START            0x01
#define AVDTP_PKT_CONTINUE         0x02
#define AVDTP_PKT_END              0x03

/* Signal identifiers */
#define AVDTP_DISCOVER             0x01
#define AVDTP_GET_CAPABILITIES     0x02
#define AVDTP_SET_CONFIGURATION    0x03
#define AVDTP_GET_CONFIGURATION    0x04
#define AVDTP_RECONFIGURE          0x05
#define AVDTP_OPEN                 0x06
#define AVDTP_START                0x07
#define AVDTP_CLOSE                0x08
#define AVDTP_SUSPEND              0x09
#define AVDTP_ABORT                0x0A
#define AVDTP_SECURITY_CONTROL     0x0B
#define AVDTP_GET_ALL_CAPABILITIES 0x0C
#define AVDTP_DELAYREPORT          0x0D

/* Service categories */
#define AVDTP_CAT_MEDIA_TRANSPORT     0x01
#define AVDTP_CAT_REPORTING           0x02
#define AVDTP_CAT_RECOVERY            0x03
#define AVDTP_CAT_CONTENT_PROTECTION  0x04
#define AVDTP_CAT_HEADER_COMPRESSION  0x05
#define AVDTP_CAT_MULTIPLEXING        0x06
#define AVDTP_CAT_MEDIA_CODEC         0x07
#define AVDTP_CAT_DELAY_REPORTING     0x08

/* Error codes (AVDTP 8.20.6) */
#define AVDTP_ERR_BAD_HEADER_FORMAT        0x01
#define AVDTP_ERR_BAD_LENGTH               0x11
#define AVDTP_ERR_BAD_ACP_SEID             0x12
#define AVDTP_ERR_SEP_IN_USE               0x13
#define AVDTP_ERR_SEP_NOT_IN_USE           0x14
#define AVDTP_ERR_BAD_SERV_CATEGORY        0x17
#define AVDTP_ERR_BAD_PAYLOAD_FORMAT       0x18
#define AVDTP_ERR_NOT_SUPPORTED_COMMAND    0x19
#define AVDTP_ERR_INVALID_CAPABILITIES     0x1A
#define AVDTP_ERR_BAD_RECOVERY_TYPE        0x22
#define AVDTP_ERR_BAD_MEDIA_TRANSPORT_FORMAT 0x23
#define AVDTP_ERR_BAD_RECOVERY_FORMAT      0x25
#define AVDTP_ERR_BAD_ROHC_FORMAT          0x26
#define AVDTP_ERR_BAD_CP_FORMAT            0x27
#define AVDTP_ERR_BAD_MULTIPLEXING_FORMAT  0x28
#define AVDTP_ERR_UNSUPPORTED_CONFIGURATION 0x29
#define AVDTP_ERR_BAD_STATE                0x31
/* A2DP codec specific error codes (A2DP 5.1.3) */
#define A2DP_ERR_INVALID_CODEC_TYPE        0xC1
#define A2DP_ERR_NOT_SUPPORTED_CODEC_TYPE  0xC2
#define A2DP_ERR_INVALID_SAMPLING_FREQUENCY 0xC3
#define A2DP_ERR_NOT_SUPPORTED_SAMPLING_FREQUENCY 0xC4
#define A2DP_ERR_INVALID_CHANNEL_MODE      0xC5
#define A2DP_ERR_NOT_SUPPORTED_CHANNEL_MODE 0xC6
#define A2DP_ERR_INVALID_SUBBANDS          0xC7
#define A2DP_ERR_NOT_SUPPORTED_SUBBANDS    0xC8
#define A2DP_ERR_INVALID_ALLOCATION_METHOD 0xC9
#define A2DP_ERR_NOT_SUPPORTED_ALLOCATION_METHOD 0xCA
#define A2DP_ERR_INVALID_MIN_BITPOOL       0xCB
#define A2DP_ERR_NOT_SUPPORTED_MIN_BITPOOL 0xCC
#define A2DP_ERR_INVALID_MAX_BITPOOL       0xCD
#define A2DP_ERR_NOT_SUPPORTED_MAX_BITPOOL 0xCE
#define A2DP_ERR_INVALID_BLOCK_LENGTH      0xD2
#define A2DP_ERR_INVALID_CP_TYPE           0xE0
#define A2DP_ERR_INVALID_CP_FORMAT         0xE1

/* Media types */
#define AVDTP_MEDIA_TYPE_AUDIO        0x00
/* TSEP */
#define AVDTP_TSEP_SOURCE             0x00
#define AVDTP_TSEP_SINK               0x01

#define AVDTP_MAX_SIGNAL_PAYLOAD      512    /* reassembled command payload */
#define AVDTP_MAX_SEPS                PNA2DP_MAX_REMOTE_SEPS
/* Signalling response timeout. GAVDP 1.3 names this timer TGAVDP100 ("100" is its number,
 * not milliseconds) and gives it a range of 0.5 to 3 s; we use the upper end for slow
 * sinks. A dead link then costs 3 s per command instead of the 4 s we used to wait. */
#define AVDTP_RESPONSE_TIMEOUT_MS     3000
#define AVDTP_LOCAL_SEP_COUNT         8

typedef struct _AVDTP_LOCAL_SEP {
    UCHAR   Seid;        /* 1..62 */
    UCHAR   InUse;
    UCHAR   MediaType;
    UCHAR   Tsep;
    ULONG   Codec;       /* PNA2DP_CODEC */
    UCHAR   CodecCaps[32];   /* capability IEs we advertise */
    UCHAR   CodecCapsLength;
    BOOLEAN DelayReporting;
} AVDTP_LOCAL_SEP, *PAVDTP_LOCAL_SEP;

typedef struct _AVDTP_REMOTE_SEP {
    UCHAR   Seid;
    UCHAR   InUse;
    UCHAR   MediaType;
    UCHAR   Tsep;
    BOOLEAN CapsValid;
    ULONG   ServiceCaps;     /* PNA2DP_SVC_* */
    ULONG   Codec;           /* PNA2DP_CODEC */
    UCHAR   MediaCodecType;  /* 0x00 SBC, 0x02 AAC, 0xFF vendor */
    ULONG   VendorId;
    USHORT  VendorCodecId;
    UCHAR   CodecCaps[32];
    UCHAR   CodecCapsLength;
    UCHAR   CpTypeMask;      /* AVDTP_CP_MASK_* of the content protection entries */
    UCHAR   UnknownCategory; /* first service category > 8 seen (skipped), 0 if none */
} AVDTP_REMOTE_SEP, *PAVDTP_REMOTE_SEP;

/* Content protection types (Bluetooth assigned numbers), recorded as a mask */
#define AVDTP_CP_TYPE_DTCP            0x0001
#define AVDTP_CP_TYPE_SCMS_T          0x0002
#define AVDTP_CP_MASK_DTCP            0x01
#define AVDTP_CP_MASK_SCMS_T          0x02
#define AVDTP_CP_MASK_OTHER           0x80

typedef enum _AVDTP_STREAM_STATE {
    AvdtpStateIdle = 0,
    AvdtpStateConfigured,
    AvdtpStateOpening,
    AvdtpStateOpen,
    AvdtpStateStreaming,
    AvdtpStateClosing,
    AvdtpStateAborting
} AVDTP_STREAM_STATE;

/* Reassembly of fragmented signalling messages (START/CONTINUE/END) */
typedef struct _AVDTP_REASSEMBLY {
    BOOLEAN  Active;
    UCHAR    Label;
    UCHAR    MessageType;
    UCHAR    SignalId;
    UCHAR    Nosp;              /* remaining packets expected */
    UCHAR    Buffer[AVDTP_MAX_SIGNAL_PAYLOAD];
    ULONG    Length;
} AVDTP_REASSEMBLY;

enum AVDTP_PARSE_RESULT { AvdtpInvalid, AvdtpMore, AvdtpComplete };
AVDTP_PARSE_RESULT AvdtpReassemble(AVDTP_REASSEMBLY *R, const UCHAR *Data, ULONG Len);
BOOLEAN AvdtpParseServiceCapabilities(const UCHAR *P, ULONG Len, PAVDTP_REMOTE_SEP Sep);

/* DISCOVER response -> audio SEPs (non-audio entries are skipped). Returns the count. */
ULONG AvdtpParseDiscover(const UCHAR *P, ULONG Len, PAVDTP_REMOTE_SEP Seps, ULONG Max);

/* Media Codec service capability: [cat][len][media type][codec type][IEs] */
ULONG AvdtpBuildMediaCodecCap(_Out_writes_bytes_(Cap) PUCHAR Out, _In_ ULONG Cap, _In_ UCHAR MediaCodecType,
                              _In_reads_bytes_(Len) const UCHAR *Ies, _In_ ULONG Len);

/* ---- Acceptor (ACP) command handling, without I/O ---------------------------------
 * AvdtpAcpProcess validates one command against a snapshot of the stream and builds the
 * response. The caller applies the effects in this order: store words (STORE_*,
 * MEDIA_EXPECTED, RESET_DELAY), send the response, REMOTE_START / REMOTE_SUSPEND callback,
 * RELEASE_REMOTE, MidState, media disconnect, NewState, delay callback. A SEP whose
 * LocalSeps[i].InUse is set is reported in use by DISCOVER (T10); SET_CONFIGURATION and
 * RECONFIGURE accept only the service categories we implement (T7); DELAYREPORT only for
 * the SEP of a configured stream (T6). The device's codec decision for SET_CONFIGURATION / RECONFIGURE is a
 * callback, made at the same point as before (returns an AVDTP/A2DP error, 0 = accept).
 * The acceptor runs while a command of ours is outstanding (IntSignal, T9): a receiver
 * SET_CONFIGURATION during ours is refused SEP_IN_USE (one stream per device), a receiver
 * OPEN during ours BAD_STATE (one side connects the transport); everything else is
 * answered as usual, at once. The media disconnect runs outside the stream lock. */
typedef UCHAR (*PFN_AVDTP_ACP_CHECK_CONFIG)(_In_ PVOID Ctx, _In_ PAVDTP_LOCAL_SEP Local, _In_ UCHAR IntSeid,
                                            _In_reads_bytes_(Len) const UCHAR *Ies, _In_ ULONG Len,
                                            _In_ ULONG ServiceCaps, _Out_ PUCHAR FailedCategory);
typedef UCHAR (*PFN_AVDTP_ACP_CHECK_RECONFIG)(_In_ PVOID Ctx, _In_reads_bytes_(Len) const UCHAR *Ies, _In_ ULONG Len,
                                              _Out_ PUCHAR FailedCategory);

typedef struct _AVDTP_ACP_CONTEXT {
    PAVDTP_LOCAL_SEP   LocalSeps;     /* Seps[i].InUse: SEP unavailable (codec off / no encoder) */
    ULONG              LocalSepCount;
    AVDTP_STREAM_STATE StreamState;
    UCHAR              LocalSeid;
    UCHAR              RemoteSeid;
    const UCHAR       *ConfiguredIes;
    UCHAR              ConfiguredIesLength;
    BOOLEAN            DelayReportingNegotiated;
    UCHAR              IntSignal;     /* our command outstanding right now (T9), 0 = none */
    PFN_AVDTP_ACP_CHECK_CONFIG   CheckConfiguration;
    PFN_AVDTP_ACP_CHECK_RECONFIG CheckReconfigure;
    PVOID              CheckContext;
} AVDTP_ACP_CONTEXT;

#define AVDTP_ACP_FX_STORE_CONFIG     0x0001  /* LocalSeid/RemoteSeid/Ies/ServiceCaps/DelayReporting */
#define AVDTP_ACP_FX_STORE_IES        0x0002  /* RECONFIGURE: Ies */
#define AVDTP_ACP_FX_MEDIA_EXPECTED   0x0004  /* the receiver will open the media channel */
#define AVDTP_ACP_FX_STORE_DELAY      0x0008  /* SinkDelayTenthsMs = DelayTenthsMs */
#define AVDTP_ACP_FX_DELAY_CALLBACK   0x0010
#define AVDTP_ACP_FX_MID_STATE        0x0020  /* after the response */
#define AVDTP_ACP_FX_DISCONNECT_MEDIA 0x0040  /* after MidState */
#define AVDTP_ACP_FX_NEW_STATE        0x0080  /* last */
#define AVDTP_ACP_FX_REMOTE_SUSPEND   0x0100  /* the receiver suspended the stream */
#define AVDTP_ACP_FX_RELEASE_REMOTE   0x0200  /* the receiver closed/aborted: its SEP is free again */
#define AVDTP_ACP_FX_RESET_DELAY      0x0400  /* a new configuration invalidates the last delay report */
#define AVDTP_ACP_FX_REMOTE_START     0x0800  /* the receiver started the stream */
#define AVDTP_ACP_FX_OPEN_COLLISION   0x1000  /* the receiver's OPEN was refused because ours is outstanding */

typedef struct _AVDTP_ACP_RESULT {
    UCHAR              MessageType;   /* AVDTP_MSG_RESPONSE_ACCEPT / _REJECT / AVDTP_MSG_GENERAL_REJECT */
    ULONG              ResponseLength;
    UCHAR              Response[AVDTP_MAX_SIGNAL_PAYLOAD];
    ULONG              Effects;       /* AVDTP_ACP_FX_* */
    AVDTP_STREAM_STATE MidState;
    AVDTP_STREAM_STATE NewState;
    UCHAR              ConfigLocalSeid;
    UCHAR              ConfigRemoteSeid;
    UCHAR              Ies[32];
    UCHAR              IesLength;
    ULONG              ServiceCaps;
    BOOLEAN            DelayReporting;
    ULONG              DelayTenthsMs;
} AVDTP_ACP_RESULT;

VOID AvdtpAcpProcess(_In_ const AVDTP_ACP_CONTEXT *Ctx, _In_ UCHAR SignalId, _In_reads_bytes_(Len) const UCHAR *Payload,
                     _In_ ULONG Len, _Out_ AVDTP_ACP_RESULT *Out);
