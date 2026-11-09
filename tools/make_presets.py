#!/usr/bin/env python3
"""Writes XO-Wave's factory presets 09-96 into presets/ (01-08 are hand-made).

96 factory presets + 32 free slots for the user = 128, the MIDI program-change
range. Each preset is its category's starting point plus its own settings.
Re-run after editing; files are rewritten. Usage: python3 tools/make_presets.py
"""
import os
import re

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "..", "presets")

# Wavetables: Basic Shapes (pos 0 sine, .33 tri, .66 saw, 1 square), PWM, Sync Saw,
# Formant, FM, Wavefold, Organ, Digital, Harmonic Sweep, Grit.
# Filter modes: 0 LP12, 1 BP, 2 HP, 3 LP24, 4 NOTCH, 5 COMB.
# Divisions: 0 free, 3 1/16, 4 1/8T, 5 1/16D, 6 1/8, 7 1/4T, 8 1/8D, 9 1/4, 11 1/4D, 12 1/2, 13 1 bar, 14 2 bars, 15 4 bars.
# Mod sources: 1 LFO1, 2 LFO2, 3 MOD ENV, 4 AMP ENV, 5 VEL, 6 WHEEL, 7 AFTERTOUCH, 8 KEY, 9 RANDOM.
# Mod destinations: 1 A POS, 2 B POS, 3 CUTOFF, 4 RESO, 5 PITCH, 6 A LEVEL, 7 B LEVEL, 8 DRIVE.

BASE = {
    "PAD": dict(a_table="Basic Shapes", a_pos=0.66, a_unison=3, a_detune=12, spread=0.85,
                amp_attack=0.9, amp_decay=1.5, amp_sustain=0.85, amp_release=2.8,
                cutoff=3800, resonance=0.15, filter_env=0.12, mod_attack=1.2, mod_decay=2.0, mod_sustain=0.4,
                lfo_rate=0.2, lfo_pos=0.3, chorus_mix=0.35, reverb_mix=0.45, reverb_size=0.8,
                vel_amp=0.3, volume=0.6),
    "LEAD": dict(a_table="Basic Shapes", a_pos=0.66, a_unison=2, a_detune=7,
                 amp_attack=0.005, amp_decay=0.4, amp_sustain=0.9, amp_release=0.2,
                 cutoff=4200, resonance=0.25, filter_env=0.35, mod_decay=0.45, mod_sustain=0.3,
                 glide=0.04, lfo_rate=5.5, delay_mix=0.18, delay_sync=8, delay_feedback=0.3,
                 reverb_mix=0.15, vel_amp=0.4, volume=0.62),
    "BASS": dict(a_table="Basic Shapes", a_pos=0.66, a_unison=1, filter_mode=3,
                 amp_attack=0.002, amp_decay=0.4, amp_sustain=0.75, amp_release=0.12,
                 cutoff=520, resonance=0.3, filter_env=0.55, filter_key=0.3, mod_attack=0.001, mod_decay=0.25,
                 mod_sustain=0.0, sub_level=0.55, drive=0.25, vel_filter=0.5, vel_amp=0.5, volume=0.68),
    "KEYS": dict(a_table="FM", a_pos=0.35, amp_attack=0.002, amp_decay=2.2, amp_sustain=0.3, amp_release=0.7,
                 cutoff=7000, resonance=0.1, filter_env=0.15, mod_decay=1.0, mod_sustain=0.2,
                 vel_amp=0.8, vel_filter=0.5, chorus_mix=0.25, reverb_mix=0.25, volume=0.62),
    "PLUCK": dict(a_table="Basic Shapes", a_pos=0.66, amp_attack=0.001, amp_decay=0.45, amp_sustain=0.0,
                  amp_release=0.35, cutoff=1100, resonance=0.25, filter_env=0.55, mod_decay=0.22, mod_sustain=0.0,
                  vel_amp=0.7, vel_filter=0.6, delay_mix=0.25, delay_sync=8, delay_feedback=0.35,
                  reverb_mix=0.2, volume=0.66),
    "ARP": dict(a_table="Basic Shapes", a_pos=0.66, amp_attack=0.001, amp_decay=0.3, amp_sustain=0.0,
                amp_release=0.2, cutoff=1500, resonance=0.3, filter_env=0.45, mod_decay=0.18, mod_sustain=0.0,
                arp_mode=1, arp_rate=3, arp_octaves=2, arp_gate=0.5, delay_mix=0.22, delay_sync=8,
                delay_feedback=0.3, reverb_mix=0.18, vel_amp=0.4, volume=0.64),
    "FX": dict(a_table="Grit", a_pos=0.5, a_unison=2, a_detune=20, amp_attack=0.4, amp_decay=2.0, amp_sustain=0.7,
               amp_release=3.0, cutoff=2500, resonance=0.4, lfo_rate=0.3, reverb_mix=0.55, reverb_size=0.9,
               delay_mix=0.25, delay_sync=11, delay_feedback=0.5, volume=0.58),
    "BRASS": dict(a_table="Basic Shapes", a_pos=0.66, a_unison=3, a_detune=9, spread=0.6,
                  amp_attack=0.06, amp_decay=0.6, amp_sustain=0.8, amp_release=0.35,
                  cutoff=1400, resonance=0.15, filter_env=0.45, mod_attack=0.08, mod_decay=0.5, mod_sustain=0.4,
                  vel_filter=0.6, vel_amp=0.5, chorus_mix=0.2, reverb_mix=0.3, volume=0.6),
    "STR": dict(a_table="Basic Shapes", a_pos=0.66, a_unison=3, a_detune=14, spread=0.9,
                amp_attack=0.35, amp_decay=1.0, amp_sustain=0.9, amp_release=1.2,
                cutoff=3000, resonance=0.1, filter_env=0.1, mod_attack=0.5, mod_decay=1.0, mod_sustain=0.6,
                lfo_rate=5.0, lfo_pitch=0.04, chorus_mix=0.4, reverb_mix=0.4, reverb_size=0.75, volume=0.6),
}

