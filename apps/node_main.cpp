#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "common.hpp"
#include "http.hpp"
#include "http_server.hpp"
#include "node.hpp"

using namespace dcache;

namespace {

std::atomic<bool> g_running{true};

void on_signal(int) { g_running = false; }

std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out;
    std::string cur;
    std::istringstream ss(s);
    while (std::getline(ss, cur, delim)) {
        if (!cur.empty()) out.push_back(cur);
    }
    return out;
}

bool split_host_port(const std::string& addr, std::string& host, std::uint16_t& port) {
    const std::size_t pos = addr.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= addr.size()) {
        return false;
    }
    host = addr.substr(0, pos);
    int p = std::atoi(addr.substr(pos + 1).c_str());
    if (p <= 0 || p > 65535) {
        return false;
    }
    port = static_cast<std::uint16_t>(p);
    return true;
}

// mock 数据源：value = "mock:" + key；以 "miss:" 开头的 key 视为不存在（演示穿透治理）。
GetResult mock_getter(const std::string& key) {
    if (key.empty() || key.rfind("miss:", 0) == 0) {
        return GetResult(false, "");
    }
    return GetResult(true, "mock:" + key);
}

struct Options {
    std::string self            = "127.0.0.1:8001";
    std::string peers           = "127.0.0.1:8001";
    std::string group           = "scores";
    int         capacity        = 100;
    int         ttl_ms          = 0;
    int         virtual_nodes   = 150;
    int         workers         = 4;
    int         report_interval = 5;
};

void usage(const char* argv0) {
    std::cerr <<
        "usage: " << argv0 << " [options]\n"
        "  --self <host:port>      本节点监听地址 (默认 127.0.0.1:8001)\n"
        "  --peers <h1:p1,h2:p2..> 集群全部节点（含自身；若缺自身会自动补上）\n"
        "  --group <name>          缓存分组名 (默认 scores)\n"
        "  --capacity <n>          本地 LRU 容量 (默认 100)\n"
        "  --ttl-ms <n>            默认 TTL，0 表示不设过期 (默认 0)\n"
        "  --virtual-nodes <n>     每物理节点虚拟节点数 (默认 150)\n"
        "  --workers <n>            HTTP 工作线程数 (默认 4)\n"
        "  --report-interval <s>   统计打印间隔秒 (默认 5)\n";
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::cerr << "缺少 " << name << " 的值\n"; return nullptr; }
            return argv[++i];
        };
        if (a == "--self") {
            const char* v = need("--self"); if (!v) return false; o.self = v;
        } else if (a == "--peers") {
            const char* v = need("--peers"); if (!v) return false; o.peers = v;
        } else if (a == "--group") {
            const char* v = need("--group"); if (!v) return false; o.group = v;
        } else if (a == "--capacity") {
            const char* v = need("--capacity"); if (!v) return false; o.capacity = std::atoi(v);
        } else if (a == "--ttl-ms") {
            const char* v = need("--ttl-ms"); if (!v) return false; o.ttl_ms = std::atoi(v);
        } else if (a == "--virtual-nodes") {
            const char* v = need("--virtual-nodes"); if (!v) return false; o.virtual_nodes = std::atoi(v);
        } else if (a == "--workers") {
            const char* v = need("--workers"); if (!v) return false; o.workers = std::atoi(v);
        } else if (a == "--report-interval") {
            const char* v = need("--report-interval"); if (!v) return false; o.report_interval = std::atoi(v);
        } else if (a == "--help" || a == "-h") {
            usage(argv[0]);
            std::exit(0);
        } else {
            std::cerr << "未知参数: " << a << "\n";
            usage(argv[0]);
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse_args(argc, argv, o)) {
        return 1;
    }

    std::string host;
    std::uint16_t port = 0;
    if (!split_host_port(o.self, host, port)) {
        std::cerr << "无效的 --self: " << o.self << "\n";
        return 1;
    }

    std::vector<std::string> peers = split(o.peers, ',');
    bool has_self = false;
    for (const auto& p : peers) {
        if (p == o.self) has_self = true;
    }
    if (!has_self) {
        peers.push_back(o.self); // 节点环必须包含自身
    }

    Node node(o.self, o.virtual_nodes);
    node.AddGroup(o.group, static_cast<std::size_t>(o.capacity), mock_getter,
                  static_cast<std::uint64_t>(o.ttl_ms), 30000);
    node.SetPeers(peers);

    HttpServer server;
    if (!server.Start(host, port, [&node](const HttpRequest& req, HttpResponse& resp) {
            node.ServeHTTP(req, resp);
        }, o.workers)) {
        std::cerr << "启动失败: " << o.self << "\n";
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    std::cout << "[node] listening on " << o.self
              << " group=" << o.group
              << " peers=" << o.peers << "\n";

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(o.report_interval));
        if (!g_running) break;
        std::cout << "[node] stats=" << node.DumpStats() << "\n";
    }

    server.Stop();
    std::cout << "[node] stopped\n";
    return 0;
}