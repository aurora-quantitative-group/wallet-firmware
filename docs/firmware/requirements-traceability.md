# Requirements traceability and verification

Source: *Hackathon Plans.pdf*, the "Build Manual" section (Steps 1–13). The
earlier "TLDR" notes are superseded by the manual where they conflict (RSA, MAC
identity, second OS build: "Not building").

Status key:

| Status | Meaning |
|---|---|
| **Verified** | An automated test exercises it and passes |
| **Sim** | Verified in the simulator / unit tests only; needs a check on the real Pi |
| **Impl** | Implemented, not testable here |
| **Blocked** | Needs something outside this environment |

## Firmware requirements

| Req | Requirement (manual step) | Implementation | Verification | Status |
|---|---|---|---|---|
| R1 | LCD on I2C-1: text 0x3e, backlight 0x62 or 0x30 (S1) | `hal/linux/grove_lcd.cpp` probes 0x62 then 0x30; `backlight_address` override | Compiles. No Pi or LCD here | Impl |
| R2 | Buttons GPIO17/27/22, active high, 3.3 V (S1, S3) | `hal/linux/gpiod_io.cpp`, pull-down bias, libgpiod 1.6 and 2.x | Compiles against both APIs | Impl |
| R3 | Buzzer on GPIO23 (S1) | `GpiodIo::set` | Compiles | Impl |
| R4 | Enable I2C (S1) | `scripts/setup-pi.sh` (`raspi-config nonint do_i2c 0`) | `bash -n` | Impl |
| R5 | USB gadget serial: `dtoverlay=dwc2`, `dwc2` + `g_serial`, `/dev/ttyGS0` (S2) | `setup-pi.sh usb`; `TtySerial` raw mode, no echo, `TIOCEXCL`; reconnect loop | PTY path exercised end to end (integration test) | Sim |
| R6 | Pico UART fallback, `/dev/serial0` (S2) | `setup-pi.sh uart`; `serial_device` config; baud table | Same code path as R5 | Impl |
| R7 | LCD functions clear, print(row, text), setRGB (S3) | `hal::Display` interface + `GroveLcd` (Seeed command set) | Fake display in unit tests | Sim |
| R8 | 30 ms software debounce (S3) | `ui/button_input.cpp` | `button_bounce_shorter_than_debounce_is_ignored`, `button_press_and_release_after_debounce` | Verified |
| R9 | Left = back/reject, middle = next/scroll, right = confirm (S3) | `controller_setup.cpp`, `controller_sign.cpp` | `ctrl_setup_*`, `sign_*` tests | Verified |
| R10 | Approval needs a 2 s right-button hold (S3) | `ButtonInput` Hold event; hold must start after the prompt | `button_hold_fires_once_after_two_seconds`, `sign_hold_that_began_before_the_prompt_never_approves`, `ctrl_hold_must_start_on_last_word` | Verified |
| R11 | Short beep on a signing request; long tone on high risk (S3) | `BuzzerController::beep/longTone` | `buzzer_pattern_plays_and_stops`, `sign_compromised_app_is_red_and_rejected`, integration test 3 | Verified |
| R12 | One JSON object per line (S4) | `device/line_framer.cpp`, 8 KiB cap | `framer_*`, integration test 5 | Verified |
| R13 | `ping` → pong + version (S4) | `handlePing` | `ctrl_ping_and_protocol_errors`, integration test 1 | Verified |
| R14 | `get_pubkey` → base58 key or `not_initialized` (S4) | `handleGetPubkey` | same | Verified |
| R15 | `setup` generates the seed, shows the words, returns the pubkey when done (S4, S5) | `handleSetup`, `showWord`, `createWallet` | `ctrl_setup_shows_words_and_creates_wallet`, integration test 1 | Verified |
| R16 | `sign_tx`: base64 message + risk → base64 signature or `rejected` (S4) | `controller_sign.cpp` | `sign_*` tests, integration tests 2–4 | Verified |
| R17 | 128-bit entropy from libsodium `randombytes_buf` (S5) | `Services.randomBytes` default | Code review (tests inject fixed entropy) | Impl |
| R18 | 12-word BIP-39 via trezor-crypto (S5) | `crypto/keys.cpp` | All official English vectors (`bip39_official_vectors`) | Verified |
| R19 | One word per screen "03/12 ocean"; middle next, left back, hold right on 12 (S5) | `showWord`, `onWordsButton` | `ctrl_setup_shows_words_and_creates_wallet` | Verified |
| R20 | SLIP-0010 ed25519 at m/44'/501'/0'/0', matches Phantom (S5) | `slip10Derive`, `deriveSolanaKeyPair` | SLIP-0010 spec vector 1; address equals bip39 + ed25519-hd-key + web3.js | Verified |
| R21 | Seed stored on disk; pubkey = device ID; no key → setup (S5) | `storage/keystore.cpp` (entropy, 0600, atomic, integrity check) | `keystore_*` tests, integration restart test | Verified |
| R22 | Sign message bytes with `crypto_sign_detached`, 64-byte signature (S6) | `crypto::signDetached` | Signature identical to web3.js `tx.sign()` (`sign_matches_web3js_and_verifies`); PyNaCl verify | Verified |
| R23 | Devnet transfer confirmed in Explorer (S6 "done when") | Firmware side complete; `tools/wallet_cli.py send` submits | `simulateTransaction` (sigVerify) on devnet accepted the device signature (`AccountNotFound`; corrupted control gave `SignatureFailure`). Faucet returned 429, so no funded on-chain confirmation | Blocked (faucet) |
| R24 | Parse header, account keys, blockhash, instructions; compact-u16 (S7, Reference) | `solana/message.cpp` (legacy + v0, sanitize rules) | `compact_u16_vectors`, `parse_*`, `rejects_malformed_messages` | Verified |
| R25 | Recognise System transfer (all-zero program ID, u32 2 + u64 lamports LE) (S7) | `solana/analyze.cpp` | `parse_web3js_transfer`, `parse_v0_message` | Verified |
| R26 | Line 1 "SEND 0.50 SOL", line 2 "TO 7xKX..9fQ2"; middle scrolls the full address (S7) | `buildReviewPages` | `sign_middle_scrolls_full_address_then_red_can_be_approved` | Verified |
| R27 | Backlight green / yellow / red by risk (S7) | `colorFor`, `ui::colors` | `sign_*` colour checks, integration tests 2–3 (logged RGB) | Verified |
| R28 | Unknown instruction → "UNKNOWN TX", red (S7) | `analyze` → `unknown`; risk High | `sign_unknown_program_is_red_and_blind_signing_can_be_disabled`, `review_blind_signing_off_by_default_and_never_learns_recipients` | Verified |
| R29 | Device rule: stores every recipient signed for; unknown recipient ≥ yellow, red if over the limit, whatever the app says (S7) | `storage/recipients.cpp`, `policy/risk.cpp` | `risk_*` tests, `sign_new_recipient_is_yellow_then_remembered`, `sign_compromised_app_is_red_and_rejected` | Verified |
| R30 | Demo: the compromised app shows one address but pays another; the device shows the real one, red, buzzer, reject (S8, demo script) | All of the above; `wallet_cli.py --really-to` rehearses it | integration test 3 | Verified |
| R31 | Insert device events on connect / sign / reject (S9) | App/backend work. Replies carry `risk` and `reason` for the event detail | — | n/a (app) |
| R32 | Risk reason ≤ 16 chars shown on the LCD (S10) | Optional `reason` on `sign_tx`, risk page | `sign_missing_risk_defaults_to_medium` | Verified |
| R33 | Model failure → medium (S10) | Missing `risk` → medium | same | Verified |
| R34 | PIN with buttons; Argon2id key; `crypto_secretbox` seed (S12) | `controller_setup.cpp` PIN flow, `Keystore` | `keystore_pin_encrypts_and_unlocks`, `pin_*` tests (incl. counter persistence, auto-lock) | Verified (opt-in) |
| R35 | X25519 + HKDF-SHA256 + ChaCha20-Poly1305; ed25519 signs the handshake (S12) | `crypto/secure_channel.cpp`, `handshake` cmd | RFC 5869 vectors, `channel_*` tests, integration test 4 (Python client) | Verified (opt-in) |
| R36 | Demo config: disable Wi-Fi and BT (S12) | `setup-pi.sh lockdown` | `bash -n` | Impl |
| R37 | Solana Pay: reference key as an extra read-only account; the Pi shows amount and recipient (S13) | Transfer recogniser allows extra accounts | `parse_solana_pay_reference_transfer` (web3.js vector) | Verified |
| R38 | Verify the backup without the phrase leaving the device (Aurora mission §12A) | 3-word on-device check in `setup` / `show_seed`; `confirm_backup` removed | `ctrl_backup_is_checked_on_the_device`, `ctrl_show_seed_finishes_an_unchecked_backup`, integration test 1 (deliberate wrong answer) | Verified |
| R39 | Aurora app compatibility: protocol v2 envelope, signed identity, wallet manifest (SOL + BTC), pairing | `controller_commands.cpp`, `storage/identity.cpp`, `crypto/bitcoin.cpp` | `test_protocol_v2.cpp`; app's Rust `authenticates_with_real_firmware_when_available` (Windows → firmware over TCP); app's `tests/firmware.test.ts` (setup, pair, sign) | Verified (sim) |

