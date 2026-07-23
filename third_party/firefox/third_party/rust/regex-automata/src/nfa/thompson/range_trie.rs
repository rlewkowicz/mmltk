
use core::{cell::RefCell, fmt, mem, ops::RangeInclusive};

use alloc::{format, string::String, vec, vec::Vec};

use regex_syntax::utf8::Utf8Range;

use crate::util::primitives::StateID;

/// There is only one final state in this trie. Every sequence of byte ranges
/// added shares the same final state.
const FINAL: StateID = StateID::ZERO;

/// The root state of the trie.
const ROOT: StateID = StateID::new_unchecked(1);

/// A range trie represents an ordered set of sequences of bytes.
///
/// A range trie accepts as input a sequence of byte ranges and merges
/// them into the existing set such that the trie can produce a sorted
/// non-overlapping sequence of byte ranges. The sequence emitted corresponds
/// precisely to the sequence of bytes matched by the given keys, although the
/// byte ranges themselves may be split at different boundaries.
///
/// The order complexity of this data structure seems difficult to analyze.
/// If the size of a byte is held as a constant, then insertion is clearly
/// O(n) where n is the number of byte ranges in the input key. However, if
/// k=256 is our alphabet size, then insertion could be O(k^2 * n). In
/// particular it seems possible for pathological inputs to cause insertion
/// to do a lot of work. However, for what we use this data structure for,
/// there should be no pathological inputs since the ultimate source is always
/// a sorted set of Unicode scalar value ranges.
///
/// Internally, this trie is setup like a finite state machine. Note though
/// that it is acyclic.
#[derive(Clone)]
pub struct RangeTrie {
    /// The states in this trie. The first is always the shared final state.
    /// The second is always the root state. Otherwise, there is no
    /// particular order.
    states: Vec<State>,
    /// A free-list of states. When a range trie is cleared, all of its states
    /// are added to this list. Creating a new state reuses states from this
    /// list before allocating a new one.
    free: Vec<State>,
    /// A stack for traversing this trie to yield sequences of byte ranges in
    /// lexicographic order.
    iter_stack: RefCell<Vec<NextIter>>,
    /// A buffer that stores the current sequence during iteration.
    iter_ranges: RefCell<Vec<Utf8Range>>,
    /// A stack used for traversing the trie in order to (deeply) duplicate
    /// a state. States are recursively duplicated when ranges are split.
    dupe_stack: Vec<NextDupe>,
    /// A stack used for traversing the trie during insertion of a new
    /// sequence of byte ranges.
    insert_stack: Vec<NextInsert>,
}

/// A single state in this trie.
#[derive(Clone)]
struct State {
    /// A sorted sequence of non-overlapping transitions to other states. Each
    /// transition corresponds to a single range of bytes.
    transitions: Vec<Transition>,
}

/// A transition is a single range of bytes. If a particular byte is in this
/// range, then the corresponding machine may transition to the state pointed
/// to by `next_id`.
#[derive(Clone)]
struct Transition {
    /// The byte range.
    range: Utf8Range,
    /// The next state to transition to.
    next_id: StateID,
}

impl RangeTrie {
    /// Create a new empty range trie.
    pub fn new() -> RangeTrie {
        let mut trie = RangeTrie {
            states: vec![],
            free: vec![],
            iter_stack: RefCell::new(vec![]),
            iter_ranges: RefCell::new(vec![]),
            dupe_stack: vec![],
            insert_stack: vec![],
        };
        trie.clear();
        trie
    }

    /// Clear this range trie such that it is empty. Clearing a range trie
    /// and reusing it can beneficial because this may reuse allocations.
    pub fn clear(&mut self) {
        self.free.append(&mut self.states);
        self.add_empty(); 
        self.add_empty(); 
    }

