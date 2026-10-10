#include "util/fs.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace wfw::fs {
namespace {

std::string errnoText(const char* what, const std::string& path) {
  return std::string(what) + " " + path + ": " + std::strerror(errno);
}

std::string dirName(const std::string& path) {
  const auto slash = path.find_last_of('/');
  if (slash == std::string::npos) return ".";
  if (slash == 0) return "/";
  return path.substr(0, slash);
}

}  // namespace

std::string joinPath(const std::string& dir, const std::string& name) {
  if (dir.empty()) return name;
  if (dir.back() == '/') return dir + name;
  return dir + "/" + name;
}

bool readFile(const std::string& path, std::string& out, std::string& err, bool* missing) {
  if (missing) *missing = false;
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    if (errno == ENOENT && missing) *missing = true;
    err = errnoText("open", path);
    return false;
  }
  out.clear();
  char buf[4096];
  for (;;) {
    const ssize_t n = ::read(fd, buf, sizeof(buf));
    if (n < 0) {
      if (errno == EINTR) continue;
      err = errnoText("read", path);
      ::close(fd);
      return false;
    }
    if (n == 0) break;
    out.append(buf, static_cast<size_t>(n));
    if (out.size() > (16u << 20)) {
      err = "file too large: " + path;
      ::close(fd);
      return false;
    }
  }
  ::close(fd);
  return true;
}

bool writeFileAtomic(const std::string& path, const std::string& data, unsigned mode,
                     std::string& err) {
  const std::string tmp = path + ".tmp";
  const int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
  if (fd < 0) {
    err = errnoText("open", tmp);
    return false;
  }
  // O_CREAT honours the umask; force the exact mode (secrets must be 0600).
  ::fchmod(fd, mode);
  size_t off = 0;
  while (off < data.size()) {
    const ssize_t n = ::write(fd, data.data() + off, data.size() - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      err = errnoText("write", tmp);
      ::close(fd);
      ::unlink(tmp.c_str());
      return false;
    }
    off += static_cast<size_t>(n);
  }
  if (::fsync(fd) != 0) {
    err = errnoText("fsync", tmp);
    ::close(fd);
    ::unlink(tmp.c_str());
    return false;
  }
  ::close(fd);
  if (::rename(tmp.c_str(), path.c_str()) != 0) {
    err = errnoText("rename", tmp);
    ::unlink(tmp.c_str());
    return false;
  }
  const int dfd = ::open(dirName(path).c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (dfd >= 0) {
    ::fsync(dfd);
    ::close(dfd);
  }
  return true;
}

bool ensureDir(const std::string& path, unsigned mode, std::string& err) {
  if (path.empty()) {
    err = "empty directory path";
    return false;
  }
  std::string partial;
  size_t pos = 0;
  while (pos != std::string::npos) {
    pos = path.find('/', pos + 1);
    partial = path.substr(0, pos);
    if (partial.empty()) continue;
    if (::mkdir(partial.c_str(), pos == std::string::npos ? mode : 0755) != 0 && errno != EEXIST) {
      err = errnoText("mkdir", partial);
      return false;
    }
  }
  struct stat st {};
  if (::stat(path.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) {
    err = "not a directory: " + path;
    return false;
  }
  return true;
}

bool removeFile(const std::string& path, std::string& err) {
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    err = errnoText("unlink", path);
    return false;
  }
  return true;
}

}  // namespace wfw::fs
