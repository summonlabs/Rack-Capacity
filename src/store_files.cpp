// Rack Capacity - operating system file, lock, process and timing primitives.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "store_files.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <random>
#include <string>
#include <system_error>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "rack_capacity/digest.hpp"

namespace rackcapacity::platform {
namespace {

[[nodiscard]] CapacityError io_error(std::string message, const std::filesystem::path& path,
                                     ErrorCode code = ErrorCode::IoFailure) {
  return make_error(code, std::move(message),
                    ErrorDetail{"io", path.filename().string(), path.parent_path().string(), 0, 0,
                                {}});
}

}  // namespace

#if defined(_WIN32)

namespace {

[[nodiscard]] std::wstring wide(const std::filesystem::path& path) { return path.wstring(); }

[[nodiscard]] ErrorCode last_error_code() noexcept {
  const DWORD code = ::GetLastError();
  switch (code) {
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
      return ErrorCode::PermissionDenied;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
      return ErrorCode::IoFailure;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
      return ErrorCode::LimitExceeded;
    default:
      return ErrorCode::IoFailure;
  }
}

[[nodiscard]] std::string system_message() {
  const DWORD code = ::GetLastError();
  LPWSTR buffer = nullptr;
  const DWORD length = ::FormatMessageW(
      FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
      nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
  std::string text;
  if (length != 0 && buffer != nullptr) {
    const std::wstring wide_text(buffer, length);
    for (const wchar_t character : wide_text) {
      if (character == L'\r' || character == L'\n') {
        continue;
      }
      text.push_back(character < 128 ? static_cast<char>(character) : '?');
    }
  }
  if (buffer != nullptr) {
    ::LocalFree(buffer);
  }
  if (text.empty()) {
    text = "windows error " + std::to_string(code);
  }
  return text;
}

}  // namespace

Result<PathKind> path_kind(const std::filesystem::path& path) {
  const DWORD attributes = ::GetFileAttributesW(wide(path).c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return PathKind::Missing;
    }
    return io_error("the path attributes could not be read: " + system_message(), path,
                    last_error_code());
  }
  if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
    return PathKind::Link;
  }
  if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
    return PathKind::Directory;
  }
  return PathKind::RegularFile;
}

Status ensure_directory(const std::filesystem::path& path) {
  const Result<PathKind> kind = path_kind(path);
  if (!kind.has_value()) {
    return Status(kind.error());
  }
  if (kind.value() == PathKind::Directory) {
    return Status{};
  }
  if (kind.value() != PathKind::Missing) {
    return Status(io_error("the path exists and is not a directory", path,
                           ErrorCode::InvalidArgument));
  }
  std::error_code error;
  if (!std::filesystem::create_directories(path, error) && error) {
    return Status(io_error("the directory could not be created: " + error.message(), path));
  }
  return Status{};
}

Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path,
                                            std::uint64_t max_bytes) {
  const HANDLE handle = ::CreateFileW(wide(path).c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return io_error("the file could not be opened for reading: " + system_message(), path,
                    last_error_code());
  }
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(handle, &size) == 0) {
    const CapacityError error =
        io_error("the file size could not be read: " + system_message(), path, last_error_code());
    ::CloseHandle(handle);
    return error;
  }
  if (size.QuadPart < 0) {
    ::CloseHandle(handle);
    return io_error("the file reports a negative size", path, ErrorCode::CorruptState);
  }
  const std::uint64_t total = static_cast<std::uint64_t>(size.QuadPart);
  if (total > max_bytes) {
    ::CloseHandle(handle);
    return make_error(ErrorCode::StateTooLarge,
                      "the file is longer than the documented bound",
                      ErrorDetail{"io", path.filename().string(), {}, max_bytes, total, {}});
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(total));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD chunk = static_cast<DWORD>(
        std::min<std::size_t>(bytes.size() - offset, 1u << 20));
    DWORD read = 0;
    if (::ReadFile(handle, bytes.data() + offset, chunk, &read, nullptr) == 0) {
      const CapacityError error =
          io_error("the file could not be read: " + system_message(), path, last_error_code());
      ::CloseHandle(handle);
      return error;
    }
    if (read == 0) {
      ::CloseHandle(handle);
      return io_error("the file ended before its recorded length", path, ErrorCode::TruncatedState);
    }
    offset += read;
  }
  ::CloseHandle(handle);
  return bytes;
}

