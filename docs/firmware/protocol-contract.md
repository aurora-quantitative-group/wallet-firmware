# wallet-fw serial protocol (v1)

This is the contract between the device firmware and the desktop app (Svelte +
Tauri). The firmware implements it in `src/device/`; `tools/wfw_host.py` is a
working host-side reference in about 250 lines of Python.

## Transport

| | |
|---|---|
| Link | USB-C gadget serial. On the laptop it is a normal serial port (`COMx` / `/dev/tty.usbmodem*` / `/dev/ttyACM*`). Fallback: Pico debugprobe UART bridge. |
| Settings | 115200 8N1, no flow control (the baud rate is ignored by the USB gadget) |
| Framing | One JSON object per line, UTF-8, terminated by `\n` (`\r\n` accepted). Max 8192 bytes per line. |
| Model | Strict request → reply. Send one request, read lines until the reply with the same `id` arrives. The device never sends unsolicited messages. |

Every request:

```json
{"id": 42, "cmd": "<command>", ...params}
```

`id` is optional (string or number) and echoed back unchanged. Always send one.

Every reply has `ok`. On failure it also has `error` (a stable code from the
table below) and usually a human-readable `message`:

```json
{"id": 42, "ok": false, "error": "not_initialized", "message": "run setup"}
```

A reply with `"id": null` and `ok: false` is an error the device could not
tie to a request (bad JSON, line too long, bad encrypted frame). Treat it
as the answer to your outstanding request.

### Commands that wait for the user

`setup`, `show_words` and `sign_tx` (and `handshake` when a PIN must be
entered) do not reply until the person acts on the device. Wait at least as
long as the device timeout: 180 s for `sign_tx`, 900 s for `setup` and
`show_words`. Meanwhile the device still answers `ping`, `get_pubkey`,
`confirm_backup` and `cancel`. Another prompting command gets `busy`.

## Commands

### `ping`

```json
→ {"id":1,"cmd":"ping"}
← {"id":1,"ok":true,"pong":true,"version":"0.1.0","protocol":1,
   "initialized":true,"state":"ready","name":"SolPi-HAgk",
   "busy":false,"display_ok":true,"secure_channel":false}
```

`state` is one of:

| `state` | Meaning |
|---|---|
| `uninitialized` | No wallet yet: run setup. |
| `ready` | Wallet present and unlocked. |
| `locked` | Wallet is PIN-protected and locked; the device asks for the PIN when needed. |
| `error` | The keystore is unreadable. Show the user an error. |

Use `ping` every 2 s to detect the device (build manual Step 8: "No device connected").

### `get_pubkey`

```json
→ {"id":2,"cmd":"get_pubkey"}
← {"id":2,"ok":true,"pubkey":"HAgk14JpMQLgt6rVgv7cBQFJWFto5Dqxi472uT3DKpqk",
   "name":"SolPi-HAgk","backup_confirmed":false,"pin_protected":false,"locked":false}
```

Errors: `not_initialized` (go to onboarding), `storage_error`. The public key
is the device ID.

### `setup`

Creates a new wallet: 128 random bits, a 12-word BIP-39 phrase, and the key at
`m/44'/501'/0'/0'` (same address as Phantom for the same phrase). The words are
only ever shown on the device screen: `01/12 ocean`, middle = next, left =
back, hold right on word 12 = done. With `pin_enabled`, the user then sets
a PIN on the device.

```json
→ {"id":3,"cmd":"setup"}
… user writes the words down and holds the right button on 12/12 …
← {"id":3,"ok":true,"pubkey":"…","backup_confirmed":false,"pin_protected":false}
```

The wallet is saved when this reply is sent. Errors: `already_initialized`,
`busy`, `display_unavailable`, `cancelled`, `timeout`, `storage_error`.

### `confirm_backup` (extension)

Checks the phrase the user typed back in the app. The device compares it by
deriving the address, so this works while PIN-locked. The device normalizes
case and whitespace.

