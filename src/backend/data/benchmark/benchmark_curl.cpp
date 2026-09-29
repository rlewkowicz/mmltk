#include "src/backend/data/benchmark/detail/benchmark_curl.h"
#include "src/common/math/checked_arithmetic.h"
#include "src/backend/data/benchmark/detail/benchmark_pipeline.h"
#include "src/backend/data/benchmark/detail/benchmark_cache.h"
#include <cerrno>
#include <list>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <condition_variable>
#include <deque>
#include <new>
#include <thread>
#include <vector>
namespace mmltk::backend::data::benchmark_internal {
namespace {
constexpr std::size_t kCurlWakeDescriptors = 2, kCurlRequestDescriptors = 1;
// Ubuntu 24.04's Curl 8.5 TCP Happy Eyeballs owner has two family candidates
// (connect.c, cf_he_ctx::baller[2]). HTTP/3 protocol racing is not selected by
// configure_curl_transfer. Security patch versions keep this inspected family.
constexpr std::size_t kCurlNativeSocketCandidates = 2;
void require_supported_curl() {
 static const bool supported = [] {
  const auto* version = curl_version_info(CURLVERSION_NOW);
  if (!version || (version->version_num & 0xffff00U) != 0x080500U) throw std::runtime_error("benchmark Curl socket admission requires the supported libcurl 8.5 family");
  return true;
 }();
 (void)supported;
}
}  // namespace
void reject_local_curl_failure(const CURLcode result) {
 switch (result) {
  case CURLE_OUT_OF_MEMORY: throw std::bad_alloc{};
  case CURLE_UNSUPPORTED_PROTOCOL:
  case CURLE_FAILED_INIT:
  case CURLE_URL_MALFORMAT:
  case CURLE_NOT_BUILT_IN:
  case CURLE_READ_ERROR:
  case CURLE_BAD_FUNCTION_ARGUMENT:
  case CURLE_INTERFACE_FAILED:
  case CURLE_UNKNOWN_OPTION:
  case CURLE_SETOPT_OPTION_SYNTAX:
  case CURLE_SSL_ENGINE_NOTFOUND:
  case CURLE_SSL_ENGINE_SETFAILED:
  case CURLE_SSL_ENGINE_INITFAILED:
  case CURLE_SSL_CERTPROBLEM:
  case CURLE_SSL_CIPHER:
  case CURLE_SSL_CACERT_BADFILE:
  case CURLE_SSL_CRL_BADFILE:
  case CURLE_FILE_COULDNT_READ_FILE:
  case CURLE_ABORTED_BY_CALLBACK:
  case CURLE_AGAIN:
  case CURLE_RECURSIVE_API_CALL:
  case CURLE_UNRECOVERABLE_POLL: throw std::runtime_error(std::string("local benchmark CURL failure: ") + curl_easy_strerror(result));
  default: break;
 }
}
BenchmarkTransferEnvelope benchmark_curl_envelope(std::size_t leases, std::uint64_t fixed_bytes, std::uint64_t payload_bytes) {
 using mmltk::common::math::checked_add;
 return {
  {fixed_bytes, checked_add(kCurlWakeDescriptors, leases, "benchmark Curl descriptor overflow"), true},
  {checked_add(std::uint64_t{256U << 10}, payload_bytes, "benchmark Curl workspace overflow"), kCurlNativeSocketCandidates + kCurlRequestDescriptors, true}
 };
}
BenchmarkResources benchmark_curl_input_resources(std::uint64_t payload_bytes) {
 auto demand = benchmark_curl_envelope(0, 0, payload_bytes).per_transfer;
 // One resolver/header/cache descriptor is reused serially by an image slot.
 // Actual physical sockets and ordinary partial/publication files have owners.
 demand.descriptors = kCurlRequestDescriptors;
 demand.producer = false;
 return demand;
}
void CurlEasyDestroy::operator()(CURL* const handle) const noexcept { curl_easy_cleanup(handle); }
void CurlMultiDestroy::operator()(CURLM* const handle) const noexcept { curl_multi_cleanup(handle); }
void CurlHeadersDestroy::operator()(curl_slist* const headers) const noexcept { curl_slist_free_all(headers); }
}  // namespace mmltk::backend::data::benchmark_internal
namespace mmltk::backend::data::benchmark_internal {
struct BenchmarkCurl::Channel::State {
 Class kind;
 BenchmarkResources source;
 std::size_t descriptor_ceiling = 0;
 bool registered = true, ready = false;
 std::deque<Completed> completed;
 std::size_t outstanding = 0;
 std::uint64_t generation = 0, observed = 0, admission_observed = 0;
 std::exception_ptr failure;
 State(Class value, BenchmarkResources demand) : kind(value), source(demand) {}
};
struct BenchmarkCurl::Impl {
 struct Request {
  CURL* handle;
  std::shared_ptr<Channel::State> channel;
  BenchmarkAllowance work;
  Impl* owner;
  std::exception_ptr socket_failure;
  bool extra_range = false;
 };
 using Queue = std::list<std::unique_ptr<Request>>;
 struct Removal {
  CURL* handle;
  Channel::State* channel;
  bool settled = false;
  Removal* next = nullptr;
 };
 struct Socket {
  BenchmarkAllowance allowance;
  curl_socket_t descriptor = CURL_SOCKET_BAD;
  bool fixed_child = false;
 };
 BenchmarkCompilePipeline* execution;
 // The public mutex protects commands and channel observations only. Curl and
 // nonblocking ledger operations run on the worker without this mutex, so a
 // credit-return wake never reverses the ledger/transport lock order.
 std::mutex mutex;
 std::condition_variable changed, readers_changed;
 std::uint64_t admission_generation = 0;
 Queue commands;
 Removal* removals = nullptr;
 std::vector<std::weak_ptr<Channel::State>> observers;
 std::size_t channels = 0;
 std::uint64_t generation = 0;
 bool stopping = false, images_selected = false, settled = true, declarations_changed = true;
 std::exception_ptr failure;
 CURLM* wake_handle = nullptr;
 CurlMulti multi;
 BenchmarkAllowance fixed, socket_commitment;
 struct ClassState {
  Queue pending;
  std::size_t active = 0, limit = 0;
 };
 std::array<ClassState, kBenchmarkCurlClasses.size()> classes;
 Queue extra_ranges;
 std::unordered_map<CURL*, std::unique_ptr<Request>> active;
 ClassState& policy(Class kind) noexcept { return classes[static_cast<std::size_t>(kind)]; }
 const ClassState& policy(Class kind) const noexcept { return classes[static_cast<std::size_t>(kind)]; }
 template <class Visitor>
 void each_queue(Visitor&& visitor) {
  for (auto& state : classes) visitor(state.pending);
  visitor(extra_ranges);
 }
 // Callback data belongs to this owner, including cached connections after
 // their easy handle retires. No image/input/decoder backing enters this list.
 std::mutex socket_mutex;
 std::list<Socket> sockets;
 // L logical connections plus at most A extra candidates for A active handles.
 // A cached connection has one socket; a connecting logical connection has at
 // most two. L + A <= 2 * aggregate, including old cached connections.
 std::size_t open_sockets = 0, connection_limit = 0, idle_limit = 1, fixed_socket_grants = 0;
 bool socket_closed = false;
 std::size_t cursor = 0;
 std::jthread worker;
 explicit Impl(std::size_t cpus, BenchmarkCompilePipeline* pipeline) : execution(pipeline) {
  cpus = std::max<std::size_t>(1, cpus);
  static_assert(std::ranges::all_of(kBenchmarkCurlClasses,
                 [](const auto& entry) {
   return static_cast<std::size_t>(entry.value) < kBenchmarkCurlClasses.size() && kBenchmarkCurlClasses[static_cast<std::size_t>(entry.value)].value == entry.value &&
          (entry.value == Class::Artifact || entry.value == Class::OpenImages);
  }),
   "benchmark Curl class needs a connection policy");
  for (const auto& entry : kBenchmarkCurlClasses) {
   if (entry.value == Class::Artifact)
    policy(entry.value).limit = std::min<std::size_t>(8, cpus);
   else
    policy(entry.value).limit = std::min<std::size_t>(256, mmltk::common::math::checked_multiply(std::size_t{10}, cpus, "benchmark connection limit overflow"));
  }
  ensure_curl_global_initialized("cannot initialize benchmark Curl: ");
  require_supported_curl();
  worker = std::jthread([this] { run(); });
 }
 ~Impl() {
  {
   const std::lock_guard lock(mutex);
   stopping = true;
   wake_locked();
  }
  worker.join();
 }
 void wake_locked() {
  ++generation;
  if (wake_handle) (void)curl_multi_wakeup(wake_handle);
  changed.notify_all();
 }
 void notify_readers_locked() {
  ++admission_generation;
  readers_changed.notify_all();
 }
 static curl_socket_t open_socket(void* opaque, curlsocktype, struct curl_sockaddr* address) noexcept {
  auto& request = *static_cast<Request*>(opaque);
  auto& owner = *request.owner;
  try {
   // Every candidate already has independent descriptor custody. In particular,
   // no callback may refuse a native address because another source used the
   // ledger between admission and this connect/redirect.
   const std::lock_guard lock(owner.socket_mutex);
   const auto slot = std::ranges::find(owner.sockets, CURL_SOCKET_BAD, &Socket::descriptor);
   if (slot == owner.sockets.end()) throw std::logic_error("benchmark Curl exceeded its guaranteed native socket envelope");
   const auto descriptor = ::socket(address->family, address->socktype | SOCK_CLOEXEC, address->protocol);
   if (descriptor == CURL_SOCKET_BAD) {
    if (errno == EMFILE || errno == ENFILE || errno == ENOMEM || errno == ENOBUFS) throw InsufficientBenchmarkResources("benchmark Curl socket capacity exhausted");
    return CURL_SOCKET_BAD;
   }
   slot->descriptor = descriptor;
   ++owner.open_sockets;
   return descriptor;
  } catch (...) {
   request.socket_failure = std::current_exception();
   return CURL_SOCKET_BAD;
  }
 }
 static int close_socket(void* opaque, curl_socket_t descriptor) noexcept {
  auto& owner = *static_cast<Impl*>(opaque);
  const std::lock_guard lock(owner.socket_mutex);
  const auto status = ::close(descriptor);
  const auto slot = std::ranges::find(owner.sockets, descriptor, &Socket::descriptor);
  if (slot != owner.sockets.end()) {
   slot->descriptor = CURL_SOCKET_BAD;
   --owner.open_sockets;
   owner.socket_closed = true;
  }
  // Reusable during the same Curl call (notably a cross-host redirect). The
  // worker returns unused descriptor custody immediately after that call.
  return status;
 }
 void set_connection_limit() {
  if (!multi) return;
  for (const auto option : {CURLMOPT_MAX_TOTAL_CONNECTIONS, CURLMOPT_MAX_HOST_CONNECTIONS})
   if (curl_multi_setopt(multi.get(), option, static_cast<long>(std::max<std::size_t>(1, connection_limit))) != CURLM_OK) throw std::runtime_error("cannot set benchmark connection ceiling");
 }
 void set_idle_limit() {
  if (multi && curl_multi_setopt(multi.get(), CURLMOPT_MAXCONNECTS, static_cast<long>(idle_limit)) != CURLM_OK) throw std::runtime_error("cannot set benchmark idle connection ceiling");
 }
 void release_closed_sockets(bool detached = false) {
  std::list<Socket> released;
  const auto previous_limit = connection_limit;
  {
   const std::lock_guard lock(socket_mutex);
   if (!detached && !socket_closed) return;
   socket_closed = false;
   // Lowering Curl's limit does not eagerly evict its cache. Every existing
   // connection has a socket, or belongs to an active handle still resolving.
   // This bound therefore covers the existing cache before changing the limit.
   connection_limit = std::min(connection_limit, open_sockets + active.size());
   const auto needed = connection_limit + (kCurlNativeSocketCandidates - 1) * active.size();
   // Native eviction can close a fixed-child socket while an independently
   // charged socket survives. Exchange custody, never physical descriptors:
   // both grants stay live throughout the exchange and only closed excess
   // slots retire. Prefer fixed children for the retained native opportunity.
   auto surviving = sockets.begin();
   for (auto& slot : sockets) {
    if (!slot.fixed_child || slot.descriptor != CURL_SOCKET_BAD) continue;
    while (surviving != sockets.end() && (surviving->fixed_child || surviving->descriptor == CURL_SOCKET_BAD)) ++surviving;
    if (surviving == sockets.end()) break;
    std::swap(slot.allowance, surviving->allowance);
    std::swap(slot.fixed_child, surviving->fixed_child);
    ++surviving;
   }
   for (const bool fixed_child : {false, true}) {
    for (auto it = sockets.begin(); it != sockets.end() && sockets.size() > needed;) {
     auto current = it++;
     if (current->descriptor != CURL_SOCKET_BAD || current->fixed_child != fixed_child) continue;
     if (fixed_child) --fixed_socket_grants;
     released.splice(released.end(), sockets, current);
    }
   }
  }
  if (connection_limit != previous_limit) set_connection_limit();
 }
 void close_native_cache() {
  {
   const std::lock_guard lock(mutex);
   wake_handle = nullptr;
  }
  multi.reset();  // Curl closes every native cached socket through close_socket.
  release_closed_sockets(true);
 }
 void retire_multi() {
  close_native_cache();
  socket_commitment = {};
  fixed = {};
  {
   const std::lock_guard lock(mutex);
   settled = true;
   changed.notify_all();
  }
 }
 bool initialize() {
  if (multi) return true;
  {
   const std::lock_guard lock(mutex);
   settled = false;
  }
  if (execution && !fixed) {
   // Channels establish the shared fixed promise before their dependent
   // source leases. Generic producer headroom alone need not cover it.
   auto value = execution->try_reserve(BenchmarkResources::handles(kCurlWakeDescriptors, false, kCurlNativeSocketCandidates + kCurlRequestDescriptors));
   if (!value) return false;
   fixed = std::move(*value);
   auto socket = execution->try_reserve(BenchmarkResources::handles(0, false, kCurlNativeSocketCandidates), fixed);
   if (!socket) throw std::logic_error("benchmark socket continuation lost its parent commitment");
   socket_commitment = std::move(*socket);
  }
  multi.reset(curl_multi_init());
  if (!multi) throw std::runtime_error("cannot allocate benchmark Curl multi");
  if (curl_multi_setopt(multi.get(), CURLMOPT_PIPELINING, CURLPIPE_MULTIPLEX) != CURLM_OK) throw std::runtime_error("cannot enable benchmark HTTP multiplexing");
  set_connection_limit();
  set_idle_limit();
  {
   const std::lock_guard lock(mutex);
   wake_handle = multi.get();
   settled = false;
  }
  return true;
 }
 // Recalculate only when channels enter/leave, never on body progress. The
 // ledger supplies each declaration's applicable ceiling; fixed custody covers
 // the first two retained sockets and every extra idle socket costs one more.
 void prepare_channels() {
  bool updated = false;
  {
   const std::lock_guard lock(mutex);
   if (declarations_changed) {
    declarations_changed = false;
    updated = true;
    idle_limit = policy(images_selected ? Class::OpenImages : Class::Artifact).limit;
    if (execution)
     for (const auto& observer : observers)
      if (auto state = observer.lock(); state && state->registered) {
       const auto source = state->source.descriptors + state->source.continuation_descriptors;
       const auto spare = state->descriptor_ceiling - benchmark_curl_envelope().demand(1).descriptors - source;
       idle_limit = std::min(idle_limit, kCurlNativeSocketCandidates + spare);
      }
   }
  }
  if (updated) set_idle_limit();
  // MAXCONNECTS only evicts on native cache checks. A late stronger declaration
  // must not report readiness with an oversized, permanently idle cache. Keep
  // the fixed promise while native cleanup closes its descriptors; no active
  // request, callback or unrelated payload waits on this idle-only retirement.
  bool excess_idle;
  {
   const std::lock_guard lock(socket_mutex);
   excess_idle = active.empty() && open_sockets > idle_limit;
  }
  if (excess_idle) {
   close_native_cache();
   (void)initialize();
  }
  if (updated) {
   const std::lock_guard lock(mutex);
   if (!declarations_changed) {
    for (const auto& observer : observers)
     if (auto state = observer.lock(); state && state->registered) state->ready = true;
    changed.notify_all();
   }
  }
 }
 void detach(std::unique_ptr<Request> request, CURLcode code, bool publish) {
  (void)curl_multi_remove_handle(multi.get(), request->handle);
  --policy(request->channel->kind).active;
  // Callbacks are detached before either work credits or borrowed input custody
  // return. The source may now checkpoint, back off or publish independently.
  request->work = {};
  release_closed_sockets(true);
  const std::lock_guard lock(mutex);
  if (publish) {
   if (request->socket_failure && code != CURLE_OK) {
    request->channel->failure = request->socket_failure;
    --request->channel->outstanding;
   } else
    request->channel->completed.push_back({request->handle, code});
   ++request->channel->generation;
   readers_changed.notify_all();
  }
 }
 void drain_commands() {
  Queue incoming;
  Removal* removing;
  {
   const std::lock_guard lock(mutex);
   incoming.swap(commands);
   removing = std::exchange(removals, nullptr);
  }
  while (!incoming.empty()) {
   const auto& request = incoming.front();
   auto& queue = request->extra_range ? extra_ranges : policy(request->channel->kind).pending;
   queue.splice(queue.end(), incoming, incoming.begin());
  }
  // Stack-owned removal receipts make cancellation and destructor settlement
  // allocation-free. Each caller stays parked until all callbacks detach.
  while (removing) {
   auto* receipt = removing;
   removing = receipt->next;
   const auto matches = [receipt](const auto& request) { return request->channel.get() == receipt->channel && (!receipt->handle || request->handle == receipt->handle); };
   std::size_t removed = 0;
   each_queue([&](auto& queue) { removed += std::erase_if(queue, matches); });
   for (auto it = active.begin(); it != active.end();) {
    if (!matches(it->second)) {
     ++it;
     continue;
    }
    auto request = std::move(it->second);
    it = active.erase(it);
    detach(std::move(request), CURLE_ABORTED_BY_CALLBACK, false);
    ++removed;
   }
   {
    const std::lock_guard lock(mutex);
    removed += std::erase_if(receipt->channel->completed, [receipt](const Completed& value) { return !receipt->handle || value.handle == receipt->handle; });
    receipt->channel->outstanding -= removed;
    receipt->settled = true;
    changed.notify_all();
   }
  }
 }
 bool admit(Queue& queue, std::size_t aggregate) {
  auto& candidate = queue.front();
  BenchmarkAllowance work;
  if (execution && !candidate->work) {
   auto value = execution->try_reserve(benchmark_curl_input_resources(), fixed);
   if (!value) return false;
   work = std::move(*value);
  }
  const auto next_limit = std::max(connection_limit, active.size() + 1);
  if (next_limit > aggregate) throw std::logic_error("benchmark Curl logical admission exceeds its class ceiling");
  const auto needed = next_limit + (kCurlNativeSocketCandidates - 1) * (active.size() + 1);
  std::size_t capacity;
  {
   const std::lock_guard lock(socket_mutex);
   capacity = sockets.size();
  }
  std::list<Socket> admitted;
  std::size_t new_fixed = 0;
  while (capacity + admitted.size() < needed) {
   BenchmarkAllowance allowance;
   const bool fixed_child = fixed_socket_grants + new_fixed < kCurlNativeSocketCandidates;
   if (execution) {
    auto value = execution->try_reserve(BenchmarkResources::handles(1), fixed_child ? socket_commitment : BenchmarkAllowance{});
    if (!value) return false;  // Every partial draw and request workspace returns.
    allowance = std::move(*value);
   }
   admitted.push_back({std::move(allowance), CURL_SOCKET_BAD, fixed_child});
   if (fixed_child) ++new_fixed;
  }
  {
   const std::lock_guard lock(socket_mutex);
   sockets.splice(sockets.end(), admitted);
   fixed_socket_grants += new_fixed;
  }
  if (connection_limit != next_limit) {
   connection_limit = next_limit;
   set_connection_limit();
  }
  auto request = std::move(candidate);
  queue.pop_front();
  if (!request->work) request->work = std::move(work);
  auto* handle = request->handle;
  set_curl_option_with_prefix(handle, CURLOPT_OPENSOCKETFUNCTION, &Impl::open_socket, "cannot configure benchmark socket: ", "open");
  set_curl_option_with_prefix(handle, CURLOPT_OPENSOCKETDATA, request.get(), "cannot configure benchmark socket: ", "open owner");
  set_curl_option_with_prefix(handle, CURLOPT_CLOSESOCKETFUNCTION, &Impl::close_socket, "cannot configure benchmark socket: ", "close");
  set_curl_option_with_prefix(handle, CURLOPT_CLOSESOCKETDATA, this, "cannot configure benchmark socket: ", "close owner");
  const auto [position, inserted] = active.emplace(handle, std::move(request));
  if (!inserted) throw std::logic_error("benchmark easy handle already active");
  const auto status = curl_multi_add_handle(multi.get(), handle);
  if (status != CURLM_OK) throw std::runtime_error(std::string("cannot admit benchmark transfer: ") + curl_multi_strerror(status));
  ++policy(position->second->channel->kind).active;
  return true;
 }
 void run() noexcept {
  try {
   for (;;) {
    std::uint64_t observed;
    std::size_t aggregate;
    bool no_channels;
    {
     const std::lock_guard lock(mutex);
     if (stopping) break;
     observed = generation;
     aggregate = policy(images_selected ? Class::OpenImages : Class::Artifact).limit;
     no_channels = !channels;
    }
    (void)drain_commands();
    bool waiting = false;
    each_queue([&](const auto& queue) { waiting = waiting || !queue.empty(); });
    // Establish fixed transport custody before a channel can admit source
    // groups. This is a nonblocking worker draw; constructing I/O controllers
    // wait for its event outside CPU lanes.
    const bool initialized = !no_channels && initialize();
    if (initialized) prepare_channels();
    if (waiting && initialized) {
     while (active.size() < aggregate) {
      bool admitted = false;
      for (std::size_t turn = 0; turn < classes.size(); ++turn) {
       const auto index = (cursor + turn) % classes.size();
       auto& state = classes[index];
       if (state.active >= state.limit) continue;
       auto& queue = kBenchmarkCurlClasses[index].value == Class::Artifact && state.pending.empty() ? extra_ranges : state.pending;
       if (!queue.empty() && admit(queue, aggregate)) {
        cursor = (index + 1) % classes.size();
        admitted = true;
        break;
       }
      }
      if (!admitted) break;
     }
    }
    bool completed = false;
    if (!active.empty()) {
     int running = 0;
     const auto status = curl_multi_perform(multi.get(), &running);
     if (status != CURLM_OK) throw std::runtime_error(std::string("benchmark Curl loop failed: ") + curl_multi_strerror(status));
     release_closed_sockets();
     int remaining = 0;
     while (auto* message = curl_multi_info_read(multi.get(), &remaining)) {
      if (message->msg != CURLMSG_DONE) continue;
      const auto found = active.find(message->easy_handle);
      if (found == active.end()) throw std::logic_error("unknown benchmark Curl completion");
      auto request = std::move(found->second);
      active.erase(found);
      detach(std::move(request), message->data.result, true);
      completed = true;
     }
    }
    if (completed) continue;
    if (active.empty()) {
     bool retire = false;
     {
      const std::lock_guard lock(mutex);
      if (!channels && !waiting) retire = true;
     }
     if (retire) retire_multi();
     std::unique_lock lock(mutex);
     changed.wait(lock, [&] { return stopping || generation != observed; });
    } else {
     int descriptors = 0;
     const auto status = curl_multi_poll(multi.get(), nullptr, 0, kBenchmarkTransferPollMilliseconds, &descriptors);
     if (status != CURLM_OK) throw std::runtime_error(std::string("benchmark Curl poll failed: ") + curl_multi_strerror(status));
    }
   }
  } catch (...) {
   const auto error = std::current_exception();
   for (auto& [handle, request] : active) {
    (void)curl_multi_remove_handle(multi.get(), handle);
    request->work = {};
   }
   active.clear();
   each_queue([](auto& queue) { queue.clear(); });
   retire_multi();
   Queue abandoned;
   const std::lock_guard lock(mutex);
   failure = error;
   for (const auto& observer : observers)
    if (auto state = observer.lock()) { state->failure = error; }
   for (auto* receipt = removals; receipt; receipt = receipt->next) receipt->settled = true;
   removals = nullptr;
   abandoned.swap(commands);
   changed.notify_all();
   readers_changed.notify_all();
  }
  retire_multi();
 }
};
BenchmarkCurl::BenchmarkCurl(std::size_t cpus, BenchmarkCompilePipeline* execution) : impl_(std::make_shared<Impl>(cpus, execution)) {}
BenchmarkCurl::~BenchmarkCurl() = default;
std::size_t BenchmarkCurl::limit(Class kind) const noexcept { return impl_->policy(kind).limit; }
BenchmarkCurl::Channel::Channel(std::shared_ptr<Impl> owner, Class kind, mmltk::common::concurrency::CancellationObservation cancellation, BenchmarkResources source)
    : owner_(std::move(owner)), state_(std::make_shared<State>(kind, source)) {
 throw_if_benchmark_cancelled(cancellation);
 if (owner_->execution) {
  using mmltk::common::math::checked_add;
  const auto dependent = checked_add(source.descriptors, source.continuation_descriptors, "benchmark Curl source descriptor overflow");
  const auto complete = checked_add(benchmark_curl_envelope().demand(1).descriptors, dependent, "benchmark Curl source descriptor overflow");
  // Check the complete promise against its real ceiling, not the generic
  // worst-case producer headroom. Admission below draws from actual capacity.
  // Source bytes keep their ordinary independent/oversized admission policy.
  owner_->execution->require_feasible(BenchmarkResources::handles(complete, source.producer));
  state_->descriptor_ceiling = owner_->execution->descriptor_ceiling(source);
 }
 std::unique_lock lock(owner_->mutex);
 if (owner_->failure) std::rethrow_exception(owner_->failure);
 std::erase_if(owner_->observers, [](const auto& observer) { return observer.expired(); });
 owner_->observers.push_back(state_);
 owner_->declarations_changed = true;
 ++owner_->channels;
 if (kind == Class::OpenImages) owner_->images_selected = true;
 owner_->wake_locked();
 try {
  while (!state_->ready && !owner_->failure) {
   // Only the blocked source controller bridges its borrowed cancellation
   // observation. The transport worker remains event-driven under pressure.
   owner_->changed.wait_for(lock, std::chrono::milliseconds{100});
   lock.unlock();
   throw_if_benchmark_cancelled(cancellation);
   lock.lock();
  }
  if (owner_->failure) std::rethrow_exception(owner_->failure);
 } catch (...) {
  if (!lock.owns_lock()) lock.lock();
  state_->registered = false;
  owner_->declarations_changed = true;
  --owner_->channels;
  owner_->wake_locked();
  if (!owner_->channels) owner_->changed.wait(lock, [&] { return owner_->channels || owner_->settled || owner_->failure; });
  throw;
 }
}
BenchmarkCurl::Channel::~Channel() {
 remove_all();
 std::unique_lock lock(owner_->mutex);
 state_->registered = false;
 owner_->declarations_changed = true;
 --owner_->channels;
 owner_->wake_locked();
 if (!owner_->channels) owner_->changed.wait(lock, [&] { return owner_->channels || owner_->settled || owner_->failure; });
}
std::unique_ptr<BenchmarkCurl::Channel> BenchmarkCurl::channel(Class kind, mmltk::common::concurrency::CancellationObservation cancellation, BenchmarkResources source) {
 return std::unique_ptr<Channel>(new Channel(impl_, kind, cancellation, source));
}
std::function<void()> BenchmarkCurl::admission_wakeup() const {
 return [weak = std::weak_ptr<Impl>(impl_)]() noexcept {
  if (auto owner = weak.lock()) {
   const std::lock_guard lock(owner->mutex);
   owner->notify_readers_locked();
   // Our own failed nonblocking draws return credits too. They must not turn a
   // capacity wait into a self-waking retry loop.
   if (std::this_thread::get_id() != owner->worker.get_id()) owner->wake_locked();
  }
 };
}
void BenchmarkCurl::Channel::add(CURL* handle, bool range, BenchmarkAllowance input) {
 auto request = std::make_unique<Impl::Request>(Impl::Request{handle, state_, std::move(input), owner_.get(), {}, state_->kind == Class::Artifact && range});
 const std::lock_guard lock(owner_->mutex);
 if (owner_->failure) std::rethrow_exception(owner_->failure);
 owner_->commands.push_back(std::move(request));
 ++state_->outstanding;
 owner_->wake_locked();
}
std::optional<BenchmarkCurl::Completed> BenchmarkCurl::Channel::next() {
 const std::lock_guard lock(owner_->mutex);
 if (state_->failure) std::rethrow_exception(state_->failure);
 if (state_->completed.empty()) return {};
 auto value = state_->completed.front();
 state_->completed.pop_front();
 --state_->outstanding;
 return value;
}
void BenchmarkCurl::Channel::wait_until(std::chrono::steady_clock::time_point deadline) {
 std::unique_lock lock(owner_->mutex);
 const auto before = state_->observed;
 owner_->readers_changed.wait_until(
  lock, deadline, [&] { return state_->failure || !state_->completed.empty() || before != state_->generation || state_->admission_observed != owner_->admission_generation; });
 state_->admission_observed = owner_->admission_generation;
 state_->observed = state_->generation;
 if (state_->failure) std::rethrow_exception(state_->failure);
}
void BenchmarkCurl::Channel::wake() noexcept {
 const std::lock_guard lock(owner_->mutex);
 ++state_->generation;
 owner_->readers_changed.notify_all();
}
void BenchmarkCurl::Channel::remove(CURL* handle) {
 // A null handle selects this channel in the same stack-owned receipt.
 Impl::Removal removal{handle, state_.get()};
 std::unique_lock lock(owner_->mutex);
 if (!handle && !state_->outstanding) return;
 if (!owner_->failure) {
  removal.next = owner_->removals;
  owner_->removals = &removal;
  owner_->wake_locked();
  owner_->changed.wait(lock, [&] { return removal.settled || owner_->failure; });
 }
 if (owner_->failure) {
  state_->completed.clear();
  state_->outstanding = 0;
 }
}
void BenchmarkCurl::Channel::remove_all() { remove(nullptr); }
}  // namespace mmltk::backend::data::benchmark_internal