Status write_file_flushed(const std::filesystem::path& path,
                          const std::vector<std::uint8_t>& bytes) {
  const HANDLE handle =
      ::CreateFileW(wide(path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    return Status(io_error("the file could not be created for writing: " + system_message(), path,
                           last_error_code()));
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const DWORD chunk =
        static_cast<DWORD>(std::min<std::size_t>(bytes.size() - offset, 1u << 20));
    DWORD written = 0;
    if (::WriteFile(handle, bytes.data() + offset, chunk, &written, nullptr) == 0 ||
        written != chunk) {
      const CapacityError error =
          io_error("the file could not be written: " + system_message(), path, last_error_code());
      ::CloseHandle(handle);
      return Status(error);
    }
    offset += written;
  }
  if (::FlushFileBuffers(handle) == 0) {
    const CapacityError error =
        io_error("the file could not be flushed: " + system_message(), path, last_error_code());
    ::CloseHandle(handle);
    return Status(error);
  }
  ::CloseHandle(handle);
  return Status{};
}

Status rename_replace(const std::filesystem::path& from, const std::filesystem::path& to) {
  if (::MoveFileExW(wide(from).c_str(), wide(to).c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) == 0) {
    return Status(io_error("the file could not be published atomically: " + system_message(), to,
                           last_error_code()));
  }
  return Status{};
}

Status remove_file(const std::filesystem::path& path) noexcept {
  if (::DeleteFileW(wide(path).c_str()) == 0) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
      return Status{};
    }
    return Status(io_error("the file could not be removed: " + system_message(), path,
                           last_error_code()));
  }
  return Status{};
}

Status sync_directory(const std::filesystem::path& path) {
  // MoveFileExW with MOVEFILE_WRITE_THROUGH already waits for the rename to
  // reach the volume, so there is no separate directory flush on this platform.
  (void)path;
  return Status{};
}

Result<std::vector<DirEntry>> list_directory(const std::filesystem::path& path) {
  std::vector<DirEntry> entries;
  std::error_code error;
  std::filesystem::directory_iterator iterator(path, error);
  if (error) {
    return io_error("the directory could not be listed: " + error.message(), path);
  }
  for (const std::filesystem::directory_entry& entry : iterator) {
    DirEntry record;
    record.name = entry.path().filename().string();
    const DWORD attributes = ::GetFileAttributesW(entry.path().wstring().c_str());
    if (attributes != INVALID_FILE_ATTRIBUTES) {
      record.is_link = (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
      record.is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
      record.is_file = !record.is_directory && !record.is_link;
    }
    entries.push_back(std::move(record));
  }
  return entries;
}

std::uint64_t current_process_id() noexcept {
  return static_cast<std::uint64_t>(::GetCurrentProcessId());
}

Result<std::uint64_t> process_start_marker(std::uint64_t pid) {
  const HANDLE handle = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE,
                                      static_cast<DWORD>(pid));
  if (handle == nullptr) {
    return io_error("the process could not be opened to read its start marker",
                    std::filesystem::path(std::to_string(pid)), last_error_code());
  }
  FILETIME creation{};
  FILETIME exit{};
  FILETIME kernel{};
  FILETIME user{};
  if (::GetProcessTimes(handle, &creation, &exit, &kernel, &user) == 0) {
    const CapacityError error = io_error("the process start time could not be read",
                                         std::filesystem::path(std::to_string(pid)),
                                         last_error_code());
    ::CloseHandle(handle);
    return error;
  }
  ::CloseHandle(handle);
  ULARGE_INTEGER combined{};
  combined.LowPart = creation.dwLowDateTime;
  combined.HighPart = creation.dwHighDateTime;
  return static_cast<std::uint64_t>(combined.QuadPart);
}

