# Firmware architecture

## Shape

One single-threaded C++17 process (`wallet-fw`) runs under systemd on Pi OS
Lite. A `poll()` loop wakes every 5 ms. In each iteration it:

1. reads the serial port and splits it into JSON lines;
2. samples the three buttons and debounces them;
3. hands lines and button events to the **Controller**;
4. lets the Controller run its timers (prompt timeouts, notices, buzzer patterns, display retry);
5. pings the systemd watchdog.

Nothing blocks except the deliberate Argon2id PIN check (~1 s) and the 2 ms
LCD clear. The firmware does not use threads, interrupts or dynamic allocation
in timing-sensitive paths.

```
 USB-C gadget /dev/ttyGS0          I2C-1                 gpiochip0
 (or UART /dev/serial0)       0x3e text, 0x62/0x30   GPIO17/27/22 in, GPIO23 out
          |                          |                         |
   hal::TtySerial              hal::GroveLcd             hal::GpiodIo
   (PtySerial in sim)          (SimDisplay in sim)       (SimButtons/SimBuzzer in sim)
          |                          |                         |
  device::LineFramer         ui::ScreenRenderer        ui::ButtonInput   ui::BuzzerController
          \__________________________|_________________________/________________/
                                     |
                           device::Controller
            dispatch · setup/words · PIN · review/approve · secure channel
                 |                 |                   |
         solana::parseMessage   policy::assess     storage::Keystore / KnownRecipients
         solana::analyze        (device risk rule)  crypto:: (BIP-39, SLIP-10, ed25519,
                                                     HKDF, X25519/ChaCha20-Poly1305)
```

Everything below the HAL line is plain logic with an injected `Clock`. The unit
tests drive the real `Controller` with a fake display, fake buzzer and a manual
clock at the same 5 ms cadence as the main loop. The `--sim` build runs the
same binary on any Linux box with a PTY in place of the USB port.

## Modules

| Path | Responsibility |
|---|---|
| `src/main.cpp` | CLI, config load, HAL selection (hardware or `--sim`), single-instance lock, event loop, watchdog, clean shutdown |
| `src/device/controller*.cpp` | Protocol dispatch and every on-device flow. `controller.cpp`: transport, sessions, screens. `controller_commands.cpp`: immediate commands. `controller_setup.cpp`: words and PIN. `controller_sign.cpp`: review and signing |
| `src/device/line_framer.cpp` | Newline framing with an 8 KiB line cap |
| `src/solana/message.cpp` | Legacy/v0 message parser with Solana's sanitize rules: canonical compact-u16, header consistency, unique keys, index bounds, no trailing bytes |
| `src/solana/analyze.cpp` | Recognises System transfer, ComputeBudget and Memo. Everything else is "unknown". Computes the priority-fee upper bound |
| `src/policy/risk.cpp` | Device risk rule; the app can only raise the level |
| `src/crypto/keys.cpp` | BIP-39 (trezor-crypto), SLIP-0010 ed25519 (libsodium HMAC-SHA512), key pair, sign/verify |
| `src/crypto/secure_channel.cpp`, `hkdf.cpp` | Step 12 encrypted channel; RFC 5869 HKDF |
| `src/storage/keystore.cpp` | `wallet.json` (0600, atomic write); optional Argon2id + secretbox PIN encryption |
| `src/storage/recipients.cpp` | Recipients the device has signed for |
| `src/ui/*` | Debounce and hold detection, screen diffing and I2C error recovery, buzzer patterns |
| `src/hal/linux/*` | i2c-dev, Grove LCD, libgpiod v1/v2, tty/pty serial, `sd_notify` |
| `src/hal/sim/*` | Console LCD + JSON frame log, scripted buttons, logging buzzer |

## Controller states

```
Idle ──setup──▶ Words(intro→01..12) ──hold R on 12──▶ [Pin Set → Pin Confirm] ──▶ save ──▶ Notice ──▶ Idle
Idle ──show_words──▶ [Pin Unlock] ──▶ Words ──hold R on 12──▶ Notice
Idle ──sign_tx──▶ (validate, parse, assess) ──▶ [Pin Unlock] ──▶ Review pages ──hold R──▶ sign ──▶ Notice
                                                                    └──L──▶ reject ──▶ Notice
any prompt ──cancel / timeout / link reset──▶ Notice ──▶ Idle
```

Only one prompt is pending at a time. While a prompt is pending, `ping`,
`get_pubkey`, `confirm_backup` and `cancel` are still answered, and another
prompting command gets `busy`.

## Safety rules the code enforces

- **Nothing is signed without a 2 s right-button hold.** The hold must start
  after the prompt appeared. In setup it must start on word 12. PIN entry uses
  the right button for "+1", and the review resets the start time afterwards,
  so a held button can never carry over.
- **The device decides what is shown.** The screen is built from the message
  bytes, never from text sent by the app. The app's `reason` appears only on
  the risk page.
