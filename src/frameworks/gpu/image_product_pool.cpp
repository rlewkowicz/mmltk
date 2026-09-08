#include "src/frameworks/gpu/image_product_pool.h"

#include <atomic>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace mmltk::frameworks::gpu {

struct ImageProductPool::Admission final {
    void Available() noexcept {
        Notify();
        const auto retained = sink.load(std::memory_order_acquire);
        if (retained) {
            try {
                (*retained)();
            } catch (...) {}
        }
    }
    void Notify() noexcept {
        epoch.fetch_add(1U, std::memory_order_release);
        epoch.notify_all();
    }
    mutable std::mutex mutex;
    std::atomic<std::uint64_t> epoch{0U};
    std::atomic<std::shared_ptr<const std::function<void()>>> sink;
};
struct ImageProductPool::Slot final {
    Slot(std::shared_ptr<Admission> gate, DeviceContext context, ImageProductLayout layout)
        : admission(std::move(gate)), buffer(std::move(context), layout) {}
    [[nodiscard]] bool Readable() const noexcept { return facts.revision != 0U && !buffer.terminal(); }
    [[nodiscard]] bool SelectedReadable() const noexcept { return selected && !reserved && Readable(); }
    std::shared_ptr<Admission> admission;
    ImageProductBuffer buffer;
    Facts facts;
    std::size_t products = 0U;
    bool reserved = false;
    bool selected = false;
};

ImageProductPool::Availability::Availability(std::shared_ptr<Admission> admission) noexcept
    : admission_(std::move(admission)), epoch_(admission_->epoch.load(std::memory_order_acquire)) {}
bool ImageProductPool::Availability::Wait(std::stop_token stop) const {
    if (!admission_) return false;
    std::stop_callback stopped(stop, [gate = admission_] { gate->Notify(); });
    if (!stop.stop_requested()) admission_->epoch.wait(epoch_, std::memory_order_acquire);
    return !stop.stop_requested();
}
void ImageProductPool::Availability::Notify() const noexcept {
    if (admission_) admission_->Notify();
}
ImageProductPool::Availability ImageProductPool::ObserveAvailability() const noexcept { return Availability{admission_}; }
ImageStorageFootprint ImageProductPool::StorageFootprint() const noexcept {
    ImageStorageFootprint result;
    for (const auto& slot : slots_) {
        const auto physical = slot->buffer.StorageFootprint();
        result.device_bytes += physical.device_bytes;
        result.pinned_bytes += physical.pinned_bytes;
    }
    return result;
}

ImageProductPool::Product::Product(std::shared_ptr<Slot> slot, std::uint64_t revision) noexcept
    : slot_(std::move(slot)), revision_(revision) {}
ImageProductPool::Product::Product() noexcept = default;
ImageProductPool::Product::~Product() { Release(); }
ImageProductPool::Product::Product(const Product& other) : slot_(other.slot_), revision_(other.revision_) { Retain(); }
ImageProductPool::Product& ImageProductPool::Product::operator=(const Product& other) {
    if (this == &other) return *this;
    Product copy(other);
    *this = std::move(copy);
    return *this;
}
ImageProductPool::Product::Product(Product&& other) noexcept
    : slot_(std::move(other.slot_)), revision_(std::exchange(other.revision_, 0U)) {}
ImageProductPool::Product& ImageProductPool::Product::operator=(Product&& other) noexcept {
    if (this == &other) return *this;
    Release();
    slot_ = std::move(other.slot_);
    revision_ = std::exchange(other.revision_, 0U);
    return *this;
}
bool ImageProductPool::Product::valid() const noexcept {
    if (!slot_ || revision_ == 0U) return false;
    std::scoped_lock lock(slot_->admission->mutex);
    return slot_->Readable() && slot_->facts.revision == revision_;
}
std::uint64_t ImageProductPool::Product::revision() const noexcept { return valid() ? revision_ : 0U; }
BorrowedImageProductReadView ImageProductPool::Product::Borrow() const {
    if (!valid()) return {};
    return slot_->buffer.Borrow();
}
void ImageProductPool::Product::Retain() {
    if (!slot_) return;
    std::scoped_lock lock(slot_->admission->mutex);
    ++slot_->products;
}
void ImageProductPool::Product::Release() noexcept {
    if (!slot_) return;
    auto slot = std::move(slot_);
    {
        std::scoped_lock lock(slot->admission->mutex);
        --slot->products;
    }
    revision_ = 0U;
    slot->admission->Available();
}
ImageProductPool::Candidate::Candidate(std::shared_ptr<Slot> slot, Product baseline, ImagePlanePreservation preservation) noexcept
    : slot_(std::move(slot)), baseline_(std::move(baseline)), preservation_(preservation) {}