bool process_is_alive(std::uint64_t pid) noexcept {
  if (pid == 0) {
    return false;
  }
  const HANDLE handle =
      ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(pid));
  if (handle == nullptr) {
    return false;
  }
  DWORD exit_code = 0;
  const bool alive = ::GetExitCodeProcess(handle, &exit_code) != 0 && exit_code == STILL_ACTIVE;
  ::CloseHandle(handle);
  return alive;
}

ExclusiveFileLock::~ExclusiveFileLock() { close(); }

ExclusiveFileLock::ExclusiveFileLock(ExclusiveFileLock&& other) noexcept
    : path_(std::move(other.path_)), handle_(other.handle_), held_(other.held_) {
  other.handle_ = nullptr;
  other.held_ = false;
}

ExclusiveFileLock& ExclusiveFileLock::operator=(ExclusiveFileLock&& other) noexcept {
  if (this != &other) {
    close();
    path_ = std::move(other.path_);
    handle_ = other.handle_;
    held_ = other.held_;
    other.handle_ = nullptr;
    other.held_ = false;
  }
  return *this;
}

void ExclusiveFileLock::close() noexcept {
  if (handle_ != nullptr) {
    if (held_) {
      OVERLAPPED overlapped{};
      overlapped.Offset = static_cast<DWORD>(kLockRangeOffset & 0xFFFFFFFFu);
      overlapped.OffsetHigh = static_cast<DWORD>(kLockRangeOffset >> 32u);
      ::UnlockFileEx(static_cast<HANDLE>(handle_), 0, 1, 0, &overlapped);
    }
    ::CloseHandle(static_cast<HANDLE>(handle_));
    handle_ = nullptr;
  }
  held_ = false;
}

Result<ExclusiveFileLock> ExclusiveFileLock::acquire(const std::filesystem::path& path,
                                                     bool create_if_missing) {
  ExclusiveFileLock lock;
  const DWORD disposition = create_if_missing ? OPEN_ALWAYS : OPEN_EXISTING;
  const HANDLE handle =
      ::CreateFileW(wide(path).c_str(), GENERIC_READ | GENERIC_WRITE,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, disposition,
                    FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) {
    const DWORD code = ::GetLastError();
    if (code == ERROR_FILE_NOT_FOUND && !create_if_missing) {
      return io_error("the lock file does not exist", path, ErrorCode::LockUnavailable);
    }
    return io_error("the lock file could not be opened: " + system_message(), path,
                    last_error_code());
  }
  OVERLAPPED overlapped{};
  overlapped.Offset = static_cast<DWORD>(kLockRangeOffset & 0xFFFFFFFFu);
  overlapped.OffsetHigh = static_cast<DWORD>(kLockRangeOffset >> 32u);
  if (::LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0,
                   &overlapped) == 0) {
    const DWORD code = ::GetLastError();
    ::CloseHandle(handle);
    if (code == ERROR_LOCK_VIOLATION || code == ERROR_SHARING_VIOLATION) {
      return make_error(ErrorCode::WriterLockHeld,
                        "another live writer holds the exclusive operating-system lock",
                        ErrorDetail{"lock.acquire", path.filename().string(), {}, 0, 0, {}});
    }
    return io_error("the exclusive operating-system lock could not be taken: " + system_message(),
                    path, ErrorCode::LockIoFailure);
  }
  lock.path_ = path;
  lock.handle_ = handle;
  lock.held_ = true;
  return lock;
}

Status ExclusiveFileLock::release() noexcept {
  if (handle_ == nullptr) {
    return Status{};
  }
  Status status;
  if (held_) {
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(kLockRangeOffset & 0xFFFFFFFFu);
    overlapped.OffsetHigh = static_cast<DWORD>(kLockRangeOffset >> 32u);
    if (::UnlockFileEx(static_cast<HANDLE>(handle_), 0, 1, 0, &overlapped) == 0) {
      status = Status(io_error("the exclusive operating-system lock could not be released",
                               path_, ErrorCode::LockIoFailure));
    }
  }
  ::CloseHandle(static_cast<HANDLE>(handle_));
  handle_ = nullptr;
  held_ = false;
  return status;
}

