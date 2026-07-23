

#include <catch2/reporters/catch_reporter_registrars.hpp>

#include <catch2/interfaces/catch_interfaces_registry_hub.hpp>
#include <catch2/internal/catch_compiler_capabilities.hpp>

namespace Catch {
namespace Detail {

void registerReporterImpl(std::string const& name, IReporterFactoryPtr reporterPtr) {
    CATCH_TRY {
        getMutableRegistryHub().registerReporter(name, CATCH_MOVE(reporterPtr));
    }
    CATCH_CATCH_ALL {
        getMutableRegistryHub().registerStartupException();
    }
}

void registerListenerImpl(Detail::unique_ptr<EventListenerFactory> listenerFactory) {
    getMutableRegistryHub().registerListener(CATCH_MOVE(listenerFactory));
}

}  
}  
