/* Diagnostics: bounded trace ring (readable without a kernel debugger), debugger output,
 * the TraceLogging (ETW) provider and System event-log entries. */
// SPDX-License-Identifier: MIT
#include "pna2dp.h"
#include <TraceLoggingProvider.h>

/* ETW event levels (evntrace.h TRACE_LEVEL_*, not reachable from the kernel include set). */
#define PN_ETW_LEVEL_ERROR       2
#define PN_ETW_LEVEL_WARNING     3
#define PN_ETW_LEVEL_INFORMATION 4
#define PN_ETW_LEVEL_VERBOSE     5

/* Provider "PnA2dp.Driver"; the GUID is the standard ETW hash of that name, so tools can
 * also enable it by name (for example: tracelog -guid *PnA2dp.Driver, or a WPR profile).
 * {7015a1ac-0944-5322-6528-aac0e90c3960} */
TRACELOGGING_DEFINE_PROVIDER(g_PnTraceProvider, "PnA2dp.Driver",
    (0x7015a1ac, 0x0944, 0x5322, 0x65, 0x28, 0xaa, 0xc0, 0xe9, 0x0c, 0x39, 0x60));

static KSPIN_LOCK g_TraceLock;
static PNA2DP_TRACE_ENTRY g_Trace[PNA2DP_TRACE_ENTRIES];
static ULONG g_TraceNext, g_TraceCount, g_TraceSequence;
static BOOLEAN g_TraceReady, g_TraceProviderRegistered;
static PDRIVER_OBJECT g_TraceDriver;

VOID PnTraceInitialize(_In_ PDRIVER_OBJECT DriverObject)
{
    KeInitializeSpinLock(&g_TraceLock);
    g_TraceDriver = DriverObject;
    g_TraceReady = TRUE;
    /* Tracing works without ETW; a registration failure only loses the ETW copy. */
    g_TraceProviderRegistered = NT_SUCCESS(TraceLoggingRegister(g_PnTraceProvider));
}

VOID PnTraceUninitialize(void)
{
    if (g_TraceProviderRegistered) {
        g_TraceProviderRegistered = FALSE;
        TraceLoggingUnregister(g_PnTraceProvider);
    }
    g_TraceDriver = NULL;
}

/* PN_LEVEL_ERROR..PN_LEVEL_VERBOSE map to PN_ETW_LEVEL_ERROR..PN_ETW_LEVEL_VERBOSE (level + 1). */
BOOLEAN PnTraceEtwEnabled(_In_ ULONG Level)
{
    return g_TraceProviderRegistered && TraceLoggingProviderEnabled(g_PnTraceProvider, (UCHAR)(Level + 1), 0);
}

static ULONG PnDbgPrintLevel(_In_ ULONG Level)
{
    switch (Level) {
    case PN_LEVEL_ERROR: return DPFLTR_ERROR_LEVEL;
    case PN_LEVEL_WARN:  return DPFLTR_WARNING_LEVEL;
    case PN_LEVEL_INFO:  return DPFLTR_INFO_LEVEL;
    default:             return DPFLTR_TRACE_LEVEL;
    }
}

VOID PnTraceWrite(ULONG Level, PCSTR Format, ...)
{
    if (!g_TraceReady) return;
    PNA2DP_TRACE_ENTRY entry = {};
    entry.TimeHns = KeQueryInterruptTime();
    entry.Level = Level;
    va_list args;
    va_start(args, Format);
    RtlStringCbVPrintfA(entry.Text, sizeof(entry.Text), Format, args);
    va_end(args);

    /* The ring keeps information messages even when DebugLevel only asks the debugger for
     * warnings; verbose messages enter it only when DebugLevel asks for them. */
    if (Level <= PN_MAX(g_PnDebugLevel, (ULONG)PN_LEVEL_INFO)) {
        KIRQL irql;
        KeAcquireSpinLock(&g_TraceLock, &irql);
        entry.Sequence = ++g_TraceSequence;
        g_Trace[g_TraceNext] = entry;
        g_TraceNext = (g_TraceNext + 1) % PNA2DP_TRACE_ENTRIES;
        if (g_TraceCount < PNA2DP_TRACE_ENTRIES) ++g_TraceCount;
        KeReleaseSpinLock(&g_TraceLock, irql);
    }

    if (g_PnDebugLevel >= Level) DbgPrintEx(DPFLTR_IHVAUDIO_ID, PnDbgPrintLevel(Level), "pna2dp: %s\n", entry.Text);

    /* TraceLogging levels must be compile-time constants. */
    switch (Level) {
    case PN_LEVEL_ERROR:
        TraceLoggingWrite(g_PnTraceProvider, "Trace", TraceLoggingLevel(PN_ETW_LEVEL_ERROR), TraceLoggingString(entry.Text, "Message"));
        break;
    case PN_LEVEL_WARN:
        TraceLoggingWrite(g_PnTraceProvider, "Trace", TraceLoggingLevel(PN_ETW_LEVEL_WARNING), TraceLoggingString(entry.Text, "Message"));
        break;
    case PN_LEVEL_INFO:
        TraceLoggingWrite(g_PnTraceProvider, "Trace", TraceLoggingLevel(PN_ETW_LEVEL_INFORMATION), TraceLoggingString(entry.Text, "Message"));
        break;
    default:
        TraceLoggingWrite(g_PnTraceProvider, "Trace", TraceLoggingLevel(PN_ETW_LEVEL_VERBOSE), TraceLoggingString(entry.Text, "Message"));
        break;
    }
}

VOID PnTraceRead(PNA2DP_TRACE *Trace)
{
    RtlZeroMemory(Trace, sizeof(*Trace));
    Trace->Size = sizeof(*Trace);
    KIRQL irql;
    KeAcquireSpinLock(&g_TraceLock, &irql);
    Trace->Count = g_TraceCount;
    ULONG first = (g_TraceNext + PNA2DP_TRACE_ENTRIES - g_TraceCount) % PNA2DP_TRACE_ENTRIES;
    for (ULONG i = 0; i < g_TraceCount; ++i) Trace->Entries[i] = g_Trace[(first + i) % PNA2DP_TRACE_ENTRIES];
    KeReleaseSpinLock(&g_TraceLock, irql);
}

/* Contract C9. IoAllocateErrorLogEntry/IoWriteErrorLogEntry run at <= DISPATCH_LEVEL; the
 * I/O manager references the object until the entry is written, so a driver object may
 * be used from a failing DriverEntry. */
VOID PnLogDeviceEvent(_In_opt_ PDEVICE_OBJECT DeviceObject, _In_ NTSTATUS Code, _In_ ULONG UniqueValue)
{
    PVOID source = DeviceObject ? (PVOID)DeviceObject : (PVOID)g_TraceDriver;
    if (!source) return;
    PIO_ERROR_LOG_PACKET entry = (PIO_ERROR_LOG_PACKET)IoAllocateErrorLogEntry(source, (UCHAR)sizeof(IO_ERROR_LOG_PACKET));
    if (!entry) return;
    RtlZeroMemory(entry, sizeof(IO_ERROR_LOG_PACKET));
    entry->ErrorCode = Code;
    entry->UniqueErrorValue = UniqueValue;
    entry->DumpDataSize = sizeof(ULONG);
    entry->DumpData[0] = UniqueValue;
    IoWriteErrorLogEntry(entry);
    TraceLoggingWrite(g_PnTraceProvider, "SystemEvent", TraceLoggingLevel(PN_ETW_LEVEL_WARNING),
        TraceLoggingHexInt32(Code, "Code"), TraceLoggingHexUInt32(UniqueValue, "UniqueValue"));
}
