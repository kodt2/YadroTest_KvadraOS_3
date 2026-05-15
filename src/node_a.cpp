#include "common.hpp"

#include <chrono>
#include <cmath>
#include <fstream>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
struct Config {
    std::string server_host = "127.0.0.1";
    int accel_port = 5000;
    int result_port = 5002;
    int frequency_hz = 50;
    bool daemon = false;
    std::string log_file;
    std::string module_file = "accel/module.log";
};

Config parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) throw std::runtime_error("missing value for " + name);
            return argv[++i];
        };
        if (arg == "--server-host") cfg.server_host = need_value(arg);
        else if (arg == "--accel-port") cfg.accel_port = std::stoi(need_value(arg));
        else if (arg == "--result-port") cfg.result_port = std::stoi(need_value(arg));
        else if (arg == "--frequency-hz") cfg.frequency_hz = std::stoi(need_value(arg));
        else if (arg == "--module-file") cfg.module_file = need_value(arg);
        else if (arg == "--daemon") cfg.daemon = true;
        else if (arg == "--log-file") cfg.log_file = need_value(arg);
        else throw std::runtime_error("unknown argument: " + arg);
    }
    if (cfg.frequency_hz <= 0) throw std::runtime_error("--frequency-hz must be positive");
    return cfg;
}

accel::AccelData emulate_accelerometer(std::uint64_t sequence) {
    constexpr double pi = 3.14159265358979323846;
    const double t = static_cast<double>(sequence) / 50.0;
    static thread_local std::mt19937 rng{std::random_device{}()};
    static thread_local std::normal_distribution<double> noise{0.0, 0.015};
    return accel::AccelData{
        accel::now_ms(),
        0.25 * std::sin(2.0 * pi * 0.7 * t) + noise(rng),
        9.807 + 0.15 * std::sin(2.0 * pi * 0.2 * t) + noise(rng),
        0.20 * std::cos(2.0 * pi * 0.5 * t) + noise(rng)};
}

void receive_modules_loop(const Config& cfg, accel::Logger& logger) {
    accel::ensure_directory_for_file(cfg.module_file);
    std::ofstream out(cfg.module_file, std::ios::app);
    if (!out) {
        logger.error("cannot open module log: " + cfg.module_file);
        accel::request_stop();
        return;
    }

    while (!accel::should_stop()) {
        int fd = accel::connect_tcp(cfg.server_host, cfg.result_port, logger);
        if (fd < 0) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        std::string line;
        while (!accel::should_stop() && accel::read_line(fd, line, logger)) {
            const auto module = accel::parse_module_json(line);
            if (!module) {
                logger.error("invalid module JSON from server: " + line);
                continue;
            }
            out << module->timestamp << ',' << module->module << '\n';
            out.flush();
            logger.info("module logged timestamp " + std::to_string(module->timestamp));
        }
        accel::close_fd(fd);
        if (!accel::should_stop()) {
            logger.info("reconnecting result channel after disconnect");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

void send_accel_loop(const Config& cfg, accel::Logger& logger) {
    const auto period = std::chrono::microseconds(1000000 / cfg.frequency_hz);
    std::uint64_t sequence = 0;
    while (!accel::should_stop()) {
        int fd = accel::connect_tcp(cfg.server_host, cfg.accel_port, logger);
        if (fd < 0) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            continue;
        }
        while (!accel::should_stop()) {
            const auto started = std::chrono::steady_clock::now();
            const auto data = emulate_accelerometer(sequence++);
            if (!accel::write_all(fd, accel::to_json_line(data), logger)) {
                break;
            }
            logger.info("sent accel timestamp " + std::to_string(data.timestamp));
            const auto elapsed = std::chrono::steady_clock::now() - started;
            if (elapsed < period) std::this_thread::sleep_for(period - elapsed);
        }
        accel::close_fd(fd);
        if (!accel::should_stop()) {
            logger.info("reconnecting accel channel after disconnect");
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}
} // namespace

int main(int argc, char** argv) {
    accel::Logger logger("node_a");
    try {
        const Config cfg = parse_args(argc, argv);
        if (!cfg.log_file.empty()) logger.set_file(cfg.log_file);
        if (cfg.daemon) accel::daemonize(logger);
        accel::install_signal_handlers();

        std::thread receiver(receive_modules_loop, std::cref(cfg), std::ref(logger));
        send_accel_loop(cfg, logger);
        if (receiver.joinable()) receiver.join();
    } catch (const std::exception& ex) {
        logger.error(ex.what());
        return 1;
    }
    return 0;
}
