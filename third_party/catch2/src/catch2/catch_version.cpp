

#include <catch2/catch_version.hpp>
#include <ostream>

namespace Catch {

Version::Version(unsigned int _majorVersion, unsigned int _minorVersion, unsigned int _patchNumber,
                 char const* const _branchName, unsigned int _buildNumber)
    : majorVersion(_majorVersion),
      minorVersion(_minorVersion),
      patchNumber(_patchNumber),
      branchName(_branchName),
      buildNumber(_buildNumber) {}

std::ostream& operator<<(std::ostream& os, Version const& version) {
    os << version.majorVersion << '.' << version.minorVersion << '.' << version.patchNumber;
    if (version.branchName[0]) {
        os << '-' << version.branchName << '.' << version.buildNumber;
    }
    return os;
}

Version const& libraryVersion() {
    static Version version(3, 13, 0, "", 0);
    return version;
}

}  