Result<std::vector<std::uint8_t>> ExclusiveFileLock::read_locked(std::uint64_t max_bytes) const {
  if (handle_ == nullptr) {
    return make_error(ErrorCode::LockUnavailable, "no lock handle is held",
                      ErrorDetail{"lock.read", path_.filename().string(), {}, 0, 0, {}});
  }
  LARGE_INTEGER zero{};
  if (::SetFilePointerEx(static_cast<HANDLE>(handle_), zero, nullptr, FILE_BEGIN) == 0) {
    return io_error("the lock record could not be rewound: " + system_message(), path_,
                    last_error_code());
  }
  LARGE_INTEGER size{};
  if (::GetFileSizeEx(static_cast<HANDLE>(handle_), &size) == 0) {
    return io_error("the lock record size could not be read: " + system_message(), path_,
                    last_error_code());
  }
  const std::uint64_t total = size.QuadPart < 0 ? 0 : static_cast<std::uint64_t>(size.QuadPart);
  if (total > max_bytes) {
    return make_error(ErrorCode::StateTooLarge, "the lock record is longer than the documented bound",
                      ErrorDetail{"lock.read", path_.filename().string(), {}, max_bytes, total, {}});
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(total));
  DWORD read = 0;
  if (!bytes.empty() &&
      ::ReadFile(static_cast<HANDLE>(handle_), bytes.data(), static_cast<DWORD>(bytes.size()),
                 &read, nullptr) == 0) {
    return io_error("the lock record could not be read: " + system_message(), path_,
                    last_error_code());
  }
  bytes.resize(read);
  return bytes;
}

Status ExclusiveFileLock::write_locked(const std::vector<std::uint8_t>& bytes) const {
  if (handle_ == nullptr) {
    return Status(make_error(ErrorCode::LockUnavailable, "no lock handle is held",
                             ErrorDetail{"lock.write", path_.filename().string(), {}, 0, 0, {}}));
  }
  LARGE_INTEGER zero{};
  if (::SetFilePointerEx(static_cast<HANDLE>(handle_), zero, nullptr, FILE_BEGIN) == 0) {
    return Status(io_error("the lock record could not be rewound: " + system_message(), path_,
                           last_error_code()));
  }
  if (::SetEndOfFile(static_cast<HANDLE>(handle_)) == 0) {
    return Status(io_error("the lock record could not be truncated: " + system_message(), path_,
                           last_error_code()));
  }
  DWORD written = 0;
  if (!bytes.empty() &&
      (::WriteFile(static_cast<HANDLE>(handle_), bytes.data(), static_cast<DWORD>(bytes.size()),
                   &written, nullptr) == 0 ||
       written != bytes.size())) {
    return Status(io_error("the lock record could not be written: " + system_message(), path_,
                           last_error_code()));
  }
  if (::FlushFileBuffers(static_cast<HANDLE>(handle_)) == 0) {
    return Status(io_error("the lock record could not be flushed: " + system_message(), path_,
                           last_error_code()));
  }
  return Status{};
}

#else  // POSIX

namespace {

[[nodiscard]] CapacityError posix_error(std::string message, const std::filesystem::path& path,
                                        ErrorCode code = ErrorCode::IoFailure) {
  return make_error(code, std::move(message) + ": " + std::strerror(errno),
                    ErrorDetail{"io", path.filename().string(), path.parent_path().string(), 0, 0,
                                {}});
}

}  // namespace

Result<PathKind> path_kind(const std::filesystem::path& path) {
  struct stat info {};
  if (::lstat(path.c_str(), &info) != 0) {
    if (errno == ENOENT || errno == ENOTDIR) {
      return PathKind::Missing;
    }
    return posix_error("the path could not be examined", path);
  }
  if (S_ISLNK(info.st_mode)) {
    return PathKind::Link;
  }
  if (S_ISDIR(info.st_mode)) {
    return PathKind::Directory;
  }
  if (S_ISREG(info.st_mode)) {
    return PathKind::RegularFile;
  }
  return PathKind::Other;
}

