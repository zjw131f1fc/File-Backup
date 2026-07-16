#include "task_runtime_impl.h"
#include "scheduler/backup_scheduler.h"
#include "scheduler/restore_scheduler.h"
#include "modules/archive_reader/archive_reader.h"
#include "modules/archive_writer/archive_writer.h"
#include "modules/filter/filter.h"
#include "modules/restore/restore.h"
#include "modules/scanner/scanner.h"
#include <exception>

namespace backup {

void TaskRuntime::Impl::configure_default_factories() {
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
}

namespace {

Result module_creation_failure(const std::string& type) {
    Result failure;
    failure.status = Status::FAILED;
    failure.message = "failed to create " + type + " task modules";
    return failure;
}

}  // namespace

// worker 主循环：取出队列任务，创建子模块并交给对应执行器。
void TaskRuntime::Impl::worker_loop() {
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
                    task_manager.complete_task(
                        job.task_id, module_creation_failure("backup"));
                    continue;
                }
                BackupScheduler scheduler(task_manager, *scanner, *filter, *writer);
                scheduler.run(job.task_id, job.backup_request);
                continue;
            }

            auto reader = factories.archive_reader(job.restore_request.archive_path);
            auto restorer = factories.restorer();
            if (!reader || !restorer) {
                task_manager.complete_task(
                    job.task_id, module_creation_failure("restore"));
                continue;
            }
            RestoreScheduler scheduler(task_manager, *reader, *restorer);
            scheduler.run(job.task_id, job.restore_request);
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

}  // namespace backup
