module;
// Native counterpart of log_query.py. Dependencies: Boost headers, PCRE2-8,
// ICU uc; link with -lpcre2-8 -licuuc. No interpreter or subprocess is used.

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <unicode/uchar.h>
#include <unicode/unistr.h>
#include <boost/rational.hpp>
#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <glob.h>
#include <iomanip>
#include <iostream>
#include <limits>
#include <iterator>
#include <numeric>
#include <queue>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

#include <nlohmann/json.hpp>
#include <cstddef>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <climits>
#include <filesystem>
#include <functional>
#include <map>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <poll.h>
#include <spawn.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <boost/multiprecision/cpp_int.hpp>
#include <compare>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <set>
#include <tuple>

module mmltk.tools.log_query:internal;
import mmltk.tools.log_query;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;

namespace mmltk::log_query::detail {
struct Observation { std::string event; Json value; };
struct Limits {
    std::size_t line_bytes = MAX_LINE_BYTES, message_lines = MAX_MESSAGE_LINES,
        vulkan_objects = MAX_VULKAN_OBJECTS, distinct_keys = MAX_DISTINCT_KEYS,
        pixel_samples = MAX_TRIAGE_PIXEL_SAMPLES;
};
thread_local Limits limits;
thread_local std::vector<Observation>* observations = nullptr;
thread_local const std::set<std::string_view>* observed_events = nullptr;
bool observing(std::string_view event) {
    return observations && (!observed_events || observed_events->contains(event));
}
template<class T> void observe(std::string_view event, T&& value) {
    observations->push_back({std::string(event), Json(std::forward<T>(value))});
}
}

