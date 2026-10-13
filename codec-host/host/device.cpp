/*
 * device.cpp - per-device worker: pends GET_WORK requests on the driver, encodes
 * PCM blocks with the right plugin and returns the payload with PUT_RESULT.
 * SPDX-License-Identifier: MIT
 */
#include "host.h"
#include <stdio.h>
#include <avrt.h>

#pragma comment(lib, "avrt.lib")

#define WORK_BUFFER_BYTES   (sizeof(PNA2DP_CODEC_WORK) + 65536)
#define INFLIGHT_REQUESTS   4

DeviceWorker::DeviceWorker(const std::wstring &path, PluginRegistry &plugins) : m_path(path), m_plugins(plugins) {}
DeviceWorker::~DeviceWorker() { Stop(); }

bool DeviceWorker::Start()
{
    m_device = CreateFileW(m_path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (m_device == INVALID_HANDLE_VALUE) { HostLog("open %ls failed %lu", m_path.c_str(), GetLastError()); return false; }

    PNA2DP_CODEC_REGISTER reg = {};
    reg.Size = sizeof(reg);
    reg.AbiVersion = PNA2DP_ABI_VERSION;
    reg.CodecMask = m_plugins.CodecMask();
    reg.MaxPcmBytes = 65536;
    reg.MaxEncodedBytes = 8192;
    reg.HostPid = GetCurrentProcessId();
    DWORD ret = 0;
    OVERLAPPED ov = {};
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    BOOL ok = DeviceIoControl(m_device, IOCTL_PNA2DP_CODEC_REGISTER, &reg, sizeof(reg), NULL, 0, &ret, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(m_device, &ov, &ret, TRUE);
    CloseHandle(ov.hEvent);
    if (!ok) {
        DWORD e = GetLastError();
        HostLog("register failed %lu", e);
        /* the driver accepts a codec host only from LocalSystem: the service, not a console */
        if (e == ERROR_ACCESS_DENIED)
            HostLog("codec registration refused: the codec host must run as LocalSystem (the PN-A2DP Codec Host service); "
                    "until then the driver keeps using its built-in SBC encoder");
        CloseHandle(m_device); m_device = INVALID_HANDLE_VALUE; return false;
    }
    HostLog("registered with %ls (codecs 0x%08x)", m_path.c_str(), reg.CodecMask);
    m_stopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    m_resultEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!m_stopEvent || !m_resultEvent) {
        if (m_stopEvent) { CloseHandle(m_stopEvent); m_stopEvent = NULL; }
        if (m_resultEvent) { CloseHandle(m_resultEvent); m_resultEvent = NULL; }
        CloseHandle(m_device); m_device = INVALID_HANDLE_VALUE; return false;
    }
    m_stop = false;
    m_alive = true;
    m_thread = std::thread(&DeviceWorker::Run, this);
    return true;
}

void DeviceWorker::Stop()
{
    m_stop = true;
    if (m_stopEvent) SetEvent(m_stopEvent);
    if (m_device != INVALID_HANDLE_VALUE) CancelIoEx(m_device, NULL);
    if (m_thread.joinable()) m_thread.join();
    CloseEncoder();
    if (m_device != INVALID_HANDLE_VALUE) {
        DWORD ret = 0;
        OVERLAPPED ov = {};
        ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        if (!DeviceIoControl(m_device, IOCTL_PNA2DP_CODEC_UNREGISTER, NULL, 0, NULL, 0, &ret, &ov) && GetLastError() == ERROR_IO_PENDING) GetOverlappedResult(m_device, &ov, &ret, TRUE);
        CloseHandle(ov.hEvent);
        CloseHandle(m_device);
        m_device = INVALID_HANDLE_VALUE;
    }
    m_alive = false;
    if (m_stopEvent) { CloseHandle(m_stopEvent); m_stopEvent = NULL; }
    if (m_resultEvent) { CloseHandle(m_resultEvent); m_resultEvent = NULL; }
}

void DeviceWorker::Run()
{
    /* The driver's watchdog writes off a block after 200 ms: encode at audio priority.
     * MMCSS "Pro Audio" is preferred; failing that, a raised thread priority. */
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcss) {
        if (!AvSetMmThreadPriority(mmcss, AVRT_PRIORITY_HIGH)) HostLog("MMCSS priority not raised (%lu)", GetLastError());
        HostLog("encoder thread registered with MMCSS Pro Audio (task %lu)", taskIndex);
    } else {
        DWORD e = GetLastError();
        BOOL raised = SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        HostLog("MMCSS registration failed (%lu); thread priority %s", e, raised ? "time critical" : "unchanged");
    }

    /* Keep several GET_WORK requests pended so the driver never waits for a buffer. */
    struct Slot { OVERLAPPED ov; std::vector<uint8_t> buf; bool pending; };
    Slot slots[INFLIGHT_REQUESTS];
    for (int i = 0; i < INFLIGHT_REQUESTS; i++) {
        memset(&slots[i].ov, 0, sizeof(OVERLAPPED));
        slots[i].ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
        slots[i].buf.resize(WORK_BUFFER_BYTES);
        slots[i].pending = false;
        if (!slots[i].ov.hEvent) m_stop = true;
    }
    auto submit = [&](Slot &s) {
        if (m_stop) return false;
        ResetEvent(s.ov.hEvent);
        DWORD ret = 0;
        BOOL ok = DeviceIoControl(m_device, IOCTL_PNA2DP_CODEC_GET_WORK, NULL, 0, s.buf.data(), (DWORD)s.buf.size(), &ret, &s.ov);
        if (!ok && GetLastError() != ERROR_IO_PENDING) { HostLog("GET_WORK submit failed %lu", GetLastError()); return false; }
        s.pending = true;
        return true;
    };
    for (int i = 0; i < INFLIGHT_REQUESTS; i++) if (!submit(slots[i])) { m_stop = true; break; }

    // GET_WORK is dequeued FIFO by the driver. Waiting on all slot events chooses the
    // lowest index, not the oldest request: reusing slot 0 can jump ahead of slots 1-3.
    // Always retire the oldest submitted slot, including OPEN/CLOSE control messages.
    unsigned next = 0;
    while (!m_stop) {
        Slot &s = slots[next];
        HANDLE events[] = {m_stopEvent, s.ov.hEvent};
        DWORD w = WaitForMultipleObjects(2, events, FALSE, 1000);
        if (w == WAIT_TIMEOUT) continue;
        if (w != WAIT_OBJECT_0 + 1 || m_stop) break;
        next = (next + 1) % INFLIGHT_REQUESTS;
        DWORD got = 0;
        BOOL ok = GetOverlappedResult(m_device, &s.ov, &got, FALSE);
        s.pending = false;
        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_OPERATION_ABORTED || e == ERROR_DEVICE_NOT_CONNECTED || e == ERROR_DEV_NOT_EXIST) { HostLog("device gone (%lu)", e); break; }
            HostLog("GET_WORK failed %lu", e);
            Sleep(50);
            if (!submit(s)) break;
            continue;
        }
        if (got >= sizeof(PNA2DP_CODEC_WORK)) {
            const PNA2DP_CODEC_WORK *work = (const PNA2DP_CODEC_WORK *)s.buf.data();
            if (work->Size != got || work->PcmBytes > got - sizeof(*work) ||
                work->Params.CodecInfoLength > sizeof(work->Params.CodecInfo)) {
                HostLog("invalid work buffer (%lu bytes)", got);
                break;
            }
            HandleWork(work, s.buf.data() + sizeof(PNA2DP_CODEC_WORK));
        }
        /* Re-pend this slot before answering: the driver is woken by the result and must
         * find a request for its next block. The work buffer is no longer needed. */
        bool resubmitted = submit(s);
        if (m_resultReady) SendResult();
        if (!resubmitted) break;
    }
    for (int i = 0; i < INFLIGHT_REQUESTS; i++) {
        if (slots[i].pending) { CancelIoEx(m_device, &slots[i].ov); DWORD r; GetOverlappedResult(m_device, &slots[i].ov, &r, TRUE); }
        CloseHandle(slots[i].ov.hEvent);
    }
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    m_alive = false;
}

