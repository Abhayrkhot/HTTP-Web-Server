#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace hws::http {

// A body streamed from a file descriptor with sendfile(2). `owner` keeps the descriptor open
// while the transfer is in flight, even if the file cache evicts the entry meanwhile.
struct FileBody {
  std::shared_ptr<const void> owner;
  int fd = -1;
  uint64_t offset = 0;
  uint64_t length = 0;
  bool active() const noexcept { return fd >= 0; }
};

// What a handler fills in. Framing headers (Content-Length, Connection, Date, Server) are added
// by the server; handlers set status, extra headers and a body.
class Response {
 public:
  uint16_t status = 200;
  bool close = false;  // ask the server to close the connection after this response

  void reset();

  // Rejects (returns false) names that are not tokens and values containing CR, LF, NUL or other
  // control bytes, so handlers cannot inject headers or split the response.
  bool add_header(std::string_view name, std::string_view value);
  bool set_content_type(std::string_view value) { return add_header("Content-Type", value); }

  void set_body(std::string_view body);
  // Body owned elsewhere: either outliving the response (string literals) or kept alive by
  // `owner` (file-cache entries) until the response is reset.
  void set_body_view(std::string_view body, std::shared_ptr<const void> owner = {}) noexcept;
  void set_file(FileBody file);
  // Plain-text body with Content-Type set.
  void text(uint16_t code, std::string_view body);

  std::string_view headers() const noexcept { return headers_; }
  std::string_view body() const noexcept { return use_view_ ? view_ : std::string_view(body_); }
  const FileBody& file() const noexcept { return file_; }
  uint64_t content_length() const noexcept { return file_.active() ? file_.length : body().size(); }

 private:
  std::string headers_;  // serialized "Name: value\r\n" lines
  std::string body_;
  std::string_view view_;
  std::shared_ptr<const void> view_owner_;
  bool use_view_ = false;
  FileBody file_;
};

}  // namespace hws::http
