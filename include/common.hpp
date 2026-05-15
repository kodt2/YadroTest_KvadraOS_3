#pragma once

#include <atomic>
#include <cstdint>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>

namespace accel {

struct AccelData {
    std::int64_t timestamp{};
    double x{};
    double y{};
    double z{};
};

struct ModuleData {
    std::int64_t timestamp{};
    double module{};
};

class Logger {
public:
    explicit Logger(std::string component);
    void set_file(const std::string& path);
    void info(const std::string& message);
    void error(const std::string& message);

private:
    void write(const std::string& level, const std::string& message);

    std::string component_;
    std::mutex mutex_;
    std::ofstream file_;
};

struct Endpoint {
    std::string host = "127.0.0.1";
    int port = 0;
};

std::int64_t now_ms();
void install_signal_handlers();
bool should_stop();
void request_stop();
void daemonize(Logger& logger);

int create_server_socket(int port, Logger& logger);
int accept_client(int server_fd, Logger& logger, const std::string& role);
int connect_tcp(const std::string& host, int port, Logger& logger);
bool write_all(int fd, const std::string& data, Logger& logger);
bool read_line(int fd, std::string& out, Logger& logger);
void close_fd(int& fd);

std::string to_json_line(const AccelData& data);
std::string to_json_line(const ModuleData& data);
std::optional<AccelData> parse_accel_json(const std::string& line);
std::optional<ModuleData> parse_module_json(const std::string& line);
bool same_accel_rounded(const AccelData& lhs, const AccelData& rhs, int decimals);
std::string ensure_directory_for_file(const std::string& file_path);

} // namespace accel
