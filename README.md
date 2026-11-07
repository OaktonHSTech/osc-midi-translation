# SQ OSC Bridge (C)

Small POSIX C daemon for the OpenWrt Netgear R6220 / MT7621.

## Fixed endpoints

- OSC UDP bind: `10.101.1.1:8000`
- SQ MIDI-over-TCP: `192.168.2.100:51325`
- SQ MIDI channel: 1
- No dry-run mode; no network or firewall changes.

## OSC commands

All messages have no arguments. OSC bundles are supported.

| OSC path | Action |
|---|---|
| `/scene/1/run` through `/scene/300/run` | Recall that position in **All Scenes** |
| `/chan/1/toggle` through `/chan/48/toggle` | Toggle input-channel mute |
| `/mtgrp/1/toggle` through `/mtgrp/8/toggle` | Toggle mute-group state |

Scene numbering is 1-based. The scene must exist as a saved scene on the SQ.
If the SQ Scene Manager is in Cue List mode, these paths still use **All Scenes**
numbering, not Cue List positions.

## Build on the router (if a compiler is available)

```sh
opkg update
opkg install gcc make
gcc -Os -Wall -Wextra -o sq-osc-bridge src/main.c
```

This is usually not the preferred route on a small router. Prefer the matching
OpenWrt SDK from the same firmware release.

## Cross-compile with the matching OpenWrt SDK

Download the SDK for your exact OpenWrt release and `ramips/mt7621` target from:
https://downloads.openwrt.org/releases/

For OpenWrt 25.12.2, the SDK directory is:
`openwrt-sdk-25.12.2-ramips-mt7621_gcc-14.3.0_musl.Linux-x86_64`

Extract it, then use its target compiler. From the SDK root:

```sh
find staging_dir -type f -name '*gcc' | head
```

The SDK's target triplet is exposed by its `staging_dir/toolchain-*` directory.
A typical compiler path can be set as:

```sh
export CROSS_COMPILE="$(find staging_dir/toolchain-* -type f -name '*-gcc' | head -n1 | sed 's/gcc$//')"
${CROSS_COMPILE}gcc -Os -Wall -Wextra -static -o sq-osc-bridge src/main.c
```

If static linking fails because the SDK omits static libc archives, remove `-static`;
the resulting binary will use the router's musl libc. Do not use a generic MIPS
compiler: use the SDK matching the router's OpenWrt release and `ramips/mt7621`.

## Install on OpenWrt

Copy the compiled binary to the router:

```sh
scp sq-osc-bridge root@192.168.1.1:/usr/sbin/
```

Copy the init script:

```sh
scp files/sq-osc-bridge.init root@192.168.1.1:/etc/init.d/sq-osc-bridge
ssh root@192.168.1.1 'chmod 755 /usr/sbin/sq-osc-bridge /etc/init.d/sq-osc-bridge'
```

Enable and start:

```sh
/etc/init.d/sq-osc-bridge enable
/etc/init.d/sq-osc-bridge start
logread -f
```

Stop/disable:

```sh
/etc/init.d/sq-osc-bridge stop
/etc/init.d/sq-osc-bridge disable
```

## Safety behavior

- Commands received while the SQ is disconnected are dropped, never queued.
- A failed or partial TCP `send()` causes the connection to be closed.
- The daemon does not replay a command after an ambiguous write, especially toggles.
- It drains the SQ's initial TCP state burst before sending MIDI commands.
- Unknown paths, malformed OSC, and out-of-range indices are ignored.

## Protocol details

- Scene recall uses Bank Select MSB (CC 0), followed by Program Change.
  Scenes 1–128 use bank 0, 129–256 bank 1, and 257–300 bank 2.
- Input channel mute NRPN addresses are MSB `00`, LSB `channel - 1`.
- Mute group addresses are MSB `04`, LSB `group - 1`.
- Toggle uses NRPN increment: CC 99 (0x63), CC 98 (0x62), CC 96 (0x60).

Based on the Allen & Heath SQ MIDI Protocol, Firmware V1.6:
https://support.allen-heath.com/hc/en-gb/articles/47793519540497-SQ-MIDI-Protocol-Firmware-V1-6
