/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use api::RasterSpace;
use crate::scene_building::IsVisible;
use crate::intern::{Internable, InternDebug, Handle as InternHandle};
use crate::internal_types::LayoutPrimitiveInfo;
use crate::picture_composite_mode::PictureCompositeKey;
use crate::prim_store::{
    PrimitiveKind, PrimitiveStore,
    InternablePrimitive,
};

#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
#[derive(Debug, Clone, Eq, MallocSizeOf, PartialEq, Hash)]
pub struct Picture {
    pub composite_mode_key: PictureCompositeKey,
    pub raster_space: RasterSpace,
}

#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
#[derive(Debug, Clone, Eq, MallocSizeOf, PartialEq, Hash)]
pub struct PictureKey {
    pub composite_mode_key: PictureCompositeKey,
    pub raster_space: RasterSpace,
}

impl PictureKey {
    pub fn new(
        pic: Picture,
    ) -> Self {
        PictureKey {
            composite_mode_key: pic.composite_mode_key,
            raster_space: pic.raster_space,
        }
    }
}

impl InternDebug for PictureKey {}

#[cfg_attr(feature = "capture", derive(Serialize))]
#[cfg_attr(feature = "replay", derive(Deserialize))]
#[derive(MallocSizeOf)]
pub struct PictureTemplate;

impl From<PictureKey> for PictureTemplate {
    fn from(_: PictureKey) -> Self {
        PictureTemplate
    }
}

pub type PictureDataHandle = InternHandle<Picture>;

impl Internable for Picture {
    type Key = PictureKey;
    type StoreData = PictureTemplate;
    type InternData = ();
    const PROFILE_COUNTER: usize = crate::render_stats::INTERNED_PICTURES;
}

impl InternablePrimitive for Picture {
    fn into_key(
        self,
        _: &LayoutPrimitiveInfo,
    ) -> PictureKey {
        PictureKey::new(self)
    }

    fn make_instance_kind(
        _key: PictureKey,
        _: PictureDataHandle,
        _: &mut PrimitiveStore,
    ) -> PrimitiveKind {
        unreachable!();
    }
}

impl IsVisible for Picture {
    fn is_visible(&self) -> bool {
        true
    }
}
