/* config.c — load and validate the TOML config. */
#include "config.h"
#include "toml.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <ctype.h>

/* error accumulator */
typedef struct { char buf[4096]; int n; } errbag;
static void adderr(errbag *e, const char *fmt, ...) {
    char line[256];
    va_list ap; va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    e->n += snprintf(e->buf + e->n, sizeof e->buf - (size_t)e->n, "  %s\n", line);
}

static void str_to_upper(char *s) { for (; *s; s++) *s = (char)toupper((unsigned char)*s); }

static char *trimcpy(char *dst, size_t cap, const char *src) {
    while (*src && isspace((unsigned char)*src)) src++;
    size_t len = strlen(src);
    while (len > 0 && isspace((unsigned char)src[len-1])) len--;
    if (len >= cap) len = cap - 1;
    memcpy(dst, src, len);
    dst[len] = 0;
    return dst;
}

static int get_str(const toml *t, errbag *e, const char *sec, const char *key,
                   int required, const char *deflt, char *out, size_t cap) {
    const toml_value *v = toml_get(t, sec, key);
    if (!v) {
        if (required) adderr(e, "[%s] %s: required", sec, key);
        if (deflt) snprintf(out, cap, "%s", deflt); else out[0] = 0;
        return 0;
    }
    if (v->type != TOML_STRING) {
        adderr(e, "[%s] %s: must be a string", sec, key);
        if (deflt) snprintf(out, cap, "%s", deflt); else out[0] = 0;
        return 0;
    }
    trimcpy(out, cap, v->s);
    return 1;
}

static int get_choice(const toml *t, errbag *e, const char *sec, const char *key,
                      int required, const char *deflt,
                      const char *const *choices, int nch, char *out, size_t cap) {
    const toml_value *v = toml_get(t, sec, key);
    if (!v) {
        if (required) adderr(e, "[%s] %s: required", sec, key);
        if (deflt) snprintf(out, cap, "%s", deflt); else out[0] = 0;
        return 0;
    }
    if (v->type != TOML_STRING) {
        adderr(e, "[%s] %s: must be a string", sec, key);
        if (deflt) snprintf(out, cap, "%s", deflt); else out[0] = 0;
        return 0;
    }
    char tmp[64]; trimcpy(tmp, sizeof tmp, v->s); str_to_upper(tmp);
    for (int i = 0; i < nch; i++)
        if (!strcmp(tmp, choices[i])) { snprintf(out, cap, "%s", choices[i]); return 1; }
    adderr(e, "[%s] %s: invalid value '%s'", sec, key, v->s);
    if (deflt) snprintf(out, cap, "%s", deflt); else out[0] = 0;
    return 0;
}

static long long get_int(const toml *t, errbag *e, const char *sec, const char *key,
                         int required, long long deflt, int has_min, long long mn,
                         int has_max, long long mx) {
    const toml_value *v = toml_get(t, sec, key);
    if (!v) { if (required) adderr(e, "[%s] %s: required", sec, key); return deflt; }
    if (v->type != TOML_INT) { adderr(e, "[%s] %s: must be an integer", sec, key); return deflt; }
    if (has_min && v->i < mn) adderr(e, "[%s] %s: must be >= %lld, got %lld", sec, key, mn, v->i);
    if (has_max && v->i > mx) adderr(e, "[%s] %s: must be <= %lld, got %lld", sec, key, mx, v->i);
    return v->i;
}

/* Build the RPTO options string from this instance's slot talkgroups.
 *
 * Load-bearing, not cosmetic: HBlink4 will not send a talkgroup to a connected
 * repeater that has not subscribed to it, so without this the instance would
 * sit connected and deaf.  HBlink3 ignores it harmlessly. */
static void build_options(InstanceCfg *ic) {
    char ts1[16] = "", ts2[16] = "";
    if (ic->slot1_tgid) snprintf(ts1, sizeof ts1, "%u", ic->slot1_tgid);
    if (ic->slot2_tgid) snprintf(ts2, sizeof ts2, "%u", ic->slot2_tgid);
    snprintf(ic->options, sizeof ic->options, "TS1=%s;TS2=%s", ts1, ts2);
}

