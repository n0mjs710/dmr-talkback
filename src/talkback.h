/* talkback.h — shared types for the capture/replay pair.
 *
 * The program is one object, `talkback`, that owns:
 *   - the HBP client (hbp.c) it receives from and transmits through;
 *   - one capture buffer (capture.c) holding at most one inbound stream;
 *   - one replay engine (replay.c) that clocks that buffer back out.
 *
 * Only one capture and one replay exist at a time.  Everything is sized at
 * init from `max_capture_secs`; nothing allocates on the data path.
 */
#ifndef TALKBACK_H
#define TALKBACK_H

#include <stdint.h>
#include "config.h"
#include "eventloop.h"

/* DMR voice frames arrive every 60 ms. */
#define TB_FRAME_MS      60
#define TB_FRAME_SECS    0.06

/* A stream with no packet for this long is treated as ended (lost tail). */
#define TB_STREAM_TIMEOUT 2.0

typedef struct talkback talkback;
typedef struct replay   replay;
struct hbp;

/* One captured stream: the wire packets verbatim, plus the addressing we need
 * to build the reply. */
typedef struct {
    int      active;
    uint32_t stream_id;      /* inbound stream ID, to match continuation packets */
    int      slot;           /* 1 or 2, as received */
    int      is_unit;        /* inbound call type; the reply mirrors it */
    uint8_t  src[3];         /* the caller */
    uint8_t  dst[3];         /* TGID (group) or our radio ID (unit) */
    double   started;
    double   last_pkt;
    int      n;              /* packets held */
    int      cap;            /* capacity in packets */
    uint8_t *pkts;           /* cap * DMRD_LEN, contiguous */
} tb_capture;

/* ---- capture.c ---- */
talkback *talkback_new(const Config *cfg, ev_loop *loop);
void      talkback_set_hbp(talkback *tb, struct hbp *hb);
void      talkback_free(talkback *tb);

/* Called by hbp.c. */
void tb_hbp_connected(talkback *tb);
void tb_hbp_disconnected(talkback *tb);
void tb_hbp_voice_received(talkback *tb, const uint8_t *pkt, int len);

/* ---- replay.c ---- */
replay *replay_new(const Config *cfg, ev_loop *loop, struct hbp **hb_slot);
void    replay_free(replay *rp);

/* Schedule a replay of `cap` to begin after the configured delay.  Takes a
 * reference to the buffer; the caller must not reuse it until replay_active()
 * goes false. */
void    replay_start(replay *rp, const tb_capture *cap);
int     replay_active(const replay *rp);
void    replay_abort(replay *rp);

/* Exposed for tests: rewrite one captured DMRD packet into its replay form.
 * `out` must be at least DMRD_LEN bytes.  Pure — no I/O, no state. */
typedef struct {
    uint8_t  src[3];         /* new source: our radio ID */
    uint8_t  dst[3];         /* new destination: TGID, or the original caller */
    uint8_t  rptr[4];        /* new repeater ID: our radio ID */
    uint32_t stream_id;      /* new stream ID */
    int      is_unit;        /* reply call type */
    uint8_t  lc[9];          /* the reply LC */
    uint8_t  h_lc[196];      /* dmr_bptc_encode_lc(lc, 0) */
    uint8_t  t_lc[196];      /* dmr_bptc_encode_lc(lc, 1) */
    uint8_t  emb[4][4];      /* dmr_encode_emblc(lc) — bursts B..E */
} tb_rewrite;

/* Build the LC and its encoded forms once per replay. */
void tb_rewrite_init(tb_rewrite *rw, uint32_t radio_id, const uint8_t dst[3],
                     int is_unit, uint32_t stream_id);

/* Apply the rewrite to one packet.  seq is the replay-relative sequence number. */
void tb_rewrite_packet(const tb_rewrite *rw, const uint8_t *in, uint8_t *out, uint8_t seq);

#endif
