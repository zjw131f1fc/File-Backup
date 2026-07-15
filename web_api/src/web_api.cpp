#include "web_api/web_api.h"
#include <utility>

namespace backup {

WebApi::WebApi(TaskRuntime& runtime, ApiConfig config)
    : runtime_(runtime), config_(std::move(config)) {}

ApiResponse WebApi::handle(const std::string&, const std::string&, const std::string&) {
    return {};
}

void WebApi::mount(httplib::Server&) {}

struct WebApiServer::Impl {};

WebApiServer::WebApiServer(TaskRuntime& runtime, ApiConfig config)
    : impl_(std::make_unique<Impl>()) {
    (void)runtime;
    (void)config;
}

WebApiServer::~WebApiServer() = default;

bool WebApiServer::start() { return false; }

void WebApiServer::stop() {}

int WebApiServer::port() const noexcept { return 0; }

}  // namespace backup
