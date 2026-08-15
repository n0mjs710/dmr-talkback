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

/* trim leading/trailing whitespace into dst */
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

/* Read an optional array of positive integers (talkgroup IDs). */
static void get_tgid_array(const toml *t, errbag *e, const char *sec, const char *key,
                           uint32_t *out, int *n_out, int cap) {
    *n_out = 0;
    const toml_value *v = toml_get(t, sec, key);
    if (!v) return;
    if (v->type != TOML_ARRAY) {
        adderr(e, "[%s] %s: must be an array of talkgroup IDs, e.g. [9990]", sec, key);
        return;
    }
    for (int i = 0; i < v->arrlen; i++) {
        if (v->arr[i].type != TOML_INT) {
            adderr(e, "[%s] %s: all entries must be integers", sec, key);
            continue;
        }
        if (v->arr[i].i < 1 || v->arr[i].i > 0xFFFFFF) {
            adderr(e, "[%s] %s: talkgroup %lld out of range (1..16777215)", sec, key, v->arr[i].i);
            continue;
        }
        if (*n_out >= cap) {
            adderr(e, "[%s] %s: too many talkgroups (max %d)", sec, key, cap);
            return;
        }
        out[(*n_out)++] = (uint32_t)v->arr[i].i;
    }
}

/* Build the RPTO options string from the group talkgroup lists.
 *
 * This is load-bearing, not cosmetic: HBlink4 will not send a talkgroup to a
 * connected repeater that has not subscribed to it, so without this the
 * talkback would sit connected and deaf.  HBlink3 ignores it harmlessly.
 *
 * Format:  TS1=<tg,tg,...>;TS2=<tg,tg,...>   (a slot with no talkgroups is
 * emitted empty, which both cores read as "nothing on this slot").
 */
static void build_options(Config *cfg) {
    char *p = cfg->options;
    char *end = cfg->options + sizeof cfg->options;

    if (!cfg_does_group(cfg) || (cfg->n_group_ts1 == 0 && cfg->n_group_ts2 == 0)) {
        cfg->options[0] = 0;
        return;
    }
    p += snprintf(p, (size_t)(end - p), "TS1=");
    for (int i = 0; i < cfg->n_group_ts1 && p < end; i++)
        p += snprintf(p, (size_t)(end - p), "%s%u", i ? "," : "", cfg->group_ts1[i]);
    if (p < end) p += snprintf(p, (size_t)(end - p), ";TS2=");
    for (int i = 0; i < cfg->n_group_ts2 && p < end; i++)
        p += snprintf(p, (size_t)(end - p), "%s%u", i ? "," : "", cfg->group_ts2[i]);
}

int cfg_group_match(const Config *cfg, int slot, uint32_t tgid) {
    const uint32_t *list = (slot == 2) ? cfg->group_ts2 : cfg->group_ts1;
    int n = (slot == 2) ? cfg->n_group_ts2 : cfg->n_group_ts1;
    for (int i = 0; i < n; i++) if (list[i] == tgid) return 1;
    return 0;
}

int cfg_unit_slot_ok(const Config *cfg, int slot) {
    return (slot == 2) ? cfg->unit_slot2 : cfg->unit_slot1;
}