P = []   # (category, name, settings)
def add(cat, name, **kw):
    P.append((cat, name, kw))

# ---- PAD (14)
add("PAD", "Velvet Dusk", a_table="Harmonic Sweep", a_pos=0.25, b_table="Basic Shapes", b_pos=0.33, b_level=0.45,
    b_semi=12, lfo_pos=0.35, cutoff=3200)
add("PAD", "Northern Glass", a_table="Formant", a_pos=0.15, lfo_rate=0.12, lfo_pos=0.6, cutoff=5200,
    b_table="Harmonic Sweep", b_pos=0.7, b_level=0.35, b_semi=19, reverb_size=0.9)
add("PAD", "Warm Analog", a_unison=4, a_detune=16, cutoff=1800, filter_mode=3, resonance=0.2, filter_env=0.25,
    b_table="Basic Shapes", b_pos=0.66, b_level=0.4, b_semi=-12, b_unison=2, b_detune=8)
add("PAD", "Choir of Machines", a_table="Formant", a_pos=0.5, lfo_shape=1, lfo_rate=0.15, lfo_pos=0.8,
    cutoff=4500, chorus_mix=0.5, mod1_src=2, mod1_dst=3, mod1_amt=0.15, lfo2_rate=0.07)
add("PAD", "Slow Bloom", amp_attack=2.5, amp_release=4.5, a_table="Wavefold", a_pos=0.0, menv_pos=0.6,
    mod_attack=3.5, mod_decay=3.0, mod_sustain=0.6, cutoff=2600, filter_env=0.3)
add("PAD", "Organ Halo", a_table="Organ", a_pos=0.75, a_unison=2, a_detune=6, cutoff=6000, chorus_mix=0.55,
    chorus_rate=0.9, reverb_mix=0.5)