void DeviceWorker::HandleWork(const PNA2DP_CODEC_WORK *w, const uint8_t *pcm)
{
    switch (w->Kind) {
    case PNA2DP_WORK_OPEN: Open(w); break;
    case PNA2DP_WORK_ENCODE: Encode(w, pcm); break;
    case PNA2DP_WORK_RECONFIG:
        if (m_encoder && m_plugin->reconfigure) m_plugin->reconfigure(m_encoder, w->Params.QualityIndex, w->Params.BitrateBps);
        break;
    case PNA2DP_WORK_FLUSH:
        if (m_encoder && m_plugin->flush) m_plugin->flush(m_encoder);
        break;
    case PNA2DP_WORK_CLOSE: CloseEncoder(); break;
    default: break;
    }
}

void DeviceWorker::Open(const PNA2DP_CODEC_WORK *w)
{
    CloseEncoder();
    m_params = w->Params;
    m_generation = w->StreamGeneration;
    m_plugin = m_plugins.ForCodec(m_params.Codec);
    if (!m_plugin) { HostLog("no plugin for codec %u", m_params.Codec); return; }
    m_encoder = m_plugin->open(&m_params);
    if (!m_encoder) { HostLog("%s: open failed for %u Hz ch %u fmt %u", m_plugin->name, m_params.SampleRate, m_params.Channels, m_params.PcmFormat); return; }
    uint32_t cap = m_plugin->max_output_bytes ? m_plugin->max_output_bytes(&m_params) : 0;
    if (cap < 4096) cap = 4096;
    m_outBuf.resize(cap);
    m_resultBuf.resize(sizeof(PNA2DP_CODEC_RESULT) + cap);
    m_lastQuality = m_params.QualityIndex;
    m_lastBitrate = m_params.BitrateBps;
    m_rejectedValid = false;
    HostLog("%s: encoder open, %u Hz, %u ch, pcm fmt %u, target %u bps, mtu %u", m_plugin->name, m_params.SampleRate, m_params.Channels, m_params.PcmFormat,
            m_params.BitrateBps, m_params.MediaMtu);
}

