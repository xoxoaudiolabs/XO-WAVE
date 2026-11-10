#!/usr/bin/env bash
# XO-Wave installer for Raspberry Pi 3 (Raspberry Pi OS Lite, Bookworm/Trixie).
#   sudo ./install/setup.sh            # install + start at boot
#   sudo ./install/setup.sh --no-screen  # skip the 3.5" TFT touchscreen setup
#   sudo ./install/setup.sh --no-wifi  # also turn off Wi-Fi (Pi 3 Wi-Fi and
#                                      # Bluetooth share one radio; Bluetooth
#                                      # audio is much steadier with Wi-Fi off)
set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "Run with sudo: sudo $0"; exit 1; }
HERE="$(cd "$(dirname "$0")/.." && pwd)"
RUN_USER="${SUDO_USER:-pi}"
NO_WIFI=0
NO_SCREEN=0
UPDATE=0   # --update: called by xowave-update after it built and tested (no apt, no build)
for a in "$@"; do
    [[ $a == --update ]] && UPDATE=1
    [[ $a == --no-wifi ]] && NO_WIFI=1
    [[ $a == --no-screen ]] && NO_SCREEN=1
done

if [[ $UPDATE == 0 ]]; then
echo "==> Installing packages"
apt-get update
apt-get install -y build-essential libasound2-dev alsa-utils bluez rfkill git
if ! apt-get install -y bluez-alsa-utils libasound2-plugin-bluez; then
    echo "!! BlueALSA packages not found - Bluetooth audio output will be unavailable."
fi

echo "==> Building XO-Wave"
sudo -u "$RUN_USER" make -C "$HERE" -j4
sudo -u "$RUN_USER" make -C "$HERE" test
fi

