/*
 * device.h - per A2DP device context: ties the Bluetooth link, AVDTP session, pump,
 * codec host, configuration and the audio endpoint together and owns the connection
 * state machine.
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

typedef enum _PN_AUDIO_REQUEST {
    PnAudioStop = 0,
    PnAudioPause,
    PnAudioRun
} PN_AUDIO_REQUEST;

typedef struct _PN_EVENT_WAITER {
    IO_CSQ      Csq;
    KSPIN_LOCK  Lock;
    LIST_ENTRY  Queue;
} PN_EVENT_WAITER;

typedef struct PNA2DP_DEVICE {
    ULONG               Signature;        /* 'PnA2' */
    PDEVICE_OBJECT      Fdo;
    PDEVICE_OBJECT      Pdo;
    PDRIVER_OBJECT      DriverObject;
    WDFDEVICE           WdfDevice;
    CPnAdapterCommon   *Adapter;          /* PortCls adapter object (owns us) */

    PN_BTH_LINK         Link;
    AVDTP_SESSION       Avdtp;
    PN_PUMP             Pump;
    PN_CODEC_HOST       Host;

    FAST_MUTEX          StateMutex;       /* serializes connect/configure/start/stop */
    KSPIN_LOCK          StatusLock;
    PNA2DP_CONFIG       Config;
    PNA2DP_CAPABILITIES Caps;
    PNA2DP_STATUS       Status;
    A2DP_CODEC_CONFIG   Active;           /* negotiated codec configuration */
    BOOLEAN             ActiveValid;

    /* audio endpoint state */
    PN_AUDIO_REQUEST    AudioRequest;     /* what the audio engine wants */
    ULONG               AudioRate;
    ULONG               AudioChannels;
    ULONG               AudioBits;
    ULONG               AudioValidBits;
    BOOLEAN             AudioFloat;
    BOOLEAN             AudioStreamOpen;
    LONG                VolumeDb16;
    BOOLEAN             Muted;

    /* work items */
    PIO_WORKITEM        ConnectWorkItem;
    LONG                ConnectQueued;
    PIO_WORKITEM        AudioWorkItem;
    LONG                AudioQueued;
    PIO_WORKITEM        ScoWorkItem;
    LONG                ScoQueued;
    KTIMER              SuspendTimer;      /* delayed SUSPEND after pause */
    KDPC                SuspendDpc;
    KTIMER              ReconnectTimer;
    KDPC                ReconnectDpc;
    ULONG               ReconnectAttempts;
    LONG                Removing;

    /* event waiters (IOCTL_PNA2DP_WAIT_EVENT) */
    PN_EVENT_WAITER     Waiters;
    ULONG               PendingEvents;

    /* control interface */
    UNICODE_STRING      InterfaceSymlink;
    BOOLEAN             InterfaceRegistered;
    LONG                OpenHandles;

    LIST_ENTRY          GlobalLink;
} PNA2DP_DEVICE;

#define PNA2DP_DEVICE_SIGNATURE  0x32416E50u  /* 'PnA2' */

/* Lifecycle (PASSIVE) */
NTSTATUS DeviceCreate(_In_ PDEVICE_OBJECT Fdo, _In_ PDEVICE_OBJECT Pdo, _In_ PDRIVER_OBJECT DriverObject,
                      _In_ CPnAdapterCommon *Adapter, _Out_ PPNA2DP_DEVICE *Device);
NTSTATUS DeviceStart(_Inout_ PPNA2DP_DEVICE D);
VOID     DeviceStop(_Inout_ PPNA2DP_DEVICE D);
VOID     DeviceDestroy(_Inout_ PPNA2DP_DEVICE D);
PPNA2DP_DEVICE DeviceFromFdo(_In_ PDEVICE_OBJECT Fdo);

/* Audio endpoint -> device */
VOID     DeviceAudioStreamOpened(_Inout_ PPNA2DP_DEVICE D, _In_ ULONG Rate, _In_ ULONG Channels,
                                 _In_ ULONG ContainerBits, _In_ ULONG ValidBits, _In_ BOOLEAN IsFloat);
VOID     DeviceAudioStreamClosed(_Inout_ PPNA2DP_DEVICE D);
VOID     DeviceAudioStateChanged(_Inout_ PPNA2DP_DEVICE D, _In_ PN_AUDIO_REQUEST Request);   /* DISPATCH ok */
VOID     DeviceAudioVolumeChanged(_Inout_ PPNA2DP_DEVICE D, _In_ LONG VolumeDb16, _In_ BOOLEAN Muted);

/* Control interface -> device */
NTSTATUS DeviceGetStatus(_In_ PPNA2DP_DEVICE D, _Out_ PPNA2DP_STATUS Status);
NTSTATUS DeviceGetCapabilities(_In_ PPNA2DP_DEVICE D, _Out_ PPNA2DP_CAPABILITIES Caps);
NTSTATUS DeviceGetConfig(_In_ PPNA2DP_DEVICE D, _Out_ PPNA2DP_CONFIG Cfg);
NTSTATUS DeviceSetConfig(_Inout_ PPNA2DP_DEVICE D, _In_ const PNA2DP_CONFIG *Cfg);   /* PASSIVE */
NTSTATUS DeviceReconnect(_Inout_ PPNA2DP_DEVICE D);                                    /* PASSIVE */
VOID     DeviceNotifyEvent(_Inout_ PPNA2DP_DEVICE D, _In_ ULONG EventMask);           /* any IRQL <= DISPATCH */
NTSTATUS DeviceQueueWaitEvent(_Inout_ PPNA2DP_DEVICE D, _In_ PIRP Irp);
VOID     DeviceFlushWaitersForFile(_Inout_ PPNA2DP_DEVICE D, _In_ PFILE_OBJECT File);
VOID     DeviceHostChanged(_Inout_ PPNA2DP_DEVICE D);                                 /* codec host (un)registered */

/* Scheduling */
VOID     DeviceQueueConnect(_Inout_ PPNA2DP_DEVICE D);
