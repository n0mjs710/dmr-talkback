/* replay.c — packet rewrite and clocked egress.
 *
 * Two jobs:
 *
 *  1. Rewrite.  A captured stream is replayed as a NEW stream sourced from our
 *     own radio ID.  That means rewriting the DMRD header *and* the Link
 *     Control carried inside the 33-byte payload.  Rewriting only the header
 *     is what every earlier implementation of this program did, and it leaves
 *     the header and the payload disagreeing about who is calling: radios and
 *     MMDVMHost decode the payload LC for display and late entry, so the echo
 *     comes back showing the original caller.  For unit calls it is worse —
 *     the FLCO has to flip to unit-voice or the receiving radio treats the
 *     private reply as a group call.
 *
 *     AMBE is never touched.  Only LC windows are replaced, so the audio that
 *     comes back is bit-identical to the audio that went in.
 *
 *  2. Clock.  Packets go out one per 60 ms off the event loop.  Never a
 *     blocking sleep — that is what stalled the Python versions' reactor for
 *     the whole duration of the playback.
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
#define LC_LOW_START     0    /* [0:98]    first half of the 196-bit BPTC LC  */
#define LC_LOW_END      98
#define SYNC_START      98    /* [98:166]  slot type + sync — PRESERVED       */
#define SYNC_END       166
#define LC_HIGH_START  166    /* [166:264] second half of the BPTC LC         */
#define LC_HIGH_END    264
#define EMB_START      116    /* [116:148] 32-bit embedded-LC fragment        */
#define EMB_END        148

struct replay {
    const Config *cfg;
    ev_loop      *loop;
    struct hbp  **hb_slot;      /* indirect: the hbp is created after us */

    int        active;
    int        idx;             /* next packet index to send */
    int        n;               /* packets to send */
    uint8_t   *pkts;            /* our own copy of the captured stream */
    int        cap;             /* capacity in packets */
    uint8_t    seq;             /* replay-relative DMRD sequence */
    tb_rewrite rw;
    ev_timer  *timer;
    double     started;
};

