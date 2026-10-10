#include "storage/recipients.h"

#include <nlohmann/json.hpp>

#include "util/fs.h"
#include "util/log.h"

namespace wfw::storage {

using nlohmann::json;

KnownRecipients::KnownRecipients(std::string dataDir)
    : path_(fs::joinPath(dataDir, "recipients.json")) {}

bool KnownRecipients::load(std::string& err) {
  order_.clear();
  set_.clear();
  std::string text;
  bool missing = false;
  if (!fs::readFile(path_, text, err, &missing)) return missing;

  const json doc = json::parse(text, nullptr, false);
  if (doc.is_discarded() || !doc.is_object() || !doc.contains("recipients") ||
      !doc["recipients"].is_array()) {
    err = "recipients.json is malformed";
    return false;
  }
  for (const auto& item : doc["recipients"]) {
    if (!item.is_string()) continue;
    auto key = decodeAddress(item.get<std::string>());
    if (!key) {
      LOG_WARN("recipients.json: skipping invalid address");
      continue;
    }
    if (set_.insert(*key).second) order_.push_back(*key);
  }
  while (order_.size() > kMaxEntries) {
    set_.erase(order_.front());
    order_.pop_front();
  }
  return true;
}

bool KnownRecipients::addAll(const std::vector<PubKey>& keys, std::string& err) {
  bool changed = false;
  for (const auto& key : keys) {
    if (!set_.insert(key).second) continue;
    order_.push_back(key);
    changed = true;
  }
  while (order_.size() > kMaxEntries) {
    set_.erase(order_.front());
    order_.pop_front();
  }
  return changed ? persist(err) : true;
}

bool KnownRecipients::clear(std::string& err) {
  order_.clear();
  set_.clear();
  return fs::removeFile(path_, err);
}

bool KnownRecipients::persist(std::string& err) {
  json list = json::array();
  for (const auto& key : order_) list.push_back(base58Encode(key));
  const json doc = {{"version", 1}, {"recipients", list}};
  return fs::writeFileAtomic(path_, doc.dump(2) + "\n", 0600, err);
}

}  // namespace wfw::storage
