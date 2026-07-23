

#ifndef CATCH_IS_PERMUTATION_HPP_INCLUDED
#define CATCH_IS_PERMUTATION_HPP_INCLUDED

#include <iterator>
#include <type_traits>

namespace Catch {
namespace Detail {

template <typename ForwardIter, typename Sentinel, typename T, typename Comparator>
constexpr ForwardIter find_sentinel(ForwardIter start, Sentinel sentinel, T const& value, Comparator cmp) {
    while (start != sentinel) {
        if (cmp(*start, value)) {
            break;
        }
        ++start;
    }
    return start;
}

template <typename ForwardIter, typename Sentinel, typename T, typename Comparator>
constexpr std::ptrdiff_t count_sentinel(ForwardIter start, Sentinel sentinel, T const& value, Comparator cmp) {
    std::ptrdiff_t count = 0;
    while (start != sentinel) {
        if (cmp(*start, value)) {
            ++count;
        }
        ++start;
    }
    return count;
}

template <typename ForwardIter, typename Sentinel>
constexpr std::enable_if_t<!std::is_same<ForwardIter, Sentinel>::value, std::ptrdiff_t> sentinel_distance(
    ForwardIter iter, const Sentinel sentinel) {
    std::ptrdiff_t dist = 0;
    while (iter != sentinel) {
        ++iter;
        ++dist;
    }
    return dist;
}

template <typename ForwardIter>
constexpr std::ptrdiff_t sentinel_distance(ForwardIter first, ForwardIter last) {
    return std::distance(first, last);
}

template <typename ForwardIter1, typename Sentinel1, typename ForwardIter2, typename Sentinel2, typename Comparator>
constexpr bool check_element_counts(ForwardIter1 first_1, const Sentinel1 end_1, ForwardIter2 first_2,
                                    const Sentinel2 end_2, Comparator cmp) {
    auto cursor = first_1;
    while (cursor != end_1) {
        if (find_sentinel(first_1, cursor, *cursor, cmp) == cursor) {
            const auto count_in_range_2 = count_sentinel(first_2, end_2, *cursor, cmp);
            if (count_in_range_2 == 0) {
                return false;
            }

            const auto count_in_range_1 = count_sentinel(cursor, end_1, *cursor, cmp);
            if (count_in_range_1 != count_in_range_2) {
                return false;
            }
        }

        ++cursor;
    }

    return true;
}

template <typename ForwardIter1, typename Sentinel1, typename ForwardIter2, typename Sentinel2, typename Comparator>
constexpr bool is_permutation(ForwardIter1 first_1, const Sentinel1 end_1, ForwardIter2 first_2, const Sentinel2 end_2,
                              Comparator cmp) {
    while (first_1 != end_1 && first_2 != end_2 && cmp(*first_1, *first_2)) {
        ++first_1;
        ++first_2;
    }

    if (first_1 == end_1 || first_2 == end_2) {
        return first_1 == end_1 && first_2 == end_2;
    }

    auto dist_1 = sentinel_distance(first_1, end_1);
    auto dist_2 = sentinel_distance(first_2, end_2);

    if (dist_1 != dist_2) {
        return false;
    }

    return check_element_counts(first_1, end_1, first_2, end_2, cmp);
}

}  
}  

#endif  // CATCH_IS_PERMUTATION_HPP_INCLUDED