add("PAD", "Digital Mist", a_table="Digital", a_pos=0.1, a_unison=3, a_detune=18, lfo_rate=0.08, lfo_pos=0.5,
    cutoff=2200, resonance=0.3, delay_mix=0.2, delay_sync=13, delay_feedback=0.55)
add("PAD", "PWM Strings Pad", a_table="PWM", a_pos=0.2, lfo_rate=0.35, lfo_pos=0.7, a_unison=3, a_detune=10,
    cutoff=3500, chorus_mix=0.45)
add("PAD", "Sub Ocean", a_table="Basic Shapes", a_pos=0.2, sub_level=0.5, cutoff=1400, filter_mode=3,
    lfo_cutoff=0.25, lfo_rate=0.1, reverb_size=0.95, reverb_mix=0.55, amp_release=4.0)
add("PAD", "Shimmer Field", a_table="Harmonic Sweep", a_pos=0.6, b_table="Harmonic Sweep", b_pos=0.9, b_semi=12,
    b_level=0.5, b_unison=3, b_detune=14, filter_mode=2, cutoff=300, reverb_mix=0.65, reverb_size=0.95,
    delay_mix=0.25, delay_sync=11, delay_feedback=0.5)
add("PAD", "Grit Veil", a_table="Grit", a_pos=0.3, lfo_pos=0.4, lfo_rate=0.18, cutoff=2400, resonance=0.25,
    noise=0.05, chorus_mix=0.4)
add("PAD", "Hollow Moon", a_table="Basic Shapes", a_pos=1.0, b_table="Basic Shapes", b_pos=0.33, b_level=0.5,
    b_semi=7, filter_mode=4, cutoff=1200, resonance=0.6, lfo_cutoff=0.35, lfo_rate=0.07)
add("PAD", "Sync Aurora", a_table="Sync Saw", a_pos=0.2, lfo_pos=0.45, lfo_rate=0.11, lfo_shape=1, cutoff=4000,
    chorus_mix=0.4, a_unison=3, a_detune=11)
add("PAD", "Breathing Fifths", chord=7, a_table="Formant", a_pos=0.4, mod1_src=2, mod1_dst=1, mod1_amt=0.4,
    lfo2_rate=0.2, lfo2_shape=0, cutoff=3000, amp_attack=1.4)

# ---- LEAD (12)
add("LEAD", "Razor Sync", a_table="Sync Saw", a_pos=0.1, menv_pos=0.7, mod_decay=0.5, cutoff=5000, resonance=0.2)
add("LEAD", "Mono Silk", a_table="Basic Shapes", a_pos=0.33, a_unison=1, cutoff=6000, filter_env=0.2, glide=0.08,
    lfo_pitch=0.08, delay_mix=0.25)
add("LEAD", "Formant Talker", a_table="Formant", a_pos=0.2, mod1_src=6, mod1_dst=1, mod1_amt=0.7,
    lfo_rate=4.0, lfo_pos=0.25, cutoff=4500)
add("LEAD", "Square Hero", a_table="Basic Shapes", a_pos=1.0, b_table="Basic Shapes", b_pos=0.66, b_level=0.5,
    b_semi=12, b_fine=6, cutoff=3200, resonance=0.35, filter_mode=3)
add("LEAD", "FM Whistle", a_table="FM", a_pos=0.15, a_unison=1, cutoff=9000, filter_env=0.0, lfo_pitch=0.1,
    glide=0.06, reverb_mix=0.3, delay_mix=0.28)
add("LEAD", "Fold Screamer", a_table="Wavefold", a_pos=0.35, mod1_src=7, mod1_dst=1, mod1_amt=0.6, drive=0.4,
    cutoff=3800, resonance=0.4, fx_drive=0.2)
add("LEAD", "Digital Chirp", a_table="Digital", a_pos=0.6, menv_pos=-0.5, mod_decay=0.25, cutoff=7000,
    delay_sync=6, delay_mix=0.3)
add("LEAD", "Portamento Glass", a_table="Harmonic Sweep", a_pos=0.4, glide=0.18, cutoff=7500, reverb_mix=0.35,
    lfo_pos=0.2, lfo_rate=3.0)
