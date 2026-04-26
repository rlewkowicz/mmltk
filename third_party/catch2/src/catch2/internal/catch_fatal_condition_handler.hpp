
//              Copyright Catch2 Authors
// Distributed under the Boost Software License, Version 1.0.
//   (See accompanying file LICENSE.txt or copy at
//        https://www.boost.org/LICENSE_1_0.txt)

// SPDX-License-Identifier: BSL-1.0
#ifndef CATCH_FATAL_CONDITION_HANDLER_HPP_INCLUDED
#define CATCH_FATAL_CONDITION_HANDLER_HPP_INCLUDED

#include <cassert>

namespace Catch {

class FatalConditionHandler {
    bool m_started = false;

    void engage_platform();
    void disengage_platform() noexcept;

   public:
    FatalConditionHandler();
    ~FatalConditionHandler();

    void engage() {
        assert(!m_started && "Handler cannot be installed twice.");
        m_started = true;
        engage_platform();
    }

    void disengage() noexcept {
        assert(m_started && "Handler cannot be uninstalled without being installed first");
        m_started = false;
        disengage_platform();
    }
};

class FatalConditionHandlerGuard {
    FatalConditionHandler* m_handler;

   public:
    FatalConditionHandlerGuard(FatalConditionHandler* handler) : m_handler(handler) {
        m_handler->engage();
    }
    ~FatalConditionHandlerGuard() {
        m_handler->disengage();
    }
};

}  // namespace Catch

#endif  // CATCH_FATAL_CONDITION_HANDLER_HPP_INCLUDED
