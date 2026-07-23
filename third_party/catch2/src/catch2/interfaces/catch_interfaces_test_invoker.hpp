

#ifndef CATCH_INTERFACES_TEST_INVOKER_HPP_INCLUDED
#define CATCH_INTERFACES_TEST_INVOKER_HPP_INCLUDED

namespace Catch {

class ITestInvoker {
   public:
    virtual void prepareTestCase();
    virtual void tearDownTestCase();
    virtual void invoke() const = 0;
    virtual ~ITestInvoker();
};

}  

#endif  // CATCH_INTERFACES_TEST_INVOKER_HPP_INCLUDED
