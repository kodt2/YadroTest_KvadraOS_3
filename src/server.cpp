#include "common.hpp"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <sys/socket.h>

namespace {
struct Config {
    int accel_port = 5000;
    int b_port = 5001;
    int result_port = 5002;
    int duplicate_decimals = 3;
    bool daemon = false;
    std::string log_file;
};

Config parse_args(int argc, char** argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto need_value = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) {
                throw std::runtime_error("missing value for " + name);
            }
            return argv[++i];
        };
        if (arg == "--accel-port") cfg.accel_port = std::stoi(need_value(arg));
        else if (arg == "--b-port") cfg.b_port = std::stoi(need_value(arg));
        else if (arg == "--result-port") cfg.result_port = std::stoi(need_value(arg));
        else if (arg == "--duplicate-decimals") cfg.duplicate_decimals = std::stoi(need_value(arg));
        else if (arg == "--daemon") cfg.daemon = true;
        else if (arg == "--log-file") cfg.log_file = need_value(arg);
        else throw std::runtime_error("unknown argument: " + arg);
    }
    return cfg;
}

struct State {
    std::mutex queue_mutex;
    std::condition_variable queue_cv;
    std::deque<accel::AccelData> validated;

    std::mutex result_mutex;
    int a_result_fd = -1;
};

void enqueue_validated(State& state, const accel::AccelData& data) {
    {
        std::lock_guard<std::mutex> lock(state.queue_mutex);
        state.validated.push_back(data);
    }
    state.queue_cv.notify_one();
}

void forward_to_a(State& state, const accel::ModuleData& module, accel::Logger& logger) {
    std::lock_guard<std::mutex> lock(state.result_mutex);
    if (state.a_result_fd < 0) {
        logger.error("cannot forward module to A: result connection is absent");
        return;
    }
    if (!accel::write_all(state.a_result_fd, accel::to_json_line(module), logger)) {
        logger.error("lost A result connection");
        accel::close_fd(state.a_result_fd);
    }
}

void accept_a_result_loop(int listen_fd, State& state, accel::Logger& logger) {
    while (!accel::should_stop()) {
        int fd = accel::accept_client(listen_fd, logger, "node A result receiver");
        if (fd < 0) continue;
        std::lock_guard<std::mutex> lock(state.result_mutex);
        accel::close_fd(state.a_result_fd);
        state.a_result_fd = fd;
    }
}

void accept_a_accel_loop(int listen_fd, State& state, const Config& cfg, accel::Logger& logger) {
    while (!accel::should_stop()) {
        int fd = accel::accept_client(listen_fd, logger, "node A accel sender");
        if (fd < 0) continue;
        std::optional<accel::AccelData> previous;
        std::string line;
        while (!accel::should_stop() && accel::read_line(fd, line, logger)) {
            const auto data = accel::parse_accel_json(line);
            if (!data) {
                logger.error("invalid accel JSON: " + line);
                continue;
            }
            if (previous && accel::same_accel_rounded(*previous, *data, cfg.duplicate_decimals)) {
                logger.info("duplicate dropped at timestamp " + std::to_string(data->timestamp));
                continue;
            }
            previous = *data;
            enqueue_validated(state, *data);
            logger.info("validated accel timestamp " + std::to_string(data->timestamp));
        }
        logger.info("node A accel sender disconnected");
        accel::close_fd(fd);
    }
}

void handle_b_connection(int fd, State& state, accel::Logger& logger) {
    std::atomic_bool connected{true};
    std::thread sender([&] {
        while (!accel::should_stop() && connected.load()) {
            accel::AccelData data;
            {
                std::unique_lock<std::mutex> lock(state.queue_mutex);
                state.queue_cv.wait(lock, [&] { return accel::should_stop() || !state.validated.empty() || !connected.load(); });
                if (accel::should_stop() || !connected.load()) break;
                data = state.validated.front();
                state.validated.pop_front();
            }
            if (!accel::write_all(fd, accel::to_json_line(data), logger)) {
                connected.store(false);
                state.queue_cv.notify_all();
                break;
            }
        }
    });

    std::string line;
    while (!accel::should_stop() && connected.load() && accel::read_line(fd, line, logger)) {
        const auto module = accel::parse_module_json(line);
        if (!module) {
            logger.error("invalid module JSON from B: " + line);
            continue;
        }
        logger.info("module received from B timestamp " + std::to_string(module->timestamp));
        forward_to_a(state, *module, logger);
    }
    connected.store(false);
    shutdown(fd, SHUT_RDWR);
    state.queue_cv.notify_all();
    if (sender.joinable()) sender.join();
    accel::close_fd(fd);
    logger.info("node B disconnected");
}

void accept_b_loop(int listen_fd, State& state, accel::Logger& logger) {
    while (!accel::should_stop()) {
        int fd = accel::accept_client(listen_fd, logger, "node B processor");
        if (fd < 0) continue;
        handle_b_connection(fd, state, logger);
    }
}
} // namespace

int main(int argc, char** argv) {
    accel::Logger logger("server");
    try {
        const Config cfg = parse_args(argc, argv);
        if (!cfg.log_file.empty()) logger.set_file(cfg.log_file);
        if (cfg.daemon) accel::daemonize(logger);
        accel::install_signal_handlers();

        State state;
        int accel_fd = accel::create_server_socket(cfg.accel_port, logger);
        int b_fd = accel::create_server_socket(cfg.b_port, logger);
        int result_fd = accel::create_server_socket(cfg.result_port, logger);

        std::thread a_accel(accept_a_accel_loop, accel_fd, std::ref(state), std::cref(cfg), std::ref(logger));
        std::thread a_result(accept_a_result_loop, result_fd, std::ref(state), std::ref(logger));
        std::thread b(accept_b_loop, b_fd, std::ref(state), std::ref(logger));

        a_accel.join();
        a_result.join();
        b.join();
    } catch (const std::exception& ex) {
        logger.error(ex.what());
        return 1;
    }
    return 0;
}
