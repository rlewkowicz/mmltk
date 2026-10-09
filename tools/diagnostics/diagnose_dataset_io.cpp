module;
// Read-only Linux capability inspection, invoked in a container by ./mmltk.
// CUDA/NUMA headers describe the real ABI; libraries remain optional at runtime.
#include <cuda.h>
#include <numaif.h>
#include <cerrno>
#include <climits>
#include <fcntl.h>
#include <glob.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <unistd.h>
export module mmltk.tools.diagnose_dataset_io;
import std;
import mmltk.tools.cpp_linux;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;
import mmltk.tools.cpp_time;
import mmltk.tools.cpp_shared_library;
namespace {
namespace support = mmltk::tools;
namespace fs = std::filesystem;
using support::json;
json unavailable(std::string reason) { return {{"status", "unavailable"}, {"reason", std::move(reason)}}; }
std::string os_error(int code, const fs::path& path) { return "[Errno " + std::to_string(code) + "] " + std::strerror(code) + ": " + mmltk::tools::text::byte_repr(path.string()); }
std::string text_mode(std::string_view bytes, std::size_t limit = std::numeric_limits<std::size_t>::max()) {
 std::string result;
 std::size_t characters = 0;
 for (std::size_t i = 0; i < bytes.size(); ++i) {
  const auto ch = static_cast<unsigned char>(bytes[i]);
  if ((ch & 0xc0) != 0x80) {
   if (characters == limit) break;
   ++characters;
  }
  if (ch == '\r') {
   result += '\n';
   if (i + 1 < bytes.size() && bytes[i + 1] == '\n') ++i;
  } else
   result += static_cast<char>(ch);
 }
 return result;
}
json observe_text(const fs::path& path) {
 support::FileDescriptor fd(::open(path.c_str(), O_RDONLY | O_CLOEXEC));
 if (fd.get() < 0) return unavailable(os_error(errno, path));
 std::string contents;
 std::array<char, 8192> buffer{};
 while (true) {
  const auto count = ::read(fd.get(), buffer.data(), buffer.size());
  if (count < 0 && errno == EINTR) continue;
  if (count < 0) return unavailable(os_error(errno, path));
  if (!count) break;
  contents.append(buffer.data(), static_cast<std::size_t>(count));
 }
 return {{"status", "observed"}, {"value", support::trim(text_mode(contents))}};
}
std::optional<std::string> which(std::string_view name) {
 const auto executable = [](const fs::path& path) {
  struct stat info{};
  return ::access(path.c_str(), F_OK | X_OK) == 0 && ::stat(path.c_str(), &info) == 0 && !S_ISDIR(info.st_mode);
 };
 if (name.find('/') != std::string_view::npos) {
  if (executable(fs::path(name))) return std::string(name);
  return std::nullopt;
 }
 const auto search_path = support::env("PATH", "/bin:/usr/bin");
 if (search_path.empty()) return std::nullopt;
 for (const auto& directory : support::split(search_path, ':')) {
  const auto candidate = fs::path(directory) / name;
  if (executable(candidate)) return candidate.string();
 }
 return std::nullopt;
}
json command(std::vector<std::string> arguments) {
 const auto executable = which(arguments.front());
 if (!executable) return unavailable(arguments.front() + " is not installed in this container");
 const auto original = arguments;
 arguments.front() = *executable;
 try {
  support::RunOptions options;
  options.timeout = std::chrono::seconds(5);
  auto result = support::capture(arguments, options);
  if (result.exec_errno) return unavailable(os_error(result.exec_errno, *executable));
  if (result.timed_out) {
   std::string invocation = "[";
   for (std::size_t i = 0; i < arguments.size(); ++i) {
    if (i) invocation += ", ";
    invocation += mmltk::tools::text::byte_repr(arguments[i]);
   }
   return unavailable("Command '" + invocation + "]' timed out after 5 seconds");
  }
  return {
   {"status", result.exit_code == 0 ? "observed" : "unavailable"}, {"argv", original}, {"returncode", result.signal ? -result.signal : result.exit_code}, {"stdout", text_mode(result.out, 65536)},
   {"stderr", text_mode(result.err, 65536)}
  };
 } catch (const std::exception& error) { return unavailable(error.what()); }
}
std::vector<std::string> glob_paths(const std::string& pattern) {
 struct Matches {
  glob_t value{};
  ~Matches() { ::globfree(&value); }
 } matches;
 const int status = ::glob(pattern.c_str(), 0, nullptr, &matches.value);
 if (status == GLOB_NOSPACE) throw std::bad_alloc();
 std::vector<std::string> paths;
 if (status == 0) {
  paths.reserve(matches.value.gl_pathc);
  for (std::size_t i = 0; i < matches.value.gl_pathc; ++i) paths.emplace_back(matches.value.gl_pathv[i]);
  std::sort(paths.begin(), paths.end());
 }
 return paths;
}
std::string resolved(const fs::path& path) { return fs::weakly_canonical(fs::absolute(path)).string(); }
json environment(std::string_view name) {
 const auto value = support::optional_env(name);
 return value ? json(*value) : json(nullptr);
}
json resource_limit(decltype(RLIMIT_NOFILE) resource) {
 struct rlimit limits{};
 if (::getrlimit(resource, &limits) != 0) throw std::system_error(errno, std::generic_category(), "getrlimit");
 const auto value = [](rlim_t limit) -> json {
  if (limit == RLIM_INFINITY) return -1;
  return limit;
 };
 return {{"soft", value(limits.rlim_cur)}, {"hard", value(limits.rlim_max)}};
}
json cpu_topology() {
 json pmus = json::object();
 for (const auto* name : {"cpu_core", "cpu_atom"}) pmus[name] = observe_text(fs::path("/sys/bus/event_source/devices") / name / "cpus");
 json cpus = json::array();
 for (const int cpu : support::CpuAffinity().cpus()) {
  json attributes = json::object();
  for (const auto* name :
   {"topology/physical_package_id", "topology/core_id", "topology/thread_siblings_list", "topology/core_type", "cpu_capacity", "cpufreq/cpuinfo_max_freq", "cpufreq/scaling_max_freq",
    "cpufreq/scaling_governor", "cpufreq/energy_performance_preference"})
   attributes[name] = observe_text(fs::path("/sys/devices/system/cpu/cpu" + std::to_string(cpu)) / name);
  cpus.push_back({{"cpu", cpu}, {"attributes", std::move(attributes)}});
 }
 return {{"scope", "eligible logical CPUs; missing core type is not evidence of a homogeneous CPU"}, {"hybrid_pmu_cpus", std::move(pmus)}, {"cpus", std::move(cpus)}};
}
json direct_io(const fs::path& path) {
 json result = {{"read", "not attempted; open and alignment metadata do not prove a DMA route"}};
 support::FileDescriptor descriptor(::open(path.c_str(), O_RDONLY | O_DIRECT | O_CLOEXEC));
 if (descriptor.get() < 0) {
  const int code = errno;
  result["open"] = {{"status", code == EINVAL || code == EOPNOTSUPP ? "unsupported" : "unavailable"}, {"errno", code}, {"reason", os_error(code, path)}};
 } else {
  descriptor.reset();
  result["open"] = {{"status", "observed"}, {"accepted", true}};
 }
 const support::SharedLibrary libc(nullptr);
 decltype(&::statx) query;
 try {
  query = libc.symbol<decltype(&::statx)>("statx");
 } catch (const support::ToolError&) {
  result["alignment"] = unavailable("container libc has no statx entrypoint");
  return result;
 }
 struct statx metadata{};
 if (query(AT_FDCWD, path.c_str(), 0, STATX_DIOALIGN, &metadata) != 0)
  result["alignment"] = unavailable(std::strerror(errno));
 else if (!(metadata.stx_mask & STATX_DIOALIGN))
  result["alignment"] = unavailable("kernel/filesystem did not return STATX_DIOALIGN");
 else
  result["alignment"] = {
   {"status", metadata.stx_dio_mem_align && metadata.stx_dio_offset_align ? "observed" : "unsupported"}, {"memory_bytes", metadata.stx_dio_mem_align},
   {"offset_and_length_bytes", metadata.stx_dio_offset_align}, {"reason", "STATX_DIOALIGN returned by kernel for this file"}
  };
 return result;
}
json pci_devices() {
 const fs::path root("/sys/bus/pci/devices");
 std::error_code error;
 if (!fs::is_directory(root, error)) return unavailable("PCI sysfs is not visible in this container");
 std::vector<fs::path> entries;
 fs::directory_iterator iterator(root, error);
 if (error) return unavailable(os_error(error.value(), root));
 for (; iterator != fs::directory_iterator{}; iterator.increment(error)) {
  if (error) return unavailable(os_error(error.value(), root));
  entries.push_back(iterator->path());
 }
 if (error) return unavailable(os_error(error.value(), root));
 std::sort(entries.begin(), entries.end());
 json devices = json::array();
 for (const auto& device : entries) {
  const auto classification = observe_text(device / "class");
  const auto value = classification.value("value", std::string{});
  if (!value.starts_with("0x03") && value != "0x010802") continue;
  json attributes = json::object();
  for (const auto* name : {"vendor", "device", "numa_node", "current_link_speed", "current_link_width", "max_link_speed", "max_link_width"}) attributes[name] = observe_text(device / name);
  devices.push_back(
   {{"address", device.filename().string()}, {"sysfs_topology", resolved(device)}, {"class", classification},
    {"driver", fs::exists(device / "driver") ? json(resolved(device / "driver")) : json(nullptr)},
    {"iommu_group", fs::exists(device / "iommu_group") ? json(resolved(device / "iommu_group")) : json(nullptr)}, {"attributes", std::move(attributes)}});
 }
 return {{"status", "observed"}, {"devices", std::move(devices)}, {"scope", "container-visible display and NVMe PCI devices"}};
}
json components() {
 auto cache = command({"ldconfig", "-p"});
 if (cache["status"] == "observed") {
  std::string filtered;
  bool first = true;
  for (const auto& line : support::lines(cache["stdout"].get<std::string>())) {
   std::string lower = line;
   for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
   if (lower.find("cufile") == std::string::npos && lower.find("gdrapi") == std::string::npos) continue;
   if (!std::exchange(first, false)) filtered += '\n';
   filtered += line;
  }
  cache["stdout"] = filtered;
 }
 std::set<std::string> libraries;
 for (const auto* root : {"/usr/lib/x86_64-linux-gnu", "/usr/local/lib", "/opt/nvidia/lib", "/usr/local/cuda*/lib64", "/usr/local/cuda*/targets/x86_64-linux/lib", "/opt/nvidia/*/lib*"})
  for (const auto* name : {"libcufile*.so*", "libgdrapi*.so*"})
   for (auto& path : glob_paths(std::string(root) + '/' + name)) libraries.insert(std::move(path));
 auto modules = observe_text("/proc/modules");
 if (modules["status"] == "observed") {
  constexpr std::array<std::string_view, 6> names{"nvidia", "nvidia_fs", "nvidia_peermem", "gdrdrv", "nvme", "nvme_core"};
  std::string filtered;
  bool first = true;
  for (const auto& line : support::lines(modules["value"].get<std::string>())) {
   const auto name = line.substr(0, line.find_first_of(" \t\r\n"));
   if (std::find(names.begin(), names.end(), name) == names.end()) continue;
   if (!std::exchange(first, false)) filtered += '\n';
   filtered += line;
  }
  modules["value"] = filtered;
 }
 return {
  {"scope", "container linker cache and conventional library paths; absence is not proof of host absence"}, {"linker_cache", std::move(cache)}, {"library_paths", libraries},
  {"kernel_modules", std::move(modules)}, {"gdr_device_visible", fs::exists("/dev/gdrdrv")}, {"gdscheck_paths", glob_paths("/usr/local/cuda*/gds/tools/gdscheck*")},
  {"gdscheck_execution", "not run; component discovery only"}, {"native_storage_dma", "not established"},
  {"gdrcopy", "Application-private static userspace library; donor libgdrapi discovery does not identify the application implementation"}
 };
}
json gdrcopy_device_capabilities() {
 try {
  const support::SharedLibrary cuda("libcuda.so.1");
  const auto init = cuda.symbol<decltype(&cuInit)>("cuInit");
  const auto driver_version = cuda.symbol<decltype(&cuDriverGetVersion)>("cuDriverGetVersion");
  const auto device_count = cuda.symbol<decltype(&cuDeviceGetCount)>("cuDeviceGetCount");
  const auto device_get = cuda.symbol<decltype(&cuDeviceGet)>("cuDeviceGet");
  const auto attribute_get = cuda.symbol<decltype(&cuDeviceGetAttribute)>("cuDeviceGetAttribute");
  auto status = init(0);
  if (status != CUDA_SUCCESS) return unavailable("cuInit returned CUDA status " + std::to_string(status));
  int version = 0, count = 0;
  status = driver_version(&version);
  if (status != CUDA_SUCCESS) return unavailable("CUDA capability query returned status " + std::to_string(status));
  status = device_count(&count);
  if (status != CUDA_SUCCESS) return unavailable("CUDA capability query returned status " + std::to_string(status));
  json devices = json::array();
  constexpr std::array<std::pair<const char*, CUdevice_attribute>, 2> attributes{
   {{"dma_buf_export", CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED}, {"dma_buf_mmap", CU_DEVICE_ATTRIBUTE_DMA_BUF_MMAP_SUPPORTED}}
  };
  for (int ordinal = 0; ordinal < count; ++ordinal) {
   CUdevice device{};
   status = device_get(&device, ordinal);
   json entry = {{"ordinal", ordinal}};
   if (status != CUDA_SUCCESS) {
    entry["query"] = unavailable("CUDA status " + std::to_string(status));
   } else {
    for (const auto& [name, attribute] : attributes) {
     int value = 0;
     status = attribute_get(&value, attribute, device);
     entry[name] = status != CUDA_SUCCESS ? unavailable("CUDA status " + std::to_string(status)) : json{{"status", "observed"}, {"supported", value != 0}};
    }
   }
   devices.push_back(std::move(entry));
  }
  return {
   {"status", "observed"}, {"driver_api_version", version}, {"minimum_dmabuf_mmap_version", 13030}, {"devices", std::move(devices)},
   {"startup_dmabuf_override", environment("GDRCOPY_USE_DMABUF_MMAP")}, {"descriptor_limit", resource_limit(RLIMIT_NOFILE)},
   {"scope", "capabilities only; allocation, BAR availability, registration and mmap remain unverified"}
  };
 } catch (const std::exception& error) { return unavailable(error.what()); }
}
json numa_policy_query() {
 try {
  const support::SharedLibrary library("libnuma.so.1");
  const auto query = library.symbol<decltype(&::get_mempolicy)>("get_mempolicy");
  int mode = 0;
  std::array<unsigned long, 64> mask{};
  if (query(&mode, mask.data(), mask.size() * sizeof(unsigned long) * CHAR_BIT, nullptr, 0)) {
   const int code = errno;
   return {{"status", code == EPERM || code == EACCES ? "denied" : "unavailable"}, {"errno", code}, {"reason", std::strerror(code)}};
  }
  return {{"status", "observed"}, {"mode", mode}, {"node_mask_words", mask}};
 } catch (const std::exception& error) { return unavailable(error.what()); }
}
json kernel() {
 struct utsname info{};
 if (::uname(&info) != 0) throw std::system_error(errno, std::generic_category(), "uname");
 std::string processor;
 // platform.uname's Linux processor field uses uname -p and hides "unknown".
 try {
  const auto result = support::capture({"uname", "-p"});
  if (!result.exit_code) processor = support::trim(text_mode(result.out));
  if (processor == "unknown") processor.clear();
 } catch (const std::exception&) {}
 return {{"system", info.sysname}, {"node", info.nodename}, {"release", info.release}, {"version", info.version}, {"machine", info.machine}, {"processor", processor}};
}
json inspect(const std::string& compiled_file, const std::string& source_path) {
 json report = {
  {"schema_version", 1}, {"observed_at", support::utc_timestamp(support::TimestampFormat::python, "")}, {"image", environment("MMLTK_DIAGNOSTIC_IMAGE")},
  {"image_id", environment("MMLTK_DIAGNOSTIC_IMAGE_ID")}, {"scope", "read-only container capability observation; no content read, writes, CUDA allocation, or benchmark"}, {"source_path", source_path},
  {"kernel", kernel()}, {"cpu", command({"lscpu", "--json"})}, {"cpu_topology", cpu_topology()},
  {"gpu", command({"nvidia-smi", "--query-gpu=uuid,name,driver_version,pci.bus_id,memory.total,memory.used,memory.free", "--format=csv"})}
 };
 report["drm_render_nodes"] = json::object();
 for (const auto& path : glob_paths("/sys/class/drm/renderD*")) report["drm_render_nodes"]["/dev/dri/" + fs::path(path).filename().string()] = resolved(path);
 report["gpu_topology"] = command({"nvidia-smi", "topo", "-m"});
 report["numa_hardware"] = command({"numactl", "--hardware"});
 report["online_nodes"] = observe_text("/sys/devices/system/node/online");
 report["node_sysfs"] = json::object();
 for (const auto& path : glob_paths("/sys/devices/system/node/node[0-9]*"))
  report["node_sysfs"][path] = {{"cpulist", observe_text(fs::path(path) / "cpulist")}, {"meminfo", observe_text(fs::path(path) / "meminfo")}};
 report["numa_policy"] = command({"numactl", "--show"});
 report["effective_affinity"] = support::CpuAffinity().cpus();
 report["process_status"] = observe_text("/proc/self/status");
 report["cpuset_cpus"] = observe_text("/sys/fs/cgroup/cpuset.cpus.effective");
 report["cpuset_memory_nodes"] = observe_text("/sys/fs/cgroup/cpuset.mems.effective");
 report["nice_limits"] = resource_limit(RLIMIT_NICE);
 report["host_transfer_observation"] = {
  {"inventory", "docs/gpu-execution.md, Device and NUMA placement"}, {"allocation_trace", "MMLTK_NUMA_TRANSFER_TRACE_FILE: context, node, capacity, registration, source/receiver route"},
  {"matcher_trace", "MMLTK_MATCHER_TRACE_FILE: exact active cost DMA and packed assignment upload counters"}, {"gdr_trace", "MMLTK_GDR_TRACE_FILE: actual mapping backend and consumer lifetime"},
  {"owned_pages", "functional tests verify local anonymous pages before portable CUDA registration"},
  {"foreign_pages", "shared file-cache, imported graphics and library-owned pages retain their actual owner's policy"},
  {"evidence", "This command does not execute any allocation, DMA, GDR write, or autograd consumer"}
 };
 report["numa_syscall_permissions"] = {
  {"get_mempolicy", numa_policy_query()}, {"set_mempolicy_mbind_move_pages", "not probed by this read-only report; functional tests verify binding and page residency"},
  {"setpriority_ioprio_set_registration", "not attempted; effective policy and registration require the focused functional tests"},
  {"scope", "diagnostic container drops capabilities; its seccomp and limits differ from runtime/test launches"}
 };
 report["pci"] = pci_devices();
 report["memlock_bytes"] = resource_limit(RLIMIT_MEMLOCK);
 report["memlock_scope"] = "diagnostic container limits; -1 means unlimited; production process may differ";
 report["components"] = components();
 report["gdrcopy_devices"] = gdrcopy_device_capabilities();
 struct stat metadata{};
 if (::stat(compiled_file.c_str(), &metadata) != 0) {
  report["file"] = unavailable(os_error(errno, compiled_file));
  return report;
 }
 if (!S_ISREG(metadata.st_mode)) {
  report["file"] = unavailable("compiled_file must be a regular file");
  return report;
 }
 report["file"] = {
  {"status", "observed"}, {"size_bytes", metadata.st_size}, {"device_major", major(metadata.st_dev)}, {"device_minor", minor(metadata.st_dev)}, {"inode", metadata.st_ino},
  {"format_validation", "not attempted; this command inspects storage, not compiled contents"}
 };
 report["filesystem"] = command({"findmnt", "--json", "--target", compiled_file, "--output", "SOURCE,FSTYPE,TARGET,OPTIONS,MAJ:MIN"});
 struct statvfs capacity{};
 if (::statvfs(compiled_file.c_str(), &capacity) != 0)
  report["filesystem_capacity"] = unavailable(os_error(errno, compiled_file));
 else
  report["filesystem_capacity"] = {
   {"status", "observed"}, {"total_bytes", capacity.f_blocks * capacity.f_frsize}, {"free_bytes", capacity.f_bfree * capacity.f_frsize}, {"available_bytes", capacity.f_bavail * capacity.f_frsize},
   {"fragment_bytes", capacity.f_frsize}, {"scope", "instantaneous capacity; unrelated writers can consume it"}
  };
 report["block_devices"] = command({"lsblk", "--json", "--output", "NAME,MAJ:MIN,TYPE,PKNAME,MODEL,TRAN,ROTA,LOG-SEC,PHY-SEC"});
 report["direct_io"] = direct_io(compiled_file);
 return report;
}
std::string usage(const std::string& name) { return "usage: " + name + " [-h] --source-path SOURCE_PATH compiled_file\n"; }
struct Arguments {
 std::string compiled_file;
 std::string source_path;
 bool help = false;
};
bool option_token(std::string_view item) {
 if (item.size() < 2 || item.front() != '-') return false;
 // argparse treats negative integers and decimals as values, since this
 // command has no options whose names are negative numbers.
 bool dot = false, digit = false;
 for (std::size_t i = 1; i < item.size(); ++i) {
  if (item[i] >= '0' && item[i] <= '9')
   digit = true;
  else if (item[i] == '.' && !dot) {
   dot = true;
   digit = false;
  } else
   return true;
 }
 return !digit;
}
Arguments parse_arguments(int argc, char** argv) {
 Arguments arguments;
 bool positional = false, source = false, options = true;
 std::vector<std::string> unrecognized;
 for (int i = 1; i < argc; ++i) {
  const std::string item(argv[i]);
  const auto equals = item.find('=');
  const auto option = item.substr(0, equals);
  if (options && (option == "-h" || (option.size() > 2 && std::string_view("--help").starts_with(option)))) {
   if (equals != std::string::npos) throw support::ToolError("argument -h/--help: ignored explicit argument " + mmltk::tools::text::byte_repr(item.substr(equals + 1)), 2);
   arguments.help = true;
   return arguments;
  }
  if (options && item == "--") {
   options = false;
   continue;
  }
  if (options && item.starts_with("-h") && !item.starts_with("--")) throw support::ToolError("argument -h/--help: ignored explicit argument " + mmltk::tools::text::byte_repr(item.substr(2)), 2);
  if (options && option.size() > 2 && std::string_view("--source-path").starts_with(option)) {
   if (equals != std::string::npos)
    arguments.source_path = item.substr(equals + 1);
   else {
    if (i + 1 == argc || option_token(argv[i + 1])) throw support::ToolError("argument --source-path: expected one argument", 2);
    arguments.source_path = argv[++i];
   }
   source = true;
  } else if (options && option_token(item))
   unrecognized.push_back(item);
  else if (!positional) {
   arguments.compiled_file = item;
   positional = true;
  } else
   unrecognized.push_back(item);
 }
 std::string missing;
 if (!positional) missing = "compiled_file";
 if (!source) {
  if (!missing.empty()) missing += ", ";
  missing += "--source-path";
 }
 if (!missing.empty()) throw support::ToolError("the following arguments are required: " + missing, 2);
 if (!unrecognized.empty()) {
  std::string error = "unrecognized arguments:";
  for (const auto& item : unrecognized) error += ' ' + item;
  throw support::ToolError(error, 2);
 }
 return arguments;
}
}  // namespace
export int mmltk_tool_diagnose_dataset_io(int argc, char** argv) {
 const auto name = fs::path(argv[0]).filename().string();
 try {
  const auto arguments = parse_arguments(argc, argv);
  if (arguments.help) {
   std::cout << usage(name) << "\nRead-only Linux capability inspection, invoked in a container by ./mmltk.\n"
             << "\npositional arguments:\n  compiled_file\n"
             << "\noptions:\n  -h, --help            show this help message and exit\n"
             << "  --source-path SOURCE_PATH\n";
   return 0;
  }
  const auto report = inspect(arguments.compiled_file, arguments.source_path);
  std::cout << report.dump(2, ' ', true) << '\n';
  return report["file"]["status"] == "observed" ? 0 : 1;
 } catch (const support::ToolError& error) {
  if (error.status == 2)
   std::cerr << usage(name) << name << ": error: " << error.what() << '\n';
  else
   std::cerr << name << ": " << error.what() << '\n';
  return error.status;
 } catch (const std::exception& error) {
  std::cerr << name << ": " << error.what() << '\n';
  return 1;
 }
}
