// 压测客户端。
//
// 为什么不用 wrk / ab / hey：
//   - 压的必须是**本仓的服务端**，包括它自己的 keep-alive 与 TLS。用外部工具就得
//     假设它对 HTTP/1.1 的实现与服务端一致（尤其 TLS），那部分差异会被算进服务端；
//   - 需要拿到客户端的连接复用状态（IdleConnections）来发现「池子漏连接」；
//   - 需要和服务端共用同一套 JSON 编解码，免得把解析错误算成服务端错误。
//
// 三条口径（换掉会得到不一样的数字，报告里必须一起交出去）：
//
//   1. **先预热，预热请求不进统计。** 第一批请求要付 getaddrinfo、TCP 握手、
//      （TLS 时）完整握手、连接池建立。这些成本是真的，但不属于稳态；混进分位数
//      的结果是 P999 由握手决定，换个 DNS 缓存就变。
//   2. **只统计成功请求的延迟。** 失败请求的「延迟」是超时上限，混进来会让 P99
//      变成 2000ms，而且失败越多、P99 越接近超时值——一个由错误率驱动的延迟
//      指标。失败率单独报。
//   3. **微秒，不是毫秒。** loopback 上的差异常在几十微秒，毫秒分辨率下全是 0 或 1。

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "http_client.hpp"
#include "json.hpp"
#include "latency_stats.hpp"
#include "tls.hpp"

using namespace dcache;

namespace {

// 严格解析非负整数。不用 atoi：`--capacity 1OOO`（字母 O）会被 atoi 解析成 0，
// 于是节点静默变成「容量 0、不缓存」，而压测报告上只会看到一个很低的命中率——
// 数字错了，程序不报错。命令行参数也值得用 strtol，理由和网络输入一样。
bool parse_nonneg_int(const char* s, int& out) {
    if (s == nullptr || *s == '\0') {
        return false;
    }
    char* end    = nullptr;
    errno        = 0;
    const long v = std::strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE || v < 0) {
        return false;
    }
    if (v > 1000000000L) {  // 10 亿，够大，同时挡住「手抖多打几个 0」
        return false;
    }
    out = static_cast<int>(v);
    return true;
}

// 解析 "host:port"。与 peer_pool::split_addr 同样的教训：atoi 溢出是 UB。
bool split_host_port(const std::string& addr, std::string& host, std::uint16_t& port) {
    const std::size_t pos = addr.rfind(':');
    if (pos == std::string::npos || pos == 0 || pos + 1 >= addr.size()) {
        return false;
    }
    host = addr.substr(0, pos);

    char* end    = nullptr;
    const long p = std::strtol(addr.c_str() + pos + 1, &end, 10);
    if (end == addr.c_str() + pos + 1 || *end != '\0' || p <= 0 || p > 65535) {
        return false;
    }
    port = static_cast<std::uint16_t>(p);
    return true;
}

struct Options {
    std::string target = "127.0.0.1:8001";
    std::string group  = "scores";
    int threads        = 2;
    int requests       = 20000;
    int keys           = 1000;
    int timeout        = 2000;
    // 预热请求数。默认取测量量的 10%：足够让连接池建满、JIT/LRU 进入稳态，
    // 又不至于在 20 万请求的压测里额外多跑 2 万。
    int warmup     = -1;  // -1 = 自动
    bool keepalive = true;
    std::string tls_ca;  // 非空则走 TLS（服务端也要开）
};

