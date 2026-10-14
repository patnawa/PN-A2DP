/*
 * sbc.c - SBC encoder/decoder written from the A2DP specification (Appendix B).
 *
 * Fixed point layout:
 *   PCM input            S16, or S32 of which the top 24 bits are used
 *   analysis delay line  Q8 relative to the 16-bit PCM domain (S16 input << 8)
 *   prototype filter C   Q30
 *   cosine matrices      Q30
 *   subband samples      Q15 relative to the 16-bit PCM domain (|x| < 2^(sf+16))
 *
 * No heap, no CRT besides memset/memcpy, no floating point: safe at any IRQL.
 *
 * SPDX-License-Identifier: MIT
 */
#include "sbc.h"
#include <string.h>

/* The filterbanks' 64-bit multiply-accumulate loops stay scalar. MSVC's auto-vectorizer would add
 * AVX-512 paths dispatched on __isa_available, which nothing sets in the kernel (dead code in
 * pna2dp.sys) and which would use vector state without KeSaveExtendedProcessorState if anything
 * did; build.ps1 refuses a driver image that contains such dispatch. Integer results are the same
 * either way. */
#if defined(_MSC_VER)
#define SBC_SCALAR_LOOP __pragma(loop(no_vector))
#else
#define SBC_SCALAR_LOOP
#endif

/* ------------------------------------------------------------------------- */
/* Tables (A2DP specification 12.8 / 12.9)                                     */
/* ------------------------------------------------------------------------- */

static const sbc_u32 sbc_rates[4] = { 16000, 32000, 44100, 48000 };
static const sbc_u8  sbc_block_counts[4] = { 4, 8, 12, 16 };

/* Loudness offset tables (12.6.3) */
static const sbc_s8 sbc_offset4[4][4] = {
    { -1, 0, 0, 0 }, { -2, 0, 0, 1 }, { -2, 0, 0, 1 }, { -2, 0, 0, 1 }
};
static const sbc_s8 sbc_offset8[4][8] = {
    { -2, 0, 0, 0, 0, 0, 0, 1 }, { -3, 0, 0, 0, 0, 0, 1, 2 },
    { -4, 0, 0, 0, 0, 0, 1, 2 }, { -4, 0, 0, 0, 0, 0, 1, 2 }
};

#define Q30(x) ((sbc_s32)((x) * 1073741824.0 + ((x) >= 0 ? 0.5 : -0.5)))

/* Prototype filter, 4 subbands (Table 12.5): C[0..39] */
static const sbc_s32 sbc_proto4[40] = {
    Q30( 0.00000000E+00), Q30( 5.36548976E-04), Q30( 1.49188357E-03), Q30( 2.73370904E-03),
    Q30( 3.83720193E-03), Q30( 3.89205149E-03), Q30( 1.86581691E-03), Q30(-3.06012286E-03),
    Q30( 1.09137620E-02), Q30( 2.04385087E-02), Q30( 2.88757392E-02), Q30( 3.21939290E-02),
    Q30( 2.58767811E-02), Q30( 6.13245186E-03), Q30(-2.88217274E-02), Q30(-7.76463494E-02),
    Q30( 1.35593274E-01), Q30( 1.94987841E-01), Q30( 2.46636662E-01), Q30( 2.81828203E-01),
    Q30( 2.94315332E-01), Q30( 2.81828203E-01), Q30( 2.46636662E-01), Q30( 1.94987841E-01),
    Q30(-1.35593274E-01), Q30(-7.76463494E-02), Q30(-2.88217274E-02), Q30( 6.13245186E-03),
    Q30( 2.58767811E-02), Q30( 3.21939290E-02), Q30( 2.88757392E-02), Q30( 2.04385087E-02),
    Q30(-1.09137620E-02), Q30(-3.06012286E-03), Q30( 1.86581691E-03), Q30( 3.89205149E-03),
    Q30( 3.83720193E-03), Q30( 2.73370904E-03), Q30( 1.49188357E-03), Q30( 5.36548976E-04),
};

/* Prototype filter, 8 subbands (Table 12.6): C[0..79] */
static const sbc_s32 sbc_proto8[80] = {
    Q30( 0.00000000E+00), Q30( 1.56575398E-04), Q30( 3.43256425E-04), Q30( 5.54620202E-04),
    Q30( 8.23919506E-04), Q30( 1.13992507E-03), Q30( 1.47640169E-03), Q30( 1.78371725E-03),
    Q30( 2.01182542E-03), Q30( 2.10371989E-03), Q30( 1.99454554E-03), Q30( 1.61656283E-03),
    Q30( 9.02154502E-04), Q30(-1.78805361E-04), Q30(-1.64973098E-03), Q30(-3.49717454E-03),
    Q30( 5.65949473E-03), Q30( 8.02941163E-03), Q30( 1.04584443E-02), Q30( 1.27472335E-02),
    Q30( 1.46525263E-02), Q30( 1.59045603E-02), Q30( 1.62208471E-02), Q30( 1.53184106E-02),
    Q30( 1.29371806E-02), Q30( 8.85757540E-03), Q30( 2.92408442E-03), Q30(-4.91578024E-03),
    Q30(-1.46404076E-02), Q30(-2.61098752E-02), Q30(-3.90751381E-02), Q30(-5.31873032E-02),
    Q30( 6.79989431E-02), Q30( 8.29847578E-02), Q30( 9.75753918E-02), Q30( 1.11196689E-01),
    Q30( 1.23264548E-01), Q30( 1.33264415E-01), Q30( 1.40753505E-01), Q30( 1.45389847E-01),
    Q30( 1.46955068E-01), Q30( 1.45389847E-01), Q30( 1.40753505E-01), Q30( 1.33264415E-01),
    Q30( 1.23264548E-01), Q30( 1.11196689E-01), Q30( 9.75753918E-02), Q30( 8.29847578E-02),
    Q30(-6.79989431E-02), Q30(-5.31873032E-02), Q30(-3.90751381E-02), Q30(-2.61098752E-02),
    Q30(-1.46404076E-02), Q30(-4.91578024E-03), Q30( 2.92408442E-03), Q30( 8.85757540E-03),
    Q30( 1.29371806E-02), Q30( 1.53184106E-02), Q30( 1.62208471E-02), Q30( 1.59045603E-02),
    Q30( 1.46525263E-02), Q30( 1.27472335E-02), Q30( 1.04584443E-02), Q30( 8.02941163E-03),
    Q30(-5.65949473E-03), Q30(-3.49717454E-03), Q30(-1.64973098E-03), Q30(-1.78805361E-04),
    Q30( 9.02154502E-04), Q30( 1.61656283E-03), Q30( 1.99454554E-03), Q30( 2.10371989E-03),
    Q30( 2.01182542E-03), Q30( 1.78371725E-03), Q30( 1.47640169E-03), Q30( 1.13992507E-03),
    Q30( 8.23919506E-04), Q30( 5.54620202E-04), Q30( 3.43256425E-04), Q30( 1.56575398E-04),
};

