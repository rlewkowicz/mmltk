#pragma once

#include <Python.h>

#include <exception>
#include <optional>
#include <stdexcept>
#include <vector>

namespace rapidgzip {
[[nodiscard]] bool pythonIsFinalizing() {
#if (PY_MAJOR_VERSION != 3) || (PY_MINOR_VERSION < 8)
    return false;
#elif PY_MINOR_VERSION < 13
    return _Py_IsFinalizing();
#else
    return Py_IsFinalizing();
#endif
}

class PythonExceptionThrownBySignal : public std::runtime_error {
   public:
    PythonExceptionThrownBySignal()
        : std::runtime_error("An exception has been thrown while checking the Python signal handler.") {}
};

class ScopedGIL {
   public:
    struct GILState {
        bool locked;
        bool exists;
    };

   public:
    explicit ScopedGIL(bool doLock) {
        m_referenceCounters.emplace_back(apply({doLock, true}));
    }

    ~ScopedGIL() noexcept {
        if (m_referenceCounters.empty()) {
            std::cerr << "Logic error: It seems there were more unlocks than locks!\n";
            std::terminate();
        }

        apply(m_referenceCounters.back());
        m_referenceCounters.pop_back();
    }

    ScopedGIL(const ScopedGIL&) = delete;
    ScopedGIL(ScopedGIL&&) = delete;
    ScopedGIL& operator=(const ScopedGIL&) = delete;
    ScopedGIL& operator=(ScopedGIL&&) = delete;

   private:
    GILState apply(const GILState targetState) noexcept {
        const auto doLock = targetState.locked;
        if (!doLock && pythonIsFinalizing()) {
            return {false, false};
        }

        if (targetState.locked && !targetState.exists) {
            std::cerr << "Invalid GIL target state, which should be locked but not exist at the same time!\n";
            std::terminate();
        }

        static thread_local bool isLocked{PyGILState_Check() == 1};

        static thread_local std::optional<PyGILState_STATE> lockState{};
        static thread_local PyThreadState* unlockState{nullptr};

        if (pythonIsFinalizing() || (isLocked && (PyGILState_Check() == 0))) {
            if ((PyGILState_Check() == 1) && lockState.has_value()) {
                PyGILState_Release(*lockState);
                lockState.reset();
            }
            std::cerr << "Detected Python finalization from running rapidgzip thread.\n"
                         "To avoid this exception you should close all RapidgzipFile objects correctly,\n"
                         "or better, use the with-statement if possible to automatically close it.\n";
            std::terminate();
        }

        const auto wasLocked = isLocked;
        if (isLocked == doLock) {
            return {wasLocked, true};
        }

        PyThreadState* threadState{nullptr};
#ifdef PYPY_VERSION_NUM
        threadState = _PyThreadState_UncheckedGet();
#else
        threadState = PyGILState_GetThisThreadState();
#endif

        const auto gilExists = threadState != nullptr;
        if (doLock) {
            if (gilExists) {
                PyEval_RestoreThread(unlockState == nullptr ? threadState : unlockState);
                unlockState = nullptr;
            } else {
                lockState.emplace(PyGILState_Ensure());
            }
        } else {
            if (!targetState.exists && lockState.has_value()) {
                PyGILState_Release(*lockState);
                lockState.reset();
            } else {
                unlockState = PyEval_SaveThread();
            }
        }

        isLocked = doLock;
        return {wasLocked, gilExists};
    }

   private:
    inline static thread_local std::vector<GILState> m_referenceCounters;
};

class ScopedGILLock : public ScopedGIL {
   public:
    ScopedGILLock() : ScopedGIL(true) {}
};

class ScopedGILUnlock : public ScopedGIL {
   public:
    ScopedGILUnlock() : ScopedGIL(false) {}
};

void checkPythonSignalHandlers() {
    const ScopedGILLock gilLock;

    for (auto result = PyErr_CheckSignals(); result != 0; result = PyErr_CheckSignals()) {
        if (PyErr_Occurred() != nullptr) {
            throw PythonExceptionThrownBySignal();
        }
    }
}
}