Status ensure_directory(const std::filesystem::path& path) {
  const Result<PathKind> kind = path_kind(path);
  if (!kind.has_value()) {
    return Status(kind.error());
  }
  if (kind.value() == PathKind::Directory) {
    return Status{};
  }
  if (kind.value() != PathKind::Missing) {
    return Status(posix_error("the path exists and is not a directory", path,
                              ErrorCode::InvalidArgument));
  }
  std::error_code error;
  if (!std::filesystem::create_directories(path, error) && error) {
    return Status(io_error("the directory could not be created: " + error.message(), path));
  }
  return Status{};
}

Result<std::vector<std::uint8_t>> read_file(const std::filesystem::path& path,
                                            std::uint64_t max_bytes) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    return posix_error("the file could not be opened for reading", path);
  }
  struct stat info {};
  if (::fstat(fd, &info) != 0) {
    const CapacityError error = posix_error("the file could not be examined", path);
    ::close(fd);
    return error;
  }
  if (info.st_size < 0) {
    ::close(fd);
    return make_error(ErrorCode::CorruptState, "the file reports a negative size",
                      ErrorDetail{"io", path.filename().string(), {}, 0, 0, {}});
  }
  const std::uint64_t total = static_cast<std::uint64_t>(info.st_size);
  if (total > max_bytes) {
    ::close(fd);
    return make_error(ErrorCode::StateTooLarge, "the file is longer than the documented bound",
                      ErrorDetail{"io", path.filename().string(), {}, max_bytes, total, {}});
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(total));
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t read = ::read(fd, bytes.data() + offset, bytes.size() - offset);
    if (read < 0) {
      if (errno == EINTR) {
        continue;
      }
      const CapacityError error = posix_error("the file could not be read", path);
      ::close(fd);
      return error;
    }
    if (read == 0) {
      ::close(fd);
      return make_error(ErrorCode::TruncatedState, "the file ended before its recorded length",
                        ErrorDetail{"io", path.filename().string(), {}, total, offset, {}});
    }
    offset += static_cast<std::size_t>(read);
  }
  ::close(fd);
  return bytes;
}

Status write_file_flushed(const std::filesystem::path& path,
                          const std::vector<std::uint8_t>& bytes) {
  const int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) {
    return Status(posix_error("the file could not be created for writing", path));
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      const CapacityError error = posix_error("the file could not be written", path);
      ::close(fd);
      return Status(error);
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(fd) != 0) {
    const CapacityError error = posix_error("the file could not be flushed", path);
    ::close(fd);
    return Status(error);
  }
  ::close(fd);
  return Status{};
}

Status rename_replace(const std::filesystem::path& from, const std::filesystem::path& to) {
  if (::rename(from.c_str(), to.c_str()) != 0) {
    return Status(posix_error("the file could not be published atomically", to));
  }
  return Status{};
}

Status remove_file(const std::filesystem::path& path) noexcept {
  if (::unlink(path.c_str()) != 0 && errno != ENOENT) {
    return Status(posix_error("the file could not be removed", path));
  }
  return Status{};
}

Status sync_directory(const std::filesystem::path& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) {
    return Status(posix_error("the directory could not be opened for syncing", path));
  }
  const int result = ::fsync(fd);
  const CapacityError error = result != 0 ? posix_error("the directory could not be synced", path)
                                          : CapacityError{};
  ::close(fd);
  if (error.code != ErrorCode::Ok) {
    return Status(error);
  }
  return Status{};
}

Result<std::vector<DirEntry>> list_directory(const std::filesystem::path& path) {
  std::vector<DirEntry> entries;
  std::error_code error;
  std::filesystem::directory_iterator iterator(path, error);
  if (error) {
    return io_error("the directory could not be listed: " + error.message(), path);
  }
  for (const std::filesystem::directory_entry& entry : iterator) {
    DirEntry record;
    record.name = entry.path().filename().string();
    const Result<PathKind> kind = path_kind(entry.path());
    if (kind.has_value()) {
      record.is_link = kind.value() == PathKind::Link;
      record.is_directory = kind.value() == PathKind::Directory;
      record.is_file = kind.value() == PathKind::RegularFile;
    }
    entries.push_back(std::move(record));
  }
  return entries;
}

