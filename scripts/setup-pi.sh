#!/usr/bin/env bash
# Prepares Raspberry Pi OS Lite (Bookworm or Trixie) on a Pi 4B for wallet-fw:
# build manual Step 1 (I2C) and Step 2 (USB serial gadget or UART fallback),
# then builds, tests and installs the firmware as a systemd service.
#
#   sudo scripts/setup-pi.sh usb       # USB-C gadget serial, /dev/ttyGS0 (default)
#   sudo scripts/setup-pi.sh uart      # Pico debugprobe fallback, /dev/serial0
#   sudo scripts/setup-pi.sh lockdown  # demo only: disable Wi-Fi and Bluetooth
#
# Run it while the Pi still has network access (packages and the pinned
# third-party sources are downloaded once). A reboot is needed afterwards.
set -euo pipefail

MODE="${1:-usb}"
SRC_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BOOT_CONFIG=/boot/firmware/config.txt
[ -f "$BOOT_CONFIG" ] || BOOT_CONFIG=/boot/config.txt

die() { echo "error: $*" >&2; exit 1; }
[ "$(id -u)" -eq 0 ] || die "run with sudo"
[ -f "$BOOT_CONFIG" ] || die "cannot find config.txt"

# Appends a setting under an [all] section unless the exact line exists.
boot_setting() {
  grep -qxF "$1" "$BOOT_CONFIG" && return 0
  printf '\n[all]\n%s\n' "$1" >> "$BOOT_CONFIG"
  echo "config.txt: added $1"
}

module_line() {
  grep -qxF "$1" /etc/modules || { echo "$1" >> /etc/modules; echo "/etc/modules: added $1"; }
}

if [ "$MODE" = "lockdown" ]; then
  echo "This disables Wi-Fi and Bluetooth after the next reboot: SSH over Wi-Fi will stop."
  read -r -p "Type LOCKDOWN to continue: " answer
  [ "$answer" = "LOCKDOWN" ] || die "aborted"
  boot_setting "dtoverlay=disable-wifi"
  boot_setting "dtoverlay=disable-bt"
  echo "Done. Reboot to apply; remove those two lines from $BOOT_CONFIG to undo."
  exit 0
fi
[ "$MODE" = "usb" ] || [ "$MODE" = "uart" ] || die "mode must be usb, uart or lockdown"

echo "== Packages"
apt-get update
apt-get install -y build-essential cmake pkg-config libsodium-dev libsecp256k1-dev libgpiod-dev \
  nlohmann-json3-dev i2c-tools gpiod python3-serial python3-nacl

echo "== Step 1: I2C for the Grove LCD"
raspi-config nonint do_i2c 0

echo "== Step 2: serial link ($MODE)"
if [ "$MODE" = "usb" ]; then
  boot_setting "dtoverlay=dwc2"
  module_line "dwc2"
  module_line "g_serial"
  # The firmware owns the gadget port; never put a login prompt on it.
  systemctl mask serial-getty@ttyGS0.service >/dev/null 2>&1 || true
  SERIAL_DEVICE=/dev/ttyGS0
else
  # Enable the UART hardware, disable the serial login console.
  raspi-config nonint do_serial_hw 0 2>/dev/null && raspi-config nonint do_serial_cons 1 \
    || raspi-config nonint do_serial 2
  SERIAL_DEVICE=/dev/serial0
fi

echo "== Service account"
id walletfw >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin walletfw
usermod -a -G i2c,gpio,dialout walletfw

echo "== Build, test, install"
# Build as the user who owns the checkout (not root), so later
# `git pull && cmake --build build` works without sudo.
OWNER="$(stat -c %U "$SRC_DIR")"
as_owner() { if [ "$OWNER" = root ]; then "$@"; else sudo -u "$OWNER" "$@"; fi; }
chown -R "$OWNER": "$SRC_DIR"  # repair trees left root-owned by older versions
as_owner cmake -S "$SRC_DIR" -B "$SRC_DIR/build" -DCMAKE_BUILD_TYPE=Release
as_owner cmake --build "$SRC_DIR/build" -j"$(nproc)"
(cd "$SRC_DIR/build" && as_owner ctest --output-on-failure)
cmake --install "$SRC_DIR/build" --prefix /usr/local
install -D -m 0644 "$SRC_DIR/README.md" /usr/local/share/doc/wallet-fw/README.md

install -d -m 0755 /etc/wallet-fw
if [ ! -f /etc/wallet-fw/config.json ]; then
  sed "s|\"serial_device\": \"/dev/ttyGS0\"|\"serial_device\": \"$SERIAL_DEVICE\"|" \
    "$SRC_DIR/deploy/config.example.json" > /etc/wallet-fw/config.json
  echo "Installed /etc/wallet-fw/config.json (serial_device $SERIAL_DEVICE)"
else
  echo "Keeping existing /etc/wallet-fw/config.json"
fi
install -m 0644 "$SRC_DIR/deploy/wallet-fw.service" /etc/systemd/system/wallet-fw.service
systemctl daemon-reload
systemctl enable wallet-fw.service

cat <<EOF

Done. Reboot now:  sudo reboot
After the reboot:
  i2cdetect -y 1                    # expect 3e and 62 (or 30)
  systemctl status wallet-fw        # should be "active (running)"
  journalctl -u wallet-fw -f        # firmware log
EOF
