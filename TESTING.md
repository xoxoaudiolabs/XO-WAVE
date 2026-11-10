# Testing status

What has been checked, how, and what still needs trying on real hardware. Last updated for 0.3.6.

Three levels:

- **Automated:** runs on every build (`make test`) and again on the Pi before every update installs.
- **On a Pi 3:** tried on a real Raspberry Pi 3 with the parts listed in the README.
- **Not yet tried:** written and covered by the offline tests where possible, but not yet confirmed on hardware.

## Automated

| Area | What's checked | Command |
|---|---|---|
| Engine | Aliasing, stability at extreme settings, voice stealing, all 6 filter types maxed, sub oscillator pitch, every FX maxed with 95% feedback, mod matrix, chord memory, arpeggiator rate and HOLD, MIDI clock follow and fallback | `make test` |
| Stress | Random MIDI storms, out-of-range parameters, preset changes, arp/chord/hold/sustain toggling, MIDI clock; output stays finite and bounded, notes always end, periods stay on time | `make stress SECONDS=120` |
| Files | Hostile WAVs, presets and settings files (nan/inf, truncated, huge) | `make filefuzz SAN=1` |
| Network | Hostile HTTP requests to the web app; random Studio-link packets | `make netfuzz SAN=1` |
| Studio link | Session handshake, MIDI both ways, audio packets and the mix back, against XOXO Studio's own `netmidi.py` | `tests/link_test.cpp` + `tests/fake_studio.py` |
| Touchscreen | Every page renders (see [`docs/screenshots/`](docs/screenshots/)) | `./xowave -c xowave.conf --ui-snapshot DIR` |

The Mac runs the fuzz tests with AddressSanitizer and UBSan. The Pi runs them with UBSan only, because AddressSanitizer doesn't run on the Pi 3's kernel.

## On a Pi 3

- [x] Boots into XO-Wave; touchscreen and web app (http://xo-wave.local) working
- [x] CPU: worst case (8 voices, 2 × 4 unison, sub, noise, ladder filter, all FX) uses 70% of one core (`--bench`)
- [x] CPU guard: an extreme MIDI storm went from 740 late periods out of 4000 to 0
- [x] Stress tests with UBSan
- [x] Studio link, MIDI only, to a real XOXO Studio over Wi-Fi
- [x] Network update over the deploy key (private repo): 0.3.5 → 0.3.6 built, passed its tests and installed on the Pi
- [x] Web app layout on a phone-size screen (375 px)
- [x] Update check and install over public HTTPS with no key (0.3.6 → 5776b5a); still works with the deploy key deleted from GitHub

## Left to test

Tick these off as you go.

### Install and updates
- [ ] Fresh SD card: `git clone` install from the README, start to finish
- [ ] Update rollback: a version that won't start puts the old one back

### Audio and MIDI
- [ ] 3.5 mm jack: clean sound, no xruns at `audio.period` default
- [ ] USB MIDI keyboard: plays; unplug and replug while running
- [ ] MIDI learn, the default CC map, sustain pedal
- [ ] Program change 0–95 selects presets, higher numbers wrap
- [ ] MIDI clock from a USB device, including start/stop; arp, synced delay and LFO follow it
- [ ] All 96 presets on the jack: similar loudness, nothing harsh or silent

### Bluetooth
- [ ] BLE MIDI controller: pair, plays, reconnects after switching it off and on
- [ ] Bluetooth speaker plays alongside the jack
- [ ] A Bluetooth dropout doesn't glitch the jack
- [ ] Devices reconnect after a reboot
- [ ] Wi-Fi + Bluetooth together on the Pi 3 radio; `--no-wifi` if it crackles

### Touchscreen
- [ ] First-boot calibration, and redo from SETTINGS → DISPLAY
- [ ] Every page and knob responds the right way
- [ ] XY pad, undo, compare, favourites
- [ ] Save, save as (on-screen keyboard), delete
- [ ] "CPU: MAX n" shows in SETTINGS → SYSTEM while the guard is limiting
- [ ] Reboot and shut down buttons

### Power and storage
- [ ] Pull the power mid-edit: comes back with the same sound and settings
- [ ] USB stick, FAT32 and exFAT: import a wavetable (then RESTART), import presets, back up presets

### Web app
- [ ] From a phone: every control, presets, volume, tempo, keyboard
- [ ] Changes on the screen show on the page and the other way round

### XOXO Studio link
- [ ] Sound into an updated Studio's XO-WAVE mixer channel
- [ ] Direct Ethernet cable (link-local addresses, no router)
- [ ] Studio mix back on XO-Wave's headphones at different STUDIO MIX levels
- [ ] The Studio's REC records XO-WAVE; program change from the Studio picks a preset
- [ ] Arpeggiator stays on the grid with the Studio's clock
- [ ] Older Studio: MIDI only, and switches to sound after the Studio updates (within 30 s, no restart)
- [ ] Link off and on from SETTINGS → STUDIO; LINK light in the top bar
