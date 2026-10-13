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
static int input_s32 = 0;   /* encode 24-bit samples through sbc_encode_frame_s32 */
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static double run_roundtrip(sbc_u32 rate, int mode, int alloc, int subbands, int blocks, int bitpool, int verbose)
{
    sbc_params p;
    sbc_encoder *e = (sbc_encoder *)calloc(1, sizeof(*e));
    sbc_encoder *e32 = (sbc_encoder *)calloc(1, sizeof(*e32));
    sbc_decoder *d = (sbc_decoder *)calloc(1, sizeof(*d));
    sbc_u32 nch, pcm_per, ch, i, f;
    sbc_s16 *in, *out, *dec;
    sbc_s32 *in32;
    sbc_u8 buf[SBC_MAX_FRAME_LEN], buf32[SBC_MAX_FRAME_LEN];
    double sig = 0, err = 0, snr;
    const sbc_u32 nframes = 200;
    sbc_u32 delay;
    FILE *encoded = NULL, *reference = NULL;
    char path[1024];

    p.sample_rate = rate; p.channel_mode = (sbc_u8)mode; p.allocation = (sbc_u8)alloc;
    p.subbands = (sbc_u8)subbands; p.blocks = (sbc_u8)blocks; p.bitpool = (sbc_u8)bitpool;
    CHECK(sbc_encoder_init(e, &p) == 0, "encoder init %u/%d/%d/%d/%d/%d", rate, mode, alloc, subbands, blocks, bitpool);
    sbc_encoder_init(e32, &p);
    sbc_decoder_init(d);
    nch = (mode == SBC_MODE_MONO) ? 1 : 2;
    pcm_per = sbc_pcm_per_frame(&p);
    in = (sbc_s16 *)calloc((size_t)nframes * pcm_per * nch, sizeof(sbc_s16));
    out = (sbc_s16 *)calloc((size_t)nframes * pcm_per * nch, sizeof(sbc_s16));
    dec = (sbc_s16 *)calloc((size_t)pcm_per * nch, sizeof(sbc_s16));
    in32 = (sbc_s32 *)calloc((size_t)nframes * pcm_per * nch, sizeof(sbc_s32));

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
        for (ch = 0; ch < nch; ch++) {
            in[i * nch + ch] = (sbc_s16)((ch == 0 ? v : w) * 32767.0);
            /* 24-bit source in an S32 container; otherwise the S16 samples << 16 */
            in32[i * nch + ch] = input_s32 ? (sbc_s32)floor((ch == 0 ? v : w) * 8388607.0 + 0.5) * 256 : (sbc_s32)in[i * nch + ch] * 65536;
        }
    }

    if (vector_dir) {
        const char *tag = input_s32 ? "-s32" : "";
        snprintf(path, sizeof(path), "%s/%u-%d-%d-%d-%d-%d-p%d%s.sbc", vector_dir, rate, mode, alloc, subbands, blocks, bitpool, signal_profile, tag);
        encoded = fopen(path, "wb");
        snprintf(path, sizeof(path), "%s/%u-%d-%d-%d-%d-%d-p%d%s.pcm", vector_dir, rate, mode, alloc, subbands, blocks, bitpool, signal_profile, tag);
        reference = fopen(path, "wb");
        CHECK(encoded && reference, "open interoperability vectors");
        if (reference) { fwrite(in, sizeof(sbc_s16), (size_t)nframes * pcm_per * nch, reference); fclose(reference); }
    }
    for (f = 0; f < nframes; f++) {
        sbc_u32 len32 = sbc_encode_frame_s32(e32, in32 + (size_t)f * pcm_per * nch, buf32, sizeof(buf32));
        sbc_u32 len = input_s32 ? len32 : sbc_encode_frame(e, in + (size_t)f * pcm_per * nch, buf, sizeof(buf));
        sbc_u32 consumed, dframes, dch, drate;
        if (input_s32) memcpy(buf, buf32, len32);
        /* the S32 entry point is bit-exact for 16-bit input */
        else CHECK(len32 == len && memcmp(buf, buf32, len) == 0, "S32 input (S16 << 16) differs from S16 input in frame %u", f);
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
    free(in); free(in32); free(out); free(dec); free(e); free(e32); free(d);
    return snr;
}

/* SNR of a quiet 24-bit two-tone signal (about -70 dBFS) against the exact signal,
 * encoded from a 24-bit S32 source or from the same source rounded to 16 bits first.
 * The decoder output is S16, so the 24-bit path can at most remove the input rounding. */
