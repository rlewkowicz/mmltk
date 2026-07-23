//! This crate implements various functions that help speed up dynamic
//! programming, most importantly the SMAWK algorithm for finding row
//! or column minima in a totally monotone matrix with *m* rows and
//! *n* columns in time O(*m* + *n*). This is much better than the
//! brute force solution which would take O(*mn*). When *m* and *n*
//! are of the same order, this turns a quadratic function into a
//! linear function.
//!
//! # Examples
//!
//! Computing the column minima of an *m* ✕ *n* Monge matrix can be
//! done efficiently with `smawk::column_minima`:
//!
//! ```
//! use smawk::Matrix;
//!
//! let matrix = vec![
//!     vec![3, 2, 4, 5, 6],
//!     vec![2, 1, 3, 3, 4],
//!     vec![2, 1, 3, 3, 4],
//!     vec![3, 2, 4, 3, 4],
//!     vec![4, 3, 2, 1, 1],
//! ];
//! let minima = vec![1, 1, 4, 4, 4];
//! assert_eq!(smawk::column_minima(&matrix), minima);
//! ```
//!
//! The `minima` vector gives the index of the minimum value per
//! column, so `minima[0] == 1` since the minimum value in the first
//! column is 2 (row 1). Note that the smallest row index is returned.
//!
//! # Definitions
//!
//! Some of the functions in this crate only work on matrices that are
//! *totally monotone*, which we will define below.
//!
//! ## Monotone Matrices
//!
//! We start with a helper definition. Given an *m* ✕ *n* matrix `M`,
//! we say that `M` is *monotone* when the minimum value of row `i` is
//! found to the left of the minimum value in row `i'` where `i < i'`.
//!
//! More formally, if we let `rm(i)` denote the column index of the
//! left-most minimum value in row `i`, then we have
//!
//! ```text
//! rm(0) ≤ rm(1) ≤ ... ≤ rm(m - 1)
//! ```
//!
//! This means that as you go down the rows from top to bottom, the
//! row-minima proceed from left to right.
//!
//! The algorithms in this crate deal with finding such row- and
//! column-minima.
//!
//! ## Totally Monotone Matrices
//!
//! We say that a matrix `M` is *totally monotone* when every
//! sub-matrix is monotone. A sub-matrix is formed by the intersection
//! of any two rows `i < i'` and any two columns `j < j'`.
//!
//! This is often expressed as via this equivalent condition:
//!
//! ```text
//! M[i, j] > M[i, j']  =>  M[i', j] > M[i', j']
//! ```
//!
//! for all `i < i'` and `j < j'`.
//!
//! ## Monge Property for Matrices
//!
//! A matrix `M` is said to fulfill the *Monge property* if
//!
//! ```text
//! M[i, j] + M[i', j'] ≤ M[i, j'] + M[i', j]
//! ```
//!
//! for all `i < i'` and `j < j'`. This says that given any rectangle
//! in the matrix, the sum of the top-left and bottom-right corners is
//! less than or equal to the sum of the bottom-left and upper-right
//! corners.
//!
//! All Monge matrices are totally monotone, so it is enough to
//! establish that the Monge property holds in order to use a matrix
//! with the functions in this crate. If your program is dealing with
//! unknown inputs, it can use [`monge::is_monge`] to verify that a
//! matrix is a Monge matrix.

#![doc(html_root_url = "https://docs.rs/smawk/0.3.2")]
#![cfg_attr(not(feature = "ndarray"), forbid(unsafe_code))]

#[cfg(feature = "ndarray")]
pub mod brute_force;
pub mod monge;
#[cfg(feature = "ndarray")]
pub mod recursive;

/// Minimal matrix trait for two-dimensional arrays.
///
/// This provides the functionality needed to represent a read-only
/// numeric matrix. You can query the size of the matrix and access
/// elements. Modeled after [`ndarray::Array2`] from the [ndarray
/// crate](https://crates.io/crates/ndarray).
///
/// Enable the `ndarray` Cargo feature if you want to use it with
/// `ndarray::Array2`.
pub trait Matrix<T: Copy> {
    /// Return the number of rows.
    fn nrows(&self) -> usize;
    /// Return the number of columns.
    fn ncols(&self) -> usize;
    /// Return a matrix element.
    fn index(&self, row: usize, column: usize) -> T;
}

