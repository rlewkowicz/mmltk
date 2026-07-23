

#ifndef CATCH_ISTREAM_HPP_INCLUDED
#define CATCH_ISTREAM_HPP_INCLUDED

#include <catch2/internal/catch_noncopyable.hpp>
#include <catch2/internal/catch_unique_ptr.hpp>

#include <iosfwd>
#include <string>

namespace Catch {

class IStream {
   public:
    virtual ~IStream();
    virtual std::ostream& stream() = 0;
    virtual bool isConsole() const {
        return false;
    }
};

auto makeStream(std::string const& filename) -> Detail::unique_ptr<IStream>;

}  

#endif  // CATCH_STREAM_HPP_INCLUDED
