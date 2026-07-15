#include "scheduler/task_runtime.h"
#include "scheduler/backup_scheduler.h"
#include "scheduler/restore_scheduler.h"
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

namespace backup {

namespace {

TaskSubmission failed_submission(const std::string& message) {
    TaskSubmission submission;
    submission.result.status = Status::FAILED;
    submission.result.message = message;
    return submission;
}

}  // namespace

struct TaskRuntime::Impl {
    enum class TaskKind {
        BACKUP,
        RESTORE,
    };

    struct Job {
        TaskKind kind;
        std::string task_id;
        BackupRequest backup_request;
        RestoreRequest restore_request;
    };

    Impl(TaskManager& manager, std::size_t workers, std::size_t max_queue)
        : task_manager(manager)
        , worker_count_value(workers)
        , max_queued_tasks(max_queue) {
        if (worker_count_value == 0) {
            throw std::invalid_argument("worker_count must be greater than zero");
        }
        if (max_queued_tasks == 0) {
            throw std::invalid_argument("max_queued_tasks must be greater than zero");
        }
    }

    ~Impl() {
        shutdown();
    }

    void start() {
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

    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (stopping && workers.empty()) {
                return;
            }
            stopping = true;
            while (!queue.empty()) {
                task_manager.cancel_task(queue.front().task_id);
                queue.pop_front();
            }
        }
        condition.notify_all();

        for (std::thread& worker : workers) {
            if (worker.joinable()) {
                worker.join();
            }
        }
        workers.clear();
    }

    TaskSubmission submit_backup(const BackupRequest& request) {
        return submit(Job{TaskKind::BACKUP, "", request, {}});
    }

    TaskSubmission submit_restore(const RestoreRequest& request) {
        return submit(Job{TaskKind::RESTORE, "", {}, request});
    }

    TaskSubmission submit(Job job) {
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping) {
            return failed_submission("task runtime is shutting down");
        }
        if (queue.size() >= max_queued_tasks) {
            return failed_submission("task queue is full");
        }

        if (job.kind == TaskKind::BACKUP) {
            job.task_id = task_manager.create_backup_task(job.backup_request);
        } else {
            job.task_id = task_manager.create_restore_task(job.restore_request);
        }
        const std::string submission_task_id = job.task_id;
        queue.push_back(std::move(job));
        condition.notify_one();

        TaskSubmission submission;
        submission.task_id = submission_task_id;
        submission.result.status = Status::SUCCESS;
        submission.result.message = "task accepted";
        return submission;
    }

    void worker_loop() {
        while (true) {
            Job job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                condition.wait(lock, [this] { return stopping || !queue.empty(); });
                if (stopping && queue.empty()) {
                    return;
                }
                job = std::move(queue.front());
                queue.pop_front();
            }

            if (!task_manager.start_task(job.task_id)) {
                continue;
            }

            try {
                if (job.kind == TaskKind::BACKUP) {
                    auto scanner = create_scanner();
                    auto filter = create_filter(job.backup_request.filter_rules);
                    auto writer = create_archive(job.backup_request.output_path);
                    if (!scanner || !filter || !writer) {
                        Result failure;
                        failure.status = Status::FAILED;
                        failure.message = "failed to create backup task modules";
                        task_manager.complete_task(job.task_id, failure);
                        continue;
                    }
                    BackupScheduler scheduler(task_manager, *scanner, *filter, *writer);
                    scheduler.run(job.task_id, job.backup_request);
                } else {
                    auto reader = open_archive(job.restore_request.archive_path);
                    auto restorer = create_restorer();
                    if (!reader || !restorer) {
                        Result failure;
                        failure.status = Status::FAILED;
                        failure.message = "failed to create restore task modules";
                        task_manager.complete_task(job.task_id, failure);
                        continue;
                    }
                    RestoreScheduler scheduler(task_manager, *reader, *restorer);
                    scheduler.run(job.task_id, job.restore_request);
                }
            } catch (const std::exception& error) {
                Result failure;
                failure.status = Status::FAILED;
                failure.message = std::string("task execution threw an exception: ") + error.what();
                task_manager.complete_task(job.task_id, failure);
            } catch (...) {
                Result failure;
                failure.status = Status::FAILED;
                failure.message = "task execution threw an unknown exception";
                task_manager.complete_task(job.task_id, failure);
            }
        }
    }

    TaskManager& task_manager;
    const std::size_t worker_count_value;
    const std::size_t max_queued_tasks;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<Job> queue;
    std::vector<std::thread> workers;
    bool started = false;
    bool stopping = false;
};

TaskRuntime::TaskRuntime(TaskManager& task_manager,
                         std::size_t worker_count,
                         std::size_t max_queued_tasks)
    : impl_(std::make_unique<Impl>(task_manager, worker_count, max_queued_tasks)) {}

TaskRuntime::~TaskRuntime() = default;

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

bool TaskRuntime::cancel_task(const std::string& task_id) {
    return impl_->task_manager.cancel_task(task_id);
}

std::size_t TaskRuntime::worker_count() const noexcept {
    return impl_->worker_count_value;
}

std::size_t TaskRuntime::queued_task_count() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->queue.size();
}

}  // namespace backup