/// Simple and inefficient matrix representation used for doctest
/// examples and simple unit tests.
///
/// You should prefer implementing it yourself, or you can enable the
/// `ndarray` Cargo feature and use the provided implementation for
/// [`ndarray::Array2`].
impl<T: Copy> Matrix<T> for Vec<Vec<T>> {
    fn nrows(&self) -> usize {
        self.len()
    }
    fn ncols(&self) -> usize {
        self[0].len()
    }
    fn index(&self, row: usize, column: usize) -> T {
        self[row][column]
    }
}

/// Adapting [`ndarray::Array2`] to the `Matrix` trait.
///
/// **Note: this implementation is only available if you enable the
/// `ndarray` Cargo feature.**
#[cfg(feature = "ndarray")]
impl<T: Copy> Matrix<T> for ndarray::Array2<T> {
    #[inline]
    fn nrows(&self) -> usize {
        self.nrows()
    }
    #[inline]
    fn ncols(&self) -> usize {
        self.ncols()
    }
    #[inline]
    fn index(&self, row: usize, column: usize) -> T {
        self[[row, column]]
    }
}

/// Compute row minima in O(*m* + *n*) time.
///
/// This implements the [SMAWK algorithm] for efficiently finding row
/// minima in a totally monotone matrix.
///
/// The SMAWK algorithm is from Agarwal, Klawe, Moran, Shor, and
/// Wilbur, *Geometric applications of a matrix searching algorithm*,
/// Algorithmica 2, pp. 195-208 (1987) and the code here is a
/// translation [David Eppstein's Python code][pads].
///
/// Running time on an *m* ✕ *n* matrix: O(*m* + *n*).
///
/// # Examples
///
/// ```
/// use smawk::Matrix;
/// let matrix = vec![vec![4, 2, 4, 3],
///                   vec![5, 3, 5, 3],
///                   vec![5, 3, 3, 1]];
/// assert_eq!(smawk::row_minima(&matrix),
///            vec![1, 1, 3]);
/// ```
///
/// # Panics
///
/// It is an error to call this on a matrix with zero columns.
///
/// [pads]: https://github.com/jfinkels/PADS/blob/master/pads/smawk.py
/// [SMAWK algorithm]: https://en.wikipedia.org/wiki/SMAWK_algorithm
pub fn row_minima<T: PartialOrd + Copy, M: Matrix<T>>(matrix: &M) -> Vec<usize> {
    let mut minima = vec![0; matrix.nrows()];
    smawk_inner(
        &|j, i| matrix.index(i, j),
        &(0..matrix.ncols()).collect::<Vec<_>>(),
        &(0..matrix.nrows()).collect::<Vec<_>>(),
        &mut minima,
    );
    minima
}

#[deprecated(since = "0.3.2", note = "Please use `row_minima` instead.")]
pub fn smawk_row_minima<T: PartialOrd + Copy, M: Matrix<T>>(matrix: &M) -> Vec<usize> {
    row_minima(matrix)
}

/// Compute column minima in O(*m* + *n*) time.
///
/// This implements the [SMAWK algorithm] for efficiently finding
/// column minima in a totally monotone matrix.
///
/// The SMAWK algorithm is from Agarwal, Klawe, Moran, Shor, and
/// Wilbur, *Geometric applications of a matrix searching algorithm*,
/// Algorithmica 2, pp. 195-208 (1987) and the code here is a
/// translation [David Eppstein's Python code][pads].
///
/// Running time on an *m* ✕ *n* matrix: O(*m* + *n*).
///
/// # Examples
///
/// ```
/// use smawk::Matrix;
/// let matrix = vec![vec![4, 2, 4, 3],
///                   vec![5, 3, 5, 3],
///                   vec![5, 3, 3, 1]];
/// assert_eq!(smawk::column_minima(&matrix),
///            vec![0, 0, 2, 2]);
/// ```
///
/// # Panics
///
/// It is an error to call this on a matrix with zero rows.
///
/// [SMAWK algorithm]: https://en.wikipedia.org/wiki/SMAWK_algorithm
/// [pads]: https://github.com/jfinkels/PADS/blob/master/pads/smawk.py
pub fn column_minima<T: PartialOrd + Copy, M: Matrix<T>>(matrix: &M) -> Vec<usize> {
    let mut minima = vec![0; matrix.ncols()];
    smawk_inner(
        &|i, j| matrix.index(i, j),
        &(0..matrix.nrows()).collect::<Vec<_>>(),
        &(0..matrix.ncols()).collect::<Vec<_>>(),
        &mut minima,
    );
    minima
}

#[deprecated(since = "0.3.2", note = "Please use `column_minima` instead.")]
pub fn smawk_column_minima<T: PartialOrd + Copy, M: Matrix<T>>(matrix: &M) -> Vec<usize> {
    column_minima(matrix)
}

