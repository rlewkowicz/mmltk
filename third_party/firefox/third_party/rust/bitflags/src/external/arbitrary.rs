//! Specialized fuzzing for flags types using `arbitrary`.

use crate::Flags;

/**
Generate some arbitrary flags value with only known bits set.
*/
pub fn arbitrary<'a, B: Flags>(u: &mut arbitrary::Unstructured<'a>) -> arbitrary::Result<B>
where
    B::Bits: arbitrary::Arbitrary<'a>,
{
    B::from_bits(u.arbitrary()?).ok_or(arbitrary::Error::IncorrectFormat)
}