add("LEAD", "Acid Saw", a_table="Basic Shapes", a_pos=0.66, a_unison=1, filter_mode=3, cutoff=700, resonance=0.75,
    filter_env=0.65, mod_decay=0.22, mod_sustain=0.0, drive=0.4, glide=0.05, delay_mix=0.2)
add("LEAD", "Sync Vibrato", a_table="Sync Saw", a_pos=0.45, lfo_pitch=0.12, lfo_rate=6.0, mod1_src=1, mod1_dst=1,
    mod1_amt=0.2, cutoff=4500)
add("LEAD", "PWM Solo", a_table="PWM", a_pos=0.3, lfo_pos=0.5, lfo_rate=0.6, cutoff=3500, glide=0.05)
add("LEAD", "Grit Lead", a_table="Grit", a_pos=0.6, a_unison=2, a_detune=10, drive=0.35, cutoff=4000,
    resonance=0.35, fx_drive=0.25)

# ---- BASS (14)
add("BASS", "Moog Floor", cutoff=420, resonance=0.35, sub_level=0.6)
add("BASS", "Acid Line", a_unison=1, sub_level=0.0, cutoff=320, resonance=0.82, filter_env=0.75, mod_decay=0.2,
    drive=0.45, glide=0.06)
add("BASS", "Sub Pressure", a_table="Basic Shapes", a_pos=0.0, a_level=0.5, sub_level=0.8, sub_octave=1,
    cutoff=900, filter_env=0.2, drive=0.15)
add("BASS", "Formant Growl", a_table="Formant", a_pos=0.4, lfo_rate=2.0, lfo_pos=0.35, cutoff=650, drive=0.5,
    resonance=0.4)
add("BASS", "Reese", a_unison=4, a_detune=22, spread=0.4, cutoff=800, filter_env=0.25, sub_level=0.5, drive=0.3,
    amp_sustain=0.95)
add("BASS", "FM Punch", a_table="FM", a_pos=0.45, menv_pos=-0.4, mod_decay=0.15, cutoff=1400, filter_env=0.4,
    sub_level=0.4)
add("BASS", "Wavefold Bass", a_table="Wavefold", a_pos=0.25, menv_pos=0.5, mod_decay=0.2, cutoff=900, drive=0.35)
add("BASS", "Square Sub", a_table="Basic Shapes", a_pos=1.0, sub_level=0.6, sub_shape=1, cutoff=600,
    resonance=0.2, filter_env=0.4)
add("BASS", "Digital Bite", a_table="Digital", a_pos=0.5, cutoff=1000, filter_env=0.5, mod_decay=0.12,
    drive=0.3, sub_level=0.5)
add("BASS", "Rubber Pluck", amp_sustain=0.0, amp_decay=0.5, cutoff=550, resonance=0.5, filter_env=0.6,
    mod_decay=0.18, glide=0.03, a_level=1.0, sub_level=0.7)
add("BASS", "Organ Pedal", a_table="Organ", a_pos=0.25, filter_mode=0, cutoff=1500, filter_env=0.0,
    amp_sustain=1.0, sub_level=0.35, drive=0.1)
add("BASS", "Sync Bass", a_table="Sync Saw", a_pos=0.3, menv_pos=0.5, mod_decay=0.3, cutoff=1200, sub_level=0.5)
add("BASS", "Grit Sub", a_table="Grit", a_pos=0.4, cutoff=700, drive=0.55, sub_level=0.6, fx_drive=0.2)
add("BASS", "Velocity Funk", a_table="Basic Shapes", a_pos=0.8, cutoff=380, resonance=0.5, filter_env=0.6,
    vel_filter=0.9, mod1_src=5, mod1_dst=4, mod1_amt=0.25)

# ---- KEYS (10)
add("KEYS", "Tine Piano", a_table="FM", a_pos=0.3, b_table="Basic Shapes", b_pos=0.0, b_level=0.5,
    menv_pos=-0.3, mod_decay=0.6, chorus_mix=0.3)
