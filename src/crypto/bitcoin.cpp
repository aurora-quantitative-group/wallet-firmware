#include "crypto/bitcoin.h"

#include <secp256k1.h>
#include <sodium.h>

#include <cstring>

extern "C" {
#include "ripemd160.h"
#include "segwit_addr.h"
}

namespace wfw::crypto {
namespace {

const secp256k1_context* context() {
  static secp256k1_context* ctx = secp256k1_context_create(SECP256K1_CONTEXT_NONE);
  return ctx;
}

struct Node {
  Secret<32> key;
  Secret<32> chain;
};

bool hmac512(const uint8_t* key, size_t keyLen, const uint8_t* data, size_t len, Secret<64>& out) {
  crypto_auth_hmacsha512_state st;
  crypto_auth_hmacsha512_init(&st, key, keyLen);
  crypto_auth_hmacsha512_update(&st, data, len);
  crypto_auth_hmacsha512_final(&st, out.data());
  sodium_memzero(&st, sizeof(st));
  return true;
}

bool compressedPubkey(const uint8_t key[32], uint8_t out[33]) {
  secp256k1_pubkey pub;
  if (!secp256k1_ec_pubkey_create(context(), &pub, key)) return false;
  size_t len = 33;
  return secp256k1_ec_pubkey_serialize(context(), out, &len, &pub, SECP256K1_EC_COMPRESSED) &&
         len == 33;
}

// BIP-32 CKDpriv: hardened uses 0x00 || k, normal uses serP(K).
bool child(const Node& parent, uint32_t index, Node& out) {
  Secret<37> data;
  if (index & kHardened) {
    data[0] = 0;
    std::memcpy(data.data() + 1, parent.key.data(), 32);
  } else if (!compressedPubkey(parent.key.data(), data.data())) {
    return false;
  }
  data[33] = static_cast<uint8_t>(index >> 24);
  data[34] = static_cast<uint8_t>(index >> 16);
  data[35] = static_cast<uint8_t>(index >> 8);
  data[36] = static_cast<uint8_t>(index);
  Secret<64> i;
  hmac512(parent.chain.data(), 32, data.data(), data.size(), i);
  out.key.copyFrom(parent.key);
  // k_i = parse256(IL) + k_par (mod n); fails for IL >= n or a zero key.
  if (!secp256k1_ec_seckey_tweak_add(context(), out.key.data(), i.data())) return false;
  out.chain.copyFrom(i.data() + 32);
  return true;
}

}  // namespace

bool bip84Address(const uint8_t* seed, size_t seedLen, uint32_t account, std::string& out) {
  static const char kKey[] = "Bitcoin seed";
  Secret<64> i;
  hmac512(reinterpret_cast<const uint8_t*>(kKey), sizeof(kKey) - 1, seed, seedLen, i);
  Node node;
  node.key.copyFrom(i.data());
  node.chain.copyFrom(i.data() + 32);
  if (!secp256k1_ec_seckey_verify(context(), node.key.data())) return false;

  const uint32_t path[] = {84 | kHardened, 0 | kHardened, account | kHardened, 0, 0};
  for (const uint32_t index : path) {
    Node next;
    if (!child(node, index, next)) return false;
    node.key.copyFrom(next.key);
    node.chain.copyFrom(next.chain);
  }
  uint8_t pub[33];
  if (!compressedPubkey(node.key.data(), pub)) return false;
  uint8_t sha[crypto_hash_sha256_BYTES];
  crypto_hash_sha256(sha, pub, sizeof(pub));
  uint8_t hash160[RIPEMD160_DIGEST_LENGTH];
  ripemd160(sha, sizeof(sha), hash160);
  char address[73 + 3] = {0};
  if (!segwit_addr_encode(address, "bc", 0, hash160, sizeof(hash160))) return false;
  out = address;
  return true;
}

bool bip84Address(const Mnemonic& m, uint32_t account, std::string& out) {
  Secret<kBip39SeedBytes> seed;
  mnemonicToSeed(m, "", seed);
  return bip84Address(seed.data(), seed.size(), account, out);
}

}  // namespace wfw::crypto
