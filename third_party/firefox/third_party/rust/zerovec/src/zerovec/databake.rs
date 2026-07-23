// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

use super::ZeroVec;
use crate::{ule::AsULE, ZeroSlice};
use databake::*;

impl<T: AsULE> Bake for ZeroVec<'_, T> {
    fn bake(&self, env: &CrateEnv) -> TokenStream {
        env.insert("zerovec");
        if self.is_empty() {
            quote! { zerovec::ZeroVec::new() }
        } else {
            let bytes = databake::Bake::bake(&self.as_bytes(), env);
            quote! { unsafe { zerovec::ZeroVec::from_bytes_unchecked(#bytes) } }
        }
    }
}

impl<T: AsULE> BakeSize for ZeroVec<'_, T> {
    fn borrows_size(&self) -> usize {
        self.as_bytes().len()
    }
}

impl<T: AsULE> Bake for &ZeroSlice<T> {
    fn bake(&self, env: &CrateEnv) -> TokenStream {
        env.insert("zerovec");
        if self.is_empty() {
            quote! { zerovec::ZeroSlice::new_empty() }
        } else {
            let bytes = databake::Bake::bake(&self.as_bytes(), env);
            quote! { unsafe { zerovec::ZeroSlice::from_bytes_unchecked(#bytes) } }
        }
    }
}

impl<T: AsULE> BakeSize for &ZeroSlice<T> {
    fn borrows_size(&self) -> usize {
        self.as_bytes().len()
    }
}
