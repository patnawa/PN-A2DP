/* Bounded diagnostic history, readable without a kernel debugger. */
// SPDX-License-Identifier: MIT
#include "pna2dp.h"

static KSPIN_LOCK g_TraceLock;
static PNA2DP_TRACE_ENTRY g_Trace[PNA2DP_TRACE_ENTRIES];
static ULONG g_TraceNext, g_TraceCount, g_TraceSequence;
static BOOLEAN g_TraceReady;

VOID PnTraceInitialize()
{
    KeInitializeSpinLock(&g_TraceLock);
    g_TraceReady = TRUE;
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
    KIRQL irql;
    KeAcquireSpinLock(&g_TraceLock, &irql);
    entry.Sequence = ++g_TraceSequence;
    g_Trace[g_TraceNext] = entry;
    g_TraceNext = (g_TraceNext + 1) % PNA2DP_TRACE_ENTRIES;
    if (g_TraceCount < PNA2DP_TRACE_ENTRIES) ++g_TraceCount;
    KeReleaseSpinLock(&g_TraceLock, irql);
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
