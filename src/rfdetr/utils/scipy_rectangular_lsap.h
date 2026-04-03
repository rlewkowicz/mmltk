#pragma once

#include <cstdint>

namespace mmltk::rfdetr {

enum class RectangularLsApStatus : std::uint8_t {
    kOk = 0,
    kInfeasible = 1,
    kInvalid = 2,
};

RectangularLsApStatus solve_rectangular_linear_sum_assignment(int64_t num_rows,
                                                             int64_t num_cols,
                                                             const double* cost_matrix,
                                                             bool maximize,
                                                             int64_t* row_indices,
                                                             int64_t* col_indices);

} // namespace mmltk::rfdetr
