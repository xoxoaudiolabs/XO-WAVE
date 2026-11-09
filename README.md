# XO-Wave — standalone wavetable synth for Raspberry Pi 3

Boots straight into an 8-voice (up to 16) stereo wavetable synth. No screen, no desktop.

- **Audio out:** 3.5 mm jack (low latency, ~8 ms) **and** a Bluetooth speaker/headphones at the same time
- **MIDI in:** USB MIDI controllers and **Bluetooth (BLE) MIDI** controllers, auto-connected, hot-pluggable
- 2 wavetable oscillators per voice with up to 4× unison, stereo spread, multimode filter, 2 envelopes, LFO, glide
- 10 built-in wavetables. You can add your own Serum-style `.wav` wavetables
- 8 presets, switched with MIDI program change
- **Touchscreen UI** on a 3.5" SPI TFT (480×320, ILI9486 + ADS7846 touch) in the XOXO//AUDIO//LABS look

## Hardware

- Raspberry Pi 3 Model B (v1.2), 5 V / 2.5 A supply, 8 GB+ microSD
- Headphones or powered speakers on the 3.5 mm jack
- USB MIDI keyboard and/or a BLE MIDI controller (e.g. Korg microKEY Air, CME, ROLI)
- Optional Bluetooth speaker

## 1. Flash the SD card

1. Install **Raspberry Pi Imager** on your Mac.
2. Choose **Raspberry Pi 3** → **Raspberry Pi OS Lite (64-bit)**.
3. Click **Edit settings**:
   - Hostname: `xowave` (if you already set `xo4`, use `xo4.local` in the commands below)
   - Username/password: your choice, e.g. `pi`
   - Wi-Fi: your network (needed for setup; you can turn it off later)
   - Services tab: **Enable SSH** → password auth, or paste your public key (`cat ~/.ssh/id_ed25519.pub`)
4. Write the card, put it in the Pi, power on, wait ~2 minutes.

## 2. Copy XO-Wave to the Pi and install

From the Mac:

```bash
rsync -av --exclude build --exclude /xowave --exclude demo.wav "$HOME/Desktop/Claude Code/XOXO Audio Lab/XO-4 Standalone /" pi@xowave.local:xowave/
```

```bash
ssh pi@xowave.local
```

On the Pi:

```bash
cd ~/xowave && sudo ./install/setup.sh
```

```bash
sudo reboot
```

After the reboot, XO-Wave starts by itself. Plug in a USB keyboard and play. Logs: `journalctl -u xowave -f`.

## 3. Bluetooth

```bash
sudo xowave-bt scan
```

Put the device in pairing mode before scanning. Then pair it using the MAC address from the list:

```bash
sudo xowave-bt pair AA:BB:CC:DD:EE:FF audio
```

```bash
sudo xowave-bt pair 11:22:33:44:55:66 midi
```

`xowave-bt status` shows what's paired. A background service reconnects these devices whenever you switch them back on.

**Bluetooth audio** plays alongside the 3.5 mm jack. The jack is the timing master, so a Bluetooth dropout never glitches the jack. Bluetooth itself adds about 150–250 ms of delay, so it's good for listening or for an audience speaker, but play from the jack or headphones. If it stutters, raise `bluetooth.latency_ms` in `/etc/xowave.conf`.

**Pi 3 radio tip:** the Pi 3 shares a single radio between Wi-Fi and Bluetooth. If Bluetooth audio crackles, install with `sudo ./install/setup.sh --no-wifi`, or add a USB Bluetooth dongle. Without Wi-Fi you'll need Ethernet or a keyboard to get back in.

**Bluetooth MIDI** uses BlueZ's built-in BLE MIDI support: the controller appears as a normal MIDI port and XO-Wave connects to it automatically. If `xowave-bt pair … midi` says no port appeared, check `aconnect -l`. If no port ever shows up, your BlueZ was built without MIDI support (`--enable-midi`).
At the moment the Pi connects *to* BLE MIDI controllers. Letting a phone or iPad connect *to the Pi* as a Bluetooth MIDI device needs an extra BLE MIDI server; that isn't built yet.

## Touchscreen

Tabs along the bottom:

