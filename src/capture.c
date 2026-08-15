/* capture.c — the instance object: ingress gate and per-lane capture buffers.
 *
 * Receives DMRD from this instance's HBP client, decides which lane (if any)
 * the stream belongs to, buffers it verbatim, and hands the finished buffer to
 * replay.c.
 *
 * Talkback answers GROUP CALLS ONLY.  Private calls are ignored: unit routing
 * draws on a globally administered radio-ID namespace, and a self-hosted tool
 * that anyone can deploy cannot demand a unique registered ID per instance.
 * Talkgroups are operator-scoped and bounded by the operator's own rules, so
 * they cost nobody else anything.
 */
#include "talkback.h"
#include "hbp.h"
#include "hbp_const.h"
#include "log.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define LOGN "talkback"

struct tb_instance {
    const InstanceCfg *cfg;
    ev_loop           *loop;
    struct hbp        *hb;
    tb_lane            lane[2];      /* index 0 = TS1, index 1 = TS2 */
    uint8_t            radio_id[3];
};

static uint32_t rd24(const uint8_t *p) {
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

const char *instance_name(const tb_instance *in) { return in->cfg->name; }
struct hbp *instance_hbp(const tb_instance *in)  { return in->hb; }
const InstanceCfg *instance_cfg(const tb_instance *in) { return in->cfg; }
ev_loop *instance_loop(const tb_instance *in)    { return in->loop; }

int instance_lane_captured(const tb_instance *in, int slot) {
    if (slot != 1 && slot != 2) return 0;
    return in->lane[slot - 1].cap.n;
}

int instance_lane_replaying(const tb_instance *in, int slot) {
    if (slot != 1 && slot != 2) return 0;
    return lane_replay_active(&in->lane[slot - 1]);
}

static void close_capture(tb_lane *ln, const char *why);
static void sweep_cb(ev_loop *loop, void *ud);

static void arm_sweep(tb_lane *ln) {
    if (ln->sweep) return;
    ln->sweep = ev_timer_after(ln->inst->loop, TB_STREAM_TIMEOUT, sweep_cb, ln);
}

/* Watchdog for a stream whose terminator never arrived.  Without this a lost
 * tail would pin the lane and that slot would go deaf. */
static void sweep_cb(ev_loop *loop, void *ud) {
    tb_lane *ln = ud;
    ln->sweep = NULL;
    if (!ln->cap.active) return;
    if (ev_now(loop) - ln->cap.last_pkt >= TB_STREAM_TIMEOUT) {
        close_capture(ln, "no terminator (lost tail)");
        return;
    }
    arm_sweep(ln);
}

static void close_capture(tb_lane *ln, const char *why) {
    if (!ln->cap.active) return;
    ln->cap.active = 0;
    if (ln->cap.n == 0) return;

    LOGI(LOGN, "[%s] TS%d capture end   — %s, %d packets, %.1fs",
         ln->inst->cfg->name, ln->slot, why, ln->cap.n,
         ln->cap.last_pkt - ln->cap.started);
    lane_replay_start(ln);
}

/* ---------------- public API ---------------- */

tb_instance *instance_new(const InstanceCfg *ic, ev_loop *loop) {
    tb_instance *in = calloc(1, sizeof *in);
    if (!in) return NULL;
    in->cfg  = ic;
    in->loop = loop;
    in->radio_id[0] = (uint8_t)(ic->radio_id >> 16);
    in->radio_id[1] = (uint8_t)(ic->radio_id >> 8);
    in->radio_id[2] = (uint8_t)(ic->radio_id);

    int npkts = (int)ceil((double)ic->max_capture_secs / TB_FRAME_SECS);

    for (int i = 0; i < 2; i++) {
        tb_lane *ln = &in->lane[i];
        ln->inst = in;
        ln->slot = i + 1;
        ln->tgid = cfg_slot_tgid(ic, ln->slot);
        if (!ln->tgid) continue;                 /* slot unused: no buffers */

        ln->cap.cap  = npkts;
        ln->cap.pkts = calloc((size_t)npkts, DMRD_LEN);
        if (!ln->cap.pkts || lane_replay_init(ln, ic->max_capture_secs) != 0) {
            instance_free(in);
            return NULL;
        }
        LOGI(LOGN, "[%s] TS%d answering TG %u — buffer %d packets (%d s, %d bytes)",
             ic->name, ln->slot, ln->tgid, npkts, ic->max_capture_secs, npkts * DMRD_LEN);
    }
    return in;
}

void instance_set_hbp(tb_instance *in, struct hbp *hb) { in->hb = hb; }

void instance_free(tb_instance *in) {
    if (!in) return;
    for (int i = 0; i < 2; i++) {
        tb_lane *ln = &in->lane[i];
        if (ln->sweep) ev_timer_cancel(in->loop, ln->sweep);
        lane_replay_cleanup(ln);
        free(ln->cap.pkts);
    }
    free(in);
}

void tb_hbp_connected(tb_instance *in) {
    LOGI(LOGN, "[%s] connected — radio ID %u, options %s",
         in->cfg->name, in->cfg->radio_id, in->cfg->options);
}

void tb_hbp_disconnected(tb_instance *in) {
    LOGW(LOGN, "[%s] disconnected — discarding captures and replays in flight",
         in->cfg->name);
    for (int i = 0; i < 2; i++) {
        tb_lane *ln = &in->lane[i];
        ln->cap.active = 0;
        ln->cap.n = 0;
        lane_replay_abort(ln);
    }
}

void tb_hbp_voice_received(tb_instance *in, const uint8_t *pkt, int len) {
    if (len < DMRD_LEN) {
        LOGD(LOGN, "[%s] short DMRD (%d bytes) ignored", in->cfg->name, len);
        return;
    }

    uint8_t flags = pkt[DMRD_FLAGS_OFF];
    int slot = (flags & HBPF_TGID_TS2) ? 2 : 1;

    /* Group calls only — see the file header for why. */
    if (flags & HBPF_TGID_CALL_P) {
        if (memcmp(pkt + DMRD_DST_OFF, in->radio_id, 3) == 0)
            LOGD(LOGN, "[%s] private call to our radio ID ignored — talkback "
                       "answers group calls only; use TG %u on TS%d",
                 in->cfg->name, cfg_slot_tgid(in->cfg, slot), slot);
        return;
    }

    tb_lane *ln = &in->lane[slot - 1];
    if (!ln->tgid) return;                                  /* slot unused */
    if (rd24(pkt + DMRD_DST_OFF) != ln->tgid) return;       /* not our TG */

    /* This lane is busy playing back.  The other lane is unaffected. */
    if (lane_replay_active(ln)) return;

    uint32_t sid  = rd32(pkt + DMRD_STREAM_OFF);
    double   now  = ev_now(in->loop);
    int      ftyp = flags & HBPF_FRAMETYPE_MASK;
    int      dtyp = flags & HBPF_DTYPE_MASK;

    if (ln->cap.active && ln->cap.stream_id != sid) {
        /* Two streams on one lane.  The wire should not produce this — a
         * timeslot carries one call at a time and the server arbitrates — but
         * if it does, first-come-wins.  Superseding unconditionally would let
         * interleaved packets reset the buffer on every frame and capture
         * nothing at all. */
        if (now - ln->cap.last_pkt < TB_SUPERSEDE_QUIET) {
            LOGD(LOGN, "[%s] TS%d stream %08x ignored — %08x still live",
                 in->cfg->name, slot, sid, ln->cap.stream_id);
            return;
        }
        LOGD(LOGN, "[%s] TS%d stream %08x supersedes stale %08x",
             in->cfg->name, slot, sid, ln->cap.stream_id);
        ln->cap.active = 0;
    }

    if (!ln->cap.active) {
        ln->cap.active    = 1;
        ln->cap.stream_id = sid;
        ln->cap.started   = now;
        ln->cap.n         = 0;
        memcpy(ln->cap.src, pkt + DMRD_SRC_OFF, 3);
        LOGI(LOGN, "[%s] TS%d capture start — from %u to TG %u, stream %08x",
             in->cfg->name, slot, rd24(ln->cap.src), ln->tgid, sid);
        arm_sweep(ln);
    }

    ln->cap.last_pkt = now;

    if (ln->cap.n < ln->cap.cap) {
        memcpy(ln->cap.pkts + (size_t)ln->cap.n * DMRD_LEN, pkt, DMRD_LEN);
        ln->cap.n++;
    } else {
        /* Ceiling reached.  Replay what we have rather than dropping it. */
        LOGW(LOGN, "[%s] TS%d hit the %d s ceiling — replaying the first %d packets",
             in->cfg->name, slot, in->cfg->max_capture_secs, ln->cap.n);
        close_capture(ln, "max_capture_secs reached");
        return;
    }

    if (ftyp == HBPF_FRAMETYPE_DATASYNC && dtyp == HBPF_SLT_VTERM)
        close_capture(ln, "terminator");
}
