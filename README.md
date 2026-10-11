# Aurora firmware (wallet-fw)

**Aurora: your gateway to finances.** Aurora is a hardware wallet built on a
Raspberry Pi 4B, and this repository is its firmware. The Pi:
- holds the keys and the recovery phrase;
- decodes every transaction itself, showing the amount and recipient on a 16x2 LCD;
- lights the backlight green, yellow or red by risk;
- signs only after a 2-second hold of the right button;
- stays encrypted with the password chosen in the app, and locks after a restart or 5 idle minutes;
- has its own menu (tap right on the home screen): wallets with the balances the app sent, disconnect the app, erase.

The [Aurora desktop app](https://github.com/blockchain-wallet-hackathon/wallet-software)
shows your accounts and prepares transactions. It can't move funds without the
device. Signing targets Solana **devnet**; Solana mainnet and Bitcoin are
read-only.

- Serial protocol v2, shared with the app: [docs/firmware/protocol-contract.md](docs/firmware/protocol-contract.md)
- Design, safety rules and decisions: [docs/firmware/architecture.md](docs/firmware/architecture.md)
- Requirements and test evidence: [docs/firmware/requirements-traceability.md](docs/firmware/requirements-traceability.md)

## Hardware (build manual Step 1)

| Part | Connection |
|---|---|
| Grove LCD RGB Backlight | GND → pin 6, VCC → pin 2 (5 V), SDA → pin 3 (GPIO2), SCL → pin 5 (GPIO3) |
| Button left (back / reject) | signal → GPIO17, VCC → pin 1 (**3.3 V**), GND → any ground |
| Button right (tap: next, hold 2 s: confirm) | signal → GPIO22, 3.3 V, GND |
| Optional middle button (next) | signal → GPIO27; set `"gpio_middle": 27` |
| Grove Buzzer | signal → GPIO23, 3.3 V, GND |
| Laptop link | Pi USB-C → powered hub → laptop (USB gadget serial) |

> **Never power the buttons from 5 V.** Pi GPIO pins are not 5 V tolerant.

**Two buttons are enough.** Left goes back or rejects. A quick tap of right
shows the next page, word or menu item; holding right for 2 s approves. On a
Grove Base HAT, find which GPIO each button is on with the button finder:

```sh
sudo systemctl stop wallet-fw
sudo wallet-fw --hw-test     # press left, then right; it prints the config lines
```

Pin numbers, I2C addresses, timings and the risk limit can all be changed in
`/etc/wallet-fw/config.json` ([example](deploy/config.example.json)).

## Install on the Pi

On a fresh Pi OS Lite (Bookworm or Trixie) with network access:

```sh
git clone <this repo> wallet-firmware && cd wallet-firmware
sudo scripts/setup-pi.sh usb      # or: uart (Pico debugprobe fallback)
sudo reboot
```

The script:

- installs the build dependencies (libsodium, libsecp256k1, libgpiod, nlohmann-json, cmake);
- enables I2C and adds `dtoverlay=dwc2` plus the `dwc2` and `g_serial` modules (Step 2);
- masks a login getty on `/dev/ttyGS0`;
- creates the `walletfw` service user;
- builds, runs the unit tests, and installs `/usr/local/bin/wallet-fw` with a systemd unit.

After the reboot:

```sh
i2cdetect -y 1                 # expect 3e and 62 (30 on newer LCD boards)
systemctl status wallet-fw     # active (running); the LCD shows "AURORA / SET UP IN APP"
journalctl -u wallet-fw -f     # live log
```

For the demo only, `sudo scripts/setup-pi.sh lockdown` adds `disable-wifi` and
`disable-bt` (Step 12). This removes SSH over Wi-Fi.

### Automatic updates from GitHub

Once, on the Pi (it needs internet, e.g. Windows Internet Connection Sharing):

```sh
cd ~/wallet-firmware && git pull
sudo scripts/install-updater.sh          # shows a deploy key to add on GitHub
# or: sudo scripts/install-updater.sh --token   (a read-only fine-grained token)
```

From then on the Pi checks the `firmware` branch every 15 minutes. When there
is a new commit it:

1. builds it and runs the unit tests (at low priority);
2. installs it and restarts `wallet-fw`, which interrupts any prompt on the
   device and locks a password wallet;
3. puts the previous binary back if the service won't stay running.

A commit that fails to build or test is skipped until a newer one arrives. If
the branch history is rewritten, nothing is installed. Watch it with
`journalctl -u aurora-update -f`, or check now with
`sudo systemctl start aurora-update`.

**Trust:** whoever can push to `firmware` can change what runs on Aurora,
including code that sees the password and keys. To accept only signed
commits, put the signers' SSH keys in `/etc/aurora-update/allowed_signers`
(`git` allowed-signers format). Turn updates off with
`sudo systemctl disable --now aurora-update.timer`.

### Manual build

```sh
sudo apt install build-essential cmake pkg-config libsodium-dev libsecp256k1-dev libgpiod-dev nlohmann-json3-dev
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4 && (cd build && ctest --output-on-failure)
sudo ./build/wallet-fw --config deploy/config.example.json --data-dir /tmp/wfw --verbose
```

The first configure downloads trezor-crypto (BIP-39), nlohmann/json (unless the
distro package is installed) and the BIP-39 test vectors. They go into
`third_party/`, pinned to exact versions and checked by SHA-256.

## Try it without a Pi (simulator)

Any Linux machine, including WSL, works. Build with `-DWFW_HARDWARE=OFF` if
libgpiod is missing:

```sh
cmake -S . -B build -DWFW_HARDWARE=OFF && cmake --build build -j
./build/wallet-fw --sim
```

The LCD is drawn in the terminal and the serial port appears at
`./wallet-fw.pty`. Type button commands on stdin:

| Command | Effect |
|---|---|
| `l` / `r` | Click left / tap right ("next") |
| `m` | Click the optional middle button (also "next") |
| `h` | Hold right for 2.5 s |
| `hold r 3000` | Hold a button for a given time in ms |

In another terminal:

```sh
pip install pyserial pynacl
python3 tools/wallet_cli.py --port ./wallet-fw.pty ping     # verified identity + wallets
python3 tools/wallet_cli.py --port ./wallet-fw.pty setup    # r ×12, h, then answer 3 words
python3 tools/wallet_cli.py --port ./wallet-fw.pty pair     # check the code, then h
python3 tools/wallet_cli.py --port ./wallet-fw.pty send --to <ADDR> --sol 0.5 --offline
```

Setup ends with a backup check on the device: Aurora asks for 3 of the words
(`WORD #07? R:NEXT`). Press `r` until the right word shows, then `h`. The
recovery phrase never leaves Aurora.

### With the Aurora desktop app

Over USB, the app finds Aurora as a serial port by itself. Without a cable,
use the development TCP transport. It works only with debug builds of the app,
and anyone who can reach the port can send requests; approval still needs the
buttons.

```sh
./build/wallet-fw --sim --tcp-listen 127.0.0.1:7777          # or on a Pi: 0.0.0.0:7777
# in wallet-software (PowerShell: $env:WALLET_SERIAL_PORT="tcp://127.0.0.1:7777"):
WALLET_SERIAL_PORT=tcp://127.0.0.1:7777 npm run desktop
```

`wallet_cli.py` works against the real device too: `--port COM5` on Windows,
`/dev/tty.usbmodem*` on macOS. Without `--offline`, `send` fetches a devnet
blockhash and submits the signed transfer (Step 6). `airdrop 1` funds the
wallet, though the public faucet is often rate-limited (use faucet.solana.com).
`--really-to <OTHER>` rehearses the "compromised app" demo toggle.

## Receive addresses and payment pages

Aurora's Receive screen requests a new device-derived address. Bitcoin uses
BIP-84 and Solana uses hardened SLIP-0010 children. Per-chain counters survive
restarts; Solana mainnet and devnet share the counter. The signed response binds
the address to the paired device, wallet, derivation path and host challenge.
Existing addresses remain valid. Generated Solana addresses can sign devnet
transfers with the same screen review and physical confirmation as index 0.

In the app repository, run `npm run site` to serve local payment pages on
`http://127.0.0.1:4173`. Creating a payment page requests a fresh address first.
The standalone checkout demo is `/pay/demo?coin=BTC` or `/pay/demo?coin=SOL`;
it uses non-payable fixtures. See the app README for desktop and site builds.

## Tests

```sh
./build/wfw_tests
python3 tests/integration/test_sim.py build/wallet-fw     # 7 end-to-end tests, PTY + TCP
cmake -S . -B build-asan -DWFW_SANITIZE=ON                # ASan + UBSan build
```

The unit tests cover:

- official BIP-39 and SLIP-0010 vectors and RFC 5869 HKDF;
- addresses, signatures and messages generated by `@solana/web3.js`
  (`tests/tools/gen_vectors.js`), checked byte for byte;
- parser fuzz cases;
- every Controller flow: setup with the on-device backup check, pairing,
  signing, the risk colours, the password and PIN, the device menu, erase,
  and the encrypted channel;
- the protocol v2 envelope and signed identity, as the desktop app verifies them.

## Layout

```
src/main.cpp            event loop, CLI
src/device/             protocol + on-device flows (Controller)
src/solana/             message parser, instruction recognizer, LCD formatting
src/policy/             device risk rule
src/crypto/             BIP-39/SLIP-10 keys, ed25519, HKDF, secure channel
src/storage/            keystore (optional password/PIN), known recipients
src/ui/                 debounce/hold, screen renderer, buzzer
src/hal/linux|sim/      I2C LCD, libgpiod, serial, systemd; simulator
tools/                  Python reference host + CLI
deploy/ scripts/        systemd unit, example config, Pi setup
tests/                  unit tests, simulator integration test, vector generator
```

## Troubleshooting

| Symptom | Fix |
|---|---|
| Buttons do nothing | Run the button finder (`--hw-test`) and put its `gpio_left` / `gpio_right` in `/etc/wallet-fw/config.json`. |
| Nothing in `i2cdetect` | Check the wiring (SDA pin 3, SCL pin 5, 5 V on pin 2), not the code. |
| LCD text works but no colour | The backlight chip is at 0x30 or 0x62; set `backlight_address` explicitly. |
| Pi reboots when the backlight turns on | Lower `backlight_brightness` or use a stronger hub. |
| No serial port on the laptop | `lsmod | grep g_serial` on the Pi, and check the cable carries data. Try `dtoverlay=dwc2,dr_mode=peripheral`. After 5 PM Saturday, switch to `setup-pi.sh uart` with the Pico. |
| `display_unavailable` from `sign_tx` | The LCD stopped answering on I2C; the firmware retries every second. |
| Need to start over | `sudo systemctl stop wallet-fw && sudo -u walletfw wallet-fw --wipe --data-dir /var/lib/wallet-fw` |

## Security note

A Pi has no secure element, so this is a prototype of the signing flow, not a
safe place for real funds. The keystore lives on the SD card. The app password
(or the optional PIN) encrypts it with Argon2id + XSalsa20-Poly1305, but a weak
one can be brute-forced by anyone holding the card. The password crosses the
cable in plaintext unless the encrypted channel is on. Use devnet only.