| Tab | What's there |
|---|---|
| **OSC** | 3D wavetable view (the frame being played is drawn in signal red and follows modulation live). Drag on it to scan the table. OSC A / OSC B switch, wavetable picker, position, level, semi, fine, unison, detune |
| **FILTER** | Low / band / high-pass, live response curve, cutoff, resonance, envelope amount, key tracking, drive, velocity |
| **ENV** | Amp and mod envelopes with shape previews |
| **MOD** | LFO shape/rate and destinations, mod-env → position, velocity, glide, bend range, stereo spread, volume |
| **KEYS** | Two-octave keyboard, octave shift, hold (latch), all notes off |
| **PRESETS** | Load, save, save as (on-screen keyboard), delete |
| **SETTINGS** | MIDI (channel, MIDI learn, connected inputs) · Bluetooth (scan & pair, forget, latency) · Audio (buffer, polyphony) · Display (touch calibration, knob speed) · System (status, restart, reboot, shut down) |

Knobs: touch a knob and drag **up or right** to turn it up. The top bar shows the preset (◀ ▶ to step), MIDI activity, Bluetooth status, voice count and output level.

The screen calibrates itself the first time XO-Wave starts: tap the four targets. You can redo this in **SETTINGS → DISPLAY**. Settings changed on the screen are saved to `/var/lib/xowave/state.conf`.

The UI uses [LVGL](https://lvgl.io) (MIT), with the **Anton** and **IBM Plex Mono** fonts (SIL Open Font License, see `fonts/`).

## Controls (default MIDI CC map)

| CC | Parameter | CC | Parameter |
|---|---|---|---|
| 1 | Mod wheel → vibrato | 64 | Sustain pedal |
| 74 | Filter cutoff | 71 | Resonance |
| 73 | Amp attack | 72 | Amp release |
| 20 | Osc A wavetable position | 21 | Osc B wavetable position |
| 22 | Osc A wavetable select | 23 | Osc B wavetable select |
| 24 | LFO rate | 25 | LFO → position |
| 26 | Filter envelope amount | 27 | Mod env → position |
| 7 | Volume | 5 | Glide |

Change these in `/etc/xowave.conf` (`cc.<number> = <parameter>`). `xowave --list-params` lists every parameter. Program change 0–7 selects a preset.

## Presets and wavetables

- Presets are text files in `/opt/xowave/presets/*.xowave`, one `parameter = value` per line. Wavetables can be given by name, e.g. `a_table = Formant`. Copy one, edit it, then run `sudo systemctl restart xowave`.
- Put Serum-style wavetable `.wav` files (2048 samples per frame) in `/opt/xowave/wavetables/`. They're added after the 10 built-in tables. `xowave -c /etc/xowave.conf --list-tables` shows their numbers.

Built-in tables: Basic Shapes (sine→tri→saw→square), PWM, Sync Saw, Formant, FM, Wavefold, Organ, Digital, Harmonic Sweep, Grit.

## Troubleshooting

| Problem | Fix |
|---|---|
| No sound from jack | `xowave --list-devices`, then set `audio.device` (older images call the jack `CARD=ALSA`). Check the volume: `alsamixer` |
| Clicks / "xruns" in log | `audio.period = 256` in `/etc/xowave.conf`, or lower `synth.voices` |
| CPU headroom | `xowave -c /etc/xowave.conf --bench` (worst case: all voices, 4× unison on both oscs) |
| Keyboard not responding | `aconnect -l` should show XO-Wave connected; check `midi.channel` |

## Developing on the Mac

`make` builds an offline version (no audio hardware needed):

```bash
make && ./xowave -c xowave.conf --render demo.wav
```

```bash
make test
```

`--render` plays a chord through each preset and writes a WAV file. `make test` checks aliasing, stability at extreme settings and voice stealing. Use `--bench` to measure CPU.

`./xowave -c xowave.conf --ui-snapshot DIR` renders every touchscreen page to PNG files, with no display needed.

Code layout: `src/ui/` (touchscreen UI), `src/engine_link.h` (lock-free UI ↔ audio link), `src/synth.*` (voices and modulation), `src/wavetable.*` (band-limited mipmapped tables), `src/dsp.h` (filter and envelopes), `src/alsa_io.*` (audio and MIDI), `src/bt_output.*` (Bluetooth output), `install/` (Pi setup and services).
