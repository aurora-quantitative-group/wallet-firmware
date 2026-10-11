#!/usr/bin/env python3
"""End-to-end test of the real wallet-fw binary in simulator mode.

Starts `wallet-fw --sim`, talks to it over its PTY (or the development TCP
transport) exactly as the Aurora app does over USB serial, presses buttons
through the control FIFO, and reads the LCD frames it logs. Identity and
transaction signatures are checked with PyNaCl.

  python3 tests/integration/test_sim.py <path/to/wallet-fw>
"""
import base64
import json
import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "..", "tools"))
import wfw_host as host  # noqa: E402

BINARY = None
RECIPIENT = "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU"
GREEN, YELLOW, RED = [0, 255, 0], [255, 180, 0], [255, 0, 0]


def free_port():
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class Sim:
    def __init__(self, workdir, tcp_port=None):
        self.workdir = workdir
        self.pty = os.path.join(workdir, "wallet.pty")
        self.fifo = os.path.join(workdir, "buttons")
        self.log = os.path.join(workdir, "display.jsonl")
        if not os.path.exists(self.fifo):
            os.mkfifo(self.fifo)
        config = os.path.join(workdir, "config.json")
        with open(config, "w") as f:
            f.write("{}")
        args = [BINARY, "--sim", "--sim-quiet", "--config", config,
                "--data-dir", os.path.join(workdir, "data"), "--pty-link", self.pty,
                "--sim-control", self.fifo, "--sim-display-log", self.log]
        if tcp_port:
            args += ["--tcp-listen", "127.0.0.1:%d" % tcp_port]
        self._stderr = open(os.path.join(workdir, "stderr.log"), "a")
        self.proc = subprocess.Popen(args, stderr=self._stderr)
        self.control = open(self.fifo, "w")
        deadline = time.monotonic() + 10
        while True:
            if self.proc.poll() is not None or time.monotonic() > deadline:
                raise RuntimeError("simulator did not start")
            try:
                if tcp_port:
                    self.dev = host.Device("tcp://127.0.0.1:%d" % tcp_port)
                    break
                if os.path.exists(self.pty):
                    self.dev = host.Device(self.pty)
                    break
            except OSError:
                pass
            time.sleep(0.05)

    def press(self, *commands):
        for c in commands:
            self.control.write(c + "\n")
        self.control.flush()

    def screen(self):
        try:
            with open(self.log) as f:
                frames = [json.loads(line) for line in f if '"top"' in line]
        except FileNotFoundError:
            return None
        return frames[-1] if frames else None

    def wait_screen(self, predicate, timeout=10.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            s = self.screen()
            if s and predicate(s):
                return s
            time.sleep(0.02)
        raise AssertionError("screen never matched; last: %r" % self.screen())

    def read_words(self):
        """From the setup intro, read the 12 words off the LCD."""
        words = []
        for i in range(12):
            self.press("m")
            s = self.wait_screen(lambda s, i=i: s["top"].startswith("%02d/12 " % (i + 1)))
            words.append(s["top"].split()[1])
        return words

    def answer_quiz(self, words, correctly=True):
        """Answers the 3-word backup check (WORD #07? / candidate n/4)."""
        for _ in range(3 if correctly else 1):
            s = self.wait_screen(lambda s: s["top"].startswith("WORD #"))
            question = s["top"]
            want = words[int(question[6:8]) - 1]
            for _ in range(4):
                current = self.screen()["bottom"]
                if (current.split()[0] == want) == correctly:
                    break
                self.press("m")
                self.wait_screen(lambda s, c=current: s["bottom"] != c)
            self.press("h")
            # Wait until the screen moves past this question.
            self.wait_screen(lambda s, q=question: s["top"] != q, timeout=6)

    def stop(self):
        self.dev.close()
        self.control.close()
        self.proc.terminate()
        self.proc.wait(timeout=10)
        self._stderr.close()


class SimulatorEndToEnd(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.workdir = tempfile.mkdtemp(prefix="wfw-sim-")
        cls.sim = Sim(cls.workdir)

    @classmethod
    def tearDownClass(cls):
        cls.sim.stop()
        shutil.rmtree(cls.workdir, ignore_errors=True)

    def sign(self, to, lamports, risk="low"):
        me_text = self.sim.dev.request("get_pubkey")["pubkey"]
        me = host.b58decode(me_text)
        message = host.transfer_message(me, host.b58decode(to), lamports, os.urandom(32))
        req = self.sim.dev.begin("sign_tx", address=me_text, message=host.b64(message), risk=risk)
        return me, message, req

    def test_1_setup_with_on_device_backup_check_and_restart(self):
        dev, sim = self.sim.dev, self.sim
        ident = dev.identity()
        self.assertEqual(ident["state"], "uninitialized")
        self.assertEqual(ident["protocol"], 2)
        self.assertFalse(ident["has_seed"])
        self.assertEqual(dev.request("get_pubkey", check=False)["code"], "not_initialized")

        req = dev.begin("setup")
        sim.wait_screen(lambda s: s["top"].startswith("WRITE DOWN"))
        words = sim.read_words()
        sim.press("h")
        sim.answer_quiz(words, correctly=False)  # wrong word: back to the words
        sim.wait_screen(lambda s: s["top"].startswith("WRITE DOWN"))
        self.assertEqual(sim.read_words(), words)
        sim.press("h")
        sim.answer_quiz(words)
        reply = dev.wait(req, timeout=10)
        self.assertTrue(reply["result"]["backup_confirmed"], reply)

        after = dev.identity(expected_id=ident["device_id"])
        self.assertTrue(after["has_seed"])
        self.assertEqual([w["id"] for w in after["wallets"]], ["sol-0", "sol-dev", "btc-0"])
        self.assertTrue(after["wallets"][2]["address"].startswith("bc1q"))
        self.assertEqual(dev.request("show_seed", check=False)["code"], "backup_already_confirmed")

        sim.stop()  # power cycle: identity and wallet persist
        type(self).sim = Sim(self.workdir)
        again = self.sim.dev.identity(expected_id=ident["device_id"])
        self.assertEqual(again["wallets"], after["wallets"])

    def test_2_first_send_yellow_then_green(self):
        from nacl.signing import VerifyKey
        sim = self.sim
        me, message, req = self.sign(RECIPIENT, 500_000_000)
        # The LCD is written row by row; wait for the complete page.
        sim.wait_screen(lambda s: s["top"].strip() == "SEND 0.50 SOL" and
                        s["bottom"].strip() == "TO 7xKX..gAsU" and s["rgb"] == YELLOW)
        sim.press("m", "m", "m")  # new recipient: read the full address and risk page
        sim.wait_screen(lambda s: s["top"].strip() == "RISK: MEDIUM")
        sim.press("h")
        reply = sim.dev.wait(req)["result"]
        self.assertEqual(reply["risk"], "medium")
        VerifyKey(me).verify(message, base64.b64decode(reply["signature"]))

        me, message, req = self.sign(RECIPIENT, 500_000_000)
        sim.wait_screen(lambda s: s["top"].strip() == "SEND 0.50 SOL" and s["rgb"] == GREEN)
        sim.press("h")
        reply = sim.dev.wait(req)["result"]
        self.assertEqual(reply["risk"], "low")
        VerifyKey(me).verify(message, base64.b64decode(reply["signature"]))

    def test_3_compromised_app_goes_red_and_is_rejected(self):
        sim = self.sim
        other = host.b58encode(bytes([7]) * 32)
        _, _, req = self.sign(other, 2 * host.LAMPORTS_PER_SOL, risk="low")
        sim.wait_screen(lambda s: s["top"].strip() == "SEND 2.00 SOL" and s["rgb"] == RED and
                        s["bottom"].strip() == "TO " + other[:4] + ".." + other[-4:])
        with open(sim.log) as f:
            self.assertIn('"buzzer":true', f.read())
        sim.press("l")
        reply = sim.dev.wait(req)
        self.assertEqual(reply["code"], "rejected")
        self.assertEqual(reply["error"], "Rejected on Aurora.")
        self.assertEqual(reply["reason"], "NEW+OVER LIMIT")

    def test_4_pairing_and_encrypted_channel(self):
        from nacl.signing import VerifyKey
        sim = self.sim
        device_id = sim.dev.identity()["device_id"]
        req = sim.dev.begin("pair", challenge="0123456789abcdef", code="2741")
        sim.wait_screen(lambda s: s["top"].strip() == "PAIR CODE 2741")
        sim.press("l")
        self.assertEqual(sim.dev.wait(req)["code"], "rejected")
        time.sleep(2.2)  # let the "rejected" notice clear
        req = sim.dev.begin("pair", challenge="fedcba9876543210", code="5309")
        sim.wait_screen(lambda s: s["top"].strip() == "PAIR CODE 5309")
        sim.press("h")
        result = sim.dev.wait(req)["result"]
        VerifyKey(host.b58decode(device_id)).verify(result["payload"].encode(),
                                                     base64.b64decode(result["signature"]))
        payload = json.loads(result["payload"])
        self.assertEqual((payload["code"], payload["device_id"]), ("5309", device_id))

        sim.dev.handshake(expected_id=device_id)
        self.assertTrue(sim.dev.identity()["secure_channel"])
        me, message, req = self.sign(RECIPIENT, 10_000_000)
        sim.wait_screen(lambda s: s["top"].strip() == "SEND 0.01 SOL")
        sim.press("h")
        reply = sim.dev.wait(req)["result"]
        VerifyKey(me).verify(message, base64.b64decode(reply["signature"]))
        sim.dev.secure = None
        sim.dev.handshake(expected_id=device_id)  # re-keying works
        sim.dev.secure = None

    def test_5_framing_errors_keep_the_link_usable(self):
        dev = self.sim.dev
        dev.send_line("not json")
        self.assertEqual(dev.read_message(5)["code"], "invalid_json")
        dev.send_line("x" * 9000)
        self.assertEqual(dev.read_message(5)["code"], "line_too_long")
        self.assertTrue(dev.identity()["device_id"])

    def test_6_host_builder_matches_web3js(self):
        # Same inputs as tests/unit/test_vectors.h kTransferMsgHex.
        sender = host.b58decode("HAgk14JpMQLgt6rVgv7cBQFJWFto5Dqxi472uT3DKpqk")
        blockhash = host.b58decode("EETubP5AKHgjPAhzPAFcb8BAY1hMH639CWCFTqi3hq1k")
        msg = host.transfer_message(sender, host.b58decode(RECIPIENT), 500_000_000, blockhash)
        self.assertEqual(msg.hex(), (
            "01000103f036276246a75b9de3349ed42b15e232f6518fc20f5fcd4f1d64e81f9bd258f76752055c20b3e9d874"
            "6656ddf73855507f87ab6d87523e4c76a7fa36096a99eb00000000000000000000000000000000000000000000"
            "00000000000000000000c49ae77603782054f17a9decea43b444eba0edb12c6f1d31c6e0e4a84bf052eb010202"
            "00010c020000000065cd1d00000000"))

    def test_7_tcp_development_transport(self):
        workdir = tempfile.mkdtemp(prefix="wfw-tcp-")
        port = free_port()
        sim = Sim(workdir, tcp_port=port)
        try:
            ident = sim.dev.identity()
            self.assertEqual(ident["state"], "uninitialized")
            sim.dev.close()  # reconnecting works (one client at a time)
            time.sleep(0.3)
            sim.dev = host.Device("tcp://127.0.0.1:%d" % port)
            self.assertEqual(sim.dev.identity()["device_id"], ident["device_id"])
        finally:
            sim.stop()
            shutil.rmtree(workdir, ignore_errors=True)


if __name__ == "__main__":
    BINARY = os.path.abspath(sys.argv.pop(1)) if len(sys.argv) > 1 else "build/wallet-fw"
    unittest.main(verbosity=2)
