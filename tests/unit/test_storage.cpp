#include <sodium.h>
#include <sys/stat.h>

#include <cstdlib>

#include "config.h"
#include "storage/keystore.h"
#include "storage/recipients.h"
#include "test.h"
#include "test_util.h"
#include "util/fs.h"

using namespace wfw;
using namespace wfw::storage;

TEST(keystore_plain_create_reload) {
  test::TempDir dir;
  uint8_t entropy[16] = {0};  // "abandon ... about"
  std::string err;
  {
    Keystore ks(dir.path);
    REQUIRE(ks.load(err));
    CHECK(ks.status() == Keystore::Status::Empty);
    REQUIRE(ks.create(entropy, "", err));
    CHECK(ks.ready());
    CHECK_EQ(ks.address(), std::string("HAgk14JpMQLgt6rVgv7cBQFJWFto5Dqxi472uT3DKpqk"));
    CHECK(!ks.create(entropy, "", err));  // already initialized
  }
  struct stat st {};
  REQUIRE(::stat(fs::joinPath(dir.path, "wallet.json").c_str(), &st) == 0);
  CHECK_EQ(st.st_mode & 0777, 0600u);

  Keystore again(dir.path);
  REQUIRE(again.load(err));
  CHECK(again.ready());
  CHECK(!again.pinProtected());
  CHECK(!again.backupConfirmed());
  REQUIRE(again.setBackupConfirmed(err));
  crypto::Mnemonic m;
  REQUIRE(again.mnemonic(m));
  CHECK_EQ(m.wordCount(), size_t{12});

  Keystore third(dir.path);
  REQUIRE(third.load(err));
  CHECK(third.backupConfirmed());
  REQUIRE(third.wipe(err));
  CHECK(third.status() == Keystore::Status::Empty);
}

TEST(keystore_pin_encrypts_and_unlocks) {
  test::TempDir dir;
  uint8_t entropy[16];
  randombytes_buf(entropy, sizeof(entropy));
  std::string err;
  std::string address;
  {
    Keystore ks(dir.path);
    ks.setKdfLimits(crypto_pwhash_OPSLIMIT_MIN, crypto_pwhash_MEMLIMIT_MIN);
    REQUIRE(ks.load(err));
    REQUIRE(ks.create(entropy, "4321", err));
    address = ks.address();
  }
  std::string onDisk;
  REQUIRE(fs::readFile(fs::joinPath(dir.path, "wallet.json"), onDisk, err));
  CHECK(onDisk.find(hexEncode(entropy, sizeof(entropy))) == std::string::npos);

  Keystore ks(dir.path);
  REQUIRE(ks.load(err));
  CHECK(ks.status() == Keystore::Status::Locked);
  CHECK(ks.pinProtected());
  CHECK_EQ(ks.address(), address);
  CHECK(ks.keyPair() == nullptr);
  CHECK(!ks.unlock("1234", err));
  CHECK_EQ(err, std::string("wrong PIN"));
  REQUIRE(ks.unlock("4321", err));
  CHECK(ks.ready());
  REQUIRE(ks.keyPair() != nullptr);
  CHECK(ks.keyPair()->publicKey == ks.publicKey());
  // Metadata changes do not need the PIN and keep the ciphertext.
  ks.lock();
  CHECK(ks.status() == Keystore::Status::Locked);
  REQUIRE(ks.setBackupConfirmed(err));
  Keystore reloaded(dir.path);
  REQUIRE(reloaded.load(err));
  CHECK(reloaded.backupConfirmed());
  REQUIRE(reloaded.unlock("4321", err));
}

TEST(keystore_detects_tampering) {
  test::TempDir dir;
  uint8_t entropy[16] = {0};
  std::string err;
  {
    Keystore ks(dir.path);
    ks.load(err);
    REQUIRE(ks.create(entropy, "", err));
  }
  const std::string path = fs::joinPath(dir.path, "wallet.json");
  std::string text;
  REQUIRE(fs::readFile(path, text, err));
  const auto pos = text.find("HAgk14");
  REQUIRE(pos != std::string::npos);
  text.replace(pos, 6, "HAgk15");
  REQUIRE(fs::writeFileAtomic(path, text, 0600, err));
  Keystore ks(dir.path);
  CHECK(!ks.load(err));
  CHECK(ks.status() == Keystore::Status::Error);

  REQUIRE(fs::writeFileAtomic(path, "{not json", 0600, err));
  CHECK(!ks.load(err));
  CHECK(ks.status() == Keystore::Status::Error);
}

TEST(known_recipients_persist) {
  test::TempDir dir;
  std::string err;
  PubKey a{};
  a.fill(7);
  PubKey b{};
  b.fill(9);
  {
    KnownRecipients r(dir.path);
    REQUIRE(r.load(err));
    CHECK_EQ(r.size(), size_t{0});
    REQUIRE(r.addAll({a, b, a}, err));
    CHECK_EQ(r.size(), size_t{2});
  }
  KnownRecipients r(dir.path);
  REQUIRE(r.load(err));
  CHECK(r.contains(a) && r.contains(b));
  REQUIRE(r.clear(err));
  CHECK(!r.contains(a));
}

TEST(config_parsing_and_validation) {
  Config cfg;
  std::string err;
  REQUIRE(applyConfigJson(R"({"serial_device": "/dev/serial0", "hold_confirm_ms": 1500,
      "risk_limit_lamports": 250000000, "pin_enabled": true, "unknown_key": 1})", cfg, err));
  CHECK_EQ(cfg.serialDevice, std::string("/dev/serial0"));
  CHECK_EQ(cfg.holdConfirmMs, 1500);
  CHECK_EQ(cfg.riskLimitLamports, uint64_t{250000000});
  CHECK(cfg.pinEnabled);
  CHECK(!applyConfigJson(R"({"debounce_ms": 1})", cfg, err));
  CHECK(!applyConfigJson(R"({"gpio_left": 22})", cfg, err));  // clashes with right
  CHECK(!applyConfigJson(R"({"pin_enabled": "yes"})", cfg, err));
  CHECK(!applyConfigJson("[1,2]", cfg, err));
  CHECK_EQ(cfg.holdConfirmMs, 1500);  // failed loads leave cfg untouched
  CHECK(loadConfigFile("/nonexistent/config.json", cfg, true, err));
  CHECK(!loadConfigFile("/nonexistent/config.json", cfg, false, err));
}