namespace mmltk::log_query {
  namespace detail {}
  using namespace detail;
  namespace detail {
    struct DirectoryObservation {
      const fs::path& path;
      explicit DirectoryObservation(const fs::path& value):path(value) {
        if (observing("directory.open")) observe("directory.open", path.string());
      }
      ~DirectoryObservation() {
        if (observing("directory.close")) observe("directory.close", path.string());
      }
    };
    using Rational = boost::rational<Big>;
    using Strings = std::vector<std::string>;
    constexpr std::int64_t ROTATION_WINDOW_NS = 10000000;
    constexpr int MAX_AUTO_ANCHORS = 128, MAX_AUTO_RELATED = 48, MAX_AUTO_WEAK = 8, MAX_AUTO_PER_ANCHOR = 3, MAX_AUTO_PER_IDENTITY = 2, MAX_AUTO_VULKAN_RELATED = 4;
    const std::string NUMBER = R"(-?(?:0|[1-9]\d*)(?:\.\d+)?(?:[eE][+-]?\d+)?\z)";
    const std::string FIELD_NAME = R"(@?[A-Za-z_][\w.-]*\z)";
    const std::string ANSI = R"(\x1b\[[0-?]*[ -/]*[@-~])";
    const std::string NATIVE = R"(^(?P<timestamp>\d{4}-\d\d-\d\d[ T]\d\d:\d\d:\d\d(?:\.\d+)?(?:Z|[+-]\d\d:\d\d)?) \[(?P<pid>\d+):(?P<tid>\d+)\] \[(?P<logger>[^]]*)\] \[(?P<level>[^]]*)\] (?P<message>[\s\S]*)$)";
    const std::string FIREFOX = R"(^(?:(?P<timestamp>\d{4}-\d\d-\d\d[ T]\d\d:\d\d:\d\d(?:\.\d+)?(?: UTC|Z|[+-]\d\d:\d\d)?)\s+)?\[(?P<process>.*?) (?P<pid>\d+): (?P<thread>[^]]+)\]: (?P<level>[VDIWE])/(?P<module>\S+) (?P<message>[\s\S]*)$)";
    const std::string RUST = R"(^\[(?:(?P<timestamp>\d{4}-\d\d-\d\dT\S+)\s+)?(?P<level>TRACE|DEBUG|INFO|WARN|ERROR)\s+(?P<module>[^\s\]]+)\]\s?(?P<message>[\s\S]*)$)";
    const std::string FATAL = R"(^fatal: (?P<component>[^:\r\n]+): (?P<detail>.*)$)";
    const std::string FATAL_STATUS = R"(^operation=(?P<operation>[A-Za-z0-9_.-]+) pid=(?P<pid>[0-9]{1,10}) ppid=(?P<ppid>[0-9]{1,10}) pgid=(?P<pgid>[0-9]{1,10}) (?P<context>.*?)status=(?P<status>.*)$)";
    const std::string KEY_VALUE = R"rx((?:^|\s)([A-Za-z_][\w.-]*)=("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*'|.*?)(?=\s+[A-Za-z_][\w.-]*=|$))rx";
    const std::string VULKAN_HEADER = R"(^VALIDATION\s+\[(?P<id>[A-Za-z0-9_-]+)(?:\s+\((?P<message_id>0x[0-9a-fA-F]+)\))?\])";
    const std::string VULKAN_BODY = R"((?i)^\s*Validation\s+(?P<level>Error|Warning|Info):\s*\[\s*(?P<id>[A-Za-z0-9_-]+)\s*\])";
    const std::string VULKAN_OBJECT = R"(Object\s+\d+:\s*handle\s*=\s*(?P<handle>0x[0-9a-fA-F]+),\s*type\s*=\s*VK_OBJECT_TYPE_(?P<type>[A-Z0-9_]+))";
    const std::string VULKAN_RUST_OBJECT = R"(\(type:\s*(?P<type>[A-Z0-9_]+),\s*hndl:\s*(?P<handle>0x[0-9a-fA-F]+)(?:,\s*name:\s*(?P<name>[^)\r\n]*))?\))";
    const std::string VULKAN_REFERENCE = R"(\bVk(?P<type>[A-Z][A-Za-z0-9]*)[ \t]+(?P<handle>0x[0-9a-fA-F]+)\[(?P<name>[^\]\r\n]*)\])";
    const std::string FAILURE_WORD = R"((?i)(?<![a-z])(?:error|fatal|panic|failed|failure|exception|crash|timeout|timed out|segmentation fault|sigsegv|sigabrt|sigbus|aborted|invalid server CBOR record|invalid fixed text size|corrupted size vs\. prev_size|corrupted double-linked list|double free|malloc\(\):|free\(\):)(?![a-z]))";
    const std::string TEST_STATUS = R"(^\[\s*(RUN|FAILED|OK|SKIPPED)\s*\]\s+(.+)$)";
    const std::string TAP_STATUS = R"((?i)^\s*(not ok|ok)\s+\d+(?:\s+-)?\s+(.+?)(?:\s+#\s*(SKIP|TODO)\b.*)?$)";
    const std::string LIBTEST_STATUS = R"(^test (.+?) \.\.\. (ok|FAILED|ignored)(?:,.*)?$)";
    const std::string CATCH_FAILURE = R"((?i)^(.+?):(\d+): (failed|skipped|warning|fatal error):\s*(.*)$)";
    const std::string CONTEXT_LABEL = R"rx((?:^['"]?|['"] and ['"]|\bwith \d+ messages?:\s*['"])(native diagnostics|native runtime log|Firefox diagnostics|acceptance diagnostics|Mozilla diagnostics|application log):\s*)rx";
    const std::string HANDOFF_EVENT = R"((?:^|[._])(?:intent|reply|response|message|failed|failure|error)(?:[._]|$))";
    const std::string TYPED_ID = R"(\b([A-Z][A-Za-z0-9]*Id)\((\d+(?:,\s*\d+)*)\))";
    const std::string STAGE_SUFFIX = R"((?i)^(.*?)[._/-](start|started|begin|begun|requested|submitted|opened|acquired|end|ended|stop|stopped|complete|completed|finish|finished|closed|released|retired|destroyed|outcome|terminal|success|succeeded|failed|failure|error|cancelled|canceled|aborted|rejected|missing|unavailable|blocked|stalled|pending|discarded)$)";
    const std::string MOZILLA_ARTIFACT = R"(^(.*)-mozilla-(?:main\.[0-9]+\.log|child\.[0-9]+\.log(?:\.child-[0-9]+)+)\.moz_log(?:\.[0-3])?$)";
    const std::string APPLICATION_ARTIFACT = R"(^(.*)-application(?:\.[1-5])?\.log$)";
    const std::string AUTO_NOISE = R"((?:^|[._])(?:pixel|probe|slot|sample|tick|heartbeat|metric|poll)(?:[._]|$))";
    const Strings& words(std::string_view value) {
      // These vocabularies are literals; reuse their allocation in record loops.
      static thread_local std::map<std::string, Strings, std::less<>> cache;
      auto found=cache.find(value);
      if (found==cache.end()) found=cache.emplace(std::string(value),tools::split(value,' ',false)).first;
      return found->second;
    }
    const Strings ANCHOR_EVENTS = words("browser.server.started child.spawned child.signaled child.exited shutdown.requested shutdown.firefox_terminal shutdown.complete");
    const Strings START_STAGES = words("start started begin begun requested submitted opened acquired");
    const Strings INCOMPLETE_STAGES = words("missing unavailable blocked stalled pending discarded");
    const Strings FAILED_STAGES = words("failed failure error aborted rejected");
    const Strings AMBIENT_IDS = words("pid tid process_id thread_id device_id host_id user_id test_id run_id session_id parent_process_id native_process_id browser_process_id ppid pgid process_group_id");
    const Strings FIREFOX_PROCESS_EVENTS = words("child.spawned child.exited child.signaled stop.term_selected stop.kill_selected");
    const Strings REFINEMENT_FIELDS = words("sequence generation slot");
    const Strings AUTO_FIELDS = words("source_session source_instance source_revision frame_revision source_observation_revision snapshot_revision observation_revision presentation_revision allocation_generation transfer_sequence timeline_ready trace_id span_id parent_span_id request_id operation_id native_process_id browser_process_id workspace_allocation dataset_identity gallery_identity gallery_generation phase phase_id control");
    const Strings MEMORY_PROVENANCE_FIELDS = words("native_process_id browser_process_id workspace_descriptor cuda_address retained_memory_descriptor import_descriptor cuda_external_memory cuda_mapped_base cuda_status vk_status device_incarnation initialization cuda_export_allocation cuda_mapped_allocation cuda_mapping_matches_export cuda_retain_status cuda_release_status cuda_property_status cuda_allocation_type cuda_location_type cuda_location_id cuda_requested_handle_types cuda_compression_type cuda_gpu_direct_rdma cuda_allocation_usage cuda_uuid_status device_uuid vk_device vk_physical_device vk_image vk_memory memory_type_index memory_property_flags memory_heap_index memory_size reserved_bytes row_pitch image_offset capacity_width capacity_height cuda_mapping_offset dedicated image_binding_offset image_tiling image_format image_usage image_flags image_depth image_type image_mip_levels image_array_layers image_samples image_sharing_mode image_initial_layout requirements_size requirements_alignment requirements_memory_type_bits requires_dedicated prefers_dedicated external_memory_features subresource_size array_pitch depth_pitch described_row_pitch described_image_offset export_handle_type import_handle_type external_handle_type fd_stat_status fd_stat_errno fd_dev fd_ino fd_rdev fd_mode fd_size fd_consumed fd_identity_scope fd_properties_query export_descriptor channel_descriptor descriptor_count memory_descriptor_index descriptor_transport record_bytes send_bytes send_errno fd_dup_errno peer_credentials_status peer_credentials_errno peer_credentials_known fd_getfd_result fd_getfd_errno fd_ofd_query fd_ofd_result fd_ofd_errno fd_ofd_status fd_observation vk_allocate_status");
    const Strings DIRECT_ALLOCATION_FIELDS = words("workspace_allocation vk_image vk_memory memory_type_index dedicated memory_size image_offset row_pitch capacity_width capacity_height");
    const std::map<std::string,std::string> NATIVE_PIXEL_STAGES = {
      {
        "presentation.pixel","native"
      }, {
        "presentation.pixel_after_release","native_after_release"
      }, {
        "presentation.pixel_source_copy_before_ready","source_copy_before_ready"
      }, {
        "presentation.pixel_source_copy_after_release","source_copy_after_release"
      }
    };
    const std::map<std::string,std::string> DIRECT_PIXEL_STAGES = {
      {
        "firefox.workspace.direct_pixel","direct_image"
      }
    };
    const std::map<std::string,std::string> VULKAN_RESOURCE_FIELDS = {
      {
        "IMAGE","vk_image"
      }, {
        "DEVICE_MEMORY","vk_memory"
      }, {
        "COMMAND_BUFFER","vk_command_buffer"
      }
    };
    const std::map<std::string,std::string> DESCRIPTOR_LINEAGE_STAGES = {
      {
        "cuda.workspace.memory_export","cuda_export"
      }, {
        "gpu.workspace.memory_export","workspace_export"
      }, {
        "presentation.workspace.descriptor_duplicated","duplicated"
      }, {
        "presentation.workspace.descriptor_sent","sent"
      }, {
        "presentation.workspace.descriptor_duplicate_failed","duplicate_failed"
      }, {
        "presentation.workspace.descriptor_send_failed","send_failed"
      }, {
        "firefox.workspace.descriptor_received","received"
      }, {
        "firefox.workspace.descriptor_claimed","claimed"
      }, {
        "firefox.workspace.memory_import","import"
      }, {
        "firefox.workspace.memory_allocation","allocation"
      }, {
        "firefox.workspace.memory_bound","bound"
      }, {
        "firefox.workspace.memory_export","vulkan_export"
      }, {
        "firefox.workspace.memory_export_failed","export_failed"
      }, {
        "firefox.workspace.descriptor_send","send_attempt"
      }, {
        "firefox.workspace.descriptor_send_result","send_result"
      }, {
        "presentation.workspace.descriptor_received","native_received"
      }, {
        "cuda.workspace.memory_import_started","cuda_import_started"
      }, {
        "cuda.workspace.memory_import","cuda_import"
      }, {
        "cuda.workspace.memory_map","cuda_map"
      }, {
        "cuda.workspace.memory_import_rejected","import_rejected"
      }, {
        "cuda.workspace.memory_retirement_started","retirement_started"
      }, {
        "cuda.workspace.memory_retirement","retirement"
      }
    };
    const Strings VULKAN_PROVENANCE_EVENTS = words("firefox.workspace.image_created firefox.workspace.memory_import firefox.workspace.memory_bound firefox.workspace.direct_binding firefox.workspace.memory_allocation firefox.workspace.memory_export firefox.workspace.memory_export_failed");
    const Strings LIBRARY_PROVENANCE_EVENTS = words("cuda.workspace.loaded_library cuda.workspace.library_inventory firefox.workspace.loaded_library firefox.workspace.library_inventory");
    const Strings MEMORY_PROVENANCE_EVENTS = [] {
      Strings r;
      for (const auto& [name, stage]: DESCRIPTOR_LINEAGE_STAGES) r.push_back(name);
      r.push_back("firefox.workspace.image_created");
      return r;
    }
    ();
    const Strings FAMILY_SUFFIXES = {
      ".jsonl", ".log", "-native.log", "-firefox.log", "-acceptance.jsonl", "-application.log"
    };
    const std::map<std::string,std::pair<std::string,std::map<std::string,std::string>>> AUTO_INTEGRATION_FIELDS = {
      {
        "integration.explore_reopen_wait", {
          "explore", {
            {
              "a","snapshot_revision"
            }, {
              "b","source_revision"
            }
          }
        }
      }, {
        "integration.explore_reopen_draw", {
          "explore", {
            {
              "c","source_revision"
            }, {
              "d","presentation_revision"
            }
          }
        }
      }, {
        "integration.explore_reopened", {
          "explore", {
            {
              "a","snapshot_revision"
            }, {
              "b","source_revision"
            }
          }
        }
      }, {
        "integration.explore_state", {
          "explore", {
            {
              "a","snapshot_revision"
            }
          }
        }
      }, {
        "integration.explore_frame", {
          "explore", {
            {
              "a","source_revision"
            }
          }
        }
      }, {
        "integration.annotation_ready", {
          "annotation", {
            {
              "a","source_revision"
            }
          }
        }
      }, {
        "integration.upscale_settlement", {
          "upscale", {
            {
              "a","snapshot_revision"
            }, {
              "b","source_revision"
            }
          }
        }
      }
    };
    std::string lower(std::string_view value) {
      auto u=icu::UnicodeString::fromUTF8(icu::StringPiece(value.data(),static_cast<int32_t>(value.size())));
      u.foldCase();
      std::string s;
      return u.toUTF8String(s);
    }
    std::string trim(std::string_view value) {
      auto u=icu::UnicodeString::fromUTF8(icu::StringPiece(value.data(),static_cast<int32_t>(value.size())));
      int32_t a=0,b=u.length();
      while(a<b && (u_isUWhiteSpace(u.char32At(a)) || (u.char32At(a)>=0x1c&&u.char32At(a)<=0x1f))) a+=U16_LENGTH(u.char32At(a));
      while(b>a) {
        auto c=u.char32At(u.moveIndex32(b,-1));
        if (!u_isUWhiteSpace(c) && !(c>=0x1c&&c<=0x1f)) break;
        b=u.moveIndex32(b,-1);
      }
      std::string s;
      return u.tempSubStringBetween(a,b).toUTF8String(s);
    }
    std::size_t chars(std::string_view s) {
      return static_cast<std::size_t>(icu::UnicodeString::fromUTF8(icu::StringPiece(s.data(),static_cast<int32_t>(s.size()))).countChar32());
    }
    std::string prefix(std::string_view s,std::size_t n) {
      auto u=icu::UnicodeString::fromUTF8(icu::StringPiece(s.data(),static_cast<int32_t>(s.size())));
      std::string r;
      return u.tempSubString(0,u.moveIndex32(0,static_cast<int32_t>(std::min<std::size_t>(n,INT32_MAX)))).toUTF8String(r);
    }
    Json get(const Json& j,std::string_view key,Json fallback=MISSING) {
      if(!j.is_object()) return fallback;
      auto it=j.find(std::string(key));
      return it==j.end()?fallback:*it;
    }
    std::string str(const Json& j);
    bool integer(const Json& j) {
      return j.is_number_integer() || (j.is_binary() && j.get_binary().subtype()==42);
    }
    Big decimal_integer(std::string_view text) {
      bool negative = !text.empty() && text.front() == '-';
      std::size_t start = negative || (!text.empty() && text.front() == '+') ? 1 : 0;
      Big result = 0;
      for (std::size_t i = start; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') throw QueryError("invalid decimal integer");
        result *= 10;
        result += text[i] - '0';
      }
      return negative ? -result : result;
    }
    Big bigint(const Json& j) {
      if(j.is_binary()) {
        const auto& b=j.get_binary();
        return decimal_integer(std::string(b.begin(),b.end()));
      }
      if(j.is_number_unsigned()) return Big(j.get<std::uint64_t>());
      if(j.is_number_integer()) return Big(j.get<std::int64_t>());
      throw QueryError("expected an exact integer");
    }
    Json number(const Big& n) {
      static const Big lo=std::numeric_limits<std::int64_t>::min(), hi=std::numeric_limits<std::uint64_t>::max();
      if(n>=0 && n<=hi) return n.convert_to<std::uint64_t>();
      if(n>=lo && n<0) return n.convert_to<std::int64_t>();
      auto text=n.convert_to<std::string>();
      return Json::binary(std::vector<std::uint8_t>(text.begin(),text.end()),42);
    }
    bool same_type(const Json& a, const Json& b) {
      return (integer(a) && integer(b)) || a.type() == b.type();
    }
    bool numeric(const Json& j) {
      return integer(j)||j.is_number_float();
    }
    bool truth(const Json& j) {
      if(missing(j)) return true;
      if(j.is_null()) return false;
      if(j.is_boolean()) return j.get<bool>();
      if(integer(j)) return bigint(j)!=0;
      if(j.is_number_float()) return j.get<double>()!=0;
      return !j.empty();
    }
    bool present(const Json& j) {
      return !missing(j)&&!j.is_null()&&!(j.is_string()&&j.get_ref<const std::string&>().empty());
    }
    std::string python_repr(const Json& value) {
      if (value.is_string()) {
        const auto text = value.get<std::string>();
        const char quote = text.find('\'') != std::string::npos && text.find('"') == std::string::npos ? '"' : '\'';
        std::string result(1, quote);
        auto unicode = icu::UnicodeString::fromUTF8(text);
        for (int32_t i=0; i<unicode.length();) {
          auto c=unicode.char32At(i);
          i+=U16_LENGTH(c);
          if (c==quote || c=='\\') { result+='\\'; result+=static_cast<char>(c); }
          else if (c=='\n') result+="\\n";
          else if (c=='\r') result+="\\r";
          else if (c=='\t') result+="\\t";
          else if (c!=0x20 && !u_isprint(c)) {
            std::ostringstream escaped;
            escaped << (c<=0xff?"\\x":c<=0xffff?"\\u":"\\U")
                    << std::hex << std::setfill('0') << std::setw(c<=0xff?2:c<=0xffff?4:8)
                    << static_cast<std::uint32_t>(c);
            result+=escaped.str();
          } else {
            icu::UnicodeString character(c);
            std::string encoded_character;
            character.toUTF8String(encoded_character);
            result+=encoded_character;
          }
        }
        result+=quote;
        return result;
      }
      if (value.is_array() || value.is_object()) {
        std::string result=value.is_array()?"[":"{";
        bool first=true;
        for (auto i=value.begin(); i!=value.end(); ++i) {
          if (!first) result+=", ";
          first=false;
          if (value.is_object()) result+=python_repr(i.key())+": ";
          result+=python_repr(i.value());
        }
        result+=value.is_array()?"]":"}";
        return result;
      }
      if (value.is_null()) return "None";
      if (value.is_boolean()) return value.get<bool>()?"True":"False";
      return encoded(value);
    }
    std::string str(const Json& j) {
      if(missing(j)) return "(not recorded)";
      if(j.is_string()) return j.get<std::string>();
      return python_repr(j);
    }

    std::string textget(const Record& r,std::string_view name) {
      return str(r.get(name));
    }
    bool equals(const Json& j,std::string_view s) {
      return j.is_string()&&j.get_ref<const std::string&>()==s;
    }
    bool in(const Json& j,const Strings& items) {
      return j.is_string()&&mmltk::tools::includes(items,j.get_ref<const std::string&>());
    }
    bool meta(const Record& r,std::string_view name) {
      auto v=get(r.metadata,name,nullptr);
      return truth(v);
    }
    Big absbig(Big n) {
      return n<0?-n:n;
    }
    Big ns(const Record& r) {
      return r.time_ns.value_or(0);
    }
    Big mtime(const Record& r) {
      auto v=get(r.metadata,"mtime_ns",0);
      return integer(v)?bigint(v):Big(0);
    }
    std::size_t part(const Record& r) {
      auto v=get(r.metadata,"part",0);
      return integer(v)?bigint(v).convert_to<std::size_t>():0;
    }
    std::string join(const Strings& v,std::string_view sep) {
      std::string r;
      for(const auto& s:v) {
        if(!r.empty())r+=sep;
        r+=s;
      }
      return r;
    }
    std::string rstrip_newline(std::string_view v) {
      while(!v.empty()&&(v.back()=='\r'||v.back()=='\n'))v.remove_suffix(1);
      return std::string(v);
    }
    struct Match {
      std::shared_ptr<const std::string> backing;
      std::string_view source;
      std::vector<std::pair<std::size_t,std::size_t>> spans;
      std::map<std::string,std::size_t> names;
      explicit operator bool() const {
        return !spans.empty();
      }
      std::string operator[](std::size_t i) const {
        if(i>=spans.size()||spans[i].first==PCRE2_UNSET)return {
        };
        return std::string(source.substr(spans[i].first,spans[i].second-spans[i].first));
      }
      std::string operator[](const std::string& n) const {
        auto i=names.find(n);
        return i==names.end()?std::string {
        }
        :(*this)[i->second];
      }
      bool matched(std::size_t i) const {
        return i<spans.size()&&spans[i].first!=PCRE2_UNSET;
      }
      bool matched(const std::string& n) const {
        auto i=names.find(n);
        return i!=names.end()&&matched(i->second);
      }
      std::size_t begin() const {
        return spans[0].first;
      }
      std::size_t end() const {
        return spans[0].second;
      }
      Json fields() const {
        Json r=Json::object();
        for(const auto&[n,i]:names)if(matched(i))r[n]=(*this)[i];
        return r;
      }
    };
    class Regex {
      std::shared_ptr<pcre2_code> code_;
      std::map<std::string,std::size_t> names_;
      public: explicit Regex(const std::string& pattern) {
        int error=0;
        PCRE2_SIZE offset=0;
        auto* p=pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()),pattern.size(),PCRE2_UTF|PCRE2_UCP,&error,&offset,nullptr);
        if(!p) {
          std::array<unsigned char,256>b {
          };
          pcre2_get_error_message(error,b.data(),b.size());
          throw QueryError("invalid regular expression: "+std::string(reinterpret_cast<char*>(b.data())));
        }
        code_= {
          p,pcre2_code_free
        };
        uint32_t count=0,size=0;
        PCRE2_SPTR table=nullptr;
        pcre2_pattern_info(p,PCRE2_INFO_NAMECOUNT,&count);
        pcre2_pattern_info(p,PCRE2_INFO_NAMEENTRYSIZE,&size);
        pcre2_pattern_info(p,PCRE2_INFO_NAMETABLE,&table);
        for(uint32_t i=0;i<count;++i) {
          auto row=table+i*size;
          names_[reinterpret_cast<const char*>(row+2)]=(row[0]<<8)|row[1];
        }
      }
      Match search(std::string_view source,std::size_t offset=0,bool anchored=false, std::shared_ptr<const std::string> backing= {
      }) const {
        auto* d=pcre2_match_data_create_from_pattern(code_.get(),nullptr);
        if(!d)throw std::bad_alloc();
        std::unique_ptr<pcre2_match_data,decltype(&pcre2_match_data_free)> guard(d,pcre2_match_data_free);
        int n=pcre2_match(code_.get(),reinterpret_cast<PCRE2_SPTR>(source.data()),source.size(),offset,anchored?PCRE2_ANCHORED:0,d,nullptr);
        if(n==PCRE2_ERROR_NOMATCH)return {
        };
        if(n<0)throw QueryError("regular expression matching failed (PCRE2 status "+std::to_string(n)+")");
        auto* v=pcre2_get_ovector_pointer(d);
        Match r;
        r.backing=backing?std::move(backing):std::make_shared<const std::string>(source);
        r.source=*r.backing;
        r.names=names_;
        for(uint32_t i=0;i<pcre2_get_ovector_count(d);++i)r.spans.emplace_back(v[2*i],v[2*i+1]);
        return r;
      }
    };
    const Regex& regex(const std::string& pattern) {
      static thread_local std::map<std::string,Regex> cache;
      auto it=cache.find(pattern);
      if(it==cache.end())it=cache.emplace(pattern,Regex(pattern)).first;
      return it->second;
    }
    Match search(const std::string& pattern,std::string_view s,std::size_t offset=0) {
      return regex(pattern).search(s,offset);
    }
    Match match(const std::string& pattern,std::string_view s) {
      return regex(pattern).search(s,0,true);
    }
    bool full(const std::string& pattern,std::string_view s) {
      auto m=match(pattern,s);
      return m&&m.end()==s.size();
    }
    class MatchRange {
      const Regex* pattern_;
      std::shared_ptr<const std::string> source_;
      public: struct Iterator {
        const Regex* pattern;
        std::shared_ptr<const std::string> source;
        Match current;
        bool finished = true;
        void advance(std::size_t offset) {
          if (offset > source->size()) {
            finished=true;
            return;
          }
          current=pattern->search(*source,offset,false,source);
          finished=!current;
        }
        const Match& operator*() const {
          return current;
        }
        Iterator& operator++() {
          auto offset=current.end();
          if (offset==current.begin()) {
            if (offset==source->size()) {
              finished=true;
              return *this;
            }
            ++offset;
            while (offset<source->size() && (static_cast<unsigned char>((*source)[offset])&0xc0)==0x80) ++offset;
          }
          advance(offset);
          return *this;
        }
        bool operator!=(std::default_sentinel_t) const {
          return !finished;
        }
      };
      MatchRange(const std::string& pattern,std::string_view source):pattern_(&regex(pattern)),source_(std::make_shared<const std::string>(source)) {
      }
      Iterator begin() const {
        Iterator i {
          pattern_,source_, {
          },false
        };
        i.advance(0);
        return i;
      }
      std::default_sentinel_t end() const {
        return {
        };
      }
    };
    MatchRange findall(const std::string& pattern,std::string_view s) {
      return {
        pattern,s
      };
    }
    std::string replace_rx(const std::string& pattern,std::string_view s,std::string_view replacement) {
      std::string r;
      std::size_t p=0;
      for(auto m:findall(pattern,s)) {
        r+=s.substr(p,m.begin()-p);
        r+=replacement;
        p=m.end();
      }
      r+=s.substr(p);
      return r;
    }
    std::string strip_ansi(std::string_view s) {
      return replace_rx(ANSI,s,"");
    }
    Rational rational(const Json& v) {
      if(integer(v))return Rational(bigint(v));
      std::string s=encoded(v);
      auto e=s.find_first_of("eE");
      int exp=0;
      if(e!=std::string::npos) {
        exp=std::stoi(s.substr(e+1));
        s.resize(e);
      }
      auto dot=s.find('.');
      if(dot!=std::string::npos) {
        exp-=static_cast<int>(s.size()-dot-1);
        s.erase(dot,1);
      }
      Big n=decimal_integer(s), d=1;
      while(exp>0) {
        n*=10;
        --exp;
      }
      while(exp<0) {
        d*=10;
        ++exp;
      }
      return Rational(n,d);
    }
    Rational comparison_number(const Json& value) {
      if (integer(value)) return Rational(bigint(value));
      double x=value.get<double>();
      if (!std::isfinite(x)) throw QueryError("non-finite numeric comparison");
      int exponent=0;
      double fraction=std::frexp(x,&exponent);
      Big mantissa=static_cast<std::int64_t>(std::ldexp(fraction,53));
      exponent-=53;
      if (exponent>=0) return Rational(mantissa<<exponent);
      return Rational(mantissa,Big(1)<<(-exponent));
    }
    int compare(const Json& a,const Json& b) {
      if(numeric(a)&&numeric(b)) {
        auto x=comparison_number(a),y=comparison_number(b);
        return x<y?-1:x>y?1:0;
      }
      if(a.is_array()&&b.is_array()) {
        for(std::size_t i=0;i<std::min(a.size(),b.size());++i) {
          int c=compare(a[i],b[i]);
          if(c)return c;
        }
        return a.size()<b.size()?-1:a.size()>b.size()?1:0;
      }
      if(a.is_string()&&b.is_string()) {
        const auto&x=a.get_ref<const std::string&>();
        const auto&y=b.get_ref<const std::string&>();
        return x<y?-1:x>y?1:0;
      }
      if(a.is_boolean()&&b.is_boolean())return int(a.get<bool>())-int(b.get<bool>());
      if(a.is_null()&&b.is_null())return 0;
      auto x=encoded(a),y=encoded(b);
      return x<y?-1:x>y?1:0;
    }
    Json array_key(const Strings& v) {
      Json r=Json::array();
      for(const auto&s:v)r.push_back(s);
      return r;
    }
    Json concatenate(Json a,const Json& b) {
      for(const auto& v:b)a.push_back(v);
      return a;
    }
    std::string fixed_ms(const Big& n,bool sign=false) {
      std::ostringstream s;
      s<<std::fixed<<std::setprecision(3);
      if(sign)s<<std::showpos;
      s<<n.convert_to<long double>()/1000000.0L;
      return s.str();
    }
  }
  bool missing(const Json& v) {
    return v.is_discarded();
  }
  std::string encoded(const Json& v) {
    if(missing(v))return "null";
    if(v.is_binary())return bigint(v).convert_to<std::string>();
    if(v.is_array()) {
      Strings s;
      for(const auto&x:v)s.push_back(encoded(x));
      return "["+join(s,",")+"]";
    }
    if(v.is_object()) {
      std::map<std::string,const Json*> items;
      for(auto it=v.begin();it!=v.end();++it)items[it.key()]=&it.value();
      Strings s;
      for(const auto&[k,x]:items)s.push_back(Json(k).dump(-1,' ',true)+":"+encoded(*x));
      return "{"+join(s,",")+"}";
    }
    return v.dump(-1,' ',true,Json::error_handler_t::replace);
  }
  bool exact(const Json& a,const Json& b) {
    if(missing(a)||missing(b))return missing(a)&&missing(b);
    if(a.is_boolean()||b.is_boolean())return a.type()==b.type()&&a==b;
    if(numeric(a)&&numeric(b))return comparison_number(a)==comparison_number(b);
    if(a.is_object()&&b.is_object()) {
      if(a.size()!=b.size())return false;
      for(auto it=a.begin();it!=a.end();++it)if(!b.contains(it.key())||!exact(it.value(),b[it.key()]))return false;
      return true;
    }
    if(a.is_array()&&b.is_array()) {
      if(a.size()!=b.size())return false;
      for(std::size_t i=0;i<a.size();++i)if(!exact(a[i],b[i]))return false;
      return true;
    }
    return a==b;
  }
  namespace detail {
    std::string unquote(std::string_view s) {
      if(s.size()<2||s.front()!=s.back())throw QueryError("invalid quoted string");
      std::string r;
      icu::UnicodeString unicode;
      for(std::size_t i=1;i+1<s.size();++i) {
        char c=s[i];
        if(c!='\\') {
          if (c=='\n'||c=='\r') throw QueryError("unterminated quoted string");
          r+=c;
          continue;
        }
        if(++i+1>=s.size())throw QueryError("invalid quoted string");
        c=s[i];
        switch(c) {
          case 'n':r+='\n';
          break;
          case 'r':r+='\r';
          break;
          case 't':r+='\t';
          break;
          case 'b':r+='\b';
          break;
          case 'f':r+='\f';
          break;
          case 'a':r+='\a';
          break;
          case 'v':r+='\v';
          break;
          case '\\':case '\'':case '"':r+=c;
          break;
          case '\n':break;
          case 'N': {
            if (i+1>=s.size()-1 || s[i+1]!='{') throw QueryError("malformed Unicode character name");
            auto end=s.find('}',i+2);
            if (end==std::string_view::npos || end>=s.size()-1) throw QueryError("malformed Unicode character name");
            auto name=std::string(s.substr(i+2,end-i-2));
            UErrorCode status=U_ZERO_ERROR;
            auto point=u_charFromName(U_UNICODE_CHAR_NAME,name.c_str(),&status);
            if (U_FAILURE(status)) throw QueryError("unknown Unicode character name");
            unicode.remove(); unicode.append(point);
            std::string decoded; unicode.toUTF8String(decoded); r+=decoded;
            i=end;
            break;
          }
          case 'x':case 'u':case 'U': {
            std::size_t count=c=='x'?2:c=='u'?4:8;
            if(i+count>=s.size()-1)throw QueryError("truncated Unicode escape");
            std::uint32_t value=0;
            for(std::size_t k=0;k<count;++k) {
              char h=s[++i];
              unsigned d=h>='0'&&h<='9'?h-'0':h>='a'&&h<='f'?h-'a'+10:h>='A'&&h<='F'?h-'A'+10:99;
              if(d==99)throw QueryError("invalid Unicode escape");
              value=(value<<4)|d;
            }
            if(value>0x10ffff)throw QueryError("invalid Unicode code point");
            unicode.remove();
            unicode.append(static_cast<UChar32>(value));
            std::string out;
            unicode.toUTF8String(out);
            r+=out;
            break;
          }
          default:if(c>='0'&&c<='7') {
            unsigned v=c-'0';
            for(int k=0;k<2&&i+2<s.size()&&s[i+1]>='0'&&s[i+1]<='7';++k)v=v*8+s[++i]-'0';
            unicode.remove();
            unicode.append(static_cast<UChar32>(v));
            std::string out;
            unicode.toUTF8String(out);
            r+=out;
          }
          else {
            r+='\\';
            r+=c;
          }
          break;
        }
      }
      return r;
    }
    class JsonReader {
      std::string_view s_;
      std::size_t p_=0;
      void space() {
        while(p_<s_.size()&&(s_[p_]==' '||s_[p_]=='\n'||s_[p_]=='\r'||s_[p_]=='\t'))++p_;
      }
      std::string string() {
        auto start=p_++;
        bool escaped=false;
        while(p_<s_.size()) {
          char c=s_[p_++];
          if(c=='"'&&!escaped) {
            try {
              return Json::parse(s_.substr(start,p_-start)).get<std::string>();
            }
            catch(const Json::exception&) {
              throw QueryError("invalid JSON string");
            }
          }
          if(c=='\\'&&!escaped)escaped=true;
          else escaped=false;
        }
        throw QueryError("invalid or truncated JSON string");
      }
      public: JsonReader(std::string_view s,std::size_t p=0):s_(s),p_(p) {
      }
      std::size_t position()const {
        return p_;
      }
      Json read(int depth=0) {
        if(depth==0) if (observing("json.decode")) observe("json.decode", 1);
        if(depth>1000)throw QueryError("JSON nesting exceeds parser limits");
        space();
        if(p_==s_.size())throw QueryError("unexpected end of JSON");
        char c=s_[p_];
        if(c=='"')return string();
        if(c=='{'||c=='[') {
          bool obj=c=='{';
          ++p_;
          Json out=obj?Json::object():Json::array();
          space();
          if(p_<s_.size()&&s_[p_]==(obj?'}':']')) {
            ++p_;
            return out;
          }
          while(true) {
            space();
            std::string name;
            if(obj) {
              if(p_==s_.size()||s_[p_]!='"')throw QueryError("expected JSON property");
              name=string();
              space();
              if(p_==s_.size()||s_[p_++]!=':')throw QueryError("expected JSON colon");
            }
            auto value=read(depth+1);
            if(obj)out[name]=std::move(value);
            else out.push_back(std::move(value));
            space();
            if(p_==s_.size())throw QueryError("truncated JSON object");
            char delim=s_[p_++];
            if(delim==(obj?'}':']'))return out;
            if(delim!=',')throw QueryError("invalid JSON delimiter");
          }
        }
        for(auto [literal,value]:std::initializer_list<std::pair<std::string_view,Json>> {
          {
            "true",true
          }, {
            "false",false
          }, {
            "null",nullptr
          }
        })if(s_.substr(p_,literal.size())==literal) {
          p_+=literal.size();
          return value;
        }
        auto m=match(R"(-?(?:0|[1-9][0-9]*)(?:\.[0-9]+)?(?:[eE][+-]?[0-9]+)?)",s_.substr(p_));
        if(!m)throw QueryError("invalid JSON value");
        p_+=m.end();
        return scalar(m[0]);
      }
      Json complete() {
        auto value=read();
        space();
        if(p_!=s_.size())throw QueryError("extra content in JSON record");
        return value;
      }
    };
  }
  Json parse_json(std::string_view s) {
    return JsonReader(s).complete();
  }
  Json scalar(std::string_view s,bool quoted) {
    if(quoted)return unquote(s);
    if(s=="true")return true;
    if(s=="false")return false;
    if(s=="null")return nullptr;
    if(full(NUMBER,s)) {
      if(s.find_first_of(".eE")==std::string_view::npos) {
        if(s.size()-s.starts_with('-')>4300)throw QueryError("numeric literal exceeds interpreter limits");
        return number(decimal_integer(s));
      }
      std::string token(s);
      char* end=nullptr;
      double value=std::strtod(token.c_str(),&end);
      if(end!=token.c_str()+token.size()||!std::isfinite(value))throw QueryError("numeric literals must be finite");
      return value;
    }
    return std::string(s);
  }
  std::string normalized_text(std::string_view value) {
    auto u=icu::UnicodeString::fromUTF8(icu::StringPiece(value.data(),static_cast<int32_t>(value.size())));
    icu::UnicodeString out;
    bool pending=false;
    for(int32_t i=0;i<u.length();) {
      auto c=u.char32At(i);
      i+=U16_LENGTH(c);
      if(u_isUWhiteSpace(c)||(c>=0x1c&&c<=0x1f)) {
        pending=!out.isEmpty();
        continue;
      }
      if(pending)out.append(static_cast<UChar32>(' '));
      pending=false;
      out.append(c);
    }
    out.foldCase();
    std::string s;
    return out.toUTF8String(s);
  }
  std::string record_text(const Record& r) {
    if (observing("record.text")) observe("record.text", r.source);
    Strings values;
    std::vector<const Json*>pending;
    for(auto it=r.data.end();it!=r.data.begin();)pending.push_back(&*--it);
    while(!pending.empty()) {
      auto*v=pending.back();
      pending.pop_back();
      if(v->is_string())values.push_back(v->get<std::string>());
      else if(v->is_object()||v->is_array())for(auto it=v->end();it!=v->begin();)pending.push_back(&*--it);
    }
    return normalized_text(join(values," "));
  }
  Json lookup(const Json& data,std::string_view name) {
    if(data.is_object()) {
      auto it=data.find(std::string(name));
      if(it!=data.end())return *it;
    }
    const Json* v=&data;
    for(const auto&p:tools::split(name,'.',true)) {
      if(v->is_object()) {
        auto it=v->find(p);
        if(it==v->end())return MISSING;
        v=&*it;
      }
      else if(v->is_array()) {
        if(!full(R"(0|[1-9][0-9]*)",p))return MISSING;
        std::size_t index=0;
        auto[e,ec]=std::from_chars(p.data(),p.data()+p.size(),index);
        if(ec!=std::errc()||index>=v->size())return MISSING;
        v=&(*v)[index];
      }
      else return MISSING;
    }
    return *v;
  }
  Json value_from(const Json& data,std::initializer_list<std::string_view>names) {
    for(auto n:names) {
      auto v=lookup(data,n);
      if(present(v))return v;
    }
    return MISSING;
  }
  namespace detail {
    std::optional<std::pair<std::string,Big>> timestamp_ns(const Json& v) {
      if(!v.is_string())return {
      };
      auto s=v.get<std::string>();
      s=replace_rx(" UTC",s,"+00:00");
      s=replace_rx("Z",s,"+00:00");
      auto m=match(R"(^(\d{4})-?(\d\d)-?(\d\d)(?:[ T](\d\d):?(\d\d)(?::?(\d\d))?(?:[.,](\d+))?)?(?:([+-])(\d\d):?(\d\d)(?::?(\d\d))?)?$)",s);
      if(!m)return {
      };
      int y=std::stoi(m[1]),mo=std::stoi(m[2]),d=std::stoi(m[3]);
      std::chrono::year_month_day day {
        std::chrono::year(y),std::chrono::month(static_cast<unsigned>(mo)),std::chrono::day(static_cast<unsigned>(d))
      };
      int h=m[4].empty()?0:std::stoi(m[4]),min=m[5].empty()?0:std::stoi(m[5]),sec=m[6].empty()?0:std::stoi(m[6]);
      if(y<1||!day.ok()||h>23||min>59||sec>59)return {
      };
      Big value=std::chrono::sys_days(day).time_since_epoch().count();
      value=(value*86400+h*3600+min*60+sec)*1000000000;
      if(!m[7].empty())value+=decimal_integer((m[7]+"000000000").substr(0,9));
      if(!m[8].empty()) {
        int oh=std::stoi(m[9]),om=std::stoi(m[10]),os=m[11].empty()?0:std::stoi(m[11]);
        if(oh>23||om>59||os>59)return {
        };
        Big offset=Big(oh*3600+om*60+os)*1000000000;
        value+=m[8]=="+"?-offset:offset;
      }
      return std::pair {
        m[8].empty()?"wall-local":"wall",value
      };
    }
    std::pair<std::string,std::optional<Big>> record_time(const Json& data,const std::string& source) {
      for(auto[name,clock]: {
        std::pair {
          "steady_ns","steady"
        }, {
          "monotonic_ns","steady"
        }, {
          "timestamp_ns","wall"
        }, {
          "unix_ns","wall"
        }
      }) {
        auto v=value_from(data, {
          name,"fields."+std::string(name)
        });
        if(integer(v))return {
          clock,bigint(v)
        };
      }
      for(auto n: {
        "timestamp","time","fields.timestamp"
      })if(auto p=timestamp_ns(lookup(data,n)))return {
        p->first,p->second
      };
      auto e=value_from(data, {
        "elapsed_ms","fields.elapsed_ms"
      });
      if(integer(e))return {
        "elapsed:"+source,bigint(e)*1000000
      };
      if(e.is_number_float()&&std::isfinite(e.get<double>())) {
        auto v=rational(e)*Rational(1000000);
        return {
          "elapsed:"+source,v.numerator()/v.denominator()
        };
      }
      return {
        "none", {
        }
      };
    }
    Json physical_identity(const Json& d,std::string text,std::string high,std::string low) {
      auto v=value_from(d, {
        text,"fields."+text
      }),h=value_from(d, {
        high,"fields."+high
      }),l=value_from(d, {
        low,"fields."+low
      });
      Json textual=MISSING,numeric_id=MISSING;
      if(v.is_string()&&full("[0-9a-fA-F]{32}",str(v))&&Big("0x"+str(v))!=0)textual=lower(str(v));
      Big max=Big(1)<<64;
      if(integer(h)&&integer(l)&&bigint(h)>=0&&bigint(l)>=0&&bigint(h)<max&&bigint(l)<max&&(bigint(h)!=0||bigint(l)!=0)) {
        std::ostringstream s;
        s<<std::hex<<std::setfill('0')<<std::setw(16)<<bigint(h).convert_to<std::uint64_t>()<<std::setw(16)<<bigint(l).convert_to<std::uint64_t>();
        numeric_id=s.str();
      }
      if((!missing(v)&&missing(textual))||((!missing(h)||!missing(l))&&missing(numeric_id))||(!missing(textual)&&!missing(numeric_id)&&textual!=numeric_id))return MISSING;
      return !missing(textual)?textual:numeric_id;
    }
    bool source_lifecycle(const Json& d) {
      auto e=value_from(d, {
        "fields.event","fields.name","event","name"
      });
      if(!e.is_string())return false;
      auto s=str(e);
      return s.starts_with("firefox.workspace.source.")||(s.starts_with("presentation.source.")&&!s.ends_with("read_submitted"));
    }
    Json surface_identity(const Json&d) {
      return source_lifecycle(d)?physical_identity(d,"arena","workspace_arena_high","workspace_arena_low"):physical_identity(d,"surface","surface_high","surface_low");
    }
    Json workspace_source_identity(const Json&d) {
      return physical_identity(d,source_lifecycle(d)?"surface":"source","workspace_source_high","workspace_source_low");
    }
    std::optional<std::string> vulkan_handle(const Json&v,bool allow_null=false) {
      if(v.is_string()&&full("0x[0-9a-fA-F]{1,16}",str(v))) {
        Big n(str(v));
        if(n!=0||allow_null) {
          std::ostringstream s;
          s<<"0x"<<std::hex<<n;
          return s.str();
        }
      }
      return {
      };
    }
    std::vector<std::pair<std::string,std::string>> vulkan_resources(const Json&d) {
      std::vector<std::pair<std::string,std::string>>r;
      std::size_t remaining=limits.vulkan_objects;
      auto add=[&](std::string field,std::string handle) {
        std::pair p {
          field,handle
        };
        mmltk::tools::append_unique(r, p);
      };
      for(auto c: {
        "objects","referenced_objects"
      }) {
        auto objects=get(d,c);
        if(objects.is_array()) {
          auto count=std::min(remaining,objects.size());
          for(std::size_t i=0;i<count;++i) {
            auto kind=get(objects[i],"type");
            if(kind.is_string()) {
              auto it=VULKAN_RESOURCE_FIELDS.find(str(kind));
              if(it!=VULKAN_RESOURCE_FIELDS.end())if(auto h=vulkan_handle(get(objects[i],"handle")))add(it->second,*h);
            }
          }
          remaining-=count;
        }
      }
      for(const auto&[kind,name]:VULKAN_RESOURCE_FIELDS)if(auto h=vulkan_handle(value_from(d, {
        name,"fields."+name
      })))add(name,*h);
      return r;
    }
    Json exact_process_id(Json v) {
      if(v.is_string()&&full("[1-9][0-9]{0,9}",str(v)))v=number(decimal_integer(str(v)));
      return integer(v)&&bigint(v)>0&&bigint(v)<=2147483647?v:MISSING;
    }
    Json process_identity(const Record&r,std::string_view field) {
      if(in(r.get("@event"),FIREFOX_PROCESS_EVENTS)&&equals(r.get("@owner"),"firefox_process")) {
        if(field=="@process"||field=="@process_group")return exact_process_id(r.get("sequence"));
        return MISSING;
      }
      Strings names=field=="@process"?words("pid process_id browser_process_id"):field=="@parent_process"?words("ppid parent_process_id"):words("pgid process_group_id");
      Json out=MISSING;
      for(auto&n:names) {
        auto observed=r.get(n);
        if(missing(observed))continue;
        auto value=exact_process_id(observed);
        if(missing(value)||(!missing(out)&&!exact(out,value)))return MISSING;
        out=value;
      }
      return out;
    }
    bool process_evidence(const Record&r) {
      auto e=r.get("@event");
      return equals(e,"fatal.report")||equals(e,"firefox.workspace.channel_terminal")||(in(e,FIREFOX_PROCESS_EVENTS)&&equals(r.get("@owner"),"firefox_process"));
    }
    Json vulkan_process(const Record&r) {
      Json out=nullptr;
      for(auto n: {
        "browser_process_id","pid"
      }) {
        auto v=r.get(n);
        if(missing(v))continue;
        if(!integer(v)||bigint(v)<=0||(!out.is_null()&&!exact(out,v)))return MISSING;
        out=v;
      }
      return out;
    }
    bool test_event(const Json&e) {
      if(!e.is_string())return false;
      auto s=str(e);
      for(auto runner: {
        "catch","tap","libtest"
      })for(auto state: {
        "started","passed","failed","skipped"
      })if(s==std::string(runner)+".test_"+state)return true;
      return false;
    }
    bool test_start(const Json&e) {
      return test_event(e)&&str(e).ends_with("started");
    }
    std::string signal_name(int n) {
      static const std::map<int,std::string>names= {
        {
          SIGHUP,"SIGHUP"
        }, {
          SIGINT,"SIGINT"
        }, {
          SIGQUIT,"SIGQUIT"
        }, {
          SIGILL,"SIGILL"
        }, {
          SIGTRAP,"SIGTRAP"
        }, {
          SIGABRT,"SIGABRT"
        }, {
          SIGBUS,"SIGBUS"
        }, {
          SIGFPE,"SIGFPE"
        }, {
          SIGKILL,"SIGKILL"
        }, {
          SIGUSR1,"SIGUSR1"
        }, {
          SIGSEGV,"SIGSEGV"
        }, {
          SIGUSR2,"SIGUSR2"
        }, {
          SIGPIPE,"SIGPIPE"
        }, {
          SIGALRM,"SIGALRM"
        }, {
          SIGTERM,"SIGTERM"
        }, {
          SIGSTKFLT,"SIGSTKFLT"
        }, {
          SIGCHLD,"SIGCHLD"
        }, {
          SIGCONT,"SIGCONT"
        }, {
          SIGSTOP,"SIGSTOP"
        }, {
          SIGTSTP,"SIGTSTP"
        }, {
          SIGTTIN,"SIGTTIN"
        }, {
          SIGTTOU,"SIGTTOU"
        }, {
          SIGURG,"SIGURG"
        }, {
          SIGXCPU,"SIGXCPU"
        }, {
          SIGXFSZ,"SIGXFSZ"
        }, {
          SIGVTALRM,"SIGVTALRM"
        }, {
          SIGPROF,"SIGPROF"
        }, {
          SIGWINCH,"SIGWINCH"
        }, {
          SIGIO,"SIGIO"
        }, {
          SIGPWR,"SIGPWR"
        }, {
          SIGSYS,"SIGSYS"
        }
      };
      if(auto i=names.find(n);i!=names.end())return i->second;
      if(n==SIGRTMIN)return "SIGRTMIN";
      if(n==SIGRTMAX)return "SIGRTMAX";
      return "SIG"+std::to_string(n);
    }
  }
  // Normalized records retain physical provenance and original native facts.
  Record::Record(std::string s,std::size_t l,std::string r,Json d,std::string f,std::string err,Json m):source(std::move(s)),line(l),raw(std::move(r)),data(std::move(d)),format(std::move(f)),parse_error(std::move(err)),metadata(std::move(m)) {
    auto t=record_time(data,source);
    clock=std::move(t.first);
    time_ns=std::move(t.second);
  }
  bool Record::training_record()const {
    auto v=log_query::get(data,"format_version");
    if(!integer(v)||bigint(v)!=3||!in(log_query::get(data,"role"),words("Live Boundary Epoch Terminal")))return false;
    for(auto name: {
      "run_id","attempt_id"
    }) {
      auto x=log_query::get(data,name);
      if(!x.is_string()||str(x).empty()||chars(str(x))>64||trim(str(x)).empty())return false;
    }
    return log_query::get(data,"progress").is_object()&&in(lookup(data,"progress.phase"),words("Starting Train Validate EpochComplete Completed Error Cancelled Merge"));
  }
  Json Record::get(std::string_view name)const {
    if (observing("record.get")) observe("record.get", std::string(name));
    if(name=="@file")return source;
    if(name=="@line")return line;
    if(name=="@line_end")return log_query::get(metadata,"line_end",line);
    if(name=="@text")return raw;
    if(name=="@format")return format;
    if(name=="@clock")return clock;
    if(name=="@time_ns")return time_ns?number(*time_ns):MISSING;
    if(name=="@parse_error")return parse_error.empty()?MISSING:Json(parse_error);
    if(name=="@surface")return surface_identity(data);
    if(name=="@workspace_source")return workspace_source_identity(data);
    if(name=="@process"||name=="@parent_process"||name=="@process_group")return process_identity(*this,name);
    if(name=="@intent_request") {
      auto e=get("@event");
      Json v;
      if(equals(e,"integration.intent"))v=value_from(data, {
        "control","fields.control"
      });
      else if(in(e,words("browser.intent.received browser.intent.accepted browser.intent.rejected browser.intent.reply_rejected")))v=value_from(data, {
        "value","fields.value"
      });
      else return MISSING;
      if(v.is_string()&&full("[0-9]{1,20}",str(v)))v=number(decimal_integer(str(v)));
      return integer(v)&&bigint(v)>0&&bigint(v)<(Big(1)<<64)?v:MISSING;
    }
    if(training_record()) {
      static const std::map<std::string,std::string> mapping= {
        {
          "@session","progress.session_id"
        }, {
          "@model","progress.model_id"
        }, {
          "@first_cause","progress.failure.first_cause"
        }, {
          "@phase","progress.phase"
        }, {
          "@role","role"
        }
      };
      if(auto i=mapping.find(std::string(name));i!=mapping.end())return lookup(data,i->second);
    }
    if(name=="@event") {
      if(training_record())return "training."+lower(str(log_query::get(data,"role")))+"."+lower(str(lookup(data,"progress.phase")));
      return value_from(data, {
        "fields.event","fields.name","event","name"
      });
    }
    if(name=="@owner") {
      auto owner=value_from(data, {
        "owner","fields.owner","component","system","module","logger"
      }),event=get("@event");
      return missing(owner)&&event.is_string()?Json(str(event).substr(0,str(event).find('.'))):owner;
    }
    if(name=="@level") {
      auto v=value_from(data, {
        "level","severity","fields.level"
      });
      if(missing(v))return MISSING;
      auto s=lower(str(v));
      static const std::map<std::string,std::string>mapping= {
        {
          "v","trace"
        }, {
          "t","trace"
        }, {
          "d","debug"
        }, {
          "i","info"
        }, {
          "w","warning"
        }, {
          "warn","warning"
        }, {
          "e","error"
        }, {
          "err","error"
        }, {
          "f","fatal"
        }
      };
      auto i=mapping.find(s);
      return i==mapping.end()?s:i->second;
    }
    if(name=="@error") {
      if(training_record())return equals(log_query::get(data,"role"),"Terminal")&&equals(lookup(data,"progress.phase"),"Error");
      auto code=get("@exit_code");
      if(!missing(code)&&!exact(code,0)&&!exact(code,false))return true;
      if(in(get("@level"),words("error critical fatal panic")))return true;
      auto event=get("@event");
      if(format=="transcript"&&(test_event(event)||in(event,words("build.progress build.source tap.summary tap.diagnostic libtest.summary libtest.output"))))return false;
      for(auto v: {
        event,value_from(data, {
          "message","fields.message","detail","error"
        })
      })if(v.is_string()&&search(FAILURE_WORD,str(v)))return true;
      return false;
    }
    if(name=="@exit_code")return in(get("@event"),words("child.signaled child.exited"))?get("value"):get("exit_code");
    if(name=="@signal"||name=="@signal_number") {
      auto c=get("@exit_code");
      if(!equals(get("@event"),"child.signaled")||!integer(c))return MISSING;
      Big n=bigint(c)-128;
      if(n<=0||n>64||n==32||n==33)return MISSING;
      int k=n.convert_to<int>();
      return name=="@signal_number"?Json(k):Json(signal_name(k));
    }
    if(name=="@terminal") {
      auto e=get("@event");
      return (training_record()&&equals(log_query::get(data,"role"),"Terminal"))||(test_event(e)&&!test_start(e))||in(e,words("child.signaled child.exited shutdown.complete shutdown.firefox_terminal catch.test_selection_failed catch.summary tap.summary libtest.summary process.terminal build.image build.failed"));
    }
    if(name.starts_with('@'))return log_query::get(metadata,name.substr(1));
    auto v=lookup(data,name);
    if(missing(v)&&name.find('.')==std::string_view::npos)v=lookup(data,"fields."+std::string(name));
    if(missing(v)) {
      Strings handles;
      for(const auto&[field,handle]:vulkan_resources(data))if(field==name)handles.push_back(handle);
      if(handles.size()==1)return handles[0];
    }
    return v;
  }
  bool Expression::matches(const Record&r)const {
    if(operation=="all")return true;
    if(operation=="and")return std::all_of(children.begin(),children.end(),[&](const auto&e) {
      return e.matches(r);
    });
    if(operation=="or")return std::any_of(children.begin(),children.end(),[&](const auto&e) {
      return e.matches(r);
    });
    if(operation=="not")return !children.at(0).matches(r);
    if(operation=="text") {
      auto needle=lower(str(arguments[0]));
      if(lower(r.raw).find(needle)!=std::string::npos)return true;
      for(auto n: {
        "@test","@tags","@run","@archive_id","@family","@signal"
      }) {
        auto v=r.get(n);
        if(!missing(v)&&lower(str(v)).find(needle)!=std::string::npos)return true;
      }
      return false;
    }
    if(operation=="phrase"||operation=="words") {
      auto text=record_text(r);
      if(operation=="phrase")return std::any_of(arguments.begin(),arguments.end(),[&](const Json&s) {
        return text.find(str(s))!=std::string::npos;
      });
      return std::all_of(arguments.begin(),arguments.end(),[&](const Json&s) {
        return text.find(str(s))!=std::string::npos;
      });
    }
    auto v=r.get(str(arguments[0]));
    if(operation=="has")return !missing(v);
    if(missing(v))return false;
    const auto&e=arguments[1];
    if(operation=="="||operation=="!=") {
      bool equal=exact(v,e);
      return operation=="="?equal:!equal;
    }
    if(operation==":")return lower(str(v)).find(lower(str(e)))!=std::string::npos;
    if(operation=="~"||operation=="!~") {
      bool found=bool(search(str(e),str(v)));
      return operation=="~"?found:!found;
    }
    if(!numeric(v)||v.is_boolean())return false;
    int c=compare(v,e);
    return operation==">"?c>0:operation==">="?c>=0:operation=="<"?c<0:c<=0;
  }
  QueryParser::QueryParser(std::string source) {
    if(chars(source)>MAX_QUERY_LENGTH)throw QueryError("query exceeds 8192 characters");
    std::size_t p=0;
    const std::string pattern=R"rx(\s*(?:("(?:\\.|[^"\\])*"|'(?:\\.|[^'\\])*')|(!=|!~|>=|<=|[():=~<>])|([^\s():=~<>!"']+)))rx";
    while(!trim(std::string_view(source).substr(p)).empty()) {
      auto m=regex(pattern).search(source,p,true);
      if(!m)throw QueryError("unexpected character at column "+std::to_string(chars(source.substr(0,p))+1));
      tokens_.push_back( {
        m.matched(1)?m[1]:m.matched(2)?m[2]:m[3],m.matched(1)
      });
      p=m.end();
      if(tokens_.size()>MAX_QUERY_TOKENS)throw QueryError("query exceeds 512 tokens");
    }
  }
  bool QueryParser::peek(std::string_view v)const {
    return index_<tokens_.size()&&!tokens_[index_].quoted&&lower(tokens_[index_].text)==v;
  }
  QueryParser::Token QueryParser::take() {
    if(index_==tokens_.size())throw QueryError("unexpected end of query");
    return tokens_[index_++];
  }
  void QueryParser::require(std::string_view v) {
    if(!peek(v))throw QueryError("expected '"+std::string(v)+"' at token "+std::to_string(index_+1));
    ++index_;
  }
  Expression QueryParser::parse() {
    if(tokens_.empty())return {
    };
    auto e=parse_or(0);
    if(index_!=tokens_.size())throw QueryError("unexpected token '"+tokens_[index_].text+"'");
    return e;
  }
  Expression QueryParser::parse_or(std::size_t d) {
    std::vector<Expression>parts {
      parse_and(d)
    };
    while(peek("or")) {
      take();
      parts.push_back(parse_and(d));
    }
    return parts.size()==1?parts[0]:Expression {
      "or",Json::array(),std::move(parts)
    };
  }
  Expression QueryParser::parse_and(std::size_t d) {
    std::vector<Expression>parts {
      primary(d)
    };
    while(index_<tokens_.size()&&!peek("or")&&!peek(")")) {
      if(peek("and"))take();
      parts.push_back(primary(d));
    }
    return parts.size()==1?parts[0]:Expression {
      "and",Json::array(),std::move(parts)
    };
  }
  Expression QueryParser::primary(std::size_t d) {
    if(d>MAX_QUERY_DEPTH)throw QueryError("query nesting exceeds 32");
    if(peek("not")) {
      take();
      return {
        "not",Json::array(), {
          primary(d+1)
        }
      };
    }
    if(peek("(")) {
      take();
      auto e=parse_or(d+1);
      require(")");
      return e;
    }
    if(peek("has")) {
      take();
      require("(");
      auto t=take();
      if(t.quoted||!full(FIELD_NAME,t.text))throw QueryError("mmltk::tools::includes() requires a field name");
      require(")");
      return {
        "has",Json::array( {
          t.text
        }), {
        }
      };
    }
    auto t=take();
    if(!t.quoted&&mmltk::tools::includes(words("and or ) = : ~ !~ != > >= < <="),lower(t.text)))throw QueryError("expected a search term or field, received '"+t.text+"'");
    if(index_<tokens_.size()&&!tokens_[index_].quoted) {
      auto op=tokens_[index_].text;
      if(mmltk::tools::includes(words("= != : ~ !~ > >= < <="),op)) {
        if(t.quoted||!full(FIELD_NAME,t.text))throw QueryError("comparisons require an unquoted field name");
        take();
        auto v=take();
        if(!v.quoted&&mmltk::tools::includes(words("( ) = : ~ !~ != > >= < <="),v.text))throw QueryError("comparison requires a value; quote literal punctuation");
        auto expected=scalar(v.text,v.quoted);
        if(mmltk::tools::includes(words("> >= < <="),op)&&!numeric(expected))throw QueryError(op+" requires a numeric value");
        if(op=="~"||op=="!~") {
          auto pattern=str(expected);
          std::string native_pattern;
          for (std::size_t k=0; k<pattern.size(); ++k) {
            if (pattern[k]=='\\' && k+1<pattern.size()) {
              native_pattern+='\\';
              native_pattern+=pattern[k+1]=='Z'?'z':pattern[k+1];
              ++k;
            } else native_pattern+=pattern[k];
          }
          expected=native_pattern;
          regex(native_pattern);
        }
        return {
          op,Json::array( {
            t.text,expected
          }), {
          }
        };
      }
    }
    return t.text=="*"&&!t.quoted?Expression {
    }
    :Expression {
      "text",Json::array( {
        t.quoted?scalar(t.text,true):Json(t.text)
      }), {
      }
    };
  }
  namespace detail {
    std::vector<std::pair<std::size_t,Json>> embedded_objects(std::string_view s) {
      std::vector<std::pair<std::size_t,Json>> result;
      std::size_t pos=0;
      int attempts=0;
      while(pos<s.size()&&attempts++<32) {
        auto m=search(R"rx(\{|"(?:\\.|[^"\\])*")rx",s,pos);
        if(!m)break;
        auto start=m.begin();
        try {
          JsonReader reader(s,start);
          auto v=reader.read();
          pos=reader.position();
          if(v.is_object())result.emplace_back(start,std::move(v));
          else if(v.is_string()) {
            auto text=str(v);
            auto p=text.find('{');
            if(p!=std::string::npos) {
              try {
                auto inner=JsonReader(text,p).read();
                if(inner.is_object())result.emplace_back(start,std::move(inner));
              }
              catch(const QueryError&) {
              }
            }
          }
        }
        catch(const QueryError&) {
          pos=start+1;
        }
      }
      return result;
    }
    Json transcript_data(const std::string& text) {
      auto m=match(TEST_STATUS,text);
      if(m) {
        static const std::map<std::string,std::string>states= {
          {
            "RUN","started"
          }, {
            "FAILED","failed"
          }, {
            "OK","passed"
          }, {
            "SKIPPED","skipped"
          }
        };
        auto state=states.at(m[1]);
        return {
          {
            "event","catch.test_"+state
          }, {
            "test",m[2]
          }, {
            "level",state=="failed"?"error":"info"
          }, {
            "message",text
          }
        };
      }
      m=match(TAP_STATUS,text);
      if(m) {
        std::string state=m.matched(3)?"skipped":lower(m[1])=="not ok"?"failed":"passed";
        return {
          {
            "event","tap.test_"+state
          }, {
            "test",m[2]
          }, {
            "level",state=="failed"?"error":"info"
          }, {
            "message",text
          }
        };
      }
      auto stripped=trim(text);
      if(stripped.starts_with("# Subtest: "))return {
        {
          "event","tap.test_started"
        }, {
          "test",stripped.substr(11)
        }, {
          "level","info"
        }, {
          "message",text
        }
      };
      m=match(R"(^\s*# (tests|pass|fail|cancelled|skipped|todo) (\d+)\s*$)",text);
      if(m) {
        auto count=scalar(m[2]);
        bool failed=((m[1]=="fail"||m[1]=="cancelled")&&!exact(count,0))||(m[1]=="tests"&&exact(count,0));
        return {
          {
            "event","tap.summary"
          }, {
            "metric",m[1]
          }, {
            "count",count
          }, {
            "level",failed?"error":"info"
          }, {
            "message",text
          }
        };
      }
      m=match(LIBTEST_STATUS,text);
      if(m) {
        std::string state=m[2]=="ok"?"passed":m[2]=="FAILED"?"failed":"skipped";
        return {
          {
            "event","libtest.test_"+state
          }, {
            "test",m[1]
          }, {
            "level",state=="failed"?"error":"info"
          }, {
            "message",text
          }
        };
      }
      m=match(R"(^---- (.+) (?:stdout|stderr) ----$)",text);
      if(m)return {
        {
          "event","libtest.output"
        }, {
          "test",m[1]
        }, {
          "message",text
        }
      };
      if(text.starts_with("test result: ok.")||text.starts_with("test result: FAILED."))return {
        {
          "event","libtest.summary"
        }, {
          "level",text.starts_with("test result: FAILED.")?"error":"info"
        }, {
          "message",text
        }
      };
      m=match(CATCH_FAILURE,text);
      if(m) {
        auto state=lower(m[3]);
        return {
          {
            "event","catch.assertion_"+replace_rx(" ",state,"_")
          }, {
            "source_file",m[1]
          }, {
            "source_line",number(decimal_integer(m[2]))
          }, {
            "level",state=="failed"||state=="fatal error"?"error":"info"
          }, {
            "message",m[4]
          }
        };
      }
      if(stripped=="{Unknown expression after the reported line}")return {
        {
          "event","catch.expression"
        }, {
          "message",text
        }
      };
      if(text.starts_with("Filters:"))return {
        {
          "event","catch.filters"
        }, {
          "filters",trim(text.substr(8))
        }, {
          "message",text
        }
      };
      if(text.starts_with("No test cases matched "))return {
        {
          "event","catch.test_selection_failed"
        }, {
          "level","error"
        }, {
          "message",text
        }
      };
      if(text=="No tests ran")return {
        {
          "event","catch.summary"
        }, {
          "level","error"
        }, {
          "tests",0
        }, {
          "message",text
        }
      };
      if(text.starts_with("test cases:")||text.starts_with("assertions:"))return {
        {
          "event","catch.summary"
        }, {
          "message",text
        }
      };
      m=search(R"((?:terminal status:\s*exit|exit(?:ed with)?(?:\s+status|\s+code))\s*[=:]?\s*(\d+))",text);
      if(m)return {
        {
          "event","process.terminal"
        }, {
          "exit_code",scalar(m[1])
        }, {
          "message",text
        }
      };
      if(text.starts_with("mmltk:"))return {
        {
          "event","mmltk.message"
        }, {
          "owner","mmltk"
        }, {
          "message",trim(text.substr(6))
        }
      };
      if(search(R"((?:writing image|naming to|exporting (?:image|manifest)|Successfully (?:built|tagged)))",text))return {
        {
          "event","build.image"
        }, {
          "owner","build"
        }, {
          "message",text
        }
      };
      if(text.starts_with("FAILED:")||text.starts_with("ERROR: failed to build")||text.starts_with("ninja: build stopped"))return {
        {
          "event","build.failed"
        }, {
          "owner","build"
        }, {
          "level","error"
        }, {
          "message",text
        }
      };
      if(match(R"(\[\d+/\d+\] (?:Building|Linking|Generating|Scanning|Re-checking)\b)",text))return {
        {
          "event","build.progress"
        }, {
          "owner","build"
        }, {
          "message",text
        }
      };
      if(match(R"(\s*\d+\s*\|)",text))return {
        {
          "event","build.source"
        }, {
          "owner","build"
        }, {
          "message",text
        }
      };
      return nullptr;
    }
    std::optional<std::string> vulkan_diagnostic(const std::string&message,Json&data) {
      auto header=match(VULKAN_HEADER,message);
      if(!header&&message.find('\n')==std::string::npos&&!match(VULKAN_BODY,message))return {
      };
      Match body;
      for(auto&line:tools::lines(message))if((body=match(VULKAN_BODY,line)))break;
      if(!header&&!body)return {
      };
      data["event"]="vulkan.validation";
      data["validation_id"]=header?header["id"]:body["id"];
      data["message"]=message;
      if(body&&!data.contains("level"))data["level"]=body["level"];
      auto id=search(R"(\bMessageID\s*=\s*(0x[0-9a-fA-F]+))",message);
      if(id||header.matched("message_id"))data["message_id"]=id?id[1]:header["message_id"];
      auto api=search(R"(\b(vk[A-Z][A-Za-z0-9]+)\s*\()",message);
      if(api)data["api"]=api[1];
      Json objects=Json::array(),refs=Json::array();
      bool overflow=false;
      for(int p=0;p<3;++p) {
        const auto& pattern=p==0?VULKAN_OBJECT:p==1?VULKAN_RUST_OBJECT:VULKAN_REFERENCE;
        auto text=p==2?replace_rx(VULKAN_RUST_OBJECT,message,""):message;
        auto&dest=p==2?refs:objects;
        for(auto m:findall(pattern,text)) {
          auto handle=vulkan_handle(m["handle"],true);
          if(!handle)continue;
          auto kind=m["type"];
          if(p==2) {
            kind=replace_rx(R"((?<=[a-z0-9])(?=[A-Z])|(?<=[A-Z])(?=[A-Z][a-z]))",kind,"_");
            std::transform(kind.begin(),kind.end(),kind.begin(),[](unsigned char c) {
              return static_cast<char>(std::toupper(c));
            });
          }
          auto it=std::find_if(dest.begin(),dest.end(),[&](const Json&v) {
            return equals(v["type"],kind)&&equals(v["handle"],*handle);
          });
          if(it==dest.end()) {
            if(objects.size()+refs.size()==limits.vulkan_objects) {
              overflow=true;
              continue;
            }
            dest.push_back( {
              {
                "type",kind
              }, {
                "handle",*handle
              }
            });
            it=dest.end()-1;
          }
          if(m.matched("name"))(*it)["name"]=m["name"];
        }
      }
      data["objects"]=objects;
      if(!refs.empty())data["referenced_objects"]=refs;
      return overflow?"Vulkan object facts exceed bounded capacity; original text retained":"";
    }
    void merge(Json&target,const Json&source) {
      if(!source.is_object())throw QueryError("expected JSON object");
      for(auto it=source.begin();it!=source.end();++it)target[it.key()]=it.value();
    }
    void parse_pairs(Json&data,const std::string&s,bool only_missing=false) {
      for(auto m:findall(KEY_VALUE,s)) {
        if(only_missing&&data.contains(m[1]))continue;
        try {
          auto value=m[2];
          data[m[1]]=scalar(value,value.starts_with('"')||value.starts_with('\''));
        }
        catch(const QueryError&) {
          data[m[1]]=m[2];
        }
      }
    }
  }
  Record parse_record(std::string source,std::size_t line,std::string raw,bool truncated) {
    auto clean=strip_ansi(rstrip_newline(raw));
    Json data=Json::object();
    std::string format="text",message=clean;
    auto context=search(CONTEXT_LABEL,clean);
    auto payload=context?clean.substr(context.end()):clean;
    auto m=match(NATIVE,payload);
    if(m)format="native";
    else if((m=match(FIREFOX,payload)))format="firefox";
    else if((m=match(RUST,payload)))format="rust";
    if(m) {
      data=m.fields();
      for(auto key: {
        "pid","tid"
      })if(data.contains(key))data[key]=number(decimal_integer(str(data[key])));
      message=str(data["message"]);
    }
    std::string error=truncated?"line exceeds byte limit; text is truncated":"";
    auto fatal=match(FATAL,message);
    if(fatal) {
      merge(data, {
        {
          "event","fatal.report"
        }, {
          "level","fatal"
        }, {
          "component",fatal["component"]
        }, {
          "message",message
        }, {
          "detail",fatal["detail"]
        }
      });
      auto envelope=match(FATAL_STATUS,fatal["detail"]);
      if(envelope) {
        for(auto key: {
          "operation","status"
        })data[key]=envelope[key];
        for(auto key: {
          "pid","ppid","pgid"
        })data[key]=number(decimal_integer(envelope[key]));
        parse_pairs(data,trim(envelope["context"]),true);
      }
      else if(auto status=search(R"( \(status=([^()]*)\)$)",fatal["detail"]))data["status"]=status[1];
      return {
        source,line,clean,data,"fatal",error
      };
    }
    if(auto ve=vulkan_diagnostic(message,data))return {
      source,line,rstrip_newline(raw),data,format,error.empty()?*ve:error
    };
    auto transcript=transcript_data(clean);
    if(!transcript.is_null()) {
      merge(data,transcript);
      format="transcript";
    }
    else if(message.find('{')!=std::string::npos) {
      try {
        Json first=MISSING;
        bool malformed=false;
        std::vector<std::pair<std::size_t,Json>> objects;
        if((source.ends_with(".jsonl")||source.ends_with(".json"))&&trim(message).starts_with('{')) {
          try {
            first=parse_json(trim(message));
            if(!first.is_object())throw QueryError("expected a JSON object");
          }
          catch(const QueryError&) {
            malformed=true;
            first=MISSING;
          }
        }
        if(missing(first)) {
          objects=embedded_objects(message);
          if(!objects.empty())first=objects.front().second;
        }
        if(!missing(first)) {
          if(objects.size()>1&&error.empty())error="multiple JSON payloads on one line; first retained";
          if(malformed&&error.empty())error="extra or malformed content in JSONL record";
          merge(data,first);
          if(equals(get(data,"message"),message)&&!first.contains("message"))data.erase("message");
          format="json";
        }
        else if(trim(message).starts_with('{')||source.ends_with(".jsonl")) {
          if(error.empty())error="invalid or truncated JSON object";
        }
      }
      catch(const QueryError&) {
        if(error.empty())error="invalid or truncated JSON object";
      }
    }
    else if(source.ends_with(".jsonl")&&error.empty())error="expected a JSON object";
    if(format!="json"&&format!="transcript") {
      if(!data.contains("message"))data["message"]=message;
      parse_pairs(data,message);
    }
    auto details=value_from(data, {
      "message","fields.message"
    });
    if(details.is_string()&&chars(str(details))<=4096&&trim(str(details)).starts_with('{')) {
      try {
        auto decoded=parse_json(str(details));
        if(decoded.is_object()&&!data.contains("message_fields"))data["message_fields"]=std::move(decoded);
      }
      catch(const QueryError&) {
      }
    }
    return {
      std::move(source),line,std::move(clean),std::move(data),std::move(format),std::move(error)
    };
  }
  namespace detail {
    struct Message {
      std::size_t first,last;
      std::string text,error;
    };
    class VulkanMessages {
      std::vector<Message>pending;
      std::size_t bytes=0;
      Json module=MISSING,process=MISSING;
      std::string identifier;
      bool body_seen=false;
      Message take(std::string error= {
      }) {
        Strings text;
        for(auto&p:pending) {
          text.push_back(rstrip_newline(p.text));
          if(error.empty()&&!p.error.empty())error=p.error;
        }
        Message r {
          pending.front().first,pending.back().last,join(text,"\n"),error
        };
        pending.clear();
        bytes=0;
        body_seen=false;
        return r;
      }
      public: std::vector<Message>push(std::size_t line,const std::string&text,const std::string&error) {
        std::vector<Message>out;
        auto clean=strip_ansi(rstrip_newline(text));
        auto p=match(RUST,clean);
        if(!p)p=match(FIREFOX,clean);
        if(!p)p=match(NATIVE,clean);
        auto fields=p?p.fields():Json::object();
        auto message=str(get(fields,"message",clean));
        auto header=match(VULKAN_HEADER,message),body=match(VULKAN_BODY,message);
        bool continuation=!pending.empty()&&!header&&((p&&exact(get(fields,"module",get(fields,"logger")),module)&&exact(get(fields,"pid"),process)&&trim(message).starts_with("objects:"))||(!p&&(clean.empty()||std::isspace(static_cast<unsigned char>(clean.front()))||body)&&!trim(clean).starts_with('{')&&!trim(clean).starts_with('[')));
        if(body&&!pending.empty()&&(body_seen||body["id"]!=identifier))continuation=false;
        std::size_t size=header||body||continuation?text.size():0;
        if(continuation&&(pending.size()==limits.message_lines||bytes+size>limits.line_bytes)) {
          out.push_back(take("Vulkan message exceeds byte/line limit; continuation retained separately"));
          continuation=false;
        }
        if(!pending.empty()&&!continuation)out.push_back(take());
        if(header||body||continuation) {
          if(pending.empty()) {
            module=get(fields,"module",get(fields,"logger"));
            process=get(fields,"pid");
            identifier=header?header["id"]:body["id"];
          }
          pending.push_back( {
            line,line,text,error
          });
          body_seen=body_seen||bool(body);
          bytes+=size;
        }
        else if(!trim(text).empty())out.push_back( {
          line,line,text,error
        });
        return out;
      }
      std::optional<Message>finish() {
        if(pending.empty())return {
        };
        return take();
      }
    };
    std::string anchor_key(const Json&e,const std::optional<Big>&t) {
      return encoded(Json::array( {
        e,t?number(*t):Json(nullptr)
      }));
    }
    bool anchor_hint(std::string_view s) {
      for(const auto&e:ANCHOR_EVENTS)if(s.find(e)!=std::string_view::npos)return true;
      return false;
    }
    class TranscriptContext {
      std::string test,context,catch_candidate;
      bool tap_test=false;
      Strings tags;
      Json active_run=nullptr;
      int capture=0;
      std::optional<std::pair<std::string,Big>>last_timestamp;
      std::optional<Strings>catch_header;
      public: void decorate(Record&r,const Json&metadata,const Anchors&anchors) {
        auto e=r.get("@event");
        if(test_event(e)) {
          catch_header.reset();
          catch_candidate.clear();
        }
        else if(r.format=="text"||r.format=="transcript") {
          auto clean=strip_ansi(r.raw),text=trim(clean);
          if(full("-{20,}",text)) {
            if(!catch_header)catch_header=Strings {
            };
            else {
              catch_candidate=join(*catch_header," ");
              catch_header.reset();
            }
          }
          else if(catch_header) {
            if(!clean.empty()&&!std::isspace(static_cast<unsigned char>(clean[0]))&&!text.empty())catch_header->push_back(text);
            std::size_t count=0;
            for(auto&s:*catch_header)count+=chars(s);
            if(catch_header->size()>16||count>2048) {
              catch_header.reset();
              catch_candidate.clear();
            }
          }
          else if(!catch_candidate.empty()) {
            if(full(R"(.+:\d+)",text)) {
              test=catch_candidate;
              tap_test=false;
            }
            catch_candidate.clear();
          }
        }
        if(equals(e,"catch.filters")) {
          tags.clear();
          for(auto m:findall(R"(\[([^]]+)\])",str(r.data["filters"])))tags.push_back(m[1]);
        }
        if(test_event(e)||equals(e,"libtest.output")) {
          test=str(r.data["test"]);
          tap_test=str(e).starts_with("tap.");
        }
        if(in(e,words("tap.summary libtest.summary"))) {
          test.clear();
          tap_test=false;
        }
        if(test_start(e)) {
          active_run=nullptr;
          ++capture;
          last_timestamp.reset();
        }
        auto label=search(CONTEXT_LABEL,r.raw);
        if(label)context=label[1];
        if(r.raw.starts_with("workspace-wayland[")||test_event(e)||equals(e,"process.terminal"))context.clear();
        if(r.raw.starts_with("workspace-wayland:")&&r.raw.find("acceptance deadline armed")!=std::string::npos) {
          active_run=nullptr;
          ++capture;
          last_timestamp.reset();
          context.clear();
        }
        auto anchor=anchors.find(anchor_key(e,r.time_ns));
        if(r.clock=="steady"&&e.is_string()&&anchor!=anchors.end()&&(capture||get(metadata,"family")!=get(anchor->second,"family")))active_run=anchor->second;
        merge(r.metadata,metadata);
        if(capture&&active_run.is_null())r.metadata["run"]=str(get(metadata,"run",r.source))+"#capture-"+std::to_string(capture);
        if(!active_run.is_null()) {
          for(auto n: {
            "run","family"
          })r.metadata[n]=active_run[n];
          r.metadata["run_link"]="shared-event-and-steady-time";
        }
        if(!test.empty())r.metadata["test"]=test;
        if(!tags.empty())r.metadata["tags"]=tags;
        if(!context.empty()&&r.format!="transcript") {
          r.metadata["context_copy"]=context;
          if(r.parse_error.empty()&&match(R"(^[\w_]+":)",r.raw))r.parse_error="truncated diagnostic context fragment";
        }
        if(r.time_ns&&!meta(r,"context_copy"))last_timestamp=std::pair {
          r.clock,*r.time_ns
        };
        if(last_timestamp) {
          r.metadata["near_clock"]=last_timestamp->first;
          r.metadata["near_time_ns"]=number(last_timestamp->second);
        }
      }
      std::vector<Record>parse_line(const std::string&source,std::size_t line,const std::string&raw,const Json&metadata,const Anchors&anchors) {
        Record primary;
        auto s=trim(raw);
        if(tap_test&&raw.starts_with("  ")&&!s.starts_with("ok ")&&!s.starts_with("not ok ")&&!s.starts_with("# "))primary=Record(source,line,rstrip_newline(raw), {
          {
            "event","tap.diagnostic"
          }, {
            "message",rstrip_newline(raw)
          }
        },"transcript");
        else primary=parse_record(source,line,raw);
        decorate(primary,metadata,anchors);
        std::vector<Record>r {
          primary
        };
        if(primary.format=="transcript"&&textget(primary,"@event").starts_with("catch.assertion_")) {
          int part=0;
          for(auto[column,data]:embedded_objects(primary.raw)) {
            Record child(source,line,primary.raw,std::move(data),"json", {
            },primary.metadata);
            child.metadata["part"]=++part;
            child.metadata["column"]=chars(std::string_view(primary.raw).substr(0,column))+1;
            child.metadata["context_copy"]="Catch INFO";
            decorate(child,metadata,anchors);
            r.push_back(std::move(child));
          }
        }
        return r;
      }
    };
    class InputFile {
      int fd_=-1;
      public:explicit InputFile(const fs::path&p) {
        fd_=::open(p.c_str(),O_RDONLY|O_CLOEXEC);
        if(fd_<0)throw QueryError("cannot read "+p.string()+": "+std::strerror(errno));
      }
      ~InputFile() {
        if(fd_>=0)::close(fd_);
      }
      InputFile(const InputFile&)=delete;
      int fd()const {
        return fd_;
      }
      std::string read(std::uint64_t offset,std::size_t count) {
        std::string result(count,'\0');
        std::size_t done=0;
        while(done<count) {
          auto n=::pread(fd_,result.data()+done,count-done,static_cast<off_t>(offset+done));
          if(n<0&&errno==EINTR)continue;
          if(n<0)throw QueryError(std::string("log read failed: ")+std::strerror(errno));
          if(n==0)break;
          done+=static_cast<std::size_t>(n);
        }
        result.resize(done);
        return result;
      }
    };
    void check_capture(const LogFile&f,int fd,bool unchanged=false) {
      struct stat info {
      };
      if(::fstat(fd,&info)!=0)throw QueryError(std::string("cannot inspect log: ")+std::strerror(errno));
      if(static_cast<std::uint64_t>(info.st_dev)!=f.device||static_cast<std::uint64_t>(info.st_ino)!=f.inode||info.st_size<0||static_cast<std::uint64_t>(info.st_size)<f.size)throw QueryError("log was replaced or truncated during query: "+f.source);
      Big modified=Big(info.st_mtim.tv_sec)*1000000000+info.st_mtim.tv_nsec;
      if(unchanged&&(static_cast<std::uint64_t>(info.st_size)!=f.size||modified!=f.modified_ns))throw QueryError("log changed during correlation query; retry a completed capture: "+f.source);
    }
    bool within(const fs::path&p,const fs::path&root) {
      auto r=p.lexically_relative(root);
      return !r.empty()&&*r.begin()!="..";
    }
    std::string utf8(const std::string&raw) {
      auto u=icu::UnicodeString::fromUTF8(icu::StringPiece(raw.data(),static_cast<int32_t>(raw.size())));
      std::string s;
      return u.toUTF8String(s);
    }
  }
  LogFile LogFile::capture(const fs::path&p,const fs::path&root) {
    if (observing("file.capture")) observe("file.capture", p.string());
    auto resolved=fs::canonical(p);
    if(!within(resolved,root))throw QueryError("log input escapes repository mount: "+p.string());
    struct stat info {
    };
    if(::stat(resolved.c_str(),&info)!=0)throw QueryError("cannot inspect log file: "+p.string());
    if(!S_ISREG(info.st_mode))throw QueryError("not a regular log file: "+p.string());
    LogFile f;
    f.path=resolved;
    f.source=resolved.lexically_relative(root).string();
    f.device=info.st_dev;
    f.inode=info.st_ino;
    f.size=info.st_size;
    f.modified_ns=Big(info.st_mtim.tv_sec)*1000000000+info.st_mtim.tv_nsec;
    return f;
  }
  LogFile LogFile::recent(std::size_t count)const {
    std::deque<std::uint64_t>starts {
      0
    };
    std::size_t lines=1;
    std::uint64_t offset=0;
    InputFile stream(path);
    check_capture(*this,stream.fd());
    while(offset<size) {
      auto b=stream.read(offset,static_cast<std::size_t>(std::min<std::uint64_t>(1024*1024,size-offset)));
      if(b.empty())throw QueryError("log was truncated during query: "+source);
      for(std::size_t p=b.find('\n');p!=std::string::npos;p=b.find('\n',p+1)) {
        auto next=offset+p+1;
        if(next<size) {
          starts.push_back(next);
          if(starts.size()>count)starts.pop_front();
          ++lines;
        }
      }
      offset+=b.size();
    }
    auto r=*this;
    r.start_offset=starts.front();
    r.first_line=lines-starts.size()+1;
    return r;
  }
  void LogFile::records(const std::function<void(Record)>&visitor,bool unchanged,const Anchors&anchors,const std::function<bool(std::string_view)>&line_hint)const {
    if (observing("file.records")) observe("file.records", path.string());
    InputFile stream(path);
    check_capture(*this,stream.fd(),unchanged);
    Json m= {
      {
        "mtime_ns",number(modified_ns)
      }, {
        "run",source
      }, {
        "artifact",fs::path(source).stem().string()
      }
    };
    merge(m,metadata);
    TranscriptContext context;
    VulkanMessages messages;
    auto emit=[&](const Message&message) {
      if(line_hint&&!line_hint(message.text)&&!anchor_hint(message.text)&&!match(TEST_STATUS,message.text)&&!match(TAP_STATUS,message.text)&&!match(LIBTEST_STATUS,message.text)) {
        auto t=trim(message.text);
        if(!t.starts_with("Filters:")&&!t.starts_with("workspace-wayland:")&&!t.starts_with("# Subtest:")&&!t.starts_with("---- "))return;
      }
      for(auto row:context.parse_line(source,message.first,message.text,m,anchors)) {
        if(!message.error.empty())row.parse_error=message.error;
        if(message.last!=message.first)row.metadata["line_end"]=message.last;
        visitor(std::move(row));
      }
    };
    if(path.extension()==".json"&&start_offset==0) {
      auto raw=stream.read(0,static_cast<std::size_t>(std::min<std::uint64_t>(size,limits.line_bytes)));
      if(raw.size()!=std::min<std::uint64_t>(size,limits.line_bytes))throw QueryError("log was truncated during query: "+source);
      auto text=utf8(raw);
      auto row=parse_record(source,1,text,size>limits.line_bytes);
      merge(row.metadata,m);
      row.metadata["line_end"]=std::max<std::size_t>(1,std::count(raw.begin(),raw.end(),'\n')+(!raw.ends_with('\n')));
      if(text!=raw&&row.parse_error.empty())row.parse_error="invalid UTF-8 bytes replaced";
      visitor(std::move(row));
      if(unchanged)check_capture(*this,stream.fd(),true);
      return;
    }
    std::uint64_t offset=start_offset;
    std::size_t line=first_line-1;
    std::string retained;
    retained.reserve(std::min<std::uint64_t>(size-start_offset,limits.line_bytes));
    std::size_t physical_bytes=0;
    auto flush=[&]() {
      ++line;
      bool truncated=physical_bytes>limits.line_bytes;
      std::string error=truncated?"line exceeds byte limit; text is truncated":"";
      auto text=utf8(retained);
      if(text!=retained&&error.empty())error="invalid UTF-8 bytes replaced";
      for(auto&msg:messages.push(line,text,error))emit(msg);
      retained.clear();
      physical_bytes=0;
    };
    while(offset<size) {
      auto block=stream.read(offset,static_cast<std::size_t>(std::min<std::uint64_t>(limits.line_bytes,size-offset)));
      if(block.empty())throw QueryError("log was truncated during query: "+source);
      offset+=block.size();
      std::size_t p=0;
      while(p<block.size()) {
        auto nl=block.find('\n',p);
        auto end=nl==std::string::npos?block.size():nl+1;
        auto amount=end-p;
        physical_bytes+=amount;
        auto copy=std::min(amount,limits.line_bytes-retained.size());
        retained.append(block,p,copy);
        p=end;
        if(nl!=std::string::npos)flush();
      }
    }
    if(physical_bytes)flush();
    if(auto last=messages.finish())emit(*last);
    if(unchanged)check_capture(*this,stream.fd(),true);
  }
  namespace detail {
    bool is_log_path(const fs::path&p,const Strings&suffixes= {
      ".jsonl",".log",".out",".txt"
    }) {
      return mmltk::tools::includes(suffixes,p.extension().string())||full(MOZILLA_ARTIFACT,p.filename().string());
    }
    void directory_logs(const fs::path&dir,const std::function<void(const fs::path&)>&visit,const Strings&suffixes= {
      ".jsonl",".log",".out",".txt"
    }) {
      if (observing("directory.logs")) observe("directory.logs", dir.string());
      DirectoryObservation observation(dir);
      for(const auto&e:fs::directory_iterator(dir)) {
        if (observing("directory.entry")) observe("directory.entry", e.path().string());
        if(is_log_path(e.path(),suffixes)&&e.is_regular_file())visit(e.path());
      }
    }
    std::string normalize_input(std::string s,const fs::path&root,const std::string&host= {
    }) {
      if(!host.empty()&&(s==host||s.starts_with(host+"/")))s=root.string()+s.substr(host.size());
      return fs::path(s).is_absolute()?s:(root/s).string();
    }
    std::vector<fs::path> glob_paths(const std::string&pattern,bool recursive) {
      if(recursive) {
        auto star=pattern.find("**");
        if(star!=std::string::npos) {
          auto sep=pattern.rfind('/',star);
          auto base=sep==std::string::npos?std::string("."):pattern.substr(0,sep);
          auto rest=pattern.substr(star+2);
          if(rest.starts_with('/'))rest.erase(0,1);
          std::set<fs::path> out;
          for(auto&p:glob_paths(base,false)) {
            if(!fs::is_directory(p))continue;
            auto add=[&](const fs::path&d) {
              if(rest.empty())out.insert(d);
              else for(auto&m:glob_paths((d/rest).string(),true))out.insert(m);
            };
            add(p);
            for(const auto&e:fs::recursive_directory_iterator(p))if(e.is_directory())add(e.path());
          }
          return {
            out.begin(),out.end()
          };
        }
      }
      glob_t results {
      };
      if (observing("glob.expand")) observe("glob.expand", pattern);
      int code=::glob(pattern.c_str(),GLOB_NOSORT,nullptr,&results);
      std::unique_ptr<glob_t,decltype(&globfree)>guard(&results,globfree);
      if(code!=0&&code!=GLOB_NOMATCH)throw QueryError("could not expand input glob: "+pattern);
      std::vector<fs::path>out;
      for(std::size_t i=0;i<results.gl_pathc;++i)out.emplace_back(results.gl_pathv[i]);
      std::sort(out.begin(),out.end());
      return out;
    }
    using Captured=std::map<fs::path,LogFile>;
    std::vector<LogFile> sorted_log_files(const Captured& selected) {
      std::vector<LogFile> out;
      out.reserve(selected.size());
      for(const auto& [path,file]:selected)out.push_back(file);
      std::sort(out.begin(),out.end(),[](const auto& a,const auto& b) { return a.source<b.source; });
      return out;
    }
    std::vector<LogFile> discover_files(const Strings&inputs,const fs::path&root,bool recursive,const std::string&host,Captured&captured,std::set<fs::path>&histories,std::map<fs::path,bool>&directories) {
      Captured selected;
      std::set<std::string>patterns;
      std::set<fs::path>seen;
      for(const auto&supplied:inputs.empty()?Strings {
        "build/validation"
      }
      :inputs) {
        auto pattern=normalize_input(supplied,root,host);
        if(!patterns.insert(pattern).second)continue;
        auto paths=glob_paths(pattern,recursive);
        if(paths.empty())throw QueryError("input path or glob matched no files: "+supplied);
        bool found=false;
        for(auto&p:paths) {
          auto resolved=fs::weakly_canonical(p);
          if(!within(resolved,root))throw QueryError("log input escapes repository mount: "+p.string());
          bool directory=fs::is_directory(p),directory_found=false;
          if(directory&&directories.contains(resolved)) {
            found=found||directories.at(resolved);
            continue;
          }
          auto add=[&](const fs::path&candidate) {
            auto resolved_candidate=fs::canonical(candidate);
            if(seen.insert(resolved_candidate).second) {
              auto i=captured.find(resolved_candidate);
              if(i==captured.end()) {
                auto f=LogFile::capture(candidate,root);
                i=captured.emplace(f.path,std::move(f)).first;
              }
              selected[i->first]=i->second;
            }
            found=true;
            directory_found=true;
          };
          if(directory) {
            if(recursive) {
              for(const auto&e:fs::recursive_directory_iterator(p))if(is_log_path(e.path())&&e.is_regular_file())add(e.path());
            }
            else directory_logs(p,add);
            directories[resolved]=directory_found;
            if(resolved.filename().string().ends_with(".history"))histories.insert(resolved);
          }
          else add(p);
        }
        if(!found)throw QueryError("input contains no .jsonl, .log, .out, .txt, or Mozilla process logs: "+supplied);
      }
      return sorted_log_files(selected);
    }
    struct Family {
      fs::path family,original;
      bool archived;
    };
    Family artifact_family(const fs::path&p) {
      bool archived=p.parent_path().filename().string().ends_with(".history");
      auto name=p.parent_path().filename().string();
      auto original=archived?p.parent_path().parent_path()/name.substr(0,name.size()-8):p;
      auto m=match(MOZILLA_ARTIFACT,original.filename().string());
      if(!m)m=match(APPLICATION_ARTIFACT,original.filename().string());
      auto stem=m?m[1]:replace_rx("-(?:native|firefox|acceptance|application)$",original.stem().string(),"");
      return {
        original.parent_path()/stem,original,archived
      };
    }
    fs::path normalize_family(std::string family,const fs::path&root,const std::string&host) {
      fs::path p=family;
      if(!p.is_absolute()&&p.parent_path().empty())p=fs::path("build/validation")/p;
      p=normalize_input(p.string(),root,host);
      if(is_log_path(p))p=artifact_family(p).family;
      return fs::weakly_canonical(p.parent_path())/p.filename();
    }
    std::optional<std::pair<std::string,Big>> rotation_identity(const std::string&s) {
      auto m=match(R"(^(\d+)-(\d+)$)",s);
      if(!m)return {
      };
      return std::pair {
        m[1],decimal_integer(m[2])
      };
    }
    // Capture discovery and cross-file run inference never read beyond captured sizes.
    class ArtifactCatalog {
      fs::path root;
      std::map<fs::path,std::map<std::string,std::set<std::string>>>inventory;
      std::size_t candidate_count=0;
      void discover_families(const std::set<fs::path>&families) {
        std::set<fs::path>parents;
        for(auto&p:families) {
          if(!within(p,root))throw QueryError("log input escapes repository mount: "+p.string());
          parents.insert(p.parent_path());
        }
        for(auto&p:parents) {
          if(inventory.contains(p))continue;
          auto&candidates=inventory[p];
          if(!fs::is_directory(p))continue;
          DirectoryObservation observation(p);
          for(const auto&e:fs::directory_iterator(p)) {
            if (observing("directory.entry")) observe("directory.entry", e.path().string());
            auto original=e.path().filename().string();
            if(original.ends_with(".history"))original.resize(original.size()-8);
            auto m=match(MOZILLA_ARTIFACT,original);
            if(!m)m=match(APPLICATION_ARTIFACT,original);
            if(!m||original==m[1]+"-application.log")continue;
            auto&names=candidates[m[1]];
            if(!names.contains(original)) {
              if(candidate_count>=limits.distinct_keys)throw QueryError("artifact candidate inventory exceeds file budget; narrow the input paths");
              names.insert(original);
              ++candidate_count;
              if (observing("catalog.candidate")) observe("catalog.candidate", candidate_count);
            }
          }
        }
      }
      std::vector<fs::path> family_paths(const fs::path&family,bool existing=true) {
        discover_families( {
          family
        });
        auto&names=inventory[family.parent_path()][family.filename().string()];
        if(!names.empty()&&names.size()+FAMILY_SUFFIXES.size()>limits.distinct_keys)throw QueryError("artifact family exceeds file budget; narrow the input paths");
        std::set<fs::path>paths;
        for(auto&n:names)paths.insert(family.parent_path()/n);
        for(auto&s:FAMILY_SUFFIXES)paths.insert(family.string()+s);
        std::vector<fs::path>out;
        for(auto&p:paths)if(!existing||fs::is_regular_file(p))out.push_back(p);
        return out;
      }
      void discover_siblings(const std::set<fs::path>&histories) {
        struct Request {
          bool current=false;
          std::map<std::string,std::set<Big>>rotations;
          std::set<std::string>names;
        };
        std::map<fs::path,Request>requests;
        Captured selected;
        for(auto&f:files)selected[f.path]=f;
        auto original_count=selected.size();
        auto add=[&](const fs::path&p,const std::string&why) {
          auto resolved=fs::canonical(p);
          if(selected.contains(resolved))return;
          if(selected.size()-original_count>=limits.distinct_keys)throw QueryError("automatic artifact discovery exceeds file budget; narrow the input paths");
          auto f=LogFile::capture(p,root);
          f.metadata["discovery"]=why;
          selected[resolved]=std::move(f);
        };
        for(auto&f:files) {
          auto family=artifact_family(f.path);
          if(!is_log_path(family.original))continue;
          auto&r=requests[family.family];
          if(!family.archived)r.current=true;
          else if(auto id=rotation_identity(f.path.stem().string()))r.rotations[id->first].insert(id->second);
          else r.names.insert(f.path.stem().string());
        }
        for(auto&[family,request]:requests)for(auto&original:family_paths(family,false)) {
          if(request.current&&fs::is_regular_file(original))add(original,"current artifact sibling");
          fs::path directory=original.string()+".history";
          if((!request.rotations.empty()||!request.names.empty())&&fs::is_directory(directory)) {
            auto resolved=fs::canonical(directory);
            if(!within(resolved,root))throw QueryError("log input escapes repository mount: "+directory.string());
            if(histories.contains(resolved))continue;
            directory_logs(directory,[&](const fs::path&p) {
              auto stem=p.stem().string();
              if(request.names.contains(stem)) {
                add(p,"matching archive name");
                return;
              }
              auto id=rotation_identity(stem);
              if(!id)return;
              auto it=request.rotations.find(id->first);
              if(it==request.rotations.end())return;
              const auto&times=it->second;
              auto near=times.lower_bound(id->second);
              if((near!=times.end()&&absbig(*near-id->second)<=ROTATION_WINDOW_NS)||(near!=times.begin()&&absbig(*std::prev(near)-id->second)<=ROTATION_WINDOW_NS))add(p,"adjacent rotation (inferred)");
            }, {
              original.extension()==".jsonl"?".jsonl":".log"
            });
          }
        }
        files.clear();
        for(auto&[p,f]:selected)files.push_back(std::move(f));
      }
      void assign_runs() {
        std::map<std::pair<std::string,std::string>,std::vector<std::pair<Big,std::size_t>>>groups;
        for(std::size_t i=0;i<files.size();++i) {
          auto&f=files[i];
          auto family=artifact_family(f.path);
          auto name=family.family.lexically_relative(root).string(),identity=family.archived?f.path.stem().string():"current",original=family.original.filename().string();
          merge(f.metadata, {
            {
              "family",name
            }, {
              "run",name+"@"+identity
            }, {
              "artifact",original
            }, {
              "run_link","artifact-stem"
            }
          });
          f.metadata["role"]=full(MOZILLA_ARTIFACT,original)?"mozilla":original.ends_with("-acceptance.jsonl")?"acceptance":full(APPLICATION_ARTIFACT,original)?"application":family.original.extension()==".jsonl"?"trace":family.original.stem().string().ends_with("-native")?"native":family.original.stem().string().ends_with("-firefox")?"firefox":"transcript";
          if(family.archived) {
            f.metadata["archive_id"]=identity;
            if(auto r=rotation_identity(identity)) {
              f.metadata["rotation_ns"]=number(r->second);
              groups[ {
                name,r->first
              }].emplace_back(r->second,i);
            }
          }
        }
        auto assign=[&](const auto&batch) {
          if(batch.empty())return;
          auto run=files[batch.front().second].metadata["run"];
          for(const auto&[time,i]:batch) {
            files[i].metadata["run"]=run;
            files[i].metadata["run_link"]=batch.size()>1?"rotation-neighbor-within-10ms":"unpaired-history";
          }
        };
        for(auto&[key,entries]:groups) {
          std::sort(entries.begin(),entries.end());
          std::vector<std::pair<Big,std::size_t>>batch;
          std::set<std::string>artifacts;
          for(auto&entry:entries) {
            auto artifact=str(files[entry.second].metadata["artifact"]);
            if(!batch.empty()&&(entry.first-batch.front().first>ROTATION_WINDOW_NS||artifacts.contains(artifact))) {
              assign(batch);
              batch.clear();
              artifacts.clear();
            }
            batch.push_back(entry);
            artifacts.insert(artifact);
          }
          assign(batch);
        }
      }
      public: std::vector<LogFile>files;
      Anchors anchors;
      std::set<std::string>selected_runs;
      ArtifactCatalog(const Options&o,fs::path r):root(std::move(r)) {
        auto host=tools::env("MMLTK_LOG_HOST_ROOT");
        Captured captured;
        std::set<fs::path>histories;
        std::map<fs::path,bool>directories;
        if(!o.paths.empty()||o.family.empty())files=discover_files(o.paths,root,o.recursive,host,captured,histories,directories);
        std::set<fs::path>requested,families;
        for(auto&s:o.family)requested.insert(normalize_family(s,root,host));
        families=requested;
        if(o.history||!o.run.empty()||o.triage)for(auto&f:files)families.insert(artifact_family(f.path).family);
        discover_families(families);
        Strings inputs;
        for(auto&family:requested) {
          auto siblings=family_paths(family);
          if(siblings.empty()&&(o.history||!o.run.empty()))for(auto&p:family_paths(family,false)) {
            fs::path h=p.string()+".history";
            if(fs::is_directory(h))siblings.push_back(h);
          }
          if(siblings.empty())throw QueryError("no artifact family found: "+family.string());
          for(auto&p:siblings)inputs.push_back(p.string());
        }
        auto captured_files=[&]() {
          files=sorted_log_files(captured);
        };
        if(!inputs.empty()) {
          discover_files(inputs,root,o.recursive,host,captured,histories,directories);
          captured_files();
        }
        if(o.history||!o.run.empty()) {
          std::set<fs::path>archives,done;
          for(auto&f:files) {
            auto family=artifact_family(f.path);
            std::vector<fs::path>siblings;
            if(family.archived||!o.family.empty()) {
              if(!done.insert(family.family).second)continue;
              siblings=family_paths(family.family,false);
            }
            else siblings.push_back(family.original);
            for(auto&p:siblings) {
              fs::path h=p.string()+".history";
              if(fs::is_directory(h)&&!histories.contains(fs::canonical(h)))archives.insert(h);
            }
          }
          if(!archives.empty()) {
            Strings in;
            for(auto&p:archives)in.push_back(p.string());
            discover_files(in,root,false, {
            },captured,histories,directories);
            captured_files();
          }
        }
        std::set<fs::path>original_paths;
        for(auto&f:files)original_paths.insert(f.path);
        if(o.triage)discover_siblings(histories);
        assign_runs();
        std::set<std::string>original_runs;
        for(auto&f:files)if(original_paths.contains(f.path))original_runs.insert(str(f.metadata["run"]));
        std::erase_if(files,[&](const LogFile&f) {
          return !original_paths.contains(f.path)&&!original_runs.contains(str(f.metadata["run"]));
        });
        if(!o.run.empty()) {
          std::set<std::string>selected_families;
          for(auto&f:files) {
            auto run=str(f.metadata["run"]);
            auto at=run.rfind('@');
            if(o.run==run||equals(get(f.metadata,"archive_id"),o.run)||o.run==run.substr(at==std::string::npos?0:at+1)) {
              selected_runs.insert(run);
              selected_families.insert(str(f.metadata["family"]));
            }
          }
          if(selected_runs.empty())throw QueryError("no artifact run matched '"+o.run+"'; use --list-runs --history");
          std::erase_if(files,[&](const auto&f) {
            return !selected_runs.contains(str(f.metadata["run"]))&&selected_families.contains(str(f.metadata["family"]));
          });
        }
      }
      void prepare_anchors() {
        for(auto&f:files) {
          if(f.path.extension()!=".jsonl")continue;
          f.records([&](Record r) {
            auto event=r.get("@event");
            if(r.clock!="steady"||!in(event,ANCHOR_EVENTS))return;
            auto key=anchor_key(event,r.time_ns);
            auto it=anchors.find(key);
            if(it!=anchors.end()&&it->second["run"]!=f.metadata["run"])throw QueryError("ambiguous shared event timestamp across captures; narrow --family/--run");
            anchors[key]=f.metadata;
            if(anchors.size()>limits.distinct_keys)throw QueryError("too many capture anchors; narrow --family/--run");
          },true, {
          },anchor_hint);
        }
        std::set<std::string>native;
        for(auto&f:files)if(f.path.extension()==".jsonl")native.insert(str(f.metadata["family"]));
        std::map<std::string,std::set<std::string>>tests,tags,runs;
        for(auto&f:files) {
          if(native.contains(str(f.metadata["family"]))&&!equals(f.metadata["role"],"transcript"))continue;
          f.records([&](Record r) {
            if(!equals(get(r.metadata,"run_link"),"shared-event-and-steady-time"))return;
            auto run=str(r.metadata["run"]);
            runs[f.source].insert(run);
            auto test=get(r.metadata,"test");
            if(present(test)) {
              tests[run].insert(str(test));
              auto t=get(r.metadata,"tags",Json::array());
              for(auto&v:t)tags[run].insert(str(v));
            }
          },true,anchors,anchor_hint);
        }
        for(auto&f:files) {
          auto&linked=runs[f.source];
          f.linked_runs.assign(linked.begin(),linked.end());
          auto run=str(f.metadata["run"]);
          if(tests[run].size()==1)f.metadata["test"]=*tests[run].begin();
          if(tags.contains(run))f.metadata["tags"]=Strings(tags[run].begin(),tags[run].end());
        }
      }
      void list_runs(std::ostream&out,int limit)const {
        std::map<std::string,std::vector<const LogFile*>>groups;
        for(auto&f:files)groups[str(f.metadata["run"])].push_back(&f);
        std::vector<std::pair<std::pair<Big,std::string>,std::vector<const LogFile*>>>ordered;
        for(auto&[run,fs]:groups) {
          Big recent=0;
          bool first=true;
          for(auto*f:fs) {
            if(first||f->modified_ns>recent)recent=f->modified_ns;
            first=false;
          }
          ordered.push_back( {
            {
              recent,run
            },fs
          });
        }
        std::sort(ordered.begin(),ordered.end(),[](const auto&a,const auto&b) {
          return a.first<b.first;
        });
        if(ordered.size()>static_cast<std::size_t>(limit))ordered.erase(ordered.begin(),ordered.end()-limit);
        for(auto&[key,fs]:ordered) {
          out<<key.second<<" ["<<str(fs.front()->metadata["run_link"])<<"]\n";
          std::sort(fs.begin(),fs.end(),[](auto*a,auto*b) {
            return a->source<b->source;
          });
          for(auto*f:fs)out<<"  "<<f->source<<" bytes="<<f->size<<" mtime_ns="<<f->modified_ns<<'\n';
        }
        out<<"Showing "<<std::min<std::size_t>(groups.size(),limit)<<'/'<<groups.size()<<" artifact runs; rotation-neighbor links are inferred.\n";
      }
    };
    std::pair<std::string, std::optional<Big>> record_timestamp(const Record& row) {
      auto clock = row.clock;
      auto time = row.time_ns;
      if (!time) {
        clock = str(get(row.metadata, "near_clock"));
        const auto near = get(row.metadata, "near_time_ns");
        if (integer(near)) time = bigint(near);
      }
      return {clock, time};
    }
    class ErrorLookup {
      const Options&options;
      std::map<std::pair<std::string,std::string>,Big>ends;
      std::optional<Big>focus_estimate;
      public: Strings phrases,words_;
      std::optional<Record>focus;
      std::size_t matches=0,malformed=0;
      std::string mode;
      Expression expression;
      Strings artifacts;
      ErrorLookup(const std::string&text,const Options&o):options(o) {
        if(trim(text).empty())throw QueryError("--error requires non-empty display text");
        phrases.push_back(normalized_text(text));
        auto normalized=normalized_text(replace_rx(R"(\n\s*\n)",trim(text),": "));
        mmltk::tools::append_unique(phrases, normalized);
        for(auto m:findall(R"(\w+)",normalized_text(text)))mmltk::tools::append_unique(words_, m[0]);
      }
      bool hint(std::string_view raw)const {
        auto s=lower(raw);
        return s.find("\\u")!=std::string::npos||std::all_of(words_.begin(),words_.end(),[&](auto&w) {
          return s.find(w)!=std::string::npos;
        });
      }
      static Json physical_key(const Record&r) {
        return Json::array( {
          equals(get(r.metadata,"role"),"transcript"),meta(r,"context_copy"),number(mtime(r)),r.source,r.line,part(r)
        });
      }
      void locate(const std::vector<LogFile>&files,const Expression&q,const Expression&w,const Anchors&a) {
        std::optional<Record>word_focus;
        std::size_t word_matches=0;
        Json phrase_key=MISSING,word_key=MISSING;
        for(auto&f:files)f.records([&](Record r) {
          malformed+=!r.parse_error.empty();
          if(!w.matches(r)||!q.matches(r))return;
          auto t=record_text(r);
          if(std::any_of(phrases.begin(),phrases.end(),[&](auto&s) {
            return t.find(s)!=std::string::npos;
          })) {
            ++matches;
            auto key=physical_key(r);
            if(missing(phrase_key)||compare(key,phrase_key)<0) {
              focus=r;
              phrase_key=key;
            }
          }
          else if(!focus&&!words_.empty()&&std::all_of(words_.begin(),words_.end(),[&](auto&s) {
            return t.find(s)!=std::string::npos;
          })) {
            ++word_matches;
            auto key=physical_key(r);
            if(missing(word_key)||compare(key,word_key)<0) {
              word_focus=r;
              word_key=key;
            }
          }
        },true,a,options.strict?std::function<bool(std::string_view)> {
        }
        :[this](std::string_view raw) {
          return hint(raw);
        });
        Expression e;
        if(focus) {
          mode="exact/display-normalized phrase";
          e= {
            "phrase",phrases, {
            }
          };
        }
        else if(word_focus) {
          mode="all-words fallback";
          focus=word_focus;
          matches=word_matches;
          e= {
            "words",words_, {
            }
          };
        }
        else return;
        expression= {
          "and",Json::array(), {
            q,e
          }
        };
      }
      bool relevant(const LogFile&f)const {
        return f.metadata["run"]==focus->metadata["run"]||f.source==focus->source||mmltk::tools::includes(f.linked_runs,str(focus->metadata["run"]));
      }
      std::optional<Big>estimate(const Record&r,const std::string&clock,const std::optional<Big>&time)const {
        auto i=ends.find( {
          r.source,clock
        });
        if(i==ends.end()||!time)return {
        };
        return mtime(r)-(i->second-*time);
      }
      void calibrate(const std::vector<LogFile>&files,const Anchors&a) {
        for(auto&f:files) {
          if(f.metadata["run"]==focus->metadata["run"])artifacts.push_back(f.source);
          if(equals(f.metadata["role"],"transcript")&&f.source!=focus->source)continue;
          f.records([&](Record r) {
            if(r.metadata["run"]==focus->metadata["run"]&&r.time_ns&&!meta(r,"context_copy")) {
              auto key=std::pair {
                r.source,r.clock
              };
              auto it=ends.find(key);
              if(it==ends.end()||*r.time_ns>it->second)ends[key]=*r.time_ns;
            }
          },true,a);
        }
        focus_estimate=estimate(*focus,focus->clock,focus->time_ns);
      }
      bool annotate(Record&r,bool force=false)const {
        if(r.metadata["run"]!=focus->metadata["run"])return false;
        auto [clock, timestamp] = record_timestamp(r);
        const bool carried = !r.time_ns;
        Big delta;
        std::string link;
        if(clock==focus->clock&&timestamp&&focus->time_ns) {
          delta=*timestamp-*focus->time_ns;
          link=carried?"preceding-same-source-timestamp proximity":"same-clock proximity";
        }
        else {
          auto estimated=estimate(r,clock,timestamp);
          if(estimated&&focus_estimate) {
            delta=*estimated-*focus_estimate;
            link="file-mtime proximity";
          }
          else if(r.source==focus->source&&absbig(Big(r.line)-focus->line)<=options.context) {
            delta=0;
            link="line-neighborhood proximity";
          }
          else if(force) {
            delta=0;
            link="matching-message link; clock unaligned";
          }
          else return false;
        }
        if(!force&&absbig(delta)>Big(options.near_ms)*1000000)return false;
        r.metadata["proximity_ns"]=number(delta);
        r.metadata["time_link"]=link;
        return true;
      }
    };
  }
  Strings field_names(std::string_view value,char separator) {
    auto names=tools::split(value,separator,true);
    for(auto&n:names) {
      n=trim(n);
      if(!full(FIELD_NAME,n))throw QueryError("invalid field list: '"+std::string(value)+"'");
    }
    if(names.empty())throw QueryError("invalid field list: '"+std::string(value)+"'");
    return names;
  }
  std::optional<Strings>correlation_key(const Record&r,const Strings&names) {
    Strings parts;
    for(auto&n:names) {
      auto v=r.get(n);
      if(missing(v)||v.is_null()||v.is_boolean()||v.is_object()||v.is_array()||exact(v,0)||equals(v,"")||(v.is_string()&&full("0+",str(v))))return {
      };
      parts.push_back(encoded(v));
    }
    return parts;
  }
  Strings group_key(const Record& row, const Strings& names) {
    Strings group;
    for (const auto& name : names) {
      const auto value = row.get(name);
      group.push_back(encoded(missing(value) ? Json(nullptr) : value));
    }
    return group;
  }
  Json physical_position(const Record&r) {
    return Json::array( {
      r.source,r.line,part(r)
    });
  }
  bool physical_position_less(const Record& left, const Record& right) {
    return compare(physical_position(left), physical_position(right)) < 0;
  }
  Json order_key(const Record&r,std::string_view order) {
    auto position=physical_position(r);
    if(order=="file")return position;
    if(order=="capture")return concatenate(Json::array( {
      number(mtime(r))
    }),position);
    if(order=="proximity")return concatenate(Json::array( {
      get(r.metadata,"proximity_ns",number(Big(1)<<63))
    }),position);
    int rank=r.clock=="steady"?0:r.clock=="wall"?1:r.clock=="wall-local"?2:r.clock=="none"?4:3;
    return concatenate(Json::array( {
      rank,r.clock,number(ns(r))
    }),position);
  }
  Record triage_snapshot(const Record&r) {
    int budget=96;
    bool shortened=false,training=r.training_record();
    std::function<Json(const Json&,int,std::string_view)>trim_value=[&](const Json&v,int depth,std::string_view path)->Json {
      if(v.is_string()) {
        auto s=str(v);
        if(chars(s)>MAX_TRIAGE_TEXT) {
          shortened=true;
          return prefix(s,MAX_TRIAGE_TEXT)+"…";
        }
        return v;
      }
      if(v.is_array()||v.is_object()) {
        if(depth>=4||budget<=0) {
          shortened=true;
          return "<triage payload omitted>";
        }
        Json result=v.is_object()?Json::object():Json::array();
        std::vector<std::pair<std::string,const Json*>>items;
        if(v.is_object()) {
          Strings priority;
          if(training&&path.empty())priority=words("format_version run_id attempt_id role sequence progress");
          else if(training&&path=="progress")priority=words("phase scope session_id model_id failure");
          for(auto&n:priority)if(v.contains(n))items.emplace_back(n,&v[n]);
          std::vector<std::pair<std::string,const Json*>>rest;
          int inspected=0;
          for(auto i=v.begin();i!=v.end()&&inspected<=budget;++i,++inspected)if(!mmltk::tools::includes(priority,i.key()))rest.emplace_back(i.key(),&i.value());
          std::stable_sort(rest.begin(),rest.end(),[](const auto&a,const auto&b) {
            return (a.second->is_object()||a.second->is_array())<(b.second->is_object()||b.second->is_array());
          });
          items.insert(items.end(),rest.begin(),rest.end());
        }
        else {
          for(std::size_t i=0;i<v.size()&&i<=static_cast<std::size_t>(budget);++i)items.emplace_back(std::to_string(i),&v[i]);
        }
        for(auto&[name,child]:items) {
          if(budget<=0) {
            shortened=true;
            break;
          }
          --budget;
          auto value=trim_value(*child,depth+1,path.empty()?name:std::string(path)+"."+name);
          if(v.is_object()) {
            std::string key=name;
            if(chars(key)>MAX_TRIAGE_TEXT) {
              key=prefix(key,MAX_TRIAGE_TEXT)+"…";
              shortened=true;
            }
            result[key]=std::move(value);
          }
          else result.push_back(std::move(value));
        }
        return result;
      }
      return v;
    };
    auto out=r;
    out.raw=str(trim_value(r.raw,0,"raw"));
    out.data=trim_value(r.data,0,"");
    if(shortened)out.metadata["triage_payload_truncated"]=true;
    return out;
  }
  std::string Identity::label()const {
    Strings decoded;
    for(auto&v:values)decoded.push_back(compact(parse_json(v),80));
    return compact(join(names,"+"),128)+"="+join(decoded,"+");
  }
  std::vector<Identity>strong_identities(const Record&r,const std::vector<Strings>&explicit_fields) {
    std::vector<Identity>out;
    auto run=str(get(r.metadata,"run",r.source));
    auto add=[&](Strings names,std::optional<Strings>values= {
    }) {
      auto key=values?values:correlation_key(r,names);
      if(!key)return;
      for(auto&p:*key) {
        auto v=parse_json(p);
        if(v.is_string()&&trim(str(v)).empty())return;
      }
      Identity id {
        run,std::move(names),*key
      };
      mmltk::tools::append_unique(out, std::move(id));
    };
    add( {
      "@surface"
    });
    add( {
      "@workspace_source"
    });
    add( {
      "@workspace_source","transfer_sequence"
    });
    for(auto&names:explicit_fields)add(names);
    add( {
      "source_session","source_instance"
    });
    if(r.training_record()) {
      add( {
        "run_id","attempt_id"
      });
      add( {
        "progress.session_id","progress.model_id"
      });
      add( {
        "progress.session_id","progress.failure.first_cause"
      });
    }
    Json values=r.data;
    auto fields=get(r.data,"fields");
    if(fields.is_object())merge(values,fields);
    for(auto it=values.begin();it!=values.end();++it) {
      if(out.size()>=16)break;
      auto name=it.key();
      auto value=it.value();
      if(chars(name)>128||!full(FIELD_NAME,name)||mmltk::tools::includes(AMBIENT_IDS,name)||name=="surface_high"||name=="surface_low")continue;
      if(equals(r.get("@event"),"vulkan.validation")&&(name=="validation_id"||name=="message_id"))continue;
      if(value.is_string()&&chars(str(value))>128)continue;
      auto typed=value.is_string()?match(TYPED_ID,str(value)):Match {
      };
      if(typed&&typed.end()==str(value).size()) {
        if(typed[1]=="DeviceId")continue;
        add( {
          typed[1]
        },Strings {
          encoded(typed[1]+"("+replace_rx(" ",typed[2],"")+")")
        });
      }
      else if(name.ends_with("_id")||name.ends_with("_identity")) {
        auto canonical=name.starts_with("parent_")?name.substr(7):name;
        auto key=correlation_key(r, {
          name
        });
        if(key)add( {
          canonical
        },key);
      }
      else if(name.ends_with("_surface")&&value.is_string()&&full("[0-9a-fA-F]{32}",str(value))&&Big("0x"+str(value))!=0)add( {
        "@surface"
      },Strings {
        encoded(lower(str(value)))
      });
    }
    for(auto m:findall(TYPED_ID,prefix(r.raw,MAX_TRIAGE_TEXT))) {
      if(out.size()>=16)break;
      if(m[1]!="DeviceId")add( {
        m[1]
      },Strings {
        encoded(m[1]+"("+replace_rx(" ",m[2],"")+")")
      });
    }
    return out;
  }
  std::pair<std::string,std::string>event_stage(const Record&r) {
    if(r.training_record())return {
    };
    auto e=r.get("@event");
    auto m=e.is_string()?match(STAGE_SUFFIX,str(e)):Match {
    };
    return m?std::pair {
      m[1],lower(m[2])
    }
    :std::pair<std::string,std::string> {
    };
  }
  bool triage_terminal(const Record&r) {
    return truth(r.get("@terminal"))||exact(r.get("terminal"),true)||event_stage(r).second=="terminal";
  }
  namespace detail {
    std::pair<int,std::string>anchor_rank(const Record&r,const std::vector<Identity>&ids= {
    }) {
      if(r.training_record())return truth(r.get("@error"))?std::pair {
        110,std::string("failed outcome/span")
      }
      :std::pair {
        0,std::string("training progress")
      };
      auto[family,stage]=event_stage(r);
      auto event=r.get("@event"),code=r.get("@exit_code");
      bool failed=false;
      for(auto n: {
        "span_outcome","outcome","status","result"
      }) {
        auto v=r.get(n);
        failed=failed||(v.is_string()&&mmltk::tools::includes(FAILED_STAGES,lower(str(v))));
      }
      int score=0;
      std::string why;
      if(failed||exact(r.get("failed"),true)) {
        score=110;
        why="failed outcome/span";
      }
      else if(equals(event,"catch.assertion_failed")||(event.is_string()&&str(event).find("assertion")!=std::string::npos&&truth(r.get("@error")))) {
        score=105;
        why="assertion failure";
      }
      else if(mmltk::tools::includes(FAILED_STAGES,stage)||in(r.get("@level"),words("error critical fatal panic"))) {
        score=100;
        why="explicit failure";
      }
      else {
        bool error=integer(code)&&bigint(code)!=0;
        for(auto n: {
          "message","detail","error"
        }) {
          auto v=r.get(n);
          error=error||(v.is_string()&&bool(search(FAILURE_WORD,str(v))));
        }
        if(error) {
          score=90;
          why="error/terminal candidate";
        }
        else if(truth(r.get("@error"))) {
          score=50;
          why="failure-related event (not an explicit failure)";
        }
        else if(mmltk::tools::includes(INCOMPLETE_STAGES,stage)) {
          score=mmltk::tools::includes(words("missing unavailable stalled"),stage)?85:45;
          why="incomplete state";
        }
        else if(!r.parse_error.empty()) {
          score=35;
          why="parse failure (may be capture damage)";
        }
        else if(triage_terminal(r)) {
          score=20;
          why="terminal evidence (not necessarily failure)";
        }
        else return {
          0,""
        };
      }
      if(!ids.empty())score+=10;
      if(meta(r,"context_copy"))score-=25;
      return {
        score,why
      };
    }
    struct PoolEntry {
      Json rank;
      Record record;
      Json why;
      std::size_t insertion;
      std::string key;
    };
    struct RankEntry {
      Json rank;
      std::size_t insertion;
      std::string key;
    };
    struct RankLess {
      bool operator()(const RankEntry&a,const RankEntry&b)const {
        if (observing("rank.compare")) observe("rank.compare", 1);
        int c=compare(a.rank,b.rank);
        if(c)return c<0;
        if(a.insertion!=b.insertion)return a.insertion>b.insertion;
        return a.key<b.key;
      }
    };
    class TriagePool {
      std::size_t limit_;
      bool retain_full_;
      std::size_t insertion_=0;
      std::map<std::string,PoolEntry>entries_;
      std::set<RankEntry,RankLess>ranks_;
      public: std::size_t discarded=0;
      explicit TriagePool(std::size_t limit,bool full=false):limit_(limit),retain_full_(full) {
      }
      std::size_t size()const {
        return entries_.size();
      }
      void add(const Record&r,Json rank,Json why,Json key=MISSING) {
        if(missing(key))key=physical_position(r);
        auto text=encoded(key);
        auto i=entries_.find(text);
        if(i!=entries_.end()) if (observing("rank.compare")) observe("rank.compare", 1);
        if(i!=entries_.end()&&compare(i->second.rank,rank)<=0)return;
        std::size_t insertion;
        if(i==entries_.end()) {
          if(entries_.size()==limit_) {
            auto worst=std::prev(ranks_.end());
            if (observing("rank.compare")) observe("rank.compare", 1);
            if(compare(worst->rank,rank)<=0) {
              ++discarded;
              return;
            }
            entries_.erase(worst->key);
            ranks_.erase(worst);
            ++discarded;
          }
          insertion=insertion_++;
        }
        else {
          insertion=i->second.insertion;
          ranks_.erase( {
            i->second.rank,insertion,text
          });
        }
        ranks_.insert( {
          rank,insertion,text
        });
        entries_.insert_or_assign(text,PoolEntry {
          std::move(rank),retain_full_?r:triage_snapshot(r),std::move(why),insertion,text
        });
        if (observing("pool.ranks")) observe("pool.ranks", ranks_.size());
      }
      std::vector<PoolEntry>rows()const {
        std::vector<PoolEntry>out;
        for(const auto&[k,e]:entries_)out.push_back(e);
        std::stable_sort(out.begin(),out.end(),[](const auto&a,const auto&b) {
          int c=compare(a.rank,b.rank);
          return c?c<0:a.insertion<b.insertion;
        });
        return out;
      }
    };
    struct NumericStats {
      std::size_t count=0,missing=0,invalid=0;
      Rational sum {
        0
      };
      Json min=nullptr,max=nullptr;
    };
    struct StatisticRow {
      Json run,recorded_run;
      std::string source,clock;
      Strings group;
      std::optional<Big>bucket;
      std::size_t events=0,first_line=0,last_line=0;
      std::optional<Big>first_ns,last_ns;
      std::map<std::string,NumericStats>fields;
    };
    // Statistics aggregate the full stream independently of bounded displayed rows.
    class Statistics {
      Strings fields_;
      std::optional<Big>width_;
      std::vector<StatisticRow>rows_;
      std::map<std::string,std::size_t>index_;
      static Json rational_number(const Rational&v) {
        return v.denominator()==1?number(v.numerator()):Json(v.numerator().convert_to<std::string>()+"/"+v.denominator().convert_to<std::string>());
      }
      public: Statistics(Strings fields,int bucket):fields_(std::move(fields)) {
        if(bucket)width_=Big(bucket)*1000000;
      }
      void include(const Record&r,const Strings&group) {
        auto recorded=r.get("run_id");
        if(missing(recorded)||recorded.is_object()||recorded.is_array()||recorded.is_boolean())recorded=nullptr;
        std::vector<std::optional<Big>>buckets {
          std::nullopt
        };
        if(width_&&r.time_ns) {
          Big q=*r.time_ns / *width_;
          if(*r.time_ns<0&&*r.time_ns % *width_!=0)--q;
          buckets.emplace_back(q * *width_);
        }
        for(auto bucket:buckets) {
          auto k=encoded(Json::array( {
            r.get("@run"),recorded,r.source,r.clock,group,bucket?number(*bucket):Json(nullptr)
          }));
          auto it=index_.find(k);
          if(it==index_.end()) {
            if(index_.size()==limits.distinct_keys)throw QueryError("statistics exceed "+std::to_string(limits.distinct_keys)+" groups/buckets; narrow inputs or increase --bucket-ms");
            StatisticRow row;
            row.run=r.get("@run");
            row.recorded_run=recorded;
            row.source=r.source;
            row.clock=r.clock;
            row.group=group;
            row.bucket=bucket;
            row.first_line=row.last_line=r.line;
            row.first_ns=row.last_ns=r.time_ns;
            for(auto&name:fields_)row.fields[name]= {
            };
            it=index_.emplace(k,rows_.size()).first;
            rows_.push_back(std::move(row));
          }
          auto&row=rows_[it->second];
          ++row.events;
          row.last_line=r.line;
          if(r.time_ns) {
            if(!row.first_ns||*r.time_ns<*row.first_ns)row.first_ns=r.time_ns;
            if(!row.last_ns||*r.time_ns>*row.last_ns)row.last_ns=r.time_ns;
          }
          for(auto&[name,v]:row.fields) {
            auto value=r.get(name);
            if(missing(value)||value.is_null()) {
              ++v.missing;
              continue;
            }
            if(!numeric(value)||(value.is_number_float()&&!std::isfinite(value.get<double>()))) {
              ++v.invalid;
              continue;
            }
            auto total=v.sum+rational(value);
            auto bits=[](const Big&n)->unsigned {
              return n==0?0:boost::multiprecision::msb(absbig(n))+1;
            };
            if(std::max(bits(total.numerator()),bits(total.denominator()))>8192)throw QueryError("statistics numeric capacity exceeds 8192 bits; narrow inputs");
            v.sum=total;
            ++v.count;
            if(v.min.is_null()||compare(value,v.min)<0)v.min=value;
            if(v.max.is_null()||compare(value,v.max)>0)v.max=value;
          }
        }
      }
      void render(std::ostream&out,const Strings&names)const {
        for(auto&row:rows_) {
          Json group=Json::object();
          for(std::size_t i=0;i<names.size();++i)group[names[i]]=parse_json(row.group[i]);
          Json r= {
            {
              "run",row.run
            }, {
              "recorded_run",row.recorded_run
            }, {
              "source",row.source
            }, {
              "clock",row.clock
            }, {
              "group",group
            }, {
              "events",row.events
            }, {
              "first_line",row.first_line
            }, {
              "last_line",row.last_line
            }, {
              "first_ns",row.first_ns?number(*row.first_ns):Json(nullptr)
            }, {
              "last_ns",row.last_ns?number(*row.last_ns):Json(nullptr)
            }
          };
          if(row.bucket) {
            r["bucket_start_ns"]=number(*row.bucket);
            r["bucket_end_ns"]=number(*row.bucket+*width_);
            r["events_per_second"]=rational_number(Rational(Big(row.events)*1000000000,*width_));
          }
          r["fields"]=Json::object();
          for(auto&name:fields_) {
            auto&v=row.fields.at(name);
            r["fields"][name]= {
              {
                "count",v.count
              }, {
                "missing",v.missing
              }, {
                "invalid",v.invalid
              }, {
                "sum",rational_number(v.sum)
              }, {
                "min",v.min
              }, {
                "max",v.max
              }, {
                "mean",v.count?rational_number(v.sum/Rational(Big(v.count))):Json(nullptr)
              }
            };
          }
          out<<"Statistics "<<encoded(r)<<'\n';
        }
      }
    };
    bool retained_better(const Retained&a,const Retained&b) {
      if(a.priority!=b.priority)return a.priority<b.priority;
      if(a.distance!=b.distance)return a.distance<b.distance;
      int c=compare(a.key,b.key);
      return a.tail?c>0:c<0;
    }
    void retain_heap(std::vector<Retained>&heap,Retained item,std::size_t limit) {
      if(!limit)return;
      if(heap.size()<limit) {
        heap.push_back(std::move(item));
        std::push_heap(heap.begin(),heap.end(),retained_better);
      }
      else if(retained_better(item,heap.front())) {
        std::pop_heap(heap.begin(),heap.end(),retained_better);
        heap.back()=std::move(item);
        std::push_heap(heap.begin(),heap.end(),retained_better);
      }
    }
    bool auto_specific_query(const Record&r) {
      auto e=textget(r,"@event");
      return !e.ends_with("progress")&&!e.ends_with("phase_advanced");
    }
    bool retained_key_less(const Retained& a, const Retained& b) {
      return compare(a.key,b.key)<0;
    }
    void collect_parse_error(const Record& row, std::size_t& count, Strings& examples) {
      if(row.parse_error.empty())return;
      ++count;
      if(examples.size()<5)examples.push_back(row.source+":"+std::to_string(row.line)+": "+row.parse_error);
    }
    struct QueryResult {
      Options options;
      std::size_t scanned=0,matches=0,related=0,proximity=0,context=0,errors=0,malformed=0;
      Strings parse_examples;
      std::map<std::string,std::size_t>files;
      std::map<Strings,std::size_t>groups;
      std::unique_ptr<Statistics>statistics;
      std::map<std::string,std::pair<std::optional<Big>,std::optional<Big>>>clocks;
      std::vector<Retained>retained,native_tail;
      std::map<std::string,Record>terminals;
      std::set<std::string>selected_runs;
      ErrorLookup*lookup=nullptr;
      std::array<std::vector<Retained>,2>recent_query;
      std::unique_ptr<TriagePool>representatives;
      std::size_t automatic_count=0,omitted_anchors=0,object_conflicts=0;
      bool automatic=false,highlight_recent=false;
      std::vector<Retained>recent;
      std::map<std::string,std::pair<std::vector<Retained>,std::vector<Retained>>>auto_related;
      explicit QueryResult(const Options&o):options(o) {
        if(!o.stats.empty()||o.bucket_ms)statistics=std::make_unique<Statistics>(o.stats,o.bucket_ms);
        if(o.representatives)representatives=std::make_unique<TriagePool>(o.limit,true);
      }
      void inspect(const Record&r,bool permitted) {
        ++scanned;
        collect_parse_error(r,malformed,parse_examples);
        if(permitted&&truth(r.get("@terminal"))) {
          auto code=r.get("@exit_code");
          auto key=encoded(Json::array( {
            r.get("@run"),r.get("@event"),missing(code)?Json(nullptr):code
          }));
          auto it=terminals.find(key);
          auto pref=[](const Record&v) {
            return Json::array( {
              meta(v,"context_copy"),v.source,v.line
            });
          };
          if(it==terminals.end()||compare(pref(r),pref(it->second))<0)terminals[key]=r;
          if(terminals.size()>limits.distinct_keys)throw QueryError("terminal summary exceeds bounded capacity; narrow inputs");
        }
        if(permitted&&lookup&&in(get(r.metadata,"role"),words("trace native"))&&r.get("@event").is_string()&&search(HANDOFF_EVENT,textget(r,"@event")))retain_heap(native_tail, {
          order_key(r,"capture"),r,"run-tail proximity",true
        },std::min(5,options.top));
      }
      void include(const Record&r,const std::string&reason) {
        if(reason=="query"&&auto_correlate_enabled(options)&&!options.representatives&&!meta(r,"context_copy")&&r.parse_error.empty())retain_heap(recent_query[auto_specific_query(r)], {
          order_key(r,"capture"),triage_snapshot(r),"query",true
        },3);
        Strings group;
        if(reason=="context") {
          ++context;
          group= {
            "context",encoded(physical_position(r))
          };
        }
        else {
          matches+=reason=="query";
          related+=reason=="correlated"||reason=="run";
          proximity+=reason=="proximity";
          selected_runs.insert(textget(r,"@run"));
          errors+=truth(r.get("@error"));
          ++files[r.source];
          group = group_key(r, options.group_by);
          if(!groups.contains(group)&&groups.size()==limits.distinct_keys)throw QueryError("group count exceeds "+std::to_string(limits.distinct_keys)+"; narrow the query or --group-by");
          ++groups[group];
          if(reason=="query"&&statistics)statistics->include(r,group);
          auto[it,inserted]=clocks.try_emplace(r.clock,std::pair {
            r.time_ns,r.time_ns
          });
          if(r.time_ns) {
            if(!it->second.first||*r.time_ns<*it->second.first)it->second.first=r.time_ns;
            if(!it->second.second||*r.time_ns>*it->second.second)it->second.second=r.time_ns;
          }
        }
        if(representatives) {
          representatives->add(r,Json::array( {
            meta(r,"context_copy"),order_key(r,options.order)
          }),reason,group);
          return;
        }
        Retained item {
          order_key(r,options.order),r,reason,options.tail
        };
        if(lookup) {
          auto e=r.get("@event");
          item.priority=reason=="query"?0:(truth(r.get("@terminal"))||(e.is_string()&&search(HANDOFF_EVENT,str(e))))?1:2;
          if(meta(r,"context_copy")||equals(get(r.metadata,"role"),"transcript"))item.priority+=3;
          item.distance=absbig(bigint(get(r.metadata,"proximity_ns",number(Big(1)<<63))));
        }
        retain_heap(retained,std::move(item),options.limit);
      }
      std::vector<Retained>rows()const {
        auto rows=retained;
        std::sort(rows.begin(),rows.end(),retained_key_less);
        if(!automatic)return rows;
        std::vector<Retained>out;
        auto append=[&](std::vector<Retained>v) {
          std::sort(v.begin(),v.end(),retained_key_less);
          out.insert(out.end(),v.begin(),v.end());
        };
        for(auto&item:rows) {
          auto i=auto_related.find(encoded(physical_position(item.record)));
          if(i!=auto_related.end())append(i->second.first);
          out.push_back(item);
          if(i!=auto_related.end())append(i->second.second);
        }
        return out;
      }
    };
  }
  bool auto_correlate_enabled(const Options&o) {
    return o.auto_correlate!=false&&(o.auto_correlate==true||o.format=="timeline")&&(!trim(o.query).empty()||o.errors)&&o.correlate.empty()&&!o.related_run&&!o.error&&!o.triage;
  }
  namespace detail {
    std::optional<std::string>auto_value(const Json&v) {
      if(integer(v)&&bigint(v)>0)return encoded(v);
      if(v.is_string()&&chars(str(v))<=128&&!trim(str(v)).empty()&&!full(R"(0+(?:[.:-]0+)*)",trim(str(v))))return encoded(v);
      return {
      };
    }
    Json identity_json(const Identity&i) {
      return Json::array( {
        i.run,i.names,i.values
      });
    }
    Identity identity_from(const Json&j) {
      return {
        j[0].get<std::string>(),j[1].get<Strings>(),j[2].get<Strings>()
      };
    }
  }
  AutoFacts auto_facts(const Record&r) {
    std::map<std::string,std::string>facts;
    auto event=r.get("@event");
    for(auto&name:AUTO_FIELDS)if(auto v=auto_value(r.get(name)))facts[name]=*v;
    for(auto name: {
      "@surface","@workspace_source"
    })if(auto v=auto_value(r.get(name)))facts[name]=*v;
    std::string domain;
    auto owner=r.get("@owner");
    if(in(owner,words("explore annotation upscale live")))domain=str(owner);
    if(equals(event,"iced.gallery.source"))domain="explore";
    if(event.is_string()) {
      auto i=AUTO_INTEGRATION_FIELDS.find(str(event));
      if(i!=AUTO_INTEGRATION_FIELDS.end()) {
        domain=i->second.first;
        for(auto&[external,native]:i->second.second) {
          auto v=r.get(external);
          if(v.is_string()&&full("[1-9][0-9]{0,15}",str(v)))v=number(decimal_integer(str(v)));
          if(numeric(v)) {
            auto n=rational(v);
            if(n>Rational(0)&&n<=Rational(Big(1)<<53)&&n.denominator()==1)facts.try_emplace(native,encoded(number(n.numerator())));
          }
        }
      }
    }
    if(in(event,words("integration.phase_progress integration.phase_advanced")))if(auto v=auto_value(r.get("detail")))facts.try_emplace("phase",*v);
    if(!domain.empty())facts["domain"]=domain;
    for(auto[canonical,first,second]: {
      std::tuple {
        "frame","source_revision","frame_revision"
      }, {
        "snapshot","source_observation_revision","snapshot_revision"
      }
    }) {
      auto i=facts.find(first);
      if(i==facts.end())i=facts.find(second);
      if(i!=facts.end())facts[canonical]=i->second;
    }
    std::string run=str(get(r.metadata,"run",r.source));
    std::vector<std::pair<int,Identity>>ids;
    auto add=[&](int priority,Strings names,std::optional<Strings>values= {
    }) {
      Strings key;
      if(values)key=*values;
      else for(auto&n:names) {
        auto it=facts.find(n);
        if(it==facts.end())return;
        key.push_back(it->second);
      }
      std::pair<int,Identity>v {
        priority, {
          run,std::move(names),std::move(key)
        }
      };
      mmltk::tools::append_unique(ids, std::move(v));
    };
    if(process_evidence(r)) {
      for(auto n: {
        "@process","@parent_process","@process_group"
      }) {
        auto v=r.get(n);
        if(!missing(v))facts[n]=encoded(v);
      }
      add(0, {
        "@process"
      });
      if(facts.contains("@parent_process"))add(0, {
        "@process"
      },Strings {
        facts["@parent_process"]
      });
      add(1, {
        "@process_group"
      });
    }
    if(equals(event,"vulkan.validation")||in(event,VULKAN_PROVENANCE_EVENTS)) {
      auto process=vulkan_process(r);
      if(!missing(process)) {
        if(!process.is_null())facts["browser_process_id"]=encoded(process);
        if(auto device=vulkan_handle(r.get("vk_device")))facts["vk_device"]=encoded(*device);
        for(auto&[name,handle]:vulkan_resources(r.data)) {
          if(!process.is_null())add(0, {
            "browser_process_id",name
          },Strings {
            encoded(process),encoded(handle)
          });
          add(1, {
            "@file",name
          },Strings {
            encoded(r.source),encoded(handle)
          });
        }
      }
    }
    if(in(event,MEMORY_PROVENANCE_EVENTS)) {
      add(0, {
        "native_process_id","workspace_allocation"
      });
      add(0, {
        "browser_process_id","@workspace_source","workspace_allocation"
      });
    }
    for(auto field: {
      "frame","snapshot"
    }) {
      add(0, {
        "source_session","source_instance",field
      });
      if(!domain.empty()&&facts.contains(field))add(1, {
        domain+"."+field
      },Strings {
        facts[field]
      });
    }
    for(auto field: {
      "presentation_revision","allocation_generation"
    })add(0, {
      "@surface",field
    });
    for(auto field: {
      "transfer_sequence","timeline_ready","workspace_allocation"
    })add(0, {
      "@workspace_source",field
    });
    for(auto field: {
      "span_id","request_id","operation_id","presentation_revision"
    })add(2, {
      field
    });
    if(facts.contains("parent_span_id"))add(2, {
      "span_id"
    },Strings {
      facts["parent_span_id"]
    });
    for(auto field: {
      "trace_id","@surface","@workspace_source","dataset_identity","gallery_identity"
    })add(3, {
      field
    });
    add(1, {
      "dataset_identity","gallery_generation"
    });
    if(!domain.empty()&&facts.contains("observation_revision"))add(1, {
      domain+".observation_revision"
    },Strings {
      facts["observation_revision"]
    });
    for(auto field: {
      "phase","phase_id"
    })add(4, {
      field
    });
    auto control=r.get("control");
    if(control.is_string()&&search("[./]",str(control)))add(5, {
      "control"
    });
    return {
      facts,ids
    };
  }
  namespace detail {
    std::optional<int>auto_event_rank(const Record&r) {
      auto event=r.get("@event");
      if(process_evidence(r))return 2;
      if(!event.is_string()||search(AUTO_NOISE,str(event)))return {
      };
      if(in(event,MEMORY_PROVENANCE_EVENTS)||in(event,VULKAN_PROVENANCE_EVENTS))return 2;
      if(AUTO_INTEGRATION_FIELDS.contains(str(event))||equals(event,"iced.gallery.source"))return 1;
      auto parts=tools::split(str(event),'.',true);
      if(mmltk::tools::includes(parts,"snapshot")||mmltk::tools::includes(words("published publication"),parts.back()))return 0;
      if(mmltk::tools::includes(words("state source observation edge"),parts.back()))return 1;
      auto stage=event_stage(r).second;
      if(parts.front()=="application"||mmltk::tools::includes(START_STAGES,stage)||mmltk::tools::includes(FAILED_STAGES,stage)||mmltk::tools::includes(INCOMPLETE_STAGES,stage)||mmltk::tools::includes(words("ready started completed complete failed failure created imported retired released exited signaled terminal"),parts.back()))return 2;
      if(equals(event,"integration.phase_advanced"))return 3;
      return {
      };
    }
    // Sorted nearest-anchor indexes bound direct enrichment without a run-wide index.
    class AutoAnchorGroup {
      std::optional<std::size_t>first;
      std::map<std::string,std::vector<std::pair<Big,std::size_t>>>times,lines;
      public: void add(std::size_t i,const Record&r) {
        if(!first)first=i;
        if(r.time_ns)times[r.clock].emplace_back(*r.time_ns,i);
        lines[r.source].emplace_back(Big(r.line),i);
      }
      void finish() {
        for(auto*map: {
          &times,&lines
        })for(auto&[key,entries]:*map)std::sort(entries.begin(),entries.end());
      }
      std::vector<std::size_t>nearest(const Record&r)const {
        const std::vector<std::pair<Big,std::size_t>>*entries=nullptr;
        Big point=ns(r);
        if(r.time_ns) {
          auto it=times.find(r.clock);
          if(it!=times.end())entries=&it->second;
        }
        if(!entries||entries->empty()) {
          auto it=lines.find(r.source);
          if(it!=lines.end())entries=&it->second;
          point=r.line;
        }
        if(!entries||entries->empty())return {
          *first
        };
        auto it=std::lower_bound(entries->begin(),entries->end(),std::pair<Big,std::size_t> {
          point,0
        },[](const auto& left,const auto& right) {
          if (observing("anchor.read")) observe("anchor.read", 1);
          return left<right;
        });
        std::vector<std::size_t>out;
        if(it!=entries->begin())out.push_back(std::prev(it)->second);
        if(it!=entries->end())out.push_back(it->second);
        return out;
      }
    };
    bool object_identity(const Identity&id) {
      return id.names.size()==2&&(id.names[0]=="@file"||id.names[0]=="browser_process_id")&&mmltk::tools::includes(words("vk_image vk_memory vk_command_buffer"),id.names[1]);
    }
    void auto_collect(QueryResult&result,const std::vector<LogFile>&files,const Expression&q,const Expression&w,const Anchors&anchors) {
      result.automatic=true;
      std::size_t limit=std::min<std::size_t>(MAX_AUTO_RELATED,std::max(0,result.options.limit-static_cast<int>(result.retained.size())));
      std::vector<Retained>matches,specific;
      for(auto&i:result.retained)if(i.reason=="query"&&!meta(i.record,"context_copy")&&i.record.parse_error.empty()) {
        matches.push_back(i);
        if(auto_specific_query(i.record))specific.push_back(i);
      }
      auto recency=[](const Retained&a,const Retained&b) {
        return compare(order_key(a.record,"capture"),order_key(b.record,"capture"))>0;
      };
      std::sort(specific.begin(),specific.end(),recency);
      if(specific.size()>MAX_AUTO_ANCHORS)specific.resize(MAX_AUTO_ANCHORS);
      std::set<std::string>positions;
      for(auto&i:specific)positions.insert(encoded(physical_position(i.record)));
      std::sort(matches.begin(),matches.end(),recency);
      for(auto&i:matches)if(specific.size()<MAX_AUTO_ANCHORS&&!positions.contains(encoded(physical_position(i.record))))specific.push_back(i);
      auto seeds=specific;
      std::sort(seeds.begin(),seeds.end(),retained_key_less);
      result.recent=result.recent_query[1].empty()?result.recent_query[0]:result.recent_query[1];
      std::sort(result.recent.begin(),result.recent.end(),retained_key_less);
      result.highlight_recent=!result.options.representatives&&(result.matches>20||result.matches>matches.size());
      result.omitted_anchors=matches.size()>seeds.size()?matches.size()-seeds.size():0;
      if(!limit)return;
      std::vector<std::map<std::string,std::string>>seed_facts;
      std::map<Identity,AutoAnchorGroup>groups;
      std::vector<TriagePool>pools;
      std::map<Identity,std::map<std::string,std::string>>owners;
      std::map<Identity,std::pair<std::string,std::string>>bridges;
      std::set<Identity>ambiguous;
      auto observe=[&](int priority,const Identity&id,const auto&facts) {
        if(object_identity(id)) {
          auto&previous=owners[id];
          for(auto name: {
            "browser_process_id","vk_device","@workspace_source","workspace_allocation"
          }) {
            auto it=facts.find(name);
            if(it!=facts.end()) {
              auto[p,added]=previous.try_emplace(name,it->second);
              if(!added&&p->second!=it->second)ambiguous.insert(id);
            }
          }
        }
        if(priority==1&&id.names.size()==1&&facts.contains("domain")&&facts.contains("source_session")&&facts.contains("source_instance")) {
          auto source=std::pair {
            facts.at("source_session"),facts.at("source_instance")
          };
          auto[it,added]=bridges.try_emplace(id,source);
          if(!added&&it->second!=source)ambiguous.insert(id);
        }
      };
      for(std::size_t i=0;i<seeds.size();++i) {
        auto[facts,ids]=auto_facts(seeds[i].record);
        seed_facts.push_back(facts);
        pools.emplace_back(equals(seeds[i].record.get("@event"),"vulkan.validation")?MAX_AUTO_VULKAN_RELATED:MAX_AUTO_PER_ANCHOR);
        for(auto&[priority,id]:ids) {
          observe(priority,id,facts);
          groups[id].add(i,seeds[i].record);
        }
      }
      for(auto&[id,group]:groups)group.finish();
      if(groups.empty())return;
      std::set<std::string>existing,runs,sources;
      for(auto&i:result.retained)existing.insert(encoded(physical_position(i.record)));
      for(auto&i:seeds) {
        runs.insert(textget(i.record,"@run"));
        sources.insert(i.record.source);
      }
      auto link=[&](const Record&r,const std::map<std::string,std::string>&facts,int priority,const Identity&id,std::size_t index)->std::optional<std::pair<Json,std::string>> {
        const auto&seed=seeds[index].record;
        const auto&original=seed_facts[index];
        auto equal_fact=[&](std::string_view a,const auto&left,std::string_view b,const auto&right,const std::string&value) {
          auto x=left.find(std::string(a)),y=right.find(std::string(b));
          return x!=left.end()&&y!=right.end()&&x->second==value&&y->second==value;
        };
        if(id.names==Strings {
          "@process"
        }
        ||id.names==Strings {
          "@process_group"
        }) {
          if(facts.contains("@process_group")&&original.contains("@process_group")&&facts.at("@process_group")!=original.at("@process_group"))return {
          };
          std::string relation;
          if(id.names==Strings {
            "@process"
          }) {
            auto observed=id.values[0];
            bool same=equal_fact("@process",facts,"@process",original,observed),parent=equal_fact("@process",facts,"@parent_process",original,observed)||equal_fact("@process",original,"@parent_process",facts,observed);
            if(!same&&!parent)return {
            };
            relation=same?"exact recorded process":"recorded parent/child relationship";
          }
          else relation="recorded process-group membership; not a crash-cause attribution";
          return std::pair {
            Json::array( {
              false,priority,number(r.source==seed.source?absbig(Big(r.line)-seed.line):Big(1)<<63),index
            }),id.label()+" ("+relation+")"
          };
        }
        for(auto name:words("domain @surface @workspace_source workspace_allocation transfer_sequence source_session source_instance frame presentation_revision allocation_generation dataset_identity gallery_identity browser_process_id vk_device"))if(facts.contains(name)&&original.contains(name)&&facts.at(name)!=original.at(name))return {
        };
        bool comparable=r.clock==seed.clock&&r.time_ns&&seed.time_ns;
        Big distance=comparable?absbig(*r.time_ns-*seed.time_ns):Big(1)<<63;
        bool weak=priority>=4;
        std::string why;
        if(weak) {
          if(comparable) {
            if(distance>Big(std::min(250,result.options.near_ms))*1000000)return {
            };
            why=id.label()+" + time proximity";
          }
          else if(r.source==seed.source&&absbig(Big(r.line)-seed.line)<=8) {
            distance=absbig(Big(r.line)-seed.line);
            why=id.label()+" + line proximity";
          }
          else return {
          };
        }
        else {
          why=id.label();
          if(object_identity(id)) {
            if(r.source==seed.source&&!comparable)distance=absbig(Big(r.line)-seed.line);
            if(id.names[0]=="@file")why="same-file handle candidate; process unrecorded: "+id.names[1]+"="+str(parse_json(id.values[1]));
            else why+=" (exact typed handle and recorded process)";
          }
        }
        return std::pair {
          Json::array( {
            weak,priority,number(distance),index
          }),why
        };
      };
      for(auto&f:files) {
        bool relevant=sources.contains(f.source)||runs.contains(str(f.metadata["run"]));
        for(auto&run:f.linked_runs)relevant=relevant||runs.contains(run);
        if(!relevant)continue;
        f.records([&](Record r) {
          if(!runs.contains(textget(r,"@run"))||meta(r,"context_copy")||!r.parse_error.empty()||existing.contains(encoded(physical_position(r)))||!w.matches(r)||q.matches(r))return;
          auto event_rank=auto_event_rank(r);
          if(!event_rank)return;
          auto[facts,ids]=auto_facts(r);
          std::optional<std::tuple<Json,std::string,Identity,std::size_t>>best;
          for(auto&[priority,id]:ids) {
            auto g=groups.find(id);
            if(g==groups.end())continue;
            observe(priority,id,facts);
            for(auto i:g->second.nearest(r)) {
              auto linked=link(r,facts,priority,id,i);
              if(linked&&(!best||compare(linked->first,std::get<0>(*best))<0))best= {
                linked->first,linked->second,id,i
              };
            }
          }
          if(!best)return;
          auto[rank,why,id,index]=*best;
          Json fullrank=Json::array( {
            rank[0],*event_rank,rank[1],rank[2],rank[3]
          });
          fullrank=concatenate(fullrank,physical_position(r));
          pools[index].add(r,fullrank,Json::array( {
            identity_json(id),why
          }),textget(r,"@event"));
        },true,anchors);
      }
      struct Candidate {
        Json rank;
        std::size_t index;
        Record record;
        Identity identity;
        std::string why;
      };
      std::vector<Candidate>candidates;
      for(std::size_t i=0;i<pools.size();++i)for(auto&e:pools[i].rows())candidates.push_back( {
        e.rank,i,e.record,identity_from(e.why[0]),str(e.why[1])
      });
      std::sort(candidates.begin(),candidates.end(),[](const auto&a,const auto&b) {
        int c=compare(a.rank,b.rank);
        return c?c<0:a.index<b.index;
      });
      std::map<Identity,int>counts;
      int weak=0;
      for(auto&c:candidates) {
        if(result.automatic_count==limit)break;
        int budget=object_identity(c.identity)?MAX_AUTO_VULKAN_RELATED:MAX_AUTO_PER_IDENTITY;
        if(ambiguous.contains(c.identity)||counts[c.identity]==budget)continue;
        bool weak_row=truth(c.rank[0]);
        if(weak_row&&weak==MAX_AUTO_WEAK)continue;
        weak+=weak_row;
        ++counts[c.identity];
        auto&seed=seeds[c.index].record;
        auto&r=c.record;
        r.metadata["auto_reason"]=compact(c.why,150);
        r.metadata["auto_anchor"]= {
          {
            "file",seed.source
          }, {
            "line",seed.line
          }, {
            "part",part(seed)
          }
        };
        if(meta(r,"triage_payload_truncated")) {
          r.metadata.erase("triage_payload_truncated");
          r.metadata["auto_payload_truncated"]=true;
        }
        bool after=r.clock==seed.clock&&r.time_ns&&seed.time_ns&&*r.time_ns>*seed.time_ns;
        auto&pair=result.auto_related[encoded(physical_position(seed))];
        (after?pair.second:pair.first).push_back( {
          order_key(r,result.options.order),r,"auto",result.options.tail
        });
        ++result.automatic_count;
      }
      for(auto&id:ambiguous)result.object_conflicts+=object_identity(id);
    }
    QueryResult execute(const std::vector<LogFile>&files,const Expression&q,const Expression&w,const Options&o,const Anchors&anchors= {
    },ErrorLookup*lookup=nullptr) {
      std::vector<Strings>relationships;
      bool scope=!o.family.empty()||o.history||!o.run.empty();
      for(auto&s:o.correlate) {
        auto names=field_names(s,'+');
        if(scope)names.insert(names.begin(),"@run");
        relationships.push_back(std::move(names));
      }
      std::vector<std::set<Strings>>seeds(relationships.size());
      std::set<std::string>seed_runs;
      bool two_passes=!relationships.empty()||o.related_run;
      if(two_passes)for(auto&f:files)f.records([&](Record r) {
        if(!w.matches(r)||!q.matches(r))return;
        if(o.related_run)seed_runs.insert(textget(r,"@run"));
        for(std::size_t i=0;i<relationships.size();++i)if(auto key=correlation_key(r,relationships[i])) {
          seeds[i].insert(*key);
          if(seeds[i].size()>limits.distinct_keys)throw QueryError("correlation seeds exceed "+std::to_string(limits.distinct_keys)+"; narrow --query or --where");
        }
      },true,anchors);
      QueryResult result(o);
      result.lookup=lookup;
      for(auto&f:files) {
        std::deque<Record>before;
        int remaining=0;
        std::pair<std::size_t,std::size_t>last {
          0,0
        };
        f.records([&](Record r) {
          bool permitted=w.matches(r);
          result.inspect(r,permitted);
          std::string reason;
          if(permitted) {
            if(q.matches(r)) {
              if(!lookup||r.metadata["run"]==lookup->focus->metadata["run"]) {
                reason="query";
                if(lookup)lookup->annotate(r,true);
              }
            }
            else {
              for(std::size_t i=0;i<relationships.size();++i) {
                auto key=correlation_key(r,relationships[i]);
                if(key&&seeds[i].contains(*key)) {
                  reason="correlated";
                  break;
                }
              }
              if(reason.empty()) {
                if(lookup&&lookup->annotate(r))reason="proximity";
                else if(seed_runs.contains(textget(r,"@run")))reason="run";
              }
            }
          }
          if(!reason.empty()) {
            for(auto&previous:before) {
              if (observing("context.visit")) observe("context.visit", 1);
              auto pos=std::pair {
                previous.line,part(previous)
              };
              if(pos>last&&w.matches(previous)) {
                result.include(previous,"context");
                last=pos;
              }
            }
            before.clear();
            result.include(r,reason);
            last= {
              r.line,part(r)
            };
            remaining=o.context;
          }
          else if(remaining) {
            if(permitted) {
              result.include(r,"context");
              last= {
                r.line,part(r)
              };
            }
            --remaining;
          }
          if(o.context) {
            before.push_back(std::move(r));
            if(before.size()>static_cast<std::size_t>(o.context))before.pop_front();
          }
        },two_passes||!anchors.empty(),anchors);
      }
      if(result.representatives)for(auto&e:result.representatives->rows()) {
        auto&r=e.record;
        if(!equals(e.why,"context")) {
          const auto group = group_key(r, o.group_by);
          r.metadata["representative_count"]=result.groups[group];
        }
        result.retained.push_back( {
          order_key(r,o.order),r,str(e.why),false
        });
      }
      if(auto_correlate_enabled(o))auto_collect(result,files,q,w,anchors);
      return result;
    }
  }
  namespace detail {
    struct TriageFinding {
      std::string kind;
      int score=0;
      std::string message;
      Record record;
      std::optional<Identity>identity;
      std::optional<Record>other;
      std::size_t observations=1;
      Json last_position=MISSING;
    };
    struct StageBalance {
      std::optional<Record>first,last_end;
      int pending=0;
      std::string last_start;
      void begin(const Record&r,const std::string&stage) {
        if(!pending)first=triage_snapshot(r);
        if(!pending||last_start==stage)++pending;
        last_start=stage;
      }
    };
    struct PixelChainSample {
      struct Entry {
        Json sample;
        Record record;
      };
      std::map<std::string,Entry>stages;
      std::size_t mailbox_count=0;
      Json mailbox=nullptr,direct_transfer=nullptr;
    };
    std::optional<Identity>lifecycle_identity(const std::vector<Identity>&ids) {
      if(ids.empty())return {
      };
      auto priority=[](const Identity&i) {
        int p=i.names[0]=="span_id"?0:i.names[0]=="request_id"?1:i.names[0]=="@surface"?2:i.names[0]=="trace_id"?4:i.names[0].ends_with("Id")?5:3;
        return std::pair {
          p,i.names
        };
      };
      return *std::min_element(ids.begin(),ids.end(),[&](const auto&a,const auto&b) {
        return priority(a)<priority(b);
      });
    }
    class Triage;
    struct IdentityChain {
      Identity identity;
      Record origin;
      std::string why;
      int hop=0;
      std::size_t count=0,copies=0;
      std::map<std::string,std::size_t>events,owners;
      Strings event_order,owner_order;
      TriagePool representatives {
        16
      };
      std::map<std::pair<std::string,std::string>,StageBalance>states;
      std::map<std::string,Record>last_by_source,bounds;
      std::map<std::string,std::tuple<Big,Record,Record>>gaps;
      bool limited=false;
      IdentityChain(Identity id,Record row,std::string reason,int h):identity(std::move(id)),origin(std::move(row)),why(std::move(reason)),hop(h) {
      }
      void observe(const Record&,Triage&,bool balance=true);
      void finish(Triage&);
    };
    // Each diagnostic pass retains only configured identities, bounded samples and facts.
    class Triage {
      public: Options options;
      Expression where;
      Anchors anchors;
      ErrorLookup*lookup;
      std::vector<Strings>explicit_fields;
      TriagePool seed_pool,context_pool {
        32
      },failure_pool,terminal_pool {
        4
      },source_tails {
        4
      },evidence;
      std::vector<PoolEntry>selected;
      std::map<Identity,IdentityChain>identities;
      std::vector<Identity>identity_order;
      std::map<Identity,std::vector<std::pair<Record,Identity>>>refinements;
      std::vector<Identity>refinement_order;
      std::map<Strings,std::map<Strings,Identity>>refinement_groups;
      std::map<std::string,TriageFinding>findings;
      std::set<std::string>notes;
      std::string run;
      std::size_t scanned=0,scoped=0,related=0,malformed=0;
      Strings parse_examples,artifacts;
      std::map<std::string,std::string>artifact_discovery;
      std::map<std::string,StageBalance>pending;
      std::map<std::string,Record>observed_ends;
      std::map<std::string,PixelChainSample>pixel_samples;
      std::map<std::string,std::pair<std::string,Record>>pixel_sources;
      bool pixel_sources_truncated=false;
      std::map<std::string,std::vector<Record>>pixel_pending;
      std::size_t pixel_pending_count=0,pixel_stage_count=0;
      std::set<std::string>pixel_divergences;
      Triage(const Options&o,Expression w,Anchors a,ErrorLookup*l=nullptr):options(o),where(std::move(w)),anchors(std::move(a)),lookup(l),seed_pool(std::max(32,o.triage_anchors*4)),failure_pool(o.triage_anchors),evidence(o.limit) {
        for(auto&s:o.correlate)explicit_fields.push_back(field_names(s,'+'));
      }
      void records(const std::vector<LogFile>&files,const std::function<void(Record)>&visit,bool apply_where=true) {
        for(auto&f:files)f.records([&](Record r) {
          if((!apply_where||where.matches(r))&&(run.empty()||equals(r.metadata["run"],run)))visit(std::move(r));
        },true,anchors);
      }
      void finding(const std::string&kind,int score,const std::string&message,const Record&r,std::optional<Identity>id= {
      },std::optional<Record>other= {
      },Json discriminator=MISSING) {
        if(id) {
          auto it=identities.find(*id);
          if(it!=identities.end())score-=std::min(15,it->second.hop*5);
        }
        auto row=r;
        if(lookup&&!nearby(row))score-=25;
        auto pos=physical_position(r);
        auto anchor=findings.find(encoded(Json::array( {
          "anchor-failure",pos
        })));
        if(kind=="explicit-failure"&&anchor!=findings.end()) {
          if(!anchor->second.identity)anchor->second.identity=id;
          return;
        }
        bool repeated=mmltk::tools::includes(words("incomplete-state explicit-failure unmatched-end duplicate-terminal owner-handoff clock-regression"),kind);
        Json key=repeated?Json::array( {
          kind,r.source,textget(r,"@event"),id?identity_json(*id):Json(nullptr)
        }):Json::array( {
          kind,pos
        });
        if(!missing(discriminator))key.push_back(discriminator);
        auto k=encoded(key);
        auto rank=[](const TriageFinding&f) {
          return concatenate(Json::array( {
            -f.score
          }),concatenate(physical_position(f.record),Json::array( {
            f.kind
          })));
        };
        auto newrank=concatenate(Json::array( {
          -score
        }),concatenate(pos,Json::array( {
          kind
        })));
        std::size_t count=1;
        auto it=findings.find(k);
        if(it!=findings.end()) {
          count=it->second.observations+!exact(it->second.last_position,pos);
          if(compare(newrank,rank(it->second))>=0) {
            it->second.observations=count;
            it->second.last_position=pos;
            return;
          }
        }
        else if(findings.size()>=64) {
          notes.insert("Finding inventory capped at 64; lower-ranked observations omitted.");
          auto worst=std::max_element(findings.begin(),findings.end(),[&](const auto&a,const auto&b) {
            return compare(rank(a.second),rank(b.second))<0;
          });
          if(compare(newrank,rank(worst->second))>=0)return;
          findings.erase(worst);
        }
        findings.insert_or_assign(k,TriageFinding {
          kind,score,message,triage_snapshot(r),id,other?std::optional<Record>(triage_snapshot(*other)):std::nullopt,count,pos
        });
      }
      static std::optional<std::tuple<Json,Json,Json>>pixel_sample(const Record&r,Json source=nullptr) {
        auto event=textget(r,"@event");
        auto boundary=r.get("boundary");
        std::string name;
        Json transfer;
        if(auto i=NATIVE_PIXEL_STAGES.find(event);i!=NATIVE_PIXEL_STAGES.end()) {
          name=i->second;
          transfer=Json::array( {
            r.get("@workspace_source"),r.get("transfer_sequence")
          });
        }
        else if(auto i=DIRECT_PIXEL_STAGES.find(event);i!=DIRECT_PIXEL_STAGES.end()) {
          name=i->second;
          transfer=Json::array( {
            source,r.get("transfer_sequence")
          });
        }
        else if(event=="firefox.workspace.pixel"&&in(boundary,words("import mailbox"))) {
          name=str(boundary);
          transfer=Json::array( {
            source,r.get("transfer_sequence")
          });
        }
        else if(event=="iced.surface.pixel") {
          name="sample";
          transfer=nullptr;
        }
        else return {
        };
        auto surface=r.get("@surface"),revision=r.get("presentation_revision"),index=r.get("sample_index");
        Json sample=Json::array( {
          r.get("sample_x"),r.get("sample_y"),r.get("sample_rgba")
        });
        if(!surface.is_string()||!integer(revision)||bigint(revision)<=0||!integer(index)||bigint(index)<0||bigint(index)>=25||!std::all_of(sample.begin(),sample.end(),integer)||(name!="sample"&&(!transfer[0].is_string()||!integer(transfer[1])||bigint(transfer[1])<=0)))return {
        };
        return std::tuple {
          Json::array( {
            get(r.metadata,"run",r.source),surface,revision,index
          }),Json::array( {
            name,transfer
          }),sample
        };
      }
      void pixel_divergence(const std::string&kind,const std::string&message,const Record&r,std::optional<Record>other,const Json&publication,bool emit,Json edge=MISSING) {
        Json id=publication;
        if(!missing(edge))id.push_back(edge);
        if(!pixel_divergences.insert(encoded(id)).second)return;
        if(emit)finding(kind,115,message,r, {
        },other,edge);
        else seed_pool.add(r,concatenate(Json::array( {
          -120,false,number(-mtime(r))
        }),physical_position(r)),"deterministic pixel-chain divergence",concatenate(Json::array( {
          "pixel-chain"
        }),id));
      }
      void observe_pixel_chain(const Record&r,bool emit,std::optional<std::pair<std::string,Record>>bridge= {
      }) {
        auto event=textget(r,"@event");
        if(event=="firefox.workspace.direct_memory_pixel") {
          notes.insert("Historical direct_memory_pixel used an alias invalidated by the image ownership acquire; excluded from pixel comparisons.");
          return;
        }
        Json source=nullptr;
        auto ds=DIRECT_PIXEL_STAGES.find(event),ns=NATIVE_PIXEL_STAGES.find(event);
        bool direct_probe=ds!=DIRECT_PIXEL_STAGES.end(),source_probe=ns!=NATIVE_PIXEL_STAGES.end()&&ns->second!="native";
        std::string optional_stage=direct_probe?ds->second:source_probe?ns->second:"",optional_kind=direct_probe?"direct":source_probe?"source-copy":"";
        if((event=="firefox.workspace.direct_memory_alias"||event=="firefox.workspace.direct_memory_status")&&(exact(r.get("memory_alias_supported"),false)||(!missing(r.get("completion"))&&!equals(r.get("completion"),"Ok(true)"))))notes.insert("Optional direct-memory probe reports unsupported or unsettled evidence; inspect its structured reason.");
        if(direct_probe&&(!equals(r.get("completion"),"Ok(true)")||!exact(r.get("readback_marker"),0x4d4d4c54)||!exact(r.get("expected_marker"),0x4d4d4c54))) {
          notes.insert("Optional "+ds->second+" probe lacks successful completion or its readback marker; bytes were not compared.");
          return;
        }
        if(source_probe&&!r.get("@workspace_source").is_string()) {
          notes.insert("Optional native probe lacks an exact source identity; bytes were not compared.");
          return;
        }
        if(direct_probe||source_probe||event=="firefox.workspace.frame_forwarded"||event=="firefox.workspace.pixel") {
          Json key=Json::array( {
            get(r.metadata,"run",r.source),r.get("@surface"),r.get("presentation_revision"),r.get("transfer_sequence")
          });
          if(!key[1].is_string()||!integer(key[2])||bigint(key[2])<=0||!integer(key[3])||bigint(key[3])<=0)return;
          Json publication=Json::array( {
            key[0],key[1],key[2]
          });
          auto k=encoded(key);
          if(event=="firefox.workspace.frame_forwarded") {
            source=r.get("@workspace_source");
            if(!source.is_string())return;
            auto previous=pixel_sources.find(k);
            if(previous!=pixel_sources.end()&&previous->second.first!=str(source)) {
              pixel_divergence("pixel-transfer-conflict","forwarded physical transfer names conflicting native sources",r,previous->second.second,publication,emit);
              return;
            }
            std::pair<std::string,Record>bound=previous==pixel_sources.end()?std::pair {
              str(source),triage_snapshot(r)
            }
            :previous->second;
            if(previous==pixel_sources.end()) {
              if(pixel_sources.size()>=limits.pixel_samples) {
                pixel_sources_truncated=true;
                notes.insert("Pixel transfer bridge capacity reached; narrow the capture to compare additional transfers.");
              }
              else pixel_sources.emplace(k,bound);
            }
            auto pending_it=pixel_pending.find(k);
            if(pending_it!=pixel_pending.end()) {
              auto pending_rows=std::move(pending_it->second);
              pixel_pending.erase(pending_it);
              for(auto&row:pending_rows) {
                --pixel_pending_count;
                observe_pixel_chain(row,emit,bound);
              }
            }
            if(exact(r.get("direct_sampling"),true)) {
              Json transfer=Json::array( {
                source,key[3]
              });
              for(int index=0;index<25;++index) {
                Json sample_key=publication;
                sample_key.push_back(index);
                auto sk=encoded(sample_key);
                if(!pixel_samples.contains(sk)) {
                  if(pixel_samples.size()>=limits.pixel_samples) {
                    notes.insert("Pixel-chain sample capacity reached; additional direct publications were not compared.");
                    break;
                  }
                  pixel_samples.emplace(sk,PixelChainSample {
                  });
                }
                auto&chain=pixel_samples.at(sk);
                if(!chain.direct_transfer.is_null()&&!exact(chain.direct_transfer,transfer)) {
                  pixel_divergence("pixel-transfer-conflict","direct publication acquired conflicting physical sources",r,bound.second,publication,emit);
                  break;
                }
                chain.direct_transfer=transfer;
                bool copied=false;
                for(auto&[stage,s]:chain.stages) {
                  auto n=parse_json(stage)[0];
                  copied=copied||equals(n,"import")||equals(n,"mailbox");
                }
                if(copied)pixel_divergence("pixel-mode-conflict","direct sampling contains a forbidden sample-arena copy receipt",r, {
                },publication,emit);
                Strings names {
                  "sample","direct_image","native","native_after_release","source_copy_before_ready","source_copy_after_release"
                };
                std::vector<Record>replay;
                for(auto&name:names) {
                  Json stage=Json::array( {
                    name,name=="sample"?Json(nullptr):transfer
                  });
                  auto it=chain.stages.find(encoded(stage));
                  if(it!=chain.stages.end())replay.push_back(it->second.record);
                }
                for(auto&row:replay)observe_pixel_chain(row,emit,bound);
              }
            }
            return;
          }
          if(!bridge) {
            auto it=pixel_sources.find(k);
            if(it!=pixel_sources.end())bridge=it->second;
          }
          if(!bridge) {
            if(pixel_pending_count<limits.pixel_samples) {
              pixel_pending[k].push_back(triage_snapshot(r));
              ++pixel_pending_count;
            }
            else notes.insert("Unbridged pixel sample capacity reached; no native transfer inferred from proximity.");
            return;
          }
          source=bridge->first;
          const auto&forwarded=bridge->second;
          auto declared=r.get("@workspace_source");
          Strings fields=source_probe?words("workspace_allocation timeline_ready"):words("generation slot frame_id session_epoch layer_generation content_width content_height");
          if(direct_probe) {
            auto more=words("layer content_session content_sequence timeline_ready timeline_release");
            fields.insert(fields.end(),more.begin(),more.end());
            fields.insert(fields.end(),DIRECT_ALLOCATION_FIELDS.begin(),DIRECT_ALLOCATION_FIELDS.end());
          }
          std::string conflict=!missing(declared)&&!exact(declared,source)?"@workspace_source":"";
          for(auto&field:fields) {
            auto stated=r.get(field),acquired=forwarded.get(field);
            if(!missing(stated)&&!missing(acquired)&&(!exact(stated,acquired)||(source_probe&&!same_type(stated, acquired)))) {
              conflict=field;
              break;
            }
          }
          if(!conflict.empty()) {
            pixel_divergence(optional_kind.empty()?"pixel-transfer-conflict":"pixel-"+optional_kind+"-transfer-conflict","pixel sample conflicts with its exact forwarded transfer ("+conflict+")",r,forwarded,publication,emit,optional_stage.empty()?MISSING:Json(optional_stage+"-bridge"));
            return;
          }
          if(direct_probe&&!exact(forwarded.get("direct_sampling"),true)) {
            notes.insert("Optional "+optional_stage+" probe has no exact direct-mode acquisition; bytes were not compared.");
            return;
          }
        }
        auto parsed=pixel_sample(r,source);
        if(!parsed)return;
        auto[key,stage,sample]=*parsed;
        Json publication=Json::array( {
          key[0],key[1],key[2]
        });
        auto k=encoded(key),sk=encoded(stage);
        if(!pixel_samples.contains(k)) {
          if(pixel_samples.size()==limits.pixel_samples) {
            notes.insert("Pixel-chain sample capacity reached; additional publications were not compared.");
            return;
          }
          pixel_samples.emplace(k,PixelChainSample {
          });
        }
        auto&chain=pixel_samples.at(k);
        auto&stages=chain.stages;
        auto previous=stages.find(sk);
        if (observing("pixel.stage")) observe("pixel.stage", 1);
        if(previous==stages.end()) {
          if(pixel_stage_count>=limits.pixel_samples*4) {
            notes.insert("Pixel-chain stage capacity reached; additional physical transfers were not compared.");
            return;
          }
          ++pixel_stage_count;
        }
        auto sample_text=[](const Json&s) {
          Strings cells;
          for(auto&v:s)cells.push_back(str(v));
          return "("+join(cells,", ")+")";
        };
        std::string coordinates="surface="+str(key[1])+" presentation_revision="+str(key[2])+" sample_index="+str(key[3]);
        if(previous!=stages.end()&&!exact(previous->second.sample,sample))pixel_divergence(optional_kind.empty()?"pixel-owner-mutation":"pixel-"+optional_kind+"-owner-mutation",str(stage[0])+" sample changed within "+coordinates+": "+sample_text(previous->second.sample)+" != "+sample_text(sample),r,previous->second.record,publication,emit,optional_stage.empty()?MISSING:Json(optional_stage+"-mutation"));
        else {
          if(previous==stages.end()&&equals(stage[0],"mailbox")) {
            ++chain.mailbox_count;
            chain.mailbox=stage;
          }
          stages.insert_or_assign(sk,PixelChainSample::Entry {
            sample,triage_snapshot(r)
          });
        }
        auto compare_edge=[&](const Json&left,const Json&right,const std::string&optional="") {
          auto a=stages.find(encoded(left)),b=stages.find(encoded(right));
          if (observing("pixel.stage")) observe("pixel.stage", 2);
          if(a==stages.end()||b==stages.end())return;
          auto&before=a->second;
          auto&after=b->second;
          Json edge=Json::array( {
            left[0],right[0]
          });
          if(optional=="source-copy") {
            if(!before.record.get("@workspace_source").is_string()||!after.record.get("@workspace_source").is_string()) {
              notes.insert("Optional source-copy comparison lacks an exact source identity; bytes were not compared.");
              return;
            }
            std::string conflict=(!exact(before.sample[0],after.sample[0])||!exact(before.sample[1],after.sample[1]))?"sample_x/sample_y":"";
            for(auto field: {
              "@workspace_source","transfer_sequence","workspace_allocation","timeline_ready"
            }) {
              auto bv=before.record.get(field),av=after.record.get(field);
              if(!missing(bv)&&!missing(av)&&(!same_type(bv, av)||!exact(bv,av))) {
                conflict=field;
                break;
              }
            }
            if(!conflict.empty()) {
              pixel_divergence("pixel-source-copy-transfer-conflict",str(left[0])+" -> "+str(right[0])+" identities or coordinates conflict ("+conflict+") for "+coordinates+"; bytes were not compared",after.record,before.record,publication,emit,edge);
              return;
            }
          }
          if(exact(before.sample,after.sample))return;
          pixel_divergence(optional.empty()?"pixel-chain-divergence":"pixel-"+optional+"-boundary-divergence",str(left[0])+" -> "+str(right[0])+" sample differs for "+coordinates+": "+sample_text(before.sample)+" != "+sample_text(after.sample),after.record,before.record,publication,emit,optional.empty()?MISSING:edge);
        };
        auto name=str(stage[0]);
        auto transfer=stage[1];
        auto st=[](std::string n,const Json&t) {
          return Json::array( {
            n,t
          });
        };
        for(auto[copied,kernel]: {
          std::pair {
            "source_copy_before_ready","native"
          }, {
            "source_copy_after_release","native_after_release"
          }
        }) {
          if(name==copied||name==kernel)compare_edge(st(kernel,transfer),st(copied,transfer),"source-copy");
          if(!chain.direct_transfer.is_null()) {
            if(name==copied||name=="direct_image")compare_edge(st(copied,chain.direct_transfer),st("direct_image",chain.direct_transfer),"source-copy");
            if(name==copied||name=="sample")compare_edge(st(copied,chain.direct_transfer),st("sample",nullptr),"source-copy");
          }
        }
        if(name=="source_copy_before_ready"||name=="source_copy_after_release")compare_edge(st("source_copy_before_ready",transfer),st("source_copy_after_release",transfer),"source-copy");
        if(!chain.direct_transfer.is_null()) {
          if(name=="import"||name=="mailbox")pixel_divergence("pixel-mode-conflict","direct sampling contains a forbidden sample-arena copy receipt",r, {
          },publication,emit);
          else if(name=="native"||name=="sample")compare_edge(st("native",chain.direct_transfer),st("sample",nullptr));
          if(name=="native"||name=="direct_image")compare_edge(st("native",chain.direct_transfer),st("direct_image",chain.direct_transfer),"direct");
          if(name=="direct_image"||name=="sample")compare_edge(st("direct_image",chain.direct_transfer),st("sample",nullptr),"direct");
          return;
        }
        if(name=="native"||name=="import")compare_edge(st("native",transfer),st("import",transfer));
        if(name=="import"||name=="mailbox")compare_edge(st("import",transfer),st("mailbox",transfer));
        if(chain.mailbox_count==1&&(name=="mailbox"||name=="sample"))compare_edge(chain.mailbox,st("sample",nullptr));
      }
      void finalize_pixel_chain(bool emit) {
        std::string kind=pixel_sources_truncated?"pixel-transfer-incomplete":"pixel-transfer-missing",reason=pixel_sources_truncated?"exact frame_forwarded correlation incomplete because bridge index was truncated":"no exact frame_forwarded bridge in scanned scope";
        for(auto&[k,all]:pixel_pending) {
          std::vector<Record>pending_rows;
          for(auto&r:all) {
            auto e=textget(r,"@event");
            auto native=NATIVE_PIXEL_STAGES.find(e);
            if(!DIRECT_PIXEL_STAGES.contains(e)&&(native==NATIVE_PIXEL_STAGES.end()||native->second=="native"))pending_rows.push_back(r);
          }
          if(pending_rows.empty()) {
            notes.insert("Optional pixel probe has no exact frame_forwarded bridge; bytes were not compared.");
            continue;
          }
          auto key=parse_json(k);
          auto r=*std::min_element(pending_rows.begin(),pending_rows.end(),physical_position_less);
          auto message=reason+": surface="+str(key[1])+" presentation_revision="+str(key[2])+" transfer_sequence="+str(key[3])+" run="+str(key[0])+"; "+std::to_string(pending_rows.size())+" pixel receipt(s) have no proven native source";
          if(emit)finding(kind,115,message,r);
          else seed_pool.add(r,concatenate(Json::array( {
            -120,false,number(-mtime(r))
          }),physical_position(r)),kind+": "+message,concatenate(Json::array( {
            kind
          }),key));
        }
        pixel_pending.clear();
        pixel_pending_count=0;
      }
      void choose_anchors(const std::vector<LogFile>&files,const Expression&query) {
        bool explicit_query=!trim(options.query).empty()||options.errors||lookup;
        Json peer_scope=MISSING,stage_scope=MISSING;
        std::optional<std::pair<Big,Big>>peer_opened;
        std::optional<Big>last_peer_close;
        int short_peers=0;
        std::map<Strings,Record>workspace_waits;
        auto retain_pending=[&]() {
          for(auto&[k,state]:pending) {
            if(!state.first)continue;
            auto&row=*state.first;
            auto span=row.get("span_id");
            bool exact_span=integer(span)&&bigint(span)>0&&exact(row.get("span_outcome"),0);
            std::string why=exact_span?"unclosed instrumented span at capture end (candidate, not proof)":"unclosed start at capture end (candidate, not proof)";
            seed_pool.add(row,concatenate(Json::array( {
              exact_span?-80:-30,false,number(-mtime(row))
            }),physical_position(row)),why,Json::array( {
              row.metadata["run"],textget(row,"@event"),"unclosed"
            }));
          }
          for(auto&[k,row]:workspace_waits)seed_pool.add(row,concatenate(Json::array( {
            -85,false,number(-mtime(row))
          }),physical_position(row)),"workspace wait without a captured frame edge: "+textget(row,"message"),Json::array( {
            row.metadata["run"],row.source,"workspace-wait"
          }));
          pending.clear();
          workspace_waits.clear();
        };
        records(files,[&](Record row) {
          ++scanned;
          Json scope=Json::array( {
            row.source,row.metadata["run"]
          });
          if(!exact(scope,stage_scope)) {
            retain_pending();
            stage_scope=scope;
          }
          collect_parse_error(row,malformed,parse_examples);
          if(!where.matches(row)||!query.matches(row))return;
          scope.push_back(row.clock);
          if(!exact(scope,peer_scope)) {
            peer_scope=scope;
            peer_opened.reset();
            last_peer_close.reset();
            short_peers=0;
          }
          auto event=row.get("@event");
          if(!meta(row,"context_copy")) {
            auto product=correlation_key(row, {
              "source_session","source_instance","source_revision"
            });
            if(product) {
              if(equals(event,"presentation.frame.edge"))workspace_waits.erase(*product);
              else if(equals(event,"presentation.workspace.wait")&&equals(row.get("message"),"producer_finalization_pending")) {
                if(workspace_waits.contains(*product)||workspace_waits.size()<MAX_TRIAGE_STATES)workspace_waits.insert_or_assign(*product,triage_snapshot(row));
              }
            }
          }
          if(!meta(row,"context_copy")&&in(event,words("browser.server.peer_opened browser.server.peer_closed browser.server.peer_replaced"))) {
            auto peer=row.get("value");
            if(equals(event,"browser.server.peer_opened")&&integer(peer)&&bigint(peer)>0&&row.time_ns)peer_opened=std::pair {
              bigint(peer),*row.time_ns
            };
            else if(equals(event,"browser.server.peer_closed")&&peer_opened&&row.time_ns) {
              bool same=integer(peer)&&bigint(peer)==peer_opened->first;
              Big lifetime=*row.time_ns-peer_opened->second;
              bool short_peer=same&&lifetime>=0&&lifetime<=100000000;
              bool consecutive=last_peer_close&&*row.time_ns-*last_peer_close>=0&&*row.time_ns-*last_peer_close<=1000000000;
              short_peers=short_peer&&consecutive?std::min(short_peers+1,4):int(short_peer);
              last_peer_close=row.time_ns;
              peer_opened.reset();
              if(short_peers==3) {
                std::string why="rapid reconnects: three peers closed within 100ms each, at intervals under 1s; cause not recorded by these events";
                seed_pool.add(row,concatenate(Json::array( {
                  -95,false,number(-mtime(row))
                }),physical_position(row)),why,Json::array( {
                  row.metadata["run"],row.source,"rapid-reconnects"
                }));
              }
            }
            else {
              peer_opened.reset();
              last_peer_close.reset();
              short_peers=0;
            }
          }
          observe_pixel_chain(row,false);
          auto ids=strong_identities(row,explicit_fields);
          auto[score,why]=anchor_rank(row,ids);
          if(!score&&explicit_query) {
            score=40;
            why="query-selected anchor";
          }
          if(score)seed_pool.add(row,concatenate(Json::array( {
            -score,meta(row,"context_copy"),number(-mtime(row))
          }),physical_position(row)),why,Json::array( {
            row.metadata["run"],textget(row,"@event"),why
          }));
          auto[family,stage]=event_stage(row);
          if(!ids.empty()&&!family.empty()&&!meta(row,"context_copy")) {
            auto id=lifecycle_identity(ids);
            auto key=encoded(Json::array( {
              row.source,identity_json(*id),family
            }));
            if(mmltk::tools::includes(START_STAGES,stage)) {
              if(pending.contains(key)||pending.size()<MAX_TRIAGE_STATES)pending[key].begin(row,stage);
              else notes.insert("Open-stage anchor inventory capped; narrow --where to inspect other identities.");
            }
            else if(!stage.empty()&&!mmltk::tools::includes(INCOMPLETE_STAGES,stage)) {
              auto i=pending.find(key);
              if(i!=pending.end()&&!--i->second.pending)pending.erase(i);
            }
          }
        },false);
        if(!explicit_query)finalize_pixel_chain(false);
        retain_pending();
        auto candidates=seed_pool.rows();
        pixel_samples.clear();
        pixel_sources.clear();
        pixel_sources_truncated=false;
        pixel_pending.clear();
        pixel_pending_count=0;
        pixel_stage_count=0;
        pixel_divergences.clear();
        if(candidates.empty())return;
        run=textget(lookup?*lookup->focus:candidates[0].record,"@run");
        if(lookup) {
          auto focus=triage_snapshot(*lookup->focus);
          selected.push_back( {
            concatenate(Json::array( {
              -1000
            }),physical_position(focus)),focus,"first physical pasted-error match",0, {
            }
          });
          return;
        }
        for(auto&entry:candidates) {
          if(selected.size()>=static_cast<std::size_t>(options.triage_anchors))break;
          if(equals(entry.record.metadata["run"],run)&&std::none_of(selected.begin(),selected.end(),[&](auto&x) {
            return exact(physical_position(x.record),physical_position(entry.record));
          }))selected.push_back(entry);
        }
      }
      std::optional<std::pair<Big,std::string>>nearby(Record&row) {
        if(lookup&&lookup->annotate(row))return std::pair {
          absbig(bigint(row.metadata["proximity_ns"])),str(row.metadata["time_link"])
        };
        std::optional<std::pair<Big,std::string>>best;
        auto choose=[&](std::pair<Big,std::string>candidate) {
          if(!best||candidate<*best)best=std::move(candidate);
        };
        for(auto&entry:selected) {
          const auto&anchor=entry.record;
          if(row.source==anchor.source) {
            Big delta=absbig(Big(row.line)-anchor.line);
            if(delta<=std::max(3,options.context))choose( {
              delta,"same-source line context (not identity)"
            });
          }
          auto [clock, time] = record_timestamp(row);
          auto [anchor_clock, anchor_time] = record_timestamp(anchor);
          const bool carried = !row.time_ns || !anchor.time_ns;
          if(clock==anchor_clock&&time&&anchor_time) {
            Big delta=absbig(*time-*anchor_time);
            if(delta<=Big(options.near_ms)*1000000)choose( {
              10+delta,std::string(carried?"preceding-source timestamp proximity":"same-clock proximity")+" (not identity)"
            });
          }
        }
        return best;
      }
      void add_identity(const Identity&id,const Record&origin,const std::string&why,int hop) {
        if(identities.contains(id))return;
        if(identities.size()==static_cast<std::size_t>(options.triage_identities)) {
          notes.insert("Identity budget reached; additional identities were not expanded.");
          return;
        }
        identities.try_emplace(id,id,triage_snapshot(origin),why,hop);
        identity_order.push_back(id);
      }
      void gather_context(const std::vector<LogFile>&files) {
        for(auto&entry:selected) {
          auto&row=entry.record;
          auto why=str(entry.why);
          if(why.starts_with("rapid reconnects:"))finding("rapid-reconnects",108,why,row);
          if(why.starts_with("workspace wait without"))finding("workspace-wait",108,why,row);
          if(anchor_rank(row).first>=85&&(truth(row.get("@error"))||why=="failed outcome/span"))finding("anchor-failure",115,why,row);
          for(auto&id:strong_identities(row,explicit_fields))add_identity(id,row,"anchor: "+why,0);
        }
        records(files,[&](Record row) {
          ++scoped;
          auto[score,why]=anchor_rank(row);
          source_tails.add(row,Json::array( {
            row.source,number(-Big(row.line)),number(-Big(part(row)))
          }),"captured source end (not a terminal unless logged)",row.source);
          if(triage_terminal(row))terminal_pool.add(row,concatenate(Json::array( {
            meta(row,"context_copy")
          }),physical_position(row)),truth(row.get("@terminal"))?"process/test terminal evidence":"generic terminal stage (not a process/test exit; numeric outcomes uninterpreted)",Json::array( {
            textget(row,"@event"),textget(row,"@exit_code")
          }));
          if(score>=90||triage_terminal(row))failure_pool.add(row,concatenate(Json::array( {
            -score,meta(row,"context_copy")
          }),physical_position(row)),"same-run terminal/failure evidence (not identity)",Json::array( {
            textget(row,"@event"),why
          }));
          auto neighborhood=nearby(row);
          if(!neighborhood)return;
          auto event=row.get("@event"),detail=value_from(row.data, {
            "detail","message","fields.detail","fields.message"
          });
          Json signature=Json::array( {
            row.source,str(event),prefix(str(detail),100)
          });
          bool conversational=event.is_string()&&search(HANDOFF_EVENT,str(event));
          auto rank=concatenate(Json::array( {
            meta(row,"context_copy"),lookup&&row.source!=lookup->focus->source,missing(event),!conversational,number(neighborhood->first)
          }),physical_position(row));
          context_pool.add(row,rank,neighborhood->second,signature);
        });
        for(auto&entry:context_pool.rows())for(auto&id:strong_identities(entry.record,explicit_fields))add_identity(id,entry.record,"neighbor seed: "+str(entry.why),0);
      }
      void add_refinement(const Record&row,const Identity&id) {
        Strings names,values;
        for(auto&name:REFINEMENT_FIELDS) {
          auto v=row.get(name);
          if(integer(v)&&bigint(v)>=0&&(name!="sequence"||bigint(v)>0)) {
            names.push_back(name);
            values.push_back(encoded(v));
          }
        }
        if(names.size()<2)return;
        Identity key {
          run,names,values
        };
        if(!refinements.contains(key)) {
          if(refinements.size()==static_cast<std::size_t>(options.triage_identities)) {
            notes.insert("Counter refinement budget reached; additional composites omitted.");
            return;
          }
          refinements[key]= {
          };
          refinement_order.push_back(key);
          refinement_groups[names][values]=key;
        }
        auto&origins=refinements[key];
        if(origins.size()<4&&std::none_of(origins.begin(),origins.end(),[&](auto&p) {
          return p.first.source==row.source;
        }))origins.emplace_back(triage_snapshot(row),id);
      }
      void expand(const std::vector<LogFile>&files) {
        if(identities.empty())return;
        for(int hop=0;hop<std::max(1,options.triage_hops);++hop) {
          std::set<Identity>known;
          for(auto&[id,chain]:identities)known.insert(id);
          TriagePool additions(options.triage_identities);
          records(files,[&](Record row) {
            auto ids=strong_identities(row,explicit_fields);
            std::vector<Identity>matched,unknown;
            for(auto&id:ids)(known.contains(id)?matched:unknown).push_back(id);
            if(matched.empty())return;
            add_refinement(row,matched[0]);
            if(hop>=options.triage_hops||unknown.empty())return;
            auto near=nearby(row);
            Json rank=concatenate(Json::array( {
              identities.at(matched[0]).hop,meta(row,"context_copy"),number(near?near->first:Big(1)<<63)
            }),physical_position(row));
            for(auto&id:unknown)additions.add(row,concatenate(rank,Json::array( {
              id.label()
            })),identity_json(id),identity_json(id));
          });
          for(auto&e:additions.rows())add_identity(identity_from(e.why),e.record,"co-occurs on an exact identity record (handoff candidate)",hop+1);
          if(identities.size()==known.size())break;
        }
      }
      std::optional<std::tuple<Identity,Identity,std::string>>counter_neighbor(const Record&row)const {
        if(refinement_groups.empty())return {
        };
        std::map<std::string,std::optional<std::string>>values;
        for(auto&name:REFINEMENT_FIELDS) {
          auto v=row.get(name);
          values[name]=missing(v)?std::nullopt:std::optional<std::string>(encoded(v));
        }
        std::set<Identity>matches;
        for(auto&[names,group]:refinement_groups) {
          Strings key;
          bool valid=true;
          for(auto&n:names) {
            if(!values[n]) {
              valid=false;
              break;
            }
            key.push_back(*values[n]);
          }
          if(valid) {
            auto i=group.find(key);
            if(i!=group.end())matches.insert(i->second);
          }
        }
        for(auto&id:refinement_order)if(matches.contains(id))for(auto&[origin,strong]:refinements.at(id)) {
          bool line_near=row.source==origin.source&&absbig(Big(row.line)-origin.line)<=100;
          bool time_near=row.clock!="none"&&row.clock==origin.clock&&row.time_ns&&origin.time_ns&&absbig(*row.time_ns-*origin.time_ns)<=Big(options.near_ms)*1000000;
          if(line_near||time_near)return std::tuple {
            id,strong,line_near?"line":"same-clock"
          };
        }
        return {
        };
      }
      void retain(const Record&row,double priority,const std::string&why) {
        evidence.add(row,concatenate(Json::array( {
          priority
        }),order_key(row,options.order)),why);
      }
      void analyze(const std::vector<LogFile>&files) {
        TriagePool counter_evidence(4);
        records(files,[&](Record row) {
          observe_pixel_chain(row,true);
          auto ids=strong_identities(row,explicit_fields);
          std::vector<Identity>matched;
          for(auto&id:ids)if(identities.contains(id))matched.push_back(id);
          if(!matched.empty()) {
            ++related;
            auto[family,stage]=event_stage(row);
            if(!family.empty()&&!mmltk::tools::includes(START_STAGES,stage)&&!mmltk::tools::includes(INCOMPLETE_STAGES,stage)&&!meta(row,"context_copy")) {
              auto snap=triage_snapshot(row);
              for(auto&id:matched) {
                auto key=encoded(Json::array( {
                  row.source,family,identity_json(id)
                }));
                if(observed_ends.contains(key)||observed_ends.size()<MAX_TRIAGE_STATES)observed_ends.insert_or_assign(key,snap);
                else notes.insert("Related end-stage inventory capped; counterpart absence may be incomplete.");
              }
            }
            auto primary=lifecycle_identity(ids);
            for(auto&id:matched)identities.at(id).observe(row,*this,primary&&id==*primary);
            auto[score,why]=anchor_rank(row);
            if(score>=85) {
              auto span=std::find_if(matched.begin(),matched.end(),[](const auto&id) {
                return id.names==Strings {
                  "span_id"
                };
              });
              std::string kind=why=="failed outcome/span"&&span!=matched.end()?"failed-span":truth(row.get("@error"))||why=="failed outcome/span"?"explicit-failure":"incomplete-state";
              finding(kind,score,why,row,kind=="failed-span"&&span!=matched.end()?*span:matched[0]);
            }
            retain(row,4,"exact identity: "+matched[0].label());
          }
          else if(auto refinement=counter_neighbor(row)) {
            auto[id,strong,link]=*refinement;
            auto why="counter refinement "+id.label()+" + "+link+" proximity to "+strong.label()+" (not identity)";
            counter_evidence.add(row,physical_position(row),why,Json::array( {
              row.source,textget(row,"@event")
            }));
          }
        });
        finalize_pixel_chain(true);
        for(auto&id:identity_order) {
          auto&chain=identities.at(id);
          chain.finish(*this);
          if(chain.hop)finding("identity-handoff",25,chain.why,chain.origin,chain.identity);
        }
        for(std::size_t index=0;index<identity_order.size();++index) {
          auto&chain=identities.at(identity_order[index]);
          double fraction=static_cast<double>(index)/std::max<std::size_t>(1,identities.size());
          for(auto*map: {
            &chain.bounds,&chain.last_by_source
          })for(auto&[source,row]:*map)retain(row,3+fraction,"identity lifecycle endpoint: "+chain.identity.label());
          for(auto&e:chain.representatives.rows())retain(e.record,4+fraction,"identity stage: "+chain.identity.label());
        }
        auto found=sorted_findings();
        for(auto&f:found)if(f.score>=50) {
          retain(f.record,1+(110-f.score)/100.0,"finding: "+f.kind);
          if(f.other)retain(*f.other,2.8,"finding context: "+f.kind);
        }
        int slots=0;
        std::set<std::string>anchor_positions;
        for(auto&e:selected)anchor_positions.insert(encoded(physical_position(e.record)));
        for(auto&e:context_pool.rows()) {
          bool redundant=anchor_positions.contains(encoded(physical_position(e.record)))||triage_terminal(e.record);
          int budget=lookup?std::min(12,std::max(1,2*options.limit/3)):std::min(6,std::max(1,options.limit/3));
          double priority=!redundant&&slots<budget?0.8:2.7;
          slots+=!redundant;
          retain(e.record,priority,str(e.why));
        }
        for(auto&e:counter_evidence.rows())retain(e.record,2.6,str(e.why));
        for(auto&e:failure_pool.rows())retain(e.record,1.8,str(e.why));
        for(auto&e:terminal_pool.rows())retain(e.record,0.7,str(e.why));
        for(auto&e:selected)retain(e.record,0,"anchor: "+str(e.why));
      }
      std::vector<TriageFinding>sorted_findings()const {
        std::vector<TriageFinding>out;
        for(auto&[k,f]:findings)out.push_back(f);
        std::sort(out.begin(),out.end(),[](auto&a,auto&b) {
          if(a.score!=b.score)return a.score>b.score;
          int c=compare(physical_position(a.record),physical_position(b.record));
          return c?c<0:a.kind<b.kind;
        });
        return out;
      }
      Triage&run_query(std::vector<LogFile>files,const Expression&query) {
        choose_anchors(files,query);
        if(selected.empty())return *this;
        std::erase_if(files,[&](const LogFile&f) {
          return !equals(f.metadata["run"],run)&&!mmltk::tools::includes(f.linked_runs,run)&&std::none_of(selected.begin(),selected.end(),[&](const auto&e) {
            return e.record.source==f.source;
          });
        });
        for(auto&f:files) {
          artifacts.push_back(f.source);
          artifact_discovery[f.source]=str(get(f.metadata,"discovery",""));
        }
        gather_context(files);
        expand(files);
        analyze(files);
        if(identities.empty())notes.insert("No non-empty strong identity near the anchors; evidence is context/proximity only.");
        if(seed_pool.discarded||context_pool.discarded)notes.insert("Anchor/context samples are bounded and diversified; repeated or lower-ranked rows omitted.");
        return *this;
      }
    };
    void IdentityChain::observe(const Record&r,Triage&t,bool balance) {
      ++count;
      auto ev=r.get("@event"),ow=r.get("@owner");
      auto event=missing(ev)?"(text)":str(ev),owner=missing(ow)?"(unknown)":str(ow);
      for(auto pair: {
        std::pair {
          &events,event
        },std::pair {
          &owners,owner
        }
      }) {
        if(pair.first->contains(pair.second)||pair.first->size()<MAX_TRIAGE_EVENTS) {
          if (!pair.first->contains(pair.second)) (pair.first==&events?event_order:owner_order).push_back(pair.second);
          ++(*pair.first)[pair.second];
        } else limited=true;
      }
      auto[score,unused]=anchor_rank(r);
      representatives.add(r,concatenate(Json::array( {
        -score
      }),physical_position(r)),"identity chain",Json::array( {
        r.source,event
      }));
      if(meta(r,"context_copy")) {
        ++copies;
        return;
      }
      auto row=triage_snapshot(r);
      auto previous=last_by_source.find(r.source);
      if(previous!=last_by_source.end()) {
        auto&p=previous->second;
        if(!missing(p.get("@owner"))&&!missing(r.get("@owner"))&&!exact(p.get("@owner"),r.get("@owner")))t.finding("owner-handoff",25,"same identity appears under another owner; handoff candidate, not proof",row,identity,p);
        if(p.clock==r.clock&&r.time_ns&&p.time_ns) {
          Big delta=*r.time_ns-*p.time_ns;
          if(delta<0)t.finding("clock-regression",65,"timestamp decreases in physical source order; no stall inferred",row,identity,p);
          else if(delta>=Big(t.options.triage_gap_ms)*1000000) {
            auto current=gaps.find(r.source);
            if(current==gaps.end()||delta>std::get<0>(current->second))gaps.insert_or_assign(r.source,std::tuple {
              delta,p,row
            });
          }
        }
      }
      if(last_by_source.size()<32||last_by_source.contains(r.source)) {
        last_by_source.insert_or_assign(r.source,row);
        bounds.try_emplace(r.source,row);
      }
      else limited=true;
      if(!balance)return;
      auto[family,stage]=event_stage(r);
      if(family.empty()||mmltk::tools::includes(INCOMPLETE_STAGES,stage)) {
        if(mmltk::tools::includes(INCOMPLETE_STAGES,stage))t.finding("incomplete-state",85,"explicit "+stage+" stage; later recovery is possible",row,identity);
        return;
      }
      auto key=std::pair {
        r.source,family
      };
      if(!states.contains(key)) {
        if(states.size()==MAX_TRIAGE_EVENTS) {
          limited=true;
          return;
        }
        states[key]= {
        };
      }
      auto&state=states[key];
      if(mmltk::tools::includes(START_STAGES,stage))state.begin(row,stage);
      else {
        if(state.pending) {
          --state.pending;
          if(!state.pending)state.first.reset();
        }
        else if(state.last_end&&equals(state.last_end->get("@event"),event))t.finding("duplicate-terminal",55,"repeated "+event+" without an intervening "+family+" start in this source; may be idempotent",row,identity,state.last_end);
        else if(!state.last_end)t.finding("unmatched-end",20,family+" end has no captured same-source start (partial capture or another source possible)",row,identity);
        state.last_end=row;
        if(mmltk::tools::includes(FAILED_STAGES,stage)&&identity.names==Strings {
          "span_id"
        })t.finding("failed-span",100,"span has explicit "+stage+" stage",row,identity);
      }
    }
    void IdentityChain::finish(Triage&t) {
      for(auto&[key,state]:states)if(state.pending&&state.first) {
        std::vector<Record>counterparts;
        for(auto&shared:strong_identities(*state.first,t.explicit_fields)) {
          auto i=t.observed_ends.find(encoded(Json::array( {
            key.first,key.second,identity_json(shared)
          })));
          if(i==t.observed_ends.end()||i->second.line<state.first->line)continue;
          auto ids=strong_identities(i->second,t.explicit_fields);
          auto primary=lifecycle_identity(ids);
          if(primary&&*primary==identity)continue;
          if(std::any_of(ids.begin(),ids.end(),[&](auto&id) {
            return id.names==identity.names&&id!=identity;
          }))continue;
          counterparts.push_back(i->second);
        }
        if(!counterparts.empty()) {
          auto other=*std::min_element(counterparts.begin(),counterparts.end(),physical_position_less);
          t.finding("identity-handoff",45,key.second+": an end shares a recorded identity but has another primary identity; handoff/instrumentation mismatch possible",*state.first,identity,other);
        }
        else t.finding("missing-counterpart",70,key.second+": "+std::to_string(state.pending)+" start(s) without a captured same-source end/completion; suffix convention only, not a proven product requirement",*state.first,identity);
      }
      for(auto&[source,gap]:gaps) {
        auto&[delta,before,after]=gap;
        t.finding("gap",50,fixed_ms(delta)+"ms without this identity in the same source/clock; a gap is not proof of a stall",after,identity,before);
      }
      if(limited)t.notes.insert("Lifecycle event/source capacity reached; absence findings describe only tracked families.");
    }
  }
  std::string compact(const Json&v,std::optional<std::size_t>width) {
    if(missing(v))return "(not recorded)";
    auto text=v.is_string()?str(v):encoded(v);
    auto u=icu::UnicodeString::fromUTF8(icu::StringPiece(text.data(),static_cast<int32_t>(text.size())));
    icu::UnicodeString cleaned;
    for(int32_t i=0;i<u.length();) {
      auto c=u.char32At(i);
      i+=U16_LENGTH(c);
      std::string escaped;
      if(c=='\t')escaped="\\t";
      else if(c=='\r')escaped="\\r";
      else if(c=='\n')escaped="\\n";
      else if(c!=0x20&&!u_isprint(c)) {
        std::ostringstream s;
        s<<"\\x"<<std::hex<<std::setfill('0')<<std::setw(2)<<static_cast<std::uint32_t>(c);
        escaped=s.str();
      }
      else {
        cleaned.append(c);
        continue;
      }
      cleaned.append(icu::UnicodeString::fromUTF8(escaped));
    }
    std::string out;
    cleaned.toUTF8String(out);
    if(width&&chars(out)>*width)return prefix(out,*width?*width-1:0)+"…";
    return out;
  }
  Json projected(const Record&r,const Strings&names) {
    Json out= {
      {
        "@file",r.source
      }, {
        "@line",r.line
      }
    };
    for(auto name: {
      "line_end","representative_count"
    })if(r.metadata.contains(name))out["@"+std::string(name)]=r.metadata[name];
    if(part(r))out["@part"]=part(r);
    for(auto&name:names) {
      auto v=r.get(name);
      out[name]=missing(v)?Json(nullptr):v;
    }
    return out;
  }
  std::string render_record(const Retained&item,const Options&o) {
    const auto&r=item.record;
    if(o.format=="jsonl") {
      Json out;
      if(!o.fields.empty()) {
        out=projected(r,o.fields);
        out["@match"]=item.reason;
        if(item.reason=="auto") {
          out["@auto_reason"]=r.metadata["auto_reason"];
          out["@auto_anchor"]=r.metadata["auto_anchor"];
        }
      }
      else {
        Json log= {
          {
            "file",r.source
          }, {
            "line",r.line
          }, {
            "format",r.format
          }, {
            "clock",r.clock
          }, {
            "time_ns",r.time_ns?number(*r.time_ns):Json(nullptr)
          }, {
            "match",item.reason
          }, {
            "parse_error",r.parse_error.empty()?Json(nullptr):Json(r.parse_error)
          }
        };
        merge(log,r.metadata);
        out= {
          {
            "_log",log
          }, {
            "data",r.data
          }, {
            "text",r.raw
          }
        };
      }
      return encoded(out);
    }
    std::string reason=item.reason=="auto"?"auto: "+str(r.metadata["auto_reason"]):item.reason;
    std::size_t detail_width=exact(r.get("@error"),true)?2048:180;
    auto event=r.get("@event");
    bool vulkan=equals(event,"vulkan.validation");
    if(!o.fields.empty()) {
      auto cells=projected(r,o.fields);
      Strings s;
      for(auto it=cells.begin();it!=cells.end();++it)s.push_back(it.key()+"="+compact(it.value(),vulkan?std::nullopt:std::optional<std::size_t>(detail_width)));
      return join(s," ")+" ["+reason+"]";
    }
    auto clock=r.clock.starts_with("elapsed:")?"elapsed":r.clock;
    auto timestamp=clock=="elapsed"?fixed_ms(ns(r))+"ms":r.time_ns?r.time_ns->convert_to<std::string>():"-";
    Strings labels;
    for(auto v: {
      r.get("@owner"),r.get("@level"),event
    })if(!missing(v))labels.push_back(str(v));
    auto detail=value_from(r.data, {
      "message","fields.message","detail"
    });
    if(missing(detail)&&r.training_record())detail=r.get("progress.failure.detail");
    if(missing(detail)&&missing(event))detail=r.raw;
    Strings facts;
    auto append=[&](const Strings&names,std::optional<std::size_t>width) {
      for(auto&name:names) {
        auto v=r.get(name);
        if(!missing(v))facts.push_back(name+"="+compact(v,width));
      }
    };
    if(r.training_record())for(auto name: {
      "run_id","attempt_id","progress.session_id","progress.model_id","progress.failure.first_cause"
    })facts.push_back(std::string(name)+"="+compact(r.get(name),70));
    if(r.metadata.contains("representative_count"))facts.push_back("occurrences="+str(r.metadata["representative_count"]));
    if(vulkan)append(words("validation_id message_id api objects referenced_objects"),std::nullopt);
    for(auto name:words("trace_id span_id @surface @workspace_source source_session source_instance source_revision frame_revision presentation_revision allocation_generation workspace_allocation workspace_plane"))if(correlation_key(r, {
      name
    }))facts.push_back(name+"="+compact(r.get(name),70));
    append(words("participant reason observed_event observed_file observed_line audit_context.field"),150);
    auto es=str(event);
    if(NATIVE_PIXEL_STAGES.contains(es)||DIRECT_PIXEL_STAGES.contains(es)||mmltk::tools::includes(words("firefox.workspace.direct_memory_alias firefox.workspace.direct_memory_status firefox.workspace.direct_memory_pixel"),es)) {
      append(words("transfer_sequence timeline_ready memory_alias_supported sample_index sample_x sample_y sample_byte_offset sample_rgba completion readback_marker"),70);
      if(es=="firefox.workspace.direct_memory_pixel")facts.push_back("pixel_evidence=undefined_alias_after_image_acquire");
    }
    if(in(event,MEMORY_PROVENANCE_EVENTS)||es=="firefox.workspace.direct_binding") {
      append(MEMORY_PROVENANCE_FIELDS,100);
      if(es=="firefox.workspace.direct_binding")append(words("phase storage_image chosen_layout transfer_sequence texture_id"),100);
    }
    if(in(event,LIBRARY_PROVENANCE_EVENTS)) {
      for(auto name:words("native_process_id browser_process_id library_path library_provenance inventory_status inventory_errno library_count maps_bytes maps_byte_limit inventory_bounded path_encoding_unavailable"))append( {
        name
      },name=="library_path"?std::nullopt:std::optional<std::size_t>(100));
    }
    for(auto name: {
      "@signal","@exit_code","@test","buffer"
    }) {
      if(std::string_view(name)=="@test"&&o.error)continue;
      append( {
        name
      },90);
    }
    if(event.is_string()&&search(HANDOFF_EVENT,es))append(words("sequence value"),70);
    if(es=="presentation.workspace.wait"||es=="presentation.workspace.service")append(words("requested_product_owner requested_product_revision observed_product_owner observed_product_revision admitted_allocation admitted_access admitted_generation admitted_display_held admitted_write_reserved admitted_completion_pending source_timeline_imported workspace_admitted workspace_write_available"),70);
    if(o.triage) {
      append(words("generation slot boundary status code outcome span_outcome"),70);
      if(triage_terminal(r)) {
        auto fields=get(r.data,"fields");
        const auto&values=fields.is_object()?fields:r.data;
        int emitted=0;
        for(auto it=values.begin();it!=values.end();++it) {
          const auto&value=it.value();
          if(mmltk::tools::includes(words("event name owner level timestamp steady_ns elapsed_ms status code outcome span_outcome"),it.key())||value.is_null()||equals(value,"")||exact(value,0)||exact(value,false)||!(value.is_string()||numeric(value)||value.is_boolean()))continue;
          facts.push_back(compact(it.key(),70)+"="+compact(value,70));
          if(++emitted==6)break;
        }
      }
    }
    append(words("strong_count settled"),70);
    if(meta(r,"context_copy"))facts.push_back("context-copy");
    if(r.metadata.contains("proximity_ns"))facts.push_back("delta_ms="+fixed_ms(bigint(r.metadata["proximity_ns"]),true)+" ("+str(r.metadata["time_link"])+")");
    std::string suffix=!missing(detail)&&!equals(detail,"")&&!vulkan?" "+compact(detail,detail_width):"";
    if(!facts.empty())suffix+=" "+join(facts," ");
    auto location=std::to_string(r.line);
    if(r.metadata.contains("line_end"))location+="-"+str(r.metadata["line_end"]);
    auto rendered=compact(clock,70)+":"+timestamp+" "+compact(r.source,150)+":"+location+" ["+reason+"] "+compact(join(labels," "))+suffix;
    if(vulkan) {
      for(auto&line:tools::lines(r.raw))rendered+="\n    "+compact(line,std::nullopt);
    }
    return rendered;
  }
  namespace detail {
    void render_descriptor_lineage(const QueryResult&result,const Options&o,std::ostream&out) {
      using Rows=std::vector<Record>;
      std::map<std::pair<std::string,std::string>,Rows>groups;
      std::map<std::pair<std::string,Strings>,Rows>exports,cuda,imports;
      std::map<std::pair<std::string,Strings>,std::set<std::string>>sources;
      auto index_row=[](auto&index,const auto&key,const Record&r) {
        auto&b=index[key];
        if(b.size()<2)b.push_back(r);
      };
      for(auto&item:result.retained) {
        auto&r=item.record;
        auto event=r.get("@event");
        if(!event.is_string()||!DESCRIPTOR_LINEAGE_STAGES.contains(str(event))||meta(r,"context_copy"))continue;
        auto e=str(event),run=textget(r,"@run");
        if(e.starts_with("cuda.workspace.")&&e!="cuda.workspace.memory_export") {
          if(auto key=correlation_key(r, {
            "native_process_id","workspace_allocation"
          }))imports[ {
            run,*key
          }].push_back(r);
        }
        else if(e=="gpu.workspace.memory_export") {
          if(auto key=correlation_key(r, {
            "native_process_id","workspace_allocation"
          }))index_row(exports,std::pair {
            run,*key
          },r);
        }
        else if(e=="cuda.workspace.memory_export") {
          if(auto key=correlation_key(r, {
            "native_process_id","workspace_descriptor","workspace_plane"
          }))index_row(cuda,std::pair {
            run,*key
          },r);
        }
        else {
          auto source=r.get("@workspace_source");
          if(source.is_string()) {
            groups[ {
              run,str(source)
            }].push_back(r);
            if(auto key=correlation_key(r, {
              "native_process_id","workspace_allocation"
            }))sources[ {
              run,*key
            }].insert(str(source));
          }
        }
      }
      out<<"Descriptor lineage: retained physical rows only; missing/ambiguous stages are not proof of a failed handoff.\n  Equal FD integers or fstat metadata never prove payload identity. Cross-process links below are SCM_RIGHTS record associations, not independent OFD/payload comparisons.\n";
      if(groups.empty()) {
        if(!imports.empty())out<<"  Missing native process/allocation source association; no CUDA import selected.\n";
        out<<"  No exact source chain retained; query workspace_allocation and include the native/browser artifact family.\n";
        return;
      }
      int displayed=0;
      for(auto&[key,original_rows]:groups) {
        if(displayed++==o.top)break;
        auto[run,source]=key;
        auto rows=original_rows;
        Strings issues,unavailable;
        auto values=[](const Rows&selected,const std::string&name) {
          std::set<std::string>found;
          for(auto&r:selected) {
            auto v=r.get(name);
            if(missing(v)||v.is_null())continue;
            if((name=="native_process_id"||name=="browser_process_id")&&(!integer(v)||bigint(v)<=0))continue;
            if(name=="dedicated"&&(v.is_boolean()||integer(v)))v=truth(v);
            found.insert(encoded(v));
            if(found.size()==3)break;
          }
          return found;
        };
        auto shown=[](const Record&r,const std::string&name) {
          auto v=r.get(name);
          return missing(v)?"not_recorded":compact(v,100);
        };
        auto compare_rows=[&](const Rows&selected,const Strings&names) {
          for(auto&name:names) {
            auto observed=values(selected,name);
            if(observed.size()>1)issues.push_back(name+" differs: "+compact(join(Strings(observed.begin(),observed.end()),","),200));
          }
        };
        compare_rows(rows,words("workspace_allocation @surface native_process_id browser_process_id"));
        bool modern=std::any_of(rows.begin(),rows.end(),[](const auto&r) {
          return in(r.get("@event"),words("firefox.workspace.memory_export firefox.workspace.memory_export_failed firefox.workspace.descriptor_send firefox.workspace.descriptor_send_result presentation.workspace.descriptor_received"))||!missing(r.get("vk_status"));
        });
        auto required=modern?words("allocation bound vulkan_export send_attempt send_result native_received cuda_import_started cuda_import cuda_map retirement_started retirement"):words("cuda_export workspace_export duplicated sent received claimed import allocation bound");
        auto native=values(rows,"native_process_id"),allocations=values(rows,"workspace_allocation");
        if(native.size()==1&&allocations.size()==1) {
          auto owner_key=std::pair {
            run,Strings {
              *native.begin(),*allocations.begin()
            }
          };
          if(modern) {
            auto count=sources[owner_key].size();
            if(count==1) {
              auto&more=imports[owner_key];
              rows.insert(rows.end(),more.begin(),more.end());
            }
            else unavailable.push_back(std::string(count==0?"missing":"ambiguous")+" native process/allocation source association; no CUDA import selected");
          }
          auto matches=modern?Rows {
          }
          :exports[owner_key];
          if(matches.size()==1) {
            auto exported=matches[0];
            rows.push_back(exported);
            auto lookup_key=correlation_key(exported, {
              "native_process_id","workspace_descriptor","workspace_plane"
            });
            auto mapped=lookup_key?cuda[ {
              run,*lookup_key
            }]:Rows {
            };
            if(mapped.size()==1)rows.push_back(mapped[0]);
            else if(!mapped.empty())unavailable.push_back("ambiguous CUDA export association; process/FD/address may have been reused");
          }
          else if(!matches.empty())unavailable.push_back("ambiguous workspace export association; no export selected");
        }
        else if(modern)unavailable.push_back(std::string(native.size()>1||allocations.size()>1?"ambiguous":"missing")+" native process/allocation source association; no CUDA import selected");
        std::map<std::string,Rows>stages;
        for(auto&r:rows)stages[DESCRIPTOR_LINEAGE_STAGES.at(textget(r,"@event"))].push_back(r);
        if(modern)for(auto stage: {
          "import_rejected","export_failed"
        })if(stages.contains(stage))required.push_back(stage);
        std::map<std::string,Record>single;
        for(auto&[stage,entries]:stages) {
          if(entries.size()==1)single.emplace(stage,entries[0]);
          else if(stage!="send_attempt"&&stage!="send_result")unavailable.push_back(stage+" has "+std::to_string(entries.size())+" retained records; no unique stage selected");
        }
        auto stage_rows=[&](const Strings&names) {
          Rows selected;
          for(auto&name:names) {
            auto i=single.find(name);
            if(i!=single.end())selected.push_back(i->second);
          }
          return selected;
        };
        auto send_success=[](const Record&r) {
          return integer(r.get("send_bytes"))&&exact(r.get("send_bytes"),r.get("record_bytes"))&&exact(r.get("send_errno"),0);
        };
        if(modern) {
          Rows results=stages.contains("send_result")?stages["send_result"]:Rows {
          },all=stages.contains("send_attempt")?stages["send_attempt"]:Rows {
          };
          all.insert(all.end(),results.begin(),results.end());
          compare_rows(all,words("workspace_descriptor native_process_id browser_process_id @workspace_source @surface workspace_allocation channel_descriptor descriptor_count memory_descriptor_index record_bytes descriptor_transport device_uuid device_incarnation memory_size image_offset row_pitch capacity_width capacity_height dedicated initialization"));
          Rows successful;
          for(auto&r:results)if(send_success(r))successful.push_back(r);
          if(successful.size()==1)single.insert_or_assign("send_result",successful[0]);
          else {
            single.erase("send_result");
            unavailable.push_back(successful.size()>1?"ambiguous successful SCM_RIGHTS sends":"SCM_RIGHTS send lacks an exact successful record-length result");
          }
          for(auto&r:results)if(!send_success(r)&&!exact(r.get("send_errno"),EINTR)&&!exact(r.get("send_errno"),EAGAIN))issues.push_back("SCM_RIGHTS send failed or returned a short record");
        }
        for(auto&[stage,row]:single) {
          if(stage.ends_with("_failed")||stage=="import_rejected")issues.push_back(stage+" reported by its owner");
          if(exact(row.get("cuda_mapping_matches_export"),false))issues.push_back("CUDA mapping differs from the exported allocation handle");
          for(auto[status,error]: {
            std::pair {
              "fd_getfd_result","fd_getfd_errno"
            }, {
              "fd_stat_status","fd_stat_errno"
            }
          }) {
            auto v=row.get(status);
            if(integer(v)&&bigint(v)<0)(exact(row.get(error),EBADF)?issues:unavailable).push_back(stage+" "+status+"="+shown(row,status)+" errno="+shown(row,error));
          }
          if(exact(row.get("peer_credentials_known"),false))unavailable.push_back(stage+" peer credentials unavailable (errno="+shown(row,"peer_credentials_errno")+")");
        }
        if(single.contains("duplicated")) {
          auto&d=single.at("duplicated");
          auto status=d.get("fd_ofd_status");
          if(equals(status,"same")) {
            if(!equals(d.get("fd_ofd_query"),"F_DUPFD_QUERY")||!exact(d.get("fd_ofd_result"),1)||!exact(d.get("fd_ofd_errno"),0))issues.push_back("inconsistent F_DUPFD_QUERY same-OFD result");
          }
          else if(in(status,words("different invalid_descriptor")))issues.push_back("native duplicate OFD comparison: "+str(status));
          else unavailable.push_back("native duplicate OFD comparison: "+shown(d,"fd_ofd_status")+" (errno="+shown(d,"fd_ofd_errno")+")");
          if(single.contains("workspace_export")) {
            auto&e=single.at("workspace_export");
            if(!missing(e.get("workspace_descriptor"))&&!missing(d.get("export_descriptor"))&&!exact(e.get("workspace_descriptor"),d.get("export_descriptor")))issues.push_back("workspace export FD differs from the native duplication argument");
          }
        }
        compare_rows(stage_rows(words("duplicated sent")), {
          "workspace_descriptor"
        });
        compare_rows(stage_rows(words("received claimed import allocation")), {
          "workspace_descriptor"
        });
        compare_rows(stage_rows(words("import allocation bound")),words("vk_device vk_image"));
        compare_rows(stage_rows(words("allocation bound")), {
          "vk_memory"
        });
        compare_rows(stage_rows(words("sent received claimed")),words("descriptor_count memory_descriptor_index record_bytes"));
        if(single.contains("sent")&&!send_success(single.at("sent")))issues.push_back("SCM_RIGHTS send lacks an exact successful record-length result");
        if(!modern&&single.contains("allocation")) {
          auto&a=single.at("allocation");
          if(!exact(a.get("vk_allocate_status"),0)||!exact(a.get("fd_consumed"),true))issues.push_back("vkAllocateMemory status="+shown(a,"vk_allocate_status")+" fd_consumed="+shown(a,"fd_consumed"));
        }
        if(modern) {
          compare_rows(stage_rows(words("allocation bound vulkan_export")),words("vk_device vk_image vk_memory"));
          compare_rows(rows,words("device_uuid device_incarnation memory_size image_offset row_pitch capacity_width capacity_height dedicated"));
          compare_rows(stage_rows(words("vulkan_export send_result")), {
            "workspace_descriptor"
          });
          compare_rows(stage_rows(words("send_result native_received")),words("descriptor_count memory_descriptor_index record_bytes"));
          compare_rows(stage_rows(words("cuda_import_started cuda_import cuda_map retirement_started retirement")), {
            "retained_memory_descriptor"
          });
          if(single.contains("cuda_import_started")) {
            auto&s=single.at("cuda_import_started");
            if(single.contains("native_received")&&!exact(single.at("native_received").get("workspace_descriptor"),s.get("retained_memory_descriptor")))issues.push_back("native received FD differs from retained backing FD");
            if(exact(s.get("retained_memory_descriptor"),s.get("import_descriptor")))issues.push_back("CUDA consuming import descriptor is not an independent duplicate");
          }
          compare_rows(stage_rows(words("cuda_import cuda_map")), {
            "cuda_external_memory"
          });
          for(auto&[stage,row]:single)for(auto status: {
            "cuda_status","vk_status"
          }) {
            auto v=row.get(status);
            if(integer(v)&&bigint(v)!=0)issues.push_back(stage+" "+status+"="+shown(row,status));
          }
          for(auto&[stage,names]:std::vector<std::pair<std::string,Strings>> {
            {
              "allocation", {
                "vk_status"
              }
            }, {
              "bound", {
                "vk_status"
              }
            }, {
              "send_result", {
                "initialization"
              }
            }, {
              "cuda_import_started",words("retained_memory_descriptor import_descriptor")
            }, {
              "cuda_import",words("cuda_status cuda_external_memory")
            }, {
              "cuda_map",words("cuda_status cuda_mapped_base")
            }, {
              "retirement",words("cuda_status cuda_mapped_base cuda_external_memory")
            }
          }) {
            if(single.contains(stage))for(auto&name:names)if(missing(single.at(stage).get(name)))unavailable.push_back(stage+" "+name+" not recorded");
          }
          for(auto[stage,handle]: {
            std::pair {
              "cuda_import","cuda_external_memory"
            }, {
              "cuda_map","cuda_mapped_base"
            }
          })if(single.contains(stage)&&exact(single.at(stage).get("cuda_status"),0)&&exact(single.at(stage).get(handle),0))issues.push_back(std::string(stage)+" reports success with null "+handle);
          if(single.contains("retirement")) {
            auto&r=single.at("retirement");
            if(exact(r.get("cuda_status"),0))for(auto handle: {
              "cuda_mapped_base","cuda_external_memory"
            }) {
              auto v=r.get(handle);
              if(!missing(v)&&!exact(v,0))issues.push_back(std::string("retirement reports success with unreleased ")+handle+"="+shown(r,handle));
            }
          }
          if(single.contains("cuda_import")&&exact(single.at("cuda_import").get("cuda_status"),0)&&!exact(single.at("cuda_import").get("fd_consumed"),true))unavailable.push_back("successful CUDA import lacks consumed-duplicate observation");
        }
        auto display_set=[](const std::set<std::string>&s) {
          auto value=join(Strings(s.begin(),s.end()),",");
          return value.empty()?"unknown":value;
        };
        out<<"  source="<<source<<" allocation="<<compact(display_set(allocations),100)<<" native_pid="<<display_set(native)<<" browser_pid="<<display_set(values(rows,"browser_process_id"))<<" run="<<run<<'\n';
        Strings chain;
        for(auto&stage:required) {
          auto i=single.find(stage);
          if(stage=="send_attempt"&&stages.contains(stage)) {
            chain.push_back("send_attempt(count="+std::to_string(stages[stage].size())+")");
            continue;
          }
          if(modern&&stage=="send_result"&&i==single.end()&&stages.contains(stage)) {
            chain.push_back("send_result(no_unique_success)");
            continue;
          }
          if(i==single.end()) {
            chain.push_back(stage+(stages.contains(stage)?"(ambiguous)":"(missing)"));
            continue;
          }
          auto&row=i->second;
          std::string label=stage;
          if(!missing(row.get("workspace_descriptor")))label+="(fd="+shown(row,"workspace_descriptor")+")";
          if(stage=="duplicated")label+="[export_fd="+shown(row,"export_descriptor")+",ofd="+shown(row,"fd_ofd_status")+"]";
          if(stage=="allocation"&&!modern)label+="[status="+shown(row,"vk_allocate_status")+",consumed="+shown(row,"fd_consumed")+"]";
          if(mmltk::tools::includes(words("cuda_import_started cuda_import cuda_map"),stage))label+="[retained_fd="+shown(row,"retained_memory_descriptor")+",import_fd="+shown(row,"import_descriptor")+",status="+shown(row,"cuda_status")+"]";
          chain.push_back(label);
        }
        auto dedup=[](const Strings&v) {
          Strings r;
          for(auto&s:v)mmltk::tools::append_unique(r, s);
          return r;
        };
        out<<"    direction="<<(modern?"Vulkan_export_to_CUDA_import":"historical_CUDA_export_to_Vulkan_import")<<"\n    "<<join(chain," -> ")<<"\n    conflicts: "<<(issues.empty()?"none observed in retained evidence":join(dedup(issues),"; "))<<'\n';
        if(!unavailable.empty())out<<"    unavailable: "<<join(dedup(unavailable),"; ")<<'\n';
      }
      if(groups.size()>static_cast<std::size_t>(o.top))out<<"  "<<groups.size()-o.top<<" additional source chains omitted by --top.\n";
    }
    void render(QueryResult&result,const Options&o,std::ostream&out,std::ostream&diagnostics) {
      auto rows=result.rows();
      auto total=result.matches+result.related+result.proximity+result.context+result.automatic_count;
      std::ostringstream status;
      status<<"Scanned "<<result.scanned<<" records; "<<result.matches<<" matched, "<<result.related<<" correlated, "<<result.proximity<<" proximity, "<<result.context<<" context; "<<result.errors<<" failure candidates; ";
      if(result.automatic_count)status<<result.automatic_count<<" automatic; ";
      status<<"showing "<<rows.size()<<'/'<<total<<'.';
      if(o.representatives)status<<" One representative per --group-by value; occurrences count all selected records.";
      auto&report=o.format=="summary"?out:diagnostics;
      if(result.statistics)result.statistics->render(report,o.group_by);
      if(result.lookup) {
        auto&l=*result.lookup;
        report<<"Error lookup: "<<l.mode<<"; "<<l.matches<<" occurrences; focused on first physical capture by file mtime.\nFirst physical match: "<<render_record( {
          Json::array(),*l.focus,"query",false
        },o)<<"\nRun: "<<str(l.focus->metadata["run"])<<" ["<<str(l.focus->metadata["run_link"])<<"]\n";
        if(meta(*l.focus,"test"))report<<"Test: "<<str(l.focus->metadata["test"])<<'\n';
        if(!l.artifacts.empty()) {
          report<<"Artifacts:\n";
          for(std::size_t i=0;i<l.artifacts.size()&&i<static_cast<std::size_t>(o.top);++i)report<<"  "<<l.artifacts[i]<<'\n';
        }
      }
      report<<status.str()<<'\n';
      if(o.format=="summary") {
        if(!result.groups.empty()) {
          out<<"Groups ("<<join(o.group_by,", ")<<"):\n";
          std::vector<std::pair<Strings,std::size_t>>groups(result.groups.begin(),result.groups.end());
          std::sort(groups.begin(),groups.end(),[](const auto&a,const auto&b) {
            return a.second!=b.second?a.second>b.second:a.first<b.first;
          });
          for(std::size_t i=0;i<groups.size()&&i<static_cast<std::size_t>(o.top);++i) {
            Strings values;
            for(auto&v:groups[i].first)values.push_back(compact(parse_json(v),110));
            out<<"  "<<std::setw(7)<<groups[i].second<<"  "<<join(values," | ")<<'\n';
          }
        }
        for(auto&[clock,bounds]:result.clocks)out<<"Clock "<<compact(clock)<<": "<<(bounds.first?bounds.first->convert_to<std::string>()+".."+bounds.second->convert_to<std::string>()+" ns":"no comparable timestamp")<<'\n';
        if(!result.retained.empty())out<<"Records:\n";
      }
      if(result.automatic) {
        if(o.format=="timeline"&&result.highlight_recent&&!result.recent.empty()) {
          out<<"Recent query matches (timeline follows):\n";
          for(auto&item:result.recent) {
            auto&r=item.record;
            Strings facts;
            for(auto name:words("reason observed_event control a b c d source_revision presentation_revision detail message")) {
              auto v=r.get(name);
              if(present(v))facts.push_back(name+"="+compact(v,70));
            }
            auto event=r.get("@event");
            out<<"  "<<compact(missing(event)?Json("(text)"):event,80)<<' '<<compact(join(facts," "),240)<<" @ "<<compact(r.source,100)<<':'<<r.line<<'\n';
          }
        }
        if(result.automatic_count||result.omitted_anchors)diagnostics<<"Auto correlation: "<<result.automatic_count<<" related rows; at most 3/anchor, 2/identity, 48 total, within --limit. Vulkan object provenance allows 4/anchor and identity. Rows are grouped beside exact query anchors; cross-clock times remain unaligned. Use --no-auto-correlate for the original query timeline.\n";
        if(result.omitted_anchors)diagnostics<<"Auto correlation: "<<result.omitted_anchors<<" query anchors outside the 128-anchor budget; recent specific matches take priority.\n";
        if(result.object_conflicts)diagnostics<<"Auto correlation: "<<result.object_conflicts<<" Vulkan handle keys have conflicting recorded process/device/source/allocation facts; their candidate joins were omitted.\n";
      }
      for(auto&item:rows)out<<render_record(item,o)<<'\n';
      if(o.descriptor_lineage)render_descriptor_lineage(result,o,report);
      if(o.format=="summary") {
        std::vector<Record>terminals;
        for(auto&[key,r]:result.terminals)if(result.selected_runs.contains(textget(r,"@run")))terminals.push_back(r);
        std::sort(terminals.begin(),terminals.end(),[](auto&a,auto&b) {
          return compare(order_key(a,"capture"),order_key(b,"capture"))>0;
        });
        if(terminals.size()>static_cast<std::size_t>(o.top))terminals.resize(o.top);
        if(!terminals.empty()) {
          out<<"Terminals in matched runs (independent of --limit):\n";
          for(auto it=terminals.rbegin();it!=terminals.rend();++it)out<<"  "<<render_record( {
            Json::array(),*it,"terminal",false
          },o)<<'\n';
        }
        else if(result.lookup)out<<"Terminal evidence: no terminal record was captured in this matched run.\n";
        if(!result.native_tail.empty()) {
          out<<"Latest native intent/reply evidence in this run (source order; cross-clock proximity only):\n";
          std::sort(result.native_tail.begin(),result.native_tail.end(),[](auto&a,auto&b) {
            return compare(a.key,b.key)<0;
          });
          for(auto&item:result.native_tail)out<<"  "<<render_record(item,o)<<'\n';
        }
      }
      if(result.malformed) {
        diagnostics<<"Warning: "<<result.malformed<<" malformed/truncated records (retained as searchable text):\n";
        for(auto&e:result.parse_examples)diagnostics<<"  "<<compact(e,300)<<'\n';
      }
      if(o.order=="proximity")diagnostics<<"Cross-clock neighborhoods use file mtime aligned to each source's last timestamp; proximity is not causality.\n";
      else if(result.clocks.size()>1)diagnostics<<"Clock domains are grouped separately; their timestamps do not establish cross-domain order.\n";
    }
    Strings most_common(const std::map<std::string,std::size_t>&m,const Strings&order,std::size_t limit,bool counts) {
      std::vector<std::pair<std::string,std::size_t>>v;
      for (const auto& name:order) v.emplace_back(name,m.at(name));
      std::stable_sort(v.begin(),v.end(),[](auto&a,auto&b) {
        return a.second>b.second;
      });
      Strings out;
      for(std::size_t i=0;i<v.size()&&i<limit;++i)out.push_back(v[i].first+(counts?"×"+std::to_string(v[i].second):""));
      return out;
    }
    void render_triage(Triage&t,const Options&o,std::ostream&out,std::ostream&diagnostics) {
      auto&report=o.format=="summary"?out:diagnostics;
      report<<"Triage: "<<t.scanned<<" records scanned; "<<t.selected.size()<<" anchors; "<<t.identities.size()<<" identities; "<<t.related<<'/'<<t.scoped<<" run records in exact chains; showing "<<t.evidence.size()<<" evidence rows (limit "<<o.limit<<").\n";
      if(!t.selected.empty()) {
        auto link=get(t.selected[0].record.metadata,"run_link","");
        report<<"Run: "<<compact(t.run,200)<<(truth(link)?" ["+compact(link)+"]":"")<<'\n';
        if(t.lookup)report<<"Error lookup: "<<t.lookup->mode<<"; "<<t.lookup->matches<<" occurrences.\nFirst physical match: "<<render_record( {
          Json::array(),*t.lookup->focus,"first physical pasted error",false
        },o)<<'\n';
        Strings artifacts;
        for(std::size_t i=0;i<t.artifacts.size()&&i<static_cast<std::size_t>(o.top);++i) {
          auto path=t.artifacts[i],why=t.artifact_discovery[path];
          artifacts.push_back(compact(path,180)+(why.empty()?"":" [auto: "+why+"]"));
        }
        report<<"Artifacts: "<<join(artifacts," | ")<<"\nAnchors (ranked evidence, not causes):\n";
        for(auto&e:t.selected)report<<"  "<<render_record( {
          Json::array(),e.record,str(e.why),false
        },o)<<'\n';
      }
      auto findings=t.sorted_findings();
      if(!findings.empty()) {
        report<<"Highlights:\n";
        for(std::size_t i=0;i<findings.size()&&i<static_cast<std::size_t>(o.top);++i) {
          auto&f=findings[i];
          report<<"  "<<f.kind<<(f.identity?" ["+f.identity->label()+"]":"")<<": "<<compact(f.message,230)<<" @ "<<compact(f.record.source,160)<<':'<<f.record.line;
          if(f.observations>1)report<<" ("<<f.observations<<" observations)";
          report<<'\n';
        }
        std::map<std::string,TriageFinding>earliest;
        auto rank=[&](TriageFinding f) {
          auto row=f.record;
          return Json::array( {
            t.lookup&&!t.nearby(row),f.kind=="missing-counterpart",f.record.line
          });
        };
        for(auto&f:findings) {
          if(f.score<65||f.kind=="clock-regression")continue;
          auto it=earliest.find(f.record.source);
          if(it==earliest.end()||compare(rank(f),rank(it->second))<0)earliest.insert_or_assign(f.record.source,f);
        }
        if(!earliest.empty()) {
          report<<"Earliest divergence candidates per source (physical order; partial-capture hypotheses):\n";
          int count=0;
          for(auto&[source,f]:earliest) {
            if(count++==std::min(3,o.top))break;
            report<<"  "<<compact(source,160)<<':'<<f.record.line<<": "<<f.kind<<"; not a root-cause conclusion\n";
          }
        }
      }
      else report<<"Highlights: no deterministic lifecycle anomaly established in the captured evidence.\n";
      if(!t.identities.empty()) {
        report<<"Identity lifecycles (same-source order; independent sources are not causally ordered):\n";
        for(std::size_t i=0;i<t.identity_order.size()&&i<static_cast<std::size_t>(o.top);++i) {
          auto&chain=t.identities.at(t.identity_order[i]);
          report<<"  "<<chain.identity.label()<<" hop="<<chain.hop<<", rows="<<chain.count<<", copies="<<chain.copies<<"; seed="<<compact(chain.why,120)<<"\n    owners: "<<compact(join(most_common(chain.owners,chain.owner_order,6,false),", "),180)<<"; events: "<<compact(join(most_common(chain.events,chain.event_order,8,true),", "),500)<<'\n';
          std::map<std::string,std::vector<Record>>ordered;
          for(auto&e:chain.representatives.rows())ordered[e.record.source].push_back(e.record);
          int sources=0;
          for(auto&[source,rows]:ordered) {
            if(sources++==2)break;
            std::sort(rows.begin(),rows.end(),physical_position_less);
            Strings stages;
            for(std::size_t j=0;j<rows.size()&&j<7;++j)stages.push_back(std::to_string(rows[j].line)+":"+compact(rows[j].get("@event")));
            report<<"    source-order "<<compact(source,130)<<": "<<compact(join(stages," -> "),520)<<" (distinct-stage sample)\n";
          }
        }
        if(t.identities.size()>static_cast<std::size_t>(o.top))report<<"  "<<t.identities.size()-o.top<<" additional identities analyzed (--top controls display).\n";
      }
      if(!t.selected.empty()) {
        report<<"Terminal evidence in this run (stage terminals need not mean run completion; independent row budget):\n";
        auto rows=t.terminal_pool.rows();
        if(rows.empty()) {
          report<<"  No terminal record captured; source ends alone do not establish success, failure, or timeout.\n";
          rows=t.source_tails.rows();
        }
        for(std::size_t i=0;i<rows.size()&&i<static_cast<std::size_t>(std::min(4,o.top));++i)report<<"  "<<render_record( {
          Json::array(),rows[i].record,str(rows[i].why),false
        },o)<<'\n';
      }
      if(!t.refinements.empty()) {
        Strings labels;
        for(std::size_t i=0;i<t.refinement_order.size()&&i<static_cast<std::size_t>(std::min(5,o.top));++i)labels.push_back(t.refinement_order[i].label());
        report<<"Nearby counter composites (weaker than identity, not transitive): "<<join(labels," | ")<<'\n';
      }
      if(o.format=="summary"&&t.evidence.size())out<<"Evidence (each row states why it was included):\n";
      auto evidence=t.evidence.rows();
      std::sort(evidence.begin(),evidence.end(),[&](auto&a,auto&b) {
        return compare(order_key(a.record,o.order),order_key(b.record,o.order))<0;
      });
      for(auto&e:evidence) {
        e.record.metadata["triage_reason"]=e.why;
        out<<render_record( {
          Json::array(),e.record,str(e.why),false
        },o)<<'\n';
      }
      for(auto&note:t.notes)diagnostics<<"Triage limit: "<<note<<'\n';
      if(t.malformed) {
        diagnostics<<"Warning: "<<t.malformed<<" malformed/truncated records; parse damage is not itself a product failure.\n";
        for(auto&e:t.parse_examples)diagnostics<<"  "<<compact(e,250)<<'\n';
      }
      diagnostics<<"Triage is heuristic, not a diagnosis: missing stages may be disabled logs, partial captures, or another source. Only recorded same-source clocks support gaps; proximity does not establish causality. Copied INFO is excluded from lifecycle balances. Payloads retained by triage are size-bounded.\n";
    }
    const char* HELP = R"mmltk_help(
Examples:
  ./mmltk --logs --family latest-wayland-test --triage
  ./mmltk --logs build/validation/latest-wayland-test-firefox.log --triage
  ./mmltk --logs build/validation/final-wayland-acceptance.log \\
      --family latest-wayland-test --triage -q 'probe_failed OR "rendered probe"'
  ./mmltk --logs --errors
  ./mmltk --logs --family latest-wayland-test -q '@event=vulkan.validation' \\
      --group-by validation_id --representatives --format timeline --limit 40 --top 20
  ./mmltk --logs --family latest-wayland-test -q 'validation_id:SYNC-HAZARD' \\
      --group-by validation_id --representatives --format jsonl --auto-correlate
  ./mmltk --logs -q 'onnx OR "CUDA error"'
  ./mmltk --logs -q '@event:shutdown AND NOT @event:started' --format timeline
  ./mmltk --logs -q 'trace_id=42' --correlate trace_id --tail --limit 80
  ./mmltk --logs --family latest-wayland-test -q 'workspace_allocation=34' \\
      --where '@event:memory_export OR @event:memory_import OR @event:memory_bound OR @event:image_created' \\
      --correlate native_process_id+workspace_descriptor+workspace_plane \\
      --correlate browser_process_id+vk_device+vk_image --format timeline --limit 16
  ./mmltk --logs --family latest-wayland-test -q 'workspace_allocation=34' \\
      --descriptor-lineage --format timeline --limit 32
  ./mmltk --logs --family latest-wayland-test \\
      -q '@event:loaded_library OR @event:library_inventory' --format timeline --limit 70
  ./mmltk --logs -q '@event=firefox.workspace.ready' --correlate @surface
  ./mmltk --logs -q 'duration_ns>=1000000' --fields @event,duration_ns,trace_id
  ./mmltk --logs run.jsonl --stats duration_ns --stats bytes --bucket-ms 1000
  ./mmltk --logs --where '@file:"latest-wayland-test"' --group-by @event
  ./mmltk --logs --family latest-wayland-test --errors --related-run --tail
  ./mmltk --logs --family latest-wayland-test --history --list-runs
  ./mmltk --logs --family latest-wayland-test --run 57-131007497132582 -q SIGSEGV
  ./mmltk --logs build/validation/viewer-copy-ownership-trace.log \\
      --family latest-wayland-test --history -q 'buffer="BufferId(21,1)"' --context 2
  ./mmltk --logs --error 'Presentation unavailable

  Explore requires a measured non-empty gallery.'

Grammar (quote the entire expression for the shell):
  expr      := expr OR expr | expr [AND] expr | NOT expr | '(' expr ')'
  primary   := text | '*' | mmltk::tools::includes(field) | field operator value
  operator  := = != : ~ !~ > >= < <=
AND binds tighter than OR; NOT binds tightest. Adjacent terms imply AND.
Bare/quoted text and ':' are case-insensitive literal substring searches.
'='/'!=' compare exact typed values; ordering compares numbers; '~'/'!~' use
Python regex (case-sensitive; use (?i) for insensitive). Missing fields do
not satisfy comparisons, including '!='; NOT includes them. mmltk::tools::includes() tests
presence including null/zero. Quote strings containing spaces or punctuation.

Fields:
  JSON paths (fields.name), event/trace_id/etc. (unqualified names also look
  inside fields), plus @file, @line, @line_end, @text, @format, @clock, @time_ns,
  @event, @owner, @level, @error, @surface, @intent_request, @parse_error, @test, @tags,
  @process, @parent_process, @process_group,
  @run, @archive_id, @family, @artifact, @mtime_ns, @context_copy, @part,
  @terminal, @exit_code, @signal, @signal_number, @proximity_ns, @time_link,
  @triage_reason, @triage_payload_truncated, @discovery.
  @event resolves wrapped fields.event/name before event/name.
  @surface joins native uint64 surface_high/low and Firefox 32-hex surfaces.
  @intent_request projects exact uint64 request correlations from integration.intent
  control and browser.intent received/accepted/rejected/reply_rejected value.
  Query it or use --correlate @intent_request within one browser peer lifecycle;
  a fresh browser may reuse counters, so this is never an automatic identity.
  @error marks failure candidates from levels/event/message text; it is a
  search aid, not a diagnosis. Numeric error=0 metrics do not mark failures.
  child.signaled value=139 decodes to SIGSEGV (11); 143 to SIGTERM (15).
  Signals describe the observed termination, not whether shutdown was intended.
  fatal: envelopes normalize to fatal.report and level=fatal, retaining original
  text and status. Structured Firefox envelopes expose operation, PID, PPID, PGID,
  and available actor/device/submission context; status text never creates identity.
  @process/@parent_process/@process_group accept exact positive Linux IDs only.
  Native firefox_process child/stop records project the observed child sequence
  as PID and spawn-established PGID; zero historical terminal IDs remain unknown.
  Automatic fatal/channel/process-terminal joins stay within one capture run and
  distinguish exact process, parent/child, and process-group membership. Shared
  parent alone does not join siblings. Group membership never attributes a crash
  cause or says which child supplied a parent's terminal status.
  Bare terms search test/tag/run/signal metadata as well as original text.
  Catch INFO copies retain their original timestamps and are labeled context-copy.
  Memory provenance follows Firefox Vulkan allocation/binding/export, SCM_RIGHTS
  send attempts and exact send results, native receipt, CUDA import and mapping,
  and retirement. --descriptor-lineage restricts a query to these physical facts
  and retained historical CUDA-export captures. Query workspace_allocation in one
  capture and use --family to include native and Firefox artifacts. Correlations
  use native_process_id+workspace_allocation, browser_process_id+source+workspace_allocation,
  and process-local image or historical export identities. FD numbers never join
  different processes; fstat metadata does not establish backing payload identity.
  The bounded report uses retained rows only (--limit/--where still apply), with
  at most --top source chains. Missing and ambiguous observations remain explicit.
  A descriptor_send is only an attempt, including EINTR/EAGAIN retries. Only an
  exact record-length descriptor_send_result with send_errno=0 proves send success.
  Native received storage remains in retained_memory_descriptor. import_descriptor
  is its consuming duplicate, observed before CUDA; successful import does not
  query that consumed FD. Historical F_DUPFD_QUERY observations retain their
  original same/different/unsupported/denied meaning without asserting identity
  across processes. Diagnostics never retain extra descriptors or GPU resources.
  loaded_library/library_inventory capture existing /proc/self/maps once at the
  native first_memory_import or Firefox first_memory_export. They load no DSO.
  Inventories bound reads to 1 MiB and 32 paths shorter than 1024 bytes, reporting
  incomplete/unavailable facts. Query these separately by process ID so they do
  not consume the allocation chain's retained-row budget. Disabled diagnostics
  collect nothing. Raw rows retain their original file/line provenance.

Vulkan layer diagnostics:
  Adjacent Rust VALIDATION headers, indented message bodies, and matching
  logger objects lines form one searchable record. The original physical lines
  and complete message remain available in text/message; @line..@line_end
  identifies their span. Assembly is bounded to 128 lines and 1 MiB; overflow is
  reported as a parse error and remaining lines stay searchable separately.
  @event=vulkan.validation exposes validation_id (VUID or SYNC-HAZARD), message_id,
  api, level, and typed objects with normalized hexadecimal handles and reported
  names. A sole IMAGE or DEVICE_MEMORY is also queryable as vk_image/vk_memory.
  Human timelines print the complete diagnostic, including its stated conditions.
  It remains a layer allegation; a handle match does not establish its cause.
  --group-by validation_id --representatives selects one first record per group,
  preferring physical records over context copies, with @representative_count.
  All selected occurrences still contribute to counts. --order controls selection
  and ordering; --limit bounds retained groups/context plus automatic evidence.
  Add api or message to --group-by when different messages use the same VUID.
  --representatives cannot combine with --tail, --error, or --triage.
  Automatic correlation joins the representative's exact typed image/memory
  handles to image_created, memory_import, memory_bound, and direct_binding.
  It retains at most four diverse provenance events per Vulkan anchor/handle,
  within the normal 48-row and --limit budgets, using the existing streaming pass.
  Explicit equal process IDs may join files in the same run; conflicting process,
  device, source, or allocation facts reject the join. PID-free Rust stderr can
  only produce labeled same-file handle candidates, never a cross-process join
  or an asserted process identity. Observed handle reuse/multiple owners suppress
  ambiguous candidates. Missing provenance is not proof that an object is unrelated.
  Narrow a capture/allocation lifetime because handle numbers may be reused.
  Correlated rows do not seed further expansion. JSONL requires --auto-correlate;
  --no-auto-correlate disables the automatic joins in human timelines.

Inputs and ordering:
  --stats FIELD (repeatable, at most 32 distinct fields) aggregates all exact
  query matches independently of --limit/--top; related/context rows do not
  contribute. Each artifact run, explicit run_id, source, clock and --group-by
  value remains separate. Missing/null and invalid/nonfinite values have
  separate counters. Sum/mean retain exact integer arithmetic; nonintegral
  results are exact numerator/denominator strings. Accumulators are bounded to
  8192 bits and 10000 total groups/buckets; overflow fails explicitly.
  --bucket-ms N adds half-open [start,end) clock-local buckets, anchored at that
  clock's zero, using exact integer nanoseconds. Rates divide observed events
  by the requested full bucket width; edge buckets may be partial captures.
  Untimed rows contribute only to totals. Statistics include first/last
  physical line and timestamp anchors. JSONL keeps projected rows on stdout
  and writes Statistics JSON objects to stderr. No cross-clock alignment or
  capture-completeness inference is performed.
  Paths/globs are repository-relative or absolute within the repository.
  Directories select .jsonl/.log/.out/.txt and Mozilla process logs; --recursive includes histories.
  Explicit files can have any suffix. Repeated paths are deduplicated.
  The default is build/validation, without recursive archived captures.
  Time ordering groups steady, UTC wall, timezone-free wall, per-file elapsed,
  and untimed records separately; ties use file/line. No clock offset is guessed.
  --order capture sorts files by captured mtime then preserves their line order;
  mtime is artifact recency, not an event timestamp or proof of causality.
  Use one capture at a time: IDs and monotonic times can repeat across runs.
  Files are read up to their captured byte size; correlation requires unchanged
  files over two passes. There is no follow mode, persistent index, or network.

Artifact families:
  --family STEM selects STEM.jsonl / STEM.log / STEM-native.log / STEM-firefox.log
  plus STEM-acceptance.jsonl, STEM-application.log, and
  STEM-mozilla-main.PID.log.moz_log[.0..3] and
  STEM-mozilla-child.PID.log.child-LOGICAL_ID.moz_log[.0..3]. Descendant
  launchers append another .child-LOGICAL_ID before .moz_log. Mozilla modules
  retain four bounded rotating files per process, separately from structured Firefox evidence.
  Families live under build/validation (or supply a repository path). --history adds rotated
  siblings; --run selects an exact archive ID or 'current'. Adjacent rotations
  with the same PID within 10ms are grouped and explicitly labeled inferred.
  Shared (event, steady_ns) anchors link transcript captures to native runs and
  propagate known test/tag metadata. Missing anchors leave transcripts separate.
  --related-run includes other records from a matched run; named correlations
  are automatically scoped to the run in family/history mode. Summary output
  includes terminal events from matched runs independently of the sample limit.

Automatic timeline correlation:
  Human --format timeline queries with --query/--errors add a small set of
  direct frame/publication/snapshot/lifecycle matches beside exact query rows.
  --no-auto-correlate restores the original timeline. JSONL and summary formats
  require explicit --auto-correlate; --correlate, --related-run, --error, and
  --triage keep their existing selection behavior without this extra pass.
  Original matches and ordering are preserved; automatic rows use spare --limit
  capacity only. Broad/truncated timelines start with three recent specific query
  matches, including matches beyond the retained sample. Recency uses captured
  file mtime and physical source order; it never aligns independent clocks.
  Up to 128 retained query anchors prefer recent specific events over progress
  chatter. Limits are 3 diverse events per anchor, 2 rows per joining identity,
  and 48 related rows total. Added rows never become new correlation seeds.
  Source/session/revision, surface/publication/allocation/transfer, trace/span,
  dataset/gallery, and scoped observation identities must be nonempty and agree
  where both records state them. Bare counters, pixel/probe/slot loops, context
  copies, malformed rows, and ambiguous source revisions do not auto-expand.
  Known integration report slots have event-specific projections, such as
  explore_reopen_wait.b -> Explore source revision and explore_reopen_draw.c/d
  -> source/presentation revision. Generic a/b/c/d numbers never join.
  Phase/control matches additionally need at most 250ms (--near-ms can narrow
  it), or eight same-source lines with unaligned clocks, and are labeled weaker
  proximity, capped at eight rows globally. Cross-clock grouping by an exact
  identity never aligns clocks.
  Every added row shows [auto: reason]. Explicit JSONL opt-in uses match=auto
  and auto_reason/auto_anchor metadata (also included with --fields). Retained
  automatic payloads use the triage size bounds; auto_payload_truncated flags
  any shortening. --where constrains all added rows. One extra streaming pass
  uses bounded per-identity nearest-anchor indexes, without an all-run index.

Pasted errors:
  --error TEXT searches exact whitespace-normalized phrases first, accepting a
  colon when a displayed title and body were separated by a blank line. Only
  when no phrase matches does it try all words, reporting that fallback.
  It includes histories, prefers physical logs over copied transcript context,
  and focuses the earliest matching capture by file mtime, then file/line.
  The original event timestamp and clock are always retained.
  Nearby records default to +/-1000ms (--near-ms). Same-clock differences are
  direct; cross-clock estimates align file mtime to each source's last timestamp.
  Untimed rows may borrow a preceding timestamp for proximity only. Every such
  estimate is labeled; file timestamps are not proof of causal order.
  Output prioritizes the error and nearby intent/reply/message/terminal records.
  --limit bounds the timeline sample; terminal summaries have at most --top rows
  and native run-tail evidence has at most min(5, --top) rows.

Automatic triage:
  Graphics pixel joins follow the acquired mode: direct_sampling=true compares
  native source -> Iced sample, without import/mailbox copy stages. Copy mode
  compares native -> import -> mailbox -> sample. An exact frame_forwarded
  source+transfer bridge selects the physical source; arena identity alone
  never identifies source storage. Direct GPU read_settled receipts prove
  released source custody; copy_completed proves the Copy-mode transfer.
  Offered publications need no read settlement until actually acquired.
  Optional firefox.workspace.direct_pixel adds native -> direct_image -> sample
  comparisons only for that exact direct acquisition after completion=Ok(true)
  and matching 0x4d4d4c54 readback markers. Missing or invalid optional probes
  never replace native -> sample or require another evidence stage.
  Optional presentation.pixel_source_copy_before_ready and
  presentation.pixel_source_copy_after_release report direct CUDA source-to-host
  samples separately from the native kernel probes (presentation.pixel and
  presentation.pixel_after_release). Exact source+transfer+publication and
  coordinates join these observations to their kernel, each other, and the
  acquired direct_image/Iced samples. Contradictory source/allocation facts
  prevent byte comparison; missing optional observations never imply failure.
  Raw buffer-alias observation is unavailable across the image's external
  ownership acquire; direct_memory_alias/status report the exact reason.
  Historical direct_memory_pixel records read undefined alias contents and
  remain inspectable, but never participate in pixel comparisons.
  --triage selects a run using ranked failure/assertion/error/terminal/parse
  anchors, or an unmatched start when no explicit failure is available. --query
  narrows anchors; --where constrains every pass. --error keeps its first-physical
  phrase lookup and proximity behavior. Nothing in triage includes the whole run.
  A single file discovers same-family current siblings, or only the matching
  archive batch (even without current files). Auto additions carry @discovery;
  rotation links are inferred. Ordinary path queries do not auto-expand inputs.
  Explicit failures outrank error-related event names; --errors remains broad.
  Defaults: 4 anchors, 12 identities, 2 expansion hops, 20 evidence rows,
  5 summary entries per section. Tune
  --triage-anchors, --triage-identities, --triage-hops, --limit, and --top.
  At most 32 diversified neighboring records seed identities, using 3 lines (or
  --context if larger) and --near-ms. Every row states its inclusion reason.
  Exact identities are run-qualified surface/typed handles, nonempty *_id or
  *_identity fields, and source_session+source_instance. parent_*_id joins its
  corresponding *_id namespace. Device/process/thread/user/session IDs and
  bare counters are excluded as ambient; --correlate explicitly opts fields in.
  Identities co-occurring on a matched row can seed the next hop. Counter
  sequence/generation/slot composites need two fields plus same-clock proximity
  or 100 same-source lines; zero slots are valid. They never seed further hops.
  Lifecycle balances use each row's most-specific operation/resource identity
  (span, request, surface, then other IDs; not secondary handles), exact event
  prefixes and generic start/end suffixes
  (requested/submitted/started -> completed/failed/outcome, opened -> closed,
  acquired -> released, etc.). These conventions are hypotheses, not product
  requirements. Missing/blocked/stalled stages, failed textual span outcomes,
  duplicate ends, unclosed starts, identity handoffs, and the largest same-source
  clock gaps (--triage-gap-ms, default 1000) are highlighted with evidence.
  Handoff candidates require a shared identity and no conflicting operation ID;
  unrelated endings or already-balanced endpoints cannot satisfy missing stages.
  Sources are never causally ordered from proximity. Copied INFO does not
  participate in lifecycle balances. Numeric outcome enums are not guessed.
  Up to 4 generic *.terminal/terminal=true or known process/test terminal
  records are reported independently of --limit. Without terminals, captured
  source ends are shown without claiming success/failure/timeout.
  Memory is bounded: 256 open anchor families, 64 findings, 64 event/family
  values per identity, 32 sources per identity, 10000 auto-added artifact files,
  and size-bounded retained
  payloads (96 fields, depth 4, 2048 characters per string). Capacity limits are
  reported. Triage uses bounded streaming passes, no all-run index or network.
  --related-run/--tail/--list-runs cannot combine with --triage. JSONL writes
  only evidence rows to stdout, summaries to stderr; @triage_reason records why.
)mmltk_help";
    void render_help(std::ostream&out) {
      out<<"usage: ./mmltk --logs [options] [paths ...]\n\nBounded, read-only queries over runtime logs; invoked through ./mmltk --logs.\n\n";
      out<<R"(options:
  -h, --help                  show this help message and exit
  -q, --query EXPR             select records using the expression grammar
  --where EXPR                 filter all records, including correlated/context rows
  --errors                     AND @error=true with --query
  --error TEXT                 locate first physical capture of displayed error text
  --triage                     rank failures and bounded identity/lifecycle evidence
  --triage-anchors N           maximum anchors, 1..12 (default: 4)
  --triage-identities N        maximum identities, 1..64 (default: 12)
  --triage-hops N              expansion hops, 0..3 (default: 2)
  --triage-gap-ms MS           comparable clock gap, 1..600000 (default: 1000)
  --near-ms MS                 proximity radius, 0..600000 (default: 1000)
  --correlate FIELD[+FIELD...]  direct identity correlation; repeat for alternatives
  --descriptor-lineage        physical Vulkan/CUDA export/import evidence
  --auto-correlate             enable bounded automatic related evidence
  --no-auto-correlate          disable automatic evidence and recent highlights
  --context N                 nonblank neighboring records, 0..100 (default: 0)
  --format {summary,timeline,jsonl}
  --fields FIELDS              comma-separated projection, with provenance retained
  --group-by FIELDS            summary groups (default: @owner,@event)
  --stats FIELD                exact numeric statistics; repeatable, max 32 fields
  --bucket-ms N                clock-local rate buckets, 1..86400000
  --representatives           one first physical representative per group
  --top N                     summary entries, 1..100 (default: 5 triage, 10 ordinary)
  --limit N                   sample rows, 1..10000 (default: 20)
  --order {time,file,capture,proximity}
  --tail                      select last records, displayed ascending
  --last-lines N              physical-line input window, 1..100000
  --recursive                 include nested directories and histories
  --family STEM               select artifact siblings; repeatable
  --history                   include rotated .history siblings
  --run RUN                   exact run/archive ID or current; implies history
  --list-runs                 list discovered captures without parsing
  --related-run               include records in each matched capture
  --strict                    return 2 for damaged/truncated input

)";
      out<<replace_rx(R"(\\\\)",HELP,"\\");
    }
    int bounded(const std::string&value,int low,int high) {
      auto s=trim(value);
      if (s.starts_with('+')) s.erase(0,1);
      int n=0;
      auto[p,error]=std::from_chars(s.data(),s.data()+s.size(),n);
      if(error!=std::errc()||p!=s.data()+s.size())throw QueryError("invalid integer value: '"+value+"'");
      if(n<low||n>high)throw QueryError("expected "+std::to_string(low)+".."+std::to_string(high));
      return n;
    }
  }
  Options parse_options(const Strings&args) {
    Options o;
    std::optional<bool>automatic;
    bool positional=false;
    for(std::size_t i=0;i<args.size();++i) {
      auto arg=args[i];
      if(positional) {
        o.paths.push_back(arg);
        continue;
      }
      if(arg=="--") {
        positional=true;
        continue;
      }
      if(arg.empty()||arg[0]!='-'||arg=="-") {
        o.paths.push_back(arg);
        continue;
      }
      std::optional<std::string>inline_value;
      auto eq=arg.find('=');
      if(eq!=std::string::npos&&arg.starts_with("--")) {
        inline_value=arg.substr(eq+1);
        arg.resize(eq);
      }
      else if(arg.starts_with("-q")&&arg!="-q") {
        inline_value=arg.substr(2);
        arg="-q";
      }
      auto value=[&]() {
        if(inline_value)return *inline_value;
        if(i+1>=args.size()||(args[i+1].starts_with("-")&&!full(NUMBER,args[i+1])))throw QueryError("argument "+arg+": expected one argument");
        return args[++i];
      };
      auto flag=[&]() {
        if(inline_value)throw QueryError("argument "+arg+": ignored explicit argument '"+*inline_value+"'");
      };
      if(arg=="-h"||arg=="--help") {
        flag();
        o.help=true;
      }
      else if(arg=="-q"||arg=="--query")o.query=value();
      else if(arg=="--where")o.where=value();
      else if(arg=="--error")o.error=value();
      else if(arg=="--correlate")o.correlate.push_back(value());
      else if(arg=="--family")o.family.push_back(value());
      else if(arg=="--run")o.run=value();
      else if(arg=="--fields") { auto fields=value(); o.fields=fields.empty()?Strings{}:field_names(fields); }
      else if(arg=="--group-by")o.group_by=field_names(value());
      else if(arg=="--stats") {
        for(auto&name:field_names(value()))mmltk::tools::append_unique(o.stats, name);
      }
      else if(arg=="--format") {
        o.format=value();
        if(!mmltk::tools::includes(words("summary timeline jsonl"),o.format))throw QueryError("argument --format: invalid choice: '"+o.format+"'");
      }
      else if(arg=="--order") {
        o.order=value();
        if(!mmltk::tools::includes(words("time file capture proximity"),o.order))throw QueryError("argument --order: invalid choice: '"+o.order+"'");
      }
      else if(arg=="--triage-anchors")o.triage_anchors=bounded(value(),1,12);
      else if(arg=="--triage-identities")o.triage_identities=bounded(value(),1,64);
      else if(arg=="--triage-hops")o.triage_hops=bounded(value(),0,3);
      else if(arg=="--triage-gap-ms")o.triage_gap_ms=bounded(value(),1,600000);
      else if(arg=="--near-ms")o.near_ms=bounded(value(),0,600000);
      else if(arg=="--context")o.context=bounded(value(),0,100);
      else if(arg=="--bucket-ms")o.bucket_ms=bounded(value(),1,86400000);
      else if(arg=="--top")o.top=bounded(value(),1,100);
      else if(arg=="--limit")o.limit=bounded(value(),1,10000);
      else if(arg=="--last-lines")o.last_lines=bounded(value(),1,100000);
      else {
        flag();
        if(arg=="--errors")o.errors=true;
        else if(arg=="--triage")o.triage=true;
        else if(arg=="--descriptor-lineage")o.descriptor_lineage=true;
        else if(arg=="--representatives")o.representatives=true;
        else if(arg=="--tail")o.tail=true;
        else if(arg=="--recursive")o.recursive=true;
        else if(arg=="--history")o.history=true;
        else if(arg=="--list-runs")o.list_runs=true;
        else if(arg=="--related-run")o.related_run=true;
        else if(arg=="--strict")o.strict=true;
        else if(arg=="--auto-correlate"||arg=="--no-auto-correlate") {
          bool v=arg=="--auto-correlate";
          if(automatic&&*automatic!=v)throw QueryError("argument "+arg+": not allowed with argument "+(*automatic?"--auto-correlate":"--no-auto-correlate"));
          automatic=v;
          o.auto_correlate=v;
        }
        else throw QueryError("unrecognized arguments: "+arg);
      }
    }
    return o;
  }
  int main(const Strings&args,const fs::path&root,std::ostream&out,std::ostream&diagnostics) {
    try {
      Options o=parse_options(args);
      if(o.help) {
        render_help(out);
        return 0;
      }
      auto query=QueryParser(o.query).parse(),where=QueryParser(o.where).parse();
      if(o.errors)query= {
        "and",Json::array(), {
          query, {
            "=",Json::array( {
              "@error",true
            }), {
            }
          }
        }
      };
      if(o.descriptor_lineage) {
        if(trim(o.query).empty()||o.triage||o.related_run||o.error||o.list_runs||o.representatives)throw QueryError("--descriptor-lineage requires --query and cannot combine with --triage, --related-run, --error, --list-runs, or --representatives");
        Expression events {
          "or",Json::array(), {
          }
        };
        for(auto&e:MEMORY_PROVENANCE_EVENTS)events.children.push_back( {
          "=",Json::array( {
            "@event",e
          }), {
          }
        });
        where= {
          "and",Json::array(), {
            where,events
          }
        };
        for(auto names: {
          "native_process_id+workspace_allocation","browser_process_id+source+workspace_allocation","native_process_id+workspace_descriptor+workspace_plane","browser_process_id+vk_device+vk_image"
        })mmltk::tools::append_unique(o.correlate, names);
      }
      if(o.triage&&(o.related_run||o.tail||o.list_runs))throw QueryError("--triage cannot combine with --related-run, --tail, or --list-runs; its evidence is bounded automatically");
      if(o.representatives&&(o.triage||o.tail||o.error))throw QueryError("--representatives cannot combine with --triage, --tail, or --error");
      if(!o.correlate.empty()&&trim(o.query).empty()&&!o.errors&&!o.triage)throw QueryError("--correlate requires an explicit --query or --errors seed");
      if(o.auto_correlate==true&&!auto_correlate_enabled(o))throw QueryError("--auto-correlate requires --query or --errors and cannot combine with --correlate, --related-run, --error, or --triage");
      if(o.stats.size()>32)throw QueryError("--stats accepts at most 32 distinct fields");
      if((!o.stats.empty()||o.bucket_ms)&&(o.triage||o.list_runs))throw QueryError("statistics require the streaming query and cannot combine with --triage or --list-runs");
      if(!o.top)o.top=o.triage?5:10;
      if(o.order.empty())o.order=o.error?"proximity":"time";
      if(o.order=="proximity"&&!o.error)throw QueryError("--order proximity requires --error");
      if(o.error)o.history=true;
      for(auto&relationship:o.correlate)field_names(relationship,'+');
      auto repository=fs::canonical(root.empty()?tools::repository_root( {
      },__FILE__):root);
      ArtifactCatalog catalog(o,repository);
      if(o.list_runs) {
        catalog.list_runs(out,o.limit);
        return 0;
      }
      if(o.last_lines) {
        for(auto&f:catalog.files)f=f.recent(o.last_lines);
        diagnostics<<"Partial input: last "<<o.last_lines<<" physical lines per file; earlier context and evidence excluded.\n";
        for(auto&f:catalog.files)diagnostics<<"  "<<f.source<<": starting at line "<<f.first_line<<", byte "<<f.start_offset<<'\n';
      }
      if(!o.family.empty()||o.history||!o.run.empty()||o.related_run||o.triage)catalog.prepare_anchors();
      if(!catalog.selected_runs.empty()) {
        Expression scope {
          "or",Json::array(), {
          }
        };
        for(auto&run:catalog.selected_runs)scope.children.push_back( {
          "=",Json::array( {
            "@run",run
          }), {
          }
        });
        where= {
          "and",Json::array(), {
            where,scope
          }
        };
      }
      std::unique_ptr<ErrorLookup>lookup;
      auto files=catalog.files;
      if(o.error) {
        lookup=std::make_unique<ErrorLookup>(*o.error,o);
        lookup->locate(files,query,where,catalog.anchors);
        if(!lookup->focus) {
          out<<"No exact/display-normalized phrase or all-words matches in "<<files.size()<<" files.\n";
          if(o.strict&&lookup->malformed) {
            diagnostics<<"Strict lookup scan observed "<<lookup->malformed<<" malformed/truncated records.\n";
            return 2;
          }
          return 1;
        }
        std::erase_if(files,[&](const LogFile&f) {
          return !lookup->relevant(f);
        });
        lookup->calibrate(files,catalog.anchors);
        lookup->annotate(*lookup->focus,true);
        query=lookup->expression;
        where= {
          "and",Json::array(), {
            where, {
              "=",Json::array( {
                "@run",lookup->focus->metadata["run"]
              }), {
              }
            }
          }
        };
      }
      if(o.triage) {
        Triage triage(o,where,catalog.anchors,lookup.get());
        triage.run_query(files,query);
        render_triage(triage,o,out,diagnostics);
        if(o.strict&&(triage.malformed||(lookup&&lookup->malformed)))return 2;
        return triage.selected.empty()?1:0;
      }
      auto result=execute(files,query,where,o,catalog.anchors,lookup.get());
      render(result,o,out,diagnostics);
      if(o.strict&&lookup&&lookup->malformed) {
        if(lookup->malformed!=result.malformed)diagnostics<<"Strict lookup scan observed "<<lookup->malformed<<" malformed/truncated records across all inputs.\n";
        return 2;
      }
      if(o.strict&&result.malformed)return 2;
      return result.matches+result.related?0:1;
    }
    catch(const std::exception&error) {
      diagnostics<<"mmltk logs: "<<error.what()<<'\n';
      return 2;
    }
  }
}
// namespace mmltk::log_query
int mmltk::log_query::entry(int argc,char**argv) {
  std::signal(SIGPIPE,SIG_IGN);
  std::vector<std::string>args(argv+1,argv+argc);
  int status=mmltk::log_query::main(args, {
  },std::cout,std::cerr);
  std::cout.flush();
  if(!std::cout&&errno==EPIPE)return 0;
  return status;
}
