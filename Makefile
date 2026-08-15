# dmr-talkback — DMR voice test / talkback endpoint
CC      ?= cc
CFLAGS  ?= -std=c11 -O2 -Wall -Wextra -D_DEFAULT_SOURCE
LDFLAGS ?= -lm

SRC_DIR := src
DMR_DIR := src/dmr

SOURCES := \
	$(SRC_DIR)/main.c \
	$(SRC_DIR)/config.c \
	$(SRC_DIR)/toml.c \
	$(SRC_DIR)/log.c \
	$(SRC_DIR)/eventloop.c \
	$(SRC_DIR)/net.c \
	$(SRC_DIR)/crypto.c \
	$(SRC_DIR)/hbp.c \
	$(SRC_DIR)/capture.c \
	$(SRC_DIR)/replay.c \
	$(DMR_DIR)/dmr_bits.c \
	$(DMR_DIR)/dmr_const.c \
	$(DMR_DIR)/hamming.c \
	$(DMR_DIR)/crc.c \
	$(DMR_DIR)/rs129.c \
	$(DMR_DIR)/golay.c \
	$(DMR_DIR)/bptc.c \
	$(DMR_DIR)/ambe.c

OBJECTS := $(SOURCES:.c=.o)
BIN     := talkback

DMR_SOURCES := $(filter $(DMR_DIR)/%,$(SOURCES))

.PHONY: all clean test install uninstall

all: $(BIN)

$(BIN): $(OBJECTS)
	$(CC) $(CFLAGS) -o $@ $(OBJECTS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# FHS install (run as root: sudo make install)
#   binary -> $(PREFIX)/bin/talkback          (default /usr/local/bin)
#   config -> $(SYSCONFDIR)/talkback/         (default /etc/talkback)
#   unit   -> $(UNITDIR)/talkback.service     (default /lib/systemd/system)
# The live config is NEVER overwritten; the sample is always refreshed.
# install does NOT enable or start the service.
PREFIX     ?= /usr/local
SYSCONFDIR ?= /etc
UNITDIR    ?= /lib/systemd/system
CONFDIR    := $(SYSCONFDIR)/talkback
UNIT       := $(UNITDIR)/talkback.service

install: $(BIN)
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 0755 $(BIN) $(DESTDIR)$(PREFIX)/bin/talkback
	install -d $(DESTDIR)$(CONFDIR)
	install -m 0644 talkback.toml.sample $(DESTDIR)$(CONFDIR)/talkback.toml.sample
	@if [ -f $(DESTDIR)$(CONFDIR)/talkback.toml ]; then \
	    echo "Preserving existing $(DESTDIR)$(CONFDIR)/talkback.toml (not overwritten)"; \
	else \
	    install -m 0600 talkback.toml.sample $(DESTDIR)$(CONFDIR)/talkback.toml; \
	    echo "Installed fresh $(DESTDIR)$(CONFDIR)/talkback.toml — edit it before starting"; \
	fi
	install -d $(DESTDIR)$(UNITDIR)
	install -m 0644 talkback.service $(DESTDIR)$(UNIT)
	-systemctl daemon-reload
	@echo
	@echo "Installed. NOT enabled/started. Edit $(CONFDIR)/talkback.toml then:"
	@echo "    sudo systemctl enable --now talkback"

uninstall:
	-systemctl disable --now talkback
	-rm -f $(DESTDIR)$(PREFIX)/bin/talkback
	-rm -f $(DESTDIR)$(UNIT)
	-systemctl daemon-reload
	@echo "Removed binary and unit. Left $(CONFDIR) intact (delete manually if desired)."

# Sources the tests link against (no main, no net).  The HBP client is stubbed
# inside the test itself.
TEST_SUPPORT := $(SRC_DIR)/replay.c $(SRC_DIR)/log.c $(SRC_DIR)/eventloop.c $(DMR_SOURCES)

# test_rewrite: the loopback-identity conformance vector — replayed AMBE is
# bit-identical to captured AMBE, headers are rewritten, and the LC in both
# carriers decodes back to the new addressing.
test: tests/test_rewrite.c $(TEST_SUPPORT)
	$(CC) $(CFLAGS) -I$(SRC_DIR) -o /tmp/talkback_test_rewrite \
		tests/test_rewrite.c $(TEST_SUPPORT) $(LDFLAGS)
	/tmp/talkback_test_rewrite

clean:
	rm -f $(OBJECTS) $(BIN)
