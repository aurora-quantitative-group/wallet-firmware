#!/usr/bin/env python3
"""Drive a wallet-fw device (or the simulator) from a terminal.

Lets the firmware be exercised end to end without the desktop app,
including build manual Step 6 (a real devnet transfer signed on the Pi):

  wallet_cli.py --port /dev/ttyACM0 ping
  wallet_cli.py --port COM5 setup           # then type the words back in
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
    print("Write down the 12 words shown on the device, then hold the right button on word 12.")
    reply = dev.request("setup", timeout=args.timeout)
    print("Wallet address:", reply["pubkey"])
    while True:
        try:
            words = input("Type the 12 words to confirm your backup (or 'back' to see them again): ")
        except EOFError:
            print("\nBackup not confirmed yet; run 'confirm-backup <12 words>' later.")
            return
        if words.strip().lower() == "back":
            dev.request("show_words", timeout=args.timeout)
            continue
        result = dev.request("confirm_backup", check=False, words=words)
        if result.get("ok"):
            print("Backup confirmed.")
            return
        print("Those words do not match (%s). Try again." % result.get("error"))


def cmd_send(dev, args):
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
    reply = dev.request("sign_tx", timeout=args.timeout, check=False,
                        message=host.b64(message), risk=args.risk)
    show(reply)
    if not reply.get("ok") or args.offline:
        return
    signature = host.b58encode(base64.b64decode(reply["signature"]))
    rpc.send(host.wire_transaction(message, base64.b64decode(reply["signature"])))
    print("Submitted:", signature)
    rpc.confirm(signature)
    print("Confirmed: https://explorer.solana.com/tx/%s?cluster=devnet" % signature)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--port", required=True, help="serial port (COM5, /dev/ttyACM0, ./wallet-fw.pty)")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--secure", action="store_true", help="use the encrypted channel")
    p.add_argument("--rpc", default="https://api.devnet.solana.com")
    p.add_argument("--timeout", type=float, default=600.0, help="seconds to wait for the user")
    sub = p.add_subparsers(dest="command", required=True)
    for name in ("ping", "pubkey", "show-words", "cancel"):
        sub.add_parser(name)
    sub.add_parser("setup")
    cb = sub.add_parser("confirm-backup")
    cb.add_argument("words", nargs=12)
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
            show(dev.request("ping"))
        elif args.command == "pubkey":
            show(dev.request("get_pubkey"))
        elif args.command == "show-words":
            show(dev.request("show_words", timeout=args.timeout))
        elif args.command == "cancel":
            show(dev.request("cancel"))
        elif args.command == "setup":
            cmd_setup(dev, args)
        elif args.command == "confirm-backup":
            show(dev.request("confirm_backup", check=False, words=" ".join(args.words)))
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
