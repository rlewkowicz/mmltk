

#ifndef CATCH_RESULT_TYPE_HPP_INCLUDED
#define CATCH_RESULT_TYPE_HPP_INCLUDED

#include <cstdint>

namespace Catch {

struct ResultWas {
    enum OfType : std::int16_t {
        Unknown = -1,
        Ok = 0,
        Info = 1,
        Warning = 2,
        ExplicitSkip = 4,

        FailureBit = 0x10,

        ExpressionFailed = FailureBit | 1,
        ExplicitFailure = FailureBit | 2,

        Exception = 0x100 | FailureBit,

        ThrewException = Exception | 1,
        DidntThrowException = Exception | 2,

        FatalErrorCondition = 0x200 | FailureBit
    };
};

constexpr bool isOk(ResultWas::OfType resultType) {
    return (resultType & ResultWas::FailureBit) == 0;
}
constexpr bool isJustInfo(int flags) {
    return flags == ResultWas::Info;
}

struct ResultDisposition {
    enum Flags : std::uint8_t {
        Normal = 0x01,

        ContinueOnFailure = 0x02,
        FalseTest = 0x04,
        SuppressFail = 0x08
    };
};

constexpr ResultDisposition::Flags operator|(ResultDisposition::Flags lhs, ResultDisposition::Flags rhs) {
    return static_cast<ResultDisposition::Flags>(static_cast<int>(lhs) | static_cast<int>(rhs));
}

constexpr bool isFalseTest(int flags) {
    return (flags & ResultDisposition::FalseTest) != 0;
}
constexpr bool shouldSuppressFailure(int flags) {
    return (flags & ResultDisposition::SuppressFail) != 0;
}

}  

#endif  // CATCH_RESULT_TYPE_HPP_INCLUDED