echo "==> Installing to /opt/xowave"
install -d /opt/xowave/presets /opt/xowave/wavetables /opt/xowave/fonts
install -m755 "$HERE/xowave" /opt/xowave/xowave
cp "$HERE/VERSION" /opt/xowave/VERSION 2>/dev/null || true
ln -sf /opt/xowave/xowave /usr/local/bin/xowave
cp -n "$HERE"/presets/*.xowave /opt/xowave/presets/          # never overwrite saved edits
cp "$HERE"/wavetables/*.wav /opt/xowave/wavetables/ 2>/dev/null || true
cp "$HERE"/fonts/* /opt/xowave/fonts/
install -d -o "$RUN_USER" /var/lib/xowave                     # touchscreen settings + last preset
chown -R "$RUN_USER" /opt/xowave/presets /opt/xowave/wavetables   # (USB import writes here)
if [[ -f /etc/xowave.conf ]]; then
    echo "    keeping existing /etc/xowave.conf (new default saved as /etc/xowave.conf.new)"
    install -m644 "$HERE/xowave.conf" /etc/xowave.conf.new
else
    install -m644 "$HERE/xowave.conf" /etc/xowave.conf
fi
install -m755 "$HERE/install/xowave-bt" /usr/local/bin/xowave-bt
install -m755 "$HERE/install/xowave-bt-keeper" /usr/local/bin/xowave-bt-keeper
install -m755 "$HERE/install/xowave-update" /usr/local/bin/xowave-update
install -m644 "$HERE/install/xowave-update.service" /etc/systemd/system/xowave-update.service
touch /var/lib/xowave/update.log /var/lib/xowave/update.log.err 2>/dev/null; chmod 644 /var/lib/xowave/update.log* 2>/dev/null || true
usermod -aG audio,bluetooth,input,video "$RUN_USER"

# Let the touchscreen run the Bluetooth helper, reboot and power off.
echo "$RUN_USER ALL=(root) NOPASSWD: /usr/local/bin/xowave-bt, /usr/sbin/reboot, /usr/sbin/poweroff, /usr/local/bin/xowave-update check, /usr/bin/systemctl start --no-block xowave-update.service" > /etc/sudoers.d/xowave
chmod 440 /etc/sudoers.d/xowave
visudo -cf /etc/sudoers.d/xowave >/dev/null || { echo "!! sudoers rule invalid, removing"; rm -f /etc/sudoers.d/xowave; }

echo "==> Boot config"
CFG=/boot/firmware/config.txt
[[ -f $CFG ]] || CFG=/boot/config.txt
grep -q '^dtparam=audio=on' "$CFG" || echo 'dtparam=audio=on' >> "$CFG"       # 3.5 mm jack
sed -i 's/^dtoverlay=disable-bt/#dtoverlay=disable-bt/' "$CFG"                  # keep Bluetooth on
if [[ $NO_SCREEN == 0 ]]; then
    # 3.5" ILI9486 SPI TFT + ADS7846 resistive touch (Waveshare/MHS "3.5inch RPi LCD (A)" type).
    grep -q '^dtparam=spi=on' "$CFG" || echo 'dtparam=spi=on' >> "$CFG"
    grep -q '^dtoverlay=piscreen' "$CFG" || echo 'dtoverlay=piscreen,speed=16000000,rotate=270' >> "$CFG"
fi
if [[ $NO_WIFI == 1 ]]; then
    grep -q '^dtoverlay=disable-wifi' "$CFG" || echo 'dtoverlay=disable-wifi' >> "$CFG"
fi

echo "==> USB sticks (wavetable + preset import)"
sed -e "s/@UID@/$(id -u "$RUN_USER")/" -e "s/@GID@/$(id -g "$RUN_USER")/" "$HERE/install/99-xowave-usb.rules" > /etc/udev/rules.d/99-xowave-usb.rules
udevadm control --reload-rules 2>/dev/null || true

echo "==> Ethernet (XOXO Studio link)"
if [[ -d /etc/NetworkManager ]]; then
    install -d /etc/NetworkManager/conf.d
    install -m644 "$HERE/install/50-xowave-ethernet-link.conf" /etc/NetworkManager/conf.d/
    nmcli general reload conf 2>/dev/null || true
fi

echo "==> Services"
sed "s/@USER@/$RUN_USER/" "$HERE/install/xowave.service" > /etc/systemd/system/xowave.service
install -m644 "$HERE/install/xowave-bt-keeper.service" /etc/systemd/system/xowave-bt-keeper.service
if [[ -x /usr/bin/bluealsa ]]; then
    # Only the profile we need: the Pi sends audio to a speaker (A2DP source).
    install -d /etc/systemd/system/bluealsa.service.d
    cat > /etc/systemd/system/bluealsa.service.d/xowave.conf <<'EOF'
[Service]
ExecStart=
ExecStart=/usr/bin/bluealsa -S -p a2dp-source
EOF
fi
systemctl daemon-reload
systemctl enable bluetooth
rfkill unblock bluetooth || true   # Raspberry Pi OS ships with Bluetooth soft-blocked
# Power the adapter on by itself whenever it appears or is unblocked.
sed -i -E 's/^#?AutoEnable=.*/AutoEnable=true/' /etc/bluetooth/main.conf
[[ -x /usr/bin/bluealsa ]] && systemctl enable --now bluealsa || true
# bluealsa-aplay plays *incoming* Bluetooth audio and can grab the jack; not needed.
systemctl disable --now bluealsa-aplay 2>/dev/null || true
systemctl enable --now xowave-bt-keeper
systemctl enable xowave
systemctl restart xowave

[[ $UPDATE == 1 ]] && exit 0
cat <<EOF

XO-Wave installed and running.
  Logs:             journalctl -u xowave -f
  Settings:         sudo nano /etc/xowave.conf   then   sudo systemctl restart xowave
  Bluetooth:        sudo xowave-bt scan
                    sudo xowave-bt pair <MAC> audio   (speaker / headphones)
                    sudo xowave-bt pair <MAC> midi    (BLE MIDI controller)
  CPU check:        xowave -c /etc/xowave.conf --bench
  Touchscreen:      calibrates itself on first start; redo it in SETTINGS > DISPLAY

Reboot once so the audio/boot settings and group membership take effect:
  sudo reboot
EOF
