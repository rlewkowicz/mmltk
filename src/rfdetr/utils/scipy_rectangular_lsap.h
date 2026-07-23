#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace mmltk::rfdetr {

enum class RectangularLsApStatus : std::uint8_t {
    kOk = 0,
    kInfeasible = 1,
    kInvalid = 2,
};

struct RectangularLsApWorkspace {
    std::vector<double> transposed_cost;
    std::vector<double> row_duals;
    std::vector<double> column_duals;
    std::vector<double> shortest_path_costs;
    std::vector<std::intptr_t> path;
    std::vector<std::intptr_t> column_for_row;
    std::vector<std::intptr_t> row_for_column;
    std::vector<bool> visited_rows;
    std::vector<bool> visited_columns;
    std::vector<std::intptr_t> remaining_columns;
    std::vector<std::intptr_t> sorted_indices;
};

RectangularLsApStatus solve_rectangular_linear_sum_assignment(int64_t num_rows, int64_t num_cols,
                                                              const double* cost_matrix, bool maximize,
                                                              int64_t* row_indices, int64_t* col_indices,
                                                              RectangularLsApWorkspace& workspace);

}  
