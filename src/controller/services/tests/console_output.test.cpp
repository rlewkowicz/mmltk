#include "src/controller/services/console_output.h"
#include <fcntl.h>
#include <unistd.h>
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <string>
#include <string_view>
namespace {
class PipeOwner final {
   public:
    PipeOwner() = default;
    PipeOwner(const PipeOwner&) = delete;
    PipeOwner& operator=(const PipeOwner&) = delete;
    ~PipeOwner() {
        for (int& descriptor : descriptors_) { close(descriptor); }
    }
    [[nodiscard]] bool create() noexcept { return ::pipe(descriptors_.data()) == 0; }
    [[nodiscard]] int read_descriptor() const noexcept { return descriptors_[0]; }
    [[nodiscard]] int write_descriptor() const noexcept { return descriptors_[1]; }
    void close_write_descriptor() noexcept { close(descriptors_[1]); }

   private:
    static void close(int& descriptor) noexcept {
        if (descriptor >= 0) {
            ::close(descriptor);
            descriptor = -1;
        }
    }
    std::array<int, 2> descriptors_{-1, -1};
};
[[nodiscard]] bool set_nonblocking(const int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    return flags >= 0 && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}
[[nodiscard]] bool write_exact(const int fd, const std::string_view payload) {
    return ::write(fd, payload.data(), payload.size()) == static_cast<ssize_t>(payload.size());
}
void test_append_console_output_normalizes_terminal_sequences() {
    std::string tail;
    mmltk::controller::services::console_output::append_console_output(tail, "hello\rworld\nabc\b!\033[31m?\n", 128);
    REQUIRE(tail == "world\nab!?\n");
}
void test_drain_nonblocking_fd_reads_available_output() {
    PipeOwner pipe;
    REQUIRE(pipe.create());
    REQUIRE(set_nonblocking(pipe.read_descriptor()));
    REQUIRE(write_exact(pipe.write_descriptor(), "ready"));
    const std::string output = mmltk::controller::services::console_output::read_fd(pipe.read_descriptor(), "failed to read test output pipe: ", true);
    REQUIRE(output == "ready");
}
void test_read_fd_to_string_reads_until_eof() {
    PipeOwner pipe;
    REQUIRE(pipe.create());
    REQUIRE(write_exact(pipe.write_descriptor(), "vast output"));
    pipe.close_write_descriptor();
    const std::string output = mmltk::controller::services::console_output::read_fd(pipe.read_descriptor(), "failed to read test blocking output pipe: ");
    REQUIRE(output == "vast output");
}
}  // namespace
TEST_CASE("console output normalizes terminal sequences", "[controller][services][console-output]") {
    test_append_console_output_normalizes_terminal_sequences();
}
TEST_CASE("console output drains a nonblocking descriptor", "[controller][services][console-output]") { test_drain_nonblocking_fd_reads_available_output(); }
TEST_CASE("console output reads through EOF", "[controller][services][console-output]") { test_read_fd_to_string_reads_until_eof(); }
