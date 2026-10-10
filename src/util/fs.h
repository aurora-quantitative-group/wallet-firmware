#pragma once

#include <string>

namespace wfw::fs {

// Reads a whole file. Returns false (with errno text in err) on failure;
// a missing file sets `missing` so callers can tell "absent" from "broken".
bool readFile(const std::string& path, std::string& out, std::string& err, bool* missing = nullptr);

// Crash-safe replace: write to a temp file in the same directory with the
// given mode, fsync it, rename over the target, then fsync the directory.
bool writeFileAtomic(const std::string& path, const std::string& data, unsigned mode,
                     std::string& err);

// Creates the directory (and parents) if missing; applies mode to the leaf.
bool ensureDir(const std::string& path, unsigned mode, std::string& err);

bool removeFile(const std::string& path, std::string& err);

std::string joinPath(const std::string& dir, const std::string& name);

}  // namespace wfw::fs
