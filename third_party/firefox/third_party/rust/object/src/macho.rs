//! Mach-O definitions.
//!
//! These definitions are independent of read/write support, although we do implement
//! some traits useful for those.
//!
//! This module is based heavily on header files from MacOSX11.1.sdk.

#![allow(missing_docs)]

use crate::endian::{BigEndian, Endian, U64Bytes, U16, U32, U64};
use crate::pod::Pod;



/// mask for architecture bits
pub const CPU_ARCH_MASK: u32 = 0xff00_0000;
/// 64 bit ABI
pub const CPU_ARCH_ABI64: u32 = 0x0100_0000;
/// ABI for 64-bit hardware with 32-bit types; LP32
pub const CPU_ARCH_ABI64_32: u32 = 0x0200_0000;


pub const CPU_TYPE_ANY: u32 = !0;

pub const CPU_TYPE_VAX: u32 = 1;
pub const CPU_TYPE_MC680X0: u32 = 6;
pub const CPU_TYPE_X86: u32 = 7;
pub const CPU_TYPE_X86_64: u32 = CPU_TYPE_X86 | CPU_ARCH_ABI64;
pub const CPU_TYPE_MIPS: u32 = 8;
pub const CPU_TYPE_MC98000: u32 = 10;
pub const CPU_TYPE_HPPA: u32 = 11;
pub const CPU_TYPE_ARM: u32 = 12;
pub const CPU_TYPE_ARM64: u32 = CPU_TYPE_ARM | CPU_ARCH_ABI64;
pub const CPU_TYPE_ARM64_32: u32 = CPU_TYPE_ARM | CPU_ARCH_ABI64_32;
pub const CPU_TYPE_MC88000: u32 = 13;
pub const CPU_TYPE_SPARC: u32 = 14;
pub const CPU_TYPE_I860: u32 = 15;
pub const CPU_TYPE_ALPHA: u32 = 16;
pub const CPU_TYPE_POWERPC: u32 = 18;
pub const CPU_TYPE_POWERPC64: u32 = CPU_TYPE_POWERPC | CPU_ARCH_ABI64;

/// mask for feature flags
pub const CPU_SUBTYPE_MASK: u32 = 0xff00_0000;
/// 64 bit libraries
pub const CPU_SUBTYPE_LIB64: u32 = 0x8000_0000;
/// pointer authentication with versioned ABI
pub const CPU_SUBTYPE_PTRAUTH_ABI: u32 = 0x8000_0000;

/// When selecting a slice, ANY will pick the slice with the best
/// grading for the selected cpu_type_t, unlike the "ALL" subtypes,
/// which are the slices that can run on any hardware for that cpu type.
pub const CPU_SUBTYPE_ANY: u32 = !0;

pub const CPU_SUBTYPE_MULTIPLE: u32 = !0;
pub const CPU_SUBTYPE_LITTLE_ENDIAN: u32 = 0;
pub const CPU_SUBTYPE_BIG_ENDIAN: u32 = 1;


pub const CPU_SUBTYPE_VAX_ALL: u32 = 0;
pub const CPU_SUBTYPE_VAX780: u32 = 1;
pub const CPU_SUBTYPE_VAX785: u32 = 2;
pub const CPU_SUBTYPE_VAX750: u32 = 3;
pub const CPU_SUBTYPE_VAX730: u32 = 4;
pub const CPU_SUBTYPE_UVAXI: u32 = 5;
pub const CPU_SUBTYPE_UVAXII: u32 = 6;
pub const CPU_SUBTYPE_VAX8200: u32 = 7;
pub const CPU_SUBTYPE_VAX8500: u32 = 8;
pub const CPU_SUBTYPE_VAX8600: u32 = 9;
pub const CPU_SUBTYPE_VAX8650: u32 = 10;
pub const CPU_SUBTYPE_VAX8800: u32 = 11;
pub const CPU_SUBTYPE_UVAXIII: u32 = 12;


pub const CPU_SUBTYPE_MC680X0_ALL: u32 = 1;
pub const CPU_SUBTYPE_MC68030: u32 = 1;
pub const CPU_SUBTYPE_MC68040: u32 = 2;
pub const CPU_SUBTYPE_MC68030_ONLY: u32 = 3;


#[inline]
pub const fn cpu_subtype_intel(f: u32, m: u32) -> u32 {
    f + (m << 4)
}

pub const CPU_SUBTYPE_I386_ALL: u32 = cpu_subtype_intel(3, 0);
pub const CPU_SUBTYPE_386: u32 = cpu_subtype_intel(3, 0);
pub const CPU_SUBTYPE_486: u32 = cpu_subtype_intel(4, 0);
pub const CPU_SUBTYPE_486SX: u32 = cpu_subtype_intel(4, 8);
pub const CPU_SUBTYPE_586: u32 = cpu_subtype_intel(5, 0);
pub const CPU_SUBTYPE_PENT: u32 = cpu_subtype_intel(5, 0);
pub const CPU_SUBTYPE_PENTPRO: u32 = cpu_subtype_intel(6, 1);
pub const CPU_SUBTYPE_PENTII_M3: u32 = cpu_subtype_intel(6, 3);
pub const CPU_SUBTYPE_PENTII_M5: u32 = cpu_subtype_intel(6, 5);
pub const CPU_SUBTYPE_CELERON: u32 = cpu_subtype_intel(7, 6);
pub const CPU_SUBTYPE_CELERON_MOBILE: u32 = cpu_subtype_intel(7, 7);
pub const CPU_SUBTYPE_PENTIUM_3: u32 = cpu_subtype_intel(8, 0);
pub const CPU_SUBTYPE_PENTIUM_3_M: u32 = cpu_subtype_intel(8, 1);
pub const CPU_SUBTYPE_PENTIUM_3_XEON: u32 = cpu_subtype_intel(8, 2);
pub const CPU_SUBTYPE_PENTIUM_M: u32 = cpu_subtype_intel(9, 0);
pub const CPU_SUBTYPE_PENTIUM_4: u32 = cpu_subtype_intel(10, 0);
pub const CPU_SUBTYPE_PENTIUM_4_M: u32 = cpu_subtype_intel(10, 1);
pub const CPU_SUBTYPE_ITANIUM: u32 = cpu_subtype_intel(11, 0);
pub const CPU_SUBTYPE_ITANIUM_2: u32 = cpu_subtype_intel(11, 1);
pub const CPU_SUBTYPE_XEON: u32 = cpu_subtype_intel(12, 0);
pub const CPU_SUBTYPE_XEON_MP: u32 = cpu_subtype_intel(12, 1);

#[inline]
pub const fn cpu_subtype_intel_family(x: u32) -> u32 {
    x & 15
}
pub const CPU_SUBTYPE_INTEL_FAMILY_MAX: u32 = 15;

#[inline]
pub const fn cpu_subtype_intel_model(x: u32) -> u32 {
    x >> 4
}
pub const CPU_SUBTYPE_INTEL_MODEL_ALL: u32 = 0;


pub const CPU_SUBTYPE_X86_ALL: u32 = 3;
pub const CPU_SUBTYPE_X86_64_ALL: u32 = 3;
pub const CPU_SUBTYPE_X86_ARCH1: u32 = 4;
/// Haswell feature subset
pub const CPU_SUBTYPE_X86_64_H: u32 = 8;


pub const CPU_SUBTYPE_MIPS_ALL: u32 = 0;
pub const CPU_SUBTYPE_MIPS_R2300: u32 = 1;
pub const CPU_SUBTYPE_MIPS_R2600: u32 = 2;
pub const CPU_SUBTYPE_MIPS_R2800: u32 = 3;
/// pmax
pub const CPU_SUBTYPE_MIPS_R2000A: u32 = 4;
pub const CPU_SUBTYPE_MIPS_R2000: u32 = 5;
/// 3max
pub const CPU_SUBTYPE_MIPS_R3000A: u32 = 6;
pub const CPU_SUBTYPE_MIPS_R3000: u32 = 7;

pub const CPU_SUBTYPE_MC98000_ALL: u32 = 0;
pub const CPU_SUBTYPE_MC98601: u32 = 1;


pub const CPU_SUBTYPE_HPPA_ALL: u32 = 0;
pub const CPU_SUBTYPE_HPPA_7100LC: u32 = 1;

pub const CPU_SUBTYPE_MC88000_ALL: u32 = 0;
pub const CPU_SUBTYPE_MC88100: u32 = 1;
pub const CPU_SUBTYPE_MC88110: u32 = 2;

pub const CPU_SUBTYPE_SPARC_ALL: u32 = 0;

pub const CPU_SUBTYPE_I860_ALL: u32 = 0;
pub const CPU_SUBTYPE_I860_860: u32 = 1;

pub const CPU_SUBTYPE_POWERPC_ALL: u32 = 0;
pub const CPU_SUBTYPE_POWERPC_601: u32 = 1;
pub const CPU_SUBTYPE_POWERPC_602: u32 = 2;
pub const CPU_SUBTYPE_POWERPC_603: u32 = 3;
pub const CPU_SUBTYPE_POWERPC_603E: u32 = 4;
pub const CPU_SUBTYPE_POWERPC_603EV: u32 = 5;
pub const CPU_SUBTYPE_POWERPC_604: u32 = 6;
pub const CPU_SUBTYPE_POWERPC_604E: u32 = 7;
pub const CPU_SUBTYPE_POWERPC_620: u32 = 8;
pub const CPU_SUBTYPE_POWERPC_750: u32 = 9;
pub const CPU_SUBTYPE_POWERPC_7400: u32 = 10;
pub const CPU_SUBTYPE_POWERPC_7450: u32 = 11;
pub const CPU_SUBTYPE_POWERPC_970: u32 = 100;

pub const CPU_SUBTYPE_ARM_ALL: u32 = 0;
pub const CPU_SUBTYPE_ARM_V4T: u32 = 5;
pub const CPU_SUBTYPE_ARM_V6: u32 = 6;
pub const CPU_SUBTYPE_ARM_V5TEJ: u32 = 7;
pub const CPU_SUBTYPE_ARM_XSCALE: u32 = 8;
/// ARMv7-A and ARMv7-R
pub const CPU_SUBTYPE_ARM_V7: u32 = 9;
/// Cortex A9
pub const CPU_SUBTYPE_ARM_V7F: u32 = 10;
/// Swift
pub const CPU_SUBTYPE_ARM_V7S: u32 = 11;
pub const CPU_SUBTYPE_ARM_V7K: u32 = 12;
pub const CPU_SUBTYPE_ARM_V8: u32 = 13;
/// Not meant to be run under xnu
pub const CPU_SUBTYPE_ARM_V6M: u32 = 14;
/// Not meant to be run under xnu
pub const CPU_SUBTYPE_ARM_V7M: u32 = 15;
/// Not meant to be run under xnu
pub const CPU_SUBTYPE_ARM_V7EM: u32 = 16;
/// Not meant to be run under xnu
pub const CPU_SUBTYPE_ARM_V8M: u32 = 17;

