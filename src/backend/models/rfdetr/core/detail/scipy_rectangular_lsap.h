#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <memory_resource>
namespace mmltk::backend::models::rfdetr {
enum class RectangularLsApStatus : std::uint8_t {
    kOk = 0,
    kInfeasible = 1,
    kInvalid = 2,
};
struct RectangularLsApWorkspace {
    explicit RectangularLsApWorkspace(std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : transposed_cost(resource),
          row_duals(resource),
          column_duals(resource),
          shortest_path_costs(resource),
          path(resource),
          column_for_row(resource),
          row_for_column(resource),
          visited_rows(resource),
          visited_columns(resource),
          remaining_columns(resource),
          sorted_indices(resource) {}
    std::pmr::vector<double> transposed_cost;
    std::pmr::vector<double> row_duals;
    std::pmr::vector<double> column_duals;
    std::pmr::vector<double> shortest_path_costs;
    std::pmr::vector<std::intptr_t> path;
    std::pmr::vector<std::intptr_t> column_for_row;
    std::pmr::vector<std::intptr_t> row_for_column;
    std::pmr::vector<bool> visited_rows;
    std::pmr::vector<bool> visited_columns;
    std::pmr::vector<std::intptr_t> remaining_columns;
    std::pmr::vector<std::intptr_t> sorted_indices;
};
RectangularLsApStatus solve_rectangular_linear_sum_assignment(int64_t num_rows, int64_t num_cols, const double* cost_matrix, bool maximize,
                                                              int64_t* row_indices, int64_t* col_indices, RectangularLsApWorkspace& workspace);
}  // namespace mmltk::backend::models::rfdetr