/* cos((i+0.5)*(k-2)*pi/4), i=0..3, k=0..7 (analysis, 4 subbands) */
static const sbc_s32 sbc_cos4[4][8] = {
    { Q30( 0.7071067812), Q30( 0.9238795325), Q30( 1.0000000000), Q30( 0.9238795325), Q30( 0.7071067812), Q30( 0.3826834324), Q30( 0.0000000000), Q30(-0.3826834324) },
    { Q30(-0.7071067812), Q30( 0.3826834324), Q30( 1.0000000000), Q30( 0.3826834324), Q30(-0.7071067812), Q30(-0.9238795325), Q30( 0.0000000000), Q30( 0.9238795325) },
    { Q30(-0.7071067812), Q30(-0.3826834324), Q30( 1.0000000000), Q30(-0.3826834324), Q30(-0.7071067812), Q30( 0.9238795325), Q30( 0.0000000000), Q30(-0.9238795325) },
    { Q30( 0.7071067812), Q30(-0.9238795325), Q30( 1.0000000000), Q30(-0.9238795325), Q30( 0.7071067812), Q30(-0.3826834324), Q30( 0.0000000000), Q30( 0.3826834324) },
};

/* cos((i+0.5)*(k-4)*pi/8), i=0..7, k=0..15 (analysis, 8 subbands) */
static const sbc_s32 sbc_cos8[8][16] = {
    { Q30( 0.7071067812), Q30( 0.8314696123), Q30( 0.9238795325), Q30( 0.9807852804), Q30( 1.0000000000), Q30( 0.9807852804), Q30( 0.9238795325), Q30( 0.8314696123),
      Q30( 0.7071067812), Q30( 0.5555702330), Q30( 0.3826834324), Q30( 0.1950903220), Q30( 0.0000000000), Q30(-0.1950903220), Q30(-0.3826834324), Q30(-0.5555702330) },
    { Q30(-0.7071067812), Q30(-0.1950903220), Q30( 0.3826834324), Q30( 0.8314696123), Q30( 1.0000000000), Q30( 0.8314696123), Q30( 0.3826834324), Q30(-0.1950903220),
      Q30(-0.7071067812), Q30(-0.9807852804), Q30(-0.9238795325), Q30(-0.5555702330), Q30( 0.0000000000), Q30( 0.5555702330), Q30( 0.9238795325), Q30( 0.9807852804) },
    { Q30(-0.7071067812), Q30(-0.9807852804), Q30(-0.3826834324), Q30( 0.5555702330), Q30( 1.0000000000), Q30( 0.5555702330), Q30(-0.3826834324), Q30(-0.9807852804),
      Q30(-0.7071067812), Q30( 0.1950903220), Q30( 0.9238795325), Q30( 0.8314696123), Q30( 0.0000000000), Q30(-0.8314696123), Q30(-0.9238795325), Q30(-0.1950903220) },
    { Q30( 0.7071067812), Q30(-0.5555702330), Q30(-0.9238795325), Q30( 0.1950903220), Q30( 1.0000000000), Q30( 0.1950903220), Q30(-0.9238795325), Q30(-0.5555702330),
      Q30( 0.7071067812), Q30( 0.8314696123), Q30(-0.3826834324), Q30(-0.9807852804), Q30( 0.0000000000), Q30( 0.9807852804), Q30( 0.3826834324), Q30(-0.8314696123) },
    { Q30( 0.7071067812), Q30( 0.5555702330), Q30(-0.9238795325), Q30(-0.1950903220), Q30( 1.0000000000), Q30(-0.1950903220), Q30(-0.9238795325), Q30( 0.5555702330),
      Q30( 0.7071067812), Q30(-0.8314696123), Q30(-0.3826834324), Q30( 0.9807852804), Q30( 0.0000000000), Q30(-0.9807852804), Q30( 0.3826834324), Q30( 0.8314696123) },
    { Q30(-0.7071067812), Q30( 0.9807852804), Q30(-0.3826834324), Q30(-0.5555702330), Q30( 1.0000000000), Q30(-0.5555702330), Q30(-0.3826834324), Q30( 0.9807852804),
      Q30(-0.7071067812), Q30(-0.1950903220), Q30( 0.9238795325), Q30(-0.8314696123), Q30( 0.0000000000), Q30( 0.8314696123), Q30(-0.9238795325), Q30( 0.1950903220) },
    { Q30(-0.7071067812), Q30( 0.1950903220), Q30( 0.3826834324), Q30(-0.8314696123), Q30( 1.0000000000), Q30(-0.8314696123), Q30( 0.3826834324), Q30( 0.1950903220),
      Q30(-0.7071067812), Q30( 0.9807852804), Q30(-0.9238795325), Q30( 0.5555702330), Q30( 0.0000000000), Q30(-0.5555702330), Q30( 0.9238795325), Q30(-0.9807852804) },
    { Q30( 0.7071067812), Q30(-0.8314696123), Q30( 0.9238795325), Q30(-0.9807852804), Q30( 1.0000000000), Q30(-0.9807852804), Q30( 0.9238795325), Q30(-0.8314696123),
      Q30( 0.7071067812), Q30(-0.5555702330), Q30( 0.3826834324), Q30(-0.1950903220), Q30( 0.0000000000), Q30( 0.1950903220), Q30(-0.3826834324), Q30( 0.5555702330) },
};