std::uint64_t current_process_id() noexcept { return static_cast<std::uint64_t>(::getpid()); }

Result<std::uint64_t> process_start_marker(std::uint64_t pid) {
  const std::string path = "/proc/" + std::to_string(pid) + "/stat";
  const Result<std::vector<std::uint8_t>> bytes =
      read_file(path, kMaxAuxiliaryFileBytes);
  if (!bytes.has_value()) {
    return bytes.error();
  }
  const std::string text(bytes.value().begin(), bytes.value().end());
  const std::size_t close = text.rfind(')');
  if (close == std::string::npos || close + 2 >= text.size()) {
    return make_error(ErrorCode::CorruptState, "the process stat record is not well formed",
                      ErrorDetail{"process", std::to_string(pid), {}, 0, 0, {}});
  }
  // Field 22 of /proc/<pid>/stat is the process start time in clock ticks; it
  // is the third field after the command name.
  std::size_t position = close + 2;
  int field = 3;
  while (field < 22 && position < text.size()) {
    while (position < text.size() && text[position] != ' ') {
      ++position;
    }
    while (position < text.size() && text[position] == ' ') {
      ++position;
    }
    ++field;
  }
  std::uint64_t value = 0;
  while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
    value = value * 10u + static_cast<std::uint64_t>(text[position] - '0');
    ++position;
  }
  return value;
}

bool process_is_alive(std::uint64_t pid) noexcept {
  if (pid == 0) {
    return false;
  }
  return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno == EPERM;
}

ExclusiveFileLock::~ExclusiveFileLock() { close(); }

ExclusiveFileLock::ExclusiveFileLock(ExclusiveFileLock&& other) noexcept
    : path_(std::move(other.path_)), fd_(other.fd_), held_(other.held_) {
  other.fd_ = -1;
  other.held_ = false;
}

ExclusiveFileLock& ExclusiveFileLock::operator=(ExclusiveFileLock&& other) noexcept {
  if (this != &other) {
    close();
    path_ = std::move(other.path_);
    fd_ = other.fd_;
    held_ = other.held_;
    other.fd_ = -1;
    other.held_ = false;
  }
  return *this;
}

void ExclusiveFileLock::close() noexcept {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  held_ = false;
}

Result<ExclusiveFileLock> ExclusiveFileLock::acquire(const std::filesystem::path& path,
                                                     bool create_if_missing) {
  const int flags = O_RDWR | O_CLOEXEC | (create_if_missing ? O_CREAT : 0);
  const int fd = ::open(path.c_str(), flags, 0644);
  if (fd < 0) {
    if (errno == ENOENT && !create_if_missing) {
      return make_error(ErrorCode::LockUnavailable, "the lock file does not exist",
                        ErrorDetail{"lock.acquire", path.filename().string(), {}, 0, 0, {}});
    }
    return posix_error("the lock file could not be opened", path, ErrorCode::LockIoFailure);
  }
  struct flock request {};
  request.l_type = F_WRLCK;
  request.l_whence = SEEK_SET;
  request.l_start = static_cast<off_t>(kLockRangeOffset);
  request.l_len = 1;
  if (::fcntl(fd, F_SETLK, &request) != 0) {
    const int code = errno;
    ::close(fd);
    if (code == EACCES || code == EAGAIN) {
      return make_error(ErrorCode::WriterLockHeld,
                        "another live writer holds the exclusive operating-system lock",
                        ErrorDetail{"lock.acquire", path.filename().string(), {}, 0, 0, {}});
    }
    return posix_error("the exclusive operating-system lock could not be taken", path,
                       ErrorCode::LockIoFailure);
  }
  ExclusiveFileLock lock;
  lock.path_ = path;
  lock.fd_ = fd;
  lock.held_ = true;
  return lock;
}

Status ExclusiveFileLock::release() noexcept {
  if (fd_ < 0) {
    return Status{};
  }
  Status status;
  if (held_) {
    struct flock request {};
    request.l_type = F_UNLCK;
    request.l_whence = SEEK_SET;
    request.l_start = static_cast<off_t>(kLockRangeOffset);
    request.l_len = 1;
    if (::fcntl(fd_, F_SETLK, &request) != 0) {
      status = Status(posix_error("the exclusive operating-system lock could not be released", path_,
                                  ErrorCode::LockIoFailure));
    }
  }
  ::close(fd_);
  fd_ = -1;
  held_ = false;
  return status;
}

