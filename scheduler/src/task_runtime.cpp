#include "task_runtime_impl.h"
#include <stdexcept>
#include <utility>

namespace backup {

// 构造阶段只做三件事：校验配置、补齐子模块工厂、注册任务事件观察者。
TaskRuntime::Impl::Impl(TaskManager& manager,
                        std::size_t workers,
                        std::size_t max_queue,
                        TaskRuntimeFactories task_factories)
    : task_manager(manager)
    , worker_count_value(workers)
    , max_queued_tasks(max_queue)
    , factories(std::move(task_factories)) {
    if (worker_count_value == 0) {
        throw std::invalid_argument("worker_count must be greater than zero");
    }
    if (max_queued_tasks == 0) {
        throw std::invalid_argument("max_queued_tasks must be greater than zero");
    }

    configure_default_factories();
    task_manager.set_observer(
        [this](const std::string& task_id, const Task& task, const std::string& change) {
            record_event(task_id, task, change);
        });
}

// Runtime 销毁时先走统一关闭流程，再解除 TaskManager 对本对象的回调引用。
TaskRuntime::Impl::~Impl() {
    shutdown();
    task_manager.set_observer({});
}

// 创建固定数量的 worker；worker 创建后会阻塞等待队列条件变量。
void TaskRuntime::Impl::start() {
    std::lock_guard<std::mutex> lock(mutex);
    if (started || stopping) {
        return;
    }

    started = true;
    workers.reserve(worker_count_value);
    for (std::size_t index = 0; index < worker_count_value; ++index) {
        workers.emplace_back([this] { worker_loop(); });
    }
    condition.notify_all();
}

// 停止时先取消尚未开始的队列任务，再等待正在运行的 worker 收尾。
void TaskRuntime::Impl::shutdown() {
    std::vector<std::string> cancelled_tasks;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping && workers.empty()) {
            return;
        }
        stopping = true;
        while (!queue.empty()) {
            cancelled_tasks.push_back(queue.front().task_id);
            queue.pop_front();
        }
    }

    for (const auto& task_id : cancelled_tasks) {
        task_manager.cancel_task(task_id);
    }
    condition.notify_all();

    for (std::thread& worker : workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }
    workers.clear();
}

TaskRuntime::TaskRuntime(TaskManager& task_manager,
                         std::size_t worker_count,
                         std::size_t max_queued_tasks,
                         TaskRuntimeFactories factories)
    : impl_(std::make_unique<Impl>(task_manager, worker_count, max_queued_tasks,
                                   std::move(factories))) {}

TaskRuntime::~TaskRuntime() = default;

// 对外生命周期 API：真正的队列和线程逻辑在 Impl 中。
void TaskRuntime::start() {
    impl_->start();
}

void TaskRuntime::shutdown() {
    impl_->shutdown();
}

TaskSubmission TaskRuntime::submit_backup(const BackupRequest& request) {
    return impl_->submit_backup(request);
}

TaskSubmission TaskRuntime::submit_restore(const RestoreRequest& request) {
    return impl_->submit_restore(request);
}

Task TaskRuntime::get_task(const std::string& task_id) const {
    return impl_->task_manager.get_task(task_id);
}

std::vector<TaskSnapshot> TaskRuntime::list_tasks() const {
    return impl_->list_tasks();
}

std::vector<TaskEvent> TaskRuntime::get_events(const std::string& task_id,
                                               uint64_t after_id) const {
    return impl_->get_events(task_id, after_id);
}

bool TaskRuntime::cancel_task(const std::string& task_id) {
    return impl_->task_manager.cancel_task(task_id);
}

std::size_t TaskRuntime::worker_count() const noexcept {
    return impl_->worker_count_value;
}

std::size_t TaskRuntime::max_queued_tasks() const noexcept {
    return impl_->max_queued_tasks;
}

std::size_t TaskRuntime::queued_task_count() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->queue.size();
}

}  // namespace backup
