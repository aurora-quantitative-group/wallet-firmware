#!/usr/bin/env python3
"""Drive an Aurora device (or the simulator) from a terminal.

Lets the firmware be exercised end to end without the desktop app,
including build manual Step 6 (a real devnet transfer signed on the Pi):

  wallet_cli.py --port /dev/ttyACM0 ping    # verified identity + wallets
  wallet_cli.py --port COM5 setup           # words and their check happen on Aurora
  wallet_cli.py --port COM5 pair            # approve this computer on Aurora
  wallet_cli.py --port tcp://127.0.0.1:7777 ping   # wallet-fw --tcp-listen (dev)
  wallet_cli.py --port COM5 airdrop 1
  wallet_cli.py --port COM5 send --to <ADDRESS> --sol 0.5
  wallet_cli.py --port COM5 send --to <SHOWN> --sol 2 --really-to <OTHER>   # demo toggle

Add --secure to run every command over the encrypted channel.
"""
import argparse
import base64
import json
import sys

import wfw_host as host


def show(reply):
    print(json.dumps(reply, indent=2))


def cmd_setup(dev, args):
    print("On Aurora: write down the 12 words, hold the right button on word 12,")
    print("then pick 3 of the words when asked. The phrase never leaves the device.")
    reply = dev.request("setup", timeout=args.timeout)
    print("Wallet created and backup checked. Solana address:", reply["pubkey"])


def cmd_pair(dev, args):
    code = "%04d" % (int.from_bytes(host.os.urandom(2), "big") % 10000)
    print("Aurora shows PAIR CODE %s. Check it matches, then hold the right button." % code)
    payload = dev.pair(code, timeout=args.timeout)
    print("Paired with", payload["device_id"])


def cmd_send(dev, args):
    dev.identity()  # verify who we are talking to before asking for a signature
    me = dev.request("get_pubkey")["pubkey"]
    shown = args.to
    actual = args.really_to or args.to
    if args.really_to:
        print("DEMO TOGGLE: the app would show %s but the transaction pays %s" % (shown, actual))
    lamports = round(args.sol * host.LAMPORTS_PER_SOL)
    rpc = host.DevnetRpc(args.rpc)
    blockhash = rpc.latest_blockhash() if not args.offline else bytes(32)
    message = host.transfer_message(host.b58decode(me), host.b58decode(actual), lamports, blockhash)
    print("Check the device screen, then hold the right button to sign (left rejects).")
    reply = dev.request("sign_tx", timeout=args.timeout, check=False, address=me,
                        message=host.b64(message), risk=args.risk)
    show(reply)
    if "error" in reply or args.offline:
        return
    signature = host.b58encode(base64.b64decode(reply["signature"]))
    rpc.send(host.wire_transaction(message, base64.b64decode(reply["signature"])))
    print("Submitted:", signature)
    rpc.confirm(signature)
    print("Confirmed: https://explorer.solana.com/tx/%s?cluster=devnet" % signature)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", required=True,
                   help="serial port (COM5, /dev/ttyACM0, ./wallet-fw.pty) or tcp://host:port")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--secure", action="store_true", help="use the encrypted channel")
    p.add_argument("--rpc", default="https://api.devnet.solana.com")
    p.add_argument("--timeout", type=float, default=600.0, help="seconds to wait for the user")
    sub = p.add_subparsers(dest="command", required=True)
    for name in ("ping", "pubkey", "show-seed", "cancel", "setup", "pair"):
        sub.add_parser(name)
    sub.add_parser("balance")
    ad = sub.add_parser("airdrop")
    ad.add_argument("sol", type=float)
    sd = sub.add_parser("send")
    sd.add_argument("--to", required=True)
    sd.add_argument("--sol", type=float, required=True)
    sd.add_argument("--risk", default="low", choices=["low", "medium", "high"],
                    help="risk level the app claims (the device may raise it)")
    sd.add_argument("--really-to", help="demo toggle: pay this address instead of --to")
    sd.add_argument("--offline", action="store_true", help="sign only; do not submit")
    args = p.parse_args()

    dev = host.Device(args.port, args.baud)
    try:
        if args.secure:
            dev.handshake()
        if args.command == "ping":
            show(dev.identity())
        elif args.command == "pubkey":
            show(dev.request("get_pubkey"))
        elif args.command == "show-seed":
            show(dev.request("show_seed", timeout=args.timeout))
        elif args.command == "cancel":
            show(dev.request("cancel"))
        elif args.command == "setup":
            cmd_setup(dev, args)
        elif args.command == "pair":
            cmd_pair(dev, args)
        elif args.command == "balance":
            address = dev.request("get_pubkey")["pubkey"]
            print("%s: %.9f SOL" % (address, host.DevnetRpc(args.rpc).balance(address) / host.LAMPORTS_PER_SOL))
        elif args.command == "airdrop":
            address = dev.request("get_pubkey")["pubkey"]
            rpc = host.DevnetRpc(args.rpc)
            sig = rpc.call("requestAirdrop", address, round(args.sol * host.LAMPORTS_PER_SOL))
            rpc.confirm(sig)
            print("Airdrop confirmed:", sig)
        elif args.command == "send":
            cmd_send(dev, args)
    except host.DeviceError as e:
        show(e.reply)
        return 1
    finally:
        dev.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
