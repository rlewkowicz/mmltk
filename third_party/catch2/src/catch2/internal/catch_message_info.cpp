

#include <catch2/internal/catch_message_info.hpp>
#include <catch2/internal/catch_thread_local.hpp>

namespace Catch {

namespace {
static CATCH_INTERNAL_THREAD_LOCAL unsigned int messageIDCounter = 0;
}

MessageInfo::MessageInfo(StringRef _macroName, SourceLineInfo const& _lineInfo, ResultWas::OfType _type)
    : macroName(_macroName), lineInfo(_lineInfo), type(_type), sequence(++messageIDCounter) {}

}  
