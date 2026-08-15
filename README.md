# dmr-talkback

A DMR **voice test** endpoint. It connects to an HBP master exactly as an
ordinary repeater does, records one call, and plays it back so the caller can
hear how they sound.

Other networks call this a "parrot". This one doesn't.

It is a small, dependency-free C program. It is **not** part of HBlink3 or
HBlink4 — it is an endpoint that speaks the same protocol a repeater speaks, so
one binary works against HBlink3, HBlink4, FreeDMR, XLXD, or anything else that
serves the HomeBrew protocol.

---

## What it does

- Answers **group calls**, **private (unit) calls**, or both.
- The reply mirrors the call it received. A group call is replayed onto the same
  talkgroup so everyone listening hears it; a private call to the talkback's
  radio ID is answered with a private call back to whoever placed it.
- Every reply is sourced from the talkback's own radio ID, in the DMRD header
  **and** in the Link Control carried inside the voice payload, so radios and
  MMDVMHost display the talkback rather than the original caller.
- The audio is never re-encoded. AMBE comes back bit-for-bit as it went in.

## Configuration

One radio ID does everything: it is the HBP login ID, the DMRD repeater ID on
every packet transmitted, the source of the replayed call, and the ID users
private-call to reach the talkback.

```toml
[talkback]
radio_id        = 3120099
mode            = "BOTH"        # GROUP | UNIT | BOTH
group_ts2_tgids = [9990]        # talkgroups answered, per slot
unit_slots      = [1, 2]        # slots a private call is accepted on
replay_delay_ms = 2000
max_capture_secs = 30

[master]
ip         = "127.0.0.1"
port       = 54000
passphrase = "s3cr37w0rd"
```

See [talkback.toml.sample](talkback.toml.sample) for the fully commented
version.

The talkgroup lists are also sent to the master as an RPTO subscription
(`TS1=…;TS2=…`) at login. That is not cosmetic: HBlink4 will not deliver a
talkgroup to a repeater that hasn't subscribed to it. HBlink3 ignores it.

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

Keep the master on loopback where you can — then the passphrase never touches
the network.

**HBlink3** — add a `MODE: SERVER` system for it to log into, and give its
talkgroup a bridge in `rules.py` so traffic reaches it.

**HBlink4** — add an access-control entry for the radio ID. The `Options=`
subscription handles the rest; no per-talkgroup server config is needed.

## Tests

```sh
make test
```

The conformance vector is the loopback identity: a synthetic capture covering
every frame kind is rewritten and then checked to confirm the AMBE is
bit-identical, the header addressing is entirely ours, the full LC in the voice
header and terminator decodes back to the new addressing, the embedded LC
fragments in bursts B–E match, the slot-type/sync window (which carries the
colour code) is untouched, and group and unit replies differ in exactly three
places and nowhere else.

## Notes and limitations

- **Unit mode needs a master that routes private calls.** HBlink3 and HBlink4
  both do, and both learn where the talkback lives from its first transmission.
  XLXD and plain HBP masters may not route unit calls at all, which makes unit
  mode a no-op there. Group mode works everywhere.
- Only one call is handled at a time. A transmission arriving while a replay is
  in progress is ignored rather than truncating the echo.
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
