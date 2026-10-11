#include "controller_harness.h"
#include "test.h"

using namespace test;

TEST(receive_addresses_are_signed_unique_and_survive_restart) {
  Harness h;
  h.preinit();
  h.boot();
  auto receive = [&](const char* wallet) {
    return h.send({{"cmd", "receive_address"}, {"wallet_id", wallet},
                   {"minimum_index", 1}, {"challenge", "receive-test-challenge"}});
  };
  const auto first = receive("btc-0");
  REQUIRE(first["ok"] == true);
  const auto body = json::parse(first["payload"].get<std::string>());
  CHECK_EQ(body["index"], 1);
  CHECK_EQ(body["path"], "m/84'/0'/0'/0/1");
  CHECK_EQ(body["root_address"], h.keystore->bitcoinAddress());
  CHECK(body["address"] != body["root_address"]);
  const auto signature = wfw::base64Decode(first["signature"].get<std::string>());
  REQUIRE(signature && signature->size() == 64);
  const std::string text = first["payload"];
  CHECK(wfw::crypto::verifyDetached(h.identity.keyPair().publicKey,
       reinterpret_cast<const uint8_t*>(text.data()), text.size(), signature->data()));
  h.boot();
  const auto second = json::parse(receive("btc-0")["payload"].get<std::string>());
  CHECK_EQ(second["index"], 2);
  CHECK(second["address"] != body["address"]);
  const auto sol = json::parse(receive("sol-0")["payload"].get<std::string>());
  CHECK_EQ(sol["index"], 1);
  CHECK_EQ(sol["path"], "m/44'/501'/0'/1'");
  CHECK(sol["address"] != h.keystore->address());
  const auto dev = json::parse(receive("sol-dev")["payload"].get<std::string>());
  CHECK_EQ(dev["index"], 2);
  CHECK(dev["address"] != sol["address"]);
  wfw::crypto::KeyPair key;
  REQUIRE(h.keystore->solanaKey(1, key));
  CHECK_EQ(wfw::base58Encode(key.publicKey), sol["address"].get<std::string>());
  CHECK(!h.keystore->solanaKey(3, key));
}

TEST(receive_address_rejects_locked_invalid_and_exhausted_requests) {
  Harness h;
  h.preinit(0, "1234");
  h.boot();
  json req = {{"cmd", "receive_address"}, {"wallet_id", "btc-0"},
              {"minimum_index", 1}, {"challenge", "receive-test-challenge"}};
  CHECK_EQ(h.send(req)["error"], "locked");
  std::string why;
  REQUIRE(h.keystore->unlock("1234", why));
  req["minimum_index"] = -1;
  CHECK_EQ(h.send(req)["error"], "invalid_request");
  req["minimum_index"] = 2147483648ULL;
  CHECK_EQ(h.send(req)["error"], "invalid_request");
  req["minimum_index"] = 1;
  req["wallet_id"] = "attacker";
  CHECK_EQ(h.send(req)["error"], "wrong_wallet");
  req["wallet_id"] = "btc-0";
  req["challenge"] = "x";
  CHECK_EQ(h.send(req)["error"], "invalid_request");
}

TEST(derived_sol_signing_requires_matching_index_and_device_approval) {
  Harness h;
  h.preinit();
  h.boot();
  const auto receipt = h.send({{"cmd", "receive_address"}, {"wallet_id", "sol-dev"},
      {"minimum_index", 1}, {"challenge", "receive-sign-challenge"}});
  const auto body = json::parse(receipt["payload"].get<std::string>());
  wfw::crypto::KeyPair key;
  REQUIRE(h.keystore->solanaKey(1, key));
  auto message = *wfw::hexDecode(kTransferMsgHex);
  std::copy(key.publicKey.begin(), key.publicKey.end(), message.begin() + 4);
  std::string why;
  REQUIRE(h.recipients->addAll({*wfw::decodeAddress(kRecipient)}, why));
  json req = {{"cmd", "sign_tx"}, {"address", body["address"]}, {"address_index", 1},
              {"message", wfw::base64Encode(message)}};
  CHECK(h.send(req).is_null());
  CHECK(h.out.back().contains("payload"));
  const auto signedReply = h.hold(Button::Right);
  REQUIRE(signedReply["ok"] == true);
  const auto sig = wfw::base64Decode(signedReply["signature"].get<std::string>());
  REQUIRE(sig && sig->size() == 64);
  CHECK(wfw::crypto::verifyDetached(key.publicKey, message.data(), message.size(), sig->data()));
  h.run(2000);
  req["address_index"] = 2;
  CHECK_EQ(h.send(req)["error"], "wrong_wallet");
}
