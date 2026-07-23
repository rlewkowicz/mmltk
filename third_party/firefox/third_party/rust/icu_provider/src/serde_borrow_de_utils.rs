// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

use alloc::borrow::Cow;
use serde::de::Deserializer;
use serde::Deserialize;

#[derive(Deserialize)]
#[serde(transparent)]
#[allow(clippy::exhaustive_structs)] 
#[derive(Debug)]
pub struct CowWrap<'data>(#[serde(borrow)] pub Cow<'data, str>);

#[derive(Deserialize)]
#[serde(transparent)]
#[allow(clippy::exhaustive_structs)] 
#[derive(Debug)]
pub struct CowBytesWrap<'data>(#[serde(borrow)] pub Cow<'data, [u8]>);

pub fn array_of_cow<'de, D, const N: usize>(deserializer: D) -> Result<[Cow<'de, str>; N], D::Error>
where
    D: Deserializer<'de>,
    [CowWrap<'de>; N]: Deserialize<'de>,
{
    <[CowWrap<'de>; N]>::deserialize(deserializer).map(|array| array.map(|wrap| wrap.0))
}

pub fn option_of_cow<'de, D>(deserializer: D) -> Result<Option<Cow<'de, str>>, D::Error>
where
    D: Deserializer<'de>,
{
    <Option<CowWrap<'de>>>::deserialize(deserializer).map(|opt| opt.map(|wrap| wrap.0))
}

pub fn tuple_of_cow<'de, D>(deserializer: D) -> Result<(Cow<'de, str>, Cow<'de, str>), D::Error>
where
    D: Deserializer<'de>,
    (CowWrap<'de>, CowWrap<'de>): Deserialize<'de>,
{
    <(CowWrap<'de>, CowWrap<'de>)>::deserialize(deserializer).map(|x| (x.0 .0, x.1 .0))
}
