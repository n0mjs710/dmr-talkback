/* test_lanes.c — ingress gate and lane isolation.
 *
 * This is the test for the concurrency model. A DMR timeslot carries one call
 * at a time, so the program runs one capture/replay lane per slot. Two things
 * have to hold:
 *
 *   1. A user on TS1 and a user on TS2 are both served, simultaneously and
 *      independently. (The first version of this program had a single global
 *      capture buffer and got this catastrophically wrong: interleaved packets
 *      from two streams reset the buffer on every frame and it captured
 *      nothing. That is also exactly the bug the 2019 Python playback.py had.)
 *
 *   2. Two streams arriving on ONE lane never thrash it. The wire should not
 *      produce that — the server arbitrates per slot — but if it does,
 *      first-come-wins and the capture in progress survives intact.
 *
 * Plus the ingress gate: wrong talkgroup, unused slot, and private calls are
 * all rejected without opening a capture.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "talkback.h"
#include "hbp.h"
#include "hbp_const.h"

/* The lanes under test never reach the wire. */
void hbp_send_dmrd(struct hbp *hb, const uint8_t *data, int len) { (void)hb; (void)data; (void)len; }
int  hbp_is_connected(struct hbp *hb) { (void)hb; return 0; }

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, ...) do { \
    checks++; \
    if (!(cond)) { failures++; printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

#define TB_RADIO   3120099u
#define TG_TS1     9u
#define TG_TS2     9990u
#define CALLER_A   3121234u
#define CALLER_B   3125678u

static void wr24(uint8_t *p, uint32_t v) { p[0]=(uint8_t)(v>>16); p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)v; }
static void wr32(uint8_t *p, uint32_t v) { p[0]=(uint8_t)(v>>24); p[1]=(uint8_t)(v>>16); p[2]=(uint8_t)(v>>8); p[3]=(uint8_t)v; }

/* One voice packet (burst B: VOICE frame, vseq 1 — carries embedded LC). */
static void voice_pkt(uint8_t *out, int slot, int call_p, uint32_t src,
                      uint32_t dst, uint32_t sid, int vseq)
{
    memset(out, 0, DMRD_LEN);
    memcpy(out, "DMRD", 4);
    wr24(out + DMRD_SRC_OFF, src);
    wr24(out + DMRD_DST_OFF, dst);
    wr32(out + DMRD_RPTR_OFF, 312999);
    out[DMRD_FLAGS_OFF] = (uint8_t)((slot == 2 ? HBPF_TGID_TS2 : 0) |
                                    (call_p ? HBPF_TGID_CALL_P : 0) |
                                    HBPF_FRAMETYPE_VOICE | (vseq & 0x0F));
    wr32(out + DMRD_STREAM_OFF, sid);
}

static void feed(tb_instance *in, int slot, int call_p, uint32_t src,
                 uint32_t dst, uint32_t sid, int n)
{
    uint8_t pkt[DMRD_LEN];
    for (int i = 0; i < n; i++) {
        voice_pkt(pkt, slot, call_p, src, dst, sid, 1 + (i % 4));
        tb_hbp_voice_received(in, pkt, DMRD_LEN);
    }
}

static void mk_cfg(InstanceCfg *ic)
{
    memset(ic, 0, sizeof *ic);
    snprintf(ic->name, sizeof ic->name, "%s", "test");
    ic->radio_id         = TB_RADIO;
    ic->slot1_tgid       = TG_TS1;
    ic->slot2_tgid       = TG_TS2;
    ic->replay_delay     = 2.0;
    ic->max_capture_secs = 2;          /* small: ~34 packets, exercises the ceiling */
    snprintf(ic->master_ip, sizeof ic->master_ip, "%s", "127.0.0.1");
    ic->master_port = 54000;
}

/* ------------------------------------------------------------------ */

static void test_lane_isolation(ev_loop *loop)
{
    printf("TS1 and TS2 capture independently and simultaneously\n");

    InstanceCfg ic; mk_cfg(&ic);
    tb_instance *in = instance_new(&ic, loop);
    CHECK(in != NULL, "instance_new failed");
    if (!in) return;

    /* Interleave two live calls, one per slot — the exact pattern that broke
     * the single-buffer version. */
    uint8_t p1[DMRD_LEN], p2[DMRD_LEN];
    for (int i = 0; i < 10; i++) {
        voice_pkt(p1, 1, 0, CALLER_A, TG_TS1, 0xAAAA0001u, 1 + (i % 4));
        tb_hbp_voice_received(in, p1, DMRD_LEN);
        voice_pkt(p2, 2, 0, CALLER_B, TG_TS2, 0xBBBB0002u, 1 + (i % 4));
        tb_hbp_voice_received(in, p2, DMRD_LEN);
    }

    CHECK(instance_lane_captured(in, 1) == 10,
          "TS1 captured %d packets, expected 10", instance_lane_captured(in, 1));
    CHECK(instance_lane_captured(in, 2) == 10,
          "TS2 captured %d packets, expected 10", instance_lane_captured(in, 2));

    instance_free(in);
}

static void test_same_lane_no_thrash(ev_loop *loop)
{
    printf("two streams on one lane do not thrash it (first-come-wins)\n");

    InstanceCfg ic; mk_cfg(&ic);
    tb_instance *in = instance_new(&ic, loop);
    if (!in) { CHECK(0, "instance_new failed"); return; }

    /* Stream A establishes itself, then B interleaves on the same slot. */
    feed(in, 2, 0, CALLER_A, TG_TS2, 0xAAAA0001u, 5);
    CHECK(instance_lane_captured(in, 2) == 5, "setup: expected 5 packets");

    uint8_t pa[DMRD_LEN], pb[DMRD_LEN];
    for (int i = 0; i < 10; i++) {
        voice_pkt(pb, 2, 0, CALLER_B, TG_TS2, 0xBBBB0002u, 1 + (i % 4));
        tb_hbp_voice_received(in, pb, DMRD_LEN);
        voice_pkt(pa, 2, 0, CALLER_A, TG_TS2, 0xAAAA0001u, 1 + (i % 4));
        tb_hbp_voice_received(in, pa, DMRD_LEN);
    }

    /* A keeps the lane and accumulates; B is ignored entirely. */
    CHECK(instance_lane_captured(in, 2) == 15,
          "expected 15 packets (5 + 10 from stream A), got %d",
          instance_lane_captured(in, 2));

    instance_free(in);
}

static void test_ingress_gate(ev_loop *loop)
{
    printf("ingress gate rejects wrong TG, unused slot, and private calls\n");

    InstanceCfg ic; mk_cfg(&ic);
    ic.slot1_tgid = 0;                 /* TS1 unused for this case */
    tb_instance *in = instance_new(&ic, loop);
    if (!in) { CHECK(0, "instance_new failed"); return; }

    feed(in, 1, 0, CALLER_A, TG_TS1, 0x11110001u, 5);      /* slot unused */
    CHECK(instance_lane_captured(in, 1) == 0, "unused slot captured traffic");

    feed(in, 2, 0, CALLER_A, 1234u, 0x22220002u, 5);       /* wrong talkgroup */
    CHECK(instance_lane_captured(in, 2) == 0, "wrong talkgroup was captured");

    feed(in, 2, 1, CALLER_A, TB_RADIO, 0x33330003u, 5);    /* private call to us */
    CHECK(instance_lane_captured(in, 2) == 0, "private call was captured");

    feed(in, 2, 0, CALLER_A, TG_TS2, 0x44440004u, 5);      /* the real thing */
    CHECK(instance_lane_captured(in, 2) == 5, "valid group call was not captured");

    instance_free(in);
}

static void test_ceiling(ev_loop *loop)
{
    printf("capture stops at max_capture_secs without overrunning the buffer\n");

    InstanceCfg ic; mk_cfg(&ic);           /* 2 s => 34 packets */
    tb_instance *in = instance_new(&ic, loop);
    if (!in) { CHECK(0, "instance_new failed"); return; }

    int limit = (int)(ic.max_capture_secs / TB_FRAME_SECS);   /* 33 */
    feed(in, 2, 0, CALLER_A, TG_TS2, 0x55550005u, limit + 50);

    /* Hitting the ceiling closes the capture and hands it to replay, which is
     * armed on a timer — so the lane is replaying and holds no more than the
     * buffer's worth. */
    CHECK(instance_lane_captured(in, 2) <= limit + 1,
          "captured %d packets, buffer holds at most %d",
          instance_lane_captured(in, 2), limit + 1);
    CHECK(instance_lane_replaying(in, 2) == 1,
          "hitting the ceiling should have started a replay");

    instance_free(in);
}

int main(void)
{
    printf("dmr-talkback lane isolation and ingress gate\n\n");

    ev_loop *loop = ev_new();

    test_lane_isolation(loop);
    test_same_lane_no_thrash(loop);
    test_ingress_gate(loop);
    test_ceiling(loop);

    ev_free(loop);

    printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