pub const CPU_SUBTYPE_ARM64_ALL: u32 = 0;
pub const CPU_SUBTYPE_ARM64_V8: u32 = 1;
pub const CPU_SUBTYPE_ARM64E: u32 = 2;

pub const CPU_SUBTYPE_ARM64_32_ALL: u32 = 0;
pub const CPU_SUBTYPE_ARM64_32_V8: u32 = 1;


/// read permission
pub const VM_PROT_READ: u32 = 0x01;
/// write permission
pub const VM_PROT_WRITE: u32 = 0x02;
/// execute permission
pub const VM_PROT_EXECUTE: u32 = 0x04;


/// The dyld cache header.
/// Corresponds to struct dyld_cache_header from dyld_cache_format.h.
/// This header has grown over time. Only the fields up to and including dyld_base_address
/// are guaranteed to be present. For all other fields, check the header size before
/// accessing the field. The header size is stored in mapping_offset; the mappings start
/// right after the theader.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct DyldCacheHeader<E: Endian> {
    /// e.g. "dyld_v0    i386"
    pub magic: [u8; 16],
    /// file offset to first dyld_cache_mapping_info
    pub mapping_offset: U32<E>, 
    /// number of dyld_cache_mapping_info entries
    pub mapping_count: U32<E>, 
    /// file offset to first dyld_cache_image_info
    pub images_offset: U32<E>, 
    /// number of dyld_cache_image_info entries
    pub images_count: U32<E>, 
    /// base address of dyld when cache was built
    pub dyld_base_address: U64<E>, 
    reserved1: [u8; 32], 
    /// file offset of where local symbols are stored
    pub local_symbols_offset: U64<E>, 
    /// size of local symbols information
    pub local_symbols_size: U64<E>, 
    /// unique value for each shared cache file
    pub uuid: [u8; 16], 
    reserved2: [u8; 32], 
    reserved3: [u8; 32], 
    reserved4: [u8; 32], 
    reserved5: [u8; 32], 
    reserved6: [u8; 32], 
    reserved7: [u8; 32], 
    reserved8: [u8; 32], 
    reserved9: [u8; 32], 
    reserved10: [u8; 32], 
    /// file offset to first dyld_subcache_info
    pub subcaches_offset: U32<E>, 
    /// number of dyld_subcache_info entries
    pub subcaches_count: U32<E>, 
    /// the UUID of the .symbols subcache
    pub symbols_subcache_uuid: [u8; 16], 
    reserved11: [u8; 32], 
    /// file offset to first dyld_cache_image_info
    /// Use this  instead of images_offset if mapping_offset is at least 0x1c4.
    pub images_across_all_subcaches_offset: U32<E>, 
    /// number of dyld_cache_image_info entries
    /// Use this  instead of images_count if mapping_offset is at least 0x1c4.
    pub images_across_all_subcaches_count: U32<E>, 
}

/// Corresponds to struct dyld_cache_mapping_info from dyld_cache_format.h.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct DyldCacheMappingInfo<E: Endian> {
    pub address: U64<E>,
    pub size: U64<E>,
    pub file_offset: U64<E>,
    pub max_prot: U32<E>,
    pub init_prot: U32<E>,
}

/// Corresponds to struct dyld_cache_image_info from dyld_cache_format.h.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct DyldCacheImageInfo<E: Endian> {
    pub address: U64<E>,
    pub mod_time: U64<E>,
    pub inode: U64<E>,
    pub path_file_offset: U32<E>,
    pub pad: U32<E>,
}

/// Added in dyld-940, which shipped with macOS 12 / iOS 15.
/// Originally called `dyld_subcache_entry`, renamed to `dyld_subcache_entry_v1`
/// in dyld-1042.1.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct DyldSubCacheEntryV1<E: Endian> {
    /// The UUID of this subcache.
    pub uuid: [u8; 16],
    /// The offset of this subcache from the main cache base address.
    pub cache_vm_offset: U64<E>,
}

/// Added in dyld-1042.1, which shipped with macOS 13 / iOS 16.
/// Called `dyld_subcache_entry` as of dyld-1042.1.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct DyldSubCacheEntryV2<E: Endian> {
    /// The UUID of this subcache.
    pub uuid: [u8; 16],
    /// The offset of this subcache from the main cache base address.
    pub cache_vm_offset: U64<E>,
    /// The file name suffix of the subCache file, e.g. ".25.data" or ".03.development".
    pub file_suffix: [u8; 32],
}



pub const FAT_MAGIC: u32 = 0xcafe_babe;
/// NXSwapLong(FAT_MAGIC)
pub const FAT_CIGAM: u32 = 0xbeba_feca;

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct FatHeader {
    /// FAT_MAGIC or FAT_MAGIC_64
    pub magic: U32<BigEndian>,
    /// number of structs that follow
    pub nfat_arch: U32<BigEndian>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct FatArch32 {
    /// cpu specifier (int)
    pub cputype: U32<BigEndian>,
    /// machine specifier (int)
    pub cpusubtype: U32<BigEndian>,
    /// file offset to this object file
    pub offset: U32<BigEndian>,
    /// size of this object file
    pub size: U32<BigEndian>,
    /// alignment as a power of 2
    pub align: U32<BigEndian>,
}

pub const FAT_MAGIC_64: u32 = 0xcafe_babf;
/// NXSwapLong(FAT_MAGIC_64)
pub const FAT_CIGAM_64: u32 = 0xbfba_feca;

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct FatArch64 {
    /// cpu specifier (int)
    pub cputype: U32<BigEndian>,
    /// machine specifier (int)
    pub cpusubtype: U32<BigEndian>,
    /// file offset to this object file
    pub offset: U64<BigEndian>,
    /// size of this object file
    pub size: U64<BigEndian>,
    /// alignment as a power of 2
    pub align: U32<BigEndian>,
    /// reserved
    pub reserved: U32<BigEndian>,
}


/// The 32-bit mach header.
///
/// Appears at the very beginning of the object file for 32-bit architectures.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct MachHeader32<E: Endian> {
    /// mach magic number identifier
    pub magic: U32<BigEndian>,
    /// cpu specifier
    pub cputype: U32<E>,
    /// machine specifier
    pub cpusubtype: U32<E>,
    /// type of file
    pub filetype: U32<E>,
    /// number of load commands
    pub ncmds: U32<E>,
    /// the size of all the load commands
    pub sizeofcmds: U32<E>,
    /// flags
    pub flags: U32<E>,
}

/// the mach magic number
pub const MH_MAGIC: u32 = 0xfeed_face;
/// NXSwapInt(MH_MAGIC)
pub const MH_CIGAM: u32 = 0xcefa_edfe;

/// The 64-bit mach header.
///
/// Appears at the very beginning of object files for 64-bit architectures.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct MachHeader64<E: Endian> {
    /// mach magic number identifier
    pub magic: U32<BigEndian>,
    /// cpu specifier
    pub cputype: U32<E>,
    /// machine specifier
    pub cpusubtype: U32<E>,
    /// type of file
    pub filetype: U32<E>,
    /// number of load commands
    pub ncmds: U32<E>,
    /// the size of all the load commands
    pub sizeofcmds: U32<E>,
    /// flags
    pub flags: U32<E>,
    /// reserved
    pub reserved: U32<E>,
}

/// the 64-bit mach magic number
pub const MH_MAGIC_64: u32 = 0xfeed_facf;
/// NXSwapInt(MH_MAGIC_64)
pub const MH_CIGAM_64: u32 = 0xcffa_edfe;


/// relocatable object file
pub const MH_OBJECT: u32 = 0x1;
/// demand paged executable file
pub const MH_EXECUTE: u32 = 0x2;
/// fixed VM shared library file
pub const MH_FVMLIB: u32 = 0x3;
/// core file
pub const MH_CORE: u32 = 0x4;
/// preloaded executable file
pub const MH_PRELOAD: u32 = 0x5;
/// dynamically bound shared library
pub const MH_DYLIB: u32 = 0x6;
/// dynamic link editor
pub const MH_DYLINKER: u32 = 0x7;
/// dynamically bound bundle file
pub const MH_BUNDLE: u32 = 0x8;
/// shared library stub for static linking only, no section contents
pub const MH_DYLIB_STUB: u32 = 0x9;
/// companion file with only debug sections
pub const MH_DSYM: u32 = 0xa;
/// x86_64 kexts
pub const MH_KEXT_BUNDLE: u32 = 0xb;
/// set of mach-o's
pub const MH_FILESET: u32 = 0xc;

