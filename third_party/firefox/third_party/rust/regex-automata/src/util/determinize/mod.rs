/*!
This module contains types and routines for implementing determinization.

In this crate, there are at least two places where we implement
determinization: fully ahead-of-time compiled DFAs in the `dfa` module and
lazily compiled DFAs in the `hybrid` module. The stuff in this module
corresponds to the things that are in common between these implementations.

There are three broad things that our implementations of determinization have
in common, as defined by this module:

* The classification of start states. That is, whether we're dealing with
word boundaries, line boundaries, etc., is all the same. This also includes
the look-behind assertions that are satisfied by each starting state
classification.
* The representation of DFA states as sets of NFA states, including
convenience types for building these DFA states that are amenable to reusing
allocations.
* Routines for the "classical" parts of determinization: computing the
epsilon closure, tracking match states (with corresponding pattern IDs, since
we support multi-pattern finite automata) and, of course, computing the
transition function between states for units of input.

I did consider a couple of alternatives to this particular form of code reuse:

1. Don't do any code reuse. The problem here is that we *really* want both
forms of determinization to do exactly identical things when it comes to
their handling of NFA states. While our tests generally ensure this, the code
is tricky and large enough where not reusing code is a pretty big bummer.

2. Implement all of determinization once and make it generic over fully
compiled DFAs and lazily compiled DFAs. While I didn't actually try this
approach, my instinct is that it would be more complex than is needed here.
And the interface required would be pretty hairy. Instead, I think splitting
it into logical sub-components works better.
*/

use alloc::vec::Vec;

pub(crate) use self::state::{
    State, StateBuilderEmpty, StateBuilderMatches, StateBuilderNFA,
};

use crate::{
    nfa::thompson,
    util::{
        alphabet,
        look::{Look, LookSet},
        primitives::StateID,
        search::MatchKind,
        sparse_set::{SparseSet, SparseSets},
        start::Start,
        utf8,
    },
};

mod state;

