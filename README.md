# dmr-talkback

A DMR **voice test** endpoint. It connects to an HBP server exactly as an
ordinary repeater does, records one call, and plays it back so the caller can
hear how they sound.

Other networks call this a "parrot". This one doesn't.

It is a small, dependency-free C program. It is **not** part of HBlink3 or
HBlink4 — it is an endpoint that speaks the same protocol a repeater speaks, so
one binary works against HBlink3, HBlink4, FreeDMR, XLXD, or anything else that
serves the HomeBrew protocol.

---

## What it does

- Answers **group calls** on a talkgroup you choose, per timeslot.
- The reply goes back onto the same talkgroup, so everyone listening hears it.
- Every reply is sourced from the talkback's own radio ID, in the DMRD header
  **and** in the Link Control carried inside the voice payload, so radios and
  MMDVMHost display the talkback rather than the original caller.
- The audio is never re-encoded. AMBE comes back bit-for-bit as it went in.
- **The two timeslots are fully independent.** A caller on TS1 and a caller on
  TS2 are recorded and replayed at the same time without interfering.
- **One process runs as many instances as you like**, each connecting to its own
  server with its own talkgroups.

## Group calls only

Talkback is never addressed by radio ID, and private calls to it are ignored.

That is deliberate. Talkgroup numbers are yours: you hand them out inside your
own network and bound their reach with your own rules, and it costs nobody else
anything. Radio IDs are a globally administered namespace, and unit-call routing
is unbounded — a private call to an unknown ID floods until the target is found,
and the map that results is global. For talkback to be reachable by ID, every
instance anyone ever deployed would need its own globally unique registered
radio ID, for something that is neither a radio nor a repeater.

BrandMeister can use a private call to 9990 because BrandMeister is a single
network that is the sole authority over its own namespace. These tools assume an
internet of independent routers. The precedent doesn't transfer.

The practical upshot is good news: the talkback's radio ID never needs to be
reachable, so **one ID you already own can serve every instance you run.**

## Configuration

```toml
[global]
log_level = "INFO"

[instance.lawrence]
radio_id    = 3120099
slot1_tgid  = 0            # 0 or omitted = slot unused
slot2_tgid  = 9990
master_ip   = "127.0.0.1"
master_port = 54000
passphrase  = "s3cr37w0rd"
```

**One talkgroup per slot**, deliberately. A DMR timeslot carries one call at a
time, so a list would advertise capacity that doesn't exist. Two slots means
exactly two callers can be served at once, and the config says so plainly.

The slot talkgroups are also sent to the server as an RPTO subscription
(`TS1=…;TS2=…`) at login. That is not cosmetic: HBlink4 will not deliver a
talkgroup to a repeater that hasn't subscribed to it. HBlink3 ignores it.

See [talkback.toml.sample](talkback.toml.sample) for the fully commented
version.

### Running more than one instance

An HBlink3 server hosts several independent **systems**. Previously, giving all of
them a talkback means building a bridge that multiplexes every system into one
talkback engine — bridge rules to write, and one shared talkback that every
system contends for.

```toml
[instance.lawrence]
radio_id = 3120099
slot2_tgid = 9990
master_port = 54000
# ...

[instance.topeka]
radio_id = 3120098
slot2_tgid = 9991
master_port = 54010
# ...
```

Bridging everyone into a **single** instance is still perfectly valid if that's
the behavior you want — one talkback heard across several systems. Both work;
pick the one that matches how you want it to behave.

Two things to know:

- **Don't point one process at several different HBlink3 servers.** Instances
  are independent so it would function, but you've made one process a single
  point of failure for several servers and interleaved the logs of unrelated
  networks. Runing a process per server is must cleaner.
- **HBlink4 has no "system" concept** — the server is the unit. One instance per
  HBlink4 server is the only arrangement that makes sense.

**Sharing one radio ID across instances is fine**, and is the normal
arrangement — group-only talkback never has anything routed to its ID, so it
carries no addressing meaning. Six instances on six HBlink3 systems can all use
the same ID.

The single exception is per *listening socket*, and it is a registration
constraint rather than a routing one: an HBP server keys its registered
repeaters by radio ID and then validates the source address on every packet, so
two connections to the **same `ip:port`** with the same ID share one entry and
fight over it. Each HBlink3 system listens on its own port, so this only bites
if you point two instances at the same system. It's rejected at startup.

## Build and install

```sh
make
sudo make install          # /usr/local/bin/talkback, /etc/talkback/, systemd unit
```

`make install` never overwrites a live `talkback.toml`, and does not enable or
start the service. Edit the config, then:

```sh
sudo systemctl enable --now talkback
```

Run it in the foreground while you're setting it up:

```sh
./talkback -c talkback.toml --log-level DEBUG
```

## Connecting it

Where talkback runs on the same server as HBlink3/4, and those programs listen
on the loopback, connect to the loopback — it avoids extra overhead on the live
network port and keeps the passphrase off the wire.

**HBlink3** — add a `MODE: SERVER` system for each instance to log into. With one
instance per system you need no bridge at all; if you'd rather have one shared
talkback, bridge its talkgroup in `rules.py` instead.

**HBlink4** — add an access-control entry for the radio ID. The `Options=`
subscription handles the rest; no per-talkgroup server config is needed.

## Tests

```sh
make test
```

Two suites:

**`test_rewrite`** — the loopback identity. A synthetic capture covering every
frame kind is rewritten and checked to confirm the AMBE is bit-identical, the
header addressing is entirely ours, the full LC in the voice header and
terminator decodes back to the new addressing, the embedded LC fragments in
bursts B–E match, the slot-type/sync window (which carries the colour code) is
untouched, and a stray private-call bit is forced clear.

**`test_lanes`** — the concurrency model. Two calls interleaved across TS1 and
TS2 are both captured in full; two streams arriving on one slot don't thrash it
(first-come-wins); and the ingress gate rejects the wrong talkgroup, an unused
slot, and private calls without opening a capture.

## Notes and limitations

- One call per slot at a time. A transmission arriving on a slot that is already
  playing back is ignored rather than truncating the echo. The other slot is
  unaffected.
- HBP only. There is no IPSC support; reach it through a bridge.
- No announcements, no ID lookups, no database, no dashboard. It records and it
  replays.

## Companions

- [HBlink3](https://github.com/n0mjs710/hblink3) — DMR transit router / conference bridge
- [HBlink4](https://github.com/n0mjs710/HBlink4) — repeater-oriented DMR endpoint server
- [ipsc2hbpc](https://github.com/n0mjs710/ipsc2hbpc) — IPSC ⇄ HBP translator
- [cc2obp](https://github.com/n0mjs710/cc2obp) — c-Bridge CC-CC ⇄ OpenBridge translator

The DMR DSP module in [src/dmr/](src/dmr/) and the HBP client in
[src/hbp.c](src/hbp.c) are lifted from `ipsc2hbpc`, which ported them from
`dmr_utils3`.

## License

Copyright (C) 2026 Cortney T. Buffington, N0MJS — n0mjs@me.com

GNU GPLv3; see [LICENSE](LICENSE).

### No Support Is Provided

This is not commercial software. It is provided free of charge. If you have
problems, the author will try to help if possible, but please have no
expectations for support.
