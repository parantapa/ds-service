#include <cstddef>
#include <queue>
#include <string>
#include <vector>
#include <experimental/scope>

#include <omp.h>
#include <spdlog/spdlog.h>
#include <argparse/argparse.hpp>
#include <parallel_hashmap/phmap.h>
#include <grpcpp/grpcpp.h>

#include <ds-service.grpc.pb.h>
#include <task_table.hpp>

namespace stdex = std::experimental;

template <typename K, typename V>
using Map = phmap::parallel_flat_hash_map<K, V>;

auto scoped_lock(omp_lock_t* lock) {
    omp_set_lock(lock);
    return stdex::scope_exit([=]() { omp_unset_lock(lock); });
}

#define GEN_SPECIAL_MEMBERS(Type)                                 \
    Type(const Type&) = delete;            /* copy constructor */ \
    Type(Type&&) = default;                /* move constructor */ \
    Type& operator=(const Type&) = delete; /* copy assignment */  \
    Type& operator=(Type&&) = default;     /* move assignment */

struct LockManager {
    omp_lock_t lock;

    LockManager() {
        omp_init_lock(&lock);
    }

    ~LockManager() {
        omp_destroy_lock(&lock);
    }

    GEN_SPECIAL_MEMBERS(LockManager)
};

using TaskQueueEntry = std::priority_queue<std::pair<double, std::size_t>>;

struct TaskManager {
    task_table::TaskTable tasks;
    Map<std::string, std::size_t> task_index;
    Map<std::string, TaskQueueEntry> queue;
};

omp_lock_t* GLOBAL_LOCK = nullptr;
Map<std::string, std::string>* GLOBAL_MAP = nullptr;
TaskManager* GLOBAL_TASK_MANAGER = nullptr;
grpc::Server* GLOBAL_SERVER = nullptr;
bool GLOBAL_SHUTDOWN = false;

struct DsServiceImpl final : public DsService::Service {
    grpc::Status MapSet(grpc::ServerContext*, const MapSetRequest* request, Empty*) override {
        auto lock = scoped_lock(GLOBAL_LOCK);

        (*GLOBAL_MAP)[request->key()] = request->value();
        return grpc::Status::OK;
    }

    grpc::Status MapGet(grpc::ServerContext*, const MapGetRequest* request, MapGetResponse* response) override {
        auto lock = scoped_lock(GLOBAL_LOCK);

        auto it = GLOBAL_MAP->find(request->key());
        if (it == GLOBAL_MAP->end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND, fmt::format("Key {} not found.", request->key()));
        } else {
            response->set_value(it->second);
        }

