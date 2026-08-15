/* replay.c — packet rewrite and clocked egress, per lane.
 *
 * Two jobs:
 *
 *  1. Rewrite.  A captured stream is replayed as a NEW stream sourced from our
 *     own radio ID.  That means rewriting the DMRD header *and* the Link
 *     Control carried inside the 33-byte payload.  Rewriting only the header
 *     is what every earlier implementation of this program did, and it leaves
 *     the header and the payload disagreeing about who is calling: radios and
 *     MMDVMHost decode the payload LC for display and late entry, so the echo
 *     comes back showing the original caller.
 *
 *     AMBE is never touched.  Only LC windows are replaced, so the audio that
 *     comes back is bit-identical to the audio that went in.
 *
 *  2. Clock.  Packets go out one per 60 ms off the event loop.  Never a
 *     blocking sleep — that is what stalled the Python versions' reactor for
 *     the whole duration of the playback.
 *
 * Each lane clocks independently, so a replay on TS1 and a replay on TS2 run
 * at the same time without interacting.
 */
#include "talkback.h"
#include "hbp.h"
#include "hbp_const.h"
#include "log.h"
#include "dmr/dmr.h"
#include <stdlib.h>
#include <string.h>

#define LOGN "talkback.replay"

/* Payload is 33 bytes = 264 bits at DMRD_PAYLOAD_OFF. */
#define PAYLOAD_BITS   264
#define FULL_LC_BITS   196

/* Bit windows inside the payload (see hblink4 hblink4/lc.py). */
#define LC_LOW_END      98    /* [0:98]    first half of the 196-bit BPTC LC  */
#define SYNC_START      98    /* [98:166]  slot type + sync — PRESERVED       */
#define LC_HIGH_START  166    /* [166:264] second half of the BPTC LC         */
#define EMB_START      116    /* [116:148] 32-bit embedded-LC fragment        */
#define EMB_END        148