/* Synthesis matrices N[i][k] = cos((i+2)*(2k+1)*pi/8) (4 sb, 8x4) and cos((i+4)*(2k+1)*pi/16) (8 sb, 16x8). */
static const sbc_s32 sbc_syn4[8][4] = {
    { Q30( 0.7071067812), Q30(-0.7071067812), Q30(-0.7071067812), Q30( 0.7071067812) },
    { Q30( 0.3826834324), Q30(-0.9238795325), Q30( 0.9238795325), Q30(-0.3826834324) },
    { Q30( 0.0000000000), Q30( 0.0000000000), Q30( 0.0000000000), Q30( 0.0000000000) },
    { Q30(-0.3826834324), Q30( 0.9238795325), Q30(-0.9238795325), Q30( 0.3826834324) },
    { Q30(-0.7071067812), Q30( 0.7071067812), Q30( 0.7071067812), Q30(-0.7071067812) },
    { Q30(-0.9238795325), Q30(-0.3826834324), Q30( 0.3826834324), Q30( 0.9238795325) },
    { Q30(-1.0000000000), Q30(-1.0000000000), Q30(-1.0000000000), Q30(-1.0000000000) },
    { Q30(-0.9238795325), Q30(-0.3826834324), Q30( 0.3826834324), Q30( 0.9238795325) },
};
static const sbc_s32 sbc_syn8[16][8] = {
    { Q30( 0.7071067812), Q30(-0.7071067812), Q30(-0.7071067812), Q30( 0.7071067812), Q30( 0.7071067812), Q30(-0.7071067812), Q30(-0.7071067812), Q30( 0.7071067812) },
    { Q30( 0.5555702330), Q30(-0.9807852804), Q30( 0.1950903220), Q30( 0.8314696123), Q30(-0.8314696123), Q30(-0.1950903220), Q30( 0.9807852804), Q30(-0.5555702330) },
    { Q30( 0.3826834324), Q30(-0.9238795325), Q30( 0.9238795325), Q30(-0.3826834324), Q30(-0.3826834324), Q30( 0.9238795325), Q30(-0.9238795325), Q30( 0.3826834324) },
    { Q30( 0.1950903220), Q30(-0.5555702330), Q30( 0.8314696123), Q30(-0.9807852804), Q30( 0.9807852804), Q30(-0.8314696123), Q30( 0.5555702330), Q30(-0.1950903220) },
    { Q30( 0.0000000000), Q30( 0.0000000000), Q30( 0.0000000000), Q30( 0.0000000000), Q30( 0.0000000000), Q30( 0.0000000000), Q30( 0.0000000000), Q30( 0.0000000000) },
    { Q30(-0.1950903220), Q30( 0.5555702330), Q30(-0.8314696123), Q30( 0.9807852804), Q30(-0.9807852804), Q30( 0.8314696123), Q30(-0.5555702330), Q30( 0.1950903220) },
    { Q30(-0.3826834324), Q30( 0.9238795325), Q30(-0.9238795325), Q30( 0.3826834324), Q30( 0.3826834324), Q30(-0.9238795325), Q30( 0.9238795325), Q30(-0.3826834324) },
    { Q30(-0.5555702330), Q30( 0.9807852804), Q30(-0.1950903220), Q30(-0.8314696123), Q30( 0.8314696123), Q30( 0.1950903220), Q30(-0.9807852804), Q30( 0.5555702330) },
    { Q30(-0.7071067812), Q30( 0.7071067812), Q30( 0.7071067812), Q30(-0.7071067812), Q30(-0.7071067812), Q30( 0.7071067812), Q30( 0.7071067812), Q30(-0.7071067812) },
    { Q30(-0.8314696123), Q30( 0.1950903220), Q30( 0.9807852804), Q30( 0.5555702330), Q30(-0.5555702330), Q30(-0.9807852804), Q30(-0.1950903220), Q30( 0.8314696123) },
    { Q30(-0.9238795325), Q30(-0.3826834324), Q30( 0.3826834324), Q30( 0.9238795325), Q30( 0.9238795325), Q30( 0.3826834324), Q30(-0.3826834324), Q30(-0.9238795325) },
    { Q30(-0.9807852804), Q30(-0.8314696123), Q30(-0.5555702330), Q30(-0.1950903220), Q30( 0.1950903220), Q30( 0.5555702330), Q30( 0.8314696123), Q30( 0.9807852804) },
    { Q30(-1.0000000000), Q30(-1.0000000000), Q30(-1.0000000000), Q30(-1.0000000000), Q30(-1.0000000000), Q30(-1.0000000000), Q30(-1.0000000000), Q30(-1.0000000000) },
    { Q30(-0.9807852804), Q30(-0.8314696123), Q30(-0.5555702330), Q30(-0.1950903220), Q30( 0.1950903220), Q30( 0.5555702330), Q30( 0.8314696123), Q30( 0.9807852804) },
    { Q30(-0.9238795325), Q30(-0.3826834324), Q30( 0.3826834324), Q30( 0.9238795325), Q30( 0.9238795325), Q30( 0.3826834324), Q30(-0.3826834324), Q30(-0.9238795325) },
    { Q30(-0.8314696123), Q30( 0.1950903220), Q30( 0.9807852804), Q30( 0.5555702330), Q30(-0.5555702330), Q30(-0.9807852804), Q30(-0.1950903220), Q30( 0.8314696123) },
};