Result<std::vector<std::uint8_t>> ExclusiveFileLock::read_locked(std::uint64_t max_bytes) const {
  if (fd_ < 0) {
    return make_error(ErrorCode::LockUnavailable, "no lock handle is held",
                      ErrorDetail{"lock.read", path_.filename().string(), {}, 0, 0, {}});
  }
  struct stat info {};
  if (::fstat(fd_, &info) != 0) {
    return posix_error("the lock record could not be examined", path_);
  }
  const std::uint64_t total = info.st_size < 0 ? 0 : static_cast<std::uint64_t>(info.st_size);
  if (total > max_bytes) {
    return make_error(ErrorCode::StateTooLarge,
                      "the lock record is longer than the documented bound",
                      ErrorDetail{"lock.read", path_.filename().string(), {}, max_bytes, total, {}});
  }
  std::vector<std::uint8_t> bytes(static_cast<std::size_t>(total));
  if (::lseek(fd_, 0, SEEK_SET) < 0) {
    return posix_error("the lock record could not be rewound", path_);
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t read = ::read(fd_, bytes.data() + offset, bytes.size() - offset);
    if (read < 0) {
      if (errno == EINTR) {
        continue;
      }
      return posix_error("the lock record could not be read", path_);
    }
    if (read == 0) {
      break;
    }
    offset += static_cast<std::size_t>(read);
  }
  bytes.resize(offset);
  return bytes;
}

Status ExclusiveFileLock::write_locked(const std::vector<std::uint8_t>& bytes) const {
  if (fd_ < 0) {
    return Status(make_error(ErrorCode::LockUnavailable, "no lock handle is held",
                             ErrorDetail{"lock.write", path_.filename().string(), {}, 0, 0, {}}));
  }
  if (::ftruncate(fd_, 0) != 0) {
    return Status(posix_error("the lock record could not be truncated", path_));
  }
  if (::lseek(fd_, 0, SEEK_SET) < 0) {
    return Status(posix_error("the lock record could not be rewound", path_));
  }
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const ssize_t written = ::write(fd_, bytes.data() + offset, bytes.size() - offset);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return Status(posix_error("the lock record could not be written", path_));
    }
    offset += static_cast<std::size_t>(written);
  }
  if (::fsync(fd_) != 0) {
    return Status(posix_error("the lock record could not be flushed", path_));
  }
  return Status{};
}

#endif

std::string random_hex(std::size_t bytes) {
  // Combined entropy from the platform generator, the clock and the process
  // identifier, hashed so that the output length is independent of how much
  // entropy each source happened to provide. The value identifies a store; it
  // is not a secret and is never used as a credential.
  std::vector<std::uint8_t> material;
  material.reserve(bytes * 8u + 64u);
  const auto append = [&material](std::uint64_t value) {
    for (int index = 0; index < 8; ++index) {
      material.push_back(static_cast<std::uint8_t>((value >> (index * 8)) & 0xFFu));
    }
  };
  std::random_device device;
  for (std::size_t index = 0; index < 8; ++index) {
    append(static_cast<std::uint64_t>(device()));
  }
  const auto now = std::chrono::high_resolution_clock::now().time_since_epoch().count();
  append(static_cast<std::uint64_t>(now));
  append(current_process_id());
  static std::uint64_t counter = 0;
  append(++counter);
  const std::array<std::uint8_t, Sha256::kDigestBytes> digest = sha256(material.data(), material.size());
  static constexpr char kDigits[] = "0123456789abcdef";
  std::string text;
  text.reserve(bytes * 2u);
  for (std::size_t index = 0; index < bytes && index < digest.size(); ++index) {
    text.push_back(kDigits[(digest[index] >> 4u) & 0x0Fu]);
    text.push_back(kDigits[digest[index] & 0x0Fu]);
  }
  return text;
}

}  // namespace rackcapacity::platform
