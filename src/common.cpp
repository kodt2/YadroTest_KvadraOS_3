#include "common.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <netinet/in.h>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>

namespace accel {
namespace {
std::atomic_bool g_stop{false};

void signal_handler(int) {
    g_stop.store(true);
}

std::string timestamp_for_log() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&time, &tm);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

void set_recv_timeout(int fd) {
    timeval tv{};
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

std::optional<std::string> extract_number_text(const std::string& json, const std::string& key) {
    const std::regex pattern("\\\"" + key + "\\\"\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)");
    std::smatch match;
    if (!std::regex_search(json, match, pattern) || match.size() < 2) {
        return std::nullopt;
    }
    return match[1].str();
}

std::optional<double> extract_double(const std::string& json, const std::string& key) {
    const auto value = extract_number_text(json, key);
    if (!value) {
        return std::nullopt;
    }
    try {
        return std::stod(*value);
    } catch (...) {
        return std::nullopt;
    }
}

std::optional<std::int64_t> extract_int64(const std::string& json, const std::string& key) {
    const auto value = extract_number_text(json, key);
    if (!value) {
        return std::nullopt;
    }
    try {
        return std::stoll(*value);
    } catch (...) {
        return std::nullopt;
    }
}

long long rounded_scaled(double value, int decimals) {
    const double scale = std::pow(10.0, decimals);
    return static_cast<long long>(std::llround(value * scale));
}
} // namespace

Logger::Logger(std::string component) : component_(std::move(component)) {}

void Logger::set_file(const std::string& path) {
    std::lock_guard<std::mutex> lock(mutex_);
    ensure_directory_for_file(path);
    file_.open(path, std::ios::app);
    if (!file_) {
        throw std::runtime_error("cannot open log file: " + path);
    }
}

void Logger::info(const std::string& message) {
    write("INFO", message);
}

void Logger::error(const std::string& message) {
    write("ERROR", message);
}

void Logger::write(const std::string& level, const std::string& message) {
    std::lock_guard<std::mutex> lock(mutex_);
    const std::string line = timestamp_for_log() + " [" + level + "] [" + component_ + "] " + message;
    if (file_) {
        file_ << line << '\n';
        file_.flush();
    } else {
        std::cerr << line << '\n';
    }
}

std::int64_t now_ms() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

void install_signal_handlers() {
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    std::signal(SIGPIPE, SIG_IGN);
}

bool should_stop() {
    return g_stop.load();
}

void request_stop() {
    g_stop.store(true);
}

void daemonize(Logger& logger) {
    const pid_t first = fork();
    if (first < 0) {
        throw std::runtime_error("first fork failed");
    }
    if (first > 0) {
        std::exit(EXIT_SUCCESS);
    }
    if (setsid() < 0) {
        throw std::runtime_error("setsid failed");
    }
    const pid_t second = fork();
    if (second < 0) {
        throw std::runtime_error("second fork failed");
    }
    if (second > 0) {
        std::exit(EXIT_SUCCESS);
    }
    umask(0);
    if (chdir("/") != 0) {
        logger.error("chdir('/') failed: " + std::string(std::strerror(errno)));
    }
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    open("/dev/null", O_RDONLY);
    open("/dev/null", O_WRONLY);
    open("/dev/null", O_RDWR);
}

int create_server_socket(int port, Logger& logger) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        throw std::runtime_error("socket failed: " + std::string(std::strerror(errno)));
    }
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
    set_recv_timeout(fd);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd);
        throw std::runtime_error("bind failed on port " + std::to_string(port) + ": " + std::string(std::strerror(errno)));
    }
    if (listen(fd, 8) < 0) {
        close(fd);
        throw std::runtime_error("listen failed: " + std::string(std::strerror(errno)));
    }
    logger.info("listening on 0.0.0.0:" + std::to_string(port));
    return fd;
}

int accept_client(int server_fd, Logger& logger, const std::string& role) {
    sockaddr_in client{};
    socklen_t len = sizeof(client);
    int fd = accept(server_fd, reinterpret_cast<sockaddr*>(&client), &len);
    if (fd < 0) {
        if (!should_stop() && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            logger.error("accept for " + role + " failed: " + std::string(std::strerror(errno)));
        }
        return -1;
    }
    char ip[INET_ADDRSTRLEN]{};
    inet_ntop(AF_INET, &client.sin_addr, ip, sizeof(ip));
    logger.info(role + " connected from " + ip + ":" + std::to_string(ntohs(client.sin_port)));
    return fd;
}

int connect_tcp(const std::string& host, int port, Logger& logger) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        logger.error("socket failed: " + std::string(std::strerror(errno)));
        return -1;
    }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
        logger.error("invalid IPv4 address: " + host);
        close(fd);
        return -1;
    }
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        logger.error("connect to " + host + ":" + std::to_string(port) + " failed: " + std::string(std::strerror(errno)));
        close(fd);
        return -1;
    }
    set_recv_timeout(fd);
    logger.info("connected to " + host + ":" + std::to_string(port));
    return fd;
}

bool write_all(int fd, const std::string& data, Logger& logger) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = send(fd, data.data() + sent, data.size() - sent, 0);
        if (n <= 0) {
            logger.error("send failed: " + std::string(std::strerror(errno)));
            return false;
        }
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool read_line(int fd, std::string& out, Logger& logger) {
    out.clear();
    char c = '\0';
    while (true) {
        const ssize_t n = recv(fd, &c, 1, 0);
        if (n == 0) {
            return false;
        }
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                if (should_stop()) {
                    return false;
                }
                continue;
            }
            logger.error("recv failed: " + std::string(std::strerror(errno)));
            return false;
        }
        if (c == '\n') {
            return true;
        }
        if (c != '\r') {
            out.push_back(c);
        }
        if (out.size() > 4096) {
            logger.error("line is too long");
            return false;
        }
    }
}

void close_fd(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

std::string to_json_line(const AccelData& data) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6)
        << "{\"timestamp\":" << data.timestamp
        << ",\"x\":" << data.x
        << ",\"y\":" << data.y
        << ",\"z\":" << data.z << "}\n";
    return oss.str();
}

std::string to_json_line(const ModuleData& data) {
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(6)
        << "{\"timestamp\":" << data.timestamp
        << ",\"module\":" << data.module << "}\n";
    return oss.str();
}

std::optional<AccelData> parse_accel_json(const std::string& line) {
    auto timestamp = extract_int64(line, "timestamp");
    auto x = extract_double(line, "x");
    auto y = extract_double(line, "y");
    auto z = extract_double(line, "z");
    if (!timestamp || !x || !y || !z) {
        return std::nullopt;
    }
    return AccelData{*timestamp, *x, *y, *z};
}

std::optional<ModuleData> parse_module_json(const std::string& line) {
    auto timestamp = extract_int64(line, "timestamp");
    auto module = extract_double(line, "module");
    if (!timestamp || !module) {
        return std::nullopt;
    }
    return ModuleData{*timestamp, *module};
}

bool same_accel_rounded(const AccelData& lhs, const AccelData& rhs, int decimals) {
    return rounded_scaled(lhs.x, decimals) == rounded_scaled(rhs.x, decimals) &&
           rounded_scaled(lhs.y, decimals) == rounded_scaled(rhs.y, decimals) &&
           rounded_scaled(lhs.z, decimals) == rounded_scaled(rhs.z, decimals);
}

std::string ensure_directory_for_file(const std::string& file_path) {
    const std::filesystem::path path(file_path);
    const auto parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent);
    }
    return parent.string();
}

} // namespace accel
