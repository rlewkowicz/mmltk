

#ifndef CATCH_NONCOPYABLE_HPP_INCLUDED
#define CATCH_NONCOPYABLE_HPP_INCLUDED

namespace Catch {
namespace Detail {

class NonCopyable {
   public:
    NonCopyable(NonCopyable const&) = delete;
    NonCopyable(NonCopyable&&) = delete;
    NonCopyable& operator=(NonCopyable const&) = delete;
    NonCopyable& operator=(NonCopyable&&) = delete;

   protected:
    NonCopyable() noexcept = default;
};

}  
}  

#endif  // CATCH_NONCOPYABLE_HPP_INCLUDED