add("KEYS", "Glass Bells", a_table="FM", a_pos=0.7, amp_decay=3.5, amp_sustain=0.0, amp_release=2.5,
    b_table="Harmonic Sweep", b_pos=0.8, b_semi=24, b_level=0.25, reverb_mix=0.4)
add("KEYS", "Drawbar Organ", a_table="Organ", a_pos=0.6, amp_attack=0.005, amp_decay=0.1, amp_sustain=1.0,
    amp_release=0.08, filter_env=0.0, cutoff=9000, chorus_mix=0.45, chorus_rate=5.0, chorus_depth=0.3,
    vel_amp=0.0)
add("KEYS", "Clav Funk", a_table="PWM", a_pos=0.45, a_level=1.0, amp_decay=0.6, amp_sustain=0.15, cutoff=2400,
    filter_env=0.4, filter_mode=0, resonance=0.4, chorus_mix=0.0, mod_decay=0.25)
add("KEYS", "Digital Rhodes", a_table="Digital", a_pos=0.15, b_table="FM", b_pos=0.2, b_level=0.5,
    cutoff=4500, chorus_mix=0.35)
add("KEYS", "Wurly Grit", a_table="Wavefold", a_pos=0.2, amp_decay=1.6, drive=0.3, cutoff=3500, chorus_mix=0.2,
    mod1_src=5, mod1_dst=1, mod1_amt=0.35)
add("KEYS", "Celesta", a_table="Harmonic Sweep", a_pos=0.85, a_semi=12, amp_decay=1.5, amp_sustain=0.0,
    amp_release=1.2, cutoff=9000, reverb_mix=0.4)
add("KEYS", "Formant Keys", a_table="Formant", a_pos=0.6, amp_decay=1.8, cutoff=5000, menv_pos=-0.4)
add("KEYS", "Toy Piano", a_table="Basic Shapes", a_pos=0.4, a_semi=12, amp_decay=0.9, amp_sustain=0.0,
    cutoff=6000, chorus_mix=0.0, delay_mix=0.15, delay_sync=6)
add("KEYS", "Mallet", a_table="FM", a_pos=0.55, amp_decay=1.0, amp_sustain=0.0, amp_release=0.6,
    b_table="Basic Shapes", b_pos=0.0, b_semi=12, b_level=0.4, reverb_mix=0.3)

# ---- PLUCK (10)
add("PLUCK", "Saw Pluck", a_unison=2, a_detune=8)
add("PLUCK", "Comb Harp", filter_mode=5, cutoff=440, filter_key=1.0, resonance=0.8, a_table="Basic Shapes",
    a_pos=0.66, noise=0.15, amp_decay=1.2, filter_env=0.0, delay_mix=0.15)
add("PLUCK", "Koto", a_table="FM", a_pos=0.25, amp_decay=0.9, cutoff=3500, filter_env=0.3, reverb_mix=0.3)
add("PLUCK", "Marimba Wood", a_table="Basic Shapes", a_pos=0.33, amp_decay=0.35, cutoff=2600, filter_env=0.2,
    sub_level=0.3, delay_mix=0.1)
add("PLUCK", "Digital Drop", a_table="Digital", a_pos=0.7, menv_pos=-0.6, mod_decay=0.2, cutoff=2000)
add("PLUCK", "Formant Pop", a_table="Formant", a_pos=0.55, a_level=1.0, menv_pos=-0.5, mod_decay=0.2, cutoff=4500,
    filter_env=0.35, amp_decay=0.6)
add("PLUCK", "Ping-Pong Stab", a_unison=3, a_detune=12, chord=2, delay_mix=0.35, delay_feedback=0.45,
    delay_sync=8, cutoff=1600)
add("PLUCK", "Nylon", a_table="Basic Shapes", a_pos=0.4, amp_decay=1.0, cutoff=1800, filter_env=0.3,
    filter_key=0.7, reverb_mix=0.25, delay_mix=0.0)
