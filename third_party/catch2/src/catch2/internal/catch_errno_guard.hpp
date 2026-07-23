

#ifndef CATCH_ERRNO_GUARD_HPP_INCLUDED
#define CATCH_ERRNO_GUARD_HPP_INCLUDED

namespace Catch {

class ErrnoGuard {
   public:
    ErrnoGuard();
    ~ErrnoGuard();

   private:
    int m_oldErrno;
};

}  

#endif  // CATCH_ERRNO_GUARD_HPP_INCLUDED
