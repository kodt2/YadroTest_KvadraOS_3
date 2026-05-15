#include "common.hpp"

#include <chrono>
#include <cmath>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
struct Config {
    std::string server_host = "127.0.0.1";
    int server_port = 5001;
    bool daemon = false;
    std::string log_file;
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
        else if (arg == "--server-port") cfg.server_port = std::stoi(need_value(arg));
        else if (arg == "--daemon") cfg.daemon = true;
        else if (arg == "--log-file") cfg.log_file = need_value(arg);
        else throw std::runtime_error("unknown argument: " + arg);
    }
    return cfg;
}
} // namespace

int main(int argc, char** argv) {
    accel::Logger logger("node_b");
    try {
        const Config cfg = parse_args(argc, argv);
        if (!cfg.log_file.empty()) logger.set_file(cfg.log_file);
        if (cfg.daemon) accel::daemonize(logger);
        accel::install_signal_handlers();

        while (!accel::should_stop()) {
            int fd = accel::connect_tcp(cfg.server_host, cfg.server_port, logger);
            if (fd < 0) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
                continue;
            }
            std::string line;
            while (!accel::should_stop() && accel::read_line(fd, line, logger)) {
                const auto data = accel::parse_accel_json(line);
                if (!data) {
                    logger.error("invalid accel JSON from server: " + line);
                    continue;
                }
                const double module = std::sqrt(data->x * data->x + data->y * data->y + data->z * data->z);
                const accel::ModuleData response{data->timestamp, module};
                if (!accel::write_all(fd, accel::to_json_line(response), logger)) {
                    break;
                }
                logger.info("processed timestamp " + std::to_string(data->timestamp));
            }
            accel::close_fd(fd);
            if (!accel::should_stop()) {
                logger.info("reconnecting to server after disconnect");
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
        }
    } catch (const std::exception& ex) {
        logger.error(ex.what());
        return 1;
    }
    return 0;
}