    /// Iterate over all of the sequences of byte ranges in this trie, and
    /// call the provided function for each sequence. Iteration occurs in
    /// lexicographic order.
    pub fn iter<E, F: FnMut(&[Utf8Range]) -> Result<(), E>>(
        &self,
        mut f: F,
    ) -> Result<(), E> {
        let mut stack = self.iter_stack.borrow_mut();
        stack.clear();
        let mut ranges = self.iter_ranges.borrow_mut();
        ranges.clear();

        stack.push(NextIter { state_id: ROOT, tidx: 0 });
        while let Some(NextIter { mut state_id, mut tidx }) = stack.pop() {
            loop {
                let state = self.state(state_id);
                if tidx >= state.transitions.len() {
                    ranges.pop();
                    break;
                }

                let t = &state.transitions[tidx];
                ranges.push(t.range);
                if t.next_id == FINAL {
                    f(&ranges)?;
                    ranges.pop();
                    tidx += 1;
                } else {
                    stack.push(NextIter { state_id, tidx: tidx + 1 });
                    state_id = t.next_id;
                    tidx = 0;
                }
            }
        }
        Ok(())
    }

    /// Inserts a new sequence of ranges into this trie.
    ///
    /// The sequence given must be non-empty and must not have a length
    /// exceeding 4.
    pub fn insert(&mut self, ranges: &[Utf8Range]) {
        assert!(!ranges.is_empty());
        assert!(ranges.len() <= 4);

        let mut stack = core::mem::replace(&mut self.insert_stack, vec![]);
        stack.clear();

        stack.push(NextInsert::new(ROOT, ranges));
        while let Some(next) = stack.pop() {
            let (state_id, ranges) = (next.state_id(), next.ranges());
            assert!(!ranges.is_empty());

            let (mut new, rest) = (ranges[0], &ranges[1..]);

            // corresponding to the partitions generated by splitting the
            let mut i = self.state(state_id).find(new);

            if i == self.state(state_id).transitions.len() {
                let next_id = NextInsert::push(self, &mut stack, rest);
                self.add_transition(state_id, new, next_id);
                continue;
            }

            'OUTER: loop {
                let old = self.state(state_id).transitions[i].clone();
                let split = match Split::new(old.range, new) {
                    Some(split) => split,
                    None => {
                        let next_id = NextInsert::push(self, &mut stack, rest);
                        self.add_transition_at(i, state_id, new, next_id);
                        continue;
                    }
                };
                let splits = split.as_slice();
                if splits.len() == 1 {
                    if !rest.is_empty() {
                        stack.push(NextInsert::new(old.next_id, rest));
                    }
                    break;
                }
                let mut first = true;
                let mut add_trans =
                    |trie: &mut RangeTrie, pos, from, range, to| {
                        if first {
                            trie.set_transition_at(pos, from, range, to);
                            first = false;
                        } else {
                            trie.add_transition_at(pos, from, range, to);
                        }
                    };
                for (j, &srange) in splits.iter().enumerate() {
                    match srange {
                        SplitRange::Old(r) => {
                            let dup_id = self.duplicate(old.next_id);
                            add_trans(self, i, state_id, r, dup_id);
                        }
                        SplitRange::New(r) => {
                            {
                                let trans = &self.state(state_id).transitions;
                                if j + 1 == splits.len()
                                    && i < trans.len()
                                    && intersects(r, trans[i].range)
                                {
                                    new = r;
                                    continue 'OUTER;
                                }
                            }

                            let next_id =
                                NextInsert::push(self, &mut stack, rest);
                            add_trans(self, i, state_id, r, next_id);
                        }
                        SplitRange::Both(r) => {
                            if !rest.is_empty() {
                                stack.push(NextInsert::new(old.next_id, rest));
                            }
                            add_trans(self, i, state_id, r, old.next_id);
                        }
                    }
                    i += 1;
                }
                break;
            }
        }
        self.insert_stack = stack;
    }

    pub fn add_empty(&mut self) -> StateID {
        let id = match StateID::try_from(self.states.len()) {
            Ok(id) => id,
            Err(_) => {
                panic!("too many sequences added to range trie");
            }
        };
        if let Some(mut state) = self.free.pop() {
            state.clear();
            self.states.push(state);
        } else {
            self.states.push(State { transitions: vec![] });
        }
        id
    }

    /// Performs a deep clone of the given state and returns the duplicate's
    /// state ID.
    ///
    /// A "deep clone" in this context means that the state given along with
    /// recursively all states that it points to are copied. Once complete,
    /// the given state ID and the returned state ID share nothing.
    ///
    /// This is useful during range trie insertion when a new range overlaps
    /// with an existing range that is bigger than the new one. The part
    /// of the existing range that does *not* overlap with the new one is
    /// duplicated so that adding the new range to the overlap doesn't disturb
    /// the non-overlapping portion.
    ///
    /// There's one exception: if old_id is the final state, then it is not
    /// duplicated and the same final state is returned. This is because all
    /// final states in this trie are equivalent.
    fn duplicate(&mut self, old_id: StateID) -> StateID {
        if old_id == FINAL {
            return FINAL;
        }

        let mut stack = mem::replace(&mut self.dupe_stack, vec![]);
        stack.clear();

        let new_id = self.add_empty();
        stack.push(NextDupe { old_id, new_id });
        while let Some(NextDupe { old_id, new_id }) = stack.pop() {
            for i in 0..self.state(old_id).transitions.len() {
                let t = self.state(old_id).transitions[i].clone();
                if t.next_id == FINAL {
                    self.add_transition(new_id, t.range, FINAL);
                    continue;
                }

                let new_child_id = self.add_empty();
                self.add_transition(new_id, t.range, new_child_id);
                stack.push(NextDupe {
                    old_id: t.next_id,
                    new_id: new_child_id,
                });
            }
        }
        self.dupe_stack = stack;
        new_id
    }

    /// Adds the given transition to the given state.
    ///
    /// Callers must ensure that all previous transitions in this state
    /// are lexicographically smaller than the given range.
    fn add_transition(
        &mut self,
        from_id: StateID,
        range: Utf8Range,
        next_id: StateID,
    ) {
        self.state_mut(from_id)
            .transitions
            .push(Transition { range, next_id });
    }

    /// Like `add_transition`, except this inserts the transition just before
    /// the ith transition.
    fn add_transition_at(
        &mut self,
        i: usize,
        from_id: StateID,
        range: Utf8Range,
        next_id: StateID,
    ) {
        self.state_mut(from_id)
            .transitions
            .insert(i, Transition { range, next_id });
    }

    /// Overwrites the transition at position i with the given transition.
    fn set_transition_at(
        &mut self,
        i: usize,
        from_id: StateID,
        range: Utf8Range,
        next_id: StateID,
    ) {
        self.state_mut(from_id).transitions[i] = Transition { range, next_id };
    }

    /// Return an immutable borrow for the state with the given ID.
    fn state(&self, id: StateID) -> &State {
        &self.states[id]
    }

    /// Return a mutable borrow for the state with the given ID.
    fn state_mut(&mut self, id: StateID) -> &mut State {
        &mut self.states[id]
    }
}

