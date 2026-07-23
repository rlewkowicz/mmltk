
#include "rfdetr/utils/scipy_rectangular_lsap.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <utility>
#include <vector>

namespace mmltk::rfdetr {

namespace {

template <typename T>
void argsort_iter(const std::vector<T>& values, std::vector<intptr_t>& index) {
    index.resize(values.size());
    std::iota(index.begin(), index.end(), 0);
    std::ranges::sort(index, [&values](intptr_t lhs, intptr_t rhs) {
        return values[static_cast<size_t>(lhs)] < values[static_cast<size_t>(rhs)];
    });
}

intptr_t augmenting_path(intptr_t num_cols, double* cost, std::vector<double>& u, std::vector<double>& v,
                         std::vector<intptr_t>& path, std::vector<intptr_t>& row_for_col,
                         std::vector<double>& shortest_path_costs, intptr_t row, std::vector<bool>& visited_rows,
                         std::vector<bool>& visited_cols, std::vector<intptr_t>& remaining, double* min_value) {
    double current_min = 0.0;
    intptr_t num_remaining = num_cols;
    for (intptr_t i = 0; i < num_cols; ++i) {
        remaining[static_cast<size_t>(i)] = num_cols - i - 1;
    }

    std::fill(visited_rows.begin(), visited_rows.end(), false);
    std::fill(visited_cols.begin(), visited_cols.end(), false);
    std::fill(shortest_path_costs.begin(), shortest_path_costs.end(), std::numeric_limits<double>::infinity());

    intptr_t sink = -1;
    while (sink == -1) {
        intptr_t index = -1;
        double lowest = std::numeric_limits<double>::infinity();
        visited_rows[static_cast<size_t>(row)] = true;

        for (intptr_t i = 0; i < num_remaining; ++i) {
            const intptr_t col = remaining[static_cast<size_t>(i)];
            const double reduced_cost =
                current_min + cost[row * num_cols + col] - u[static_cast<size_t>(row)] - v[static_cast<size_t>(col)];
            if (reduced_cost < shortest_path_costs[static_cast<size_t>(col)]) {
                path[static_cast<size_t>(col)] = row;
                shortest_path_costs[static_cast<size_t>(col)] = reduced_cost;
            }
            if (shortest_path_costs[static_cast<size_t>(col)] < lowest ||
                (shortest_path_costs[static_cast<size_t>(col)] == lowest &&
                 row_for_col[static_cast<size_t>(col)] == -1)) {
                lowest = shortest_path_costs[static_cast<size_t>(col)];
                index = i;
            }
        }

        current_min = lowest;
        if (current_min == std::numeric_limits<double>::infinity()) {
            return -1;
        }

        const intptr_t col = remaining[static_cast<size_t>(index)];
        if (row_for_col[static_cast<size_t>(col)] == -1) {
            sink = col;
        } else {
            row = row_for_col[static_cast<size_t>(col)];
        }

        visited_cols[static_cast<size_t>(col)] = true;
        remaining[static_cast<size_t>(index)] = remaining[static_cast<size_t>(--num_remaining)];
    }

    *min_value = current_min;
    return sink;
}

RectangularLsApStatus solve_impl(intptr_t num_rows, intptr_t num_cols, double* input_cost, bool maximize,
                                 int64_t* row_indices, int64_t* col_indices, RectangularLsApWorkspace& workspace) {
    if (num_rows == 0 || num_cols == 0) {
        return RectangularLsApStatus::kOk;
    }

    bool transpose = num_cols < num_rows;
    auto& temp = workspace.transposed_cost;
    if (transpose || maximize) {
        temp.resize(static_cast<size_t>(num_rows * num_cols));
        if (transpose) {
            for (intptr_t row = 0; row < num_rows; ++row) {
                for (intptr_t col = 0; col < num_cols; ++col) {
                    temp[static_cast<size_t>(col * num_rows + row)] =
                        input_cost[static_cast<size_t>(row * num_cols + col)];
                }
            }
            std::swap(num_rows, num_cols);
        } else {
            std::ranges::copy_n(input_cost, num_rows * num_cols, temp.begin());
        }
        if (maximize) {
            for (double& value : temp) {
                value = -value;
            }
        }
        input_cost = temp.data();
    }

    for (intptr_t i = 0; i < num_rows * num_cols; ++i) {
        const double value = input_cost[static_cast<size_t>(i)];
        if (value != value || value == -std::numeric_limits<double>::infinity()) {
            return RectangularLsApStatus::kInvalid;
        }
    }

    auto& u = workspace.row_duals;
    auto& v = workspace.column_duals;
    auto& shortest_path_costs = workspace.shortest_path_costs;
    auto& path = workspace.path;
    auto& col_for_row = workspace.column_for_row;
    auto& row_for_col = workspace.row_for_column;
    auto& visited_rows = workspace.visited_rows;
    auto& visited_cols = workspace.visited_columns;
    auto& remaining = workspace.remaining_columns;
    u.assign(static_cast<size_t>(num_rows), 0.0);
    v.assign(static_cast<size_t>(num_cols), 0.0);
    shortest_path_costs.resize(static_cast<size_t>(num_cols));
    path.assign(static_cast<size_t>(num_cols), -1);
    col_for_row.assign(static_cast<size_t>(num_rows), -1);
    row_for_col.assign(static_cast<size_t>(num_cols), -1);
    visited_rows.resize(static_cast<size_t>(num_rows));
    visited_cols.resize(static_cast<size_t>(num_cols));
    remaining.resize(static_cast<size_t>(num_cols));

    for (intptr_t row = 0; row < num_rows; ++row) {
        double min_value = 0.0;
        const intptr_t sink = augmenting_path(num_cols, input_cost, u, v, path, row_for_col, shortest_path_costs, row,
                                              visited_rows, visited_cols, remaining, &min_value);
        if (sink < 0) {
            return RectangularLsApStatus::kInfeasible;
        }

        u[static_cast<size_t>(row)] += min_value;
        for (intptr_t i = 0; i < num_rows; ++i) {
            if (visited_rows[static_cast<size_t>(i)] && i != row) {
                u[static_cast<size_t>(i)] +=
                    min_value - shortest_path_costs[static_cast<size_t>(col_for_row[static_cast<size_t>(i)])];
            }
        }
        for (intptr_t j = 0; j < num_cols; ++j) {
            if (visited_cols[static_cast<size_t>(j)]) {
                v[static_cast<size_t>(j)] -= min_value - shortest_path_costs[static_cast<size_t>(j)];
            }
        }

        intptr_t col = sink;
        while (true) {
            const intptr_t matched_row = path[static_cast<size_t>(col)];
            row_for_col[static_cast<size_t>(col)] = matched_row;
            std::swap(col_for_row[static_cast<size_t>(matched_row)], col);
            if (matched_row == row) {
                break;
            }
        }
    }

    if (transpose) {
        intptr_t out = 0;
        argsort_iter(col_for_row, workspace.sorted_indices);
        for (const intptr_t row : workspace.sorted_indices) {
            row_indices[out] = static_cast<int64_t>(col_for_row[static_cast<size_t>(row)]);
            col_indices[out] = static_cast<int64_t>(row);
            ++out;
        }
    } else {
        for (intptr_t row = 0; row < num_rows; ++row) {
            row_indices[row] = static_cast<int64_t>(row);
            col_indices[row] = static_cast<int64_t>(col_for_row[static_cast<size_t>(row)]);
        }
    }
    return RectangularLsApStatus::kOk;
}

}  

RectangularLsApStatus solve_rectangular_linear_sum_assignment(int64_t num_rows, int64_t num_cols,
                                                              const double* cost_matrix, bool maximize,
                                                              int64_t* row_indices, int64_t* col_indices,
                                                              RectangularLsApWorkspace& workspace) {
    return solve_impl(static_cast<intptr_t>(num_rows), static_cast<intptr_t>(num_cols),
                      const_cast<double*>(cost_matrix), maximize, row_indices, col_indices, workspace);
}

}  
