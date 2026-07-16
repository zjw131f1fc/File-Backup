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
    // HTTP 服务监听地址和端口。
    std::string bind_address = "127.0.0.1";
    int port = 8080;
    // 文件浏览器允许访问的根目录；为空表示不限制根目录。
    std::vector<std::string> allowed_roots;
    // 前端跨端口访问时允许的 Origin；为空表示不添加 CORS 头。
    std::string allowed_origin;
};

struct ApiResponse {
    // WebApi::handle 返回给 HTTP 层的统一响应。
    int status = 500;
    std::string content_type = "application/json; charset=utf-8";
    std::string body;
};

class WebApi {
public:
    // 绑定任务运行时和 API 配置；WebApi 不拥有 Runtime 的生命周期。
    WebApi(TaskRuntime&, ApiConfig config = {});

    // 根据 HTTP 方法、路径、查询参数和请求体分发一个 API 请求。
    ApiResponse handle(const std::string& method,
                       const std::string& target,
                       const std::string& body = "");

    // 把 API 路由注册到 httplib Server，包括普通请求和 SSE 进度流。
    void mount(httplib::Server& server);

private:
    // 解析并提交 /api/backup 或 /api/restore 请求。
    ApiResponse handle_task_submission(const std::string& path,
                                       const std::string& body);

    TaskRuntime& runtime_;
    ApiConfig config_;
};

class WebApiServer {
public:
    // 创建 HTTP 服务对象；真正的 httplib Server 保存在 Impl 中。
    WebApiServer(TaskRuntime&, ApiConfig config = {});
    // 停止服务并释放内部资源。
    ~WebApiServer();

    WebApiServer(const WebApiServer&) = delete;
    WebApiServer& operator=(const WebApiServer&) = delete;

    // 启动 HTTP 监听；成功后可通过 port() 获取实际端口。
    bool start();
    // 停止 HTTP 监听并等待服务线程退出。
    void stop();
    // 返回实际绑定端口，支持配置 port=0 自动选择端口。
    int port() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace backup