impl State {
    /// Find the position at which the given range should be inserted in this
    /// state.
    ///
    /// The position returned is always in the inclusive range
    /// [0, transitions.len()]. If 'transitions.len()' is returned, then the
    /// given range overlaps with no other range in this state *and* is greater
    /// than all of them.
    ///
    /// For all other possible positions, the given range either overlaps
    /// with the transition at that position or is otherwise less than it
    /// with no overlap (and is greater than the previous transition). In the
    /// former case, careful attention must be paid to inserting this range
    /// as a new transition. In the latter case, the range can be inserted as
    /// a new transition at the given position without disrupting any other
    /// transitions.
    fn find(&self, range: Utf8Range) -> usize {
        /// Returns the position `i` at which `pred(xs[i])` first returns true
        /// such that for all `j >= i`, `pred(xs[j]) == true`. If `pred` never
        /// returns true, then `xs.len()` is returned.
        ///
        /// We roll our own binary search because it doesn't seem like the
        /// standard library's binary search can be used here. Namely, if
        /// there is an overlapping range, then we want to find the first such
        /// occurrence, but there may be many. Or at least, it's not quite
        /// clear to me how to do it.
        fn binary_search<T, F>(xs: &[T], mut pred: F) -> usize
        where
            F: FnMut(&T) -> bool,
        {
            let (mut left, mut right) = (0, xs.len());
            while left < right {
                let mid = (left + right) / 2;
                if pred(&xs[mid]) {
                    right = mid;
                } else {
                    left = mid + 1;
                }
            }
            left
        }

        binary_search(&self.transitions, |t| range.start <= t.range.end)
    }

