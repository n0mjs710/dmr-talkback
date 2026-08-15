/* main.c — dmr-talkback entry point.
 *
 * Wires the HBP client to the capture/replay pair and runs the event loop.
 * That is the whole program: connect as a repeater, record a call, play it
 * back sourced from our own radio ID.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "eventloop.h"
#include "hbp.h"
#include "talkback.h"

static ev_loop  *g_loop = NULL;
static hbp      *g_hbp  = NULL;

static void on_signal(int signum)
{
    LOGI("talkback", "Signal %d received — shutting down", signum);
    if (g_hbp)  hbp_stop(g_hbp);
    if (g_loop) ev_stop(g_loop);
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [-c config.toml] [--log-level LEVEL] [--wire]\n"
        "  -c, --config PATH    Path to TOML config (default: /etc/talkback/talkback.toml)\n"
        "  --log-level LEVEL    Override config log level (DEBUG|INFO|WARNING|ERROR)\n"
        "  --wire               Log raw HBP hex only; silence everything else\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *cfg_path = "/etc/talkback/talkback.toml";
    const char *log_level_override = NULL;
    int wire = 0;

    for (int i = 1; i < argc; i++) {
        if ((!strcmp(argv[i], "-c") || !strcmp(argv[i], "--config")) && i + 1 < argc) {
            cfg_path = argv[++i];
        } else if (!strcmp(argv[i], "--log-level") && i + 1 < argc) {
            log_level_override = argv[++i];
        } else if (!strcmp(argv[i], "--wire")) {
            wire = 1;
        } else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
            usage(argv[0]); return 0;
        } else {
            usage(argv[0]); return 2;
        }
    }

    Config cfg;
    char err[4096];
    if (config_load(cfg_path, &cfg, err, sizeof err) != 0) {
        fprintf(stderr, "Configuration error: %s\n", err);
        return 1;
    }

    int level = cfg.log_level;
    if (log_level_override) {
        int l = log_level_from_str(log_level_override);
        if (l >= 0) level = l;
    }
    log_init(level, wire);

    srand((unsigned)time(NULL) ^ (unsigned)getpid());

    LOGI("talkback", "dmr-talkback starting — radio ID %u, mode %s, master %s:%d",
         cfg.radio_id,
         cfg.mode == MODE_BOTH ? "BOTH" : cfg.mode == MODE_GROUP ? "GROUP" : "UNIT",
         cfg.master_ip, cfg.master_port);
    if (cfg.options[0])
        LOGI("talkback", "subscribing with options: %s", cfg.options);
    else
        LOGI("talkback", "no group talkgroups configured — unit calls only");

    g_loop = ev_new();

    talkback *tb = talkback_new(&cfg, g_loop);
    if (!tb) { fprintf(stderr, "Out of memory allocating the capture buffer\n"); return 1; }

    g_hbp = hbp_new(&cfg, tb, g_loop);
    talkback_set_hbp(tb, g_hbp);

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    hbp_start(g_hbp);
    ev_run(g_loop);

    hbp_free(g_hbp);
    talkback_free(tb);
    ev_free(g_loop);
    LOGI("talkback", "dmr-talkback stopped");
    return 0;
}
