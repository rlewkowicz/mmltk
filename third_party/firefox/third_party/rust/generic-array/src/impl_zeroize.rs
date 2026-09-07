use {ArrayLength, GenericArray};

use zeroize::Zeroize;

#[cfg_attr(docsrs, doc(cfg(feature = "zeroize")))]
impl<T: Zeroize, N: ArrayLength<T>> Zeroize for GenericArray<T, N> {
    fn zeroize(&mut self) {
        self.as_mut_slice().iter_mut().zeroize()
    }
}