void usage(const char* argv0) {
    std::cerr << "usage: " << argv0
              << " [options]\n"
                 "  --target <host:port>   目标节点地址 (默认 127.0.0.1:8001)\n"
                 "  --group <name>         缓存分组名 (默认 scores)\n"
                 "  --threads <n>          并发线程数 (默认 2，与 CI runner 的 vCPU 数一致)\n"
                 "  --requests <n>         测量阶段的请求数，不含预热 (默认 20000)\n"
                 "  --keys <n>             随机 key 空间大小 (默认 1000)\n"
                 "  --warmup <n>           预热请求数，不计入统计 (默认 requests/10)\n"
                 "  --timeout <ms>         单请求超时 (默认 2000)\n"
                 "  --no-keepalive         关闭连接复用（用于对比连接池的价值）\n"
                 "  --tls-ca <pem>         节点间 TLS 的 CA 文件（服务端要同时开 TLS）\n"
                 "\n"
                 "注意：节点的 --capacity 默认只有 100。key 空间明显大于容量时，\n"
                 "命中率的上限就是 容量/key空间（默认 100/1000 = 10%），此时压的是\n"
                 "LRU 淘汰而不是缓存。做对比实验请显式给 --capacity。\n";
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto need           = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::cerr << "缺少 " << name << " 的值\n";
                return nullptr;
            }
            return argv[++i];
        };
        auto int_of = [&](const char* name, int& out) -> bool {
            const char* v = need(name);
            if (v == nullptr) {
                return false;
            }
            if (!parse_nonneg_int(v, out)) {
                std::cerr << "无效的 " << name << ": " << v << "（需要非负整数）\n";
                return false;
            }
            return true;
        };

        if (a == "--target") {
            const char* v = need("--target");
            if (v == nullptr) {
                return false;
            }
            o.target = v;
        } else if (a == "--group") {
            const char* v = need("--group");
            if (v == nullptr) {
                return false;
            }
            o.group = v;
        } else if (a == "--threads") {
            if (!int_of("--threads", o.threads)) {
                return false;
            }
        } else if (a == "--requests") {
            if (!int_of("--requests", o.requests)) {
                return false;
            }
        } else if (a == "--keys") {
            if (!int_of("--keys", o.keys)) {
                return false;
            }
        } else if (a == "--warmup") {
            if (!int_of("--warmup", o.warmup)) {
                return false;
            }
        } else if (a == "--timeout") {
            if (!int_of("--timeout", o.timeout)) {
                return false;
            }
        } else if (a == "--no-keepalive") {
            o.keepalive = false;
        } else if (a == "--tls-ca") {
            const char* v = need("--tls-ca");
            if (v == nullptr) {
                return false;
            }
            o.tls_ca = v;
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

struct Endpoint {
    std::string host;
    std::uint16_t port = 0;
};

// 一轮测量的结果（预热轮用完即弃，但走的是同一条代码路径——
// 两条路径各写一遍的话，预热就压不到真正会跑的代码）。
struct PhaseResult {
    std::uint64_t ok          = 0;
    std::uint64_t failed      = 0;
    std::uint64_t found       = 0;
    std::uint64_t undecodable = 0;
    LatencyStats latency;
};

std::string KeyOf(int index) {
    return "key" + std::to_string(index);
}

// 从 /stats 的扁平 JSON 里取一个数值字段。
//
// 这不是通用 JSON 解析器，只认本仓 /stats 的形状（`{"scores":{"hit_rate":0.99,...}}`）。
// 用它而不是 JsonFindString，是因为 /stats 里的值是数字不是字符串——JsonFindString
// 只找带引号的值，拿数字会安全失败。刻意不加一个「数字版 find」进 json.hpp：
// 那里是协议层，指标输出是另一件事，混在一起以后会有人拿它去解对端报文。
bool FindNumber(const std::string& json, const std::string& key, double& out) {
    const std::string needle = "\"" + key + "\":";
    const std::size_t pos    = json.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    const char* begin = json.c_str() + pos + needle.size();
    char* end         = nullptr;
    const double v    = std::strtod(begin, &end);
    if (end == begin) {
        return false;
    }
    out = v;
    return true;
}

PhaseResult RunPhase(const Options& o, const Endpoint& ep, int requests, std::uint32_t seed) {
    PhaseResult total;

    const int threads = requests < o.threads ? requests : o.threads;
    if (threads <= 0) {
        return total;
    }

    const int per = requests / threads;
    const int rem = requests % threads;

    std::vector<PhaseResult> partial(static_cast<std::size_t>(threads));
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(threads));

    for (int t = 0; t < threads; ++t) {
        const int n = per + (t < rem ? 1 : 0);
        workers.emplace_back([&, t, n]() {
            PhaseResult& out = partial[static_cast<std::size_t>(t)];
            if (n <= 0) {
                return;
            }
            out.latency.Reserve(static_cast<std::size_t>(n));

            std::mt19937 rng(seed + static_cast<std::uint32_t>(t));
            std::uniform_int_distribution<int> dist(0, o.keys - 1);

            for (int i = 0; i < n; ++i) {
                const std::string path = "/api/" + o.group + "/" + KeyOf(dist(rng));

                std::string body;
                const auto t0 = std::chrono::steady_clock::now();
                const bool ok = HttpClient::Get(ep.host, ep.port, path, body, o.timeout);
                const auto t1 = std::chrono::steady_clock::now();

                if (!ok) {
                    ++out.failed;
                    continue;  // 失败请求的「延迟」是超时上限，不进分位数
                }
                ++out.ok;
                out.latency.Add(
                    std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());

                std::string value;
                bool f = false;
                if (DecodeResult(body, value, f)) {
                    if (f) {
                        ++out.found;
                    }
                } else {
                    ++out.undecodable;
                }
            }
        });
    }

    for (auto& w : workers) {
        w.join();
    }

    for (auto& p : partial) {
        total.ok += p.ok;
        total.failed += p.failed;
        total.found += p.found;
        total.undecodable += p.undecodable;
        total.latency.Merge(p.latency);
    }
    return total;
}

