#pragma once
// Reference data generated independently with @solana/web3.js 1.95.3,
// bip39 3.1.0 and ed25519-hd-key 1.3.0 (see tests/tools/gen_vectors.js).
// Sender: the "abandon ... about" wallet at m/44'/501'/0'/0'.
// Recipient: 7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU.

namespace test {

constexpr const char* kAbandonMnemonic =
    "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about";
constexpr const char* kAbandonAddress = "HAgk14JpMQLgt6rVgv7cBQFJWFto5Dqxi472uT3DKpqk";
constexpr const char* kRecipient = "7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU";

// Transaction.add(SystemProgram.transfer(0.5 SOL)).serializeMessage()
constexpr const char* kTransferMsgHex =
    "01000103f036276246a75b9de3349ed42b15e232f6518fc20f5fcd4f1d64e81f9bd258f76752055c20b3e9d874"
    "6656ddf73855507f87ab6d87523e4c76a7fa36096a99eb00000000000000000000000000000000000000000000"
    "00000000000000000000c49ae77603782054f17a9decea43b444eba0edb12c6f1d31c6e0e4a84bf052eb010202"
    "00010c020000000065cd1d00000000";
// tx.sign(keypair) over the message above (ed25519 is deterministic).
constexpr const char* kTransferSigHex =
    "1d93bb774bdd524eadc379554fbd531547669559403b0b51f62903fb5b3a26b0fa364859ebe0546a3dfa6bcb2d"
    "24cfed97cef6771b3764b2bf8ae0a208fc1f03";

// Solana Pay: 1234567 lamports with read-only reference key appended.
constexpr const char* kSolanaPayRef = "Ref1111111111111111111111111111111111111111";
constexpr const char* kSolanaPayMsgHex =
    "01000204f036276246a75b9de3349ed42b15e232f6518fc20f5fcd4f1d64e81f9bd258f76752055c20b3e9d874"
    "6656ddf73855507f87ab6d87523e4c76a7fa36096a99eb00000000000000000000000000000000000000000000"
    "00000000000000000000065084296af91b7f8873b378c38df3c7a59c76445eedf86264a8e80000000000c49ae7"
    "7603782054f17a9decea43b444eba0edb12c6f1d31c6e0e4a84bf052eb0102030001030c0200000087d6120000"
    "000000";

// setComputeUnitLimit(300000), setComputeUnitPrice(50000), transfer 42
// lamports, memo "coffee for alice".
constexpr const char* kBudgetMemoMsgHex =
    "01000305f036276246a75b9de3349ed42b15e232f6518fc20f5fcd4f1d64e81f9bd258f76752055c20b3e9d874"
    "6656ddf73855507f87ab6d87523e4c76a7fa36096a99eb00000000000000000000000000000000000000000000"
    "000000000000000000000306466fe5211732ffecadba72c39be7bc8ce5bbc5f7126b2c439b3a40000000054a53"
    "5a992921064d24e87160da387c7c35b5ddbc92bb81e41fa8404105448dc49ae77603782054f17a9decea43b444"
    "eba0edb12c6f1d31c6e0e4a84bf052eb0403000502e09304000300090350c3000000000000020200010c020000"
    "002a00000000000000040010636f6666656520666f7220616c696365";

// TransactionMessage(...).compileToV0Message(): transfer of 2 SOL.
constexpr const char* kV0MsgHex =
    "8001000103f036276246a75b9de3349ed42b15e232f6518fc20f5fcd4f1d64e81f9bd258f76752055c20b3e9d8"
    "746656ddf73855507f87ab6d87523e4c76a7fa36096a99eb0000000000000000000000000000000000000000000"
    "000000000000000000000c49ae77603782054f17a9decea43b444eba0edb12c6f1d31c6e0e4a84bf052eb010202"
    "00010c02000000009435770000000000";

// One instruction to the SPL Token program (not recognised by the device).
constexpr const char* kUnknownMsgHex =
    "01000102f036276246a75b9de3349ed42b15e232f6518fc20f5fcd4f1d64e81f9bd258f706ddf6e1d765a193d9"
    "cbe146ceeb79ac1cb485ed5f5b37913a8cf5857eff00a9c49ae77603782054f17a9decea43b444eba0edb12c6f"
    "1d31c6e0e4a84bf052eb010101000403010203";

}  // namespace test
