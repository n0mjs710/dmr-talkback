/* test_rewrite.c — the loopback-identity conformance vector.
 *
 * The whole correctness claim of this program is:
 *
 *   the audio that comes back is bit-identical to the audio that went in,
 *   and everything identifying the call is rewritten to us — in the DMRD
 *   header AND in both LC carriers inside the payload.
 *
 * These tests assert exactly that, for group and unit replies, over a
 * synthetic capture covering every frame kind a real stream contains.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "talkback.h"
#include "hbp_const.h"
#include "dmr/dmr.h"

/* replay.c transmits through the HBP client; the rewrite path under test does
 * not, so stubs are enough to link. */
void hbp_send_dmrd(struct hbp *hb, const uint8_t *data, int len) { (void)hb; (void)data; (void)len; }
int  hbp_is_connected(struct hbp *hb) { (void)hb; return 0; }

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { failures++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

/* ------------------------------------------------------------------ */
/* Synthetic capture                                                    */
/* ------------------------------------------------------------------ */

#define ORIG_SRC   3121234u
#define ORIG_TGID  9990u
#define TB_RADIO   3120099u
#define ORIG_SID   0xAABBCCDDu
#define NEW_SID    0x11223344u

static uint32_t prng_state = 0x12345678u;
static uint8_t prng_byte(void) {
    prng_state = prng_state * 1664525u + 1013904223u;
    return (uint8_t)(prng_state >> 24);
}

static void wr24(uint8_t *p, uint32_t v) { p[0]=(uint8_t)(v>>16); p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }
static uint32_t rd24(const uint8_t *p) { return (uint32_t)p[0]<<16 | (uint32_t)p[1]<<8 | p[2]; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0]<<24 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<8 | p[3]; }

/* One DMRD packet.  flags carries slot / call type / frame type / dtype. */
static void mk_pkt(uint8_t *out, uint8_t seq, uint32_t src, uint32_t dst,
                   uint8_t flags, uint32_t sid)
{
    memset(out, 0, DMRD_LEN);
    memcpy(out, "DMRD", 4);
    out[DMRD_SEQ_OFF] = seq;
    wr24(out + DMRD_SRC_OFF, src);
    wr24(out + DMRD_DST_OFF, dst);
    wr32(out + DMRD_RPTR_OFF, 312999);        /* the caller's repeater */
    out[DMRD_FLAGS_OFF] = flags;
    wr32(out + DMRD_STREAM_OFF, sid);
    for (int i = 0; i < 33; i++) out[DMRD_PAYLOAD_OFF + i] = prng_byte();
    out[DMRD_BER_OFF]  = 0x07;                /* non-zero, must be cleared */
    out[DMRD_RSSI_OFF] = 0x5A;
}

/* Build VHEAD + 2 superframes (A..F) + VTERM = 14 packets, on TS2. */
#define N_PKTS 14
static int build_capture(uint8_t *buf, int is_unit, uint32_t dst)
{
    uint8_t base = HBPF_TGID_TS2 | (is_unit ? HBPF_TGID_CALL_P : 0);
    int n = 0;
    uint8_t seq = 0;

    mk_pkt(buf + n*DMRD_LEN, seq++, ORIG_SRC, dst,
           base | HBPF_FRAMETYPE_DATASYNC | HBPF_SLT_VHEAD, ORIG_SID); n++;

    for (int sf = 0; sf < 2; sf++) {
        /* Burst A is a VOICESYNC frame, vseq 0; B..F are VOICE, vseq 1..5. */
        mk_pkt(buf + n*DMRD_LEN, seq++, ORIG_SRC, dst,
               base | HBPF_FRAMETYPE_VOICESYNC | 0, ORIG_SID); n++;
        for (int v = 1; v <= 5; v++) {
            mk_pkt(buf + n*DMRD_LEN, seq++, ORIG_SRC, dst,
                   base | HBPF_FRAMETYPE_VOICE | (uint8_t)v, ORIG_SID); n++;
        }
    }

    mk_pkt(buf + n*DMRD_LEN, seq++, ORIG_SRC, dst,
           base | HBPF_FRAMETYPE_DATASYNC | HBPF_SLT_VTERM, ORIG_SID); n++;
    return n;
}

/* ------------------------------------------------------------------ */
/* Assertions                                                           */
/* ------------------------------------------------------------------ */

static void expected_lc(uint8_t lc[9], int is_unit, uint32_t dst, uint32_t src)
{
    lc[0] = is_unit ? 0x03 : 0x00;   /* FLCO: group voice / unit voice */
    lc[1] = 0x00;                    /* FID */
    lc[2] = 0x00;                    /* service options — normalized */
    wr24(lc + 3, dst);
    wr24(lc + 6, src);
}

