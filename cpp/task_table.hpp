#pragma once
// This code is auto generated.
// Do not edit manually.

#include <string>
#include <ds-service.grpc.pb.h>
#include <vector>

namespace task_table {


struct TaskTable {
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;

    std::vector<std::string> task_id;
    std::vector<double> priority;
    std::vector<std::string> function;
    std::vector<std::string> input;
    std::vector<std::string> output;
    std::vector<TaskState> state;
    std::vector<double> start_time;
    std::vector<std::vector<std::string>> queues;

    struct reference {
        TaskTable* _c;
        size_type _i;

        std::string& task_id() {
            return _c->task_id[_i];
        }
        double& priority() {
            return _c->priority[_i];
        }
        std::string& function() {
            return _c->function[_i];
        }
        std::string& input() {
            return _c->input[_i];
        }
        std::string& output() {
            return _c->output[_i];
        }
        TaskState& state() {
            return _c->state[_i];
        }
        double& start_time() {
            return _c->start_time[_i];
        }
        std::vector<std::string>& queues() {
            return _c->queues[_i];
        }

        const std::string& task_id() const {
            return _c->task_id[_i];
        }
        const double& priority() const {
            return _c->priority[_i];
        }
        const std::string& function() const {
            return _c->function[_i];
        }
        const std::string& input() const {
            return _c->input[_i];
        }
        const std::string& output() const {
            return _c->output[_i];
        }
        const TaskState& state() const {
            return _c->state[_i];
        }
        const double& start_time() const {
            return _c->start_time[_i];
        }
        const std::vector<std::string>& queues() const {
            return _c->queues[_i];
        }
    };

    struct const_reference {
        const TaskTable* _c;
        size_type _i;

        const std::string& task_id() const {
            return _c->task_id[_i];
        }
        const double& priority() const {
            return _c->priority[_i];
        }
        const std::string& function() const {
            return _c->function[_i];
        }
        const std::string& input() const {
            return _c->input[_i];
        }
        const std::string& output() const {
            return _c->output[_i];
        }
        const TaskState& state() const {
            return _c->state[_i];
        }
        const double& start_time() const {
            return _c->start_time[_i];
        }
        const std::vector<std::string>& queues() const {
            return _c->queues[_i];
        }
    };

    struct iterator {
        TaskTable* _c;
        size_type _i;

        bool operator==(iterator const& other) const { return _i == other._i; }
        bool operator!=(iterator const& other) const { return _i != other._i; }
        bool operator<(iterator const& other) const { return _i < other._i; }

        difference_type operator-(iterator const& other) const {
            return difference_type(_i) - difference_type(other._i);
        }

        iterator operator+(difference_type i) const { return {_c, _i + i}; }
        iterator operator-(difference_type i) const { return {_c, _i - i}; }

        iterator& operator++() { ++_i; return *this; }
        iterator& operator--() { --_i; return *this; }

        reference operator*() const { return {_c, _i}; }
    };

    struct const_iterator {
        const TaskTable* _c;
        size_type _i;

        bool operator==(const_iterator const& other) const { return _i == other._i; }
        bool operator!=(const_iterator const& other) const { return _i != other._i; }
        bool operator<(const_iterator const& other) const { return _i < other._i; }

        difference_type operator-(const_iterator const& other) const {
            return difference_type(_i) - difference_type(other._i);
        }

        const_iterator operator+(difference_type i) const { return {_c, _i + i}; }
        const_iterator operator-(difference_type i) const { return {_c, _i - i}; }

        const_iterator& operator++() { ++_i; return *this; }
        const_iterator& operator--() { --_i; return *this; }

        const_reference operator*() const { return {_c, _i}; }
    };

    void clear() {
        task_id.clear();
        priority.clear();
        function.clear();
        input.clear();
        output.clear();
        state.clear();
        start_time.clear();
        queues.clear();
    }

    void reserve(size_type new_cap) {
        task_id.reserve(new_cap);
        priority.reserve(new_cap);
        function.reserve(new_cap);
        input.reserve(new_cap);
        output.reserve(new_cap);
        state.reserve(new_cap);
        start_time.reserve(new_cap);
        queues.reserve(new_cap);
    }

    void shrink_to_fit() {
        task_id.shrink_to_fit();
        priority.shrink_to_fit();
        function.shrink_to_fit();
        input.shrink_to_fit();
        output.shrink_to_fit();
        state.shrink_to_fit();
        start_time.shrink_to_fit();
        queues.shrink_to_fit();
    }

    void push_back(const std::string& task_id, const double& priority, const std::string& function, const std::string& input, const std::string& output, const TaskState& state, const double& start_time, const std::vector<std::string>& queues) {
        this->task_id.push_back(task_id);
        this->priority.push_back(priority);
        this->function.push_back(function);
        this->input.push_back(input);
        this->output.push_back(output);
        this->state.push_back(state);
        this->start_time.push_back(start_time);
        this->queues.push_back(queues);
    }

    void resize(size_type count) {
        task_id.resize(count);
        priority.resize(count);
        function.resize(count);
        input.resize(count);
        output.resize(count);
        state.resize(count);
        start_time.resize(count);
        queues.resize(count);
    }

    [[nodiscard]] reference operator[](size_type i) {
        return {this, i};
    }

    [[nodiscard]] const_reference operator[](size_type i) const {
        return {this, i};
    }

    [[nodiscard]] size_type size() const {
        return task_id.size();
    }

    [[nodiscard]] iterator begin() {
        return {this, 0};
    }

    [[nodiscard]] const_iterator begin() const {
        return {this, 0};
    }

    [[nodiscard]] iterator end() {
        return {this, size()};
    }

    [[nodiscard]] const_iterator end() const {
        return {this, size()};
    }
};

} // namespace task_table