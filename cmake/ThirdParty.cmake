# Third-party sources are not committed. They are downloaded once into
# third_party/ (gitignored) from pinned URLs and verified by SHA-256, so a
# tampered or changed upstream file fails the configure step.
#
# On the Pi, run the first configure while it still has network access
# (before the demo config disables Wi-Fi); later builds reuse the cache.

set(WFW_TP_DIR ${CMAKE_SOURCE_DIR}/third_party)

function(wfw_fetch url dest sha256)
  set(path ${WFW_TP_DIR}/${dest})
  if(EXISTS ${path})
    file(SHA256 ${path} actual)
    if(actual STREQUAL sha256)
      return()
    endif()
    message(WARNING "${dest}: hash mismatch, downloading again")
  endif()
  message(STATUS "Downloading ${dest}")
  file(DOWNLOAD ${url} ${path} EXPECTED_HASH SHA256=${sha256} STATUS st TLS_VERIFY ON)
  list(GET st 0 code)
  if(NOT code EQUAL 0)
    file(REMOVE ${path})
    message(FATAL_ERROR "Download failed for ${url}: ${st}")
  endif()
endfunction()

# trezor-crypto, BIP-39 subset, from trezor-firmware tag core/v2.12.5 (MIT).
set(_tz https://raw.githubusercontent.com/trezor/trezor-firmware/core/v2.12.5/crypto)
set(WFW_TREZOR_FILES
  LICENSE b53d2eb93806f3d618370726f27eafeb6c279c7a90351297f79e073e49e569be
  bip39.c 6baf82d7a522d2334c5d6777a9aa2452eeea1925891a14556dd15998ce546225
  bip39.h 3774828450685eae9f3c47af5c95d0a0062be6c35c73b61d194dcc9465d25c15
  bip39_english.c 7d594a31f97c7732ca5b88c1382ff83961607288c544cf3e339bbc5804eac021
  byte_order.h 496a5a80346a0fb48ef3fbf53c0da2b69399e1de224d67962342657a9dfd77d1
  consteq.c 5bfe0f3b7a79c7d4e8cdd4d052273ae8d4f2e5180a07db0bcad07076220a07c7
  consteq.h 09b70ff94f5daf83a0bfa6bb76a1c110d19456f07b61c0982a16ce2e7b7e4fec
  fault_handler.h a82155e446d326f0155a7ef6696b6e425e18cc4a59c50014e16e69ae36e2352d
  hmac.c ef44ae6fb924e2aa975e58789603e59399f9383887d3eda55e6920d23c627e24
  hmac.h 268e806610171b276d5e9bfb2fab84e825782b755eb62500c77ee812e50d4026
  memzero.c 2f014e8d03420b42fb7b9df39f9150a33ac96d1c48a065f5a908a967561bca02
  memzero.h ffec031a236dde8df1c0b95cd0c3bd28ca046f6dc62faff36be8401ba82b214a
  options.h 762e3f5cf352740183330743d52e8f7aa977797038f97a3c0d8b88ff98074b1a
  pbkdf2.c 8482559322823c26ba3676eb281f229d4b0fc9d0cf22f9271629a1b8eef89736
  pbkdf2.h 8dfb7eea8ee9e0f99ff875a16d7fec538518227fbaa71fb0b9efc9a18b4e0483
  rand.h 161ecc075038007a61fa7805b2d532bf36dabe521c3de480899eb4b8daa37c66
  sha2.c 4e0a22b05988882a1f857084fc6c5a0731a115f5637e9946f65ccd23f361e24a
  sha2.h acc5ba73f3ca604ebbaa34651d254eee7eaab808198ce60c42c015e723d9a656)
while(WFW_TREZOR_FILES)
  list(POP_FRONT WFW_TREZOR_FILES name hash)
  wfw_fetch(${_tz}/${name} trezor-crypto/${name} ${hash})
endwhile()

# nlohmann/json: prefer the distro package (nlohmann-json3-dev), else fetch.
find_package(nlohmann_json 3.9 QUIET)
if(nlohmann_json_FOUND)
  message(STATUS "Using system nlohmann_json ${nlohmann_json_VERSION}")
else()
  set(_nj https://raw.githubusercontent.com/nlohmann/json/v3.11.3)
  wfw_fetch(${_nj}/single_include/nlohmann/json.hpp nlohmann/json.hpp
    9bea4c8066ef4a1c206b2be5a36302f8926f7fdc6087af5d20b417d0cf103ea6)
  wfw_fetch(${_nj}/LICENSE.MIT nlohmann/LICENSE.MIT
    86b998c792894ccb911a1cb7994f7a9652894e7a094c0b5e45be2f553f45cf14)
  add_library(nlohmann_json INTERFACE)
  target_include_directories(nlohmann_json SYSTEM INTERFACE ${WFW_TP_DIR})
  add_library(nlohmann_json::nlohmann_json ALIAS nlohmann_json)
endif()

# Official BIP-39 test vectors (trezor/python-mnemonic), tests only.
if(WFW_BUILD_TESTS)
  wfw_fetch(
    https://raw.githubusercontent.com/trezor/python-mnemonic/b57a5ad77a981e743f4167ab2f7927a55c1e82a8/vectors.json
    vectors/bip39_vectors.json
    fa3b937b7cff9c9b8ecd3aa011faeb8d6dd67993174b72326e83f4de8fdb30f8)
endif()
