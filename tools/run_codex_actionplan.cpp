module;
// Native counterpart of run_codex_actionplan.sh. Requires cpp_support.hpp and
// the existing third_party/json/include headers; invokes only the Codex CLI.

#include <cerrno>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>
#include <cstddef>
#include <algorithm>
#include <initializer_list>
#include <optional>
#include <array>
#include <chrono>
#include <climits>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

export module mmltk.tools.run_codex_actionplan;
import mmltk.tools.cpp_support;
import mmltk.tools.cpp_text;

namespace {
namespace support = mmltk::tools;
using support::json;

// This is literal program data, never an instruction to source-processing tools.
constexpr std::string_view prompt = R"PROMPT(If the current actionplan.md is not complete, exit. If the current actionplan.md is complete, create a new one. We need to focus on folder organization. We have ./inlcude which has some includes, but then src has scattered includes as well. The cmake files are massive and monolithic. The application systems must keep ordinary domain classes and infrastructure boundaries modular instead of accumulating capabilities in one folder. Broadly, the number one goal is compilation time, and parallelism of compilation. The secondary goal is code organization and structure. src/rfdetr for example has cuda functionality and drawing that could be used for other non existent models. We don't want a spaghetti mess of code reaching across the code base. Broadly where can we have better interface demarcation, better organization, better modularity. Where are there sprawling duplicated classes, functions, templates across the code base that could be consolidated into into central frameworks to reduce boiler plate for future model modules. Where can we make model layers agnostic and reusable? ../ultralytics has examples of folder organization for multiple models and layers. It's python, but it can be used for organizational reference. ../obs-studio is a world class, compiled language implementation same with ../godot, and ../krita. With strong modularity and organization. Use these references and try to come up with features and phases that could benefit this repository. If you created a new actionplan.md, execute it. If the code base is well organized, free of massive "god files" (some might be ok), exit.)PROMPT";

class AgentMessages {
public:
    void append(std::string_view chunk) {
        // Retain only the current record. CRLF and bare CR are accepted just
        // like readline, including when the separator spans stream chunks.
        for (const char byte : chunk) {
            if (byte == '\n' && after_cr_) {
                after_cr_ = false;
                continue;
            }
            after_cr_ = byte == '\r';
            if (byte == '\n' || byte == '\r') {
                render();
                pending_.clear();
            } else {
                pending_ += byte;
            }
        }
    }

    void finish() {
        if (!pending_.empty()) render();
        pending_.clear();
    }

private:
    void render() const {
        const auto event = json::parse(pending_, nullptr, false);
        if (!event.is_object()) return;
        const auto type = event.find("type");
        if (type == event.end() || *type != "item.completed") return;
        const auto item = event.find("item");
        if (item == event.end() || !item->is_object()) return;
        const auto item_type = item->find("type");
        if (item_type == item->end() || *item_type != "agent_message") return;
        const auto text = item->find("text");
        std::cout << (text == item->end() ? "undefined" : mmltk::tools::js_string(*text)) << '\n';
        std::cout.flush();
        if (!std::cout) throw support::ToolError("cannot write agent message to stdout");
    }

    std::string pending_;
    bool after_cr_ = false;
};

long long run_count(const char* argument) {
    const std::string text = support::trim(argument);
    if (text.empty()) return 0;
    char* end = nullptr;
    errno = 0;
    const long long count = std::strtoll(text.c_str(), &end, 0);
    if (errno == ERANGE || end == text.c_str() || *end != '\0')
        throw support::ToolError("run count must be a signed integer");
    return count;
}
} // namespace

export int mmltk_tool_run_codex_actionplan(int argc, char** argv) {
    try {
        const long long runs = run_count(argc > 1 ? argv[1] : "");
        const auto root = support::repository_root(argv[0], __FILE__);
        const std::vector<std::string> command{
            "codex", "--sandbox", "danger-full-access", "--ask-for-approval", "never",
            "exec", "--json", "--color", "never", "--skip-git-repo-check", "--cd",
            root.string(), std::string(prompt)};
        for (long long completed = 0; completed < runs; ++completed) {
            const long long current = completed + 1;
            std::cerr << "codex actionplan run " << current << '/' << runs << '\n';
            AgentMessages messages;
            support::RunOptions options;
            options.merge_stderr = true;
            options.stdout_sink = [&](std::string_view chunk) { messages.append(chunk); };
            const auto result = support::run(command, options);
            messages.finish();
            if (result.exit_code != 0) {
                std::cerr << "codex actionplan run " << current << '/' << runs
                          << " failed (exit " << result.exit_code << ")\n";
                return result.exit_code;
            }
            std::cerr << "codex actionplan run " << current << " of " << runs << " done\n";
        }
        return 0;
    } catch (const support::ToolError& error) {
        std::cerr << "run_codex_actionplan: " << error.what() << '\n';
        return error.status;
    } catch (const std::exception& error) {
        std::cerr << "run_codex_actionplan: " << error.what() << '\n';
        return 1;
    }
}
