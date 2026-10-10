#pragma once

#include <deque>
#include <set>
#include <string>
#include <vector>

#include "crypto/encoding.h"

namespace wfw::storage {

// Every recipient the device has signed a transfer to (build manual Step 7:
// "the Pi stores every recipient it has signed for"). Persisted to
// <data_dir>/recipients.json. Oldest entries are dropped past kMaxEntries.
class KnownRecipients {
 public:
  static constexpr size_t kMaxEntries = 4096;

  explicit KnownRecipients(std::string dataDir);

  bool load(std::string& err);  // missing file = empty list
  bool contains(const PubKey& key) const { return set_.count(key) != 0; }
  // Adds any new keys and persists if something changed.
  bool addAll(const std::vector<PubKey>& keys, std::string& err);
  bool clear(std::string& err);
  size_t size() const { return order_.size(); }

 private:
  bool persist(std::string& err);

  std::string path_;
  std::deque<PubKey> order_;
  std::set<PubKey> set_;
};

}  // namespace wfw::storage