add("PLUCK", "Glass Pluck", a_table="Harmonic Sweep", a_pos=0.5, amp_decay=0.7, cutoff=5000, reverb_mix=0.35)
add("PLUCK", "Fold Snap", a_table="Wavefold", a_pos=0.6, menv_pos=-0.6, mod_decay=0.12, drive=0.3, cutoff=2400)

# ---- ARP (10): run on the internal tempo, or follow the Studio's clock
add("ARP", "Up Sixteen", arp_mode=1, arp_octaves=2)
add("ARP", "Down Glass", arp_mode=2, a_table="Harmonic Sweep", a_pos=0.5, arp_octaves=2, cutoff=4000,
    reverb_mix=0.3)
add("ARP", "Bounce Triplets", arp_mode=3, arp_rate=4, arp_octaves=2, a_table="FM", a_pos=0.4, cutoff=3000)
add("ARP", "Random Rain", arp_mode=4, arp_octaves=3, a_table="Digital", a_pos=0.3, delay_mix=0.35,
    delay_feedback=0.5, reverb_mix=0.35)
add("ARP", "Played Order", arp_mode=5, arp_octaves=1, arp_gate=0.7, a_table="Sync Saw", a_pos=0.2)
add("ARP", "Chord Pulse", arp_mode=1, arp_octaves=1, chord=4, arp_rate=6, arp_gate=0.3, a_unison=2, a_detune=10)
add("ARP", "Acid Arp", arp_mode=3, arp_octaves=2, filter_mode=3, cutoff=500, resonance=0.75, filter_env=0.6,
    drive=0.35, sub_level=0.3)
add("ARP", "Formant Steps", arp_mode=1, arp_octaves=2, a_table="Formant", a_level=1.0, mod1_src=2, mod1_dst=1,
    mod1_amt=0.4, lfo2_sync=13, lfo2_shape=1, cutoff=4500, amp_decay=0.45, filter_env=0.3)
add("ARP", "Fifths Sequencer", arp_mode=1, chord=7, arp_octaves=3, arp_rate=5, a_table="PWM", a_pos=0.3)
add("ARP", "Dotted Echo", arp_mode=1, arp_rate=6, arp_octaves=2, delay_sync=8, delay_mix=0.4,
    delay_feedback=0.55, a_table="Basic Shapes", a_pos=0.9)

# ---- FX (10)
add("FX", "Noise Riser", a_level=0.3, noise=0.7, filter_mode=1, cutoff=300, resonance=0.6, filter_env=0.9,
    mod_attack=4.0, mod_decay=4.0, mod_sustain=1.0, amp_attack=2.0)
add("FX", "Sample & Hold", a_table="Digital", lfo_shape=4, lfo_rate=6.0, lfo_cutoff=0.6, lfo_pos=0.4,
    cutoff=1500, resonance=0.6, amp_attack=0.01, amp_sustain=0.9, amp_release=0.4)
add("FX", "Comb Drone", filter_mode=5, cutoff=110, resonance=0.85, noise=0.3, a_level=0.4, lfo_cutoff=0.1,
    lfo_rate=0.05)
add("FX", "Laser Zap", a_table="Basic Shapes", a_pos=0.66, a_unison=1, mod1_src=3, mod1_dst=5, mod1_amt=1.0,
    mod_decay=0.25, mod_sustain=0.0, amp_attack=0.001, amp_decay=0.3, amp_sustain=0.0, amp_release=0.1,
    cutoff=6000, reverb_mix=0.2, delay_mix=0.3)
add("FX", "Alien Choir", a_table="Formant", a_pos=0.8, mod1_src=9, mod1_dst=1, mod1_amt=0.5, mod2_src=2,
    mod2_dst=5, mod2_amt=0.03, lfo2_rate=0.3, lfo_pos=0.5)
