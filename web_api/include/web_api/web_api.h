#pragma once

#include "scheduler/task_runtime.h"
#include <memory>
#include <string>
#include <vector>

namespace httplib {
class Server;
}

namespace backup {

struct ApiConfig {
    std::string bind_address = "127.0.0.1";
    int port = 8080;
    std::vector<std::string> allowed_roots;
    std::string allowed_origin;
};

struct ApiResponse {
    int status = 500;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
};

class WebApi {
public:
    WebApi(TaskRuntime&, ApiConfig config = {});

    ApiResponse handle(const std::string& method,
                       const std::string& target,
                       const std::string& body = "");

    void mount(httplib::Server& server);

private:
    TaskRuntime& runtime_;
    ApiConfig config_;
};

class WebApiServer {
public:
    WebApiServer(TaskRuntime&, ApiConfig config = {});
    ~WebApiServer();

    WebApiServer(const WebApiServer&) = delete;
    WebApiServer& operator=(const WebApiServer&) = delete;

    bool start();
    void stop();
    int port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace backup