```json
→ {"id":4,"cmd":"confirm_backup","words":"ocean wing … about"}
← {"id":4,"ok":true,"backup_confirmed":true}
← {"id":4,"ok":false,"error":"phrase_mismatch","message":"the words do not match this device"}
```

On `phrase_mismatch`, let the user retry, or offer "Back", which sends
`show_words`. After a success the words can never be shown again.

### `show_words` (extension)

Shows the 12 words on the device again (the "back" button in Step 8). Only
allowed while `backup_confirmed` is false.

```json
→ {"id":5,"cmd":"show_words"}
← {"id":5,"ok":true}                 // after the user holds right on word 12
```

Errors: `backup_already_confirmed`, `not_initialized`, `busy`, `cancelled`, `timeout`.

### `sign_tx`

```json
→ {"id":6,"cmd":"sign_tx",
   "message":"<base64 of transaction.serializeMessage()>",
   "risk":"low", "reason":"Known payee"}
← {"id":6,"ok":true,"signature":"<base64, 64 bytes>","risk":"medium","reason":"NEW RECIPIENT"}
← {"id":6,"ok":false,"error":"rejected","risk":"high","reason":"NEW+OVER LIMIT"}
```

| Field | Notes |
|---|---|
| `message` | Required. The serialized **message** (not the transaction), legacy or v0, ≤ 1232 bytes. |
| `risk` | `low` / `medium` / `high` from the Gemini risk check. If it is missing, `medium` is used (the Step 10 fallback). |
| `reason` | Optional, shown on the device's risk page; keep it ≤ 16 characters. |

What the device does:

1. **Parses the message itself.** It does not trust the app's description.
   Malformed bytes return `invalid_message` immediately. If the device's key
   is not a required signer, it returns `not_signer`.
2. **Recognises the instructions:** System `transfer` (extra read-only accounts
   such as Solana Pay references are fine), ComputeBudget unit limit, price,
   heap and data-size, and Memo v1/v2. Anything else is shown as `UNKNOWN TX`
   and is red.
3. **Applies its own risk rule.** The app's level can only be raised:
   - a recipient it has never signed for is at least `medium`;
   - a never-seen recipient receiving more than `risk_limit_lamports` (default 1 SOL) is `high`;
   - an unknown instruction, or a priority fee above 0.01 SOL, is `high`.
4. **Shows each part on its own page**, with green, yellow or red backlight. Line 1
   is `SEND 0.50 SOL`, line 2 is `TO 7xKX..gAsU`. Middle scrolls through the full
   address, memo, priority fee, the `RISK:` page and the hint page. It beeps on
   every request and plays a long tone for `high`.
5. **Signs only after a 2 s hold** of the right button that started after the
   prompt appeared. A red transaction also needs its risk page viewed. Left
   rejects.

The reply's `risk` and `reason` are what the device enforced, so log them as the
Tiger Data `device_events` detail. After a signed transfer, the recipient is
remembered and later sends to it can be green.

Then, in the app (Step 6):

```js
const tx = new Transaction({ recentBlockhash, feePayer: devicePubkey })
  .add(SystemProgram.transfer({ fromPubkey: devicePubkey, toPubkey, lamports }));
const message = tx.serializeMessage();
const reply = await device.request({ cmd: "sign_tx", message: message.toString("base64"), risk });
tx.addSignature(devicePubkey, Buffer.from(reply.signature, "base64"));
const sig = await connection.sendRawTransaction(tx.serialize());
await connection.confirmTransaction(sig);
```

Errors: `rejected`, `invalid_request`, `invalid_message`, `not_signer`,
`not_initialized`, `busy`, `display_unavailable`, `pin_locked`, `cancelled`, `timeout`.

### `cancel` (extension)

Aborts the prompt that is waiting on the device. The waiting request gets
`cancelled` first, then:

```json
← {"id":7,"ok":true,"cancelled":true}     // false if nothing was pending
```

## Error codes

