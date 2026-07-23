

#ifndef CATCH_OUTLIER_CLASSIFICATION_HPP_INCLUDED
#define CATCH_OUTLIER_CLASSIFICATION_HPP_INCLUDED

namespace Catch {
namespace Benchmark {
struct OutlierClassification {
    int samples_seen = 0;
    int low_severe = 0;
    int low_mild = 0;
    int high_mild = 0;
    int high_severe = 0;

    constexpr int total() const {
        return low_severe + low_mild + high_mild + high_severe;
    }
};
}  
}  

#endif  // CATCH_OUTLIERS_CLASSIFICATION_HPP_INCLUDED
