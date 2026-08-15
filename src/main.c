/* main.c — dmr-talkback entry point.
 *
 * Builds N independent talkback instances from the config, gives each its own
 * HBP client, and runs them all on one event loop.  Instances share nothing;
 * the process exists only so an operator manages one service instead of six.
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

static ev_loop     *g_loop = NULL;
static hbp         *g_hbp[CFG_MAX_INSTANCES];
static tb_instance *g_inst[CFG_MAX_INSTANCES];
static int          g_n = 0;

static void on_signal(int signum)
{
    LOGI("talkback", "Signal %d received — shutting down", signum);
    for (int i = 0; i < g_n; i++)
        if (g_hbp[i]) hbp_stop(g_hbp[i]);
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

    static Config cfg;
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

    LOGI("talkback", "dmr-talkback starting — %d instance%s",
         cfg.n_inst, cfg.n_inst == 1 ? "" : "s");

    g_loop = ev_new();
    g_n = cfg.n_inst;

    for (int i = 0; i < cfg.n_inst; i++) {
        const InstanceCfg *ic = &cfg.inst[i];
        LOGI("talkback", "[%s] radio ID %u -> %s:%d",
             ic->name, ic->radio_id, ic->master_ip, ic->master_port);

        g_inst[i] = instance_new(ic, g_loop);
        if (!g_inst[i]) {
            fprintf(stderr, "Out of memory building instance '%s'\n", ic->name);
            return 1;
        }
        g_hbp[i] = hbp_new(ic, g_inst[i], g_loop);
        instance_set_hbp(g_inst[i], g_hbp[i]);
    }

    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    for (int i = 0; i < g_n; i++) hbp_start(g_hbp[i]);

    ev_run(g_loop);

    for (int i = 0; i < g_n; i++) {
        hbp_free(g_hbp[i]);
        instance_free(g_inst[i]);
    }
    ev_free(g_loop);
    LOGI("talkback", "dmr-talkback stopped");
    return 0;
}