    /// Clear this state such that it has zero transitions.
    fn clear(&mut self) {
        self.transitions.clear();
    }
}

/// The next state to process during duplication.
#[derive(Clone, Debug)]
struct NextDupe {
    /// The state we want to duplicate.
    old_id: StateID,
    /// The ID of the new state that is a duplicate of old_id.
    new_id: StateID,
}

/// The next state (and its corresponding transition) that we want to visit
/// during iteration in lexicographic order.
#[derive(Clone, Debug)]
struct NextIter {
    state_id: StateID,
    tidx: usize,
}

/// The next state to process during insertion and any remaining ranges that we
/// want to add for a particular sequence of ranges. The first such instance
/// is always the root state along with all ranges given.
#[derive(Clone, Debug)]
struct NextInsert {
    /// The next state to begin inserting ranges. This state should be the
    /// state at which `ranges[0]` should be inserted.
    state_id: StateID,
    /// The ranges to insert. We used a fixed-size array here to avoid an
    /// allocation.
    ranges: [Utf8Range; 4],
    /// The number of valid ranges in the above array.
    len: u8,
}

impl NextInsert {
    /// Create the next item to visit. The given state ID should correspond
    /// to the state at which the first range in the given slice should be
    /// inserted. The slice given must not be empty and it must be no longer
    /// than 4.
    fn new(state_id: StateID, ranges: &[Utf8Range]) -> NextInsert {
        let len = ranges.len();
        assert!(len > 0);
        assert!(len <= 4);

        let mut tmp = [Utf8Range { start: 0, end: 0 }; 4];
        tmp[..len].copy_from_slice(ranges);
        NextInsert { state_id, ranges: tmp, len: u8::try_from(len).unwrap() }
    }

    /// Push a new empty state to visit along with any remaining ranges that
    /// still need to be inserted. The ID of the new empty state is returned.
    ///
    /// If ranges is empty, then no new state is created and FINAL is returned.
    fn push(
        trie: &mut RangeTrie,
        stack: &mut Vec<NextInsert>,
        ranges: &[Utf8Range],
    ) -> StateID {
        if ranges.is_empty() {
            FINAL
        } else {
            let next_id = trie.add_empty();
            stack.push(NextInsert::new(next_id, ranges));
            next_id
        }
    }

    /// Return the ID of the state to visit.
    fn state_id(&self) -> StateID {
        self.state_id
    }

    /// Return the remaining ranges to insert.
    fn ranges(&self) -> &[Utf8Range] {
        &self.ranges[..usize::try_from(self.len).unwrap()]
    }
}

