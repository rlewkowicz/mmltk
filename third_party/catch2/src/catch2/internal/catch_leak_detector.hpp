

#ifndef CATCH_LEAK_DETECTOR_HPP_INCLUDED
#define CATCH_LEAK_DETECTOR_HPP_INCLUDED

namespace Catch {

struct LeakDetector {
    LeakDetector() noexcept;
    ~LeakDetector();
};

}
#endif  // CATCH_LEAK_DETECTOR_HPP_INCLUDED