/* Pull the 9-byte LC back out of a VHEAD/VTERM payload. */
static void extract_full_lc(const uint8_t *pkt, uint8_t lc_out[9])
{
    dmr_bit fb[264];
    dmr_bytes_to_bits(pkt + DMRD_PAYLOAD_OFF, 33, fb);
    dmr_bit bptc_bits[196];
    memcpy(bptc_bits, fb, 98);
    memcpy(bptc_bits + 98, fb + 166, 98);
    dmr_bptc_decode_full_lc(bptc_bits, lc_out);
}

static void run_case(const char *label, int is_unit, uint32_t inbound_dst, uint32_t reply_dst)
{
    printf("%s\n", label);

    uint8_t in[N_PKTS * DMRD_LEN];
    int n = build_capture(in, is_unit, inbound_dst);

    uint8_t dst3[3]; wr24(dst3, reply_dst);
    tb_rewrite rw;
    tb_rewrite_init(&rw, TB_RADIO, dst3, is_unit, NEW_SID);

    uint8_t exp_lc[9];
    expected_lc(exp_lc, is_unit, reply_dst, TB_RADIO);
    CHECK(memcmp(rw.lc, exp_lc, 9) == 0,
          "reply LC mismatch: got %02x%02x%02x %06x %06x",
          rw.lc[0], rw.lc[1], rw.lc[2], rd24(rw.lc+3), rd24(rw.lc+6));

    for (int i = 0; i < n; i++) {
        const uint8_t *ip = in + i * DMRD_LEN;
        uint8_t op[DMRD_LEN];
        tb_rewrite_packet(&rw, ip, op, (uint8_t)i);

        /* ---- header ---- */
        CHECK(op[DMRD_SEQ_OFF] == (uint8_t)i, "pkt %d: seq %u != %d", i, op[DMRD_SEQ_OFF], i);
        CHECK(rd24(op + DMRD_SRC_OFF) == TB_RADIO,
              "pkt %d: src %u != %u", i, rd24(op + DMRD_SRC_OFF), TB_RADIO);
        CHECK(rd24(op + DMRD_DST_OFF) == reply_dst,
              "pkt %d: dst %u != %u", i, rd24(op + DMRD_DST_OFF), reply_dst);
        CHECK(rd32(op + DMRD_RPTR_OFF) == TB_RADIO,
              "pkt %d: repeater %u != %u", i, rd32(op + DMRD_RPTR_OFF), TB_RADIO);
        CHECK(rd32(op + DMRD_STREAM_OFF) == NEW_SID, "pkt %d: stream ID not rewritten", i);
        CHECK(op[DMRD_BER_OFF] == 0 && op[DMRD_RSSI_OFF] == 0, "pkt %d: BER/RSSI not zeroed", i);

        uint8_t ifl = ip[DMRD_FLAGS_OFF], ofl = op[DMRD_FLAGS_OFF];
        CHECK((ofl & HBPF_TGID_TS2) == (ifl & HBPF_TGID_TS2), "pkt %d: slot bit changed", i);
        CHECK((ofl & HBPF_FRAMETYPE_MASK) == (ifl & HBPF_FRAMETYPE_MASK), "pkt %d: frame type changed", i);
        CHECK((ofl & HBPF_DTYPE_MASK) == (ifl & HBPF_DTYPE_MASK), "pkt %d: dtype/vseq changed", i);
        CHECK(((ofl & HBPF_TGID_CALL_P) != 0) == (is_unit != 0), "pkt %d: call-type bit wrong", i);

        /* ---- payload ---- */
        int ftyp = ifl & HBPF_FRAMETYPE_MASK;
        int dtyp = ifl & HBPF_DTYPE_MASK;

        dmr_bit ib[264], ob[264];
        dmr_bytes_to_bits(ip + DMRD_PAYLOAD_OFF, 33, ib);
        dmr_bytes_to_bits(op + DMRD_PAYLOAD_OFF, 33, ob);

        if (ftyp == HBPF_FRAMETYPE_DATASYNC && (dtyp == HBPF_SLT_VHEAD || dtyp == HBPF_SLT_VTERM)) {
            uint8_t got[9];
            extract_full_lc(op, got);
            CHECK(memcmp(got, exp_lc, 9) == 0,
                  "pkt %d (%s): decoded LC %02x%02x%02x %06x %06x != expected",
                  i, dtyp == HBPF_SLT_VHEAD ? "VHEAD" : "VTERM",
                  got[0], got[1], got[2], rd24(got+3), rd24(got+6));
            /* Slot type + sync window carries the colour code — preserved. */
            CHECK(memcmp(ib + 98, ob + 98, 68) == 0,
                  "pkt %d: slot-type/sync window [98:166] was modified", i);
        } else if (ftyp == HBPF_FRAMETYPE_VOICE && dtyp >= 1 && dtyp <= 4) {
            uint8_t emb[4][4];
            dmr_encode_emblc(exp_lc, emb);
            dmr_bit want[32];
            dmr_bytes_to_bits(emb[dtyp - 1], 4, want);
            CHECK(memcmp(ob + 116, want, 32) == 0,
                  "pkt %d: EMB LC fragment for burst %c wrong", i, 'A' + dtyp);
            /* Everything outside [116:148] is AMBE and must be untouched. */
            CHECK(memcmp(ib, ob, 116) == 0 && memcmp(ib + 148, ob + 148, 264 - 148) == 0,
                  "pkt %d: AMBE outside the EMB window was modified", i);
        } else {
            /* Burst A, burst F, and anything else: payload untouched entirely. */
            CHECK(memcmp(ip + DMRD_PAYLOAD_OFF, op + DMRD_PAYLOAD_OFF, 33) == 0,
                  "pkt %d: payload modified on a frame that carries no LC", i);
        }
    }
}