/* CRC-8, polynomial 0x1D, init 0x0F (12.6.4 / 12.7) */
static const sbc_u8 sbc_crc_table[256] = {
    0x00, 0x1D, 0x3A, 0x27, 0x74, 0x69, 0x4E, 0x53, 0xE8, 0xF5, 0xD2, 0xCF, 0x9C, 0x81, 0xA6, 0xBB,
    0xCD, 0xD0, 0xF7, 0xEA, 0xB9, 0xA4, 0x83, 0x9E, 0x25, 0x38, 0x1F, 0x02, 0x51, 0x4C, 0x6B, 0x76,
    0x87, 0x9A, 0xBD, 0xA0, 0xF3, 0xEE, 0xC9, 0xD4, 0x6F, 0x72, 0x55, 0x48, 0x1B, 0x06, 0x21, 0x3C,
    0x4A, 0x57, 0x70, 0x6D, 0x3E, 0x23, 0x04, 0x19, 0xA2, 0xBF, 0x98, 0x85, 0xD6, 0xCB, 0xEC, 0xF1,
    0x13, 0x0E, 0x29, 0x34, 0x67, 0x7A, 0x5D, 0x40, 0xFB, 0xE6, 0xC1, 0xDC, 0x8F, 0x92, 0xB5, 0xA8,
    0xDE, 0xC3, 0xE4, 0xF9, 0xAA, 0xB7, 0x90, 0x8D, 0x36, 0x2B, 0x0C, 0x11, 0x42, 0x5F, 0x78, 0x65,
    0x94, 0x89, 0xAE, 0xB3, 0xE0, 0xFD, 0xDA, 0xC7, 0x7C, 0x61, 0x46, 0x5B, 0x08, 0x15, 0x32, 0x2F,
    0x59, 0x44, 0x63, 0x7E, 0x2D, 0x30, 0x17, 0x0A, 0xB1, 0xAC, 0x8B, 0x96, 0xC5, 0xD8, 0xFF, 0xE2,
    0x26, 0x3B, 0x1C, 0x01, 0x52, 0x4F, 0x68, 0x75, 0xCE, 0xD3, 0xF4, 0xE9, 0xBA, 0xA7, 0x80, 0x9D,
    0xEB, 0xF6, 0xD1, 0xCC, 0x9F, 0x82, 0xA5, 0xB8, 0x03, 0x1E, 0x39, 0x24, 0x77, 0x6A, 0x4D, 0x50,
    0xA1, 0xBC, 0x9B, 0x86, 0xD5, 0xC8, 0xEF, 0xF2, 0x49, 0x54, 0x73, 0x6E, 0x3D, 0x20, 0x07, 0x1A,
    0x6C, 0x71, 0x56, 0x4B, 0x18, 0x05, 0x22, 0x3F, 0x84, 0x99, 0xBE, 0xA3, 0xF0, 0xED, 0xCA, 0xD7,
    0x35, 0x28, 0x0F, 0x12, 0x41, 0x5C, 0x7B, 0x66, 0xDD, 0xC0, 0xE7, 0xFA, 0xA9, 0xB4, 0x93, 0x8E,
    0xF8, 0xE5, 0xC2, 0xDF, 0x8C, 0x91, 0xB6, 0xAB, 0x10, 0x0D, 0x2A, 0x37, 0x64, 0x79, 0x5E, 0x43,
    0xB2, 0xAF, 0x88, 0x95, 0xC6, 0xDB, 0xFC, 0xE1, 0x5A, 0x47, 0x60, 0x7D, 0x2E, 0x33, 0x14, 0x09,
    0x7F, 0x62, 0x45, 0x58, 0x0B, 0x16, 0x31, 0x2C, 0x97, 0x8A, 0xAD, 0xB0, 0xE3, 0xFE, 0xD9, 0xC4,
};

/* CRC over the first bit_len bits of data (MSB first). */
static sbc_u8 sbc_crc8(const sbc_u8 *data, sbc_u32 bit_len)
{
    sbc_u8 crc = 0x0F;
    sbc_u32 i;
    for (i = 0; i < bit_len / 8; i++) crc = sbc_crc_table[crc ^ data[i]];
    if (bit_len % 8) {
        sbc_u8 octet = data[i];
        sbc_u32 b;
        for (b = 0; b < bit_len % 8; b++) {
            sbc_u8 bit = (sbc_u8)(((octet ^ crc) & 0x80) >> 7);
            crc = (sbc_u8)((crc << 1) ^ (bit ? 0x1D : 0));
            octet = (sbc_u8)(octet << 1);
        }
    }
    return crc;
}

/* ------------------------------------------------------------------------- */
/* Parameter helpers                                                            */
/* ------------------------------------------------------------------------- */

static int sbc_freq_index(sbc_u32 rate)
{
    switch (rate) { case 16000: return 0; case 32000: return 1; case 44100: return 2; case 48000: return 3; default: return -1; }
}

static int sbc_block_index(sbc_u8 blocks)
{
    switch (blocks) { case 4: return 0; case 8: return 1; case 12: return 2; case 16: return 3; default: return -1; }
}

int sbc_params_valid(const sbc_params *p)
{
    if (sbc_freq_index(p->sample_rate) < 0) return 0;
    if (sbc_block_index(p->blocks) < 0) return 0;
    if (p->subbands != 4 && p->subbands != 8) return 0;
    if (p->channel_mode > SBC_MODE_JOINT) return 0;
    if (p->allocation > SBC_ALLOC_SNR) return 0;
    if (p->bitpool < 2 || p->bitpool > 250) return 0;
    /* the bit allocator can hand out at most 16 bits per subband (A2DP 12.6.3) */
    {
        sbc_u32 maxbp = (p->channel_mode == SBC_MODE_MONO || p->channel_mode == SBC_MODE_DUAL) ? 16u * p->subbands : 32u * p->subbands;
        if (maxbp > 250) maxbp = 250;
        if (p->bitpool > maxbp) return 0;
    }
    return 1;
}

sbc_u32 sbc_frame_length(const sbc_params *p)
{
    sbc_u32 nch, len;
    if (!sbc_params_valid(p)) return 0;
    nch = (p->channel_mode == SBC_MODE_MONO) ? 1 : 2;
    len = 4 + (4u * p->subbands * nch) / 8;
    if (p->channel_mode == SBC_MODE_MONO || p->channel_mode == SBC_MODE_DUAL) {
        len += ((sbc_u32)p->blocks * nch * p->bitpool + 7) / 8;
    } else {
        sbc_u32 join = (p->channel_mode == SBC_MODE_JOINT) ? p->subbands : 0;
        len += (join + (sbc_u32)p->blocks * p->bitpool + 7) / 8;
    }
    return len;
}

sbc_u32 sbc_pcm_per_frame(const sbc_params *p)
{
    return (sbc_u32)p->subbands * p->blocks;
}

sbc_u32 sbc_bitrate(const sbc_params *p)
{
    sbc_u32 len = sbc_frame_length(p);
    if (!len) return 0;
    return (sbc_u32)(((sbc_u64)8 * len * p->sample_rate) / sbc_pcm_per_frame(p));
}

sbc_u8 sbc_max_bitpool_for(const sbc_params *p, sbc_u32 max_bitrate, sbc_u32 max_frame_bytes)
{
    sbc_params q = *p;
    sbc_u8 bp;
    for (bp = 250; bp >= 2; bp--) {
        q.bitpool = bp;
        if (!sbc_params_valid(&q)) continue;
        if (max_frame_bytes && sbc_frame_length(&q) > max_frame_bytes) continue;
        if (max_bitrate && sbc_bitrate(&q) > max_bitrate) continue;
        return bp;
    }
    return 2;
}

/* ------------------------------------------------------------------------- */
/* Bit allocation (12.6.3 / 12.7.3) - shared by encoder and decoder             */
/* ------------------------------------------------------------------------- */