/// the object file has no undefined references
pub const MH_NOUNDEFS: u32 = 0x1;
/// the object file is the output of an incremental link against a base file and can't be link edited again
pub const MH_INCRLINK: u32 = 0x2;
/// the object file is input for the dynamic linker and can't be statically link edited again
pub const MH_DYLDLINK: u32 = 0x4;
/// the object file's undefined references are bound by the dynamic linker when loaded.
pub const MH_BINDATLOAD: u32 = 0x8;
/// the file has its dynamic undefined references prebound.
pub const MH_PREBOUND: u32 = 0x10;
/// the file has its read-only and read-write segments split
pub const MH_SPLIT_SEGS: u32 = 0x20;
/// the shared library init routine is to be run lazily via catching memory faults to its writeable segments (obsolete)
pub const MH_LAZY_INIT: u32 = 0x40;
/// the image is using two-level name space bindings
pub const MH_TWOLEVEL: u32 = 0x80;
/// the executable is forcing all images to use flat name space bindings
pub const MH_FORCE_FLAT: u32 = 0x100;
/// this umbrella guarantees no multiple definitions of symbols in its sub-images so the two-level namespace hints can always be used.
pub const MH_NOMULTIDEFS: u32 = 0x200;
/// do not have dyld notify the prebinding agent about this executable
pub const MH_NOFIXPREBINDING: u32 = 0x400;
/// the binary is not prebound but can have its prebinding redone. only used when MH_PREBOUND is not set.
pub const MH_PREBINDABLE: u32 = 0x800;
/// indicates that this binary binds to all two-level namespace modules of its dependent libraries. only used when MH_PREBINDABLE and MH_TWOLEVEL are both set.
pub const MH_ALLMODSBOUND: u32 = 0x1000;
/// safe to divide up the sections into sub-sections via symbols for dead code stripping
pub const MH_SUBSECTIONS_VIA_SYMBOLS: u32 = 0x2000;
/// the binary has been canonicalized via the unprebind operation
pub const MH_CANONICAL: u32 = 0x4000;
/// the final linked image contains external weak symbols
pub const MH_WEAK_DEFINES: u32 = 0x8000;
/// the final linked image uses weak symbols
pub const MH_BINDS_TO_WEAK: u32 = 0x10000;
/// When this bit is set, all stacks in the task will be given stack execution privilege.  Only used in MH_EXECUTE filetypes.
pub const MH_ALLOW_STACK_EXECUTION: u32 = 0x20000;
/// When this bit is set, the binary declares it is safe for use in processes with uid zero
pub const MH_ROOT_SAFE: u32 = 0x40000;
/// When this bit is set, the binary declares it is safe for use in processes when issetugid() is true
pub const MH_SETUID_SAFE: u32 = 0x80000;
/// When this bit is set on a dylib, the static linker does not need to examine dependent dylibs to see if any are re-exported
pub const MH_NO_REEXPORTED_DYLIBS: u32 = 0x10_0000;
/// When this bit is set, the OS will load the main executable at a random address.  Only used in MH_EXECUTE filetypes.
pub const MH_PIE: u32 = 0x20_0000;
/// Only for use on dylibs.  When linking against a dylib that has this bit set, the static linker will automatically not create a LC_LOAD_DYLIB load command to the dylib if no symbols are being referenced from the dylib.
pub const MH_DEAD_STRIPPABLE_DYLIB: u32 = 0x40_0000;
/// Contains a section of type S_THREAD_LOCAL_VARIABLES
pub const MH_HAS_TLV_DESCRIPTORS: u32 = 0x80_0000;
/// When this bit is set, the OS will run the main executable with a non-executable heap even on platforms (e.g. i386) that don't require it. Only used in MH_EXECUTE filetypes.
pub const MH_NO_HEAP_EXECUTION: u32 = 0x100_0000;
/// The code was linked for use in an application extension.
pub const MH_APP_EXTENSION_SAFE: u32 = 0x0200_0000;
/// The external symbols listed in the nlist symbol table do not include all the symbols listed in the dyld info.
pub const MH_NLIST_OUTOFSYNC_WITH_DYLDINFO: u32 = 0x0400_0000;
/// Allow LC_MIN_VERSION_MACOS and LC_BUILD_VERSION load commands with
/// the platforms macOS, iOSMac, iOSSimulator, tvOSSimulator and watchOSSimulator.
pub const MH_SIM_SUPPORT: u32 = 0x0800_0000;
/// Only for use on dylibs. When this bit is set, the dylib is part of the dyld
/// shared cache, rather than loose in the filesystem.
pub const MH_DYLIB_IN_CACHE: u32 = 0x8000_0000;

/// Common fields at the start of every load command.
///
/// The load commands directly follow the mach_header.  The total size of all
/// of the commands is given by the sizeofcmds field in the mach_header.  All
/// load commands must have as their first two fields `cmd` and `cmdsize`.  The `cmd`
/// field is filled in with a constant for that command type.  Each command type
/// has a structure specifically for it.  The `cmdsize` field is the size in bytes
/// of the particular load command structure plus anything that follows it that
/// is a part of the load command (i.e. section structures, strings, etc.).  To
/// advance to the next load command the `cmdsize` can be added to the offset or
/// pointer of the current load command.  The `cmdsize` for 32-bit architectures
/// MUST be a multiple of 4 bytes and for 64-bit architectures MUST be a multiple
/// of 8 bytes (these are forever the maximum alignment of any load commands).
/// The padded bytes must be zero.  All tables in the object file must also
/// follow these rules so the file can be memory mapped.  Otherwise the pointers
/// to these tables will not work well or at all on some machines.  With all
/// padding zeroed like objects will compare byte for byte.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct LoadCommand<E: Endian> {
    /// Type of load command.
    ///
    /// One of the `LC_*` constants.
    pub cmd: U32<E>,
    /// Total size of command in bytes.
    pub cmdsize: U32<E>,
}

pub const LC_REQ_DYLD: u32 = 0x8000_0000;

/// segment of this file to be mapped
pub const LC_SEGMENT: u32 = 0x1;
/// link-edit stab symbol table info
pub const LC_SYMTAB: u32 = 0x2;
/// link-edit gdb symbol table info (obsolete)
pub const LC_SYMSEG: u32 = 0x3;
/// thread
pub const LC_THREAD: u32 = 0x4;
/// unix thread (includes a stack)
pub const LC_UNIXTHREAD: u32 = 0x5;
/// load a specified fixed VM shared library
pub const LC_LOADFVMLIB: u32 = 0x6;
/// fixed VM shared library identification
pub const LC_IDFVMLIB: u32 = 0x7;
/// object identification info (obsolete)
pub const LC_IDENT: u32 = 0x8;
/// fixed VM file inclusion (internal use)
pub const LC_FVMFILE: u32 = 0x9;
/// prepage command (internal use)
pub const LC_PREPAGE: u32 = 0xa;
/// dynamic link-edit symbol table info
pub const LC_DYSYMTAB: u32 = 0xb;
/// load a dynamically linked shared library
pub const LC_LOAD_DYLIB: u32 = 0xc;
/// dynamically linked shared lib ident
pub const LC_ID_DYLIB: u32 = 0xd;
/// load a dynamic linker
pub const LC_LOAD_DYLINKER: u32 = 0xe;
/// dynamic linker identification
pub const LC_ID_DYLINKER: u32 = 0xf;
/// modules prebound for a dynamically linked shared library
pub const LC_PREBOUND_DYLIB: u32 = 0x10;
/// image routines
pub const LC_ROUTINES: u32 = 0x11;
/// sub framework
pub const LC_SUB_FRAMEWORK: u32 = 0x12;
/// sub umbrella
pub const LC_SUB_UMBRELLA: u32 = 0x13;
/// sub client
pub const LC_SUB_CLIENT: u32 = 0x14;
/// sub library
pub const LC_SUB_LIBRARY: u32 = 0x15;
/// two-level namespace lookup hints
pub const LC_TWOLEVEL_HINTS: u32 = 0x16;
/// prebind checksum
pub const LC_PREBIND_CKSUM: u32 = 0x17;
/// load a dynamically linked shared library that is allowed to be missing
/// (all symbols are weak imported).
pub const LC_LOAD_WEAK_DYLIB: u32 = 0x18 | LC_REQ_DYLD;
/// 64-bit segment of this file to be mapped
pub const LC_SEGMENT_64: u32 = 0x19;
/// 64-bit image routines
pub const LC_ROUTINES_64: u32 = 0x1a;
/// the uuid
pub const LC_UUID: u32 = 0x1b;
/// runpath additions
pub const LC_RPATH: u32 = 0x1c | LC_REQ_DYLD;
/// local of code signature
pub const LC_CODE_SIGNATURE: u32 = 0x1d;
/// local of info to split segments
pub const LC_SEGMENT_SPLIT_INFO: u32 = 0x1e;
/// load and re-export dylib
pub const LC_REEXPORT_DYLIB: u32 = 0x1f | LC_REQ_DYLD;
/// delay load of dylib until first use
pub const LC_LAZY_LOAD_DYLIB: u32 = 0x20;
/// encrypted segment information
pub const LC_ENCRYPTION_INFO: u32 = 0x21;
/// compressed dyld information
pub const LC_DYLD_INFO: u32 = 0x22;
/// compressed dyld information only
pub const LC_DYLD_INFO_ONLY: u32 = 0x22 | LC_REQ_DYLD;
/// load upward dylib
pub const LC_LOAD_UPWARD_DYLIB: u32 = 0x23 | LC_REQ_DYLD;
/// build for MacOSX min OS version
pub const LC_VERSION_MIN_MACOSX: u32 = 0x24;
/// build for iPhoneOS min OS version
pub const LC_VERSION_MIN_IPHONEOS: u32 = 0x25;
/// compressed table of function start addresses
pub const LC_FUNCTION_STARTS: u32 = 0x26;
/// string for dyld to treat like environment variable
pub const LC_DYLD_ENVIRONMENT: u32 = 0x27;
/// replacement for LC_UNIXTHREAD
pub const LC_MAIN: u32 = 0x28 | LC_REQ_DYLD;
/// table of non-instructions in __text
pub const LC_DATA_IN_CODE: u32 = 0x29;
/// source version used to build binary
pub const LC_SOURCE_VERSION: u32 = 0x2A;
/// Code signing DRs copied from linked dylibs
pub const LC_DYLIB_CODE_SIGN_DRS: u32 = 0x2B;
/// 64-bit encrypted segment information
pub const LC_ENCRYPTION_INFO_64: u32 = 0x2C;
/// linker options in MH_OBJECT files
pub const LC_LINKER_OPTION: u32 = 0x2D;
/// optimization hints in MH_OBJECT files
pub const LC_LINKER_OPTIMIZATION_HINT: u32 = 0x2E;
/// build for AppleTV min OS version
pub const LC_VERSION_MIN_TVOS: u32 = 0x2F;
/// build for Watch min OS version
pub const LC_VERSION_MIN_WATCHOS: u32 = 0x30;
/// arbitrary data included within a Mach-O file
pub const LC_NOTE: u32 = 0x31;
/// build for platform min OS version
pub const LC_BUILD_VERSION: u32 = 0x32;
/// used with `LinkeditDataCommand`, payload is trie
pub const LC_DYLD_EXPORTS_TRIE: u32 = 0x33 | LC_REQ_DYLD;
/// used with `LinkeditDataCommand`
pub const LC_DYLD_CHAINED_FIXUPS: u32 = 0x34 | LC_REQ_DYLD;
/// used with `FilesetEntryCommand`
pub const LC_FILESET_ENTRY: u32 = 0x35 | LC_REQ_DYLD;

/// A variable length string in a load command.
///
/// The strings are stored just after the load command structure and
/// the offset is from the start of the load command structure.  The size
/// of the string is reflected in the `cmdsize` field of the load command.
/// Once again any padded bytes to bring the `cmdsize` field to a multiple
/// of 4 bytes must be zero.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct LcStr<E: Endian> {
    /// offset to the string
    pub offset: U32<E>,
}

