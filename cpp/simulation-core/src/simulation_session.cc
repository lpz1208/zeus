#include "zeus/simulation/simulation_session.h"

#include <algorithm>
#include <condition_variable>
#include <exception>
#include <limits>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace zeus::simulation {

class SimulationSession::Impl final : public SimulationRunControl {
public:
    Impl(
        const SimulationEngine& engine,
        SimulationConfig config,
        std::span<const VehicleDemand> demands,
        std::span<const SimulationControlEvent> controls,
        std::span<const JunctionSignalPlan> signal_plans)
        : engine_(engine),
          config_(config),
          demands_(demands.begin(), demands.end()),
          controls_(controls.begin(), controls.end()),
          signal_plans_(signal_plans.begin(), signal_plans.end()) {}

    ~Impl() override {
        stopAndJoin(true);
    }

    [[nodiscard]] SimulationSessionState reset() {
        stopAndJoin(false);

        std::unique_lock lock(mutex_);
        if (closed_) {
            throw std::logic_error("simulation session is closed");
        }
        version_base_ = std::max<std::uint64_t>(1, state_.state_version + 1);
        state_ = {};
        state_.state_version = version_base_;
        state_.paused = true;
        stop_requested_ = false;
        run_to_end_ = false;
        pause_requested_ = false;
        allowed_until_tick_ = 0;
        command_active_ = false;
        snapshot_.reset();
        pending_injections_.clear();
        step_until_event_ = false;
        event_reached_ = false;
        result_.reset();
        worker_error_ = nullptr;
        started_ = true;
        worker_ = std::thread([this] { runWorker(); });

        condition_.wait(lock, [this] {
            return state_.ready || state_.finished || worker_error_ != nullptr;
        });
        if (worker_error_ != nullptr) {
            const std::exception_ptr error = worker_error_;
            lock.unlock();
            joinWorker();
            std::rethrow_exception(error);
        }
        return state_;
    }

    [[nodiscard]] SimulationSessionState step(std::uint64_t steps) {
        if (steps == 0) {
            throw std::invalid_argument("simulation session step count must be positive");
        }
        std::unique_lock lock(mutex_);
        requireRunnableLocked();
        if (state_.finished) {
            return state_;
        }
        if (command_active_) {
            throw std::logic_error("another simulation session command is active");
        }
        if (steps > std::numeric_limits<std::uint64_t>::max() - state_.tick) {
            throw std::overflow_error("simulation session step target overflow");
        }
        const std::uint64_t target_tick = state_.tick + steps;
        command_active_ = true;
        pause_requested_ = false;
        run_to_end_ = false;
        allowed_until_tick_ = target_tick;
        condition_.notify_all();
        condition_.wait(lock, [this, target_tick] {
            return state_.finished || worker_error_ != nullptr ||
                   (state_.paused && state_.tick >= target_tick);
        });
        command_active_ = false;
        if (worker_error_ != nullptr) {
            std::rethrow_exception(worker_error_);
        }
        return state_;
    }

    [[nodiscard]] SimulationSessionState runToEnd() {
        std::unique_lock lock(mutex_);
        requireRunnableLocked();
        if (state_.finished) {
            return state_;
        }
        if (command_active_) {
            throw std::logic_error("another simulation session command is active");
        }
        command_active_ = true;
        pause_requested_ = false;
        run_to_end_ = true;
        condition_.notify_all();
        condition_.wait(lock, [this] {
            return state_.finished || worker_error_ != nullptr ||
                   (pause_requested_ && state_.paused);
        });
        command_active_ = false;
        if (worker_error_ != nullptr) {
            std::rethrow_exception(worker_error_);
        }
        return state_;
    }

    [[nodiscard]] SimulationSessionState resume() {
        std::lock_guard lock(mutex_);
        requireRunnableLocked();
        if (state_.finished) {
            return state_;
        }
        if (command_active_) {
            throw std::logic_error("another simulation session command is active");
        }
        pause_requested_ = false;
        run_to_end_ = true;
        condition_.notify_all();
        return state_;
    }

