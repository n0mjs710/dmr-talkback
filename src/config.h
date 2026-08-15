/* config.h — parsed, validated configuration.
 *
 * One process runs N independent talkback instances.  Each instance is one
 * HBP connection to one server, with its own radio ID and its own talkgroups.
 * Instances share nothing but the event loop.
 */
#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stddef.h>

#define CFG_MAX_INSTANCES 16

typedef struct {
    char     name[64];              /* the [instance.<name>] label, for logging */

    /* The radio ID.  Source of every stream this instance originates and its
     * HBP login ID.  Nothing routes *to* it — talkback answers group calls
     * only, so this never needs to be a globally unique registered ID. */
    uint32_t radio_id;

    /* One talkgroup per slot; 0 means the slot is unused.  One TGID per slot
     * is deliberate: a slot carries one call at a time, so a list would
     * advertise capacity that does not exist. */
    uint32_t slot1_tgid;
    uint32_t slot2_tgid;

    double   replay_delay;          /* seconds after capture end */
    int      max_capture_secs;      /* bounds each lane's fixed buffer */

    /* server */
    char     master_ip[256];
    int      master_port;
    char     passphrase[256];
    int      passphrase_len;

    /* RPTC announcement fields.  There is no radio; most are inert constants.
     * `options` is generated from the slot talkgroups, not read from file. */
    char     options[512];
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
} InstanceCfg;

typedef struct {
    int         log_level;          /* LOG_* enum */
    InstanceCfg inst[CFG_MAX_INSTANCES];
    int         n_inst;
} Config;

/* Load and validate a TOML config file.  Returns 0 on success; on failure
 * returns -1 and fills err with a human-readable message. */
int config_load(const char *path, Config *cfg, char *err, size_t errlen);

/* The talkgroup an instance answers on `slot` (1 or 2); 0 if that slot is
 * unused. */
static inline uint32_t cfg_slot_tgid(const InstanceCfg *ic, int slot) {
    return (slot == 2) ? ic->slot2_tgid : ic->slot1_tgid;
}

#endif
