/*
 * sbc_test.c - user-mode self test for the SBC codec shared with the kernel driver.
 *
 * Checks: frame length formula, CRC/header round trip, encode->decode SNR for a
 * multi-tone signal at several configurations, bitpool live change, and the
 * "SBC XQ" style configurations used by the driver.
 *
 * SPDX-License-Identifier: MIT
 */
#include "../sbc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

static int failures = 0;
static const char *vector_dir = NULL;
static int signal_profile = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static double run_roundtrip(sbc_u32 rate, int mode, int alloc, int subbands, int blocks, int bitpool, int verbose)
{
    sbc_params p;
    sbc_encoder *e = (sbc_encoder *)calloc(1, sizeof(*e));
    sbc_decoder *d = (sbc_decoder *)calloc(1, sizeof(*d));
    sbc_u32 nch, pcm_per, ch, i, f;
    sbc_s16 *in, *out, *dec;
    sbc_u8 buf[SBC_MAX_FRAME_LEN];
    double sig = 0, err = 0, snr;
    const sbc_u32 nframes = 200;
    sbc_u32 delay;
    FILE *encoded = NULL, *reference = NULL;
    char path[1024];

    p.sample_rate = rate; p.channel_mode = (sbc_u8)mode; p.allocation = (sbc_u8)alloc;
    p.subbands = (sbc_u8)subbands; p.blocks = (sbc_u8)blocks; p.bitpool = (sbc_u8)bitpool;
    CHECK(sbc_encoder_init(e, &p) == 0, "encoder init %u/%d/%d/%d/%d/%d", rate, mode, alloc, subbands, blocks, bitpool);
    sbc_decoder_init(d);
    nch = (mode == SBC_MODE_MONO) ? 1 : 2;
    pcm_per = sbc_pcm_per_frame(&p);
    in = (sbc_s16 *)calloc((size_t)nframes * pcm_per * nch, sizeof(sbc_s16));
    out = (sbc_s16 *)calloc((size_t)nframes * pcm_per * nch, sizeof(sbc_s16));
    dec = (sbc_s16 *)calloc((size_t)pcm_per * nch, sizeof(sbc_s16));

    for (i = 0; i < nframes * pcm_per; i++) {
        double t = (double)i / rate;
        double v = 0.30 * sin(2 * 3.14159265358979 * 440.0 * t) + 0.15 * sin(2 * 3.14159265358979 * 3100.0 * t) +
                   0.08 * sin(2 * 3.14159265358979 * 9700.0 * t);
        double w = 0.25 * sin(2 * 3.14159265358979 * 660.0 * t) + 0.10 * sin(2 * 3.14159265358979 * 5200.0 * t);
        /* Exercise near-full-scale bass, correlated and opposite-phase stereo,
         * plus loud transients. Low-amplitude midrange tones miss these cases. */
        if (signal_profile) {
            v = 0.99 * sin(2 * 3.14159265358979 * 53.0 * t);
            w = signal_profile == 2 ? -v : v;
            if (signal_profile == 3) {
                v = 0.70 * sin(2 * 3.14159265358979 * 63.0 * t) + 0.29 * sin(2 * 3.14159265358979 * 3011.0 * t);
                w = 0.70 * sin(2 * 3.14159265358979 * 97.0 * t) + 0.29 * sin(2 * 3.14159265358979 * 6971.0 * t);
            }
            if (signal_profile >= 4) {
                v = (i % 997 < 498) ? 1.0 : -1.0;
                w = signal_profile == 5 ? -v : v;
            }
        }
        for (ch = 0; ch < nch; ch++) in[i * nch + ch] = (sbc_s16)((ch == 0 ? v : w) * 32767.0);
    }

    if (vector_dir) {
        snprintf(path, sizeof(path), "%s/%u-%d-%d-%d-%d-%d-p%d.sbc", vector_dir, rate, mode, alloc, subbands, blocks, bitpool, signal_profile);
        encoded = fopen(path, "wb");
        snprintf(path, sizeof(path), "%s/%u-%d-%d-%d-%d-%d-p%d.pcm", vector_dir, rate, mode, alloc, subbands, blocks, bitpool, signal_profile);
        reference = fopen(path, "wb");
        CHECK(encoded && reference, "open interoperability vectors");
        if (reference) { fwrite(in, sizeof(sbc_s16), (size_t)nframes * pcm_per * nch, reference); fclose(reference); }
    }
    for (f = 0; f < nframes; f++) {
        sbc_u32 len = sbc_encode_frame(e, in + (size_t)f * pcm_per * nch, buf, sizeof(buf));
        sbc_u32 consumed, dframes, dch, drate;
        CHECK(len == e->frame_length, "frame length %u != %u", len, e->frame_length);
        CHECK(len == sbc_frame_length(&p), "frame length formula");
        if (encoded) fwrite(buf, 1, len, encoded);
        consumed = sbc_decode_frame(d, buf, len, dec, pcm_per * nch, &dframes, &dch, &drate);
        CHECK(consumed == len, "decode consumed %u != %u (frame %u)", consumed, len, f);
        CHECK(dframes == pcm_per && dch == nch && drate == rate, "decode params");
        memcpy(out + (size_t)f * pcm_per * nch, dec, (size_t)pcm_per * nch * sizeof(sbc_s16));
    }
    if (encoded) fclose(encoded);
    /* The filterbank pair delays the signal by 10*subbands - subbands... measure the delay empirically */
    {
        sbc_u32 best = 0; double beste = 1e100;
        for (delay = 0; delay < 200; delay++) {
            double error = 0;
            for (i = 2000; i < 6000; i++) {
                double difference = (double)in[i * nch] - out[(i + delay) * nch];
                error += difference * difference;
            }
            if (error < beste) { beste = error; best = delay; }
        }
        delay = best;
    }
    for (i = 2000; i < (nframes - 2) * pcm_per - 300; i++) {
        for (ch = 0; ch < nch; ch++) {
            double a = in[i * nch + ch], b = out[(i + delay) * nch + ch];
            sig += a * a; err += (a - b) * (a - b);
        }
    }
    snr = 10.0 * log10(sig / (err + 1e-9));
    if (verbose) printf("  %5u Hz mode %d alloc %d sb %d blk %d bitpool %3d: %6u bps, frame %3u B, delay %3u, SNR %6.2f dB\n",
                        rate, mode, alloc, subbands, blocks, bitpool, sbc_bitrate(&p), sbc_frame_length(&p), delay, snr);
    free(in); free(out); free(dec); free(e); free(d);
    return snr;
}

