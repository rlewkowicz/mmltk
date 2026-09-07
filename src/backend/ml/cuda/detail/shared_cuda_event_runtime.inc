namespace mmltk::backend::ml::cuda {

class CudaEventPoolOwner final {
   private:
    static constexpr std::size_t max_capacity_ = 64;

    enum class Lifecycle : std::uint8_t {
        open,
        closing,
        terminal_closing,
        closed,
        terminal,
    };

    enum class SlotState : std::uint8_t {
        free,
        recording,
        leased,
        retiring,
    };

    struct Slot final {
        cudaEvent_t event = nullptr;
        std::uint64_t generation = 0;
        SlotState state = SlotState::free;
    };

    class State final : public std::enable_shared_from_this<State> {
       public:
        State(const mmltk::frameworks::gpu::CudaDeviceOwner owner, const std::size_t capacity,
              mmltk::frameworks::gpu::TerminalCudaRetirementLease terminal_lease)
            : owner_(owner), capacity_(capacity), free_count_(capacity), terminal_lease_(std::move(terminal_lease)) {
            if (owner.device < 0) { throw std::invalid_argument("CUDA event pool requires a valid owning device"); }
            if (owner.record_failure == nullptr) {
                throw std::invalid_argument("CUDA event pool requires its resource owner's existing failure recorder");
            }
            if (capacity == 0 || capacity > max_capacity_) {
                throw std::invalid_argument("CUDA event pool capacity is outside its fixed bound");
            }
            for (std::size_t index = 0; index < capacity_; ++index) {
                free_slots_[index] = static_cast<std::uint16_t>(index);
            }
        }

        ~State() noexcept { close(false); }

        State(const State&) = delete;
        State& operator=(const State&) = delete;

        void initialize() { construct_events(); }

        struct Ticket final {
            std::uint16_t slot = 0;
            std::uint64_t generation = 0;
        };

        [[nodiscard]] std::optional<Ticket> record(cudaStream_t stream, const char* context) {
            const auto reserved = reserve_slot(context);
            if (!reserved.has_value()) { return std::nullopt; }
            const Ticket ticket = *reserved;

            auto scope = owner_scope();
            const cudaError_t status = scope ? scope.FinalizeStatus(cudaEventRecord(slots_[ticket.slot].event, stream)) : scope.Finalize();
            const bool admitted = finish_record(ticket, status, status != cudaSuccess);
            if (status != cudaSuccess) {
                close(true);
                mmltk::frameworks::gpu::ensure_cuda_ok(status, context);
            }
            if (!admitted) { return std::nullopt; }
            return ticket;
        }

        void wait(const Ticket ticket, cudaStream_t stream, const char* context) {
            cudaEvent_t event = nullptr;
            {
                std::scoped_lock lock(mutex_);
                Slot& slot = checked_lease_locked(ticket);
                event = slot.event;
                ++active_cuda_calls_;
            }

            auto scope = owner_scope();
            const cudaError_t status = scope ? scope.FinalizeStatus(cudaStreamWaitEvent(stream, event, 0)) : scope.Finalize();
            finish_cuda_call(status != cudaSuccess);
            if (status != cudaSuccess) {
                close(true);
                mmltk::frameworks::gpu::ensure_cuda_ok(status, context);
            }
        }

        void retire(const Ticket ticket) {
            std::scoped_lock lock(mutex_);
            Slot& slot = checked_lease_locked(ticket);
            slot.state = SlotState::retiring;
            push_retiring_locked(ticket.slot);
        }

        void abandon(const Ticket ticket) noexcept {
            std::scoped_lock lock(mutex_);
            if (!is_open_locked() || ticket.slot >= capacity_) { return; }
            Slot& slot = slots_[ticket.slot];
            if (slot.generation != ticket.generation || slot.state != SlotState::leased) { return; }
            slot.state = SlotState::retiring;
            push_retiring_locked(ticket.slot);
        }

        void close(const bool terminal) noexcept {
            bool settle = false;
            {
                std::unique_lock lock(mutex_);
                if (lifecycle_ == Lifecycle::closed || lifecycle_ == Lifecycle::terminal) { return; }
                if (terminal) {
                    request_terminal_locked();
                } else if (lifecycle_ == Lifecycle::open) {
                    lifecycle_ = Lifecycle::closing;
                }
                if (!settlement_started_) {
                    settlement_started_ = true;
                    settle = true;
                    lifecycle_changed_.wait(lock, [this] { return active_cuda_calls_ == 0; });
                } else {
                    lifecycle_changed_.wait(lock, [this] { return lifecycle_ == Lifecycle::closed || lifecycle_ == Lifecycle::terminal; });
                }
            }
            if (!settle) { return; }

            const Settlement settlement = settle_events();
            if (settlement.unresolved) { handoff_terminal(settlement.failure); }

            mmltk::frameworks::gpu::TerminalCudaRetirementLease released_reservation;
            {
                std::scoped_lock lock(mutex_);
                const bool failed = lifecycle_ == Lifecycle::terminal_closing || settlement.failure != cudaSuccess;
                lifecycle_ = failed ? Lifecycle::terminal : Lifecycle::closed;
                for (std::size_t index = 0; index < capacity_; ++index) {
                    slots_[index].state = SlotState::free;
                }
                if (!settlement.unresolved) { released_reservation = std::move(terminal_lease_); }
            }
            lifecycle_changed_.notify_all();
        }

       private:
        [[nodiscard]] bool is_open_locked() const noexcept { return lifecycle_ == Lifecycle::open; }

        void require_open_locked() const {
            if (!is_open_locked()) { throw std::runtime_error("CUDA event pool is closed"); }
        }

        void request_terminal_locked() noexcept {
            if (lifecycle_ == Lifecycle::open || lifecycle_ == Lifecycle::closing) { lifecycle_ = Lifecycle::terminal_closing; }
        }

        [[nodiscard]] mmltk::frameworks::gpu::CudaDeviceScope owner_scope() const noexcept {
            return mmltk::frameworks::gpu::CudaDeviceScope(owner_);
        }

        void construct_events() {
            auto scope = owner_scope();
            std::size_t created = 0;
            cudaError_t failure = scope.status();
            bool unresolved = false;
            if (scope) {
                for (; created < capacity_; ++created) {
                    failure = cudaEventCreateWithFlags(&slots_[created].event, cudaEventDisableTiming);
                    if (failure != cudaSuccess) { break; }
                }
                if (failure != cudaSuccess) {
                    while (created != 0) {
                        --created;
                        const cudaError_t destroyed = cudaEventDestroy(slots_[created].event);
                        scope.RecordFailure(destroyed);
                        if (destroyed == cudaSuccess) {
                            slots_[created].event = nullptr;
                        } else {
                            unresolved = true;
                        }
                    }
                }
                const cudaError_t finalized = scope.FinalizeStatus(failure);
                if (failure == cudaSuccess) {
                    failure = finalized;
                    unresolved = finalized != cudaSuccess;
                }
            } else {
                failure = scope.Finalize();
            }
            if (failure == cudaSuccess) { return; }
            {
                std::scoped_lock lock(mutex_);
                lifecycle_ = Lifecycle::terminal;
            }
            if (unresolved) { handoff_terminal(failure); }
            mmltk::frameworks::gpu::ensure_cuda_ok(failure, "create owner CUDA event pool");
        }

        void handoff_terminal(const cudaError_t failure) noexcept {
            mmltk::frameworks::gpu::TerminalCudaRetirementLease lease;
            {
                std::scoped_lock lock(mutex_);
                if (terminal_handed_off_) { return; }
                terminal_handed_off_ = true;
                lifecycle_ = Lifecycle::terminal;
                lease = std::move(terminal_lease_);
            }
            auto custody = mmltk::frameworks::gpu::TerminalCudaCustody::Share(shared_from_this());
            std::move(lease).Install(std::move(custody), failure);
        }

        struct Settlement final {
            bool unresolved = false;
            cudaError_t failure = cudaSuccess;
        };

        [[nodiscard]] Settlement settle_events() noexcept {
            Settlement result{};
            auto scope = owner_scope();
            if (!scope) {
                result.failure = scope.Finalize();
                for (std::size_t index = 0; index < capacity_; ++index) {
                    result.unresolved = result.unresolved || slots_[index].event != nullptr;
                }
                return result;
            }
            for (std::size_t index = 0; index < capacity_; ++index) {
                const cudaEvent_t event = slots_[index].event;
                if (event == nullptr) { continue; }
                const cudaError_t synchronized = slots_[index].state == SlotState::free ? cudaSuccess : cudaEventSynchronize(event);
                const cudaError_t destroyed = cudaEventDestroy(event);
                scope.RecordFailure(synchronized);
                scope.RecordFailure(destroyed);
                if (result.failure == cudaSuccess && synchronized != cudaSuccess) { result.failure = synchronized; }
                if (result.failure == cudaSuccess && destroyed != cudaSuccess) { result.failure = destroyed; }
                if (destroyed == cudaSuccess) {
                    slots_[index].event = nullptr;
                } else {
                    result.unresolved = true;
                }
            }
            const cudaError_t restored = scope.FinalizeStatus(result.failure);
            if (result.failure == cudaSuccess) { result.failure = restored; }
            return result;
        }

        [[nodiscard]] std::optional<Ticket> reserve_slot(const char* context) {
            for (;;) {
                std::uint16_t retiring_slot = 0;
                bool generation_exhausted = false;
                {
                    std::scoped_lock lock(mutex_);
                    require_open_locked();
                    if (free_count_ != 0) {
                        const std::uint16_t slot_index = pop_free_locked();
                        Slot& slot = slots_[slot_index];
                        if (slot.generation == std::numeric_limits<std::uint64_t>::max()) {
                            push_free_locked(slot_index);
                            request_terminal_locked();
                            generation_exhausted = true;
                        } else {
                            ++slot.generation;
                            slot.state = SlotState::recording;
                            ++active_cuda_calls_;
                            return Ticket{slot_index, slot.generation};
                        }
                    } else if (retiring_count_ == 0) {
                        return std::nullopt;
                    } else {
                        retiring_slot = retiring_slots_[retiring_head_];
                        ++active_cuda_calls_;
                    }
                }
                if (generation_exhausted) {
                    close(true);
                    throw std::overflow_error("CUDA event pool generation capacity is exhausted");
                }

                auto scope = owner_scope();
                cudaError_t status = scope.status();
                if (scope) {
                    const cudaError_t queried = cudaEventQuery(slots_[retiring_slot].event);
                    const cudaError_t restored = scope.FinalizeStatus(queried == cudaErrorNotReady ? cudaSuccess : queried);
                    status = queried == cudaErrorNotReady && restored == cudaSuccess ? cudaErrorNotReady : restored;
                } else {
                    status = scope.Finalize();
                }

                {
                    std::scoped_lock lock(mutex_);
                    --active_cuda_calls_;
                    if (status == cudaSuccess && is_open_locked() && retiring_count_ != 0 &&
                        retiring_slots_[retiring_head_] == retiring_slot) {
                        pop_retiring_locked();
                        slots_[retiring_slot].state = SlotState::free;
                        push_free_locked(retiring_slot);
                    } else if (status != cudaSuccess && status != cudaErrorNotReady) {
                        request_terminal_locked();
                    }
                }
                lifecycle_changed_.notify_all();
                if (status != cudaSuccess && status != cudaErrorNotReady) {
                    close(true);
                    mmltk::frameworks::gpu::ensure_cuda_ok(status, context);
                }
                if (status == cudaErrorNotReady) { return std::nullopt; }
            }
        }

        [[nodiscard]] bool finish_record(const Ticket ticket, const cudaError_t status, const bool terminal) noexcept {
            bool admitted = false;
            {
                std::scoped_lock lock(mutex_);
                --active_cuda_calls_;
                Slot& slot = slots_[ticket.slot];
                if (status == cudaSuccess && is_open_locked()) {
                    slot.state = SlotState::leased;
                    admitted = true;
                } else {
                    slot.state = SlotState::retiring;
                    push_retiring_locked(ticket.slot);
                }
                if (terminal) { request_terminal_locked(); }
            }
            lifecycle_changed_.notify_all();
            return admitted;
        }

        void finish_cuda_call(const bool terminal) noexcept {
            {
                std::scoped_lock lock(mutex_);
                --active_cuda_calls_;
                if (terminal) { request_terminal_locked(); }
            }
            lifecycle_changed_.notify_all();
        }

        Slot& checked_lease_locked(const Ticket ticket) {
            require_open_locked();
            if (ticket.slot >= capacity_) { throw std::logic_error("CUDA event lease slot is outside its owner"); }
            Slot& slot = slots_[ticket.slot];
            if (slot.generation != ticket.generation || slot.state != SlotState::leased) {
                throw std::logic_error("CUDA event lease no longer owns its slot");
            }
            return slot;
        }

        [[nodiscard]] std::uint16_t pop_free_locked() noexcept {
            const std::uint16_t slot = free_slots_[free_head_];
            free_head_ = (free_head_ + 1) % capacity_;
            --free_count_;
            return slot;
        }

        void push_free_locked(const std::uint16_t slot) noexcept {
            const std::size_t tail = (free_head_ + free_count_) % capacity_;
            free_slots_[tail] = slot;
            ++free_count_;
        }

        void push_retiring_locked(const std::uint16_t slot) noexcept {
            const std::size_t tail = (retiring_head_ + retiring_count_) % capacity_;
            retiring_slots_[tail] = slot;
            ++retiring_count_;
        }

        void pop_retiring_locked() noexcept {
            retiring_head_ = (retiring_head_ + 1) % capacity_;
            --retiring_count_;
        }

        mmltk::frameworks::gpu::CudaDeviceOwner owner_{};
        std::size_t capacity_ = 0;
        std::array<Slot, max_capacity_> slots_{};
        std::array<std::uint16_t, max_capacity_> free_slots_{};
        std::array<std::uint16_t, max_capacity_> retiring_slots_{};
        std::size_t free_head_ = 0;
        std::size_t free_count_ = 0;
        std::size_t retiring_head_ = 0;
        std::size_t retiring_count_ = 0;
        std::size_t active_cuda_calls_ = 0;
        Lifecycle lifecycle_ = Lifecycle::open;
        bool settlement_started_ = false;
        bool terminal_handed_off_ = false;
        mmltk::frameworks::gpu::TerminalCudaRetirementLease terminal_lease_{};
        std::mutex mutex_;
        std::condition_variable lifecycle_changed_;
    };

   public:
    static constexpr std::size_t max_capacity = max_capacity_;

    class Lease final {
       public:
        Lease() = default;
        ~Lease() noexcept {
            if (state_ != nullptr) { state_->abandon(ticket_); }
        }

        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        Lease(Lease&& other) noexcept : state_(std::move(other.state_)), ticket_(other.ticket_) {}

        Lease& operator=(Lease&& other) noexcept {
            if (this == &other) { return *this; }
            if (state_ != nullptr) { state_->abandon(ticket_); }
            state_ = std::move(other.state_);
            ticket_ = other.ticket_;
            return *this;
        }

        void wait(cudaStream_t stream, const char* context) const {
            if (state_ == nullptr) { throw std::logic_error("cannot wait on a retired CUDA event lease"); }
            state_->wait(ticket_, stream, context);
        }

        void retire() {
            if (state_ == nullptr) { throw std::logic_error("CUDA event lease was already retired"); }
            state_->retire(ticket_);
            state_.reset();
        }

        [[nodiscard]] explicit operator bool() const noexcept { return state_ != nullptr; }

       private:
        friend class CudaEventPoolOwner;

        Lease(std::shared_ptr<State> state, const State::Ticket ticket) noexcept : state_(std::move(state)), ticket_(ticket) {}

        std::shared_ptr<State> state_;
        State::Ticket ticket_{};
    };

    CudaEventPoolOwner(const mmltk::frameworks::gpu::CudaDeviceOwner owner, const std::size_t capacity,
                       mmltk::frameworks::gpu::TerminalCudaRetirementAuthority& retirement_authority)
        : state_(create_state(owner, capacity, retirement_authority)), capacity_(capacity) {}

    ~CudaEventPoolOwner() noexcept { state_->close(false); }

    CudaEventPoolOwner(const CudaEventPoolOwner&) = delete;
    CudaEventPoolOwner& operator=(const CudaEventPoolOwner&) = delete;
    CudaEventPoolOwner(CudaEventPoolOwner&&) = delete;
    CudaEventPoolOwner& operator=(CudaEventPoolOwner&&) = delete;

    [[nodiscard]] std::optional<Lease> record(cudaStream_t stream, const char* context) {
        const auto ticket = state_->record(stream, context);
        if (!ticket.has_value()) { return std::nullopt; }
        return Lease(state_, *ticket);
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

   private:
    [[nodiscard]] static std::shared_ptr<State> create_state(
        const mmltk::frameworks::gpu::CudaDeviceOwner owner, const std::size_t capacity,
        mmltk::frameworks::gpu::TerminalCudaRetirementAuthority& retirement_authority) {
        auto terminal_lease = mmltk::frameworks::gpu::ReserveTerminalCudaLease(retirement_authority);
        auto state = std::make_shared<State>(owner, capacity, std::move(terminal_lease));
        state->initialize();
        return state;
    }

    std::shared_ptr<State> state_;
    std::size_t capacity_ = 0;
};

}  // namespace mmltk::backend::ml::cuda
