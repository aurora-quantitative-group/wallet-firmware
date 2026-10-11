# Aurora serial protocol (v2)

This is the contract between the Aurora firmware and the Aurora desktop app
(Svelte + Tauri,
[wallet-software](https://github.com/blockchain-wallet-hackathon/wallet-software),
`src-tauri/src/device.rs`). The firmware implements it in `src/device/`. The
app repository keeps a matching summary in `docs/protocol.md`.
`tools/wfw_host.py` is a working host-side reference in Python.

## Transport

| | |
|---|---|
| Link | USB-C gadget serial; on the laptop it's a normal serial port (`COMx` / `/dev/tty.usbmodem*` / `/dev/ttyACM*`). Fallback: Pico debugprobe UART bridge. **Development only:** `wallet-fw --tcp-listen host:port`, which the app's debug build reaches through `WALLET_SERIAL_PORT=tcp://host:port`. |
| Settings | 115200 8N1, no flow control |
| Framing | One JSON object per line, UTF-8, `\n`-terminated (`\r\n` is accepted). Max 8192 bytes per request; replies stay under 64 KiB. |
| Model | Strict request → reply, matched by `id`. Aurora never sends unsolicited messages. |

```json
{"id": "r1", "command": "<name>", "params": {...}}
{"id": "r1", "result": {...}}
{"id": "r1", "error": "Rejected on Aurora.", "code": "rejected", "detail": "...", "risk": "high"}
```

- `id` is a string or number, echoed back.
- `params` is optional and must be an object.
- `error` is text to show the user. `code` is stable and meant for programs. `detail` is a developer hint. `sign_tx` rejections also carry `risk` and `reason`.
- An error that can't be tied to a request (bad JSON, a line that's too long, a bad encrypted frame) has `"id": null`.

`setup`, `show_seed`, `pair` and `sign_tx` wait for the person at the device.
Meanwhile Aurora still answers `ping`, `get_pubkey` and `cancel`; another
prompt gets `busy`.

## Commands

### `ping {challenge}`: signed identity

Aurora creates a persistent Ed25519 **identity key** on first boot, stored in
`identity.key` and separate from the wallet seed. `device_id` is its base58
public key. The result is `{"payload": "<JSON string>", "signature": "<base64>"}`,
signed over the exact payload bytes.

```json
{"challenge":"…","device_id":"BpdE…","name":"Aurora BpdE","has_seed":true,"protocol":2,
 "firmware":"0.2.0","state":"ready","backup_confirmed":true,"pin_protected":false,
 "busy":false,"display_ok":true,"secure_channel":false,"receive_addresses":true,
 "wallets":[{"id":"sol-0","name":"Solana wallet","chain":"SOL","network":"mainnet","account":0,"address":"HAgk…"},
            {"id":"sol-dev","name":"Devnet wallet","chain":"SOL","network":"devnet","account":0,"address":"HAgk…"},
            {"id":"btc-0","name":"Bitcoin wallet","chain":"BTC","network":"mainnet","account":0,"address":"bc1q…"}]}
```

- **Challenge:** 8–128 characters.
- **`state`:** `uninitialized`, `ready`, `locked` (needs the app password, or the PIN on the device with `pin_enabled`) or `error`.
- **`pin_protected`:** the wallet is encrypted with a password or PIN. When `true` and `state` is `locked`, the app asks for the password and sends `unlock`.
- **Wallets:** SOL comes from SLIP-0010 `m/44'/501'/0'/0'`, the same address on both clusters; only devnet signs. BTC is the BIP-84 `m/84'/0'/0'/0/0` P2WPKH address, for tracking only. It's stored at setup so it's known while locked. Before setup, `wallets` is `[]`.

### `setup {password?}`: new wallet, backup checked on the device

`password` is optional, 8–128 characters, chosen in the app. Aurora encrypts
the wallet with it (Argon2id + secretbox) and locks after a restart, after
`auto_lock_s` idle, or on `lock`. Anything shorter gives `invalid_request`.

1. Aurora draws 128 bits from libsodium and shows the 12 BIP-39 words one per screen (`03/12 ocean`). A tap of right shows the next word and left goes back. Holding right on word 12 finishes.
2. **Backup check:** Aurora asks for 3 random positions (`WORD #07? R:NEXT`). For each, the user picks the right word from 4 candidates (tapping right or left cycles, holding right chooses).
3. A wrong answer goes back to the words. Nothing is saved until all 3 are right.
4. Without a password and with `pin_enabled`, the user then sets a PIN on the device.
5. The wallet is saved, and the result is `{"created":true,"shown":true,"has_seed":true,"backup_confirmed":true,"pubkey":"…","pin_protected":…}`.

The phrase never crosses the serial link. An existing wallet is never replaced
(`already_initialized`).

### `show_seed {}`

This re-shows the words of a wallet whose backup was never checked, then runs
the same check. The result is `{"shown":true,"backup_confirmed":true}`. Once
the backup is confirmed, Aurora refuses (`backup_already_confirmed`).

### `pair {challenge, code}`

