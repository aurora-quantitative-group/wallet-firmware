#include <sodium.h>

#include "crypto/encoding.h"
#include "test.h"

using namespace wfw;

namespace {
Bytes bytesOf(const std::string& s) { return Bytes(s.begin(), s.end()); }
}  // namespace

TEST(base58_known_vectors) {
  CHECK_EQ(base58Encode(nullptr, 0), std::string(""));
  const uint8_t zero = 0;
  CHECK_EQ(base58Encode(&zero, 1), std::string("1"));
  const Bytes hello = bytesOf("Hello World!");
  CHECK_EQ(base58Encode(hello.data(), hello.size()), std::string("2NEpo7TZRRrLZSi2U"));
  const PubKey zeros{};
  CHECK_EQ(base58Encode(zeros), std::string("11111111111111111111111111111111"));
  const Bytes leading = {0, 0, 0x28, 0x7f, 0xb4, 0xcd};
  CHECK_EQ(base58Encode(leading.data(), leading.size()), std::string("11233QC4"));
}

TEST(base58_decode_round_trip) {
  for (int len : {1, 5, 31, 32, 33, 64}) {
    for (int i = 0; i < 50; ++i) {
      Bytes data(static_cast<size_t>(len));
      randombytes_buf(data.data(), data.size());
      if (i % 5 == 0) data[0] = 0;
      const auto decoded = base58Decode(base58Encode(data.data(), data.size()));
      REQUIRE(decoded.has_value());
      CHECK(*decoded == data);
    }
  }
}

TEST(base58_rejects_invalid) {
  CHECK(!base58Decode("0OIl").has_value());
  CHECK(!base58Decode("abc!").has_value());
  CHECK(!decodeAddress("").has_value());
  CHECK(!decodeAddress("2NEpo7TZRRrLZSi2U").has_value());  // 12 bytes, not 32
  CHECK(decodeAddress("11111111111111111111111111111111").has_value());
  const auto sys = decodeAddress("ComputeBudget111111111111111111111111111111");
  REQUIRE(sys.has_value());
  CHECK_EQ(base58Encode(*sys), std::string("ComputeBudget111111111111111111111111111111"));
}

TEST(base64_round_trip_and_strictness) {
  const Bytes data = bytesOf("any carnal pleasure.");
  CHECK_EQ(base64Encode(data), std::string("YW55IGNhcm5hbCBwbGVhc3VyZS4="));
  auto back = base64Decode("YW55IGNhcm5hbCBwbGVhc3VyZS4=");
  REQUIRE(back.has_value());
  CHECK(*back == data);
  CHECK(base64Decode("").has_value());
  CHECK(!base64Decode("YW55*").has_value());
  CHECK(!base64Decode("YW55IGNhcm5hbCBwbGVhc3VyZS4= ").has_value());
  CHECK(!base64Decode("YW5").has_value());
}

TEST(hex_round_trip) {
  const Bytes data = {0x00, 0xab, 0xff, 0x10};
  CHECK_EQ(hexEncode(data.data(), data.size()), std::string("00abff10"));
  auto back = hexDecode("00ABff10");
  REQUIRE(back.has_value());
  CHECK(*back == data);
  CHECK(!hexDecode("abc").has_value());
  CHECK(!hexDecode("zz").has_value());
}
