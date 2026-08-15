/* capture.c — the talkback object: ingress gate and capture buffer.
 *
 * Receives DMRD from the HBP client, decides whether the stream is one we
 * answer, buffers it verbatim, and hands the finished buffer to replay.c.
 */
#include "talkback.h"
#include "hbp.h"
#include "hbp_const.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define LOGN "talkback"

struct talkback {
    const Config *cfg;
    ev_loop      *loop;
    struct hbp   *hb;
    tb_capture    cap;
    replay       *rp;
    ev_timer     *sweep_timer;
    uint8_t       radio_id[3];   /* 24-bit form, for unit-call matching */
};

static uint32_t rd24(const uint8_t *p) {
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void close_capture(talkback *tb, const char *why);
static void sweep_cb(ev_loop *loop, void *ud);

static void arm_sweep(talkback *tb) {
    if (tb->sweep_timer) return;
    tb->sweep_timer = ev_timer_after(tb->loop, TB_STREAM_TIMEOUT, sweep_cb, tb);
}

/* Watchdog for a stream whose terminator never arrived.  Without this a lost
 * tail would pin the buffer and the talkback would go deaf. */
static void sweep_cb(ev_loop *loop, void *ud) {
    talkback *tb = ud;
    tb->sweep_timer = NULL;
    if (!tb->cap.active) return;
    if (ev_now(loop) - tb->cap.last_pkt >= TB_STREAM_TIMEOUT) {
        close_capture(tb, "no terminator (lost tail)");
        return;
    }
    arm_sweep(tb);
}

static void close_capture(talkback *tb, const char *why) {
    if (!tb->cap.active) return;
    tb->cap.active = 0;
    if (tb->cap.n == 0) return;

    double dur = tb->cap.last_pkt - tb->cap.started;
    LOGI(LOGN, "capture end   — %s, %d packets, %.1fs", why, tb->cap.n, dur);
    replay_start(tb->rp, &tb->cap);
}

/* ---------------- ingress gate ---------------- */

/* Decide whether this packet belongs to a stream we answer.  Returns 1 to
 * accept, 0 to ignore.  Fills *is_unit and *slot. */
static int gate(talkback *tb, const uint8_t *pkt, int *is_unit, int *slot) {
    uint8_t flags = pkt[DMRD_FLAGS_OFF];
    *slot    = (flags & HBPF_TGID_TS2) ? 2 : 1;
    *is_unit = (flags & HBPF_TGID_CALL_P) ? 1 : 0;

    const Config *c = tb->cfg;
    if (*is_unit) {
        if (!cfg_does_unit(c)) return 0;
        if (!cfg_unit_slot_ok(c, *slot)) return 0;
        /* A private call is for us only if it is addressed to our radio ID. */
        if (memcmp(pkt + DMRD_DST_OFF, tb->radio_id, 3) != 0) return 0;
        return 1;
    }
    if (!cfg_does_group(c)) return 0;
    return cfg_group_match(c, *slot, rd24(pkt + DMRD_DST_OFF));
}

/* ---------------- public API ---------------- */

talkback *talkback_new(const Config *cfg, ev_loop *loop) {
    talkback *tb = calloc(1, sizeof *tb);
    if (!tb) return NULL;
    tb->cfg  = cfg;
    tb->loop = loop;
    tb->radio_id[0] = (uint8_t)(cfg->radio_id >> 16);
    tb->radio_id[1] = (uint8_t)(cfg->radio_id >> 8);
    tb->radio_id[2] = (uint8_t)(cfg->radio_id);

    /* Fixed buffer, sized once, from the configured ceiling. */
    tb->cap.cap  = (int)ceil((double)cfg->max_capture_secs / TB_FRAME_SECS);
    tb->cap.pkts = calloc((size_t)tb->cap.cap, DMRD_LEN);
    if (!tb->cap.pkts) { free(tb); return NULL; }

    tb->rp = replay_new(cfg, loop, &tb->hb);
    if (!tb->rp) { free(tb->cap.pkts); free(tb); return NULL; }

    LOGI(LOGN, "capture buffer: %d packets (%d s max), %d bytes",
         tb->cap.cap, cfg->max_capture_secs, tb->cap.cap * DMRD_LEN);
    return tb;
}

void talkback_set_hbp(talkback *tb, struct hbp *hb) { tb->hb = hb; }

void talkback_free(talkback *tb) {
    if (!tb) return;
    if (tb->sweep_timer) ev_timer_cancel(tb->loop, tb->sweep_timer);
    replay_free(tb->rp);
    free(tb->cap.pkts);
    free(tb);
}

void tb_hbp_connected(talkback *tb) {
    LOGI(LOGN, "connected — radio ID %u, mode %s", tb->cfg->radio_id,
         tb->cfg->mode == MODE_BOTH ? "BOTH" :
         tb->cfg->mode == MODE_GROUP ? "GROUP" : "UNIT");
}

void tb_hbp_disconnected(talkback *tb) {
    LOGW(LOGN, "disconnected — discarding any capture/replay in flight");
    tb->cap.active = 0;
    tb->cap.n = 0;
    replay_abort(tb->rp);
}

void tb_hbp_voice_received(talkback *tb, const uint8_t *pkt, int len) {
    if (len < DMRD_LEN) {
        LOGD(LOGN, "short DMRD (%d bytes) ignored", len);
        return;
    }

    /* busy_policy = ignore: nothing is captured while a replay is live.
     * Our own replay comes back to us on no sane master, but a *different*
     * caller keying up mid-replay would otherwise truncate the echo. */
    if (replay_active(tb->rp)) return;

    int is_unit, slot;
    if (!gate(tb, pkt, &is_unit, &slot)) return;

    uint32_t sid  = rd32(pkt + DMRD_STREAM_OFF);
    double   now  = ev_now(tb->loop);
    uint8_t  fl   = pkt[DMRD_FLAGS_OFF];
    int      ftyp = fl & HBPF_FRAMETYPE_MASK;
    int      dtyp = fl & HBPF_DTYPE_MASK;

    if (!tb->cap.active || tb->cap.stream_id != sid) {
        /* New stream.  Any half-captured previous one is abandoned: it lost
         * its terminator and a fresh call is more interesting than a stale
         * fragment. */
        if (tb->cap.active)
            LOGD(LOGN, "new stream %08x supersedes incomplete %08x", sid, tb->cap.stream_id);
        tb->cap.active    = 1;
        tb->cap.stream_id = sid;
        tb->cap.slot      = slot;
        tb->cap.is_unit   = is_unit;
        tb->cap.started   = now;
        tb->cap.n         = 0;
        memcpy(tb->cap.src, pkt + DMRD_SRC_OFF, 3);
        memcpy(tb->cap.dst, pkt + DMRD_DST_OFF, 3);
        LOGI(LOGN, "capture start — %s call from %u to %u, TS%d, stream %08x",
             is_unit ? "unit" : "group", rd24(tb->cap.src), rd24(tb->cap.dst), slot, sid);
        arm_sweep(tb);
    }

    tb->cap.last_pkt = now;

    if (tb->cap.n < tb->cap.cap) {
        memcpy(tb->cap.pkts + (size_t)tb->cap.n * DMRD_LEN, pkt, DMRD_LEN);
        tb->cap.n++;
    } else if (tb->cap.n == tb->cap.cap) {
        /* Ceiling reached.  Replay what we have rather than dropping it. */
        LOGW(LOGN, "capture hit the %d s ceiling — replaying the first %d packets",
             tb->cfg->max_capture_secs, tb->cap.n);
        close_capture(tb, "max_capture_secs reached");
        return;
    }

    if (ftyp == HBPF_FRAMETYPE_DATASYNC && dtyp == HBPF_SLT_VTERM)
        close_capture(tb, "terminator");
}