/// Compute the set of all reachable NFA states, including the full epsilon
/// closure, from a DFA state for a single unit of input. The set of reachable
/// states is returned as a `StateBuilderNFA`. The `StateBuilderNFA` returned
/// also includes any look-behind assertions satisfied by `unit`, in addition
/// to whether it is a match state. For multi-pattern DFAs, the builder will
/// also include the pattern IDs that match (in the order seen).
///
/// `nfa` must be able to resolve any NFA state in `state` and any NFA state
/// reachable via the epsilon closure of any NFA state in `state`. `sparses`
/// must have capacity equivalent to `nfa.len()`.
///
/// `match_kind` should correspond to the match semantics implemented by the
/// DFA being built. Generally speaking, for leftmost-first match semantics,
/// states that appear after the first NFA match state will not be included in
/// the `StateBuilderNFA` returned since they are impossible to visit.
///
/// `sparses` is used as scratch space for NFA traversal. Other than their
/// capacity requirements (detailed above), there are no requirements on what's
/// contained within them (if anything). Similarly, what's inside of them once
/// this routine returns is unspecified.
///
/// `stack` must have length 0. It is used as scratch space for depth first
/// traversal. After returning, it is guaranteed that `stack` will have length
/// 0.
///
/// `state` corresponds to the current DFA state on which one wants to compute
/// the transition for the input `unit`.
///
/// `empty_builder` corresponds to the builder allocation to use to produce a
/// complete `StateBuilderNFA` state. If the state is not needed (or is already
/// cached), then it can be cleared and reused without needing to create a new
/// `State`. The `StateBuilderNFA` state returned is final and ready to be
/// turned into a `State` if necessary.
pub(crate) fn next(
    nfa: &thompson::NFA,
    match_kind: MatchKind,
    sparses: &mut SparseSets,
    stack: &mut Vec<StateID>,
    state: &State,
    unit: alphabet::Unit,
    empty_builder: StateBuilderEmpty,
) -> StateBuilderNFA {
    sparses.clear();

    let rev = nfa.is_reverse();
    let lookm = nfa.look_matcher();

    state.iter_nfa_state_ids(|nfa_id| {
        sparses.set1.insert(nfa_id);
    });

    if !state.look_need().is_empty() {
        let mut look_have = state.look_have();
        match unit.as_u8() {
            Some(b'\r') => {
                if !rev || !state.is_half_crlf() {
                    look_have = look_have.insert(Look::EndCRLF);
                }
            }
            Some(b'\n') => {
                if rev || !state.is_half_crlf() {
                    look_have = look_have.insert(Look::EndCRLF);
                }
            }
            Some(_) => {}
            None => {
                look_have = look_have
                    .insert(Look::End)
                    .insert(Look::EndLF)
                    .insert(Look::EndCRLF);
            }
        }
        if unit.is_byte(lookm.get_line_terminator()) {
            look_have = look_have.insert(Look::EndLF);
        }
        if state.is_half_crlf()
            && ((rev && !unit.is_byte(b'\r'))
                || (!rev && !unit.is_byte(b'\n')))
        {
            look_have = look_have.insert(Look::StartCRLF);
        }
        if state.is_from_word() == unit.is_word_byte() {
            look_have = look_have
                .insert(Look::WordAsciiNegate)
                .insert(Look::WordUnicodeNegate);
        } else {
            look_have =
                look_have.insert(Look::WordAscii).insert(Look::WordUnicode);
        }
        if !unit.is_word_byte() {
            look_have = look_have
                .insert(Look::WordEndHalfAscii)
                .insert(Look::WordEndHalfUnicode);
        }
        if state.is_from_word() && !unit.is_word_byte() {
            look_have = look_have
                .insert(Look::WordEndAscii)
                .insert(Look::WordEndUnicode);
        } else if !state.is_from_word() && unit.is_word_byte() {
            look_have = look_have
                .insert(Look::WordStartAscii)
                .insert(Look::WordStartUnicode);
        }
        if !look_have
            .subtract(state.look_have())
            .intersect(state.look_need())
            .is_empty()
        {
            for nfa_id in sparses.set1.iter() {
                epsilon_closure(
                    nfa,
                    nfa_id,
                    look_have,
                    stack,
                    &mut sparses.set2,
                );
            }
            sparses.swap();
            sparses.set2.clear();
        }
    }

    let mut builder = empty_builder.into_matches();
    if nfa.look_set_any().contains_anchor_line()
        && unit.is_byte(lookm.get_line_terminator())
    {
        builder.set_look_have(|have| have.insert(Look::StartLF));
    }
    if nfa.look_set_any().contains_anchor_crlf()
        && ((rev && unit.is_byte(b'\r')) || (!rev && unit.is_byte(b'\n')))
    {
        builder.set_look_have(|have| have.insert(Look::StartCRLF));
    }
    if nfa.look_set_any().contains_word() && !unit.is_word_byte() {
        builder.set_look_have(|have| {
            have.insert(Look::WordStartHalfAscii)
                .insert(Look::WordStartHalfUnicode)
        });
    }
    for nfa_id in sparses.set1.iter() {
        match *nfa.state(nfa_id) {
            thompson::State::Union { .. }
            | thompson::State::BinaryUnion { .. }
            | thompson::State::Fail
            | thompson::State::Look { .. }
            | thompson::State::Capture { .. } => {}
            thompson::State::Match { pattern_id } => {
                builder.add_match_pattern_id(pattern_id);
                if !match_kind.continue_past_first_match() {
                    break;
                }
            }
            thompson::State::ByteRange { ref trans } => {
                if trans.matches_unit(unit) {
                    epsilon_closure(
                        nfa,
                        trans.next,
                        builder.look_have(),
                        stack,
                        &mut sparses.set2,
                    );
                }
            }
            thompson::State::Sparse(ref sparse) => {
                if let Some(next) = sparse.matches_unit(unit) {
                    epsilon_closure(
                        nfa,
                        next,
                        builder.look_have(),
                        stack,
                        &mut sparses.set2,
                    );
                }
            }
            thompson::State::Dense(ref dense) => {
                if let Some(next) = dense.matches_unit(unit) {
                    epsilon_closure(
                        nfa,
                        next,
                        builder.look_have(),
                        stack,
                        &mut sparses.set2,
                    );
                }
            }
        }
    }
    if !sparses.set2.is_empty() {
        if nfa.look_set_any().contains_word() && unit.is_word_byte() {
            builder.set_is_from_word();
        }
        if nfa.look_set_any().contains_anchor_crlf()
            && ((rev && unit.is_byte(b'\n')) || (!rev && unit.is_byte(b'\r')))
        {
            builder.set_is_half_crlf();
        }
    }
    let mut builder_nfa = builder.into_nfa();
    add_nfa_states(nfa, &sparses.set2, &mut builder_nfa);
    builder_nfa
}

