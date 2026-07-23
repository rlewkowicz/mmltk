

#ifndef CATCH_TEST_FAILURE_EXCEPTION_HPP_INCLUDED
#define CATCH_TEST_FAILURE_EXCEPTION_HPP_INCLUDED

namespace Catch {

struct TestFailureException {};
struct TestSkipException {};

[[noreturn]] void throw_test_failure_exception();

[[noreturn]] void throw_test_skip_exception();

}  

#endif  // CATCH_TEST_FAILURE_EXCEPTION_HPP_INCLUDED
