/* talkback.h — shared types for the capture/replay pair.
 *
 * Shape:
 *
 *   process
 *     └── N instances          one HBP connection to one server each
 *           └── 2 lanes        one per timeslot, fully independent
 *                 ├── capture  at most one inbound stream
 *                 └── replay   at most one outbound stream
 *
 * A DMR timeslot carries one call at a time, so one lane per slot is exactly
 * the concurrency the wire can deliver — no more, no less.  Lanes never touch
 * each other's state, which is what lets a user on TS1 and a user on TS2 both
 * be served at the same time.
 *
 * Everything is sized at startup from `max_capture_secs`; nothing allocates on
 * the data path.
 */
#ifndef TALKBACK_H
#define TALKBACK_H

#include <stdint.h>
#include "config.h"
#include "eventloop.h"

/* DMR voice frames arrive every 60 ms. */
#define TB_FRAME_MS      60
#define TB_FRAME_SECS    0.06

/* A capture with no packet for this long is treated as ended (lost tail). */
#define TB_STREAM_TIMEOUT 2.0

/* A different stream ID on a lane supersedes the capture in progress only once
 * the current one has gone this quiet.  Below it, first-come-wins: two live
 * streams interleaving on one lane must never thrash the buffer. */
#define TB_SUPERSEDE_QUIET 0.30

typedef struct tb_instance tb_instance;
typedef struct tb_lane     tb_lane;
struct hbp;

/* The rewritten addressing for one replay, encoded once at replay start.
 * Pure data — the rewrite functions below have no I/O and no state. */
typedef struct {
    uint8_t  src[3];         /* our radio ID */
    uint8_t  dst[3];         /* the talkgroup we are replaying onto */
    uint8_t  rptr[4];        /* our radio ID again, as the DMRD repeater */
    uint32_t stream_id;      /* fresh */
    uint8_t  lc[9];          /* the reply LC */
    uint8_t  h_lc[196];      /* dmr_bptc_encode_lc(lc, 0) */
    uint8_t  t_lc[196];      /* dmr_bptc_encode_lc(lc, 1) */
    uint8_t  emb[4][4];      /* dmr_encode_emblc(lc) — bursts B..E */
} tb_rewrite;

/* One inbound stream being buffered. */
typedef struct {
    int      active;
    uint32_t stream_id;
    double   started;
    double   last_pkt;
    int      n;              /* packets held */
    int      cap;            /* capacity in packets */
    uint8_t *pkts;           /* cap * DMRD_LEN, contiguous */
    uint8_t  src[3];         /* the caller, for logging */
} tb_capture;

/* One outbound stream being clocked out. */
typedef struct {
    int        active;
    int        idx;
    int        n;
    int        cap;
    uint8_t   *pkts;
    uint8_t    seq;
    double     started;
    tb_rewrite rw;
    ev_timer  *timer;
} tb_replay;

/* One timeslot of one instance. */
struct tb_lane {
    tb_instance *inst;
    int          slot;       /* 1 or 2 */
    uint32_t     tgid;       /* 0 = lane disabled */
    tb_capture   cap;
    tb_replay    rp;
    ev_timer    *sweep;
};

/* ---- capture.c ---- */
tb_instance *instance_new(const InstanceCfg *ic, ev_loop *loop);
void         instance_set_hbp(tb_instance *in, struct hbp *hb);
void         instance_free(tb_instance *in);
const char  *instance_name(const tb_instance *in);

/* Called by hbp.c. */
void tb_hbp_connected(tb_instance *in);
void tb_hbp_disconnected(tb_instance *in);
void tb_hbp_voice_received(tb_instance *in, const uint8_t *pkt, int len);

/* Used by replay.c. */
struct hbp     *instance_hbp(const tb_instance *in);
const InstanceCfg *instance_cfg(const tb_instance *in);
ev_loop        *instance_loop(const tb_instance *in);

/* Lane introspection — how many packets a lane currently holds, and whether it
 * is playing back.  `slot` is 1 or 2; an unused slot reports 0.  Used by the
 * lane tests, and the natural hook for any future status output. */
int instance_lane_captured(const tb_instance *in, int slot);
int instance_lane_replaying(const tb_instance *in, int slot);

/* ---- replay.c ---- */
int  lane_replay_init(tb_lane *ln, int max_capture_secs);
void lane_replay_cleanup(tb_lane *ln);
void lane_replay_start(tb_lane *ln);   /* takes ln->cap's contents */
int  lane_replay_active(const tb_lane *ln);
void lane_replay_abort(tb_lane *ln);

/* Exposed for tests: pure rewrite, no I/O, no state. */
void tb_rewrite_init(tb_rewrite *rw, uint32_t radio_id, const uint8_t dst[3],
                     uint32_t stream_id);
void tb_rewrite_packet(const tb_rewrite *rw, const uint8_t *in, uint8_t *out, uint8_t seq);

#endif
