/* config.h — parsed, validated configuration. */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stddef.h>

#define CFG_MAX_TGIDS 32

enum { MODE_GROUP = 1, MODE_UNIT = 2, MODE_BOTH = 3 };

typedef struct {
    int      log_level;             /* LOG_* enum */

    /* [talkback] */
    uint32_t radio_id;              /* THE radio ID: HBP login, DMRD repeater,
                                     * DMRD source of the replay, and the unit
                                     * call target.  One ID, every role. */
    int      mode;                  /* MODE_GROUP | MODE_UNIT | MODE_BOTH */
    double   replay_delay;          /* seconds after capture end */
    int      max_capture_secs;      /* bounds the fixed capture buffer */

    /* Group talkgroups we listen and reply on, per slot.  Also drives the
     * RPTO options string (see hbp.c), which is what makes this work on
     * HBlink4 without server-side configuration. */
    uint32_t group_ts1[CFG_MAX_TGIDS];
    int      n_group_ts1;
    uint32_t group_ts2[CFG_MAX_TGIDS];
    int      n_group_ts2;

    /* Slots on which a private call to radio_id is accepted. */
    int      unit_slot1;
    int      unit_slot2;

    /* [master] */
    char     master_ip[256];
    int      master_port;
    char     passphrase[256];
    int      passphrase_len;

    /* RPTC announcement fields.  There is no radio; these exist only to fill
     * the 302-byte config blob.  Only callsign/description/location are worth
     * configuring. */
    char     options[512];          /* generated, not read from file */
    char     callsign[64];
    char     rx_freq[32];
    char     tx_freq[32];
    char     tx_power[16];
    char     colorcode[16];
    char     latitude[32];
    char     longitude[32];
    char     height[16];
    char     location[64];
    char     description[64];
    char     url[256];
    char     software_id[64];
    char     package_id[64];
} Config;

/* Load and validate a TOML config file.  Returns 0 on success; on failure
 * returns -1 and fills err with a human-readable message. */
int config_load(const char *path, Config *cfg, char *err, size_t errlen);

/* True if the configured mode answers group / unit calls. */
static inline int cfg_does_group(const Config *c) { return (c->mode & MODE_GROUP) != 0; }
static inline int cfg_does_unit (const Config *c) { return (c->mode & MODE_UNIT)  != 0; }

/* True if tgid is in the listen list for slot (1 or 2). */
int cfg_group_match(const Config *cfg, int slot, uint32_t tgid);

/* True if a private call arriving on slot is accepted. */
int cfg_unit_slot_ok(const Config *cfg, int slot);

#endif