## Added for safety and robustness

| Req | Item | Verification |
|---|---|---|
| X1 | Malformed input never crashes: JSON, base64, message bytes, oversized lines | Parser negative tests; ASan + UBSan run of all unit tests is clean |
| X2 | No prompt or signature while the LCD is not responding | `ctrl_display_failure_blocks_prompts` |
| X3 | Prompt timeouts, `cancel`, and abandoning prompts on link reset | `ctrl_setup_cancel_and_timeout`, `sign_busy_cancel_timeout_and_disconnect` |
| X4 | Priority-fee bound and duplicate ComputeBudget detection | `parse_compute_budget_and_memo`, `priority_fee_without_limit_uses_default_units`, `duplicate_compute_budget_is_flagged`, `risk_unknown_instruction_and_fee` |
| X5 | Split transfers summed per recipient before the limit check | `risk_split_transfers_are_summed` |
| X6 | Single instance per data dir; systemd watchdog; restart on failure | Code + unit file (`systemd-analyze verify`) |
| X7 | Third-party sources pinned by SHA-256 | `cmake/ThirdParty.cmake` |

## Verification log (2026-10-10, WSL2 Ubuntu 24.04, GCC 16, libsodium 1.0.18, libgpiod 1.6.3)

| Command | Result |
|---|---|
| `cmake --build build` with `-Wall -Wextra -Wpedantic -Wshadow -Wconversion` | No warnings |
| `build/wfw_tests` | 78 tests, 0 failed |
| `cmake -DWFW_SANITIZE=ON` + `wfw_tests` (ASan + UBSan) | 78 tests, 0 failed, no sanitizer reports |
| `g++ -fsyntax-only -DWFW_GPIOD_V2` against Debian Trixie libgpiod 2.2.1 headers | Clean |
| `cmake -DWFW_HARDWARE=OFF` (simulator-only build) | Clean |
| `python3 tests/integration/test_sim.py build/wallet-fw` | 6 tests OK (~12 s); 10 consecutive runs OK; also OK against the ASan binary |
| `node tests/tools/gen_vectors.js`, diffed against `test_vectors.h` | Identical |
| Devnet `simulateTransaction` with `sigVerify: true` on a firmware-signed transfer | Signature accepted (`AccountNotFound`, unfunded); bit-flipped control: `SignatureFailure` |
| `bash -n scripts/setup-pi.sh`; `systemd-analyze verify deploy/wallet-fw.service` | OK |