static void load_instance(const toml *t, errbag *e, const char *sec, InstanceCfg *ic)
{
    memset(ic, 0, sizeof *ic);

    /* section is "instance.<name>"; keep the tail as the label */
    const char *dot = strrchr(sec, '.');
    const char *nm  = dot ? dot + 1 : sec;
    size_t nl = strlen(nm);
    if (nl >= sizeof ic->name) nl = sizeof ic->name - 1;
    memcpy(ic->name, nm, nl);
    ic->name[nl] = 0;

    ic->radio_id = (uint32_t)get_int(t, e, sec, "radio_id", 1, 0, 1, 1, 1, 0xFFFFFF);

    ic->slot1_tgid = (uint32_t)get_int(t, e, sec, "slot1_tgid", 0, 0, 1, 0, 1, 0xFFFFFF);
    ic->slot2_tgid = (uint32_t)get_int(t, e, sec, "slot2_tgid", 0, 0, 1, 0, 1, 0xFFFFFF);
    if (!ic->slot1_tgid && !ic->slot2_tgid)
        adderr(e, "[%s] needs slot1_tgid and/or slot2_tgid — an instance with "
                  "no talkgroup on either slot would answer nothing", sec);
    if (ic->slot1_tgid && ic->slot1_tgid == ic->slot2_tgid)
        adderr(e, "[%s] slot1_tgid and slot2_tgid are both %u — give each slot a "
                  "distinct talkgroup, or leave one unused", sec, ic->slot1_tgid);

    /* Integer milliseconds: the vendored TOML reader has no float type. */
    ic->replay_delay = (double)get_int(t, e, sec, "replay_delay_ms",
                                       0, 2000, 1, 0, 1, 30000) / 1000.0;
    ic->max_capture_secs = (int)get_int(t, e, sec, "max_capture_secs",
                                        0, 30, 1, 1, 1, 300);

    get_str(t, e, sec, "master_ip", 1, "127.0.0.1", ic->master_ip, sizeof ic->master_ip);
    ic->master_port = (int)get_int(t, e, sec, "master_port", 1, 0, 1, 1, 1, 65535);
    { char pp[256]; get_str(t, e, sec, "passphrase", 1, "", pp, sizeof pp);
      ic->passphrase_len = (int)strlen(pp);
      memcpy(ic->passphrase, pp, (size_t)ic->passphrase_len); }

    get_str(t, e, sec, "callsign",    0, "TALKBACK",   ic->callsign,    sizeof ic->callsign);
    get_str(t, e, sec, "description", 0, "Voice Test", ic->description, sizeof ic->description);
    get_str(t, e, sec, "location",    0, "",           ic->location,    sizeof ic->location);
    get_str(t, e, sec, "url",         0, "",           ic->url,         sizeof ic->url);
    get_str(t, e, sec, "colorcode",   0, "01",         ic->colorcode,   sizeof ic->colorcode);

    /* No radio, no site. */
    snprintf(ic->rx_freq,     sizeof ic->rx_freq,     "%s", "000000000");
    snprintf(ic->tx_freq,     sizeof ic->tx_freq,     "%s", "000000000");
    snprintf(ic->tx_power,    sizeof ic->tx_power,    "%s", "00");
    snprintf(ic->latitude,    sizeof ic->latitude,    "%s", "00.0000 ");
    snprintf(ic->longitude,   sizeof ic->longitude,   "%s", "000.0000 ");
    snprintf(ic->height,      sizeof ic->height,      "%s", "000");
    snprintf(ic->software_id, sizeof ic->software_id, "%s", "dmr-talkback");
    snprintf(ic->package_id,  sizeof ic->package_id,  "%s", "1.0.0");

    build_options(ic);
}

int config_load(const char *path, Config *cfg, char *err, size_t errlen)
{
    char perr[256];
    toml *t = toml_parse_file(path, perr, sizeof perr);
    if (!t) { snprintf(err, errlen, "%s", perr); return -1; }

    errbag e; e.buf[0] = 0; e.n = 0;
    memset(cfg, 0, sizeof *cfg);

    { static const char *LV[] = {"DEBUG","INFO","WARNING","ERROR"};
      char lvl[16];
      get_choice(t, &e, "global", "log_level", 0, "INFO", LV, 4, lvl, sizeof lvl);
      cfg->log_level = log_level_from_str(lvl);
      if (cfg->log_level < 0) cfg->log_level = LOG_INFO;
    }

    char secs[CFG_MAX_INSTANCES][64];
    int n = toml_sections(t, "instance.", secs, CFG_MAX_INSTANCES);
    if (n == 0)
        adderr(&e, "no [instance.<name>] sections found — at least one is required");

    for (int i = 0; i < n; i++)
        load_instance(t, &e, secs[i], &cfg->inst[i]);
    cfg->n_inst = n;

    /* Two instances pointed at the same server with the same radio ID would be
     * the same repeater logging in twice: the server drops one, or they fight
     * over the registration.  Distinct servers with a shared ID is fine and
     * expected — that is the whole point of group-only talkback. */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++) {
            const InstanceCfg *a = &cfg->inst[i], *b = &cfg->inst[j];
            if (a->radio_id == b->radio_id && a->master_port == b->master_port &&
                !strcmp(a->master_ip, b->master_ip))
                adderr(&e, "instances '%s' and '%s' share radio ID %u on the same "
                           "server %s:%d — one of them must differ",
                       a->name, b->name, a->radio_id, a->master_ip, a->master_port);
        }

    toml_free(t);

    if (e.n > 0) {
        snprintf(err, errlen, "Configuration errors:\n%s", e.buf);
        return -1;
    }
    return 0;
}