- **Warnings must be read.** Before approval, these pages must be seen:
  - the `RISK:` page, for anything yellow or red;
  - the full address, for a first-time recipient (4+4 characters are easy to forge);
  - each transfer summary, memo, fee and unknown-instruction page.

  "Seen" means it actually reached the LCD. An I2C failure resets this and
  cancels any hold in progress.
- **Memo text can't pose as firmware text.** Every memo page carries a
  `MEMO i/n` top line, and a cut-off memo is marked `(CUT)`.
- **Device findings win.** The app's `reason` never replaces a device flag.
  Blind-signed transactions never make a recipient "known".
- **Amounts are never truncated.** A long amount moves to its own line or lines.
- **No display, no prompt.** If the LCD stops answering on I2C, prompts are
  refused with `display_unavailable` until it recovers. The renderer retries
  once a second.
- **Unknown means red.** Unrecognised programs, non-transfer System
  instructions, transfers not from this wallet, lookup-table accounts and
  duplicate ComputeBudget instructions all show `UNKNOWN TX` in red. They are
  unsignable unless `allow_blind_signing` is turned on.

## Decisions

| # | Decision | Why |
|---|---|---|
| D1 | Solana (ed25519) only, on devnet; no RSA or MAC-address identity | The build manual drops those from the original plan. The public key is the device ID. |
| D2 | Store the 16-byte BIP-39 entropy, not the 64-byte seed | It regenerates both the words (for `show_words`) and the seed. On load, the derived key is checked against the stored public key. |
| D3 | SLIP-0010 implemented on libsodium; trezor-crypto only for BIP-39 | That takes ~40 lines and passes the SLIP-0010 spec vectors. It avoids trezor's large bip32/ecdsa dependency tree. Phantom compatibility is checked against `ed25519-hd-key` + web3.js. |
| D4 | trezor's BIP-39 cache disabled (`USE_BIP39_CACHE=0`); trezor's PRNG never used | The cache would keep mnemonics in static memory. Entropy comes from libsodium `randombytes_buf`. |
| D5 | HKDF implemented on HMAC-SHA256 | Pi OS Bookworm ships libsodium 1.0.18, which has no `crypto_kdf_hkdf_sha256`. Checked against RFC 5869 vectors. |
| D6 | `setup` saves the wallet when the user finishes the words, as Step 4 says. Backup verification is a separate, optional `confirm_backup` | An app that implements only the four commands still works. The Step 8 "type it back / back button" flow is available through `confirm_backup` and `show_words`. |
| D7 | Extensions `show_words`, `confirm_backup`, `cancel`, `handshake`, and an `id` echo | Needed for Step 8's retry/back flow, for aborting prompts when the app closes, and for Step 12. The four required commands are unchanged. |
| D8 | Missing `risk` is treated as `medium` | Same as the Step 10 rule "if the model call fails, the level is medium". |
| D9 | Recipients are aggregated per transaction before the over-limit check | A large send can't be split into several small transfers. |
| D10 | Third-party code is fetched at configure time, pinned by SHA-256, not committed | Keeps every commit small (≤ 400 lines) and makes upstream changes fail loudly. On the Pi, the first configure needs network. |
| D11 | Single-threaded poll loop at 5 ms | Deterministic, no locking. 5 ms sampling is well inside the 30 ms debounce. |
| D12 | PIN and encrypted channel are present but off by default | Step 12 says "only after Steps 1–11". Config flags turn them on once the app supports them. |
| D13 | The wrong-PIN counter is stored with the wallet. After `pin_max_attempts` (5) the wallet refuses to unlock, but is not deleted. A PIN-protected wallet relocks after `auto_lock_s` idle | Restarting can't reset the count. Not self-wiping avoids bricking a demo. The 12 words recover the funds in any BIP-39 wallet; the firmware itself has no restore-from-words (out of scope). |
| D14 | Independent review (one subagent, read-only) before release | It found 11 issues and no critical ones; all were fixed with regression tests (see the traceability doc). |

## Known limitations

- A Raspberry Pi has no secure element. The keystore sits on the SD card, and
  a 4-digit PIN (Argon2id "interactive" cost) can be brute-forced offline in
  minutes by anyone holding the card. The PIN mostly guards against casual use
  of an unattended device.
- The serial link does not authenticate the app. Any process on the laptop
  that opens the port can send requests (including `handshake`). Approval
  still always needs the physical button.
  This is a prototype of the signing flow, as the build manual's demo script states.
- The device can't tell a recipient's account age or balance share. Those parts
  of the risk check stay in the app or backend (Gemini). The device's own rule
  uses only what it can verify from the message.
- Address lookup tables (v0) can't be resolved on the device, so any
  instruction that touches a looked-up account is treated as unknown.
- The keystore JSON is parsed with nlohmann/json, which may leave short-lived
  copies of the hex entropy in freed heap memory. Our own buffers are wiped.