void PrintLatency(const LatencyStats& s) {
    std::cout << "\n延迟（微秒，n=" << s.Count() << "，最近秩，不含失败请求）\n";
    std::cout << "  " << std::left << std::setw(12) << "min" << ": " << s.Permille(0) << "\n";
    std::cout << "  " << std::left << std::setw(12) << "p50" << ": " << s.Percentile(50) << "\n";
    std::cout << "  " << std::left << std::setw(12) << "p90" << ": " << s.Percentile(90) << "\n";
    std::cout << "  " << std::left << std::setw(12) << "p99" << ": " << s.Percentile(99) << "\n";
    std::cout << "  " << std::left << std::setw(12) << "p999" << ": " << s.Permille(999) << "\n";
    std::cout << "  " << std::left << std::setw(12) << "max" << ": " << s.Max() << "\n";
    std::cout << "  " << std::left << std::setw(12) << "mean" << ": " << std::fixed
              << std::setprecision(1) << s.Mean() << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    if (!parse_args(argc, argv, o)) {
        return 1;
    }
    if (o.threads < 1) {
        o.threads = 1;
    }
    if (o.requests < 1) {
        o.requests = 1;
    }
    if (o.keys < 1) {
        o.keys = 1;
    }
    if (o.warmup < 0) {
        o.warmup = o.requests / 10;
    }

    Endpoint ep;
    if (!split_host_port(o.target, ep.host, ep.port)) {
        std::cerr << "无效的 --target: " << o.target << "\n";
        return 1;
    }

    // 连接复用与 TLS 都必须在并发调用 Get 之前完成（HttpClient 的公开契约）。
    HttpClient::SetKeepAlive(o.keepalive);

    if (!o.tls_ca.empty()) {
        std::string err;
        if (!HttpClient::EnableTls(tls::ClientConfig{o.tls_ca}, err)) {
            std::cerr << "TLS 初始化失败: " << err << "\n";
            return 1;
        }
    }

    std::cout << "====== 压测开始 ======\n";
    std::cout << "target     : " << o.target << "\n";
    std::cout << "transport  : " << (HttpClient::TlsEnabled() ? "tls" : "tcp")
              << " (keep-alive=" << (o.keepalive ? "on" : "off") << ")\n";
    std::cout << "threads    : " << o.threads << "\n";
    std::cout << "requests   : " << o.requests << " (另有预热 " << o.warmup << " 次，不计入)\n";
    std::cout << "keys       : " << o.keys << "\n";

    // 预热：连接池建满、服务端缓存进入稳态。结果直接丢掉。
    if (o.warmup > 0) {
        const PhaseResult warm = RunPhase(o, ep, o.warmup, 1);
        if (warm.ok == 0 && warm.failed > 0) {
            std::cerr << "预热阶段全部失败：目标是否在监听？端口写对了吗？\n";
            return 1;
        }
        std::cout << "warmup     : " << warm.ok << " ok / " << warm.failed << " failed\n";
    }

    const auto t0       = std::chrono::steady_clock::now();
    const PhaseResult r = RunPhase(o, ep, o.requests, 12345);
    const auto t1       = std::chrono::steady_clock::now();

    const double secs = std::chrono::duration<double>(t1 - t0).count();
    const double qps  = secs > 0 ? static_cast<double>(r.ok) / secs : 0.0;
    const double found_rate =
        r.ok > 0 ? static_cast<double>(r.found) / static_cast<double>(r.ok) : 0.0;

    std::cout << "\n====== 压测结果 ======\n";
    std::cout << "ok / failed: " << r.ok << " / " << r.failed << "\n";
    std::cout << "elapsed    : " << std::fixed << std::setprecision(3) << secs << " s\n";
    std::cout << "QPS        : " << std::fixed << std::setprecision(0) << qps << "\n";
    std::cout << "found 占比 : " << std::fixed << std::setprecision(3) << found_rate << "\n";
    if (r.undecodable > 0) {
        // 服务端回了 200 但 body 解析不了 —— 那是协议层的问题，不该混进命中率里沉默。
        std::cout << "解析失败   : " << r.undecodable << "（响应体不是预期 JSON）\n";
    }

    PrintLatency(r.latency);

    // 拉取目标节点的统计（含本地命中率 / 回源次数）。
    // 必须放在诊断之前：下面「容量 < key 空间」那条提示要用 cache_size。
    //
    // 顺带说明一个容易读错的数：上面的 `found 占比` **不是命中率**。
    // 它只表示「响应里有值」——mock 数据源对每个 key 都有值，所以它恒等于 1.0。
    // 真正的命中率只有节点知道（local_hit / (hit + miss)），在 /stats 里。
    std::string stats;
    if (HttpClient::Get(ep.host, ep.port, "/stats", stats, o.timeout)) {
        std::cout << "\nnode /stats : " << stats << "\n";
    }
    std::cout << "池内空闲连接: " << HttpClient::IdleConnections() << "\n";

    // 命中率异常时把最可能的原因直接说出来。默认 --capacity 是 100，而这里的
    // --keys 默认 1000 —— 不提示的话，压出来的「命中率 10%」会被当成缓存不行。
    //
    // 判据用 cache_size 而不是 hit_rate：命中率低有很多原因（TTL 太短、key 分布
    // 太散、别的节点在转发），而「缓存条目数 < key 空间」是一个确定的上界——
    // 命中率上限就是 cache_size / keys，跟调优无关。
    double hit_rate      = -1.0;
    double cache_size    = -1.0;
    const bool got_hit   = FindNumber(stats, "hit_rate", hit_rate);
    const bool got_csize = FindNumber(stats, "cache_size", cache_size);

    if (got_csize && cache_size >= 0.0 && static_cast<double>(o.keys) > cache_size) {
        const double ceiling = cache_size / static_cast<double>(o.keys);
        std::cerr << "\n提示：节点缓存条目数（" << static_cast<long long>(cache_size)
                  << "）小于 key 空间（" << o.keys << "），命中率上限约 " << std::fixed
                  << std::setprecision(1) << ceiling * 100.0
                  << "%。\n      节点 --capacity 默认只有 100；做命中率实验请显式给它"
                     "（例如 --capacity 10000 --keys 1000）。\n";
    }

    if (r.failed > 0) {
        std::cerr << "\n注意：" << r.failed
                  << " 个请求失败，它们的延迟未计入分位数"
                     "（失败率见上）。P99 偏低可能是「失败的那部分没算」，不是变快了。\n";
    }

    // 单独一行给脚本取数。RESULT 那行只放延迟与吞吐，节点侧计数器放这里——
    // 混在一起的话，两边的字段顺序就成了一个谁也不敢改的紧耦合。
    if (got_hit || got_csize) {
        std::cout << "\nSTATS";
        if (got_hit) {
            std::cout << " hit_rate=" << std::fixed << std::setprecision(4) << hit_rate;
        }
        if (got_csize) {
            std::cout << " cache_size=" << std::fixed << std::setprecision(0) << cache_size;
        }
        std::cout << "\n";
    }

    // 机器可读的一行：CI 的注解与 scripts/bench_perf.sh 都靠它取数，
    // 而不是去 grep 上面那张人看的表格（改一次对齐就全断了）。
    std::cout << "\nRESULT qps=" << static_cast<long long>(qps + 0.5) << " ok=" << r.ok
              << " failed=" << r.failed << " found=" << r.found << " n=" << r.latency.Count()
              << " p50=" << r.latency.Percentile(50) << " p90=" << r.latency.Percentile(90)
              << " p99=" << r.latency.Percentile(99) << " p999=" << r.latency.Permille(999)
              << " max=" << r.latency.Max() << "\n";

    HttpClient::CloseIdleConnections();
    return 0;
}
