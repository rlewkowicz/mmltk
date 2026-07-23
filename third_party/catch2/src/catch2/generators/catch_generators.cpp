

#include <catch2/generators/catch_generators.hpp>
#include <catch2/interfaces/catch_interfaces_capture.hpp>

namespace Catch {

IGeneratorTracker::~IGeneratorTracker() = default;

namespace Generators {

GeneratorUntypedBase::~GeneratorUntypedBase() = default;

IGeneratorTracker* acquireGeneratorTracker(StringRef generatorName, SourceLineInfo const& lineInfo) {
    return getResultCapture().acquireGeneratorTracker(generatorName, lineInfo);
}

IGeneratorTracker* createGeneratorTracker(StringRef generatorName, SourceLineInfo lineInfo,
                                          GeneratorBasePtr&& generator) {
    return getResultCapture().createGeneratorTracker(generatorName, lineInfo, CATCH_MOVE(generator));
}

}  
}  