add("FX", "Broken Radio", a_table="Grit", a_pos=0.8, filter_mode=1, cutoff=1500, resonance=0.5, noise=0.15,
    lfo_shape=4, lfo_rate=9.0, lfo_cutoff=0.4, fx_drive=0.5)
add("FX", "Wind Tunnel", a_level=0.0, noise=0.8, filter_mode=1, cutoff=800, resonance=0.7, lfo_cutoff=0.6,
    lfo_rate=0.12, lfo_shape=1)
add("FX", "Metal Rain", a_table="FM", a_pos=0.9, filter_mode=5, cutoff=800, resonance=0.7, amp_attack=0.001,
    amp_decay=0.8, amp_sustain=0.0, delay_mix=0.4, delay_feedback=0.6)
add("FX", "Space Swell", a_table="Harmonic Sweep", a_pos=0.9, amp_attack=3.0, reverb_mix=0.75, reverb_size=1.0,
    delay_mix=0.3, delay_sync=13, menv_pos=-0.8, mod_attack=3.0, mod_decay=3.0)
add("FX", "Glitch Steps", a_table="Digital", arp_mode=4, arp_rate=1, arp_octaves=3, arp_gate=0.25,
    amp_attack=0.001, amp_decay=0.15, amp_sustain=0.0, amp_release=0.1, cutoff=4000, reverb_mix=0.2)

# ---- BRASS / STR (8)
add("BRASS", "Brass Section", a_unison=3)
add("BRASS", "Synth Horns", a_table="PWM", a_pos=0.15, b_table="Basic Shapes", b_pos=0.66, b_level=0.45,
    b_semi=12, cutoff=1800)
add("BRASS", "Formant Brass", a_table="Formant", a_pos=0.3, menv_pos=0.4, cutoff=2000)
add("BRASS", "Stab Brass", amp_sustain=0.0, amp_decay=0.5, chord=2, cutoff=2200, reverb_mix=0.35)
add("STR", "String Ensemble")
add("STR", "Cello Section", a_table="Basic Shapes", a_pos=0.66, a_semi=-12, cutoff=1600, amp_attack=0.25,
    filter_mode=3, resonance=0.15)
add("STR", "Pizzicato", amp_attack=0.002, amp_decay=0.3, amp_sustain=0.0, amp_release=0.25, cutoff=2500,
    filter_env=0.3, mod_decay=0.15, chorus_mix=0.2)
add("STR", "Glass Strings", a_table="Harmonic Sweep", a_pos=0.4, lfo_pos=0.25, lfo_rate=0.15, cutoff=5000)

assert len(P) == 88, len(P)

# Loudness trims (dB) from tools/level_presets.py: every preset sounds about as loud
# as the others while playing. Applied to the patch volume.
TRIMS = {}
_tf = os.path.join(HERE, "preset_trims.txt")
if os.path.exists(_tf):
    for line in open(_tf):
        if line.strip():
            k, v = line.split()
            TRIMS[k] = float(v)

def slug(s):
    return re.sub(r"[^a-z0-9]+", "-", s.lower()).strip("-")

os.makedirs(OUT, exist_ok=True)
for f in os.listdir(OUT):   # remove generated presets from an earlier run (09-96)
    m = re.match(r"(\d\d)-", f)
    if m and 9 <= int(m.group(1)) <= 96 and f.endswith(".xowave"):
        os.remove(os.path.join(OUT, f))
for i, (cat, name, kw) in enumerate(P):
    settings = dict(BASE[cat])
    settings.update(kw)
    full = f"{cat} {name}"
    trim = TRIMS.get(slug(full), 0.0)
    settings["volume"] = round(min(2.0, max(0.05, settings["volume"] * 10 ** (trim / 20))), 3)
    with open(os.path.join(OUT, f"{i + 9:02d}-{slug(full)}.xowave"), "w") as f:
        f.write(f"name = {full}\n")
        for k, v in settings.items():
            f.write(f"{k} = {v}\n")
print(f"wrote {len(P)} presets (09-{len(P) + 8:02d})")
