// Regenerates the reference data in tests/unit/test_vectors.h with the
// libraries the app uses, as an independent check of the firmware.
//
//   npm install @solana/web3.js@1.95.3 bip39@3.1.0 ed25519-hd-key@1.3.0
//   node tests/tools/gen_vectors.js
// (On Node 18, add "overrides": {"uuid": "8.3.2"} to package.json.)
const w3 = require('@solana/web3.js');
const bip39 = require('bip39');
const { derivePath } = require('ed25519-hd-key');

const mnemonic = 'abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about';
const seed = bip39.mnemonicToSeedSync(mnemonic, '');
const kp = w3.Keypair.fromSeed(derivePath("m/44'/501'/0'/0'", seed.toString('hex')).key);
const to = new w3.PublicKey('7xKXtg2CW87d97TXJSDpbD5jBkheTqA83TZRuJosgAsU');
const recentBlockhash = 'EETubP5AKHgjPAhzPAFcb8BAY1hMH639CWCFTqi3hq1k';
const legacy = () => new w3.Transaction({ recentBlockhash, feePayer: kp.publicKey });
const transfer = (lamports) =>
  w3.SystemProgram.transfer({ fromPubkey: kp.publicKey, toPubkey: to, lamports });

console.log('phantom_address', kp.publicKey.toBase58());

let tx = legacy().add(transfer(500000000));
console.log('transfer_msg', tx.serializeMessage().toString('hex'));
tx.sign(kp);
console.log('transfer_sig', Buffer.from(tx.signature).toString('hex'));

const ref = new w3.PublicKey('Ref1111111111111111111111111111111111111111');
const payIx = transfer(1234567);
payIx.keys.push({ pubkey: ref, isWritable: false, isSigner: false });
console.log('solanapay_msg', legacy().add(payIx).serializeMessage().toString('hex'));

tx = legacy()
  .add(w3.ComputeBudgetProgram.setComputeUnitLimit({ units: 300000 }))
  .add(w3.ComputeBudgetProgram.setComputeUnitPrice({ microLamports: 50000 }))
  .add(transfer(42))
  .add(new w3.TransactionInstruction({
    programId: new w3.PublicKey('MemoSq4gqABAXKb96qnH8TysNcWxMyWCqXgDLGmfcHr'),
    keys: [],
    data: Buffer.from('coffee for alice'),
  }));
console.log('budget_memo_msg', tx.serializeMessage().toString('hex'));

const v0 = new w3.TransactionMessage({
  payerKey: kp.publicKey, recentBlockhash, instructions: [transfer(2000000000)],
}).compileToV0Message();
console.log('v0_msg', Buffer.from(v0.serialize()).toString('hex'));

tx = legacy().add(new w3.TransactionInstruction({
  programId: new w3.PublicKey('TokenkegQfeZyiNwAJbNbGKPFXCWuBvf9Ss623VQ5DA'),
  keys: [{ pubkey: kp.publicKey, isSigner: true, isWritable: true }],
  data: Buffer.from([3, 1, 2, 3]),
}));
console.log('unknown_msg', tx.serializeMessage().toString('hex'));
