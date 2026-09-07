use crate::backend::c;

/// The windows `sockaddr_in6` type is a union with accessor functions which
/// are not `const fn`. Define our own layout-compatible version so that we
/// can transmute in and out of it.

#[inline]
pub(crate) const fn in_addr_s_addr(addr: c::in_addr) -> u32 {
    addr.s_addr
}


#[inline]
pub(crate) const fn in_addr_new(s_addr: u32) -> c::in_addr {
    c::in_addr { s_addr }
}


#[inline]
pub(crate) const fn in6_addr_s6_addr(addr: c::in6_addr) -> [u8; 16] {
    addr.s6_addr
}


#[inline]
pub(crate) const fn in6_addr_new(s6_addr: [u8; 16]) -> c::in6_addr {
    c::in6_addr { s6_addr }
}


#[inline]
pub(crate) const fn sockaddr_in6_sin6_scope_id(addr: &c::sockaddr_in6) -> u32 {
    addr.sin6_scope_id
}


#[inline]
pub(crate) const fn sockaddr_in6_new(
#[cfg(any(bsd, target_os = "aix", target_os = "espidf", target_os = "hurd", target_os = "nto", target_os = "vita"))]
sin6_len: u8,
    sin6_family: c::sa_family_t,
    sin6_port: u16,
    sin6_flowinfo: u32,
    sin6_addr: c::in6_addr,
    sin6_scope_id: u32,
) -> c::sockaddr_in6 {
    c::sockaddr_in6 {
#[cfg(any(bsd, target_os = "aix", target_os = "espidf", target_os = "hurd", target_os = "nto", target_os = "vita"))]
sin6_len,
        sin6_family,
        sin6_port,
        sin6_flowinfo,
        sin6_addr,
        sin6_scope_id,
        #[cfg(solarish)]
        __sin6_src_id: 0,
        #[cfg(target_os = "vita")]
        sin6_vport: 0,
    }
}
