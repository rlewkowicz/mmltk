use crate::{
    dfa::DEAD,
    util::{
        primitives::StateID,
        wire::{self, DeserializeError, Endian, SerializeError},
    },
};

macro_rules! err {
    ($msg:expr) => {
        return Err(DeserializeError::generic($msg));
    };
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct Special {
    /// The identifier of the last special state in a DFA. A state is special
    /// if and only if its identifier is less than or equal to `max`.
    pub(crate) max: StateID,
    /// The identifier of the quit state in a DFA. (There is no analogous field
    /// for the dead state since the dead state's ID is always zero, regardless
    /// of state ID size.)
    pub(crate) quit_id: StateID,
    /// The identifier of the first match state.
    pub(crate) min_match: StateID,
    /// The identifier of the last match state.
    pub(crate) max_match: StateID,
    /// The identifier of the first accelerated state.
    pub(crate) min_accel: StateID,
    /// The identifier of the last accelerated state.
    pub(crate) max_accel: StateID,
    /// The identifier of the first start state.
    pub(crate) min_start: StateID,
    /// The identifier of the last start state.
    pub(crate) max_start: StateID,
}

impl Special {
    /// Creates a new set of special ranges for a DFA. All ranges are initially
    /// set to only contain the dead state. This is interpreted as an empty
    /// range.
    #[cfg(feature = "dfa-build")]
    pub(crate) fn new() -> Special {
        Special {
            max: DEAD,
            quit_id: DEAD,
            min_match: DEAD,
            max_match: DEAD,
            min_accel: DEAD,
            max_accel: DEAD,
            min_start: DEAD,
            max_start: DEAD,
        }
    }

    /// Remaps all of the special state identifiers using the function given.
    #[cfg(feature = "dfa-build")]
    pub(crate) fn remap(&self, map: impl Fn(StateID) -> StateID) -> Special {
        Special {
            max: map(self.max),
            quit_id: map(self.quit_id),
            min_match: map(self.min_match),
            max_match: map(self.max_match),
            min_accel: map(self.min_accel),
            max_accel: map(self.max_accel),
            min_start: map(self.min_start),
            max_start: map(self.max_start),
        }
    }

    /// Deserialize the given bytes into special state ranges. If the slice
    /// given is not big enough, then this returns an error. Similarly, if
    /// any of the expected invariants around special state ranges aren't
    /// upheld, an error is returned. Note that this does not guarantee that
    /// the information returned is correct.
    ///
    /// Upon success, this returns the number of bytes read in addition to the
    /// special state IDs themselves.
    pub(crate) fn from_bytes(
        mut slice: &[u8],
    ) -> Result<(Special, usize), DeserializeError> {
        wire::check_slice_len(slice, 8 * StateID::SIZE, "special states")?;

        let mut nread = 0;
        let mut read_id = |what| -> Result<StateID, DeserializeError> {
            let (id, nr) = wire::try_read_state_id(slice, what)?;
            nread += nr;
            slice = &slice[StateID::SIZE..];
            Ok(id)
        };

        let max = read_id("special max id")?;
        let quit_id = read_id("special quit id")?;
        let min_match = read_id("special min match id")?;
        let max_match = read_id("special max match id")?;
        let min_accel = read_id("special min accel id")?;
        let max_accel = read_id("special max accel id")?;
        let min_start = read_id("special min start id")?;
        let max_start = read_id("special max start id")?;

        let special = Special {
            max,
            quit_id,
            min_match,
            max_match,
            min_accel,
            max_accel,
            min_start,
            max_start,
        };
        special.validate()?;
        assert_eq!(nread, special.write_to_len());
        Ok((special, nread))
    }

    /// Validate that the information describing special states satisfies
    /// all known invariants.
    pub(crate) fn validate(&self) -> Result<(), DeserializeError> {
        if self.min_match == DEAD && self.max_match != DEAD {
            err!("min_match is DEAD, but max_match is not");
        }
        if self.min_match != DEAD && self.max_match == DEAD {
            err!("max_match is DEAD, but min_match is not");
        }
        if self.min_accel == DEAD && self.max_accel != DEAD {
            err!("min_accel is DEAD, but max_accel is not");
        }
        if self.min_accel != DEAD && self.max_accel == DEAD {
            err!("max_accel is DEAD, but min_accel is not");
        }
        if self.min_start == DEAD && self.max_start != DEAD {
            err!("min_start is DEAD, but max_start is not");
        }
        if self.min_start != DEAD && self.max_start == DEAD {
            err!("max_start is DEAD, but min_start is not");
        }

        if self.min_match > self.max_match {
            err!("min_match should not be greater than max_match");
        }
        if self.min_accel > self.max_accel {
            err!("min_accel should not be greater than max_accel");
        }
        if self.min_start > self.max_start {
            err!("min_start should not be greater than max_start");
        }

        if self.matches() && self.quit_id >= self.min_match {
            err!("quit_id should not be greater than min_match");
        }
        if self.accels() && self.quit_id >= self.min_accel {
            err!("quit_id should not be greater than min_accel");
        }
        if self.starts() && self.quit_id >= self.min_start {
            err!("quit_id should not be greater than min_start");
        }
        if self.matches() && self.accels() && self.min_accel < self.min_match {
            err!("min_match should not be greater than min_accel");
        }
        if self.matches() && self.starts() && self.min_start < self.min_match {
            err!("min_match should not be greater than min_start");
        }
        if self.accels() && self.starts() && self.min_start < self.min_accel {
            err!("min_accel should not be greater than min_start");
        }

        if self.max < self.quit_id {
            err!("quit_id should not be greater than max");
        }
        if self.max < self.max_match {
            err!("max_match should not be greater than max");
        }
        if self.max < self.max_accel {
            err!("max_accel should not be greater than max");
        }
        if self.max < self.max_start {
            err!("max_start should not be greater than max");
        }

        Ok(())
    }

    /// Validate that the special state information is compatible with the
    /// given state len.
    pub(crate) fn validate_state_len(
        &self,
        len: usize,
        stride2: usize,
    ) -> Result<(), DeserializeError> {
        if (self.max.as_usize() >> stride2) >= len {
            err!("max should not be greater than or equal to state length");
        }
        Ok(())
    }

    /// Write the IDs and ranges for special states to the given byte buffer.
    /// The buffer given must have enough room to store all data, otherwise
    /// this will return an error. The number of bytes written is returned
    /// on success. The number of bytes written is guaranteed to be a multiple
    /// of 8.
    pub(crate) fn write_to<E: Endian>(
        &self,
        dst: &mut [u8],
    ) -> Result<usize, SerializeError> {
        use crate::util::wire::write_state_id as write;

        if dst.len() < self.write_to_len() {
            return Err(SerializeError::buffer_too_small("special state ids"));
        }

        let mut nwrite = 0;
        nwrite += write::<E>(self.max, &mut dst[nwrite..]);
        nwrite += write::<E>(self.quit_id, &mut dst[nwrite..]);
        nwrite += write::<E>(self.min_match, &mut dst[nwrite..]);
        nwrite += write::<E>(self.max_match, &mut dst[nwrite..]);
        nwrite += write::<E>(self.min_accel, &mut dst[nwrite..]);
        nwrite += write::<E>(self.max_accel, &mut dst[nwrite..]);
        nwrite += write::<E>(self.min_start, &mut dst[nwrite..]);
        nwrite += write::<E>(self.max_start, &mut dst[nwrite..]);

        assert_eq!(
            self.write_to_len(),
            nwrite,
            "expected to write certain number of bytes",
        );
        assert_eq!(
            nwrite % 8,
            0,
            "expected to write multiple of 8 bytes for special states",
        );
        Ok(nwrite)
    }

    /// Returns the total number of bytes written by `write_to`.
    pub(crate) fn write_to_len(&self) -> usize {
        8 * StateID::SIZE
    }

    /// Sets the maximum special state ID based on the current values. This
    /// should be used once all possible state IDs are set.
    #[cfg(feature = "dfa-build")]
    pub(crate) fn set_max(&mut self) {
        use core::cmp::max;
        self.max = max(
            self.quit_id,
            max(self.max_match, max(self.max_accel, self.max_start)),
        );
    }

    /// Sets the maximum special state ID such that starting states are not
    /// considered "special." This also marks the min/max starting states as
    /// DEAD such that 'is_start_state' always returns false, even if the state
    /// is actually a starting state.
    ///
    /// This is useful when there is no prefilter set. It will avoid
    /// ping-ponging between the hot path in the DFA search code and the start
    /// state handling code, which is typically only useful for executing a
    /// prefilter.
    #[cfg(feature = "dfa-build")]
    pub(crate) fn set_no_special_start_states(&mut self) {
        use core::cmp::max;
        self.max = max(self.quit_id, max(self.max_match, self.max_accel));
        self.min_start = DEAD;
        self.max_start = DEAD;
    }

    /// Returns true if and only if the given state ID is a special state.
    #[inline]
    pub(crate) fn is_special_state(&self, id: StateID) -> bool {
        id <= self.max
    }

    /// Returns true if and only if the given state ID is a dead state.
    #[inline]
    pub(crate) fn is_dead_state(&self, id: StateID) -> bool {
        id == DEAD
    }

    /// Returns true if and only if the given state ID is a quit state.
    #[inline]
    pub(crate) fn is_quit_state(&self, id: StateID) -> bool {
        !self.is_dead_state(id) && self.quit_id == id
    }

    /// Returns true if and only if the given state ID is a match state.
    #[inline]
    pub(crate) fn is_match_state(&self, id: StateID) -> bool {
        !self.is_dead_state(id) && self.min_match <= id && id <= self.max_match
    }

    /// Returns true if and only if the given state ID is an accel state.
    #[inline]
    pub(crate) fn is_accel_state(&self, id: StateID) -> bool {
        !self.is_dead_state(id) && self.min_accel <= id && id <= self.max_accel
    }

    /// Returns true if and only if the given state ID is a start state.
    #[inline]
    pub(crate) fn is_start_state(&self, id: StateID) -> bool {
        !self.is_dead_state(id) && self.min_start <= id && id <= self.max_start
    }

    /// Returns the total number of match states for a dense table based DFA.
    #[inline]
    pub(crate) fn match_len(&self, stride: usize) -> usize {
        if self.matches() {
            (self.max_match.as_usize() - self.min_match.as_usize() + stride)
                / stride
        } else {
            0
        }
    }

    /// Returns true if and only if there is at least one match state.
    #[inline]
    pub(crate) fn matches(&self) -> bool {
        self.min_match != DEAD
    }

    /// Returns the total number of accel states.
    #[cfg(feature = "dfa-build")]
    pub(crate) fn accel_len(&self, stride: usize) -> usize {
        if self.accels() {
            (self.max_accel.as_usize() - self.min_accel.as_usize() + stride)
                / stride
        } else {
            0
        }
    }

    /// Returns true if and only if there is at least one accel state.
    #[inline]
    pub(crate) fn accels(&self) -> bool {
        self.min_accel != DEAD
    }

    /// Returns true if and only if there is at least one start state.
    #[inline]
    pub(crate) fn starts(&self) -> bool {
        self.min_start != DEAD
    }
}