static double quiet_tone_snr(int use_s32, int mode, int bitpool)
{
    sbc_params p;
    sbc_encoder *e = (sbc_encoder *)calloc(1, sizeof(*e));
    sbc_decoder *d = (sbc_decoder *)calloc(1, sizeof(*d));
    const sbc_u32 nframes = 300, pcm_per = 128, nch = 2;
    double *ref = (double *)calloc((size_t)nframes * pcm_per, sizeof(double));
    sbc_s16 *out = (sbc_s16 *)calloc((size_t)nframes * pcm_per * nch, sizeof(sbc_s16));
    sbc_s32 pcm32[128 * 2]; sbc_s16 pcm16[128 * 2]; sbc_s16 dec[128 * 2];
    sbc_u8 buf[SBC_MAX_FRAME_LEN];
    sbc_u32 i, f, fr, ch, rate, best = 0;
    double beste = 1e300, sig = 0, err = 0;
    p.sample_rate = 48000; p.channel_mode = (sbc_u8)mode; p.allocation = SBC_ALLOC_LOUDNESS; p.subbands = 8; p.blocks = 16; p.bitpool = (sbc_u8)bitpool;
    sbc_encoder_init(e, &p); sbc_decoder_init(d);
    for (i = 0; i < nframes * pcm_per; i++)
        ref[i] = 10.3 * sin(2 * 3.14159265358979 * 997.0 * i / 48000.0) + 3.1 * sin(2 * 3.14159265358979 * 3001.0 * i / 48000.0);
    for (f = 0; f < nframes; f++) {
        sbc_u32 len;
        for (i = 0; i < pcm_per; i++) {
            double v = ref[f * pcm_per + i];
            pcm32[i * 2] = pcm32[i * 2 + 1] = (sbc_s32)floor(v * 256.0 + 0.5) * 256;   /* 24-bit */
            pcm16[i * 2] = pcm16[i * 2 + 1] = (sbc_s16)floor(v + 0.5);
        }
        len = use_s32 ? sbc_encode_frame_s32(e, pcm32, buf, sizeof(buf)) : sbc_encode_frame(e, pcm16, buf, sizeof(buf));
        CHECK(len && sbc_decode_frame(d, buf, len, dec, 256, &fr, &ch, &rate) == len, "quiet tone decode");
        memcpy(out + (size_t)f * pcm_per * nch, dec, sizeof(dec));
    }
    for (i = 0; i < 200; i++) {
        double e2 = 0; sbc_u32 j;
        for (j = 2000; j < 6000; j++) { double t = ref[j] - out[(j + i) * nch]; e2 += t * t; }
        if (e2 < beste) { beste = e2; best = i; }
    }
    for (i = 2000; i < (nframes - 4) * pcm_per; i++) {
        double t = ref[i] - out[(i + best) * nch];
        sig += ref[i] * ref[i]; err += t * t;
    }
    free(e); free(d); free(ref); free(out);
    return 10.0 * log10(sig / (err + 1e-12));
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

    /* SBC XQ points (dual channel, 8 subbands, 16 blocks): 4 frames per 2-DH5, 5 per 3-DH5 */
    printf("SBC XQ\n");
    p.sample_rate = 44100; p.channel_mode = SBC_MODE_DUAL; p.allocation = SBC_ALLOC_LOUDNESS; p.subbands = 8; p.blocks = 16; p.bitpool = 38;
    CHECK(sbc_frame_length(&p) == 164 && 4 * 164 + 13 <= 679 - 4 && sbc_bitrate(&p) == 452025, "XQ 2-DH5 point: %u B, %u bps", sbc_frame_length(&p), sbc_bitrate(&p));
    p.bitpool = 47;
    CHECK(sbc_frame_length(&p) == 200 && 5 * 200 + 13 <= 1021 - 4 && sbc_bitrate(&p) == 551250, "XQ 3-DH5 point: %u B, %u bps", sbc_frame_length(&p), sbc_bitrate(&p));
    snr = run_roundtrip(44100, SBC_MODE_DUAL, SBC_ALLOC_LOUDNESS, 8, 16, 38, 1);   CHECK(snr > 45.0, "SNR XQ dual/38 too low: %.1f", snr);
    snr = run_roundtrip(44100, SBC_MODE_DUAL, SBC_ALLOC_LOUDNESS, 8, 16, 47, 1);   CHECK(snr > 50.0, "SNR XQ dual/47 too low: %.1f", snr);
    snr = run_roundtrip(48000, SBC_MODE_DUAL, SBC_ALLOC_LOUDNESS, 8, 16, 38, 1);   CHECK(snr > 45.0, "SNR XQ 48k dual/38 too low: %.1f", snr);

    /* 24-bit input: the same configurations from an S32 source */
    printf("24-bit (S32) input\n");
    input_s32 = 1;
    snr = run_roundtrip(44100, SBC_MODE_JOINT, SBC_ALLOC_LOUDNESS, 8, 16, 53, 1);  CHECK(snr > 25.0, "S32 SNR joint/53 too low: %.1f", snr);
    snr = run_roundtrip(44100, SBC_MODE_DUAL, SBC_ALLOC_LOUDNESS, 8, 16, 47, 1);   CHECK(snr > 50.0, "S32 SNR dual/47 too low: %.1f", snr);
    snr = run_roundtrip(48000, SBC_MODE_JOINT, SBC_ALLOC_LOUDNESS, 8, 16, 250, 1); CHECK(snr > 45.0, "S32 SNR joint/250 too low: %.1f", snr);
    input_s32 = 0;
    {
        double s16 = quiet_tone_snr(0, SBC_MODE_JOINT, 250), s32 = quiet_tone_snr(1, SBC_MODE_JOINT, 250);
        double d16 = quiet_tone_snr(0, SBC_MODE_DUAL, 128), d32 = quiet_tone_snr(1, SBC_MODE_DUAL, 128);
        printf("  quiet tone (-70 dBFS) joint/250: %.2f dB from 16-bit, %.2f dB from 24-bit; dual/128: %.2f / %.2f dB\n", s16, s32, d16, d32);
        CHECK(s32 > s16 + 1.5 && d32 > d16 + 1.5, "24-bit input must keep the precision lost by rounding to 16 bits");
    }

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

    /* 2b. bitpool changes inside a stream keep the decoder's synthesis history: alternating
     * 53/40 every 10 frames, the error near each switch stays at its steady level (the decoder
     * used to reset its filter: a -1.3 dBFS click at every switch, finding #9) */
    {
        sbc_encoder *e = (sbc_encoder *)calloc(1, sizeof(*e));
        sbc_decoder *d = (sbc_decoder *)calloc(1, sizeof(*d));
        sbc_s16 pcm[128 * 2], dec[128 * 2]; sbc_u8 buf[600];
        sbc_u32 fr, ch, rate, f, i;
        const sbc_u32 nframes = 300;
        sbc_u32 delay = 0;
        sbc_s16 *in = (sbc_s16 *)calloc((size_t)nframes * 128 * 2, sizeof(sbc_s16));
        sbc_s16 *outp = (sbc_s16 *)calloc((size_t)nframes * 128 * 2, sizeof(sbc_s16));
        double worst = 0, typical = 0;
        p.sample_rate = 48000; p.channel_mode = SBC_MODE_DUAL; p.allocation = SBC_ALLOC_LOUDNESS; p.subbands = 8; p.blocks = 16; p.bitpool = 53;
        sbc_encoder_init(e, &p); sbc_decoder_init(d);
        for (i = 0; i < nframes * 128; i++) {
            double t = (double)i / 48000.0, fsw = 20.0 * pow(1000.0, t / (nframes * 128 / 48000.0));   /* 20 Hz .. 20 kHz */
            double ph = 2 * 3.14159265358979 * 20.0 * (nframes * 128 / 48000.0) / log(1000.0) * (fsw / 20.0 - 1.0);
            in[i * 2] = in[i * 2 + 1] = (sbc_s16)(0.7 * 32767.0 * sin(ph));
        }
        for (f = 0; f < nframes; f++) {
            sbc_u32 len;
            sbc_encoder_set_bitpool(e, (f / 10) % 2 ? 40 : 53);
            memcpy(pcm, in + f * 256, sizeof(pcm));
            len = sbc_encode_frame(e, pcm, buf, sizeof(buf));
            CHECK(len && sbc_decode_frame(d, buf, len, dec, 256, &fr, &ch, &rate) == len, "decode across bitpool change, frame %u", f);
            memcpy(outp + f * 256, dec, sizeof(dec));
        }
        {   /* the filterbank pair's delay, on the first frames (one bitpool) */
            double beste = 1e300; sbc_u32 dl;
            for (dl = 0; dl < 200; dl++) {
                double error = 0;
                for (i = 300; i < 1200; i++) { double x = (double)in[i * 2] - outp[(i + dl) * 2]; error += x * x; }
                if (error < beste) { beste = error; delay = dl; }
            }
        }
        /* peak error per 64-sample block, against the delayed input */
        for (f = 4; f < nframes * 2 - 2; f++) {
            double peak = 0;
            for (i = f * 64; i < f * 64 + 64; i++) {
                double err = fabs((double)outp[(i + delay) * 2] - in[i * 2]);
                if (err > peak) peak = err;
            }
            if (peak > worst) worst = peak;
            if (f == nframes) typical = peak;
        }
        printf("bitpool switches 53/40: worst block error %.0f LSB (mid-stream block %.0f)\n", worst, typical);
        CHECK(worst < 2000.0, "decoder click at a bitpool change: %.0f LSB", worst);
        free(e); free(d); free(in); free(outp);
    }

    /* 3. max bitpool for MTU */
    p.sample_rate = 48000; p.channel_mode = SBC_MODE_DUAL; p.allocation = 0; p.subbands = 8; p.blocks = 16; p.bitpool = 250;
    CHECK(sbc_max_bitpool_for(&p, 0, 0) == 128, "unconstrained max bitpool (dual) %u", sbc_max_bitpool_for(&p, 0, 0));
    CHECK(sbc_max_bitpool_for(&p, 345000, 0) <= 53, "bitrate constrained bitpool %u", sbc_max_bitpool_for(&p, 345000, 0));

    printf("%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
