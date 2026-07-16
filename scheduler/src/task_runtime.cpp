#include "scheduler/task_runtime.h"
#include "scheduler/backup_scheduler.h"
#include "scheduler/restore_scheduler.h"
#include <condition_variable>
#include <chrono>
#include <ctime>
#include <deque>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace backup {

namespace {

bool valid_archive_name(const std::string& name) {
    if (name.empty()) return true;
    const std::filesystem::path path(name);
    return !path.is_absolute() && path.filename().string() == name &&
        name != "." && name != "..";
}

TaskSubmission failed_submission(const std::string& code, const std::string& message) {
    TaskSubmission submission;
    submission.error_code = code;
    submission.result.status = Status::FAILED;
    submission.result.message = message;
    return submission;
}

std::string timestamp_now() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm utc {};
    gmtime_r(&time, &utc);
    std::ostringstream output;
    output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

bool is_terminal_status(TaskStatus status) {
    return status == TaskStatus::SUCCESS ||
        status == TaskStatus::PARTIAL_SUCCESS ||
        status == TaskStatus::FAILED ||
        status == TaskStatus::CANCELLED;
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

    struct Metadata {
        // Runtime metadata is separate from TaskManager's mutable status.
        std::string type;
        std::string output_path;
        std::string source_path;
        std::string created_at;
        std::string started_at;
        std::string finished_at;
    };

    Impl(TaskManager& manager,
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
        if (!factories.scanner) factories.scanner = [] { return create_scanner(); };
        if (!factories.filter) factories.filter = [](const FilterRules& rules) {
            return create_filter(rules);
        };
        if (!factories.archive_writer) {
            factories.archive_writer = [](const std::string& path) {
                return create_archive(path);
            };
        }
        if (!factories.archive_reader) {
            factories.archive_reader = [](const std::string& path) {
                return open_archive(path);
            };
        }
        if (!factories.restorer) factories.restorer = [] { return create_restorer(); };
        task_manager.set_observer(
            [this](const std::string& task_id, const Task& task, const std::string& change) {
                record_event(task_id, task, change);
            });
    }

    ~Impl() {
        shutdown();
        task_manager.set_observer({});
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

    TaskSubmission submit_backup(const BackupRequest& request) {
        return submit(Job{TaskKind::BACKUP, "", request, {}});
    }

    TaskSubmission submit_restore(const RestoreRequest& request) {
        return submit(Job{TaskKind::RESTORE, "", {}, request});
    }

    TaskSubmission submit(Job job) {
        // Keep the full request in the queue; TaskManager only tracks status
        // and progress so it remains independent from worker execution.
        std::lock_guard<std::mutex> lock(mutex);
        if (stopping) {
            return failed_submission("RUNTIME_STOPPED", "task runtime is shutting down");
        }
        if (queue.size() >= max_queued_tasks) {
            return failed_submission("QUEUE_FULL", "task queue is full");
        }

        if (job.kind == TaskKind::BACKUP) {
            TaskSubmission preparation_failure;
            if (!prepare_backup_output(job.backup_request, preparation_failure)) {
                return preparation_failure;
            }
        }

        if (job.kind == TaskKind::BACKUP && !job.backup_request.output_path.empty()) {
            for (const auto& item : metadata) {
                if (item.second.type != "backup" ||
                    item.second.output_path != job.backup_request.output_path) {
                    continue;
                }
                const Task task = task_manager.get_task(item.first);
                if (task.status == TaskStatus::PENDING || task.status == TaskStatus::RUNNING) {
                    return failed_submission("OUTPUT_CONFLICT", "output archive is already in use");
                }
            }
        }

        if (job.kind == TaskKind::BACKUP) {
            job.task_id = task_manager.create_backup_task(job.backup_request);
        } else {
            job.task_id = task_manager.create_restore_task(job.restore_request);
        }
        const std::string submission_task_id = job.task_id;
        Metadata task_metadata;
        task_metadata.type = job.kind == TaskKind::BACKUP ? "backup" : "restore";
        task_metadata.output_path = job.backup_request.output_path;
        task_metadata.source_path = job.kind == TaskKind::BACKUP
            ? job.backup_request.source_path : std::string();
        task_metadata.created_at = timestamp_now();
        metadata.emplace(submission_task_id, task_metadata);
        order.push_back(submission_task_id);
        queue.push_back(std::move(job));
        record_event_locked(submission_task_id, task_manager.get_task(submission_task_id), "status");
        condition.notify_one();

        TaskSubmission submission;
        submission.task_id = submission_task_id;
        submission.result.status = Status::SUCCESS;
        submission.result.message = "task accepted";
        return submission;
    }

    bool prepare_backup_output(BackupRequest& request,
                               TaskSubmission& failure) {
        if (request.output_directory.empty()) return true;

        if (!valid_archive_name(request.archive_name)) {
            failure = failed_submission(
                "INVALID_ARCHIVE_NAME",
                "archive_name must be a file name without a directory path");
            return false;
        }

        const std::filesystem::path directory(request.output_directory);
        std::error_code error;
        if (!std::filesystem::is_directory(directory, error) || error) {
            failure = failed_submission(
                "INVALID_PATH", "output_path must be an existing directory");
            return false;
        }

        const bool explicit_name = !request.archive_name.empty();
        const std::string requested_name = explicit_name
            ? request.archive_name : "backup.dat";
        const std::filesystem::path requested = directory / requested_name;

        const auto is_reserved = [this](const std::filesystem::path& candidate) {
            for (const auto& item : metadata) {
                if (item.second.type != "backup" ||
                    item.second.output_path != candidate.string()) {
                    continue;
                }
                const Task task = task_manager.get_task(item.first);
                if (task.status == TaskStatus::PENDING ||
                    task.status == TaskStatus::RUNNING) {
                    return true;
                }
            }
            return false;
        };

        const auto is_occupied = [&is_reserved](const std::filesystem::path& candidate) {
            std::error_code exists_error;
            return std::filesystem::exists(candidate, exists_error) ||
                is_reserved(candidate);
        };

        std::filesystem::path selected = requested;
        if (explicit_name) {
            std::error_code exists_error;
            if (std::filesystem::exists(selected, exists_error)) {
                failure = failed_submission(
                    "OUTPUT_EXISTS", "output archive already exists");
                return false;
            }
            if (is_reserved(selected)) {
                failure = failed_submission(
                    "OUTPUT_CONFLICT", "output archive is already in use");
                return false;
            }
        } else {
            const auto stem = requested.stem().string();
            const auto extension = requested.extension().string();
            for (std::size_t suffix = 0; suffix < 100000; ++suffix) {
                selected = suffix == 0
                    ? requested
                    : directory / (stem + "-" + std::to_string(suffix) + extension);
                if (!is_occupied(selected)) break;
            }
            if (is_occupied(selected)) {
                failure = failed_submission(
                    "OUTPUT_CONFLICT", "could not allocate a unique output archive");
                return false;
            }
        }

        request.output_path = selected.string();
        return true;
    }

    void record_event_locked(const std::string& task_id,
                             const Task& task,
                             const std::string& change) {
        auto metadata_it = metadata.find(task_id);
        if (metadata_it == metadata.end()) return;
        if (task.status == TaskStatus::RUNNING && metadata_it->second.started_at.empty()) {
            metadata_it->second.started_at = timestamp_now();
        }
        if (is_terminal_status(task.status)) {
            if (metadata_it->second.finished_at.empty()) {
                metadata_it->second.finished_at = timestamp_now();
            }
        }
        events[task_id].push_back(TaskEvent{next_event_id++, task_id, change, task});
    }

    void record_event(const std::string& task_id,
                      const Task& task,
                      const std::string& change) {
        std::lock_guard<std::mutex> lock(mutex);
        record_event_locked(task_id, task, change);
    }

    std::vector<TaskSnapshot> list_tasks() const {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<TaskSnapshot> result;
        result.reserve(order.size());
        for (const auto& task_id : order) {
            const auto metadata_it = metadata.find(task_id);
            if (metadata_it == metadata.end()) continue;
            const auto& item = metadata_it->second;
            result.push_back(TaskSnapshot{
                task_manager.get_task(task_id), item.type, item.created_at,
                item.started_at, item.finished_at, item.source_path
            });
        }
        return result;
    }

    std::vector<TaskEvent> get_events(const std::string& task_id, uint64_t after_id) const {
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<TaskEvent> result;
        const auto it = events.find(task_id);
        if (it == events.end()) return result;
        for (const auto& event : it->second) {
            if (event.id > after_id) result.push_back(event);
        }
        return result;
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
                    auto scanner = factories.scanner();
                    auto filter = factories.filter(job.backup_request.filter_rules);
                    auto writer = factories.archive_writer(job.backup_request.output_path);
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
                    auto reader = factories.archive_reader(job.restore_request.archive_path);
                    auto restorer = factories.restorer();
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
    TaskRuntimeFactories factories;
    mutable std::mutex mutex;
    std::condition_variable condition;
    std::deque<Job> queue;
    std::vector<std::thread> workers;
    std::unordered_map<std::string, Metadata> metadata;
    std::vector<std::string> order;
    std::unordered_map<std::string, std::vector<TaskEvent>> events;
    uint64_t next_event_id = 1;
    bool started = false;
    bool stopping = false;
};

TaskRuntime::TaskRuntime(TaskManager& task_manager,
                         std::size_t worker_count,
                         std::size_t max_queued_tasks,
                         TaskRuntimeFactories factories)
    : impl_(std::make_unique<Impl>(task_manager, worker_count, max_queued_tasks,
                                   std::move(factories))) {}

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