| Code | Meaning |
|---|---|
| `invalid_json` | The line was not one JSON object |
| `invalid_request` | Missing or ill-typed field (`cmd`, `message`, `risk`, `words`, …) |
| `unknown_cmd` | Unrecognised `cmd` |
| `line_too_long` | Line over 8192 bytes; it was discarded |
| `busy` | Another device prompt is open |
| `not_initialized` / `already_initialized` | Wallet state is wrong for the command |
| `storage_error` | Keystore unreadable or not writable |
| `display_unavailable` | The LCD is not responding, so nothing can be shown and nothing is signed |
| `invalid_message` | `message` is not a valid Solana message |
| `not_signer` | The device key is not a required signer of `message` |
| `rejected` | The user pressed reject |
| `cancelled` / `timeout` | Prompt aborted by `cancel` / not answered in time |
| `phrase_mismatch` | `confirm_backup` words are wrong |
| `backup_already_confirmed` | `show_words` / `confirm_backup` after confirmation |
| `pin_locked` | Too many wrong PINs; restart the device |
| `secure_channel_required` | `require_secure_channel` is on and the request was not encrypted |
| `no_secure_channel` / `decrypt_failed` / `handshake_failed` | Encrypted-channel errors |
| `internal_error` | Bug or resource failure; check `journalctl -u wallet-fw` |

## Onboarding sequence (Step 8)

```
loop every 2 s: ping
  no reply / no port      -> "No device connected"
  state = uninitialized   -> setup ............ show "write down the words on the device"
                             reply.pubkey ...... ask the user to type the 12 words
                             confirm_backup .... phrase_mismatch -> retry, or "Back" -> show_words
                             ok -> homepage
  state = ready | locked  -> get_pubkey -> homepage (name, address, balance, history)
```

## Encrypted channel (Step 12, optional)

This is off unless the app starts it, or the device has `require_secure_channel`
(then only `ping`, `get_pubkey` and `handshake` work in plaintext).

1. The app makes an X25519 key pair `(a, A)` and sends
   `{"id":1,"cmd":"handshake","client_pubkey":"<base64 A>"}`. If the device is
   PIN-locked, it asks for the PIN first.
2. The device replies in plaintext:
   `{"ok":true,"device_ephemeral":"<base64 B>","pubkey":"<base58 device key>","signature":"<base64>"}`.
3. The app checks:
   - that `pubkey` is the device it paired with;
   - the ed25519 `signature` over `transcript = "WFW-HANDSHAKE-V1" (16 ASCII bytes) ‖ A ‖ B ‖ devicePubkey`.
4. Both sides derive the session keys:
   - `shared = X25519(a, B)`
   - `okm = HKDF-SHA256(salt = SHA-256(transcript), ikm = shared, info = "wfw1 session keys", L = 64)`
   - `k_c2d = okm[0..32]`, `k_d2c = okm[32..64]`.
5. Every later line is `{"enc":"<base64(nonce ‖ ciphertext ‖ tag)>"}`:
   - The cipher is ChaCha20-Poly1305 (IETF).
   - `nonce` = 4 zero bytes ‖ u64 little-endian counter. Each direction counts from 0 and must arrive in order; replays are refused.
   - Associated data is `"wfw1-c2d"` for app → device and `"wfw1-d2c"` for device → app.
   - The plaintext is the normal request/reply JSON.

A new handshake replaces the channel, and the channel ends when the serial link
resets. Reference implementations: `SecureSession` in `tools/wfw_host.py` and
`src/crypto/secure_channel.cpp`.

## Timing summary

| Item | Value |
|---|---|
| Reply to non-prompting commands | < 50 ms (`confirm_backup` ~ 20–50 ms on a Pi 4 because of PBKDF2) |
| Confirm hold | 2 s (`hold_confirm_ms`) |
| `sign_tx` prompt timeout | 180 s (`review_timeout_s`) |
| `setup` / `show_words` timeout | 900 s (`setup_timeout_s`) |
| PIN unlock (Argon2id) | ~0.5–1 s on a Pi 4 |