/// Split represents a partitioning of two ranges into one or more ranges. This
/// is the secret sauce that makes a range trie work, as it's what tells us
/// how to deal with two overlapping but unequal ranges during insertion.
///
/// Essentially, either two ranges overlap or they don't. If they don't, then
/// handling insertion is easy: just insert the new range into its
/// lexicographically correct position. Since it does not overlap with anything
/// else, no other transitions are impacted by the new range.
///
/// If they do overlap though, there are generally three possible cases to
/// handle:
///
/// 1. The part where the two ranges actually overlap. i.e., The intersection.
/// 2. The part of the existing range that is not in the new range.
/// 3. The part of the new range that is not in the old range.
///
/// (1) is guaranteed to always occur since all overlapping ranges have a
/// non-empty intersection. If the two ranges are not equivalent, then at
/// least one of (2) or (3) is guaranteed to occur as well. In some cases,
/// e.g., `[0-4]` and `[4-9]`, all three cases will occur.
///
/// This `Split` type is responsible for providing (1), (2) and (3) for any
/// possible pair of byte ranges.
///
/// As for insertion, for the overlap in (1), the remaining ranges to insert
/// should be added by following the corresponding transition. However, this
/// should only be done for the overlapping parts of the range. If there was
/// a part of the existing range that was not in the new range, then that
/// existing part must be split off from the transition and duplicated. The
/// remaining parts of the overlap can then be added to using the new ranges
/// without disturbing the existing range.
///
/// Handling the case for the part of a new range that is not in an existing
/// range is seemingly easy. Just treat it as if it were a non-overlapping
/// range. The problem here is that if this new non-overlapping range occurs
/// after both (1) and (2), then it's possible that it can overlap with the
/// next transition in the current state. If it does, then the whole process
/// must be repeated!
///
/// # Details of the 3 cases
///
/// The following details the various cases that are implemented in code
/// below. It's plausible that the number of cases is not actually minimal,
/// but it's important for this code to remain at least somewhat readable.
///
/// Given [a,b] and [x,y], where a <= b, x <= y, b < 256 and y < 256, we define
/// the follow distinct relationships where at least one must apply. The order
/// of these matters, since multiple can match. The first to match applies.
///
///   1. b < x <=> [a,b] < [x,y]
///   2. y < a <=> [x,y] < [a,b]
///
/// In the case of (1) and (2), these are the only cases where there is no
/// overlap. Or otherwise, the intersection of [a,b] and [x,y] is empty. In
/// order to compute the intersection, one can do [max(a,x), min(b,y)]. The
/// intersection in all of the following cases is non-empty.
///
///    3. a = x && b = y <=> [a,b] == [x,y]
///    4. a = x && b < y <=> [x,y] right-extends [a,b]
///    5. b = y && a > x <=> [x,y] left-extends [a,b]
///    6. x = a && y < b <=> [a,b] right-extends [x,y]
///    7. y = b && x > a <=> [a,b] left-extends [x,y]
///    8. a > x && b < y <=> [x,y] covers [a,b]
///    9. x > a && y < b <=> [a,b] covers [x,y]
///   10. b = x && a < y <=> [a,b] is left-adjacent to [x,y]
///   11. y = a && x < b <=> [x,y] is left-adjacent to [a,b]
///   12. b > x && b < y <=> [a,b] left-overlaps [x,y]
///   13. y > a && y < b <=> [x,y] left-overlaps [a,b]
///
/// In cases 3-13, we can form rules that partition the ranges into a
/// non-overlapping ordered sequence of ranges:
///
///    3. [a,b]
///    4. [a,b], [b+1,y]
///    5. [x,a-1], [a,b]
///    6. [x,y], [y+1,b]
///    7. [a,x-1], [x,y]
///    8. [x,a-1], [a,b], [b+1,y]
///    9. [a,x-1], [x,y], [y+1,b]
///   10. [a,b-1], [b,b], [b+1,y]
///   11. [x,y-1], [y,y], [y+1,b]
///   12. [a,x-1], [x,b], [b+1,y]
///   13. [x,a-1], [a,y], [y+1,b]
///
/// In the code below, we go a step further and identify each of the above
/// outputs as belonging either to the overlap of the two ranges or to one
/// of [a,b] or [x,y] exclusively.
#[derive(Clone, Debug, Eq, PartialEq)]
struct Split {
    partitions: [SplitRange; 3],
    len: usize,
}

/// A tagged range indicating how it was derived from a pair of ranges.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum SplitRange {
    Old(Utf8Range),
    New(Utf8Range),
    Both(Utf8Range),
}

