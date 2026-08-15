/* test_rewrite.c — the loopback-identity conformance vector.
 *
 * The whole correctness claim of this program is:
 *
 *   the audio that comes back is bit-identical to the audio that went in,
 *   and everything identifying the call is rewritten to us — in the DMRD
 *   header AND in both LC carriers inside the payload.
 *
 * These tests assert exactly that over a synthetic capture covering every
 * frame kind a real stream contains.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "talkback.h"
#include "hbp_const.h"
#include "dmr/dmr.h"

/* replay.c transmits through the HBP client and reads back through the
 * instance accessors; the rewrite path under test does neither, so stubs are
 * enough to link. */
void hbp_send_dmrd(struct hbp *hb, const uint8_t *data, int len) { (void)hb; (void)data; (void)len; }
int  hbp_is_connected(struct hbp *hb) { (void)hb; return 0; }
const char *instance_name(const tb_instance *in) { (void)in; return "test"; }
struct hbp *instance_hbp(const tb_instance *in) { (void)in; return NULL; }
const InstanceCfg *instance_cfg(const tb_instance *in) { (void)in; return NULL; }
ev_loop *instance_loop(const tb_instance *in) { (void)in; return NULL; }

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

/* VHEAD + 2 superframes (A..F) + VTERM = 14 packets. */
#define N_PKTS 14
static int build_capture(uint8_t *buf, int slot, int call_p, uint32_t dst)
{
    uint8_t base = (uint8_t)((slot == 2 ? HBPF_TGID_TS2 : 0) |
                             (call_p ? HBPF_TGID_CALL_P : 0));
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

static void expected_lc(uint8_t lc[9], uint32_t dst, uint32_t src)
{
    lc[0] = 0x00;                    /* FLCO: group voice — always */
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

/* `call_p` seeds the captured packets with the private-call bit set, to prove
 * the rewrite forces it clear.  Real captures never carry it — the ingress
 * gate rejects private calls — but the header must never be able to disagree
 * with the group FLCO we build. */
static void run_case(const char *label, int slot, int call_p, uint32_t tgid)
{
    printf("%s\n", label);

    uint8_t in[N_PKTS * DMRD_LEN];
    int n = build_capture(in, slot, call_p, tgid);

    uint8_t dst3[3]; wr24(dst3, tgid);
    tb_rewrite rw;
    tb_rewrite_init(&rw, TB_RADIO, dst3, NEW_SID);

    uint8_t exp_lc[9];
    expected_lc(exp_lc, tgid, TB_RADIO);
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
        CHECK(rd24(op + DMRD_DST_OFF) == tgid,
              "pkt %d: dst %u != %u", i, rd24(op + DMRD_DST_OFF), tgid);
        CHECK(rd32(op + DMRD_RPTR_OFF) == TB_RADIO,
              "pkt %d: repeater %u != %u", i, rd32(op + DMRD_RPTR_OFF), TB_RADIO);
        CHECK(rd32(op + DMRD_STREAM_OFF) == NEW_SID, "pkt %d: stream ID not rewritten", i);
        CHECK(op[DMRD_BER_OFF] == 0 && op[DMRD_RSSI_OFF] == 0, "pkt %d: BER/RSSI not zeroed", i);

        uint8_t ifl = ip[DMRD_FLAGS_OFF], ofl = op[DMRD_FLAGS_OFF];
        CHECK((ofl & HBPF_TGID_TS2) == (ifl & HBPF_TGID_TS2), "pkt %d: slot bit changed", i);
        CHECK((ofl & HBPF_FRAMETYPE_MASK) == (ifl & HBPF_FRAMETYPE_MASK), "pkt %d: frame type changed", i);
        CHECK((ofl & HBPF_DTYPE_MASK) == (ifl & HBPF_DTYPE_MASK), "pkt %d: dtype/vseq changed", i);
        CHECK((ofl & HBPF_TGID_CALL_P) == 0, "pkt %d: private-call bit not forced clear", i);

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

/* Two lanes rewritten independently must not bleed into each other: each keeps
 * its own slot bit and its own talkgroup. */
static void run_lane_independence(void)
{
    printf("TS1 and TS2 rewrites stay on their own slot and talkgroup\n");

    uint8_t in1[N_PKTS * DMRD_LEN], in2[N_PKTS * DMRD_LEN];
    int n1 = build_capture(in1, 1, 0, 9u);
    int n2 = build_capture(in2, 2, 0, ORIG_TGID);
    CHECK(n1 == n2, "capture lengths differ");

    uint8_t d1[3], d2[3];
    wr24(d1, 9u); wr24(d2, ORIG_TGID);
    tb_rewrite rw1, rw2;
    tb_rewrite_init(&rw1, TB_RADIO, d1, 0xAAAA0001u);
    tb_rewrite_init(&rw2, TB_RADIO, d2, 0xBBBB0002u);

    for (int i = 0; i < n1; i++) {
        uint8_t o1[DMRD_LEN], o2[DMRD_LEN];
        tb_rewrite_packet(&rw1, in1 + i*DMRD_LEN, o1, (uint8_t)i);
        tb_rewrite_packet(&rw2, in2 + i*DMRD_LEN, o2, (uint8_t)i);

        CHECK((o1[DMRD_FLAGS_OFF] & HBPF_TGID_TS2) == 0, "pkt %d: TS1 reply left TS1", i);
        CHECK((o2[DMRD_FLAGS_OFF] & HBPF_TGID_TS2) != 0, "pkt %d: TS2 reply left TS2", i);
        CHECK(rd24(o1 + DMRD_DST_OFF) == 9u, "pkt %d: TS1 reply wrong TG", i);
        CHECK(rd24(o2 + DMRD_DST_OFF) == ORIG_TGID, "pkt %d: TS2 reply wrong TG", i);
        CHECK(rd32(o1 + DMRD_STREAM_OFF) != rd32(o2 + DMRD_STREAM_OFF),
              "pkt %d: lanes share a stream ID", i);
    }
}

int main(void)
{
    printf("dmr-talkback rewrite conformance\n\n");

    run_case("group call on TS2 -> replayed onto the same talkgroup", 2, 0, ORIG_TGID);
    run_case("group call on TS1 -> replayed onto the same talkgroup", 1, 0, 9u);
    run_case("stray private-call bit is forced clear on the reply",   2, 1, ORIG_TGID);
    run_lane_independence();

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
