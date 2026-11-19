#include "server/metrics.h"

#include <unistd.h>

#include <cstdio>

namespace hws {
namespace {

void line(std::string& out, const char* name, const char* labels, uint64_t v) {
  out.append(name);
  if (labels != nullptr && *labels != '\0') {
    out.push_back('{');
    out.append(labels);
    out.push_back('}');
  }
  out.push_back(' ');
  out.append(std::to_string(v));
  out.push_back('\n');
}

void header(std::string& out, const char* name, const char* type, const char* help) {
  out.append("# HELP ").append(name).append(" ").append(help).append("\n");
  out.append("# TYPE ").append(name).append(" ").append(type).append("\n");
}

template <class F>
uint64_t sum(const std::vector<const WorkerMetrics*>& ws, F&& f) {
  uint64_t total = 0;
  for (const auto* w : ws) total += f(*w);
  return total;
}

}  // namespace

uint64_t resident_memory_bytes() noexcept {
  FILE* f = std::fopen("/proc/self/statm", "r");
  if (f == nullptr) return 0;
  unsigned long size = 0;
  unsigned long resident = 0;
  const int n = std::fscanf(f, "%lu %lu", &size, &resident);
  std::fclose(f);
  if (n != 2) return 0;
  return static_cast<uint64_t>(resident) * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
}

std::string render_prometheus(const std::vector<const WorkerMetrics*>& ws, const ProcessInfo& info) {
  std::string out;
  out.reserve(8192);

  header(out, "hws_build_info", "gauge", "Build information.");
  out.append("hws_build_info{version=\"" HWS_VERSION "\"} 1\n");
  header(out, "hws_uptime_seconds", "gauge", "Seconds since the server started.");
  line(out, "hws_uptime_seconds", nullptr, info.uptime_s);
  header(out, "hws_draining", "gauge", "1 while the server is shutting down gracefully.");
  line(out, "hws_draining", nullptr, info.draining ? 1 : 0);
  header(out, "hws_workers", "gauge", "Worker threads (event loops).");
  line(out, "hws_workers", nullptr, info.workers);
  header(out, "hws_process_resident_memory_bytes", "gauge", "Resident set size.");
  line(out, "hws_process_resident_memory_bytes", nullptr, resident_memory_bytes());

  header(out, "hws_connections_active", "gauge", "Open client connections.");
  line(out, "hws_connections_active", nullptr, sum(ws, [](const WorkerMetrics& w) { return w.active(); }));
  header(out, "hws_worker_connections_active", "gauge", "Open connections per worker (SO_REUSEPORT balance).");
  for (size_t i = 0; i < ws.size(); ++i) {
    const std::string labels = "worker=\"" + std::to_string(i) + "\"";
    line(out, "hws_worker_connections_active", labels.c_str(), ws[i]->active());
  }
  header(out, "hws_connections_accepted_total", "counter", "Accepted connections.");
  line(out, "hws_connections_accepted_total", nullptr,
       sum(ws, [](const WorkerMetrics& w) { return w.accepted.get(); }));
  header(out, "hws_connections_rejected_total", "counter", "Connections refused under load.");
  line(out, "hws_connections_rejected_total", "reason=\"max_connections\"",
       sum(ws, [](const WorkerMetrics& w) { return w.rejected_limit.get(); }));
  line(out, "hws_connections_rejected_total", "reason=\"fd_exhausted\"",
       sum(ws, [](const WorkerMetrics& w) { return w.rejected_fd.get(); }));

  header(out, "hws_responses_total", "counter", "Responses by status class.");
  for (size_t c = 1; c <= 5; ++c) {
    const std::string labels = "code=\"" + std::to_string(c) + "xx\"";
    line(out, "hws_responses_total", labels.c_str(),
         sum(ws, [c](const WorkerMetrics& w) { return w.responses[c].get(); }));
  }
  header(out, "hws_received_bytes_total", "counter", "Bytes read from clients.");
  line(out, "hws_received_bytes_total", nullptr, sum(ws, [](const WorkerMetrics& w) { return w.bytes_in.get(); }));
  header(out, "hws_sent_bytes_total", "counter", "Bytes written to clients.");
  line(out, "hws_sent_bytes_total", nullptr, sum(ws, [](const WorkerMetrics& w) { return w.bytes_out.get(); }));

  header(out, "hws_timeouts_total", "counter", "Connections closed by a timeout.");
  line(out, "hws_timeouts_total", "kind=\"header\"",
       sum(ws, [](const WorkerMetrics& w) { return w.timeout_header.get(); }));
  line(out, "hws_timeouts_total", "kind=\"body\"", sum(ws, [](const WorkerMetrics& w) { return w.timeout_body.get(); }));
  line(out, "hws_timeouts_total", "kind=\"idle\"", sum(ws, [](const WorkerMetrics& w) { return w.timeout_idle.get(); }));
  line(out, "hws_timeouts_total", "kind=\"write\"",
       sum(ws, [](const WorkerMetrics& w) { return w.timeout_write.get(); }));
  header(out, "hws_parse_errors_total", "counter", "Requests rejected as malformed.");
  line(out, "hws_parse_errors_total", nullptr, sum(ws, [](const WorkerMetrics& w) { return w.parse_errors.get(); }));
  header(out, "hws_handler_errors_total", "counter", "Handlers that threw (answered 500).");
  line(out, "hws_handler_errors_total", nullptr,
       sum(ws, [](const WorkerMetrics& w) { return w.handler_errors.get(); }));
  header(out, "hws_file_cache_lookups_total", "counter", "Open-file cache lookups.");
  line(out, "hws_file_cache_lookups_total", "result=\"hit\"",
       sum(ws, [](const WorkerMetrics& w) { return w.file_cache_hits.get(); }));
  line(out, "hws_file_cache_lookups_total", "result=\"miss\"",
       sum(ws, [](const WorkerMetrics& w) { return w.file_cache_misses.get(); }));

  header(out, "hws_request_duration_seconds", "histogram",
         "Time from a request being fully received to its response being written to the socket.");
  uint64_t cumulative = 0;
  char buf[96];
  for (size_t i = 0; i <= Histogram::kBoundsNs.size(); ++i) {
    cumulative += sum(ws, [i](const WorkerMetrics& w) { return w.latency.bucket(i); });
    if (i < Histogram::kBoundsNs.size()) {
      std::snprintf(buf, sizeof(buf), "le=\"%g\"", static_cast<double>(Histogram::kBoundsNs[i]) / 1e9);
    } else {
      std::snprintf(buf, sizeof(buf), "le=\"+Inf\"");
    }
    line(out, "hws_request_duration_seconds_bucket", buf, cumulative);
  }
  const double sum_s = static_cast<double>(sum(ws, [](const WorkerMetrics& w) { return w.latency.sum_ns(); })) / 1e9;
  std::snprintf(buf, sizeof(buf), "hws_request_duration_seconds_sum %.9f\n", sum_s);
  out.append(buf);
  line(out, "hws_request_duration_seconds_count", nullptr,
       sum(ws, [](const WorkerMetrics& w) { return w.latency.count(); }));
  return out;
}

}  // namespace hws
