#pragma once
#include <algorithm>
#include <cstdint>
#include <memory_resource>
#include <vector>
#include "src/backend/models/rfdetr/core/detail/scipy_rectangular_lsap.h"
namespace mmltk::backend::models::rfdetr {
// One workspace per runtime solver worker, retained through its high-water size.
struct LsapScratch final {
    explicit LsapScratch(std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : costs(resource), solver(resource), row_indices(resource), col_indices(resource), batch_rows(resource), batch_cols(resource) {}
    std::pmr::vector<double> costs;
    RectangularLsApWorkspace solver;
    std::pmr::vector<std::int64_t> row_indices, col_indices, batch_rows, batch_cols;
    void ensure_assignment_capacity(std::int64_t assignment_size, std::int64_t grouped_assignment_size) {
        row_indices.reserve(static_cast<std::size_t>(assignment_size));
        col_indices.reserve(static_cast<std::size_t>(assignment_size));
        batch_rows.reserve(static_cast<std::size_t>(grouped_assignment_size));
        batch_cols.reserve(static_cast<std::size_t>(grouped_assignment_size));
    }
};
}  // namespace mmltk::backend::models::rfdetr
