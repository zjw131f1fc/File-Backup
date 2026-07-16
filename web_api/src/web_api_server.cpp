#include "web_api/web_api.h"
#include <httplib.h>
#include <chrono>
#include <thread>
#include <utility>

namespace backup {

struct WebApiServer::Impl {
    Impl(TaskRuntime& runtime, ApiConfig config)
        : runtime(runtime)
        , config(std::move(config))
        , api(runtime, this->config) {}

    TaskRuntime& runtime;
    ApiConfig config;
    WebApi api;
    httplib::Server server;
    std::thread thread;
    int bound_port = 0;
    bool started = false;
};

WebApiServer::WebApiServer(TaskRuntime& runtime, ApiConfig config)
    : impl_(std::make_unique<Impl>(runtime, std::move(config))) {}

// 析构时确保后台 HTTP 线程已经停止。
WebApiServer::~WebApiServer() {
    stop();
}

// 启动 Runtime 和 HTTP 服务；端口为 0 时先让 httplib 选择可用端口。
bool WebApiServer::start() {
    if (impl_->started) return true;
    impl_->runtime.start();
    impl_->api.mount(impl_->server);
    if (impl_->config.port == 0) {
        impl_->bound_port = impl_->server.bind_to_any_port(impl_->config.bind_address);
    } else if (impl_->server.bind_to_port(impl_->config.bind_address, impl_->config.port)) {
        impl_->bound_port = impl_->config.port;
    } else {
        impl_->bound_port = 0;
    }
    if (impl_->bound_port <= 0) {
        impl_->runtime.shutdown();
        return false;
    }
    impl_->started = true;
    impl_->thread = std::thread([this] { impl_->server.listen_after_bind(); });
    return true;
}

// 停止 HTTP 服务，并随后停止 Runtime 的 worker。
void WebApiServer::stop() {
    if (!impl_->started) return;
    impl_->server.stop();
    if (impl_->thread.joinable()) impl_->thread.join();
    impl_->started = false;
    impl_->runtime.shutdown();
}

// 返回 httplib 实际绑定的端口。
int WebApiServer::port() const noexcept {
    return impl_->bound_port;
}

}  // namespace backup
