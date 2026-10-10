#include <nlohmann/json.hpp>

#include "crypto/keys.h"
#include "test.h"
#include "test_vectors.h"
#include "util/fs.h"

using namespace wfw;
using namespace wfw::crypto;

namespace {

Bytes hex(const std::string& s) { return *hexDecode(s); }

std::string hexOf(const uint8_t* p, size_t n) { return hexEncode(p, n); }

}  // namespace

TEST(bip39_official_vectors) {
  std::string text;
  std::string err;
  REQUIRE(fs::readFile(std::string(WFW_TEST_VECTORS_DIR) + "/bip39_vectors.json", text, err));
  const auto doc = nlohmann::json::parse(text);
  int checked = 0;
  for (const auto& v : doc.at("english")) {
    const Bytes entropy = hex(v[0].get<std::string>());
    const std::string words = v[1].get<std::string>();
    Mnemonic m;
    REQUIRE(m.fromEntropy(entropy.data(), entropy.size()));
    CHECK_EQ(std::string(m.c_str()), words);
    Secret<kBip39SeedBytes> seed;
    mnemonicToSeed(m, "TREZOR", seed);
    CHECK_EQ(hexOf(seed.data(), seed.size()), v[2].get<std::string>());
    Mnemonic typed;
    CHECK(typed.fromText(words));
    CHECK(typed.equals(m));
    ++checked;
  }
  CHECK(checked >= 24);
}

TEST(mnemonic_from_text_normalises_and_validates) {
  Mnemonic m;
  CHECK(m.fromText(
      "  ABANDON abandon\tabandon abandon abandon abandon\n abandon abandon abandon abandon "
      "abandon About  "));
  CHECK_EQ(std::string(m.c_str()),
           std::string("abandon abandon abandon abandon abandon abandon abandon abandon abandon "
                       "abandon abandon about"));
  CHECK_EQ(m.wordCount(), size_t{12});
  char w[16];
  CHECK(m.word(11, w, sizeof(w)));
  CHECK_EQ(std::string(w), std::string("about"));
  CHECK(!m.word(12, w, sizeof(w)));
  // Bad checksum, unknown word, wrong count, non-ASCII.
  CHECK(!m.fromText("abandon abandon abandon abandon abandon abandon abandon abandon abandon "
                    "abandon abandon abandon"));
  CHECK(!m.fromText("abandon abandon abandon abandon abandon abandon abandon abandon abandon "
                    "abandon abandon zzzz"));
  CHECK(!m.fromText("abandon about"));
  CHECK(!m.fromText("abandón abandon abandon abandon abandon abandon abandon abandon abandon "
                    "abandon abandon about"));
  CHECK(!m.fromText(""));
  CHECK_EQ(m.wordCount(), size_t{0});
}

TEST(slip10_ed25519_spec_vector_1) {
  // SLIP-0010, "Test vector 1 for ed25519".
  const Bytes seed = hex("000102030405060708090a0b0c0d0e0f");
  struct Row {
    const char* path;
    const char* chain;
    const char* priv;
    const char* pub;
  };
  const Row rows[] = {
      {"m", "90046a93de5380a72b5e45010748567d5ea02bbf6522f979e05c0d8d8ca9fffb",
       "2b4be7f19ee27bbf30c667b642d5f4aa69fd169872f8fc3059c08ebae2eb19e7",
       "a4b2856bfec510abab89753fac1ac0e1112364e7d250545963f135f2a33188ed"},
      {"m/0'", "8b59aa11380b624e81507a27fedda59fea6d0b779a778918a2fd3590e16e9c69",
       "68e0fe46dfb67e368c75379acec591dad19df3cde26e63b93a8e704f1dade7a3",
       "8c8a13df77a28f3445213a0f432fde644acaa215fc72dcdf300d5efaa85d350c"},
      {"m/0'/1'", "a320425f77d1b5c2505a6b1b27382b37368ee640e3557c315416801243552f14",
       "b1d0bad404bf35da785a64ca1ac54b2617211d2777696fbffaf208f746ae84f2",
       "1932a5270f335bed617d5b935c80aedb1a35bd9fc1e31acafd5372c30f5c1187"},
      {"m/0'/1'/2'/2'/1000000000'", "68789923a0cac2cd5a29172a475fe9e0fb14cd6adb5ad98a3fa70333e7afa230",
       "8f94d394a8e8fd6b1bc2f3f49f5c47e385281d5c17e65324b0f62483e37e8793",
       "3c24da049451555d51a7014a37337aa4e12d41e485abccfa46b47dfb2af54b7a"},
  };
  for (const auto& row : rows) {
    std::vector<uint32_t> path;
    REQUIRE(parsePath(row.path, path));
    Slip10Node node;
    REQUIRE(slip10Derive(seed.data(), seed.size(), path, node));
    CHECK_EQ(hexOf(node.chainCode.data(), 32), std::string(row.chain));
    CHECK_EQ(hexOf(node.key.data(), 32), std::string(row.priv));
    KeyPair kp;
    keyPairFromSeed(node.key.data(), kp);
    CHECK_EQ(hexOf(kp.publicKey.data(), 32), std::string(row.pub));
  }
}

TEST(slip10_rejects_non_hardened) {
  std::vector<uint32_t> path;
  CHECK(parsePath("m/44'/501'/0'/0'", path));
  CHECK_EQ(path.size(), size_t{4});
  CHECK_EQ(path[1], 501u | kHardened);
  CHECK(parsePath("m", path) && path.empty());
  CHECK(!parsePath("m/44", path));
  CHECK(!parsePath("44'/0'", path));
  CHECK(!parsePath("m/'", path));
  CHECK(!parsePath("m/2147483648'", path));
  Slip10Node parent;
  Slip10Node child;
  CHECK(!slip10Child(parent, 0, child));
}

TEST(solana_derivation_matches_phantom) {
  // Cross-checked with bip39 + ed25519-hd-key + @solana/web3.js (Keypair.fromSeed).
  Mnemonic m;
  REQUIRE(m.fromText(test::kAbandonMnemonic));
  KeyPair kp;
  deriveSolanaKeyPair(m, kp);
  CHECK_EQ(base58Encode(kp.publicKey), std::string(test::kAbandonAddress));
}

TEST(sign_matches_web3js_and_verifies) {
  Mnemonic m;
  REQUIRE(m.fromText(test::kAbandonMnemonic));
  KeyPair kp;
  deriveSolanaKeyPair(m, kp);
  const Bytes msg = hex(test::kTransferMsgHex);
  uint8_t sig[kSignatureBytes];
  signDetached(kp, msg.data(), msg.size(), sig);
  CHECK_EQ(hexOf(sig, sizeof(sig)), std::string(test::kTransferSigHex));
  CHECK(verifyDetached(kp.publicKey, msg.data(), msg.size(), sig));
  Bytes tampered = msg;
  tampered.back() ^= 1;
  CHECK(!verifyDetached(kp.publicKey, tampered.data(), tampered.size(), sig));
}