void DeviceWorker::Encode(const PNA2DP_CODEC_WORK *w, const uint8_t *pcm)
{
    if (!m_encoder || w->StreamGeneration != m_generation) {
        /* the driver may send ENCODE before OPEN arrives after a quick restart: try to open on the fly */
        if (!m_encoder || w->StreamGeneration != m_generation) { Open(w); if (!m_encoder) { PrepareResult(w->Sequence, w->StreamGeneration, -1, nullptr); return; } }
    }
    if (w->Params.QualityIndex != m_lastQuality || w->Params.BitrateBps != m_lastBitrate) {
        /* A refused quality change keeps the current quality: the block is still encoded
         * (failing it would cost audio and count against the host). Retry only when the
         * driver asks for something different. */
        bool retry = !m_rejectedValid || w->Params.QualityIndex != m_rejectedQuality || w->Params.BitrateBps != m_rejectedBitrate;
        if (retry) {
            if (m_plugin->reconfigure && m_plugin->reconfigure(m_encoder, w->Params.QualityIndex, w->Params.BitrateBps) == 0) {
                m_lastQuality = w->Params.QualityIndex;
                m_lastBitrate = w->Params.BitrateBps;
                m_rejectedValid = false;
            } else {
                HostLog("%s: quality change to %u / %u bps rejected; keeping %u / %u bps", m_plugin->name,
                        w->Params.QualityIndex, w->Params.BitrateBps, m_lastQuality, m_lastBitrate);
                m_rejectedValid = true;
                m_rejectedQuality = w->Params.QualityIndex;
                m_rejectedBitrate = w->Params.BitrateBps;
            }
        }
    }
    PnCodecOutput out = {};
    out.buffer = m_outBuf.data();
    out.capacity = (uint32_t)m_outBuf.size();
    int rc = m_plugin->encode(m_encoder, pcm, w->PcmBytes, w->PcmFrames, &out);
    if (out.written > out.capacity || out.buffer != m_outBuf.data()) rc = -1;
    PrepareResult(w->Sequence, w->StreamGeneration, rc, rc == 0 ? &out : nullptr);
}

void DeviceWorker::CloseEncoder()
{
    if (m_encoder && m_plugin) m_plugin->close(m_encoder);
    m_encoder = nullptr;
}

/* Builds the PUT_RESULT for the current work item; Run sends it after re-pending the slot. */
void DeviceWorker::PrepareResult(uint32_t seq, uint32_t gen, int status, const PnCodecOutput *out)
{
    uint32_t payload = (out && status == 0) ? out->written : 0;
    if (m_resultBuf.size() < sizeof(PNA2DP_CODEC_RESULT) + payload) m_resultBuf.resize(sizeof(PNA2DP_CODEC_RESULT) + payload);
    PNA2DP_CODEC_RESULT *r = (PNA2DP_CODEC_RESULT *)m_resultBuf.data();
    memset(r, 0, sizeof(*r));
    r->Size = (uint32_t)(sizeof(*r) + payload);
    r->Sequence = seq;
    r->StreamGeneration = gen;
    r->Status = status;
    r->PayloadBytes = payload;
    if (out) {
        r->FrameCount = out->frame_count;
        r->PcmFramesConsumed = out->pcm_frames_consumed;
        r->PayloadHeaderByte = out->payload_header_byte;
        r->ActualBitrateBps = out->actual_bitrate_bps;
        if (payload) memcpy(r + 1, out->buffer, payload);
    }
    m_resultReady = true;
}

void DeviceWorker::SendResult()
{
    m_resultReady = false;
    const PNA2DP_CODEC_RESULT *r = (const PNA2DP_CODEC_RESULT *)m_resultBuf.data();
    DWORD ret = 0;
    OVERLAPPED ov = {};
    ov.hEvent = m_resultEvent;
    ResetEvent(m_resultEvent);
    BOOL ok = DeviceIoControl(m_device, IOCTL_PNA2DP_CODEC_PUT_RESULT, (LPVOID)r, r->Size, NULL, 0, &ret, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(m_device, &ov, &ret, TRUE);
    if (!ok) HostLog("PUT_RESULT failed %lu", GetLastError());
}
