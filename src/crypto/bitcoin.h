#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "crypto/keys.h"

namespace wfw::crypto {

// BIP-84 native SegWit (P2WPKH) receive address at m/84'/0'/account'/0/0 on
// Bitcoin mainnet ("bc1q..."). BIP-32 over secp256k1 uses libsecp256k1;
// hash160 and bech32 come from trezor-crypto. Public information only: the
// device reports it so the app can track the balance (no BTC signing).
bool bip84Address(const uint8_t* seed, size_t seedLen, uint32_t account, std::string& out, uint32_t index = 0);
bool bip84Address(const Mnemonic& m, uint32_t account, std::string& out, uint32_t index = 0);

}  // namespace wfw::crypto
