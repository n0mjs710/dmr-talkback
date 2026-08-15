/* hbp.h — HBP client (repeater side) with auto-reconnect.
 *
 * Lifted from ipsc2hbpc.  The talkback connects to a master exactly as an
 * ordinary repeater does; nothing about it is special on the wire. */
#ifndef HBP_H
#define HBP_H

#include <stdint.h>
#include "config.h"
#include "eventloop.h"

typedef struct hbp hbp;
struct talkback;

hbp *hbp_new(const Config *cfg, struct talkback *app, ev_loop *loop);
void hbp_start(hbp *hb);       /* connect and keep connected */
void hbp_stop(hbp *hb);        /* clean shutdown: RPTCL + cancel timers */
void hbp_free(hbp *hb);

void hbp_send_dmrd(hbp *hb, const uint8_t *data, int len);
int  hbp_is_connected(hbp *hb);

#endif