static void sbc_calculate_bits(const sbc_params *p, sbc_u8 freq, const sbc_u8 scale_factor[2][8], sbc_u8 bits[2][8])
{
    int bitneed[2][8];
    int ch, sb, loudness, max_bitneed, bitcount, slicecount, bitslice;
    int subbands = p->subbands;
    int bitpool = p->bitpool;

    if (p->channel_mode == SBC_MODE_MONO || p->channel_mode == SBC_MODE_DUAL) {
        int nch = (p->channel_mode == SBC_MODE_MONO) ? 1 : 2;
        for (ch = 0; ch < nch; ch++) {
            max_bitneed = 0;
            for (sb = 0; sb < subbands; sb++) {
                if (p->allocation == SBC_ALLOC_SNR) {
                    bitneed[ch][sb] = scale_factor[ch][sb];
                } else if (scale_factor[ch][sb] == 0) {
                    bitneed[ch][sb] = -5;
                } else {
                    loudness = (int)scale_factor[ch][sb] - (subbands == 4 ? sbc_offset4[freq][sb] : sbc_offset8[freq][sb]);
                    bitneed[ch][sb] = (loudness > 0) ? (loudness / 2) : loudness;
                }
                if (bitneed[ch][sb] > max_bitneed) max_bitneed = bitneed[ch][sb];
            }
            bitcount = 0; slicecount = 0; bitslice = max_bitneed + 1;
            do {
                bitslice--;
                bitcount += slicecount;
                slicecount = 0;
                for (sb = 0; sb < subbands; sb++) {
                    if (bitneed[ch][sb] > bitslice + 1 && bitneed[ch][sb] < bitslice + 16) slicecount++;
                    else if (bitneed[ch][sb] == bitslice + 1) slicecount += 2;
                }
            } while (bitcount + slicecount < bitpool);
            if (bitcount + slicecount == bitpool) { bitcount += slicecount; bitslice--; }
            for (sb = 0; sb < subbands; sb++) {
                if (bitneed[ch][sb] < bitslice + 2) bits[ch][sb] = 0;
                else { int b = bitneed[ch][sb] - bitslice; bits[ch][sb] = (sbc_u8)(b > 16 ? 16 : b); }
            }
            for (sb = 0; bitcount < bitpool && sb < subbands; sb++) {
                if (bits[ch][sb] >= 2 && bits[ch][sb] < 16) { bits[ch][sb]++; bitcount++; }
                else if (bitneed[ch][sb] == bitslice + 1 && bitpool > bitcount + 1) { bits[ch][sb] = 2; bitcount += 2; }
            }
            for (sb = 0; bitcount < bitpool && sb < subbands; sb++) {
                if (bits[ch][sb] < 16) { bits[ch][sb]++; bitcount++; }
            }
        }
    } else {
        max_bitneed = 0;
        for (ch = 0; ch < 2; ch++) {
            for (sb = 0; sb < subbands; sb++) {
                if (p->allocation == SBC_ALLOC_SNR) {
                    bitneed[ch][sb] = scale_factor[ch][sb];
                } else if (scale_factor[ch][sb] == 0) {
                    bitneed[ch][sb] = -5;
                } else {
                    loudness = (int)scale_factor[ch][sb] - (subbands == 4 ? sbc_offset4[freq][sb] : sbc_offset8[freq][sb]);
                    bitneed[ch][sb] = (loudness > 0) ? (loudness / 2) : loudness;
                }
                if (bitneed[ch][sb] > max_bitneed) max_bitneed = bitneed[ch][sb];
            }
        }
        bitcount = 0; slicecount = 0; bitslice = max_bitneed + 1;
        do {
            bitslice--;
            bitcount += slicecount;
            slicecount = 0;
            for (ch = 0; ch < 2; ch++) {
                for (sb = 0; sb < subbands; sb++) {
                    if (bitneed[ch][sb] > bitslice + 1 && bitneed[ch][sb] < bitslice + 16) slicecount++;
                    else if (bitneed[ch][sb] == bitslice + 1) slicecount += 2;
                }
            }
        } while (bitcount + slicecount < bitpool);
        if (bitcount + slicecount == bitpool) { bitcount += slicecount; bitslice--; }
        for (ch = 0; ch < 2; ch++) {
            for (sb = 0; sb < subbands; sb++) {
                if (bitneed[ch][sb] < bitslice + 2) bits[ch][sb] = 0;
                else { int b = bitneed[ch][sb] - bitslice; bits[ch][sb] = (sbc_u8)(b > 16 ? 16 : b); }
            }
        }
        ch = 0; sb = 0;
        while (bitcount < bitpool) {
            if (bits[ch][sb] >= 2 && bits[ch][sb] < 16) { bits[ch][sb]++; bitcount++; }
            else if (bitneed[ch][sb] == bitslice + 1 && bitpool > bitcount + 1) { bits[ch][sb] = 2; bitcount += 2; }
            if (ch == 1) { ch = 0; sb++; if (sb >= subbands) break; } else ch = 1;
        }
        ch = 0; sb = 0;
        while (bitcount < bitpool) {
            if (bits[ch][sb] < 16) { bits[ch][sb]++; bitcount++; }
            if (ch == 1) { ch = 0; sb++; if (sb >= subbands) break; } else ch = 1;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Encoder                                                                      */
/* ------------------------------------------------------------------------- */

int sbc_encoder_init(sbc_encoder *e, const sbc_params *p)
{
    memset(e, 0, sizeof(*e));
    if (!sbc_params_valid(p)) return -1;
    e->p = *p;
    e->freq_index = (sbc_u8)sbc_freq_index(p->sample_rate);
    e->channels = (p->channel_mode == SBC_MODE_MONO) ? 1 : 2;
    e->frame_length = sbc_frame_length(p);
    e->pcm_per_frame = sbc_pcm_per_frame(p);
    return 0;
}

void sbc_encoder_set_bitpool(sbc_encoder *e, sbc_u8 bitpool)
{
    if (bitpool < 2) bitpool = 2;
    if (bitpool > 250) bitpool = 250;
    {
        sbc_u32 maxbp = (e->p.channel_mode == SBC_MODE_MONO || e->p.channel_mode == SBC_MODE_DUAL) ? 16u * e->p.subbands : 32u * e->p.subbands;
        if (maxbp > 250) maxbp = 250;
        if (bitpool > maxbp) bitpool = (sbc_u8)maxbp;
    }
    e->p.bitpool = bitpool;
    e->frame_length = sbc_frame_length(&e->p);
}

/* One input sample in the Q8 delay-line format: S16 << 8, or S32 rounded to 24 bits. */
static sbc_s32 sbc_input(const sbc_s16 *in16, const sbc_s32 *in32, int index)
{
    if (in16) return (sbc_s32)in16[index] * 256;
    return (sbc_s32)(((sbc_s64)in32[index] + 128) >> 8);
}

/* Analysis filterbank (12.6.2): processes `subbands` new samples of one channel.
 * Exactly one of in16/in32 is set. x is Q8, so Q8*Q30 >> 23 = Q15; for S16 input
 * ((v << 8) * c) >> 23 == (v * c) >> 15, i.e. bit-exact with a 16-bit delay line. */
static void sbc_analyze(sbc_s32 *x, const sbc_s16 *in16, const sbc_s32 *in32, int stride, int subbands, sbc_s32 *out)
{
    sbc_s64 z[16];
    int i, k;
    if (subbands == 4) {
        for (i = 39; i >= 4; i--) x[i] = x[i - 4];
        for (i = 3; i >= 0; i--) x[i] = sbc_input(in16, in32, (3 - i) * stride);
        for (i = 0; i < 8; i++) {
            sbc_s64 acc = 0;
            SBC_SCALAR_LOOP
            for (k = 0; k < 5; k++) acc += (sbc_s64)x[i + 8 * k] * sbc_proto4[i + 8 * k];
            z[i] = acc >> 23;                         /* Q8*Q30 -> Q15 */
        }
        for (i = 0; i < 4; i++) {
            sbc_s64 acc = 0;
            SBC_SCALAR_LOOP
            for (k = 0; k < 8; k++) acc += z[k] * sbc_cos4[i][k];
            out[i] = (sbc_s32)(acc >> 30);            /* Q15 */
        }
    } else {
        for (i = 79; i >= 8; i--) x[i] = x[i - 8];
        for (i = 7; i >= 0; i--) x[i] = sbc_input(in16, in32, (7 - i) * stride);
        for (i = 0; i < 16; i++) {
            sbc_s64 acc = 0;
            SBC_SCALAR_LOOP
            for (k = 0; k < 5; k++) acc += (sbc_s64)x[i + 16 * k] * sbc_proto8[i + 16 * k];
            z[i] = acc >> 23;
        }
        for (i = 0; i < 8; i++) {
            sbc_s64 acc = 0;
            SBC_SCALAR_LOOP
            for (k = 0; k < 16; k++) acc += z[k] * sbc_cos8[i][k];
            out[i] = (sbc_s32)(acc >> 30);
        }
    }
}

static sbc_u8 sbc_scale_factor_of(sbc_u32 maxabs)
{
    /* subband samples are Q15 in the 16-bit domain: sf = floor(log2(max)) - 15, >= 0 */
    sbc_u8 sf = 0;
    if (maxabs < (1u << 15)) return 0;
    while ((maxabs >> (sf + 16)) != 0 && sf < 15) sf++;
    return sf;
}

typedef struct sbc_bitwriter {
    sbc_u8 *buf;
    sbc_u32 cap;
    sbc_u32 pos;      /* bit position */
} sbc_bitwriter;

static void bw_put(sbc_bitwriter *w, sbc_u32 value, sbc_u32 nbits)
{
    while (nbits) {
        sbc_u32 byte = w->pos >> 3;
        sbc_u32 free_bits = 8 - (w->pos & 7);
        sbc_u32 take = nbits < free_bits ? nbits : free_bits;
        sbc_u32 chunk = (value >> (nbits - take)) & ((1u << take) - 1);
        if (byte < w->cap) {
            if ((w->pos & 7) == 0) w->buf[byte] = 0;
            w->buf[byte] |= (sbc_u8)(chunk << (free_bits - take));
        }
        w->pos += take;
        nbits -= take;
    }
}

static sbc_u32 sbc_encode(sbc_encoder *e, const sbc_s16 *pcm16, const sbc_s32 *pcm32, sbc_u8 *out, sbc_u32 out_cap)
{
    const sbc_params *p = &e->p;
    int blk, ch, sb;
    int subbands = p->subbands, blocks = p->blocks, nch = e->channels;
    sbc_bitwriter w;
    sbc_u8 crc_buf[12];
    sbc_u32 crc_bits;
    sbc_u8 crc_scratch_pos;

    if (out_cap < e->frame_length) return 0;

    /* 1. analysis */
    for (blk = 0; blk < blocks; blk++) {
        for (ch = 0; ch < nch; ch++) {
            sbc_u32 at = ((sbc_u32)blk * subbands) * nch + ch;
            sbc_analyze(e->x[ch], pcm16 ? pcm16 + at : 0, pcm32 ? pcm32 + at : 0, nch, subbands, e->sb[blk][ch]);
        }
    }

    /* 2. joint stereo decision and scale factors */
    memset(e->join, 0, sizeof(e->join));
    if (p->channel_mode == SBC_MODE_JOINT) {
        for (sb = 0; sb < subbands - 1; sb++) {
            sbc_u32 maxL = 0, maxR = 0, maxM = 0, maxS = 0;
            for (blk = 0; blk < blocks; blk++) {
                sbc_s32 l = e->sb[blk][0][sb], r = e->sb[blk][1][sb];
                /* Filterbank overshoot can put a valid subband above 2^30.
                 * Widen before adding: loud correlated bass otherwise wraps
                 * during the joint-stereo decision and corrupts the frame. */
                sbc_s32 m = (sbc_s32)(((sbc_s64)l + r) >> 1);
                sbc_s32 s = (sbc_s32)(((sbc_s64)l - r) >> 1);
                sbc_u32 al = (sbc_u32)(l < 0 ? -l : l), ar = (sbc_u32)(r < 0 ? -r : r);
                sbc_u32 am = (sbc_u32)(m < 0 ? -m : m), as = (sbc_u32)(s < 0 ? -s : s);
                if (al > maxL) maxL = al; if (ar > maxR) maxR = ar;
                if (am > maxM) maxM = am; if (as > maxS) maxS = as;
            }
            if ((int)sbc_scale_factor_of(maxM) + (int)sbc_scale_factor_of(maxS) <
                (int)sbc_scale_factor_of(maxL) + (int)sbc_scale_factor_of(maxR)) {
                e->join[sb] = 1;
                for (blk = 0; blk < blocks; blk++) {
                    sbc_s32 l = e->sb[blk][0][sb], r = e->sb[blk][1][sb];
                    e->sb[blk][0][sb] = (sbc_s32)(((sbc_s64)l + r) >> 1);
                    e->sb[blk][1][sb] = (sbc_s32)(((sbc_s64)l - r) >> 1);
                }
            }
        }
    }
    for (ch = 0; ch < nch; ch++) {
        for (sb = 0; sb < subbands; sb++) {
            sbc_u32 maxabs = 0;
            for (blk = 0; blk < blocks; blk++) {
                sbc_s32 v = e->sb[blk][ch][sb];
                sbc_u32 a = (sbc_u32)(v < 0 ? -v : v);
                if (a > maxabs) maxabs = a;
            }
            e->scale_factor[ch][sb] = sbc_scale_factor_of(maxabs);
        }
    }

    /* 3. bit allocation */
    sbc_calculate_bits(p, e->freq_index, (const sbc_u8 (*)[8])e->scale_factor, e->bits);

    /* 4. header */
    w.buf = out; w.cap = out_cap; w.pos = 0;
    bw_put(&w, SBC_SYNCWORD, 8);
    bw_put(&w, e->freq_index, 2);
    bw_put(&w, (sbc_u32)sbc_block_index(p->blocks), 2);
    bw_put(&w, p->channel_mode, 2);
    bw_put(&w, p->allocation, 1);
    bw_put(&w, subbands == 8 ? 1 : 0, 1);
    bw_put(&w, p->bitpool, 8);
    bw_put(&w, 0, 8);             /* CRC placeholder */

    /* join bits + scale factors also feed the CRC: collect them in crc_buf */
    memset(crc_buf, 0, sizeof(crc_buf));
    crc_buf[0] = out[1];
    crc_buf[1] = out[2];
    crc_bits = 16;
    crc_scratch_pos = 0; (void)crc_scratch_pos;
    {
        sbc_bitwriter cw; cw.buf = crc_buf; cw.cap = sizeof(crc_buf); cw.pos = 16;
        if (p->channel_mode == SBC_MODE_JOINT) {
            for (sb = 0; sb < subbands - 1; sb++) { bw_put(&w, e->join[sb], 1); bw_put(&cw, e->join[sb], 1); }
            bw_put(&w, 0, 1); bw_put(&cw, 0, 1);   /* RFA */
        }
        for (ch = 0; ch < nch; ch++) {
            for (sb = 0; sb < subbands; sb++) { bw_put(&w, e->scale_factor[ch][sb], 4); bw_put(&cw, e->scale_factor[ch][sb], 4); }
        }
        crc_bits = cw.pos;
    }
    out[3] = sbc_crc8(crc_buf, crc_bits);

    /* 5. quantise */
    for (blk = 0; blk < blocks; blk++) {
        for (ch = 0; ch < nch; ch++) {
            for (sb = 0; sb < subbands; sb++) {
                sbc_u32 nb = e->bits[ch][sb];
                if (nb == 0) continue;
                {
                    sbc_u32 sf = e->scale_factor[ch][sb];
                    sbc_u64 levels = (1u << nb) - 1;
                    sbc_s64 x = e->sb[blk][ch][sb];
                    sbc_s64 delta = (sbc_s64)1 << (sf + 16);
                    sbc_s64 num = x + delta;
                    sbc_u32 q;
                    if (num < 0) num = 0;
                    q = (sbc_u32)(((sbc_u64)num * levels) >> (sf + 17));
                    if (q > levels) q = (sbc_u32)levels;
                    bw_put(&w, q, nb);
                }
            }
        }
    }
    /* 6. pad to byte boundary */
    if (w.pos & 7) bw_put(&w, 0, 8 - (w.pos & 7));
    return (w.pos >> 3) == e->frame_length ? e->frame_length : (w.pos >> 3);
}

sbc_u32 sbc_encode_frame(sbc_encoder *e, const sbc_s16 *pcm, sbc_u8 *out, sbc_u32 out_cap)
{
    return sbc_encode(e, pcm, 0, out, out_cap);
}

sbc_u32 sbc_encode_frame_s32(sbc_encoder *e, const sbc_s32 *pcm, sbc_u8 *out, sbc_u32 out_cap)
{
    return sbc_encode(e, 0, pcm, out, out_cap);
}

/* ------------------------------------------------------------------------- */
/* Decoder                                                                      */
/* ------------------------------------------------------------------------- */

int sbc_decoder_init(sbc_decoder *d)
{
    memset(d, 0, sizeof(*d));
    return 0;
}

sbc_u32 sbc_parse_header(const sbc_u8 *in, sbc_u32 in_len, sbc_params *p)
{
    if (in_len < 4 || in[0] != SBC_SYNCWORD) return 0;
    p->sample_rate = sbc_rates[(in[1] >> 6) & 3];
    p->blocks = sbc_block_counts[(in[1] >> 4) & 3];
    p->channel_mode = (sbc_u8)((in[1] >> 2) & 3);
    p->allocation = (sbc_u8)((in[1] >> 1) & 1);
    p->subbands = (in[1] & 1) ? 8 : 4;
    p->bitpool = in[2];
    return sbc_frame_length(p);
}

typedef struct sbc_bitreader {
    const sbc_u8 *buf;
    sbc_u32 len_bits;
    sbc_u32 pos;
} sbc_bitreader;

static sbc_u32 br_get(sbc_bitreader *r, sbc_u32 nbits)
{
    sbc_u32 v = 0;
    while (nbits--) {
        sbc_u32 bit = 0;
        if (r->pos < r->len_bits) bit = (r->buf[r->pos >> 3] >> (7 - (r->pos & 7))) & 1;
        r->pos++;
        v = (v << 1) | bit;
    }
    return v;
}

static sbc_s16 sbc_clip16(sbc_s64 v)
{
    if (v > 32767) return 32767;
    if (v < -32768) return -32768;
    return (sbc_s16)v;
}

/* Synthesis filterbank (12.7.2) for one block of one channel. */
static void sbc_synthesize(sbc_s32 *v, const sbc_s32 *sb_in, int subbands, sbc_s16 *out, int stride)
{
    sbc_s64 u[80];
    int i, k;
    if (subbands == 4) {
        for (i = 79; i >= 8; i--) v[i] = v[i - 8];
        for (i = 0; i < 8; i++) {
            sbc_s64 acc = 0;
            SBC_SCALAR_LOOP
            for (k = 0; k < 4; k++) acc += (sbc_s64)sbc_syn4[i][k] * sb_in[k];
            v[i] = (sbc_s32)(acc >> 30);              /* Q15 */
        }
        for (i = 0; i < 5; i++) {
            for (k = 0; k < 4; k++) {
                u[i * 8 + k] = v[i * 16 + k];
                u[i * 8 + 4 + k] = v[i * 16 + 12 + k];
            }
        }
        for (k = 0; k < 4; k++) {
            sbc_s64 acc = 0;
            SBC_SCALAR_LOOP
            for (i = 0; i < 10; i++) acc += u[k + 4 * i] * sbc_proto4[k + 4 * i];
            /* D = -4*C (measured against the analysis bank): Q15 * Q30 -> Q45; gain 4 -> >> 43 */
            out[k * stride] = sbc_clip16(-(acc >> 43));
        }
    } else {
        for (i = 159; i >= 16; i--) v[i] = v[i - 16];
        for (i = 0; i < 16; i++) {
            sbc_s64 acc = 0;
            SBC_SCALAR_LOOP
            for (k = 0; k < 8; k++) acc += (sbc_s64)sbc_syn8[i][k] * sb_in[k];
            v[i] = (sbc_s32)(acc >> 30);
        }
        for (i = 0; i < 5; i++) {
            for (k = 0; k < 8; k++) {
                u[i * 16 + k] = v[i * 32 + k];
                u[i * 16 + 8 + k] = v[i * 32 + 24 + k];
            }
        }
        for (k = 0; k < 8; k++) {
            sbc_s64 acc = 0;
            SBC_SCALAR_LOOP
            for (i = 0; i < 10; i++) acc += u[k + 8 * i] * sbc_proto8[k + 8 * i];
            /* D = -8*C -> >> 42 */
            out[k * stride] = sbc_clip16(-(acc >> 42));
        }
    }
}

sbc_u32 sbc_decode_frame(sbc_decoder *d, const sbc_u8 *in, sbc_u32 in_len, sbc_s16 *pcm, sbc_u32 pcm_cap_samples,
                         sbc_u32 *pcm_frames, sbc_u32 *channels, sbc_u32 *sample_rate)
{
    sbc_params p;
    sbc_u32 len = sbc_parse_header(in, in_len, &p);
    sbc_bitreader r;
    sbc_u8 crc_buf[12];
    sbc_bitwriter cw;
    int blk, ch, sb, subbands, blocks, nch;

    if (!len || len > in_len) return 0;
    subbands = p.subbands; blocks = p.blocks; nch = (p.channel_mode == SBC_MODE_MONO) ? 1 : 2;
    if (pcm_cap_samples < (sbc_u32)subbands * blocks * nch) return 0;

    /* new stream parameters: reset the filter state. The bitpool only changes the bit
     * allocation: an in-band change (ABR, a live change) keeps the synthesis history, as real
     * sinks do; compared field by field, not with the struct's padding (finding #9) */
    if (d->p.sample_rate != p.sample_rate || d->p.channel_mode != p.channel_mode || d->p.subbands != p.subbands ||
        d->p.blocks != p.blocks) {
        memset(d->v, 0, sizeof(d->v));
        d->p = p;
        d->freq_index = (sbc_u8)sbc_freq_index(p.sample_rate);
        d->channels = (sbc_u8)nch;
        d->frame_length = len;
    }
    d->p.allocation = p.allocation;
    d->p.bitpool = p.bitpool;
    d->frame_length = len;

    r.buf = in; r.len_bits = len * 8; r.pos = 32;
    memset(crc_buf, 0, sizeof(crc_buf));
    crc_buf[0] = in[1]; crc_buf[1] = in[2];
    cw.buf = crc_buf; cw.cap = sizeof(crc_buf); cw.pos = 16;

    memset(d->join, 0, sizeof(d->join));
    if (p.channel_mode == SBC_MODE_JOINT) {
        for (sb = 0; sb < subbands - 1; sb++) { d->join[sb] = (sbc_u8)br_get(&r, 1); bw_put(&cw, d->join[sb], 1); }
        bw_put(&cw, br_get(&r, 1), 1);
    }
    for (ch = 0; ch < nch; ch++) {
        for (sb = 0; sb < subbands; sb++) { d->scale_factor[ch][sb] = (sbc_u8)br_get(&r, 4); bw_put(&cw, d->scale_factor[ch][sb], 4); }
    }
    if (sbc_crc8(crc_buf, cw.pos) != in[3]) return 0;

    sbc_calculate_bits(&p, d->freq_index, (const sbc_u8 (*)[8])d->scale_factor, d->bits);

    for (blk = 0; blk < blocks; blk++) {
        for (ch = 0; ch < nch; ch++) {
            for (sb = 0; sb < subbands; sb++) {
                sbc_u32 nb = d->bits[ch][sb];
                if (nb == 0) { d->sb[blk][ch][sb] = 0; continue; }
                {
                    sbc_u32 q = br_get(&r, nb);
                    sbc_u32 sf = d->scale_factor[ch][sb];
                    sbc_u64 levels = (1u << nb) - 1;
                    /* x = ((2q+1) * 2^(sf+16)) / levels - 2^(sf+16), Q15 */
                    sbc_s64 x = (sbc_s64)((((sbc_u64)(2 * q + 1)) << (sf + 16)) / levels) - ((sbc_s64)1 << (sf + 16));
                    d->sb[blk][ch][sb] = (sbc_s32)x;
                }
            }
        }
    }
    if (p.channel_mode == SBC_MODE_JOINT) {
        for (blk = 0; blk < blocks; blk++) {
            for (sb = 0; sb < subbands; sb++) {
                if (d->join[sb]) {
                    sbc_s32 m = d->sb[blk][0][sb], s = d->sb[blk][1][sb];
                    d->sb[blk][0][sb] = m + s;
                    d->sb[blk][1][sb] = m - s;
                }
            }
        }
    }
    for (blk = 0; blk < blocks; blk++) {
        for (ch = 0; ch < nch; ch++) {
            sbc_synthesize(d->v[ch], d->sb[blk][ch], subbands, pcm + ((sbc_u32)blk * subbands) * nch + ch, nch);
        }
    }
    *pcm_frames = (sbc_u32)subbands * blocks;
    *channels = (sbc_u32)nch;
    *sample_rate = p.sample_rate;
    return len;
}