static uint32_t rd24(const uint8_t *p) {
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

/* ---------------- rewrite (pure; exercised directly by tests) ------------- */

void tb_rewrite_init(tb_rewrite *rw, uint32_t radio_id, const uint8_t dst[3],
                     uint32_t stream_id)
{
    memset(rw, 0, sizeof *rw);

    rw->src[0] = (uint8_t)(radio_id >> 16);
    rw->src[1] = (uint8_t)(radio_id >> 8);
    rw->src[2] = (uint8_t)(radio_id);
    memcpy(rw->dst, dst, 3);

    /* The DMRD repeater field is the same ID.  Servers that check it against
     * the logged-in repeater ID therefore accept the stream. */
    rw->rptr[0] = (uint8_t)(radio_id >> 24);
    rw->rptr[1] = (uint8_t)(radio_id >> 16);
    rw->rptr[2] = (uint8_t)(radio_id >> 8);
    rw->rptr[3] = (uint8_t)(radio_id);

    rw->stream_id = stream_id;

    /* The reply LC, built from scratch — nothing is carried over from the
     * caller.  FLCO 0x00 group voice, FID 0, service options 0
     * (dmr_utils3 const.py LC_OPT_G). */
    rw->lc[0] = 0x00;
    rw->lc[1] = 0x00;
    rw->lc[2] = 0x00;
    memcpy(rw->lc + 3, rw->dst, 3);
    memcpy(rw->lc + 6, rw->src, 3);

    /* Encode once per replay, not per packet.  dmr_bptc_encode_lc computes the
     * RS(12,9) parity and applies the header/terminator mask internally. */
    dmr_bptc_encode_lc(rw->lc, 0, rw->h_lc);
    dmr_bptc_encode_lc(rw->lc, 1, rw->t_lc);
    dmr_encode_emblc(rw->lc, rw->emb);
}

void tb_rewrite_packet(const tb_rewrite *rw, const uint8_t *in, uint8_t *out, uint8_t seq)
{
    memcpy(out, in, DMRD_LEN);

    /* ---- header ---- */
    out[DMRD_SEQ_OFF] = seq;
    memcpy(out + DMRD_SRC_OFF,  rw->src,  3);
    memcpy(out + DMRD_DST_OFF,  rw->dst,  3);
    memcpy(out + DMRD_RPTR_OFF, rw->rptr, 4);
    out[DMRD_STREAM_OFF + 0] = (uint8_t)(rw->stream_id >> 24);
    out[DMRD_STREAM_OFF + 1] = (uint8_t)(rw->stream_id >> 16);
    out[DMRD_STREAM_OFF + 2] = (uint8_t)(rw->stream_id >> 8);
    out[DMRD_STREAM_OFF + 3] = (uint8_t)(rw->stream_id);

    /* Slot bit, frame type and dtype/vseq are preserved — the replay is the
     * same sequence of frame kinds, on the same slot, as the capture.  The
     * call-type bit is forced clear: every reply is a group call, and the
     * header must never disagree with the FLCO we just built. */
    out[DMRD_FLAGS_OFF] &= (uint8_t)~HBPF_TGID_CALL_P;

    /* No radio, no receiver: these are ours to zero. */
    out[DMRD_BER_OFF]  = 0;
    out[DMRD_RSSI_OFF] = 0;

    /* ---- payload LC ---- */
    uint8_t flags = in[DMRD_FLAGS_OFF];
    int ftyp = flags & HBPF_FRAMETYPE_MASK;
    int dtyp = flags & HBPF_DTYPE_MASK;

    const dmr_bit *full_lc  = NULL;
    const uint8_t *emb_frag = NULL;

    if (ftyp == HBPF_FRAMETYPE_DATASYNC) {
        if      (dtyp == HBPF_SLT_VHEAD) full_lc = rw->h_lc;
        else if (dtyp == HBPF_SLT_VTERM) full_lc = rw->t_lc;
    } else if (ftyp == HBPF_FRAMETYPE_VOICE) {
        /* Bursts B..E carry an embedded-LC fragment; vseq 1..4.  Burst A is a
         * VOICESYNC frame and burst F (vseq 5) carries a null fragment — both
         * are left alone. */
        if (dtyp >= 1 && dtyp <= 4) emb_frag = rw->emb[dtyp - 1];
    }

    if (!full_lc && !emb_frag) return;   /* voice burst A/F, CSBK, data: untouched */

    dmr_bit bits[PAYLOAD_BITS];
    dmr_bytes_to_bits(in + DMRD_PAYLOAD_OFF, 33, bits);

    if (full_lc) {
        /* [0:98] and [166:264] replaced; [98:166] (slot type + sync, which is
         * where the colour code lives) deliberately untouched. */
        memcpy(bits,                 full_lc,      LC_LOW_END);
        memcpy(bits + LC_HIGH_START, full_lc + 98, FULL_LC_BITS - 98);
    } else {
        dmr_bit fb[32];
        dmr_bytes_to_bits(emb_frag, 4, fb);
        memcpy(bits + EMB_START, fb, EMB_END - EMB_START);
    }

    dmr_bits_to_bytes(bits, PAYLOAD_BITS, out + DMRD_PAYLOAD_OFF);
}

/* ---------------- clocked egress ---------------- */

static void tick_cb(ev_loop *loop, void *ud);

static void finish(tb_lane *ln) {
    LOGI(LOGN, "[%s] TS%d replay end    — %d packets, %.1fs",
         instance_name(ln->inst), ln->slot, ln->rp.n,
         ev_now(instance_loop(ln->inst)) - ln->rp.started);
    ln->rp.active = 0;
    ln->rp.idx = 0;
    ln->rp.n = 0;
}

static void tick_cb(ev_loop *loop, void *ud) {
    tb_lane *ln = ud;
    ln->rp.timer = NULL;
    if (!ln->rp.active) return;

    struct hbp *hb = instance_hbp(ln->inst);
    if (!hb || !hbp_is_connected(hb)) {
        LOGW(LOGN, "[%s] TS%d replay aborted — link down", instance_name(ln->inst), ln->slot);
        finish(ln);
        return;
    }

    uint8_t out[DMRD_LEN];
    tb_rewrite_packet(&ln->rp.rw, ln->rp.pkts + (size_t)ln->rp.idx * DMRD_LEN,
                      out, ln->rp.seq++);
    hbp_send_dmrd(hb, out, DMRD_LEN);

    ln->rp.idx++;
    if (ln->rp.idx >= ln->rp.n) { finish(ln); return; }
    ln->rp.timer = ev_timer_after(loop, TB_FRAME_SECS, tick_cb, ln);
}

static void begin_cb(ev_loop *loop, void *ud) {
    tb_lane *ln = ud;
    ln->rp.timer = NULL;
    ln->rp.started = ev_now(loop);
    ln->rp.idx = 0;
    ln->rp.seq = 0;
    LOGI(LOGN, "[%s] TS%d replay start  — TG %u from %u, %d packets, stream %08x",
         instance_name(ln->inst), ln->slot, rd24(ln->rp.rw.dst), rd24(ln->rp.rw.src),
         ln->rp.n, ln->rp.rw.stream_id);
    tick_cb(loop, ln);
}

/* ---------------- public API ---------------- */

int lane_replay_init(tb_lane *ln, int max_capture_secs) {
    ln->rp.cap  = (int)((double)max_capture_secs / TB_FRAME_SECS) + 2;
    ln->rp.pkts = calloc((size_t)ln->rp.cap, DMRD_LEN);
    return ln->rp.pkts ? 0 : -1;
}

void lane_replay_cleanup(tb_lane *ln) {
    if (ln->rp.timer) ev_timer_cancel(instance_loop(ln->inst), ln->rp.timer);
    ln->rp.timer = NULL;
    free(ln->rp.pkts);
    ln->rp.pkts = NULL;
}

int lane_replay_active(const tb_lane *ln) { return ln->rp.active; }

void lane_replay_abort(tb_lane *ln) {
    if (ln->rp.timer) { ev_timer_cancel(instance_loop(ln->inst), ln->rp.timer); ln->rp.timer = NULL; }
    ln->rp.active = 0;
    ln->rp.idx = 0;
    ln->rp.n = 0;
}

void lane_replay_start(tb_lane *ln) {
    if (ln->rp.active) { LOGW(LOGN, "[%s] TS%d replay already in flight — ignoring",
                              instance_name(ln->inst), ln->slot); return; }
    if (ln->cap.n <= 0) return;

    int n = ln->cap.n;
    if (n > ln->rp.cap) n = ln->rp.cap;
    memcpy(ln->rp.pkts, ln->cap.pkts, (size_t)n * DMRD_LEN);
    ln->rp.n = n;

    /* The reply goes back onto the talkgroup it was captured from, sourced
     * from our own radio ID. */
    uint8_t dst[3] = { (uint8_t)(ln->tgid >> 16), (uint8_t)(ln->tgid >> 8), (uint8_t)ln->tgid };

    uint32_t sid = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
    if (sid == 0) sid = 1;
    tb_rewrite_init(&ln->rp.rw, instance_cfg(ln->inst)->radio_id, dst, sid);

    ln->rp.active = 1;
    ln->rp.timer = ev_timer_after(instance_loop(ln->inst),
                                  instance_cfg(ln->inst)->replay_delay, begin_cb, ln);
}
