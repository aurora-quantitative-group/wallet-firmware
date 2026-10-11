cmake_minimum_required(VERSION 3.16)
project(wallet_fw VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 99)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

option(WFW_HARDWARE "Build the Raspberry Pi drivers (I2C LCD, libgpiod buttons and buzzer)" ON)
option(WFW_BUILD_TESTS "Build the unit tests" ON)
option(WFW_SANITIZE "Build with AddressSanitizer and UBSan" OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(SODIUM REQUIRED IMPORTED_TARGET libsodium>=1.0.18)
# Bitcoin Core's secp256k1, for BIP-84 address derivation (libsecp256k1-dev).
pkg_check_modules(SECP256K1 REQUIRED IMPORTED_TARGET libsecp256k1>=0.2.0)
include(cmake/ThirdParty.cmake)

if(WFW_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# --- trezor-crypto (BIP-39, RIPEMD-160, bech32), fetched by cmake/ThirdParty.cmake ---
add_library(trezor_crypto STATIC
  third_party/trezor-crypto/bip39.c
  third_party/trezor-crypto/bip39_english.c
  third_party/trezor-crypto/consteq.c
  third_party/trezor-crypto/hmac.c
  third_party/trezor-crypto/memzero.c
  third_party/trezor-crypto/pbkdf2.c
  third_party/trezor-crypto/ripemd160.c
  third_party/trezor-crypto/segwit_addr.c
  third_party/trezor-crypto/sha2.c
  src/crypto/trezor_platform.c)
target_include_directories(trezor_crypto SYSTEM PUBLIC third_party/trezor-crypto)
# The BIP-39 cache keeps mnemonics and seeds in static memory: keep it off.
target_compile_definitions(trezor_crypto PUBLIC USE_BIP39_CACHE=0 USE_BIP32_CACHE=0)
set_target_properties(trezor_crypto PROPERTIES POSITION_INDEPENDENT_CODE ON)

set(WFW_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wno-sign-conversion)

# --- Hardware-independent core: protocol, flows, crypto, storage, parsing ---
add_library(wfw_core STATIC
  src/util/clock.cpp
  src/util/log.cpp
  src/util/fs.cpp
  src/crypto/encoding.cpp
  src/crypto/keys.cpp
  src/crypto/bitcoin.cpp
  src/crypto/hkdf.cpp
  src/crypto/secure_channel.cpp
  src/solana/message.cpp
  src/solana/analyze.cpp
  src/solana/format.cpp
  src/policy/risk.cpp
  src/storage/keystore.cpp
  src/storage/recipients.cpp
  src/storage/identity.cpp
  src/config.cpp
  src/ui/button_input.cpp
  src/ui/screen.cpp
  src/ui/buzzer.cpp
  src/device/line_framer.cpp
  src/device/controller.cpp
  src/device/controller_commands.cpp
  src/device/controller_setup.cpp
  src/device/controller_sign.cpp)
target_include_directories(wfw_core PUBLIC src)
target_link_libraries(wfw_core PUBLIC PkgConfig::SODIUM PkgConfig::SECP256K1 trezor_crypto
  nlohmann_json::nlohmann_json)
target_compile_options(wfw_core PRIVATE ${WFW_WARNINGS})
target_compile_definitions(wfw_core PUBLIC WFW_VERSION="${PROJECT_VERSION}")

# --- Linux HAL: serial (tty / pty), simulator, systemd notify ---
add_library(wfw_hal STATIC
  src/hal/linux/tty_serial.cpp
  src/hal/linux/sd_notify.cpp
  src/hal/sim/sim_hal.cpp)
target_link_libraries(wfw_hal PUBLIC wfw_core util)
target_compile_options(wfw_hal PRIVATE ${WFW_WARNINGS})

if(WFW_HARDWARE)
  pkg_check_modules(GPIOD REQUIRED IMPORTED_TARGET libgpiod)
  target_sources(wfw_hal PRIVATE
    src/hal/linux/i2c_bus.cpp
    src/hal/linux/grove_lcd.cpp
    src/hal/linux/gpiod_io.cpp
    src/hw_test.cpp)
  target_link_libraries(wfw_hal PUBLIC PkgConfig::GPIOD)
  if(GPIOD_VERSION VERSION_GREATER_EQUAL 2)
    target_compile_definitions(wfw_hal PRIVATE WFW_GPIOD_V2=1)
  endif()
  target_compile_definitions(wfw_hal PUBLIC WFW_HARDWARE=1)
  message(STATUS "Hardware drivers enabled (libgpiod ${GPIOD_VERSION})")
endif()

add_executable(wallet-fw src/main.cpp)
target_link_libraries(wallet-fw PRIVATE wfw_hal)
target_compile_options(wallet-fw PRIVATE ${WFW_WARNINGS})

install(TARGETS wallet-fw RUNTIME DESTINATION bin)

if(WFW_BUILD_TESTS)
  enable_testing()
  add_executable(wfw_tests
    tests/unit/test_main.cpp
    tests/unit/test_encoding.cpp
    tests/unit/test_keys.cpp
    tests/unit/test_hkdf_channel.cpp
    tests/unit/test_message.cpp
    tests/unit/test_format_risk.cpp
    tests/unit/test_button_input.cpp
    tests/unit/test_framer.cpp
    tests/unit/test_storage.cpp
    tests/unit/test_controller.cpp
    tests/unit/test_controller_sign.cpp
    tests/unit/test_controller_secure.cpp
    tests/unit/test_controller_review.cpp
    tests/unit/test_protocol_v2.cpp)
  target_link_libraries(wfw_tests PRIVATE wfw_core)
  target_compile_definitions(wfw_tests PRIVATE
    WFW_TEST_VECTORS_DIR="${CMAKE_CURRENT_SOURCE_DIR}/third_party/vectors")
  add_test(NAME unit COMMAND wfw_tests)
endif()
