#include "web_api/web_api.h"
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

namespace {

std::atomic<bool> running{true};

// 收到终止信号时通知主循环退出，避免直接中断服务线程。
void stop_handler(int) {
    running = false;
}

// 将命令行端口解析为 0 到 65535 范围内的整数。
bool parse_port(const char* value, int& port) {
    char* end = nullptr;
    const long parsed = std::strtol(value, &end, 10);
    if (end == value || *end != '\0' || parsed < 0 || parsed > 65535) {
        return false;
    }
    port = static_cast<int>(parsed);
    return true;
}

}  // namespace

// 读取命令行配置，启动 Runtime 和 WebApiServer，并等待终止信号。
int main(int argc, char** argv) {
    backup::ApiConfig config;
    for (int index = 1; index < argc; ++index) {
        const std::string argument = argv[index];
        if (argument == "--port" && index + 1 < argc) {
            if (!parse_port(argv[++index], config.port)) {
                std::cerr << "invalid port\n";
                return 2;
            }
        } else if (argument == "--root" && index + 1 < argc) {
            config.allowed_roots.push_back(argv[++index]);
        } else if (argument == "--origin" && index + 1 < argc) {
            config.allowed_origin = argv[++index];
        } else if (argument == "--help") {
            std::cout << "usage: backup_web_server [--port PORT] [--root PATH] [--origin ORIGIN]\n";
            return 0;
        } else {
            std::cerr << "unknown argument: " << argument << "\n";
            return 2;
        }
    }

    std::signal(SIGINT, stop_handler);
    std::signal(SIGTERM, stop_handler);
    backup::TaskManager task_manager;
    backup::TaskRuntime runtime(task_manager);
    backup::WebApiServer server(runtime, config);
    if (!server.start()) {
        std::cerr << "failed to start web api server\n";
        return 1;
    }

    std::cout << "backup web api listening on " << config.bind_address
              << ":" << server.port() << "\n";
    while (running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    server.stop();
    return 0;
}