/* Group and unit replies must differ in exactly three places and nowhere else. */
static void run_diff_case(void)
{
    printf("group vs unit differ only in dst, call-type bit, and FLCO\n");

    uint8_t in[N_PKTS * DMRD_LEN];
    int n = build_capture(in, 0, ORIG_TGID);

    uint8_t g_dst[3], u_dst[3];
    wr24(g_dst, ORIG_TGID);
    wr24(u_dst, ORIG_SRC);

    tb_rewrite gr, ur;
    tb_rewrite_init(&gr, TB_RADIO, g_dst, 0, NEW_SID);
    tb_rewrite_init(&ur, TB_RADIO, u_dst, 1, NEW_SID);

    CHECK(gr.lc[0] == 0x00 && ur.lc[0] == 0x03, "FLCO not 0x00 group / 0x03 unit");
    CHECK(memcmp(gr.lc + 6, ur.lc + 6, 3) == 0, "source in LC should be identical");

    for (int i = 0; i < n; i++) {
        uint8_t go[DMRD_LEN], uo[DMRD_LEN];
        tb_rewrite_packet(&gr, in + i*DMRD_LEN, go, (uint8_t)i);
        tb_rewrite_packet(&ur, in + i*DMRD_LEN, uo, (uint8_t)i);

        CHECK(memcmp(go, uo, DMRD_DST_OFF) == 0, "pkt %d: bytes before dst differ", i);
        CHECK(memcmp(go + DMRD_RPTR_OFF, uo + DMRD_RPTR_OFF,
                     DMRD_FLAGS_OFF - DMRD_RPTR_OFF) == 0, "pkt %d: repeater differs", i);
        CHECK((go[DMRD_FLAGS_OFF] & ~HBPF_TGID_CALL_P) ==
              (uo[DMRD_FLAGS_OFF] & ~HBPF_TGID_CALL_P),
              "pkt %d: flags differ outside the call-type bit", i);

        uint8_t fl = in[i*DMRD_LEN + DMRD_FLAGS_OFF];
        int ftyp = fl & HBPF_FRAMETYPE_MASK, dtyp = fl & HBPF_DTYPE_MASK;
        int carries_lc = (ftyp == HBPF_FRAMETYPE_DATASYNC &&
                          (dtyp == HBPF_SLT_VHEAD || dtyp == HBPF_SLT_VTERM)) ||
                         (ftyp == HBPF_FRAMETYPE_VOICE && dtyp >= 1 && dtyp <= 4);
        if (!carries_lc)
            CHECK(memcmp(go + DMRD_PAYLOAD_OFF, uo + DMRD_PAYLOAD_OFF, 33) == 0,
                  "pkt %d: non-LC payload differs between group and unit", i);
    }
}

int main(void)
{
    printf("dmr-talkback rewrite conformance\n\n");

    run_case("group call -> replayed onto the same talkgroup",
             0, ORIG_TGID, ORIG_TGID);
    run_case("unit call  -> private reply to the original caller",
             1, TB_RADIO, ORIG_SRC);
    run_diff_case();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