int main(int argc, char **argv)
{
    sbc_params p;
    double snr;
    if (argc > 1) vector_dir = argv[1];

    /* 1. frame length / bitrate sanity (values from the A2DP spec examples) */
    p.sample_rate = 44100; p.channel_mode = SBC_MODE_JOINT; p.allocation = SBC_ALLOC_LOUDNESS; p.subbands = 8; p.blocks = 16; p.bitpool = 53;
    CHECK(sbc_frame_length(&p) == 119, "joint 44.1k bitpool 53 frame length = %u (expect 119)", sbc_frame_length(&p));
    CHECK(sbc_bitrate(&p) == 327993, "joint 44.1k bitpool 53 bitrate = %u (expect 327993)", sbc_bitrate(&p));
    p.channel_mode = SBC_MODE_DUAL; p.sample_rate = 48000;
    CHECK(sbc_frame_length(&p) == 224, "dual 48k bitpool 53 frame length = %u (expect 224)", sbc_frame_length(&p));
    CHECK(sbc_bitrate(&p) == 672000, "dual 48k bitpool 53 bitrate = %u (expect 672000)", sbc_bitrate(&p));
    p.channel_mode = SBC_MODE_MONO; p.sample_rate = 16000; p.blocks = 15; /* invalid */
    CHECK(sbc_frame_length(&p) == 0, "invalid blocks rejected");

    printf("SBC round trip tests\n");
    snr = run_roundtrip(44100, SBC_MODE_JOINT, SBC_ALLOC_LOUDNESS, 8, 16, 53, 1);  CHECK(snr > 25.0, "SNR joint/53 too low: %.1f", snr);
    snr = run_roundtrip(48000, SBC_MODE_DUAL, SBC_ALLOC_LOUDNESS, 8, 16, 53, 1);   CHECK(snr > 25.0, "SNR dual/53 too low: %.1f", snr);
    snr = run_roundtrip(48000, SBC_MODE_STEREO, SBC_ALLOC_SNR, 8, 16, 76, 1);      CHECK(snr > 35.0, "SNR stereo/76 too low: %.1f", snr);
    snr = run_roundtrip(44100, SBC_MODE_JOINT, SBC_ALLOC_LOUDNESS, 8, 16, 35, 1);  CHECK(snr > 15.0, "SNR joint/35 too low: %.1f", snr);
    snr = run_roundtrip(16000, SBC_MODE_MONO, SBC_ALLOC_LOUDNESS, 8, 15 == 15 ? 16 : 16, 26, 1); CHECK(snr > 15.0, "SNR mono/26 too low: %.1f", snr);
    snr = run_roundtrip(32000, SBC_MODE_STEREO, SBC_ALLOC_LOUDNESS, 4, 8, 40, 1);  CHECK(snr > 15.0, "SNR 4sb/40 too low: %.1f", snr);
    snr = run_roundtrip(48000, SBC_MODE_DUAL, SBC_ALLOC_LOUDNESS, 8, 16, 128, 1);  CHECK(snr > 40.0, "SNR dual/128 too low: %.1f", snr);
    snr = run_roundtrip(48000, SBC_MODE_JOINT, SBC_ALLOC_LOUDNESS, 8, 16, 250, 1); CHECK(snr > 45.0, "SNR joint/250 too low: %.1f", snr);

    for (signal_profile = 1; signal_profile <= 5; ++signal_profile) {
        printf("Loud/bass profile %d\n", signal_profile);
        snr = run_roundtrip(48000, SBC_MODE_JOINT, SBC_ALLOC_LOUDNESS, 8, 16, 53, 1);
        CHECK(snr > 45.0, "loud/bass SNR too low: %.1f", snr);
    }
    signal_profile = 0;

    /* 2. live bitpool change keeps decoding */
    {
        sbc_encoder *e = (sbc_encoder *)calloc(1, sizeof(*e));
        sbc_decoder *d = (sbc_decoder *)calloc(1, sizeof(*d));
        sbc_s16 pcm[128 * 2]; sbc_s16 dec[128 * 2]; sbc_u8 buf[600];
        sbc_u32 i, fr, ch, rate;
        p.sample_rate = 44100; p.channel_mode = SBC_MODE_JOINT; p.allocation = SBC_ALLOC_LOUDNESS; p.subbands = 8; p.blocks = 16; p.bitpool = 53;
        sbc_encoder_init(e, &p); sbc_decoder_init(d);
        for (i = 0; i < 256; i++) pcm[i] = (sbc_s16)(10000.0 * sin(i * 0.1));
        CHECK(sbc_encode_frame(e, pcm, buf, sizeof(buf)) == 119, "bitpool 53 frame");
        CHECK(sbc_decode_frame(d, buf, 119, dec, 256, &fr, &ch, &rate) == 119, "decode 53");
        sbc_encoder_set_bitpool(e, 35);
        CHECK(e->frame_length == 83, "bitpool 35 frame length %u (expect 83)", e->frame_length);
        CHECK(sbc_encode_frame(e, pcm, buf, sizeof(buf)) == 83, "bitpool 35 frame");
        CHECK(sbc_decode_frame(d, buf, 83, dec, 256, &fr, &ch, &rate) == 83, "decode 35 after change");
        CHECK(buf[2] == 35, "header bitpool byte");
        free(e); free(d);
    }

    /* 3. max bitpool for MTU */
    p.sample_rate = 48000; p.channel_mode = SBC_MODE_DUAL; p.allocation = 0; p.subbands = 8; p.blocks = 16; p.bitpool = 250;
    CHECK(sbc_max_bitpool_for(&p, 0, 0) == 128, "unconstrained max bitpool (dual) %u", sbc_max_bitpool_for(&p, 0, 0));
    CHECK(sbc_max_bitpool_for(&p, 345000, 0) <= 53, "bitrate constrained bitpool %u", sbc_max_bitpool_for(&p, 345000, 0));

    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