/// Compute column minima in the given area of the matrix. The
/// `minima` slice is updated inplace.
fn smawk_inner<T: PartialOrd + Copy, M: Fn(usize, usize) -> T>(
    matrix: &M,
    rows: &[usize],
    cols: &[usize],
    minima: &mut [usize],
) {
    if cols.is_empty() {
        return;
    }

    let mut stack = Vec::with_capacity(cols.len());
    for r in rows {
        while !stack.is_empty()
            && matrix(stack[stack.len() - 1], cols[stack.len() - 1])
                > matrix(*r, cols[stack.len() - 1])
        {
            stack.pop();
        }
        if stack.len() != cols.len() {
            stack.push(*r);
        }
    }
    let rows = &stack;

    let mut odd_cols = Vec::with_capacity(1 + cols.len() / 2);
    for (idx, c) in cols.iter().enumerate() {
        if idx % 2 == 1 {
            odd_cols.push(*c);
        }
    }

    smawk_inner(matrix, rows, &odd_cols, minima);

    let mut r = 0;
    for (c, &col) in cols.iter().enumerate().filter(|(c, _)| c % 2 == 0) {
        let mut row = rows[r];
        let last_row = if c == cols.len() - 1 {
            rows[rows.len() - 1]
        } else {
            minima[cols[c + 1]]
        };
        let mut pair = (matrix(row, col), row);
        while row != last_row {
            r += 1;
            row = rows[r];
            if (matrix(row, col), row) < pair {
                pair = (matrix(row, col), row);
            }
        }
        minima[col] = pair.1;
    }
}

/// Compute upper-right column minima in O(*m* + *n*) time.
///
/// The input matrix must be totally monotone.
///
/// The function returns a vector of `(usize, T)`. The `usize` in the
/// tuple at index `j` tells you the row of the minimum value in
/// column `j` and the `T` value is minimum value itself.
///
/// The algorithm only considers values above the main diagonal, which
/// means that it computes values `v(j)` where:
///
/// ```text
/// v(0) = initial
/// v(j) = min { M[i, j] | i < j } for j > 0
/// ```
///
/// If we let `r(j)` denote the row index of the minimum value in
/// column `j`, the tuples in the result vector become `(r(j), M[r(j),
/// j])`.
///
/// The algorithm is an *online* algorithm, in the sense that `matrix`
/// function can refer back to previously computed column minima when
/// determining an entry in the matrix. The guarantee is that we only
/// call `matrix(i, j)` after having computed `v(i)`. This is
/// reflected in the `&[(usize, T)]` argument to `matrix`, which grows
/// as more and more values are computed.
pub fn online_column_minima<T: Copy + PartialOrd, M: Fn(&[(usize, T)], usize, usize) -> T>(
    initial: T,
    size: usize,
    matrix: M,
) -> Vec<(usize, T)> {
    let mut result = vec![(0, initial)];

    let mut finished = 0;
    let mut base = 0;
    let mut tentative = 0;

    macro_rules! m {
        ($i:expr, $j:expr) => {{
            assert!($i < $j, "(i, j) not above diagonal: ({}, {})", $i, $j);
            assert!(
                $i < size && $j < size,
                "(i, j) out of bounds: ({}, {}), size: {}",
                $i,
                $j,
                size
            );
            matrix(&result[..finished + 1], $i, $j)
        }};
    }

    while finished < size - 1 {
        let i = finished + 1;
        if i > tentative {
            let rows = (base..finished + 1).collect::<Vec<_>>();
            tentative = std::cmp::min(finished + rows.len(), size - 1);
            let cols = (finished + 1..tentative + 1).collect::<Vec<_>>();
            let mut minima = vec![0; tentative + 1];
            smawk_inner(&|i, j| m![i, j], &rows, &cols, &mut minima);
            for col in cols {
                let row = minima[col];
                let v = m![row, col];
                if col >= result.len() {
                    result.push((row, v));
                } else if v < result[col].1 {
                    result[col] = (row, v);
                }
            }
            finished = i;
            continue;
        }

        let diag = m![i - 1, i];
        if diag < result[i].1 {
            result[i] = (i - 1, diag);
            base = i - 1;
            tentative = i;
            finished = i;
            continue;
        }

        if m![i - 1, tentative] >= result[tentative].1 {
            finished = i;
            continue;
        }

        base = i - 1;
        tentative = i;
        finished = i;
    }

    result
}