`code` is 4–6 digits and the app shows it.

1. Aurora shows `PAIR CODE 4821` / `HOLD R  L:REJECT`. If PIN-locked, it asks for the PIN first.
2. Holding right returns a signed `{"challenge","code","device_id","paired":true}` payload, like `ping`. Left returns `rejected`.

The app verifies the signature, the challenge and the code before saving the
link.

### `receive_address {wallet_id, minimum_index, challenge}`

Requires an initialized, unlocked wallet and no active prompt. `wallet_id` is
one of the signed manifest's IDs. `minimum_index` is an integer from 1 through
2147483647; `challenge` is 8–128 characters. Unsupported wallets and malformed
requests are rejected before allocating an index.

The device returns `{"payload":"<JSON string>","signature":"<base64>"}`,
signed by its persistent identity key over the exact payload bytes:

```json
{"device_id":"…","wallet_id":"btc-0","root_address":"bc1q…","chain":"BTC",
 "network":"mainnet","account":0,"index":1,"path":"m/84'/0'/0'/0/1",
 "address":"bc1q…","challenge":"…"}
```

The app checks the identity signature, challenge, wallet fields, path and index
before retaining the address. The root address and manifest remain unchanged.
The LCD shows the generated address; no recovery phrase or private key leaves
the device.

| Coin | Receive path |
|---|---|
| Bitcoin | BIP-84 `m/84'/0'/0'/0/{index}` |
| Solana | SLIP-0010 `m/44'/501'/0'/{index}'` |

Index 0 is the original wallet. Each chain has a durable next-index counter in
`wallet.json`; both Solana clusters share that counter. Allocation uses the
larger of the device counter and `minimum_index`. The counter is saved before
the reply, so interrupted requests may skip an index but never reuse one.
Counters survive locks and restarts. Erasing or recovering from only the phrase
loses these counters; retain the app's public receive-address history when
recovering the same wallet. A phrase backup still derives every address at its
recorded path.

### `sign_tx {address, address_index?, message, risk?, reason?}`

| Param | Notes |
|---|---|
| `message` | Base64 of `transaction.serializeMessage()`: the legacy or v0 **message**, ≤ 1232 bytes. |
| `address` | Must match the Solana key at `address_index`, else `wrong_wallet`. |
| `address_index` | Optional, defaults to 0. Must be a nonnegative index already allocated by this device. Generated addresses use the receive path above. |
| `risk` | Optional: `low`, `medium` or `high`. It can only *raise* Aurora's verdict; when missing, Aurora's own rule decides. |
| `reason` | Optional, ≤ 16 characters, shown on the risk page. |

What Aurora does:
1. **Parses the message itself.** Bad bytes give `invalid_message`, and a message Aurora doesn't need to sign gives `not_signer`.
2. **Recognises the instructions.** System transfer (Solana Pay reference accounts are allowed), ComputeBudget and Memo are known. Anything else is `UNKNOWN TX`: red, and unsignable unless `allow_blind_signing` is on.
3. **Applies its own rule.**
   - A recipient Aurora has never signed for is at least yellow.
   - A never-seen recipient receiving more than 1 SOL (`risk_limit_lamports`), an unknown instruction, or a priority fee above 0.01 SOL is red.
4. **Shows every part on a page,** with a green, yellow or red backlight: `SEND 0.50 SOL` / `TO 7xKX..gAsU`, then the full address, memo (`MEMO i/n`), fee, risk and hint pages.
5. **Requires certain pages to be seen first.** Every summary page always; the risk page when yellow or red; the full address for a first-time recipient.
6. **Signs only after a 2-second right-button hold,** which must start after the prompt appeared and while the LCD is working.

The result is `{"signature":"<base64, 64 bytes>","risk":"medium","reason":"NEW RECIPIENT"}`.
The app adds the signature, verifies it, and broadcasts.

### `cancel {}` and `get_pubkey {}`

- **`cancel`** aborts the waiting prompt. The prompt's request gets `cancelled`, then `cancel` returns `{"cancelled": bool}`.
- **`get_pubkey`** is an unsigned convenience lookup: `{"pubkey","device_id","name","backup_confirmed","pin_protected","locked"}`.

### `unlock {password}` and `lock {}`

- **`unlock`** decrypts a locked wallet: `{"state":"ready"}`. A wrong password
  gives `wrong_password` with `attempts_left`; after `pin_max_attempts`
  (default 5, counted across restarts) every attempt gives `pin_locked`.
  Unlocking an unprotected or unlocked wallet is a no-op.
- **`lock`** drops the decrypted keys and cancels any open prompt:
  `{"locked": bool}` (`false` when the wallet has no password).
- While locked, `pair`, `receive_address`, `sign_tx` and `show_seed` reply `locked` instead of
  prompting.

The password crosses the cable in plaintext unless the encrypted channel is
running. It is never stored on the PC. If it is forgotten, the 12 words recover the
funds in any BIP-39 wallet, and erasing Aurora starts over.

### `balances {wallets}`, `reset {}`