/// Compute the epsilon closure for the given NFA state. The epsilon closure
/// consists of all NFA state IDs, including `start_nfa_id`, that can be
/// reached from `start_nfa_id` without consuming any input. These state IDs
/// are written to `set` in the order they are visited, but only if they are
/// not already in `set`. `start_nfa_id` must be a valid state ID for the NFA
/// given.
///
/// `look_have` consists of the satisfied assertions at the current
/// position. For conditional look-around epsilon transitions, these are
/// only followed if they are satisfied by `look_have`.
///
/// `stack` must have length 0. It is used as scratch space for depth first
/// traversal. After returning, it is guaranteed that `stack` will have length
/// 0.
pub(crate) fn epsilon_closure(
    nfa: &thompson::NFA,
    start_nfa_id: StateID,
    look_have: LookSet,
    stack: &mut Vec<StateID>,
    set: &mut SparseSet,
) {
    assert!(stack.is_empty());
    if !nfa.state(start_nfa_id).is_epsilon() {
        set.insert(start_nfa_id);
        return;
    }

    stack.push(start_nfa_id);
    while let Some(mut id) = stack.pop() {
        loop {
            if !set.insert(id) {
                break;
            }
            match *nfa.state(id) {
                thompson::State::ByteRange { .. }
                | thompson::State::Sparse { .. }
                | thompson::State::Dense { .. }
                | thompson::State::Fail
                | thompson::State::Match { .. } => break,
                thompson::State::Look { look, next } => {
                    if !look_have.contains(look) {
                        break;
                    }
                    id = next;
                }
                thompson::State::Union { ref alternates } => {
                    id = match alternates.get(0) {
                        None => break,
                        Some(&id) => id,
                    };
                    stack.extend(alternates[1..].iter().rev());
                }
                thompson::State::BinaryUnion { alt1, alt2 } => {
                    id = alt1;
                    stack.push(alt2);
                }
                thompson::State::Capture { next, .. } => {
                    id = next;
                }
            }
        }
    }
}

/// Add the NFA state IDs in the given `set` to the given DFA builder state.
/// The order in which states are added corresponds to the order in which they
/// were added to `set`.
///
/// The DFA builder state given should already have its complete set of match
/// pattern IDs added (if any) and any look-behind assertions (StartLF, Start
/// and whether this state is being generated for a transition over a word byte
/// when applicable) that are true immediately prior to transitioning into this
/// state (via `builder.look_have()`). The match pattern IDs should correspond
/// to matches that occurred on the previous transition, since all matches are
/// delayed by one byte. The things that should _not_ be set are look-ahead
/// assertions (EndLF, End and whether the next byte is a word byte or not).
/// The builder state should also not have anything in `look_need` set, as this
/// routine will compute that for you.
///
/// The given NFA should be able to resolve all identifiers in `set` to a
/// particular NFA state. Additionally, `set` must have capacity equivalent
/// to `nfa.len()`.
pub(crate) fn add_nfa_states(
    nfa: &thompson::NFA,
    set: &SparseSet,
    builder: &mut StateBuilderNFA,
) {
    for nfa_id in set.iter() {
        match *nfa.state(nfa_id) {
            thompson::State::ByteRange { .. } => {
                builder.add_nfa_state_id(nfa_id);
            }
            thompson::State::Sparse { .. } => {
                builder.add_nfa_state_id(nfa_id);
            }
            thompson::State::Dense { .. } => {
                builder.add_nfa_state_id(nfa_id);
            }
            thompson::State::Look { look, .. } => {
                builder.add_nfa_state_id(nfa_id);
                builder.set_look_need(|need| need.insert(look));
            }
            thompson::State::Union { .. }
            | thompson::State::BinaryUnion { .. } => {
                // because it was generated by computing an epsilon closure,
                builder.add_nfa_state_id(nfa_id);
            }
            thompson::State::Capture { .. } => {}
            thompson::State::Fail => {
                builder.add_nfa_state_id(nfa_id);
            }
            thompson::State::Match { .. } => {
                builder.add_nfa_state_id(nfa_id);
            }
        }
    }
    if builder.look_need().is_empty() {
        builder.set_look_have(|_| LookSet::empty());
    }
}

