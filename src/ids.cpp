// Rack Capacity - identity validation and reference rendering.
//
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.

#include "rack_capacity/ids.hpp"

#include <string>

namespace rackcapacity {
namespace {

[[nodiscard]] bool is_identity_character(unsigned char character) noexcept {
  // Printable non-space ASCII only. Path separators, quoting characters, shell
  // metacharacters and the colon are all refused: the colon separates fields in
  // every canonical text form this library emits, and the rest are refused
  // because an identity is never a path component, a command line or a pattern.
  if (character < 0x21u || character > 0x7Eu) {
    return false;
  }
  switch (character) {
    case '/':
    case '\\':
    case ':':
    case '*':
    case '?':
    case '"':
    case '<':
    case '>':
    case '|':
    case '\'':
    case '`':
    case '$':
    case ';':
    case '&':
    case '%':
    case '!':
    case '(':
    case ')':
    case '{':
    case '}':
    case '[':
    case ']':
    case ',':
    case '=':
    case '\t':
      return false;
    default:
      return true;
  }
}

}  // namespace

namespace detail {

Status validate_identity_text(std::string_view kind, std::string_view text) {
  const auto reject = [&](ErrorCode code, std::string message) {
    return Status(make_error(code, std::move(message),
                             ErrorDetail{"identity.validate", std::string(kind), {}, 0,
                                         text.size(), {}}));
  };

  if (text.empty()) {
    return reject(ErrorCode::EmptyValue, "an identity must not be empty");
  }
  if (text.size() > kMaxIdentityTextBytes) {
    return Status(make_error(ErrorCode::IdentityTooLong,
                             "an identity exceeds the documented byte length",
                             ErrorDetail{"identity.validate", std::string(kind), {},
                                         kMaxIdentityTextBytes, text.size(), {}}));
  }
  for (const char character : text) {
    if (!is_identity_character(static_cast<unsigned char>(character))) {
      return reject(ErrorCode::InvalidCharacter,
                    "an identity contains a character outside the accepted set");
    }
  }
  if (text.find("..") != std::string_view::npos) {
    return reject(ErrorCode::InvalidCharacter,
                  "an identity must not contain a parent directory sequence");
  }
  if (text.front() == '.' || text.back() == '.') {
    return reject(ErrorCode::InvalidCharacter,
                  "an identity must not begin or end with a dot");
  }
  return Status{};
}

}  // namespace detail

std::string PolicyReference::to_text() const {
  std::string text = policy.text();
  text.push_back(':');
  text += std::to_string(version);
  return text;
}

std::string LocationReference::to_text() const {
  const auto part = [](const auto& value) -> std::string {
    return value.has_value() ? value->text() : std::string("?");
  };
  std::string text = part(site);
  text.push_back('/');
  text += part(hall);
  text.push_back('/');
  text += part(row);
  text.push_back('/');
  text += part(position);
  return text;
}

namespace {

template <typename T>
[[nodiscard]] int compare_optional(const std::optional<T>& left, const std::optional<T>& right) {
  if (left.has_value() != right.has_value()) {
    return left.has_value() ? 1 : -1;
  }
  if (!left.has_value()) {
    return 0;
  }
  if (*left == *right) {
    return 0;
  }
  return *left < *right ? -1 : 1;
}

}  // namespace

bool LocationReference::operator<(const LocationReference& other) const noexcept {
  int comparison = compare_optional(site, other.site);
  if (comparison != 0) {
    return comparison < 0;
  }
  comparison = compare_optional(hall, other.hall);
  if (comparison != 0) {
    return comparison < 0;
  }
  comparison = compare_optional(row, other.row);
  if (comparison != 0) {
    return comparison < 0;
  }
  return compare_optional(position, other.position) < 0;
}

}  // namespace rackcapacity