int config_load(const char *path, Config *cfg, char *err, size_t errlen)
{
    char perr[256];
    toml *t = toml_parse_file(path, perr, sizeof perr);
    if (!t) { snprintf(err, errlen, "%s", perr); return -1; }

    errbag e; e.buf[0] = 0; e.n = 0;
    memset(cfg, 0, sizeof *cfg);

    /* [global] */
    { static const char *LV[] = {"DEBUG","INFO","WARNING","ERROR"};
      char lvl[16];
      get_choice(t, &e, "global", "log_level", 0, "INFO", LV, 4, lvl, sizeof lvl);
      cfg->log_level = log_level_from_str(lvl);
      if (cfg->log_level < 0) cfg->log_level = LOG_INFO;
    }

    /* [talkback] */
    cfg->radio_id = (uint32_t)get_int(t, &e, "talkback", "radio_id", 1, 0, 1, 1, 1, 0xFFFFFF);

    { static const char *M[] = {"GROUP","UNIT","BOTH"};
      char m[16];
      get_choice(t, &e, "talkback", "mode", 0, "BOTH", M, 3, m, sizeof m);
      if      (!strcmp(m, "GROUP")) cfg->mode = MODE_GROUP;
      else if (!strcmp(m, "UNIT"))  cfg->mode = MODE_UNIT;
      else                          cfg->mode = MODE_BOTH;
    }

    /* Integer milliseconds rather than a float: the vendored TOML reader has
     * no float type, and milliseconds are precise enough for a replay delay. */
    cfg->replay_delay = (double)get_int(t, &e, "talkback", "replay_delay_ms",
                                        0, 2000, 1, 0, 1, 30000) / 1000.0;
    cfg->max_capture_secs = (int)get_int(t, &e, "talkback", "max_capture_secs",
                                         0, 30, 1, 1, 1, 300);

    get_tgid_array(t, &e, "talkback", "group_ts1_tgids", cfg->group_ts1, &cfg->n_group_ts1, CFG_MAX_TGIDS);
    get_tgid_array(t, &e, "talkback", "group_ts2_tgids", cfg->group_ts2, &cfg->n_group_ts2, CFG_MAX_TGIDS);

    /* unit_slots: array of 1 and/or 2; default both */
    { const toml_value *v = toml_get(t, "talkback", "unit_slots");
      if (!v) { cfg->unit_slot1 = 1; cfg->unit_slot2 = 1; }
      else if (v->type != TOML_ARRAY) {
          adderr(&e, "[talkback] unit_slots: must be an array, e.g. [1, 2]");
      } else {
          for (int i = 0; i < v->arrlen; i++) {
              if (v->arr[i].type != TOML_INT || (v->arr[i].i != 1 && v->arr[i].i != 2)) {
                  adderr(&e, "[talkback] unit_slots: entries must be 1 or 2");
                  continue;
              }
              if (v->arr[i].i == 1) cfg->unit_slot1 = 1; else cfg->unit_slot2 = 1;
          }
      }
    }

    /* Cross-field validation: a mode has to have something to act on. */
    if (cfg_does_group(cfg) && cfg->n_group_ts1 == 0 && cfg->n_group_ts2 == 0)
        adderr(&e, "[talkback] mode includes GROUP but no talkgroups are configured "
                   "(set group_ts1_tgids and/or group_ts2_tgids)");
    if (cfg_does_unit(cfg) && !cfg->unit_slot1 && !cfg->unit_slot2)
        adderr(&e, "[talkback] mode includes UNIT but unit_slots is empty");

    /* [master] */
    get_str(t, &e, "master", "ip", 1, "127.0.0.1", cfg->master_ip, sizeof cfg->master_ip);
    cfg->master_port = (int)get_int(t, &e, "master", "port", 1, 0, 1, 1, 1, 65535);
    { char pp[256]; get_str(t, &e, "master", "passphrase", 1, "", pp, sizeof pp);
      cfg->passphrase_len = (int)strlen(pp);
      memcpy(cfg->passphrase, pp, (size_t)cfg->passphrase_len); }

    /* [announce] — RPTC blob filler.  There is no radio behind this; the
     * defaults are deliberately inert. */
    get_str(t, &e, "announce", "callsign",    0, "TALKBACK",  cfg->callsign,    sizeof cfg->callsign);
    get_str(t, &e, "announce", "description", 0, "Voice Test", cfg->description, sizeof cfg->description);
    get_str(t, &e, "announce", "location",    0, "",          cfg->location,    sizeof cfg->location);
    get_str(t, &e, "announce", "url",         0, "",          cfg->url,         sizeof cfg->url);
    get_str(t, &e, "announce", "colorcode",   0, "01",        cfg->colorcode,   sizeof cfg->colorcode);
    /* Fixed: no radio, no site. */
    snprintf(cfg->rx_freq,     sizeof cfg->rx_freq,     "%s", "000000000");
    snprintf(cfg->tx_freq,     sizeof cfg->tx_freq,     "%s", "000000000");
    snprintf(cfg->tx_power,    sizeof cfg->tx_power,    "%s", "00");
    snprintf(cfg->latitude,    sizeof cfg->latitude,    "%s", "00.0000 ");
    snprintf(cfg->longitude,   sizeof cfg->longitude,   "%s", "000.0000 ");
    snprintf(cfg->height,      sizeof cfg->height,      "%s", "000");
    snprintf(cfg->software_id, sizeof cfg->software_id, "%s", "dmr-talkback");
    snprintf(cfg->package_id,  sizeof cfg->package_id,  "%s", "1.0.0");

    build_options(cfg);

    toml_free(t);

    if (e.n > 0) {
        snprintf(err, errlen, "Configuration errors:\n%s", e.buf);
        return -1;
    }
    return 0;
}