        return grpc::Status::OK;
    }

    grpc::Status TaskAdd(grpc::ServerContext*, const TaskAddRequest* request, Empty*) override {
        auto lock = scoped_lock(GLOBAL_LOCK);

        auto it = GLOBAL_TASK_MANAGER->task_index.find(request->task_id());
        if (it == GLOBAL_TASK_MANAGER->task_index.end()) {
            GLOBAL_TASK_MANAGER->tasks.push_back(request->task_id(), request->priority(), request->function(),
                                                 request->input(), "", TaskState::Ready, -1.0, {});

            auto index = GLOBAL_TASK_MANAGER->tasks.size() - 1;
            auto task = GLOBAL_TASK_MANAGER->tasks[index];

            GLOBAL_TASK_MANAGER->task_index[request->task_id()] = index;
            for (const auto& qname : request->queue()) {
                task.queues().push_back(qname);
                GLOBAL_TASK_MANAGER->queue[qname].push(std::make_pair(request->priority(), index));
            }

            return grpc::Status::OK;
        } else {
            return grpc::Status(grpc::StatusCode::ALREADY_EXISTS,
                                fmt::format("Task with ID = {} already exists.", request->task_id()));
        }
    }

    grpc::Status TaskStatus(grpc::ServerContext*, const TaskStatusRequest* request,
                            TaskStatusResponse* response) override {
        auto lock = scoped_lock(GLOBAL_LOCK);

        auto it = GLOBAL_TASK_MANAGER->task_index.find(request->task_id());
        if (it == GLOBAL_TASK_MANAGER->task_index.end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                fmt::format("Task with ID = {} not found.", request->task_id()));
        } else {
            auto index = GLOBAL_TASK_MANAGER->task_index[request->task_id()];
            const auto& task = GLOBAL_TASK_MANAGER->tasks[index];
            response->set_state(task.state());
            response->set_output(task.output());
            return grpc::Status::OK;
        }
    }

    grpc::Status TaskGet(grpc::ServerContext*, const TaskGetRequest* request, TaskGetResponse* response) override {
        auto lock = scoped_lock(GLOBAL_LOCK);

        for (const auto& qname : request->queue()) {
            auto& queue = GLOBAL_TASK_MANAGER->queue[qname];
            while (!queue.empty()) {
                const auto& [_, index] = queue.top();
                auto task = GLOBAL_TASK_MANAGER->tasks[index];
                if (task.state() == TaskState::Ready) {
                    task.state() = TaskState::Running;
                    task.start_time() = omp_get_wtime();

                    response->set_task_id(task.task_id());
                    response->set_function(task.function());
                    response->set_input(task.input());

                    queue.pop();
                    return grpc::Status::OK;
                } else {
                    queue.pop();
                }
            }
        }

        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "No tasks available.");
    }

    grpc::Status TaskDone(grpc::ServerContext*, const TaskDoneRequest* request, Empty*) override {
        auto lock = scoped_lock(GLOBAL_LOCK);

        auto it = GLOBAL_TASK_MANAGER->task_index.find(request->task_id());
        if (it == GLOBAL_TASK_MANAGER->task_index.end()) {
            return grpc::Status(grpc::StatusCode::NOT_FOUND,
                                fmt::format("Task with ID = {} not found.", request->task_id()));
        } else {
            auto index = GLOBAL_TASK_MANAGER->task_index[request->task_id()];
            auto task = GLOBAL_TASK_MANAGER->tasks[index];
            if (task.state() == TaskState::Running) {
                task.state() = TaskState::Complete;
                task.output() = request->output();
            }
            return grpc::Status::OK;
        }
    }

    grpc::Status Requeue(grpc::ServerContext*, const RequeueRequest* request, Empty*) override {
        auto lock = scoped_lock(GLOBAL_LOCK);

        double max_start_time = omp_get_wtime() - request->timeout_s();

        for (std::size_t index = 0; index < std::size(GLOBAL_TASK_MANAGER->tasks); index++) {
            auto task = GLOBAL_TASK_MANAGER->tasks[index];
            if (task.state() == TaskState::Running && task.start_time() < max_start_time) {
                task.state() = TaskState::Ready;
                task.start_time() = -1;

                for (const auto& qname : task.queues()) {
                    GLOBAL_TASK_MANAGER->queue[qname].push(std::make_pair(task.priority(), index));
                }
            }
        }

        return grpc::Status::OK;
    }
};

int main(int argc, char* argv[]) {
    argparse::ArgumentParser program(argv[0]);
    program.add_description("A data structure server.");

    std::string server_address;

    // clang-format off
    program.add_argument("-a", "--address")
	.help("server address")
	.default_value(std::string{"127.0.0.1:5051"})
	.store_into(server_address);
    // clang-format on

    try {
        program.parse_args(argc, argv);
    } catch (const std::exception& err) {
        spdlog::error("Failed to parse arguments: {}\n{}", err.what(), program.usage());
        std::exit(1);
    }

    spdlog::info("server_address = {}", server_address);

    LockManager global_lock;
    GLOBAL_LOCK = &global_lock.lock;

    Map<std::string, std::string> global_map;
    GLOBAL_MAP = &global_map;

    TaskManager global_task_manager;
    GLOBAL_TASK_MANAGER = &global_task_manager;

    DsServiceImpl service;
    grpc::EnableDefaultHealthCheckService(true);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    builder.AddChannelArgument("grpc.keepalive_time_ms", 120 * 1000);
    builder.AddChannelArgument("grpc.keepalive_timeout_ms", 30 * 1000);
    builder.AddChannelArgument("grpc.keepalive_permit_without_calls", 1);
    builder.AddChannelArgument("grpc.http2.min_ping_interval_without_data_ms", 10 * 1000);

    builder.AddChannelArgument("grpc.max_connection_idle_ms", 120 * 1000);
    builder.AddChannelArgument("grpc.max_connection_age_ms", 120 * 1000);
    builder.AddChannelArgument("grpc.max_connection_age_grace_ms", 5 * 1000);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    GLOBAL_SERVER = server.get();

    spdlog::info("starting server ...");
    server->Wait();

    return 0;
}
