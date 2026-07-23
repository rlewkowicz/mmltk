use crate::{std::convert::TryInto, Builder, Uuid};

use arbitrary::{Arbitrary, Unstructured};

impl Arbitrary<'_> for Uuid {
    fn arbitrary(u: &mut Unstructured<'_>) -> arbitrary::Result<Self> {
        let b = u
            .bytes(16)?
            .try_into()
            .map_err(|_| arbitrary::Error::NotEnoughData)?;

        Ok(Builder::from_random_bytes(b).into_uuid())
    }

    fn size_hint(depth: usize) -> (usize, Option<usize>) {
        (16, Some(16))
    }
}