    [[nodiscard]] SimulationSessionState stepUntilEvent(std::uint64_t max_steps) {
        if (max_steps == 0) {
            throw std::invalid_argument(
                "simulation session until-event step cap must be positive");
        }
        std::unique_lock lock(mutex_);
        requireRunnableLocked();
        if (state_.finished) {
            return state_;
        }
        if (command_active_) {
            throw std::logic_error("another simulation session command is active");
        }
        if (max_steps > std::numeric_limits<std::uint64_t>::max() - state_.tick) {
            throw std::overflow_error("simulation session step target overflow");
        }
        const std::uint64_t target_tick = state_.tick + max_steps;
        command_active_ = true;
        step_until_event_ = true;
        event_reached_ = false;
        pause_requested_ = false;
        run_to_end_ = false;
        allowed_until_tick_ = target_tick;
        condition_.notify_all();
        condition_.wait(lock, [this, target_tick] {
            return state_.finished || worker_error_ != nullptr ||
                   // publishTickState observes the committed event before the
                   // engine enters the following waitForTick boundary. Do not
                   // return merely because the event flag was published: the
                   // authoritative state version must first advance to the
                   // same snapshot tick and the worker must actually pause.
                   (event_reached_ && state_.paused && snapshot_.has_value() &&
                    state_.tick >= snapshot_->tick) ||
                   (state_.paused && state_.tick >= target_tick);
        });
        step_until_event_ = false;
        command_active_ = false;
        if (worker_error_ != nullptr) {
            std::rethrow_exception(worker_error_);
        }
        return state_;
    }

    void pause() {
        std::lock_guard lock(mutex_);
        if (!started_ || state_.finished || closed_) {
            return;
        }
        pause_requested_ = true;
        run_to_end_ = false;
        allowed_until_tick_ = state_.tick;
        condition_.notify_all();
    }

    void close() {
        stopAndJoin(true);
    }

    [[nodiscard]] SimulationSessionState observe() const {
        std::lock_guard lock(mutex_);
        return state_;
    }

    [[nodiscard]] TickSnapshot snapshot() const {
        std::lock_guard lock(mutex_);
        return snapshot_.value_or(TickSnapshot{});
    }

    [[nodiscard]] CommitResult commitRoute(
        std::uint32_t vehicle_id,
        zeus::routing::Algorithm algorithm,
        std::uint64_t expected_state_version) {
        std::lock_guard lock(mutex_);
        if (closed_ || !started_) {
            return CommitResult::kRejectedClosed;
        }
        if (state_.state_version != expected_state_version) {
            return CommitResult::kRejectedStaleVersion;
        }
        if (vehicle_id >= demands_.size()) {
            return CommitResult::kRejectedUnknownVehicle;
        }
        if (!demands_[vehicle_id].agent_controlled) {
            return CommitResult::kRejectedNotAgent;
        }
        RouteInjection injection;
        injection.vehicle_id = vehicle_id;
        injection.algorithm = algorithm;
        injection.based_on_state_version = expected_state_version;
        pending_injections_.push_back(injection);
        return CommitResult::kApplied;
    }

    [[nodiscard]] CommitResult keepRoute(
        std::uint32_t vehicle_id,
        std::uint64_t expected_state_version) {
        std::lock_guard lock(mutex_);
        if (closed_ || !started_) {
            return CommitResult::kRejectedClosed;
        }
        if (state_.state_version != expected_state_version) {
            return CommitResult::kRejectedStaleVersion;
        }
        if (vehicle_id >= demands_.size()) {
            return CommitResult::kRejectedUnknownVehicle;
        }
        if (!demands_[vehicle_id].agent_controlled) {
            return CommitResult::kRejectedNotAgent;
        }
        std::erase_if(pending_injections_, [vehicle_id](const RouteInjection& injection) {
            return injection.vehicle_id == vehicle_id;
        });
        return CommitResult::kApplied;
    }

    [[nodiscard]] bool hasResult() const {
        std::lock_guard lock(mutex_);
        return result_.has_value();
    }

    [[nodiscard]] SimulationResult result() const {
        std::lock_guard lock(mutex_);
        if (!result_.has_value()) {
            throw std::logic_error("simulation session has no completed result");
        }
        return *result_;
    }

    [[nodiscard]] bool waitForTick(
        std::uint64_t next_tick,
        double simulation_time_s) override {
        std::unique_lock lock(mutex_);
        state_.ready = true;
        state_.tick = next_tick;
        state_.simulation_time_s = simulation_time_s;
        state_.state_version = version_base_ + next_tick;

        while (!stop_requested_ && !run_to_end_ &&
               (pause_requested_ || allowed_until_tick_ <= next_tick)) {
            state_.paused = true;
            condition_.notify_all();
            condition_.wait(lock);
        }
        if (stop_requested_) {
            return false;
        }
        state_.paused = false;
        condition_.notify_all();
        return true;
    }

    void publishTickState(const TickSnapshot& published) override {
        // Same engine thread, immediately before the next waitForTick: a
        // decision wake-up set here takes effect at that barrier.
        std::lock_guard lock(mutex_);
        snapshot_ = published;
        snapshot_->state_version = version_base_ + published.tick;
        if (step_until_event_ && snapshot_->decision_due) {
            pause_requested_ = true;
            run_to_end_ = false;
            allowed_until_tick_ = state_.tick;
            event_reached_ = true;
        }
        condition_.notify_all();
    }