impl Split {
    /// Create a partitioning of the given ranges.
    ///
    /// If the given ranges have an empty intersection, then None is returned.
    fn new(o: Utf8Range, n: Utf8Range) -> Option<Split> {
        let range = |r: RangeInclusive<u8>| Utf8Range {
            start: *r.start(),
            end: *r.end(),
        };
        let old = |r| SplitRange::Old(range(r));
        let new = |r| SplitRange::New(range(r));
        let both = |r| SplitRange::Both(range(r));

        let (a, b, x, y) = (o.start, o.end, n.start, n.end);

        if b < x || y < a {
            None
        } else if a == x && b == y {
            Some(Split::parts1(both(a..=b)))
        } else if a == x && b < y {
            Some(Split::parts2(both(a..=b), new(b + 1..=y)))
        } else if b == y && a > x {
            Some(Split::parts2(new(x..=a - 1), both(a..=b)))
        } else if x == a && y < b {
            Some(Split::parts2(both(x..=y), old(y + 1..=b)))
        } else if y == b && x > a {
            Some(Split::parts2(old(a..=x - 1), both(x..=y)))
        } else if a > x && b < y {
            Some(Split::parts3(new(x..=a - 1), both(a..=b), new(b + 1..=y)))
        } else if x > a && y < b {
            Some(Split::parts3(old(a..=x - 1), both(x..=y), old(y + 1..=b)))
        } else if b == x && a < y {
            Some(Split::parts3(old(a..=b - 1), both(b..=b), new(b + 1..=y)))
        } else if y == a && x < b {
            Some(Split::parts3(new(x..=y - 1), both(y..=y), old(y + 1..=b)))
        } else if b > x && b < y {
            Some(Split::parts3(old(a..=x - 1), both(x..=b), new(b + 1..=y)))
        } else if y > a && y < b {
            Some(Split::parts3(new(x..=a - 1), both(a..=y), old(y + 1..=b)))
        } else {
            unreachable!()
        }
    }

    /// Create a new split with a single partition. This only occurs when two
    /// ranges are equivalent.
    fn parts1(r1: SplitRange) -> Split {
        let nada = SplitRange::Old(Utf8Range { start: 0, end: 0 });
        Split { partitions: [r1, nada, nada], len: 1 }
    }

    /// Create a new split with two partitions.
    fn parts2(r1: SplitRange, r2: SplitRange) -> Split {
        let nada = SplitRange::Old(Utf8Range { start: 0, end: 0 });
        Split { partitions: [r1, r2, nada], len: 2 }
    }

    /// Create a new split with three partitions.
    fn parts3(r1: SplitRange, r2: SplitRange, r3: SplitRange) -> Split {
        Split { partitions: [r1, r2, r3], len: 3 }
    }

    /// Return the partitions in this split as a slice.
    fn as_slice(&self) -> &[SplitRange] {
        &self.partitions[..self.len]
    }
}

impl fmt::Debug for RangeTrie {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(f)?;
        for (i, state) in self.states.iter().enumerate() {
            let status = if i == FINAL.as_usize() { '*' } else { ' ' };
            writeln!(f, "{status}{i:06}: {state:?}")?;
        }
        Ok(())
    }
}

impl fmt::Debug for State {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let rs = self
            .transitions
            .iter()
            .map(|t| format!("{t:?}"))
            .collect::<Vec<String>>()
            .join(", ");
        write!(f, "{rs}")
    }
}

impl fmt::Debug for Transition {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.range.start == self.range.end {
            write!(
                f,
                "{:02X} => {:02X}",
                self.range.start,
                self.next_id.as_usize(),
            )
        } else {
            write!(
                f,
                "{:02X}-{:02X} => {:02X}",
                self.range.start,
                self.range.end,
                self.next_id.as_usize(),
            )
        }
    }
}

/// Returns true if and only if the given ranges intersect.
fn intersects(r1: Utf8Range, r2: Utf8Range) -> bool {
    !(r1.end < r2.start || r2.end < r1.start)
}
