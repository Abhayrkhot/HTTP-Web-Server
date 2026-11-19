#include "server/config.h"

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

using hws::ArgsResult;
using hws::Config;
using hws::parse_args;
using hws::parse_duration_ms;
using hws::parse_size;

namespace {

ArgsResult run(std::vector<std::string> args, Config& cfg, std::string& err) {
  args.insert(args.begin(), "hws");
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(a.data());
  return parse_args(static_cast<int>(argv.size()), argv.data(), cfg, err);
}

}  // namespace

TEST(Config, Durations) {
  uint64_t v = 0;
  EXPECT_TRUE(parse_duration_ms("250ms", v));
  EXPECT_EQ(v, 250u);
  EXPECT_TRUE(parse_duration_ms("10s", v));
  EXPECT_EQ(v, 10000u);
  EXPECT_TRUE(parse_duration_ms("2m", v));
  EXPECT_EQ(v, 120000u);
  EXPECT_TRUE(parse_duration_ms("750", v));
  EXPECT_EQ(v, 750u);
  EXPECT_FALSE(parse_duration_ms("10x", v));
  EXPECT_FALSE(parse_duration_ms("s", v));
  EXPECT_FALSE(parse_duration_ms("-1s", v));
}

TEST(Config, Sizes) {
  uint64_t v = 0;
  EXPECT_TRUE(parse_size("16k", v));
  EXPECT_EQ(v, 16384u);
  EXPECT_TRUE(parse_size("1M", v));
  EXPECT_EQ(v, 1048576u);
  EXPECT_TRUE(parse_size("512", v));
  EXPECT_EQ(v, 512u);
  EXPECT_FALSE(parse_size("1t", v));
}

TEST(Config, CommandLineForms) {
  Config cfg;
  std::string err;
  ASSERT_EQ(run({"--listen=127.0.0.1:9000,[::1]:9001", "--workers", "3", "--reuseport=off", "--pin-workers",
                 "--keepalive-timeout", "5s", "--max-body-bytes=2m"},
                cfg, err),
            ArgsResult::Run)
      << err;
  EXPECT_EQ(cfg.listen.size(), 2u);
  EXPECT_EQ(cfg.workers, 3u);
  EXPECT_FALSE(cfg.reuseport);
  EXPECT_TRUE(cfg.pin_workers);
  EXPECT_EQ(cfg.keepalive_timeout_ms, 5000u);
  EXPECT_EQ(cfg.max_body_bytes, 2u << 20);
}

TEST(Config, Errors) {
  Config cfg;
  std::string err;
  EXPECT_EQ(run({"--bogus"}, cfg, err), ArgsResult::Error);
  EXPECT_EQ(run({"--workers"}, cfg, err), ArgsResult::Error);
  EXPECT_EQ(run({"--workers", "many"}, cfg, err), ArgsResult::Error);
  EXPECT_EQ(run({"--listen", "::1:80"}, cfg, err), ArgsResult::Error);  // unbracketed IPv6
  EXPECT_EQ(run({"--keepalive-timeout", "soon"}, cfg, err), ArgsResult::Error);
  EXPECT_EQ(run({"positional"}, cfg, err), ArgsResult::Error);
}

TEST(Config, FileThenFlagsOverride) {
  const std::string path = ::testing::TempDir() + "hws_test.conf";
  {
    std::ofstream f(path);
    f << "# comment\nworkers = 2\nroot = /srv/www   # trailing comment\nheader-timeout = 3s\n\n";
  }
  Config cfg;
  std::string err;
  ASSERT_EQ(run({"--workers=5", "--config", path}, cfg, err), ArgsResult::Run) << err;
  EXPECT_EQ(cfg.workers, 5u);  // flag beats file regardless of order
  EXPECT_EQ(cfg.root, "/srv/www");
  EXPECT_EQ(cfg.header_timeout_ms, 3000u);
  std::remove(path.c_str());
}

TEST(Config, Validation) {
  Config cfg;
  std::string err;
  EXPECT_TRUE(hws::validate_config(cfg, err)) << err;
  cfg.max_headers = 1000;
  EXPECT_FALSE(hws::validate_config(cfg, err));
  cfg = Config{};
  cfg.health_path = "healthz";
  EXPECT_FALSE(hws::validate_config(cfg, err));
  cfg = Config{};
  cfg.index = "../x";
  EXPECT_FALSE(hws::validate_config(cfg, err));
}