    [[nodiscard]] std::vector<RouteInjection> collectRouteInjections() override {
        std::lock_guard lock(mutex_);
        std::vector<RouteInjection> drained;
        drained.swap(pending_injections_);
        return drained;
    }

private:
    void requireRunnableLocked() const {
        if (closed_) {
            throw std::logic_error("simulation session is closed");
        }
        if (!started_) {
            throw std::logic_error("simulation session must be reset before use");
        }
    }

    void runWorker() noexcept {
        try {
            SimulationResult completed = engine_.run(
                config_, std::span<const VehicleDemand>(demands_),
                std::span<const SimulationControlEvent>(controls_),
                std::span<const JunctionSignalPlan>(signal_plans_), this);
            std::lock_guard lock(mutex_);
            const std::uint64_t completed_ticks = completed.stats.ticks_executed;
            if (completed_ticks > state_.tick) {
                state_.tick = completed_ticks;
                state_.simulation_time_s =
                    std::min(config_.duration_seconds,
                             completed_ticks * config_.step_seconds);
                state_.state_version = version_base_ + completed_ticks;
            }
            state_.ready = true;
            state_.paused = true;
            state_.finished = true;
            state_.cancelled = completed.stats.cancelled;
            result_ = std::move(completed);
            condition_.notify_all();
        } catch (...) {
            std::lock_guard lock(mutex_);
            worker_error_ = std::current_exception();
            state_.ready = true;
            state_.paused = true;
            state_.finished = true;
            condition_.notify_all();
        }
    }

    void stopAndJoin(bool permanently_close) {
        {
            std::lock_guard lock(mutex_);
            if (permanently_close) {
                closed_ = true;
            }
            if (worker_.joinable()) {
                stop_requested_ = true;
                condition_.notify_all();
            }
        }
        joinWorker();
        if (permanently_close) {
            std::lock_guard lock(mutex_);
            if (started_) {
                state_.paused = true;
                state_.finished = true;
                state_.cancelled = state_.cancelled || !result_.has_value() ||
                                   result_->stats.cancelled;
            }
        }
    }

    void joinWorker() {
        if (worker_.joinable()) {
            worker_.join();
        }
    }

    const SimulationEngine& engine_;
    SimulationConfig config_;
    std::vector<VehicleDemand> demands_;
    std::vector<SimulationControlEvent> controls_;
    std::vector<JunctionSignalPlan> signal_plans_;

    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::thread worker_;
    SimulationSessionState state_;
    std::optional<TickSnapshot> snapshot_;
    std::vector<RouteInjection> pending_injections_;
    std::optional<SimulationResult> result_;
    std::exception_ptr worker_error_;
    std::uint64_t version_base_ = 0;
    std::uint64_t allowed_until_tick_ = 0;
    bool started_ = false;
    bool closed_ = false;
    bool stop_requested_ = false;
    bool run_to_end_ = false;
    bool pause_requested_ = false;
    bool command_active_ = false;
    bool step_until_event_ = false;
    bool event_reached_ = false;
};

SimulationSession::SimulationSession(
    const SimulationEngine& engine,
    SimulationConfig config,
    std::span<const VehicleDemand> demands,
    std::span<const SimulationControlEvent> controls,
    std::span<const JunctionSignalPlan> signal_plans)
    : impl_(std::make_unique<Impl>(
          engine, config, demands, controls, signal_plans)) {}

SimulationSession::~SimulationSession() = default;

SimulationSessionState SimulationSession::reset() {
    return impl_->reset();
}

SimulationSessionState SimulationSession::step(std::uint64_t steps) {
    return impl_->step(steps);
}

SimulationSessionState SimulationSession::runToEnd() {
    return impl_->runToEnd();
}

SimulationSessionState SimulationSession::resume() {
    return impl_->resume();
}

SimulationSessionState SimulationSession::stepUntilEvent(std::uint64_t max_steps) {
    return impl_->stepUntilEvent(max_steps);
}

void SimulationSession::pause() {
    impl_->pause();
}

void SimulationSession::close() {
    impl_->close();
}

SimulationSessionState SimulationSession::observe() const {
    return impl_->observe();
}

TickSnapshot SimulationSession::snapshot() const {
    return impl_->snapshot();
}

SimulationSession::CommitResult SimulationSession::commitRoute(
    std::uint32_t vehicle_id,
    zeus::routing::Algorithm algorithm,
    std::uint64_t expected_state_version) {
    return impl_->commitRoute(vehicle_id, algorithm, expected_state_version);
}

SimulationSession::CommitResult SimulationSession::keepRoute(
    std::uint32_t vehicle_id,
    std::uint64_t expected_state_version) {
    return impl_->keepRoute(vehicle_id, expected_state_version);
}

bool SimulationSession::hasResult() const {
    return impl_->hasResult();
}

SimulationResult SimulationSession::result() const {
    return impl_->result();
}

}  // namespace zeus::simulation
