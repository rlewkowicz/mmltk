

#include <catch2/catch_translate_exception.hpp>
#include <catch2/interfaces/catch_interfaces_registry_hub.hpp>

namespace Catch {
namespace Detail {
void registerTranslatorImpl(Detail::unique_ptr<IExceptionTranslator>&& translator) {
    getMutableRegistryHub().registerTranslator(CATCH_MOVE(translator));
}
}  
}  