- **`balances`** sends `[{"id":"sol-dev","amount":"1.25","symbol":"SOL"}]`
  (≤ 20 entries; `amount` ≤ 16 digits with at most one `.`; `symbol` ≤ 6 capital letters) for the
  device's WALLETS menu: `{"shown": n}`.
- **`reset`** shows `ERASE WALLET?`, `MOVE FUNDS FIRST`, `HOLD R TO ERASE` in
  red. All three must be seen, then a hold erases the keys, the recipient list
  and the balances: `{"erased":true}`. Left keeps the wallet (`rejected`). It
  works even when locked out by wrong passwords.

### Device menu

Tapping right on the home screen opens `WALLETS`, `DISCONNECT APP`, `ERASE WALLET`
and `ABOUT`; the menu closes after 60 s. While the app is disconnected from the
menu, every command (`ping` too) gets `app_disconnected` until
`CONNECT APP` is chosen.

## Error codes

| `code` | `error` text (abridged) |
|---|---|
| `invalid_json`, `invalid_request`, `unknown_cmd`, `line_too_long` | Aurora received a malformed / unsupported request |
| `busy` | Aurora is waiting for you on its screen |
| `not_initialized` / `already_initialized` | No wallet yet / a wallet exists and is never replaced |
| `backup_already_confirmed` | The phrase is never shown again |
| `storage_error`, `internal_error` | Device-side failure (see `journalctl -u wallet-fw`) |
| `display_unavailable` | The screen isn't responding, so nothing can be confirmed |
| `invalid_message`, `not_signer`, `wrong_wallet` | Transaction problems |
| `rejected`, `cancelled`, `timeout` | The user said no, the app cancelled, or no answer in time |
| `locked` / `wrong_password` | Aurora needs the app password / the password was wrong (`attempts_left`) |
| `app_disconnected` | The app was disconnected from Aurora's menu |
| `pin_locked` | Too many wrong passwords or PINs. The counter survives restarts; recover the funds with the 12 words in any BIP-39 wallet. |
| `secure_channel_required`, `no_secure_channel`, `decrypt_failed`, `handshake_failed` | Encrypted-channel errors |

## Onboarding sequence (app)

```
every 2 s: ping (signed)  -> verify signature, challenge, protocol == 2
  no reply / no port      -> "No device connected"
  has_seed = false        -> choose password -> setup {password} ... "write the words down on Aurora, then confirm 3 on it"
  state = locked          -> password screen -> unlock {password}
  has_seed, not paired    -> pair (code shown in the app and on Aurora) -> save link -> dashboard
  paired link matches     -> dashboard (wallets, balances, history, send)
```

## Encrypted channel (Step 12, optional)

This is off unless the app starts it, or the device has `require_secure_channel`
(then only `ping`, `get_pubkey` and `handshake` work in plaintext).

1. The app makes an X25519 key pair `(a, A)` and sends
   `{"id":1,"command":"handshake","params":{"client_pubkey":"<base64 A>"}}`.
2. The device replies in plaintext:
   `{"id":1,"result":{"device_ephemeral":"<base64 B>","device_id":"<base58 identity key>","signature":"<base64>"}}`.
   The identity key signs it, so no wallet or PIN is needed.
3. The app checks:
   - that `device_id` is the device it paired with;
   - the ed25519 `signature` over `transcript = "WFW-HANDSHAKE-V1" (16 ASCII bytes) ‖ A ‖ B ‖ identityPubkey`.
4. Both sides derive the session keys:
   - `shared = X25519(a, B)`
   - `okm = HKDF-SHA256(salt = SHA-256(transcript), ikm = shared, info = "wfw1 session keys", L = 64)`
   - `k_c2d = okm[0..32]`, `k_d2c = okm[32..64]`.
5. Every later line is `{"enc":"<base64(nonce ‖ ciphertext ‖ tag)>"}`:
   - The cipher is ChaCha20-Poly1305 (IETF).
   - `nonce` = 4 zero bytes ‖ u64 little-endian counter. Each direction counts from 0 and must arrive in order; replays are refused.
   - Associated data is `"wfw1-c2d"` for app → device and `"wfw1-d2c"` for device → app.
   - The plaintext is the normal request/reply JSON.

A successful handshake replaces the channel; a failed one leaves the current
channel working. The channel ends when the serial link resets. A
PIN-protected wallet relocks after `auto_lock_s` (default 300 s) without a prompt. Reference implementations: `SecureSession` in `tools/wfw_host.py` and
`src/crypto/secure_channel.cpp`.

## Timing summary

| Item | Value |
|---|---|
| Reply to non-prompting commands | < 50 ms |
| Confirm hold | 2 s (`hold_confirm_ms`) |
| `sign_tx` prompt timeout | 180 s (`review_timeout_s`) |
| `setup` / `show_seed` timeout | 900 s (`setup_timeout_s`); the app waits 900 s, and 200 s for `pair` / `sign_tx` |
| Password / PIN unlock (Argon2id) | ~0.5–1 s on a Pi 4 |