ImageProductPool::Candidate::~Candidate() { Release(); }
ImageProductPool::Candidate::Candidate(Candidate&& other) noexcept
    : slot_(std::move(other.slot_)),
      baseline_(std::move(other.baseline_)),
      // CLEANUP-IGNORE: Candidate preservation and revision complete reservation transfer; Product has no rollback baseline.
      preservation_(other.preservation_),
      revision_(std::exchange(other.revision_, 0U)) {}
// CLEANUP-IGNORE: Candidate move assignment transfers reservation and rollback state; Product move assignment
// transfers a retained completion and has different release semantics.
ImageProductPool::Candidate& ImageProductPool::Candidate::operator=(Candidate&& other) noexcept {
    if (this == &other) return *this;
    Release();
    slot_ = std::move(other.slot_);
    baseline_ = std::move(other.baseline_);
    preservation_ = other.preservation_;
    revision_ = std::exchange(other.revision_, 0U);
    return *this;
}
bool ImageProductPool::Candidate::valid() const noexcept { return slot_ != nullptr; }
std::uint64_t ImageProductPool::Candidate::revision() const noexcept { return revision_; }
void ImageProductPool::Candidate::Release() noexcept {
    if (!slot_) return;
    auto slot = std::move(slot_);
    {
        std::scoped_lock lock(slot->admission->mutex);
        slot->reserved = false;
    }
    baseline_ = {};
    revision_ = 0U;
    slot->admission->Available();
}
ImageProductPool::ImageProductPool(DeviceContext context, ImageProductLayout layout, std::size_t count)
    : admission_(std::make_shared<Admission>()) {
    if (count == 0U) throw std::invalid_argument("image product pool is empty");
    slots_.reserve(count);
    const auto wake = std::make_shared<const std::function<void()>>([gate = std::weak_ptr{admission_}] {
        if (const auto retained = gate.lock()) retained->Available();
    });
    for (std::size_t index = 0U; index != count; ++index) {
        auto slot = std::make_shared<Slot>(admission_, context, layout);
        slot->buffer.SetAvailabilitySink(wake);
        slots_.push_back(std::move(slot));
    }
}
ImageProductPool::~ImageProductPool() {
    SetAvailabilitySink({});
    admission_->Notify();
}
ImageProductPool::Candidate ImageProductPool::Acquire(std::stop_token stop, Product baseline, ImagePlanePreservation preservation) {
    if (baseline.slot_ && (baseline.slot_->admission != admission_ || !baseline.valid()))
        throw std::invalid_argument("image product baseline is invalid or foreign");
    const auto find = [&]() -> std::shared_ptr<Slot> {
        for (const auto& slot : slots_) {
            if (slot->buffer.terminal()) throw std::runtime_error("image product storage is quarantined");
            const bool in_place = slots_.size() == 1U;
            const auto owned_baseline = static_cast<std::size_t>(in_place && baseline.slot_ == slot);
            if (slot->reserved || slot->products != owned_baseline || (slot->selected && !in_place)) continue;
            if (slot->buffer.writable()) return slot;
        }
        return {};
    };
    while (!stop.stop_requested()) {
        const auto available = ObserveAvailability();
        {
            std::scoped_lock lock(admission_->mutex);
            if (auto slot = find()) {
                slot->reserved = true;
                return Candidate{std::move(slot), std::move(baseline), preservation};
            }
        }
        if (!available.Wait(stop)) return {};
    }
    return {};
}
void ImageProductPool::Publish(ImageStream& stream, Candidate& candidate, std::uint32_t width, std::uint32_t height, std::uint64_t revision,
                               ImageProductBuffer::ProductSubmit submit) {
    if (!candidate.slot_ || candidate.slot_->admission != admission_ || candidate.revision_ != 0U || revision == 0U)
        throw std::invalid_argument("image product candidate is invalid");
    if (width == 0U || height == 0U || !submit) throw std::invalid_argument("image product submit is empty");
    auto& slot = *candidate.slot_;
    const bool same_slot = candidate.baseline_.slot_ == candidate.slot_;
    bool initialized = false;
    if (candidate.baseline_.slot_) {
        if (!candidate.baseline_.valid()) throw std::invalid_argument("image product baseline is unavailable");
        auto baseline = candidate.baseline_.Borrow();
        if (!baseline.valid()) throw std::invalid_argument("image product baseline is unavailable");
        const auto descriptor = baseline.plane(0U).plane().descriptor;
        initialized = descriptor.width == width && descriptor.height == height &&
                      baseline.plane_count() == (slot.buffer.layout() == ImageProductLayout::Clean ? 1U : 2U);
        if (initialized && !same_slot)
            static_cast<void>(slot.buffer.CopyFromAs(stream, std::move(baseline), {}, 0U, false, candidate.preservation_));
    }
    {
        std::scoped_lock lock(admission_->mutex);
        slot.selected = false;
        slot.facts.revision = 0U;
    }
    try {
        slot.buffer.PublishAs(stream, width, height, revision, !initialized, std::move(submit));
        stream.Synchronize();
    } catch (...) { stream.RethrowAfterSettlement(std::current_exception()); }
    candidate.revision_ = revision;
}
std::array<ImageCopyPath, 2U> ImageProductPool::CopyFrom(ImageStream& stream, BorrowedImageProductReadView source, std::uint64_t revision) {
    if (revision == 0U) throw std::invalid_argument("image product revision is invalid");
    const auto planes = slots_.front()->buffer.layout() == ImageProductLayout::Clean ? 1U : 2U;
    if (!source.valid() || source.plane_count() < planes) throw std::invalid_argument("source image product lacks a receiver plane");
    for (const auto& slot : slots_)
        if (slot->buffer.Owns(source)) throw std::invalid_argument("an image product cannot copy from its own pool");
    auto candidate = Acquire();
    auto& slot = *candidate.slot_;
    {
        std::scoped_lock lock(admission_->mutex);
        slot.selected = false;
        slot.facts.revision = 0U;
    }
    auto paths = slot.buffer.CopyFromAs(stream, std::move(source), {}, revision);
    candidate.revision_ = revision;
    static_cast<void>(Commit(std::move(candidate)));
    return paths;
}
ImageProductPool::Product ImageProductPool::Commit(Candidate&& candidate) {
    if (!candidate.slot_ || candidate.slot_->admission != admission_ || candidate.revision_ == 0U)
        throw std::invalid_argument("image product candidate is incomplete");
    const auto& buffer = candidate.slot_->buffer;
    if (buffer.terminal()) throw std::invalid_argument("image product candidate is quarantined");
    const Facts facts{candidate.revision_, buffer.capacity_width(), buffer.capacity_height(), buffer.staging_capacity_bytes()};
    Product completed;
    {
        std::scoped_lock lock(admission_->mutex);
        candidate.slot_->facts = facts;
        for (const auto& slot : slots_)
            slot->selected = slot == candidate.slot_;
        candidate.slot_->reserved = false;
        ++candidate.slot_->products;
        completed = Product{candidate.slot_, candidate.revision_};
    }
    candidate.slot_.reset();
    candidate.baseline_ = {};
    candidate.revision_ = 0U;
    admission_->Available();
    return completed;
}
void ImageProductPool::Select(const Product& product) {
    if (!product.slot_ || product.slot_->admission != admission_ || !product.valid())
        throw std::invalid_argument("completed image product is invalid or foreign");
    {
        std::scoped_lock lock(admission_->mutex);
        if (!product.slot_->Readable() || product.slot_->facts.revision != product.revision_)
            throw std::invalid_argument("completed image product is unavailable");
        for (const auto& slot : slots_)
            slot->selected = slot == product.slot_;
    }
    admission_->Available();
}
ImageProductPool::Product ImageProductPool::Selected() const {
    std::scoped_lock lock(admission_->mutex);
    for (const auto& slot : slots_) {
        if (!slot->SelectedReadable()) continue;
        ++slot->products;
        return Product{slot, slot->facts.revision};
    }
    return {};
}
ImageProductPool::Facts ImageProductPool::SelectedFacts() const {
    std::scoped_lock lock(admission_->mutex);
    for (const auto& slot : slots_)
        if (slot->SelectedReadable()) return slot->facts;
    return {};
}
BorrowedImageProductReadView ImageProductPool::Borrow() const { return Selected().Borrow(); }
void ImageProductPool::SetAvailabilitySink(std::function<void()> sink) {
    auto retained = sink ? std::make_shared<const std::function<void()>>(std::move(sink)) : nullptr;
    admission_->sink.store(std::move(retained), std::memory_order_release);
}
std::size_t ImageProductPool::size() const noexcept { return slots_.size(); }

}  // namespace mmltk::frameworks::gpu
