#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "http_client.hpp"
#include "json.hpp"

using namespace dcache;

namespace {

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

struct Options {
    std::string target   = "127.0.0.1:8001";
    std::string group    = "scores";
    int         threads  = 8;
    int         requests = 10000;
    int         keys     = 1000;
    int         timeout  = 2000;
};

void usage(const char* argv0) {
    std::cerr <<
        "usage: " << argv0 << " [options]\n"
        "  --target <host:port>   目标节点地址 (默认 127.0.0.1:8001)\n"
        "  --group <name>         缓存分组名 (默认 scores)\n"
        "  --threads <n>          并发线程数 (默认 8)\n"
        "  --requests <n>         总请求数 (默认 10000)\n"
        "  --keys <n>             随机 key 空间大小 (默认 1000)\n"
        "  --timeout <ms>         单请求超时 (默认 2000)\n";
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need = [&](const char* name) -> const char* {
            if (i + 1 >= argc) { std::cerr << "缺少 " << name << " 的值\n"; return nullptr; }
            return argv[++i];
        };
        if (a == "--target") {
            const char* v = need("--target"); if (!v) return false; o.target = v;
        } else if (a == "--group") {
            const char* v = need("--group"); if (!v) return false; o.group = v;
        } else if (a == "--threads") {
            const char* v = need("--threads"); if (!v) return false; o.threads = std::atoi(v);
        } else if (a == "--requests") {
            const char* v = need("--requests"); if (!v) return false; o.requests = std::atoi(v);
        } else if (a == "--keys") {
            const char* v = need("--keys"); if (!v) return false; o.keys = std::atoi(v);
        } else if (a == "--timeout") {
            const char* v = need("--timeout"); if (!v) return false; o.timeout = std::atoi(v);
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
    if (o.threads < 1) o.threads = 1;
    if (o.requests < 1) o.requests = 1;
    if (o.keys < 1) o.keys = 1;

    std::string host;
    std::uint16_t port = 0;
    if (!split_host_port(o.target, host, port)) {
        std::cerr << "无效的 --target: " << o.target << "\n";
        return 1;
    }

    std::atomic<std::uint64_t> ok{0};
    std::atomic<std::uint64_t> found{0};

    const int per = o.requests / o.threads;
    const int rem = o.requests % o.threads;
    const std::uint32_t base_seed = 12345;

    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(o.threads));

    for (int t = 0; t < o.threads; ++t) {
        const int n = per + (t < rem ? 1 : 0);
        workers.emplace_back([&, t, n]() {
            std::mt19937 rng(base_seed + static_cast<std::uint32_t>(t));
            std::uniform_int_distribution<int> dist(0, o.keys - 1);
            for (int i = 0; i < n; ++i) {
                const std::string key = "key" + std::to_string(dist(rng));
                std::string body;
                if (HttpClient::Get(host, port, "/api/" + o.group + "/" + key, body, o.timeout)) {
                    ++ok;
                    // 解析 {"value":"...","found":true}
                    std::string value;
                    bool f = false;
                    if (DecodeResult(body, value, f) && f) {
                        ++found;
                    }
                }
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }
    const auto t1 = std::chrono::steady_clock::now();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double qps = secs > 0 ? static_cast<double>(ok.load()) / secs : 0.0;
    const double found_rate = ok.load() > 0
        ? static_cast<double>(found.load()) / static_cast<double>(ok.load())
        : 0.0;

    std::cout << "====== 压测结果 ======\n";
    std::cout << "target       : " << o.target << "\n";
    std::cout << "threads      : " << o.threads << "\n";
    std::cout << "requests     : " << o.requests << " (ok=" << ok.load() << ")\n";
    std::cout << "elapsed      : " << std::fixed << std::setprecision(3) << secs << " s\n";
    std::cout << "QPS          : " << std::fixed << std::setprecision(0) << qps << "\n";
    std::cout << "found 占比   : " << std::fixed << std::setprecision(3) << found_rate << "\n";

    // 拉取目标节点的统计（含本地命中率 / 回源次数）。
    std::string stats;
    if (HttpClient::Get(host, port, "/stats", stats, o.timeout)) {
        std::cout << "node /stats  : " << stats << "\n";
    }
    return 0;
}