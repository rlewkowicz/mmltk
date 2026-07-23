use std::{
    convert::{TryFrom, TryInto},
    mem,
    os::raw::c_int,
};

use log::trace;

use super::{
    err::secstatus_to_res,
    p11::{
        sys::{
            self, PK11Context, PK11_AEADOp, PK11_CreateContextBySymKey, PRBool, CKA_DECRYPT,
            CKA_ENCRYPT, CKA_NSS_MESSAGE, CKG_GENERATE_COUNTER_XOR, CKG_NO_GENERATE, CKM_AES_GCM,
            CKM_CHACHA20_POLY1305, CK_ATTRIBUTE_TYPE, CK_GENERATOR_FUNCTION, CK_MECHANISM_TYPE,
        },
        Item, SymKey,
    },
};
use crate::{
    crypto::{Decrypt, Encrypt},
    err::{Error, Res},
    hpke::Aead as AeadId,
};

/// All the nonces are the same length.  Exploit that.
pub const NONCE_LEN: usize = 12;
/// The portion of the nonce that is a counter.
const COUNTER_LEN: usize = mem::size_of::<SequenceNumber>();
/// The NSS API insists on us identifying the tag separately, which is awful.
/// All of the AEAD functions here have a tag of this length, so use a fixed offset.
const TAG_LEN: usize = 16;

pub type SequenceNumber = u64;

/// All the lengths used by `PK11_AEADOp` are signed.  This converts to that.
fn c_int_len<T>(l: T) -> c_int
where
    T: TryInto<c_int>,
    T::Error: std::error::Error,
{
    l.try_into().unwrap()
}

unsafe fn destroy_aead_context(ctx: *mut PK11Context) {
    sys::PK11_DestroyContext(ctx, PRBool::from(true));
}
scoped_ptr!(Context, PK11Context, destroy_aead_context);

unsafe impl Send for Context {}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Mode {
    Encrypt,
    Decrypt,
}

impl Mode {
    fn p11mode(self) -> CK_ATTRIBUTE_TYPE {
        CK_ATTRIBUTE_TYPE::from(
            CKA_NSS_MESSAGE
                | match self {
                    Self::Encrypt => CKA_ENCRYPT,
                    Self::Decrypt => CKA_DECRYPT,
                },
        )
    }
}

/// This is an AEAD instance that uses the
pub struct Aead {
    mode: Mode,
    #[allow(dead_code)]
    algorithm: AeadId,
    ctx: Context,
    nonce_base: [u8; NONCE_LEN],
    decrypt_counter: SequenceNumber,
}

impl Aead {
    fn mech(algorithm: AeadId) -> CK_MECHANISM_TYPE {
        CK_MECHANISM_TYPE::from(match algorithm {
            AeadId::Aes128Gcm | AeadId::Aes256Gcm => CKM_AES_GCM,
            AeadId::ChaCha20Poly1305 => CKM_CHACHA20_POLY1305,
        })
    }


    pub fn new(
        mode: Mode,
        algorithm: AeadId,
        key: &SymKey,
        nonce_base: [u8; NONCE_LEN],
    ) -> Res<Self> {
        trace!(
            "New AEAD: key={} nonce_base={}",
            hex::encode(key.key_data()?),
            hex::encode(nonce_base)
        );

        let ptr = unsafe {
            PK11_CreateContextBySymKey(
                Self::mech(algorithm),
                mode.p11mode(),
                key.ptr(),
                &Item::wrap(&nonce_base[..]),
            )
        };
        Ok(Self {
            mode,
            algorithm,
            ctx: Context::from_ptr(ptr)?,
            nonce_base,
            decrypt_counter: 0,
        })
    }

    fn do_open(ctx: &Context, aad: &[u8], ct: &[u8], nonce: &mut [u8], mech: u32) -> Res<Vec<u8>> {
        let mut pt = vec![0; ct.len()]; 
        let mut pt_len: c_int = 0;
        let pt_expected = ct.len().checked_sub(TAG_LEN).ok_or(Error::Truncated)?;
        secstatus_to_res(unsafe {
            PK11_AEADOp(
                ctx.ptr(),
                CK_GENERATOR_FUNCTION::from(mech),
                c_int_len(NONCE_LEN - COUNTER_LEN), 
                nonce.as_mut_ptr(),
                c_int_len(nonce.len()),
                aad.as_ptr(),
                c_int_len(aad.len()),
                pt.as_mut_ptr(),
                &raw mut pt_len,
                c_int_len(pt.len()),                     
                ct.as_ptr().add(pt_expected).cast_mut(), 
                c_int_len(TAG_LEN),
                ct.as_ptr(),
                c_int_len(pt_expected),
            )
        })?;
        let len = usize::try_from(pt_len).unwrap();
        debug_assert_eq!(len, pt_expected);
        pt.truncate(len);
        Ok(pt)
    }

    pub fn open_seq(&mut self, aad: &[u8], seq: SequenceNumber, ct: &[u8]) -> Res<Vec<u8>> {
        assert_eq!(self.mode, Mode::Decrypt);
        let mut nonce = self.nonce_base;
        for (i, n) in nonce.iter_mut().rev().take(COUNTER_LEN).enumerate() {
            *n ^= u8::try_from((seq >> (8 * i)) & 0xff).unwrap();
        }

        Self::do_open(&self.ctx, aad, ct, &mut nonce, CKG_NO_GENERATE)
    }
}

impl Decrypt for Aead {
    fn open(&mut self, aad: &[u8], ct: &[u8]) -> Res<Vec<u8>> {
        assert_eq!(self.mode, Mode::Decrypt);
        let counter = self.decrypt_counter;
        self.decrypt_counter += 1;
        self.open_seq(aad, counter, ct)
    }

    fn alg(&self) -> AeadId {
        self.algorithm
    }
}

impl Encrypt for Aead {
    fn seal(&mut self, aad: &[u8], pt: &[u8]) -> Res<Vec<u8>> {
        assert_eq!(self.mode, Mode::Encrypt);
        let mut nonce = self.nonce_base;

        let mut ct = vec![0; pt.len() + TAG_LEN];
        let mut ct_len: c_int = 0;
        let mut tag = vec![0; TAG_LEN];
        secstatus_to_res(unsafe {
            PK11_AEADOp(
                self.ctx.ptr(),
                CK_GENERATOR_FUNCTION::from(CKG_GENERATE_COUNTER_XOR),
                c_int_len(NONCE_LEN - COUNTER_LEN), 
                nonce.as_mut_ptr(),
                c_int_len(nonce.len()),
                aad.as_ptr(),
                c_int_len(aad.len()),
                ct.as_mut_ptr(),
                &raw mut ct_len,
                c_int_len(ct.len()), 
                tag.as_mut_ptr(),
                c_int_len(tag.len()),
                pt.as_ptr(),
                c_int_len(pt.len()),
            )
        })?;
        ct.truncate(usize::try_from(ct_len).unwrap());
        debug_assert_eq!(ct.len(), pt.len());
        ct.append(&mut tag);
        Ok(ct)
    }

    fn alg(&self) -> AeadId {
        self.algorithm
    }
}
