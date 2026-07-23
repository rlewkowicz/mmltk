

#ifndef CATCH_INTERFACES_TESTCASE_HPP_INCLUDED
#define CATCH_INTERFACES_TESTCASE_HPP_INCLUDED

#include <vector>

namespace Catch {

struct TestCaseInfo;
class TestCaseHandle;
class IConfig;

class ITestCaseRegistry {
   public:
    virtual ~ITestCaseRegistry();
    virtual std::vector<TestCaseInfo*> const& getAllInfos() const = 0;
    virtual std::vector<TestCaseHandle> const& getAllTests() const = 0;
    virtual std::vector<TestCaseHandle> const& getAllTestsSorted(IConfig const& config) const = 0;
};

}  

#endif  // CATCH_INTERFACES_TESTCASE_HPP_INCLUDED