/// Sets the appropriate look-behind assertions on the given state based on
/// this starting configuration.
pub(crate) fn set_lookbehind_from_start(
    nfa: &thompson::NFA,
    start: &Start,
    builder: &mut StateBuilderMatches,
) {
    let rev = nfa.is_reverse();
    let lineterm = nfa.look_matcher().get_line_terminator();
    let lookset = nfa.look_set_any();
    match *start {
        Start::NonWordByte => {
            if lookset.contains_word() {
                builder.set_look_have(|have| {
                    have.insert(Look::WordStartHalfAscii)
                        .insert(Look::WordStartHalfUnicode)
                });
            }
        }
        Start::WordByte => {
            if lookset.contains_word() {
                builder.set_is_from_word();
            }
        }
        Start::Text => {
            if lookset.contains_anchor_haystack() {
                builder.set_look_have(|have| have.insert(Look::Start));
            }
            if lookset.contains_anchor_line() {
                builder.set_look_have(|have| {
                    have.insert(Look::StartLF).insert(Look::StartCRLF)
                });
            }
            if lookset.contains_word() {
                builder.set_look_have(|have| {
                    have.insert(Look::WordStartHalfAscii)
                        .insert(Look::WordStartHalfUnicode)
                });
            }
        }
        Start::LineLF => {
            if rev {
                if lookset.contains_anchor_crlf() {
                    builder.set_is_half_crlf();
                }
                if lookset.contains_anchor_line() {
                    builder.set_look_have(|have| have.insert(Look::StartLF));
                }
            } else {
                if lookset.contains_anchor_line() {
                    builder.set_look_have(|have| have.insert(Look::StartCRLF));
                }
            }
            if lookset.contains_anchor_line() && lineterm == b'\n' {
                builder.set_look_have(|have| have.insert(Look::StartLF));
            }
            if lookset.contains_word() {
                builder.set_look_have(|have| {
                    have.insert(Look::WordStartHalfAscii)
                        .insert(Look::WordStartHalfUnicode)
                });
            }
        }
        Start::LineCR => {
            if lookset.contains_anchor_crlf() {
                if rev {
                    builder.set_look_have(|have| have.insert(Look::StartCRLF));
                } else {
                    builder.set_is_half_crlf();
                }
            }
            if lookset.contains_anchor_line() && lineterm == b'\r' {
                builder.set_look_have(|have| have.insert(Look::StartLF));
            }
            if lookset.contains_word() {
                builder.set_look_have(|have| {
                    have.insert(Look::WordStartHalfAscii)
                        .insert(Look::WordStartHalfUnicode)
                });
            }
        }
        Start::CustomLineTerminator => {
            if lookset.contains_anchor_line() {
                builder.set_look_have(|have| have.insert(Look::StartLF));
            }
            if lookset.contains_word() {
                if utf8::is_word_byte(lineterm) {
                    builder.set_is_from_word();
                } else {
                    builder.set_look_have(|have| {
                        have.insert(Look::WordStartHalfAscii)
                            .insert(Look::WordStartHalfUnicode)
                    });
                }
            }
        }
    }
}
