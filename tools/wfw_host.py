"""Reference host for the wallet-fw serial protocol.

Implements docs/firmware/protocol-contract.md from the host side: JSON-lines
framing, request ids, the optional encrypted channel, and a Solana legacy
transfer message builder. Used by the integration tests and wallet_cli.py.

Dependencies: pyserial (or Linux/macOS termios fallback); pynacl for the
encrypted channel and signature checks.
"""
import base64
import hashlib
import hmac
import json
import os
import struct
import time

B58_ALPHABET = "123456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnopqrstuvwxyz"
SYSTEM_PROGRAM = bytes(32)
LAMPORTS_PER_SOL = 1_000_000_000


def b58encode(data: bytes) -> str:
    n = int.from_bytes(data, "big")
    out = ""
    while n:
        n, r = divmod(n, 58)
        out = B58_ALPHABET[r] + out
    return "1" * (len(data) - len(data.lstrip(b"\0"))) + out


def b58decode(text: str) -> bytes:
    n = 0
    for c in text:
        n = n * 58 + B58_ALPHABET.index(c)
    body = n.to_bytes((n.bit_length() + 7) // 8, "big") if n else b""
    return b"\0" * (len(text) - len(text.lstrip("1"))) + body


def b64(data: bytes) -> str:
    return base64.b64encode(data).decode()


def compact_u16(n: int) -> bytes:
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def transfer_message(sender: bytes, recipient: bytes, lamports: int, blockhash: bytes,
                     reference: bytes = None) -> bytes:
    """Legacy message for one SystemProgram.transfer. Without `reference` it
    is byte-identical to web3.js Transaction.serializeMessage(); `reference`
    adds a Solana Pay read-only reference account to the instruction."""
    readonly = ([reference] if reference else []) + [SYSTEM_PROGRAM]
    keys = [sender, recipient] + readonly
    accounts = [0, 1] + ([2] if reference else [])
    data = struct.pack("<IQ", 2, lamports)
    return (bytes([1, 0, len(readonly)]) + compact_u16(len(keys)) + b"".join(keys) + blockhash +
            compact_u16(1) + bytes([len(keys) - 1]) + compact_u16(len(accounts)) +
            bytes(accounts) + compact_u16(len(data)) + data)


def wire_transaction(message: bytes, signature: bytes) -> bytes:
    return compact_u16(1) + signature + message


def hkdf_sha256(salt: bytes, ikm: bytes, info: bytes, length: int) -> bytes:
    prk = hmac.new(salt or bytes(32), ikm, hashlib.sha256).digest()
    okm, block, counter = b"", b"", 1
    while len(okm) < length:
        block = hmac.new(prk, block + info + bytes([counter]), hashlib.sha256).digest()
        okm += block
        counter += 1
    return okm[:length]


class SecureSession:
    """Client half of the encrypted channel (X25519 + HKDF-SHA256 +
    ChaCha20-Poly1305). Mirrors src/crypto/secure_channel.cpp."""

    LABEL = b"WFW-HANDSHAKE-V1"

    def __init__(self):
        from nacl import bindings
        self._nacl = bindings
        self._secret = os.urandom(32)
        self.public = bindings.crypto_scalarmult_base(self._secret)
        self._tx = self._rx = 0

    def complete(self, device_ephemeral: bytes, device_pubkey: bytes, signature: bytes):
        from nacl.signing import VerifyKey
        transcript = self.LABEL + self.public + device_ephemeral + device_pubkey
        VerifyKey(device_pubkey).verify(transcript, signature)  # raises if forged
        shared = self._nacl.crypto_scalarmult(self._secret, device_ephemeral)
        okm = hkdf_sha256(hashlib.sha256(transcript).digest(), shared, b"wfw1 session keys", 64)
        self._send_key, self._recv_key = okm[:32], okm[32:]

    def seal(self, text: str) -> str:
        nonce = bytes(4) + struct.pack("<Q", self._tx)
        self._tx += 1
        ct = self._nacl.crypto_aead_chacha20poly1305_ietf_encrypt(
            text.encode(), b"wfw1-c2d", nonce, self._send_key)
        return b64(nonce + ct)

    def open(self, frame: str) -> str:
        raw = base64.b64decode(frame)
        nonce = raw[:12]
        if nonce[:4] != bytes(4) or struct.unpack("<Q", nonce[4:])[0] != self._rx:
            raise ValueError("unexpected nonce from device")
        self._rx += 1
        return self._nacl.crypto_aead_chacha20poly1305_ietf_decrypt(
            raw[12:], b"wfw1-d2c", nonce, self._recv_key).decode()


class _TermiosPort:
    """Minimal raw serial port for systems without pyserial (Linux/macOS)."""

    def __init__(self, path, baud):
        import termios
        import tty
        self.fd = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
        tty.setraw(self.fd)
        attrs = termios.tcgetattr(self.fd)
        speed = getattr(termios, "B%d" % baud, termios.B115200)
        attrs[4] = attrs[5] = speed
        termios.tcsetattr(self.fd, termios.TCSANOW, attrs)

    def write(self, data):
        view = memoryview(data)
        while view:
            try:
                view = view[os.write(self.fd, view):]
            except BlockingIOError:
                time.sleep(0.005)

    def read_some(self):
        try:
            return os.read(self.fd, 4096)
        except BlockingIOError:
            return b""

    def close(self):
        os.close(self.fd)


class _PySerialPort:
    def __init__(self, path, baud):
        import serial
        self.ser = serial.Serial(path, baud, timeout=0)

    def write(self, data):
        self.ser.write(data)

    def read_some(self):
        return self.ser.read(4096)

    def close(self):
        self.ser.close()


class DeviceError(Exception):
    def __init__(self, reply):
        super().__init__("%s: %s" % (reply.get("error"), reply.get("message", "")))
        self.reply = reply


class Device:
    """One request in flight at a time, matched to its reply by id."""

    def __init__(self, path, baud=115200):
        try:
            self.port = _PySerialPort(path, baud)
        except ImportError:
            self.port = _TermiosPort(path, baud)
        self.secure = None
        self._next_id = 1
        self._buffer = b""

    def close(self):
        self.port.close()

    def send_line(self, line: str):
        self.port.write(line.encode() + b"\n")

    def begin(self, cmd, **params):
        req = {"id": self._next_id, "cmd": cmd, **params}
        self._next_id += 1
        line = json.dumps(req)
        if self.secure is not None and cmd != "handshake":
            line = json.dumps({"enc": self.secure.seal(line)})
        self.send_line(line)
        return req["id"]

    def read_message(self, timeout):
        deadline = time.monotonic() + timeout
        while b"\n" not in self._buffer:
            if time.monotonic() > deadline:
                raise TimeoutError("no reply from device")
            chunk = self.port.read_some()
            if chunk:
                self._buffer += chunk
            else:
                time.sleep(0.005)
        line, self._buffer = self._buffer.split(b"\n", 1)
        msg = json.loads(line)
        if "enc" in msg:
            msg = json.loads(self.secure.open(msg["enc"]))
        return msg

    def wait(self, req_id, timeout=30.0):
        deadline = time.monotonic() + timeout
        while True:
            msg = self.read_message(max(0.0, deadline - time.monotonic()))
            # Errors the device could not tie to a request carry id null.
            if msg.get("id") == req_id or (msg.get("id") is None and not msg.get("ok")):
                return msg

    def request(self, cmd, timeout=30.0, check=True, **params):
        reply = self.wait(self.begin(cmd, **params), timeout)
        if check and not reply.get("ok"):
            raise DeviceError(reply)
        return reply

    def handshake(self, expected_pubkey: str = None):
        session = SecureSession()
        reply = self.request("handshake", client_pubkey=b64(session.public))
        if expected_pubkey and reply["pubkey"] != expected_pubkey:
            raise ValueError("device identity changed")
        session.complete(base64.b64decode(reply["device_ephemeral"]),
                         b58decode(reply["pubkey"]), base64.b64decode(reply["signature"]))
        self.secure = session
        return reply


class DevnetRpc:
    def __init__(self, url="https://api.devnet.solana.com"):
        self.url = url

    def call(self, method, *params):
        body = json.dumps({"jsonrpc": "2.0", "id": 1, "method": method, "params": list(params)})
        req = urllib_request(self.url, body.encode())
        reply = json.loads(req.read())
        if "error" in reply:
            raise RuntimeError("%s: %s" % (method, reply["error"]))
        return reply["result"]

    def latest_blockhash(self) -> bytes:
        return b58decode(self.call("getLatestBlockhash", {"commitment": "finalized"})["value"]["blockhash"])

    def balance(self, address) -> int:
        return self.call("getBalance", address)["value"]

    def send(self, wire: bytes) -> str:
        return self.call("sendTransaction", b64(wire), {"encoding": "base64"})

    def confirm(self, signature, timeout=60.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            status = self.call("getSignatureStatuses", [signature])["value"][0]
            if status and status.get("err"):
                raise RuntimeError("transaction failed: %s" % status["err"])
            if status and status.get("confirmationStatus") in ("confirmed", "finalized"):
                return status
            time.sleep(1.0)
        raise TimeoutError("not confirmed in time")


def urllib_request(url, body):
    import urllib.request
    req = urllib.request.Request(url, body, {"Content-Type": "application/json"})
    return urllib.request.urlopen(req, timeout=30)