/// 32-bit segment load command.
///
/// The segment load command indicates that a part of this file is to be
/// mapped into the task's address space.  The size of this segment in memory,
/// vmsize, maybe equal to or larger than the amount to map from this file,
/// filesize.  The file is mapped starting at fileoff to the beginning of
/// the segment in memory, vmaddr.  The rest of the memory of the segment,
/// if any, is allocated zero fill on demand.  The segment's maximum virtual
/// memory protection and initial virtual memory protection are specified
/// by the maxprot and initprot fields.  If the segment has sections then the
/// `Section32` structures directly follow the segment command and their size is
/// reflected in `cmdsize`.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct SegmentCommand32<E: Endian> {
    /// LC_SEGMENT
    pub cmd: U32<E>,
    /// includes sizeof section structs
    pub cmdsize: U32<E>,
    /// segment name
    pub segname: [u8; 16],
    /// memory address of this segment
    pub vmaddr: U32<E>,
    /// memory size of this segment
    pub vmsize: U32<E>,
    /// file offset of this segment
    pub fileoff: U32<E>,
    /// amount to map from the file
    pub filesize: U32<E>,
    /// maximum VM protection
    pub maxprot: U32<E>,
    /// initial VM protection
    pub initprot: U32<E>,
    /// number of sections in segment
    pub nsects: U32<E>,
    /// flags
    pub flags: U32<E>,
}

/// 64-bit segment load command.
///
/// The 64-bit segment load command indicates that a part of this file is to be
/// mapped into a 64-bit task's address space.  If the 64-bit segment has
/// sections then `Section64` structures directly follow the 64-bit segment
/// command and their size is reflected in `cmdsize`.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct SegmentCommand64<E: Endian> {
    /// LC_SEGMENT_64
    pub cmd: U32<E>,
    /// includes sizeof section_64 structs
    pub cmdsize: U32<E>,
    /// segment name
    pub segname: [u8; 16],
    /// memory address of this segment
    pub vmaddr: U64<E>,
    /// memory size of this segment
    pub vmsize: U64<E>,
    /// file offset of this segment
    pub fileoff: U64<E>,
    /// amount to map from the file
    pub filesize: U64<E>,
    /// maximum VM protection
    pub maxprot: U32<E>,
    /// initial VM protection
    pub initprot: U32<E>,
    /// number of sections in segment
    pub nsects: U32<E>,
    /// flags
    pub flags: U32<E>,
}

/// the file contents for this segment is for the high part of the VM space, the low part is zero filled (for stacks in core files)
pub const SG_HIGHVM: u32 = 0x1;
/// this segment is the VM that is allocated by a fixed VM library, for overlap checking in the link editor
pub const SG_FVMLIB: u32 = 0x2;
/// this segment has nothing that was relocated in it and nothing relocated to it, that is it maybe safely replaced without relocation
pub const SG_NORELOC: u32 = 0x4;
/// This segment is protected.  If the segment starts at file offset 0, the first page of the segment is not protected.  All other pages of the segment are protected.
pub const SG_PROTECTED_VERSION_1: u32 = 0x8;
/// This segment is made read-only after fixups
pub const SG_READ_ONLY: u32 = 0x10;

/// 32-bit section.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct Section32<E: Endian> {
    /// name of this section
    pub sectname: [u8; 16],
    /// segment this section goes in
    pub segname: [u8; 16],
    /// memory address of this section
    pub addr: U32<E>,
    /// size in bytes of this section
    pub size: U32<E>,
    /// file offset of this section
    pub offset: U32<E>,
    /// section alignment (power of 2)
    pub align: U32<E>,
    /// file offset of relocation entries
    pub reloff: U32<E>,
    /// number of relocation entries
    pub nreloc: U32<E>,
    /// flags (section type and attributes)
    pub flags: U32<E>,
    /// reserved (for offset or index)
    pub reserved1: U32<E>,
    /// reserved (for count or sizeof)
    pub reserved2: U32<E>,
}

/// 64-bit section.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct Section64<E: Endian> {
    /// name of this section
    pub sectname: [u8; 16],
    /// segment this section goes in
    pub segname: [u8; 16],
    /// memory address of this section
    pub addr: U64<E>,
    /// size in bytes of this section
    pub size: U64<E>,
    /// file offset of this section
    pub offset: U32<E>,
    /// section alignment (power of 2)
    pub align: U32<E>,
    /// file offset of relocation entries
    pub reloff: U32<E>,
    /// number of relocation entries
    pub nreloc: U32<E>,
    /// flags (section type and attributes)
    pub flags: U32<E>,
    /// reserved (for offset or index)
    pub reserved1: U32<E>,
    /// reserved (for count or sizeof)
    pub reserved2: U32<E>,
    /// reserved
    pub reserved3: U32<E>,
}

/// 256 section types
pub const SECTION_TYPE: u32 = 0x0000_00ff;
/// 24 section attributes
pub const SECTION_ATTRIBUTES: u32 = 0xffff_ff00;

/// regular section
pub const S_REGULAR: u32 = 0x0;
/// zero fill on demand section
pub const S_ZEROFILL: u32 = 0x1;
/// section with only literal C strings
pub const S_CSTRING_LITERALS: u32 = 0x2;
/// section with only 4 byte literals
pub const S_4BYTE_LITERALS: u32 = 0x3;
/// section with only 8 byte literals
pub const S_8BYTE_LITERALS: u32 = 0x4;
/// section with only pointers to literals
pub const S_LITERAL_POINTERS: u32 = 0x5;
/// section with only non-lazy symbol pointers
pub const S_NON_LAZY_SYMBOL_POINTERS: u32 = 0x6;
/// section with only lazy symbol pointers
pub const S_LAZY_SYMBOL_POINTERS: u32 = 0x7;
/// section with only symbol stubs, byte size of stub in the reserved2 field
pub const S_SYMBOL_STUBS: u32 = 0x8;
/// section with only function pointers for initialization
pub const S_MOD_INIT_FUNC_POINTERS: u32 = 0x9;
/// section with only function pointers for termination
pub const S_MOD_TERM_FUNC_POINTERS: u32 = 0xa;
/// section contains symbols that are to be coalesced
pub const S_COALESCED: u32 = 0xb;
/// zero fill on demand section (that can be larger than 4 gigabytes)
pub const S_GB_ZEROFILL: u32 = 0xc;
/// section with only pairs of function pointers for interposing
pub const S_INTERPOSING: u32 = 0xd;
/// section with only 16 byte literals
pub const S_16BYTE_LITERALS: u32 = 0xe;
/// section contains DTrace Object Format
pub const S_DTRACE_DOF: u32 = 0xf;
/// section with only lazy symbol pointers to lazy loaded dylibs
pub const S_LAZY_DYLIB_SYMBOL_POINTERS: u32 = 0x10;
/// template of initial values for TLVs
pub const S_THREAD_LOCAL_REGULAR: u32 = 0x11;
/// template of initial values for TLVs
pub const S_THREAD_LOCAL_ZEROFILL: u32 = 0x12;
/// TLV descriptors
pub const S_THREAD_LOCAL_VARIABLES: u32 = 0x13;
/// pointers to TLV descriptors
pub const S_THREAD_LOCAL_VARIABLE_POINTERS: u32 = 0x14;
/// functions to call to initialize TLV values
pub const S_THREAD_LOCAL_INIT_FUNCTION_POINTERS: u32 = 0x15;
/// 32-bit offsets to initializers
pub const S_INIT_FUNC_OFFSETS: u32 = 0x16;

/// User setable attributes
pub const SECTION_ATTRIBUTES_USR: u32 = 0xff00_0000;
/// section contains only true machine instructions
pub const S_ATTR_PURE_INSTRUCTIONS: u32 = 0x8000_0000;
/// section contains coalesced symbols that are not to be in a ranlib table of contents
pub const S_ATTR_NO_TOC: u32 = 0x4000_0000;
/// ok to strip static symbols in this section in files with the MH_DYLDLINK flag
pub const S_ATTR_STRIP_STATIC_SYMS: u32 = 0x2000_0000;
/// no dead stripping
pub const S_ATTR_NO_DEAD_STRIP: u32 = 0x1000_0000;
/// blocks are live if they reference live blocks
pub const S_ATTR_LIVE_SUPPORT: u32 = 0x0800_0000;
/// Used with i386 code stubs written on by dyld
pub const S_ATTR_SELF_MODIFYING_CODE: u32 = 0x0400_0000;
/// a debug section
pub const S_ATTR_DEBUG: u32 = 0x0200_0000;
/// system setable attributes
pub const SECTION_ATTRIBUTES_SYS: u32 = 0x00ff_ff00;
/// section contains some machine instructions
pub const S_ATTR_SOME_INSTRUCTIONS: u32 = 0x0000_0400;
/// section has external relocation entries
pub const S_ATTR_EXT_RELOC: u32 = 0x0000_0200;
/// section has local relocation entries
pub const S_ATTR_LOC_RELOC: u32 = 0x0000_0100;



/// the pagezero segment which has no protections and catches NULL references for MH_EXECUTE files
pub const SEG_PAGEZERO: &str = "__PAGEZERO";

/// the tradition UNIX text segment
pub const SEG_TEXT: &str = "__TEXT";
/// the real text part of the text section no headers, and no padding
pub const SECT_TEXT: &str = "__text";
/// the fvmlib initialization section
pub const SECT_FVMLIB_INIT0: &str = "__fvmlib_init0";
/// the section following the fvmlib initialization section
pub const SECT_FVMLIB_INIT1: &str = "__fvmlib_init1";

/// the tradition UNIX data segment
pub const SEG_DATA: &str = "__DATA";
/// the real initialized data section no padding, no bss overlap
pub const SECT_DATA: &str = "__data";
/// the real uninitialized data section no padding
pub const SECT_BSS: &str = "__bss";
/// the section common symbols are allocated in by the link editor
pub const SECT_COMMON: &str = "__common";

/// objective-C runtime segment
pub const SEG_OBJC: &str = "__OBJC";
/// symbol table
pub const SECT_OBJC_SYMBOLS: &str = "__symbol_table";
/// module information
pub const SECT_OBJC_MODULES: &str = "__module_info";
/// string table
pub const SECT_OBJC_STRINGS: &str = "__selector_strs";
/// string table
pub const SECT_OBJC_REFS: &str = "__selector_refs";

/// the icon segment
pub const SEG_ICON: &str = "__ICON";
/// the icon headers
pub const SECT_ICON_HEADER: &str = "__header";
/// the icons in tiff format
pub const SECT_ICON_TIFF: &str = "__tiff";

/// the segment containing all structs created and maintained by the link editor.  Created with -seglinkedit option to ld(1) for MH_EXECUTE and FVMLIB file types only
pub const SEG_LINKEDIT: &str = "__LINKEDIT";

