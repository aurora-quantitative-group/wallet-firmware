#pragma once

#include <stdlib.h>
#include <unistd.h>

#include <cstdio>
#include <string>

namespace test {

// mkdtemp directory removed (shallowly) on destruction.
struct TempDir {
  std::string path;
  TempDir() {
    char tmpl[] = "/tmp/wfw-test-XXXXXX";
    const char* p = ::mkdtemp(tmpl);
    path = p ? p : "/tmp";
  }
  ~TempDir() {
    for (const char* name : {"wallet.json", "wallet.json.tmp", "recipients.json",
                             "recipients.json.tmp"}) {
      std::remove((path + "/" + name).c_str());
    }
    ::rmdir(path.c_str());
  }
  TempDir(const TempDir&) = delete;
  TempDir& operator=(const TempDir&) = delete;
};

}  // namespace test
