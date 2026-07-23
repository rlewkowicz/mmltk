

#ifndef CATCH_INTERFACES_REPORTER_FACTORY_HPP_INCLUDED
#define CATCH_INTERFACES_REPORTER_FACTORY_HPP_INCLUDED

#include <catch2/internal/catch_unique_ptr.hpp>
#include <catch2/internal/catch_stringref.hpp>

#include <string>

namespace Catch {

struct ReporterConfig;
class IConfig;
class IEventListener;
using IEventListenerPtr = Detail::unique_ptr<IEventListener>;

class IReporterFactory {
   public:
    virtual ~IReporterFactory();

    virtual IEventListenerPtr create(ReporterConfig&& config) const = 0;
    virtual std::string getDescription() const = 0;
};
using IReporterFactoryPtr = Detail::unique_ptr<IReporterFactory>;

class EventListenerFactory {
   public:
    virtual ~EventListenerFactory();
    virtual IEventListenerPtr create(IConfig const* config) const = 0;
    virtual StringRef getName() const = 0;
    virtual std::string getDescription() const = 0;
};
}  

#endif  // CATCH_INTERFACES_REPORTER_FACTORY_HPP_INCLUDED