static uint32_t rd24(const uint8_t *p) {
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

/* ---------------- rewrite (pure; exercised directly by tests) ------------- */

void tb_rewrite_init(tb_rewrite *rw, uint32_t radio_id, const uint8_t dst[3],
                     int is_unit, uint32_t stream_id)
{
    memset(rw, 0, sizeof *rw);

    rw->src[0] = (uint8_t)(radio_id >> 16);
    rw->src[1] = (uint8_t)(radio_id >> 8);
    rw->src[2] = (uint8_t)(radio_id);
    memcpy(rw->dst, dst, 3);

    /* The DMRD repeater field is the same ID.  Masters that check it against
     * the logged-in repeater ID therefore accept the stream. */
    rw->rptr[0] = (uint8_t)(radio_id >> 24);
    rw->rptr[1] = (uint8_t)(radio_id >> 16);
    rw->rptr[2] = (uint8_t)(radio_id >> 8);
    rw->rptr[3] = (uint8_t)(radio_id);

    rw->stream_id = stream_id;
    rw->is_unit   = is_unit;

    /* The reply LC, built from scratch — nothing is carried over from the
     * caller.  FLCO 0x00 group / 0x03 unit, FID 0, service options 0.
     * (dmr_utils3 const.py LC_OPT_G / LC_OPT_U.) */
    rw->lc[0] = is_unit ? 0x03 : 0x00;
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
     * same sequence of frame kinds as the capture.  Only the call-type bit
     * changes, and only because the reply may differ from... nothing, in
     * practice: the reply mirrors the inbound type.  Set it explicitly anyway
     * so the header can never disagree with the FLCO we just built. */
    if (rw->is_unit) out[DMRD_FLAGS_OFF] |=  HBPF_TGID_CALL_P;
    else             out[DMRD_FLAGS_OFF] &= (uint8_t)~HBPF_TGID_CALL_P;

    /* No radio, no receiver: these are ours to zero. */
    out[DMRD_BER_OFF]  = 0;
    out[DMRD_RSSI_OFF] = 0;

    /* ---- payload LC ---- */
    uint8_t flags = in[DMRD_FLAGS_OFF];
    int ftyp = flags & HBPF_FRAMETYPE_MASK;
    int dtyp = flags & HBPF_DTYPE_MASK;

    const dmr_bit *full_lc = NULL;
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
        memcpy(bits + LC_LOW_START,  full_lc,      LC_LOW_END - LC_LOW_START);
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

static void arm_tick(replay *rp, double delay) {
    rp->timer = ev_timer_after(rp->loop, delay, tick_cb, rp);
}

static void finish(replay *rp) {
    double dur = ev_now(rp->loop) - rp->started;
    LOGI(LOGN, "replay end    — %d packets, %.1fs", rp->n, dur);
    rp->active = 0;
    rp->idx = 0;
    rp->n = 0;
}

static void tick_cb(ev_loop *loop, void *ud) {
    replay *rp = ud;
    rp->timer = NULL;
    if (!rp->active) return;

    struct hbp *hb = *rp->hb_slot;
    if (!hb || !hbp_is_connected(hb)) {
        LOGW(LOGN, "replay aborted — HBP link down");
        finish(rp);
        return;
    }

    if (rp->idx == 0)
        LOGI(LOGN, "replay start  — %s call to %u from %u, %d packets, stream %08x",
             rp->rw.is_unit ? "unit" : "group", rd24(rp->rw.dst), rd24(rp->rw.src),
             rp->n, rp->rw.stream_id);

    uint8_t out[DMRD_LEN];
    tb_rewrite_packet(&rp->rw, rp->pkts + (size_t)rp->idx * DMRD_LEN, out, rp->seq++);
    hbp_send_dmrd(hb, out, DMRD_LEN);

    rp->idx++;
    if (rp->idx >= rp->n) { finish(rp); return; }
    arm_tick(rp, TB_FRAME_SECS);
    (void)loop;
}

static void begin_cb(ev_loop *loop, void *ud) {
    replay *rp = ud;
    rp->timer = NULL;
    rp->started = ev_now(loop);
    rp->idx = 0;
    rp->seq = 0;
    tick_cb(loop, rp);
}

/* ---------------- public API ---------------- */

replay *replay_new(const Config *cfg, ev_loop *loop, struct hbp **hb_slot) {
    replay *rp = calloc(1, sizeof *rp);
    if (!rp) return NULL;
    rp->cfg = cfg; rp->loop = loop; rp->hb_slot = hb_slot;
    rp->cap  = (int)((double)cfg->max_capture_secs / TB_FRAME_SECS) + 2;
    rp->pkts = calloc((size_t)rp->cap, DMRD_LEN);
    if (!rp->pkts) { free(rp); return NULL; }
    return rp;
}

void replay_free(replay *rp) {
    if (!rp) return;
    if (rp->timer) ev_timer_cancel(rp->loop, rp->timer);
    free(rp->pkts);
    free(rp);
}

int replay_active(const replay *rp) { return rp->active; }

void replay_abort(replay *rp) {
    if (rp->timer) { ev_timer_cancel(rp->loop, rp->timer); rp->timer = NULL; }
    rp->active = 0;
    rp->idx = 0;
    rp->n = 0;
}

void replay_start(replay *rp, const tb_capture *capd) {
    if (rp->active) { LOGW(LOGN, "replay already in flight — ignoring"); return; }
    if (capd->n <= 0) return;

    int n = capd->n;
    if (n > rp->cap) n = rp->cap;
    memcpy(rp->pkts, capd->pkts, (size_t)n * DMRD_LEN);
    rp->n = n;

    /* The reply mirrors the inbound call type; only the destination differs.
     *   group in  -> back onto the same talkgroup
     *   unit  in  -> a private call back to whoever called us
     * Both are sourced from our own radio ID. */
    const uint8_t *dst = capd->is_unit ? capd->src : capd->dst;

    uint32_t sid = ((uint32_t)rand() << 16) ^ (uint32_t)rand();
    if (sid == 0) sid = 1;
    tb_rewrite_init(&rp->rw, rp->cfg->radio_id, dst, capd->is_unit, sid);

    rp->active = 1;
    rp->timer = ev_timer_after(rp->loop, rp->cfg->replay_delay, begin_cb, rp);
}
