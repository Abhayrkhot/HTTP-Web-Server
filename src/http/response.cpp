#include "http/response.h"

#include "http/chars.h"

namespace hws::http {

void Response::reset() {
  status = 200;
  close = false;
  headers_.clear();
  body_.clear();
  view_ = {};
  view_owner_.reset();
  use_view_ = false;
  file_ = FileBody{};
}

bool Response::add_header(std::string_view name, std::string_view value) {
  if (name.empty()) return false;
  for (char c : name) {
    if (!is_tchar(c)) return false;
  }
  for (char c : value) {
    if (!is_field_value_char(c)) return false;  // excludes CR, LF, NUL and other controls
  }
  headers_.append(name);
  headers_.append(": ");
  headers_.append(value);
  headers_.append("\r\n");
  return true;
}

void Response::set_body(std::string_view body) {
  body_.assign(body);
  view_ = {};
  view_owner_.reset();
  use_view_ = false;
  file_ = FileBody{};
}

void Response::set_body_view(std::string_view body, std::shared_ptr<const void> owner) noexcept {
  body_.clear();
  view_ = body;
  view_owner_ = std::move(owner);
  use_view_ = true;
  file_ = FileBody{};
}

void Response::set_file(FileBody file) {
  body_.clear();
  view_ = {};
  view_owner_.reset();
  use_view_ = false;
  file_ = std::move(file);
}

void Response::text(uint16_t code, std::string_view body) {
  status = code;
  set_content_type("text/plain; charset=utf-8");
  set_body(body);
}

}  // namespace hws::http