## Aurora integration evidence (2026-10-10)

| Check | Result |
|---|---|
| Firmware unit tests (`wfw_tests`) | 86 tests, 0 failed; clean under ASan/UBSan |
| Firmware simulator end-to-end (`tests/integration/test_sim.py`, PTY + TCP) | 7 tests OK |
| App Rust tests on Windows (`cargo test --locked`) | 6 passed, including a live identity check of the firmware simulator over TCP (WSL → Windows) |
| App frontend tests (`npm test`, Windows) | 13 passed, 1 skipped (needs the simulator's button FIFO) |
| App ↔ firmware full flow (`tests/firmware.test.ts`, WSL) | Setup with the on-device check, signed pairing, web3.js transfer signed by the firmware, `verifySignatures()` true |
| App UI tests (`npm run test:ui`, Windows) | 15 passed, 2 failed. Both failures are clipboard checks that also fail on unmodified `main` in this environment. |
| App release build (`cargo build --release`) | OK |

## Independent review

One read-only reviewer subagent audited the firmware. It reported no critical
defects and 11 findings; all are fixed, with regression tests in
`tests/unit/test_controller_review.cpp` unless noted:

| # | Finding | Fix | Test |
|---|---|---|---|
| 1 | Pages counted as seen, and holds accepted, while I2C writes were failing | Seen only after a successful draw; an LCD failure resets seen pages and any hold | `review_display_failure_unsees_pages_and_blocks_hold` |
| 2 | Yellow was signalled only by colour | Risk page required when risk is above low | `sign_new_recipient_is_yellow_then_remembered` |
| 3 | Memo continuation pages unlabelled (could fake "RISK: LOW") | `MEMO i/n` label on every page, `(CUT)` marker | `review_memo_pages_are_always_labelled` |
| 4 | New recipient checked only by 4+4 characters | Full-address pages required for first-time recipients | `sign_new_recipient_is_yellow_then_remembered`, integration test 2 |
| 5 | PIN counter reset on restart | Counter stored in the keystore | `pin_failures_survive_restart` |
| 6 | Unlocked indefinitely | `auto_lock_s` idle relock (default 300 s) | `pin_success_resets_counter_and_auto_lock_relocks` |
| 7 | App reason could replace a device flag | Device flag always wins | `risk_device_flag_beats_app_reason` |
| 8 | Blind-signed transactions made recipients known | Not recorded | `review_blind_signing_off_by_default_and_never_learns_recipients` |
| 9 | Blind signing on by default | Default `false` | same |
| 10 | A failed handshake dropped the working channel | Keys replaced only on success | `failed_handshake_keeps_existing_channel` |
| 11 | Output overflow could split a line; allocation before the bounds check; secrets left in heap; LCD retry stalls sampling | Drop whole lines; check the instruction count first; wipe words and lines; power-on delay once | `rejects_malformed_messages` (count check); others by code review |

Accepted, not fixed (documented in architecture.md):
- the serial link does not authenticate the app;
- a short PIN on an SD card can be brute-forced offline.

## Not verified here (needs the hardware)

These need the real Pi and parts. Run them in this order on the Pi:

1. `i2cdetect -y 1` shows 0x3e and 0x62 (or 0x30). On boot the LCD shows "AURORA / SET UP IN APP".
2. Each button press shows up in `journalctl -u wallet-fw` with `--verbose`.
   Check that bounce doesn't double-step the word screens.
3. The buzzer beeps on `wallet_cli.py send`.
4. The laptop sees the USB gadget serial port; `wallet_cli.py ping` replies.
5. Power: the backlight at full red doesn't brown out the Pi on the chosen hub.
6. `wallet_cli.py send` after funding at faucet.solana.com: the Explorer shows the transaction confirmed (R23).
7. Phantom imports the 12 words and shows the same address as `get_pubkey`.

## External dependencies

| Owner | Needed | Why |
|---|---|---|
| App team | Implement the protocol in `protocol-contract.md` (Rust `serialport` + JSON lines) | Integration |
| App team | Send `serializeMessage()` bytes, not the full transaction | `sign_tx` expects message bytes |
| App team | Done on branch `firmware-integration` of wallet-software: protocol 2, device-side setup and code pairing | Merge and publish |
| Backend | Optional `reason` (≤ 16 chars) alongside `risk` | Shown on the device risk page |
| Team | Fund the device address at faucet.solana.com | R23 on-chain confirmation |
