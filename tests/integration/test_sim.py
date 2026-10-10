#!/usr/bin/env python3
"""End-to-end test of the real wallet-fw binary in simulator mode.

Starts `wallet-fw --sim`, talks to it over its PTY exactly as the app would
over USB serial, presses buttons through the control FIFO, and reads the
LCD frames it logs. Signatures are checked with PyNaCl.

  python3 tests/integration/test_sim.py <path/to/wallet-fw>
"""
import base64
import json
import os
import shutil
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


class Sim:
    def __init__(self, workdir, extra_config=None):
        self.workdir = workdir
        self.pty = os.path.join(workdir, "wallet.pty")
        self.fifo = os.path.join(workdir, "buttons")
        self.log = os.path.join(workdir, "display.jsonl")
        self.config = os.path.join(workdir, "config.json")
        with open(self.config, "w") as f:
            json.dump(extra_config or {}, f)
        if not os.path.exists(self.fifo):
            os.mkfifo(self.fifo)
        self._stderr = open(os.path.join(workdir, "stderr.log"), "a")
        self.proc = subprocess.Popen(
            [BINARY, "--sim", "--sim-quiet", "--config", self.config,
             "--data-dir", os.path.join(workdir, "data"), "--pty-link", self.pty,
             "--sim-control", self.fifo, "--sim-display-log", self.log],
            stderr=self._stderr)
        # Opening the FIFO for writing blocks until the firmware opens it.
        self.control = open(self.fifo, "w")
        deadline = time.monotonic() + 10
        while not os.path.exists(self.pty):
            if time.monotonic() > deadline or self.proc.poll() is not None:
                raise RuntimeError("simulator did not start")
            time.sleep(0.05)
        self.dev = host.Device(self.pty)

    def press(self, *commands):
        for c in commands:
            self.control.write(c + "\n")
        self.control.flush()

    def frames(self):
        with open(self.log) as f:
            return [json.loads(line) for line in f if '"top"' in line]

    def screen(self):
        frames = self.frames()
        return frames[-1] if frames else None

    def wait_screen(self, predicate, timeout=10.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            s = self.screen()
            if s and predicate(s):
                return s
            time.sleep(0.02)
        raise AssertionError("screen never matched; last: %r" % self.screen())

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
        me = host.b58decode(self.sim.dev.request("get_pubkey")["pubkey"])
        message = host.transfer_message(me, host.b58decode(to), lamports, os.urandom(32))
        req = self.sim.dev.begin("sign_tx", message=host.b64(message), risk=risk)
        return me, message, req

    def test_1_setup_backup_and_restart(self):
        dev, sim = self.sim.dev, self.sim
        ping = dev.request("ping")
        self.assertEqual(ping["state"], "uninitialized")
        self.assertEqual(dev.request("get_pubkey", check=False)["error"], "not_initialized")

        req = dev.begin("setup")
        sim.wait_screen(lambda s: s["top"].startswith("WRITE DOWN"))
        words = []
        for i in range(12):
            sim.press("m")
            s = sim.wait_screen(lambda s, i=i: s["top"].startswith("%02d/12 " % (i + 1)))
            words.append(s["top"].split()[1])
        sim.press("h")
        reply = dev.wait(req, timeout=10)
        self.assertTrue(reply["ok"], reply)
        address = reply["pubkey"]

        self.assertEqual(dev.request("confirm_backup", check=False, words="abandon " * 11 + "about")["error"],
                         "phrase_mismatch")
        self.assertTrue(dev.request("confirm_backup", words=" ".join(words))["backup_confirmed"])

        # Power cycle: the wallet persists.
        sim.stop()
        type(self).sim = Sim(self.workdir)
        self.assertEqual(self.sim.dev.request("get_pubkey")["pubkey"], address)
        self.assertTrue(self.sim.dev.request("get_pubkey")["backup_confirmed"])

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
        reply = sim.dev.wait(req)
        self.assertTrue(reply["ok"], reply)
        self.assertEqual(reply["risk"], "medium")
        VerifyKey(me).verify(message, base64.b64decode(reply["signature"]))

        me, message, req = self.sign(RECIPIENT, 500_000_000)
        s = sim.wait_screen(lambda s: s["top"].strip() == "SEND 0.50 SOL" and s["rgb"] == GREEN)
        sim.press("h")
        reply = sim.dev.wait(req)
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
        self.assertEqual(reply["error"], "rejected")
        self.assertEqual(reply["reason"], "NEW+OVER LIMIT")

    def test_4_encrypted_channel(self):
        from nacl.signing import VerifyKey
        sim = self.sim
        address = sim.dev.request("get_pubkey")["pubkey"]
        sim.dev.handshake(expected_pubkey=address)
        self.assertTrue(sim.dev.request("ping")["secure_channel"])
        me, message, req = self.sign(RECIPIENT, 10_000_000)
        sim.wait_screen(lambda s: s["top"].strip() == "SEND 0.01 SOL")
        sim.press("h")
        reply = sim.dev.wait(req)
        self.assertTrue(reply["ok"], reply)
        VerifyKey(me).verify(message, base64.b64decode(reply["signature"]))
        sim.dev.secure = None
        sim.dev.handshake(expected_pubkey=address)  # re-keying works
        sim.dev.secure = None

    def test_5_framing_errors_keep_the_link_usable(self):
        dev = self.sim.dev
        dev.send_line("not json")
        self.assertEqual(dev.read_message(5)["error"], "invalid_json")
        dev.send_line("x" * 9000)
        self.assertEqual(dev.read_message(5)["error"], "line_too_long")
        self.assertTrue(dev.request("ping")["pong"])

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


if __name__ == "__main__":
    BINARY = os.path.abspath(sys.argv.pop(1)) if len(sys.argv) > 1 else "build/wallet-fw"
    unittest.main(verbosity=2)
