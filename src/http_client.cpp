#include "http_client.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <cstdlib>

namespace dcache {

namespace {

bool set_timeout(int fd, int ms, int optname) {
    struct timeval tv;
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    return ::setsockopt(fd, SOL_SOCKET, optname, &tv, sizeof(tv)) == 0;
}

int connect_timeout(const std::string& host, std::uint16_t port, int timeout_ms) {
    struct addrinfo hints;
    std::memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo* res = nullptr;
    if (::getaddrinfo(host.c_str(), std::to_string(port).c_str(), &hints, &res) != 0) {
        return -1;
    }

    int fd = ::socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        ::freeaddrinfo(res);
        return -1;
    }

    // 非阻塞 connect + poll，实现可控超时。
    int flags = ::fcntl(fd, F_GETFL, 0);
    ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    int rc = ::connect(fd, res->ai_addr, res->ai_addrlen);
    if (rc < 0 && errno != EINPROGRESS) {
        ::close(fd);
        ::freeaddrinfo(res);
        return -1;
    }

    struct pollfd pfd;
    pfd.fd     = fd;
    pfd.events = POLLOUT;
    int prc    = ::poll(&pfd, 1, timeout_ms);

    int so_error = 0;
    socklen_t slen = sizeof(so_error);
    ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &slen);

    ::freeaddrinfo(res);

    if (prc <= 0 || so_error != 0) {
        ::close(fd);
        return -1;
    }

    ::fcntl(fd, F_SETFL, flags); // 恢复阻塞
    set_timeout(fd, timeout_ms, SO_RCVTIMEO);
    set_timeout(fd, timeout_ms, SO_SNDTIMEO);
    return fd;
}

bool send_all(int fd, const char* data, std::size_t len) {
    std::size_t off = 0;
    while (off < len) {
        ssize_t n = ::send(fd, data + off, len - off, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        off += static_cast<std::size_t>(n);
    }
    return true;
}

// 读入响应头（以 \r\n\r\n 结束）。
bool read_headers(int fd, std::string& buf, std::size_t& header_end) {
    char tmp[4096];
    for (;;) {
        std::size_t pos = buf.find("\r\n\r\n");
        if (pos != std::string::npos) {
            header_end = pos + 4;
            return true;
        }
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n == 0) {
            // 连接被关闭但可能已经收到完整头。
            std::size_t pos2 = buf.find("\r\n\r\n");
            if (pos2 != std::string::npos) { header_end = pos2 + 4; return true; }
            return false;
        }
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        buf.append(tmp, static_cast<std::size_t>(n));
    }
}

// 找到响应状态码；content_length 取 Content-Length（若存在），否则 -1。
int parse_response(const std::string& header_block, long& content_length) {
    content_length = -1;

    const std::size_t line_end = header_block.find("\r\n");
    const std::string reqline  = header_block.substr(0, line_end);

    // "HTTP/1.1 200 OK"
    const std::size_t sp1 = reqline.find(' ');
    if (sp1 == std::string::npos) return -1;
    const std::size_t sp2 = reqline.find(' ', sp1 + 1);
    const std::string status_str = reqline.substr(sp1 + 1, sp2 - sp1 - 1);
    const int status = std::atoi(status_str.c_str());
    if (status <= 0) return -1;

    // 大小写不敏感地找 Content-Length:（本项目服务器固定发送该头）。
    auto case_find = [&](const std::string& block, const char* needle) -> std::size_t {
        std::string lower(block.size(), '\0');
        for (std::size_t i = 0; i < block.size(); ++i) {
            char c = block[i];
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
            lower[i] = c;
        }
        return lower.find(needle);
    };

    const std::size_t clp = case_find(header_block, "content-length:");
    if (clp != std::string::npos) {
        const char* p = header_block.c_str() + clp + 15; // strlen("content-length:") == 15
        content_length = std::strtol(p, nullptr, 10);
    }
    return status;
}

} // namespace

bool HttpClient::Get(const std::string& host, std::uint16_t port,
                     const std::string& path, std::string& out, int timeout_ms,
                     const std::map<std::string, std::string>& extra_headers,
                     int* http_status) {
    if (http_status != nullptr) {
        *http_status = 0; // 默认：连状态行都没读到
    }

    int fd = connect_timeout(host, port, timeout_ms);
    if (fd < 0) {
        return false;
    }

    std::string req = "GET " + path + " HTTP/1.1\r\n";
    req += "Host: " + host + ":" + std::to_string(port) + "\r\n";
    req += "Accept: */*\r\n";
    for (const auto& h : extra_headers) {
        req += h.first + ": " + h.second + "\r\n";
    }
    req += "Connection: close\r\n\r\n";

    if (!send_all(fd, req.data(), req.size())) {
        ::close(fd);
        return false;
    }

    std::string buf;
    std::size_t header_end = 0;
    if (!read_headers(fd, buf, header_end)) {
        ::close(fd);
        return false;
    }

    long content_length = -1;
    int status = parse_response(buf.substr(0, header_end), content_length);
    if (status <= 0) {
        ::close(fd);
        return false;
    }
    if (http_status != nullptr) {
        *http_status = status;
    }

    std::string body;
    if (content_length >= 0) {
        // 先把头部之外已读到的字节计入 body。
        std::size_t body_so_far = buf.size() - header_end;
        if (body_so_far > static_cast<std::size_t>(content_length)) {
            body_so_far = static_cast<std::size_t>(content_length);
        }
        body.assign(buf, header_end, body_so_far);

        char tmp[4096];
        while (body.size() < static_cast<std::size_t>(content_length)) {
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n == 0) break;
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            body.append(tmp, static_cast<std::size_t>(n));
        }
        if (body.size() > static_cast<std::size_t>(content_length)) {
            body.resize(static_cast<std::size_t>(content_length));
        }
    } else {
        // 无 Content-Length：读到对端关闭。
        body.assign(buf, header_end, std::string::npos);
        char tmp[4096];
        for (;;) {
            ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
            if (n == 0) break;
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            body.append(tmp, static_cast<std::size_t>(n));
        }
    }

    ::close(fd);

    if (status != 200) {
        return false;
    }
    out = std::move(body);
    return true;
}

} // namespace dcache