/// the segment overlapping with linkedit containing linking information
pub const SEG_LINKINFO: &str = "__LINKINFO";

/// the unix stack segment
pub const SEG_UNIXSTACK: &str = "__UNIXSTACK";

/// the segment for the self (dyld) modifying code stubs that has read, write and execute permissions
pub const SEG_IMPORT: &str = "__IMPORT";

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct Fvmlib<E: Endian> {
    /// library's target pathname
    pub name: LcStr<E>,
    /// library's minor version number
    pub minor_version: U32<E>,
    /// library's header address
    pub header_addr: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct FvmlibCommand<E: Endian> {
    /// LC_IDFVMLIB or LC_LOADFVMLIB
    pub cmd: U32<E>,
    /// includes pathname string
    pub cmdsize: U32<E>,
    /// the library identification
    pub fvmlib: Fvmlib<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct Dylib<E: Endian> {
    /// library's path name
    pub name: LcStr<E>,
    /// library's build time stamp
    pub timestamp: U32<E>,
    /// library's current version number
    pub current_version: U32<E>,
    /// library's compatibility vers number
    pub compatibility_version: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct DylibCommand<E: Endian> {
    /// LC_ID_DYLIB, LC_LOAD_{,WEAK_}DYLIB, LC_REEXPORT_DYLIB
    pub cmd: U32<E>,
    /// includes pathname string
    pub cmdsize: U32<E>,
    /// the library identification
    pub dylib: Dylib<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct SubFrameworkCommand<E: Endian> {
    /// LC_SUB_FRAMEWORK
    pub cmd: U32<E>,
    /// includes umbrella string
    pub cmdsize: U32<E>,
    /// the umbrella framework name
    pub umbrella: LcStr<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct SubClientCommand<E: Endian> {
    /// LC_SUB_CLIENT
    pub cmd: U32<E>,
    /// includes client string
    pub cmdsize: U32<E>,
    /// the client name
    pub client: LcStr<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct SubUmbrellaCommand<E: Endian> {
    /// LC_SUB_UMBRELLA
    pub cmd: U32<E>,
    /// includes sub_umbrella string
    pub cmdsize: U32<E>,
    /// the sub_umbrella framework name
    pub sub_umbrella: LcStr<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct SubLibraryCommand<E: Endian> {
    /// LC_SUB_LIBRARY
    pub cmd: U32<E>,
    /// includes sub_library string
    pub cmdsize: U32<E>,
    /// the sub_library name
    pub sub_library: LcStr<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct PreboundDylibCommand<E: Endian> {
    /// LC_PREBOUND_DYLIB
    pub cmd: U32<E>,
    /// includes strings
    pub cmdsize: U32<E>,
    /// library's path name
    pub name: LcStr<E>,
    /// number of modules in library
    pub nmodules: U32<E>,
    /// bit vector of linked modules
    pub linked_modules: LcStr<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct DylinkerCommand<E: Endian> {
    /// LC_ID_DYLINKER, LC_LOAD_DYLINKER or LC_DYLD_ENVIRONMENT
    pub cmd: U32<E>,
    /// includes pathname string
    pub cmdsize: U32<E>,
    /// dynamic linker's path name
    pub name: LcStr<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct ThreadCommand<E: Endian> {
    /// LC_THREAD or  LC_UNIXTHREAD
    pub cmd: U32<E>,
    /// total size of this command
    pub cmdsize: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct RoutinesCommand32<E: Endian> {
    /// LC_ROUTINES
    pub cmd: U32<E>,
    /// total size of this command
    pub cmdsize: U32<E>,
    /// address of initialization routine
    pub init_address: U32<E>,
    /// index into the module table that the init routine is defined in
    pub init_module: U32<E>,
    pub reserved1: U32<E>,
    pub reserved2: U32<E>,
    pub reserved3: U32<E>,
    pub reserved4: U32<E>,
    pub reserved5: U32<E>,
    pub reserved6: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct RoutinesCommand64<E: Endian> {
    /// LC_ROUTINES_64
    pub cmd: U32<E>,
    /// total size of this command
    pub cmdsize: U32<E>,
    /// address of initialization routine
    pub init_address: U64<E>,
    /// index into the module table that the init routine is defined in
    pub init_module: U64<E>,
    pub reserved1: U64<E>,
    pub reserved2: U64<E>,
    pub reserved3: U64<E>,
    pub reserved4: U64<E>,
    pub reserved5: U64<E>,
    pub reserved6: U64<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct SymtabCommand<E: Endian> {
    /// LC_SYMTAB
    pub cmd: U32<E>,
    /// sizeof(struct SymtabCommand)
    pub cmdsize: U32<E>,
    /// symbol table offset
    pub symoff: U32<E>,
    /// number of symbol table entries
    pub nsyms: U32<E>,
    /// string table offset
    pub stroff: U32<E>,
    /// string table size in bytes
    pub strsize: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct DysymtabCommand<E: Endian> {
    /// LC_DYSYMTAB
    pub cmd: U32<E>,
    /// sizeof(struct DysymtabCommand)
    pub cmdsize: U32<E>,

    /// index to local symbols
    pub ilocalsym: U32<E>,
    /// number of local symbols
    pub nlocalsym: U32<E>,

    /// index to externally defined symbols
    pub iextdefsym: U32<E>,
    /// number of externally defined symbols
    pub nextdefsym: U32<E>,

    /// index to undefined symbols
    pub iundefsym: U32<E>,
    /// number of undefined symbols
    pub nundefsym: U32<E>,

    /// file offset to table of contents
    pub tocoff: U32<E>,
    /// number of entries in table of contents
    pub ntoc: U32<E>,

    /// file offset to module table
    pub modtaboff: U32<E>,
    /// number of module table entries
    pub nmodtab: U32<E>,

    /// offset to referenced symbol table
    pub extrefsymoff: U32<E>,
    /// number of referenced symbol table entries
    pub nextrefsyms: U32<E>,

    /// file offset to the indirect symbol table
    pub indirectsymoff: U32<E>,
    /// number of indirect symbol table entries
    pub nindirectsyms: U32<E>,

    /// offset to external relocation entries
    pub extreloff: U32<E>,
    /// number of external relocation entries
    pub nextrel: U32<E>,

    /// offset to local relocation entries
    pub locreloff: U32<E>,
    /// number of local relocation entries
    pub nlocrel: U32<E>,
}

pub const INDIRECT_SYMBOL_LOCAL: u32 = 0x8000_0000;
pub const INDIRECT_SYMBOL_ABS: u32 = 0x4000_0000;

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct DylibTableOfContents<E: Endian> {
    /// the defined external symbol (index into the symbol table)
    pub symbol_index: U32<E>,
    /// index into the module table this symbol is defined in
    pub module_index: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct DylibModule32<E: Endian> {
    /// the module name (index into string table)
    pub module_name: U32<E>,

    /// index into externally defined symbols
    pub iextdefsym: U32<E>,
    /// number of externally defined symbols
    pub nextdefsym: U32<E>,
    /// index into reference symbol table
    pub irefsym: U32<E>,
    /// number of reference symbol table entries
    pub nrefsym: U32<E>,
    /// index into symbols for local symbols
    pub ilocalsym: U32<E>,
    /// number of local symbols
    pub nlocalsym: U32<E>,

    /// index into external relocation entries
    pub iextrel: U32<E>,
    /// number of external relocation entries
    pub nextrel: U32<E>,

    /// low 16 bits are the index into the init section, high 16 bits are the index into the term section
    pub iinit_iterm: U32<E>,
    /// low 16 bits are the number of init section entries, high 16 bits are the number of term section entries
    pub ninit_nterm: U32<E>,

    /// for this module address of the start of the (__OBJC,__module_info) section
    pub objc_module_info_addr: U32<E>,
    /// for this module size of the (__OBJC,__module_info) section
    pub objc_module_info_size: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct DylibModule64<E: Endian> {
    /// the module name (index into string table)
    pub module_name: U32<E>,

    /// index into externally defined symbols
    pub iextdefsym: U32<E>,
    /// number of externally defined symbols
    pub nextdefsym: U32<E>,
    /// index into reference symbol table
    pub irefsym: U32<E>,
    /// number of reference symbol table entries
    pub nrefsym: U32<E>,
    /// index into symbols for local symbols
    pub ilocalsym: U32<E>,
    /// number of local symbols
    pub nlocalsym: U32<E>,

    /// index into external relocation entries
    pub iextrel: U32<E>,
    /// number of external relocation entries
    pub nextrel: U32<E>,

    /// low 16 bits are the index into the init section, high 16 bits are the index into the term section
    pub iinit_iterm: U32<E>,
    /// low 16 bits are the number of init section entries, high 16 bits are the number of term section entries
    pub ninit_nterm: U32<E>,

    /// for this module size of the (__OBJC,__module_info) section
    pub objc_module_info_size: U32<E>,
    /// for this module address of the start of the (__OBJC,__module_info) section
    pub objc_module_info_addr: U64<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct DylibReference<E: Endian> {
    pub bitfield: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct TwolevelHintsCommand<E: Endian> {
    /// LC_TWOLEVEL_HINTS
    pub cmd: U32<E>,
    /// sizeof(struct TwolevelHintsCommand)
    pub cmdsize: U32<E>,
    /// offset to the hint table
    pub offset: U32<E>,
    /// number of hints in the hint table
    pub nhints: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct TwolevelHint<E: Endian> {
    pub bitfield: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct PrebindCksumCommand<E: Endian> {
    /// LC_PREBIND_CKSUM
    pub cmd: U32<E>,
    /// sizeof(struct PrebindCksumCommand)
    pub cmdsize: U32<E>,
    /// the check sum or zero
    pub cksum: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct UuidCommand<E: Endian> {
    /// LC_UUID
    pub cmd: U32<E>,
    /// sizeof(struct UuidCommand)
    pub cmdsize: U32<E>,
    /// the 128-bit uuid
    pub uuid: [u8; 16],
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct RpathCommand<E: Endian> {
    /// LC_RPATH
    pub cmd: U32<E>,
    /// includes string
    pub cmdsize: U32<E>,
    /// path to add to run path
    pub path: LcStr<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct LinkeditDataCommand<E: Endian> {
    /// `LC_CODE_SIGNATURE`, `LC_SEGMENT_SPLIT_INFO`, `LC_FUNCTION_STARTS`,
    /// `LC_DATA_IN_CODE`, `LC_DYLIB_CODE_SIGN_DRS`, `LC_LINKER_OPTIMIZATION_HINT`,
    /// `LC_DYLD_EXPORTS_TRIE`, or `LC_DYLD_CHAINED_FIXUPS`.
    pub cmd: U32<E>,
    /// sizeof(struct LinkeditDataCommand)
    pub cmdsize: U32<E>,
    /// file offset of data in __LINKEDIT segment
    pub dataoff: U32<E>,
    /// file size of data in __LINKEDIT segment
    pub datasize: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct FilesetEntryCommand<E: Endian> {
    pub cmd: U32<E>,
    /// includes id string
    pub cmdsize: U32<E>,
    /// memory address of the dylib
    pub vmaddr: U64<E>,
    /// file offset of the dylib
    pub fileoff: U64<E>,
    /// contained entry id
    pub entry_id: LcStr<E>,
    /// entry_id is 32-bits long, so this is the reserved padding
    pub reserved: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct EncryptionInfoCommand32<E: Endian> {
    /// LC_ENCRYPTION_INFO
    pub cmd: U32<E>,
    /// sizeof(struct EncryptionInfoCommand32)
    pub cmdsize: U32<E>,
    /// file offset of encrypted range
    pub cryptoff: U32<E>,
    /// file size of encrypted range
    pub cryptsize: U32<E>,
    /// which enryption system, 0 means not-encrypted yet
    pub cryptid: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct EncryptionInfoCommand64<E: Endian> {
    /// LC_ENCRYPTION_INFO_64
    pub cmd: U32<E>,
    /// sizeof(struct EncryptionInfoCommand64)
    pub cmdsize: U32<E>,
    /// file offset of encrypted range
    pub cryptoff: U32<E>,
    /// file size of encrypted range
    pub cryptsize: U32<E>,
    /// which enryption system, 0 means not-encrypted yet
    pub cryptid: U32<E>,
    /// padding to make this struct's size a multiple of 8 bytes
    pub pad: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct VersionMinCommand<E: Endian> {
    /// LC_VERSION_MIN_MACOSX or LC_VERSION_MIN_IPHONEOS or LC_VERSION_MIN_WATCHOS or LC_VERSION_MIN_TVOS
    pub cmd: U32<E>,
    /// sizeof(struct VersionMinCommand)
    pub cmdsize: U32<E>,
    /// X.Y.Z is encoded in nibbles xxxx.yy.zz
    pub version: U32<E>,
    /// X.Y.Z is encoded in nibbles xxxx.yy.zz
    pub sdk: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct BuildVersionCommand<E: Endian> {
    /// LC_BUILD_VERSION
    pub cmd: U32<E>,
    /// sizeof(struct BuildVersionCommand) plus ntools * sizeof(struct BuildToolVersion)
    pub cmdsize: U32<E>,
    /// platform
    pub platform: U32<E>,
    /// X.Y.Z is encoded in nibbles xxxx.yy.zz
    pub minos: U32<E>,
    /// X.Y.Z is encoded in nibbles xxxx.yy.zz
    pub sdk: U32<E>,
    /// number of tool entries following this
    pub ntools: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct BuildToolVersion<E: Endian> {
    /// enum for the tool
    pub tool: U32<E>,
    /// version number of the tool
    pub version: U32<E>,
}

pub const PLATFORM_MACOS: u32 = 1;
pub const PLATFORM_IOS: u32 = 2;
pub const PLATFORM_TVOS: u32 = 3;
pub const PLATFORM_WATCHOS: u32 = 4;
pub const PLATFORM_BRIDGEOS: u32 = 5;
pub const PLATFORM_MACCATALYST: u32 = 6;
pub const PLATFORM_IOSSIMULATOR: u32 = 7;
pub const PLATFORM_TVOSSIMULATOR: u32 = 8;
pub const PLATFORM_WATCHOSSIMULATOR: u32 = 9;
pub const PLATFORM_DRIVERKIT: u32 = 10;
pub const PLATFORM_XROS: u32 = 11;
pub const PLATFORM_XROSSIMULATOR: u32 = 12;

pub const TOOL_CLANG: u32 = 1;
pub const TOOL_SWIFT: u32 = 2;
pub const TOOL_LD: u32 = 3;

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct DyldInfoCommand<E: Endian> {
    /// LC_DYLD_INFO or LC_DYLD_INFO_ONLY
    pub cmd: U32<E>,
    /// sizeof(struct DyldInfoCommand)
    pub cmdsize: U32<E>,

    /// file offset to rebase info
    pub rebase_off: U32<E>,
    /// size of rebase info
    pub rebase_size: U32<E>,

    /// file offset to binding info
    pub bind_off: U32<E>,
    /// size of binding info
    pub bind_size: U32<E>,

    /// file offset to weak binding info
    pub weak_bind_off: U32<E>,
    /// size of weak binding info
    pub weak_bind_size: U32<E>,

    /// file offset to lazy binding info
    pub lazy_bind_off: U32<E>,
    /// size of lazy binding infs
    pub lazy_bind_size: U32<E>,

    /// file offset to lazy binding info
    pub export_off: U32<E>,
    /// size of lazy binding infs
    pub export_size: U32<E>,
}

pub const REBASE_TYPE_POINTER: u8 = 1;
pub const REBASE_TYPE_TEXT_ABSOLUTE32: u8 = 2;
pub const REBASE_TYPE_TEXT_PCREL32: u8 = 3;

pub const REBASE_OPCODE_MASK: u8 = 0xF0;
pub const REBASE_IMMEDIATE_MASK: u8 = 0x0F;
pub const REBASE_OPCODE_DONE: u8 = 0x00;
pub const REBASE_OPCODE_SET_TYPE_IMM: u8 = 0x10;
pub const REBASE_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: u8 = 0x20;
pub const REBASE_OPCODE_ADD_ADDR_ULEB: u8 = 0x30;
pub const REBASE_OPCODE_ADD_ADDR_IMM_SCALED: u8 = 0x40;
pub const REBASE_OPCODE_DO_REBASE_IMM_TIMES: u8 = 0x50;
pub const REBASE_OPCODE_DO_REBASE_ULEB_TIMES: u8 = 0x60;
pub const REBASE_OPCODE_DO_REBASE_ADD_ADDR_ULEB: u8 = 0x70;
pub const REBASE_OPCODE_DO_REBASE_ULEB_TIMES_SKIPPING_ULEB: u8 = 0x80;

pub const BIND_TYPE_POINTER: u8 = 1;
pub const BIND_TYPE_TEXT_ABSOLUTE32: u8 = 2;
pub const BIND_TYPE_TEXT_PCREL32: u8 = 3;

pub const BIND_SPECIAL_DYLIB_SELF: i8 = 0;
pub const BIND_SPECIAL_DYLIB_MAIN_EXECUTABLE: i8 = -1;
pub const BIND_SPECIAL_DYLIB_FLAT_LOOKUP: i8 = -2;
pub const BIND_SPECIAL_DYLIB_WEAK_LOOKUP: i8 = -3;

pub const BIND_SYMBOL_FLAGS_WEAK_IMPORT: u8 = 0x1;
pub const BIND_SYMBOL_FLAGS_NON_WEAK_DEFINITION: u8 = 0x8;

pub const BIND_OPCODE_MASK: u8 = 0xF0;
pub const BIND_IMMEDIATE_MASK: u8 = 0x0F;
pub const BIND_OPCODE_DONE: u8 = 0x00;
pub const BIND_OPCODE_SET_DYLIB_ORDINAL_IMM: u8 = 0x10;
pub const BIND_OPCODE_SET_DYLIB_ORDINAL_ULEB: u8 = 0x20;
pub const BIND_OPCODE_SET_DYLIB_SPECIAL_IMM: u8 = 0x30;
pub const BIND_OPCODE_SET_SYMBOL_TRAILING_FLAGS_IMM: u8 = 0x40;
pub const BIND_OPCODE_SET_TYPE_IMM: u8 = 0x50;
pub const BIND_OPCODE_SET_ADDEND_SLEB: u8 = 0x60;
pub const BIND_OPCODE_SET_SEGMENT_AND_OFFSET_ULEB: u8 = 0x70;
pub const BIND_OPCODE_ADD_ADDR_ULEB: u8 = 0x80;
pub const BIND_OPCODE_DO_BIND: u8 = 0x90;
pub const BIND_OPCODE_DO_BIND_ADD_ADDR_ULEB: u8 = 0xA0;
pub const BIND_OPCODE_DO_BIND_ADD_ADDR_IMM_SCALED: u8 = 0xB0;
pub const BIND_OPCODE_DO_BIND_ULEB_TIMES_SKIPPING_ULEB: u8 = 0xC0;
pub const BIND_OPCODE_THREADED: u8 = 0xD0;
pub const BIND_SUBOPCODE_THREADED_SET_BIND_ORDINAL_TABLE_SIZE_ULEB: u8 = 0x00;
pub const BIND_SUBOPCODE_THREADED_APPLY: u8 = 0x01;

pub const EXPORT_SYMBOL_FLAGS_KIND_MASK: u32 = 0x03;
pub const EXPORT_SYMBOL_FLAGS_KIND_REGULAR: u32 = 0x00;
pub const EXPORT_SYMBOL_FLAGS_KIND_THREAD_LOCAL: u32 = 0x01;
pub const EXPORT_SYMBOL_FLAGS_KIND_ABSOLUTE: u32 = 0x02;
pub const EXPORT_SYMBOL_FLAGS_WEAK_DEFINITION: u32 = 0x04;
pub const EXPORT_SYMBOL_FLAGS_REEXPORT: u32 = 0x08;
pub const EXPORT_SYMBOL_FLAGS_STUB_AND_RESOLVER: u32 = 0x10;

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct LinkerOptionCommand<E: Endian> {
    /// LC_LINKER_OPTION only used in MH_OBJECT filetypes
    pub cmd: U32<E>,
    pub cmdsize: U32<E>,
    /// number of strings
    pub count: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct SymsegCommand<E: Endian> {
    /// LC_SYMSEG
    pub cmd: U32<E>,
    /// sizeof(struct SymsegCommand)
    pub cmdsize: U32<E>,
    /// symbol segment offset
    pub offset: U32<E>,
    /// symbol segment size in bytes
    pub size: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct IdentCommand<E: Endian> {
    /// LC_IDENT
    pub cmd: U32<E>,
    /// strings that follow this command
    pub cmdsize: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct FvmfileCommand<E: Endian> {
    /// LC_FVMFILE
    pub cmd: U32<E>,
    /// includes pathname string
    pub cmdsize: U32<E>,
    /// files pathname
    pub name: LcStr<E>,
    /// files virtual address
    pub header_addr: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct EntryPointCommand<E: Endian> {
    /// LC_MAIN only used in MH_EXECUTE filetypes
    pub cmd: U32<E>,
    /// 24
    pub cmdsize: U32<E>,
    /// file (__TEXT) offset of main()
    pub entryoff: U64<E>,
    /// if not zero, initial stack size
    pub stacksize: U64<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct SourceVersionCommand<E: Endian> {
    /// LC_SOURCE_VERSION
    pub cmd: U32<E>,
    /// 16
    pub cmdsize: U32<E>,
    /// A.B.C.D.E packed as a24.b10.c10.d10.e10
    pub version: U64<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct DataInCodeEntry<E: Endian> {
    /// from mach_header to start of data range
    pub offset: U32<E>,
    /// number of bytes in data range
    pub length: U16<E>,
    /// a DICE_KIND_* value
    pub kind: U16<E>,
}
pub const DICE_KIND_DATA: u32 = 0x0001;
pub const DICE_KIND_JUMP_TABLE8: u32 = 0x0002;
pub const DICE_KIND_JUMP_TABLE16: u32 = 0x0003;
pub const DICE_KIND_JUMP_TABLE32: u32 = 0x0004;
pub const DICE_KIND_ABS_JUMP_TABLE32: u32 = 0x0005;


#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct NoteCommand<E: Endian> {
    /// LC_NOTE
    pub cmd: U32<E>,
    /// sizeof(struct NoteCommand)
    pub cmdsize: U32<E>,
    /// owner name for this LC_NOTE
    pub data_owner: [u8; 16],
    /// file offset of this data
    pub offset: U64<E>,
    /// length of data region
    pub size: U64<E>,
}


#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct Nlist32<E: Endian> {
    /// index into the string table
    pub n_strx: U32<E>,
    /// type flag, see below
    pub n_type: u8,
    /// section number or NO_SECT
    pub n_sect: u8,
    /// see <mach-o/stab.h>
    pub n_desc: U16<E>,
    /// value of this symbol (or stab offset)
    pub n_value: U32<E>,
}

#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct Nlist64<E: Endian> {
    /// index into the string table
    pub n_strx: U32<E>,
    /// type flag, see below
    pub n_type: u8,
    /// section number or NO_SECT
    pub n_sect: u8,
    /// see <mach-o/stab.h>
    pub n_desc: U16<E>,
    /// value of this symbol (or stab offset)
    pub n_value: U64Bytes<E>,
}


/// if any of these bits set, a symbolic debugging entry
pub const N_STAB: u8 = 0xe0;
/// private external symbol bit
pub const N_PEXT: u8 = 0x10;
/// mask for the type bits
pub const N_TYPE: u8 = 0x0e;
/// external symbol bit, set for external symbols
pub const N_EXT: u8 = 0x01;


/// undefined, n_sect == NO_SECT
pub const N_UNDF: u8 = 0x0;
/// absolute, n_sect == NO_SECT
pub const N_ABS: u8 = 0x2;
/// defined in section number n_sect
pub const N_SECT: u8 = 0xe;
/// prebound undefined (defined in a dylib)
pub const N_PBUD: u8 = 0xc;
/// indirect
pub const N_INDR: u8 = 0xa;


/// symbol is not in any section
pub const NO_SECT: u8 = 0;
/// 1 thru 255 inclusive
pub const MAX_SECT: u8 = 255;


pub const REFERENCE_TYPE: u16 = 0x7;
pub const REFERENCE_FLAG_UNDEFINED_NON_LAZY: u16 = 0;
pub const REFERENCE_FLAG_UNDEFINED_LAZY: u16 = 1;
pub const REFERENCE_FLAG_DEFINED: u16 = 2;
pub const REFERENCE_FLAG_PRIVATE_DEFINED: u16 = 3;
pub const REFERENCE_FLAG_PRIVATE_UNDEFINED_NON_LAZY: u16 = 4;
pub const REFERENCE_FLAG_PRIVATE_UNDEFINED_LAZY: u16 = 5;

pub const REFERENCED_DYNAMICALLY: u16 = 0x0010;

pub const SELF_LIBRARY_ORDINAL: u8 = 0x0;
pub const MAX_LIBRARY_ORDINAL: u8 = 0xfd;
pub const DYNAMIC_LOOKUP_ORDINAL: u8 = 0xfe;
pub const EXECUTABLE_ORDINAL: u8 = 0xff;


/// symbol is not to be dead stripped
pub const N_NO_DEAD_STRIP: u16 = 0x0020;

/// symbol is discarded
pub const N_DESC_DISCARDED: u16 = 0x0020;

/// symbol is weak referenced
pub const N_WEAK_REF: u16 = 0x0040;

/// coalesced symbol is a weak definition
pub const N_WEAK_DEF: u16 = 0x0080;

/// reference to a weak symbol
pub const N_REF_TO_WEAK: u16 = 0x0080;

/// symbol is a Thumb function (ARM)
pub const N_ARM_THUMB_DEF: u16 = 0x0008;

pub const N_SYMBOL_RESOLVER: u16 = 0x0100;

pub const N_ALT_ENTRY: u16 = 0x0200;



/// global symbol: name,,NO_SECT,type,0
pub const N_GSYM: u8 = 0x20;
/// procedure name (f77 kludge): name,,NO_SECT,0,0
pub const N_FNAME: u8 = 0x22;
/// procedure: name,,n_sect,linenumber,address
pub const N_FUN: u8 = 0x24;
/// static symbol: name,,n_sect,type,address
pub const N_STSYM: u8 = 0x26;
/// .lcomm symbol: name,,n_sect,type,address
pub const N_LCSYM: u8 = 0x28;
/// begin nsect sym: 0,,n_sect,0,address
pub const N_BNSYM: u8 = 0x2e;
/// AST file path: name,,NO_SECT,0,0
pub const N_AST: u8 = 0x32;
/// emitted with gcc2_compiled and in gcc source
pub const N_OPT: u8 = 0x3c;
/// register sym: name,,NO_SECT,type,register
pub const N_RSYM: u8 = 0x40;
/// src line: 0,,n_sect,linenumber,address
pub const N_SLINE: u8 = 0x44;
/// end nsect sym: 0,,n_sect,0,address
pub const N_ENSYM: u8 = 0x4e;
/// structure elt: name,,NO_SECT,type,struct_offset
pub const N_SSYM: u8 = 0x60;
/// source file name: name,,n_sect,0,address
pub const N_SO: u8 = 0x64;
/// object file name: name,,0,0,st_mtime
pub const N_OSO: u8 = 0x66;
/// local sym: name,,NO_SECT,type,offset
pub const N_LSYM: u8 = 0x80;
/// include file beginning: name,,NO_SECT,0,sum
pub const N_BINCL: u8 = 0x82;
/// #included file name: name,,n_sect,0,address
pub const N_SOL: u8 = 0x84;
/// compiler parameters: name,,NO_SECT,0,0
pub const N_PARAMS: u8 = 0x86;
/// compiler version: name,,NO_SECT,0,0
pub const N_VERSION: u8 = 0x88;
/// compiler -O level: name,,NO_SECT,0,0
pub const N_OLEVEL: u8 = 0x8A;
/// parameter: name,,NO_SECT,type,offset
pub const N_PSYM: u8 = 0xa0;
/// include file end: name,,NO_SECT,0,0
pub const N_EINCL: u8 = 0xa2;
/// alternate entry: name,,n_sect,linenumber,address
pub const N_ENTRY: u8 = 0xa4;
/// left bracket: 0,,NO_SECT,nesting level,address
pub const N_LBRAC: u8 = 0xc0;
/// deleted include file: name,,NO_SECT,0,sum
pub const N_EXCL: u8 = 0xc2;
/// right bracket: 0,,NO_SECT,nesting level,address
pub const N_RBRAC: u8 = 0xe0;
/// begin common: name,,NO_SECT,0,0
pub const N_BCOMM: u8 = 0xe2;
/// end common: name,,n_sect,0,0
pub const N_ECOMM: u8 = 0xe4;
/// end common (local name): 0,,n_sect,0,address
pub const N_ECOML: u8 = 0xe8;
/// second stab entry with length information
pub const N_LENG: u8 = 0xfe;

/// global pascal symbol: name,,NO_SECT,subtype,line
pub const N_PC: u8 = 0x30;


/// A relocation entry.
///
/// Mach-O relocations have plain and scattered variants, with the
/// meaning of the fields depending on the variant.
///
/// This type provides functions for determining whether the relocation
/// is scattered, and for accessing the fields of each variant.
#[derive(Debug, Clone, Copy)]
#[repr(C)]
pub struct Relocation<E: Endian> {
    pub r_word0: U32<E>,
    pub r_word1: U32<E>,
}

impl<E: Endian> Relocation<E> {
    /// Determine whether this is a scattered relocation.
    #[inline]
    pub fn r_scattered(self, endian: E, cputype: u32) -> bool {
        if cputype == CPU_TYPE_X86_64 {
            false
        } else {
            self.r_word0.get(endian) & R_SCATTERED != 0
        }
    }

    /// Return the fields of a plain relocation.
    pub fn info(self, endian: E) -> RelocationInfo {
        let r_address = self.r_word0.get(endian);
        let r_word1 = self.r_word1.get(endian);
        if endian.is_little_endian() {
            RelocationInfo {
                r_address,
                r_symbolnum: r_word1 & 0x00ff_ffff,
                r_pcrel: ((r_word1 >> 24) & 0x1) != 0,
                r_length: ((r_word1 >> 25) & 0x3) as u8,
                r_extern: ((r_word1 >> 27) & 0x1) != 0,
                r_type: (r_word1 >> 28) as u8,
            }
        } else {
            RelocationInfo {
                r_address,
                r_symbolnum: r_word1 >> 8,
                r_pcrel: ((r_word1 >> 7) & 0x1) != 0,
                r_length: ((r_word1 >> 5) & 0x3) as u8,
                r_extern: ((r_word1 >> 4) & 0x1) != 0,
                r_type: (r_word1 & 0xf) as u8,
            }
        }
    }

    /// Return the fields of a scattered relocation.
    pub fn scattered_info(self, endian: E) -> ScatteredRelocationInfo {
        let r_word0 = self.r_word0.get(endian);
        let r_value = self.r_word1.get(endian);
        ScatteredRelocationInfo {
            r_address: r_word0 & 0x00ff_ffff,
            r_type: ((r_word0 >> 24) & 0xf) as u8,
            r_length: ((r_word0 >> 28) & 0x3) as u8,
            r_pcrel: ((r_word0 >> 30) & 0x1) != 0,
            r_value,
        }
    }
}


#[derive(Debug, Clone, Copy)]
pub struct RelocationInfo {
    /// offset in the section to what is being relocated
    pub r_address: u32,
    /// symbol index if r_extern == 1 or section ordinal if r_extern == 0
    pub r_symbolnum: u32,
    /// was relocated pc relative already
    pub r_pcrel: bool,
    /// 0=byte, 1=word, 2=long, 3=quad
    pub r_length: u8,
    /// does not include value of sym referenced
    pub r_extern: bool,
    /// if not 0, machine specific relocation type
    pub r_type: u8,
}

impl RelocationInfo {
    /// Combine the fields into a `Relocation`.
    pub fn relocation<E: Endian>(self, endian: E) -> Relocation<E> {
        let r_word0 = U32::new(endian, self.r_address);
        let r_word1 = U32::new(
            endian,
            if endian.is_little_endian() {
                self.r_symbolnum & 0x00ff_ffff
                    | u32::from(self.r_pcrel) << 24
                    | u32::from(self.r_length & 0x3) << 25
                    | u32::from(self.r_extern) << 27
                    | u32::from(self.r_type) << 28
            } else {
                self.r_symbolnum >> 8
                    | u32::from(self.r_pcrel) << 7
                    | u32::from(self.r_length & 0x3) << 5
                    | u32::from(self.r_extern) << 4
                    | u32::from(self.r_type) & 0xf
            },
        );
        Relocation { r_word0, r_word1 }
    }
}

/// absolute relocation type for Mach-O files
pub const R_ABS: u8 = 0;




/// Bit set in `Relocation::r_word0` for scattered relocations.
pub const R_SCATTERED: u32 = 0x8000_0000;

#[derive(Debug, Clone, Copy)]
pub struct ScatteredRelocationInfo {
    /// offset in the section to what is being relocated
    pub r_address: u32,
    /// if not 0, machine specific relocation type
    pub r_type: u8,
    /// 0=byte, 1=word, 2=long, 3=quad
    pub r_length: u8,
    /// was relocated pc relative already
    pub r_pcrel: bool,
    /// the value the item to be relocated is referring to (without any offset added)
    pub r_value: u32,
}

impl ScatteredRelocationInfo {
    /// Combine the fields into a `Relocation`.
    pub fn relocation<E: Endian>(self, endian: E) -> Relocation<E> {
        let r_word0 = U32::new(
            endian,
            self.r_address & 0x00ff_ffff
                | u32::from(self.r_type & 0xf) << 24
                | u32::from(self.r_length & 0x3) << 28
                | u32::from(self.r_pcrel) << 30
                | R_SCATTERED,
        );
        let r_word1 = U32::new(endian, self.r_value);
        Relocation { r_word0, r_word1 }
    }
}

/// generic relocation as described above
pub const GENERIC_RELOC_VANILLA: u8 = 0;
/// Only follows a GENERIC_RELOC_SECTDIFF
pub const GENERIC_RELOC_PAIR: u8 = 1;
pub const GENERIC_RELOC_SECTDIFF: u8 = 2;
/// prebound lazy pointer
pub const GENERIC_RELOC_PB_LA_PTR: u8 = 3;
pub const GENERIC_RELOC_LOCAL_SECTDIFF: u8 = 4;
/// thread local variables
pub const GENERIC_RELOC_TLV: u8 = 5;


/// generic relocation as described above
pub const ARM_RELOC_VANILLA: u8 = 0;
/// the second relocation entry of a pair
pub const ARM_RELOC_PAIR: u8 = 1;
/// a PAIR follows with subtract symbol value
pub const ARM_RELOC_SECTDIFF: u8 = 2;
/// like ARM_RELOC_SECTDIFF, but the symbol referenced was local.
pub const ARM_RELOC_LOCAL_SECTDIFF: u8 = 3;
/// prebound lazy pointer
pub const ARM_RELOC_PB_LA_PTR: u8 = 4;
/// 24 bit branch displacement (to a word address)
pub const ARM_RELOC_BR24: u8 = 5;
/// 22 bit branch displacement (to a half-word address)
pub const ARM_THUMB_RELOC_BR22: u8 = 6;
/// obsolete - a thumb 32-bit branch instruction possibly needing page-spanning branch workaround
pub const ARM_THUMB_32BIT_BRANCH: u8 = 7;

pub const ARM_RELOC_HALF: u8 = 8;
pub const ARM_RELOC_HALF_SECTDIFF: u8 = 9;


/// for pointers
pub const ARM64_RELOC_UNSIGNED: u8 = 0;
/// must be followed by a ARM64_RELOC_UNSIGNED
pub const ARM64_RELOC_SUBTRACTOR: u8 = 1;
/// a B/BL instruction with 26-bit displacement
pub const ARM64_RELOC_BRANCH26: u8 = 2;
/// pc-rel distance to page of target
pub const ARM64_RELOC_PAGE21: u8 = 3;
/// offset within page, scaled by r_length
pub const ARM64_RELOC_PAGEOFF12: u8 = 4;
/// pc-rel distance to page of GOT slot
pub const ARM64_RELOC_GOT_LOAD_PAGE21: u8 = 5;
/// offset within page of GOT slot, scaled by r_length
pub const ARM64_RELOC_GOT_LOAD_PAGEOFF12: u8 = 6;
/// for pointers to GOT slots
pub const ARM64_RELOC_POINTER_TO_GOT: u8 = 7;
/// pc-rel distance to page of TLVP slot
pub const ARM64_RELOC_TLVP_LOAD_PAGE21: u8 = 8;
/// offset within page of TLVP slot, scaled by r_length
pub const ARM64_RELOC_TLVP_LOAD_PAGEOFF12: u8 = 9;
/// must be followed by PAGE21 or PAGEOFF12
pub const ARM64_RELOC_ADDEND: u8 = 10;

pub const ARM64_RELOC_AUTHENTICATED_POINTER: u8 = 11;


/// generic relocation as described above
pub const PPC_RELOC_VANILLA: u8 = 0;
/// the second relocation entry of a pair
pub const PPC_RELOC_PAIR: u8 = 1;
/// 14 bit branch displacement (to a word address)
pub const PPC_RELOC_BR14: u8 = 2;
/// 24 bit branch displacement (to a word address)
pub const PPC_RELOC_BR24: u8 = 3;
/// a PAIR follows with the low half
pub const PPC_RELOC_HI16: u8 = 4;
/// a PAIR follows with the high half
pub const PPC_RELOC_LO16: u8 = 5;
/// Same as the RELOC_HI16 except the low 16 bits and the high 16 bits are added together
/// with the low 16 bits sign extended first.  This means if bit 15 of the low 16 bits is
/// set the high 16 bits stored in the instruction will be adjusted.
pub const PPC_RELOC_HA16: u8 = 6;
/// Same as the LO16 except that the low 2 bits are not stored in the instruction and are
/// always zero.  This is used in double word load/store instructions.
pub const PPC_RELOC_LO14: u8 = 7;
/// a PAIR follows with subtract symbol value
pub const PPC_RELOC_SECTDIFF: u8 = 8;
/// prebound lazy pointer
pub const PPC_RELOC_PB_LA_PTR: u8 = 9;
/// section difference forms of above.  a PAIR
pub const PPC_RELOC_HI16_SECTDIFF: u8 = 10;
/// follows these with subtract symbol value
pub const PPC_RELOC_LO16_SECTDIFF: u8 = 11;
pub const PPC_RELOC_HA16_SECTDIFF: u8 = 12;
pub const PPC_RELOC_JBSR: u8 = 13;
pub const PPC_RELOC_LO14_SECTDIFF: u8 = 14;
/// like PPC_RELOC_SECTDIFF, but the symbol referenced was local.
pub const PPC_RELOC_LOCAL_SECTDIFF: u8 = 15;


/// for absolute addresses
pub const X86_64_RELOC_UNSIGNED: u8 = 0;
/// for signed 32-bit displacement
pub const X86_64_RELOC_SIGNED: u8 = 1;
/// a CALL/JMP instruction with 32-bit displacement
pub const X86_64_RELOC_BRANCH: u8 = 2;
/// a MOVQ load of a GOT entry
pub const X86_64_RELOC_GOT_LOAD: u8 = 3;
/// other GOT references
pub const X86_64_RELOC_GOT: u8 = 4;
/// must be followed by a X86_64_RELOC_UNSIGNED
pub const X86_64_RELOC_SUBTRACTOR: u8 = 5;
/// for signed 32-bit displacement with a -1 addend
pub const X86_64_RELOC_SIGNED_1: u8 = 6;
/// for signed 32-bit displacement with a -2 addend
pub const X86_64_RELOC_SIGNED_2: u8 = 7;
/// for signed 32-bit displacement with a -4 addend
pub const X86_64_RELOC_SIGNED_4: u8 = 8;
/// for thread local variables
pub const X86_64_RELOC_TLV: u8 = 9;

unsafe_impl_pod!(FatHeader, FatArch32, FatArch64,);
unsafe_impl_endian_pod!(
    DyldCacheHeader,
    DyldCacheMappingInfo,
    DyldCacheImageInfo,
    DyldSubCacheEntryV1,
    DyldSubCacheEntryV2,
    MachHeader32,
    MachHeader64,
    LoadCommand,
    LcStr,
    SegmentCommand32,
    SegmentCommand64,
    Section32,
    Section64,
    Fvmlib,
    FvmlibCommand,
    Dylib,
    DylibCommand,
    SubFrameworkCommand,
    SubClientCommand,
    SubUmbrellaCommand,
    SubLibraryCommand,
    PreboundDylibCommand,
    DylinkerCommand,
    ThreadCommand,
    RoutinesCommand32,
    RoutinesCommand64,
    SymtabCommand,
    DysymtabCommand,
    DylibTableOfContents,
    DylibModule32,
    DylibModule64,
    DylibReference,
    TwolevelHintsCommand,
    TwolevelHint,
    PrebindCksumCommand,
    UuidCommand,
    RpathCommand,
    LinkeditDataCommand,
    FilesetEntryCommand,
    EncryptionInfoCommand32,
    EncryptionInfoCommand64,
    VersionMinCommand,
    BuildVersionCommand,
    BuildToolVersion,
    DyldInfoCommand,
    LinkerOptionCommand,
    SymsegCommand,
    IdentCommand,
    FvmfileCommand,
    EntryPointCommand,
    SourceVersionCommand,
    DataInCodeEntry,
    NoteCommand,
    Nlist32,
    Nlist64,
    Relocation,
);
