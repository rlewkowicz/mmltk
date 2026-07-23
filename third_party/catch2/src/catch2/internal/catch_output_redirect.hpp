

#ifndef CATCH_OUTPUT_REDIRECT_HPP_INCLUDED
#define CATCH_OUTPUT_REDIRECT_HPP_INCLUDED

#include <catch2/internal/catch_unique_ptr.hpp>

#include <cassert>
#include <cstdint>
#include <string>

namespace Catch {

class OutputRedirect {
    bool m_redirectActive = false;
    virtual void activateImpl() = 0;
    virtual void deactivateImpl() = 0;

   public:
    enum Kind : std::uint8_t {
        None,
        Streams,
        FileDescriptors,
    };

    virtual ~OutputRedirect();

    virtual std::string getStdout() = 0;
    virtual std::string getStderr() = 0;
    virtual void clearBuffers() = 0;
    bool isActive() const {
        return m_redirectActive;
    }
    void activate() {
        assert(!m_redirectActive && "redirect is already active");
        activateImpl();
        m_redirectActive = true;
    }
    void deactivate() {
        assert(m_redirectActive && "redirect is not active");
        deactivateImpl();
        m_redirectActive = false;
    }
};

bool isRedirectAvailable(OutputRedirect::Kind kind);
Detail::unique_ptr<OutputRedirect> makeOutputRedirect(bool actual);

class RedirectGuard {
    OutputRedirect* m_redirect;
    bool m_activate;
    bool m_previouslyActive;
    bool m_moved = false;

   public:
    RedirectGuard(bool activate, OutputRedirect& redirectImpl);
    ~RedirectGuard() noexcept(false);

    RedirectGuard(RedirectGuard const&) = delete;
    RedirectGuard& operator=(RedirectGuard const&) = delete;

    RedirectGuard(RedirectGuard&& rhs) noexcept;
    RedirectGuard& operator=(RedirectGuard&& rhs) noexcept;
};

RedirectGuard scopedActivate(OutputRedirect& redirectImpl);
RedirectGuard scopedDeactivate(OutputRedirect& redirectImpl);

}  

#endif  // CATCH_OUTPUT_REDIRECT_HPP_INCLUDED
