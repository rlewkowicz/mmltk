use alloc::vec::Vec;
use scroll::{Pread, Pwrite};

use log::debug;

use crate::error;

use crate::pe::data_directories;
use crate::pe::options;
use crate::pe::section_table;
use crate::pe::utils;

#[repr(C)]
#[derive(Debug, PartialEq, Copy, Clone, Default, Pread, Pwrite)]
pub struct ExportDirectoryTable {
    pub export_flags: u32,
    pub time_date_stamp: u32,
    pub major_version: u16,
    pub minor_version: u16,
    pub name_rva: u32,
    pub ordinal_base: u32,
    pub address_table_entries: u32,
    pub number_of_name_pointers: u32,
    pub export_address_table_rva: u32,
    pub name_pointer_rva: u32,
    pub ordinal_table_rva: u32,
}

pub const SIZEOF_EXPORT_DIRECTORY_TABLE: usize = 40;

impl ExportDirectoryTable {
    pub fn parse(bytes: &[u8], offset: usize) -> error::Result<Self> {
        let res = bytes.pread_with(offset, scroll::LE)?;
        Ok(res)
    }
}

#[derive(Debug)]
pub enum ExportAddressTableEntry {
    ExportRVA(u32),
    ForwarderRVA(u32),
}

pub const SIZEOF_EXPORT_ADDRESS_TABLE_ENTRY: usize = 4;

pub type ExportAddressTable = Vec<ExportAddressTableEntry>;

/// Array of rvas into the export name table
///
/// Export name is defined iff pointer table has pointer to the name
pub type ExportNamePointerTable = Vec<u32>;

/// Array of indexes into the export address table.
///
/// Should obey the formula `idx = ordinal - ordinalbase`
pub type ExportOrdinalTable = Vec<u16>;

#[derive(Debug, Default)]
/// Export data contains the `dll` name which other libraries can import symbols by (two-level namespace), as well as other important indexing data allowing symbol lookups
pub struct ExportData<'a> {
    pub name: Option<&'a str>,
    pub export_directory_table: ExportDirectoryTable,
    pub export_name_pointer_table: ExportNamePointerTable,
    pub export_ordinal_table: ExportOrdinalTable,
    pub export_address_table: ExportAddressTable,
}

impl<'a> ExportData<'a> {
    pub fn parse(
        bytes: &'a [u8],
        dd: data_directories::DataDirectory,
        sections: &[section_table::SectionTable],
        file_alignment: u32,
    ) -> error::Result<ExportData<'a>> {
        Self::parse_with_opts(
            bytes,
            dd,
            sections,
            file_alignment,
            &options::ParseOptions::default(),
        )
    }

    pub fn parse_with_opts(
        bytes: &'a [u8],
        dd: data_directories::DataDirectory,
        sections: &[section_table::SectionTable],
        file_alignment: u32,
        opts: &options::ParseOptions,
    ) -> error::Result<ExportData<'a>> {
        let export_rva = dd.virtual_address as usize;
        let size = dd.size as usize;
        debug!("export_rva {:#x} size {:#}", export_rva, size);
        let export_offset = utils::find_offset_or(
            export_rva,
            sections,
            file_alignment,
            opts,
            &format!("cannot map export_rva ({:#x}) into offset", export_rva),
        )?;
        let export_directory_table =
            ExportDirectoryTable::parse(bytes, export_offset).map_err(|_| {
                error::Error::Malformed(format!(
                    "cannot parse export_directory_table (offset {:#x})",
                    export_offset
                ))
            })?;
        let number_of_name_pointers = export_directory_table.number_of_name_pointers as usize;
        let address_table_entries = export_directory_table.address_table_entries as usize;

        if number_of_name_pointers > bytes.len() {
            return Err(error::Error::BufferTooShort(
                number_of_name_pointers,
                "name pointers",
            ));
        }
        if address_table_entries > bytes.len() {
            return Err(error::Error::BufferTooShort(
                address_table_entries,
                "address table entries",
            ));
        }

        let export_name_pointer_table = utils::find_offset(
            export_directory_table.name_pointer_rva as usize,
            sections,
            file_alignment,
            opts,
        )
        .map_or(vec![], |table_offset| {
            let mut offset = table_offset;
            let mut table: ExportNamePointerTable = Vec::with_capacity(number_of_name_pointers);

            for _ in 0..number_of_name_pointers {
                if let Ok(name_rva) = bytes.gread_with(&mut offset, scroll::LE) {
                    table.push(name_rva);
                } else {
                    break;
                }
            }

            table
        });

        let export_ordinal_table = utils::find_offset(
            export_directory_table.ordinal_table_rva as usize,
            sections,
            file_alignment,
            opts,
        )
        .map_or(vec![], |table_offset| {
            let mut offset = table_offset;
            let mut table: ExportOrdinalTable = Vec::with_capacity(number_of_name_pointers);

            for _ in 0..number_of_name_pointers {
                if let Ok(name_ordinal) = bytes.gread_with(&mut offset, scroll::LE) {
                    table.push(name_ordinal);
                } else {
                    break;
                }
            }

            table
        });

        let export_address_table = utils::find_offset(
            export_directory_table.export_address_table_rva as usize,
            sections,
            file_alignment,
            opts,
        )
        .map_or(vec![], |table_offset| {
            let mut offset = table_offset;
            let mut table: ExportAddressTable = Vec::with_capacity(address_table_entries);
            let export_end = export_rva + size;

            for _ in 0..address_table_entries {
                if let Ok(func_rva) = bytes.gread_with::<u32>(&mut offset, scroll::LE) {
                    if utils::is_in_range(func_rva as usize, export_rva, export_end) {
                        table.push(ExportAddressTableEntry::ForwarderRVA(func_rva));
                    } else {
                        table.push(ExportAddressTableEntry::ExportRVA(func_rva));
                    }
                } else {
                    break;
                }
            }

            table
        });

        let name = utils::find_offset(
            export_directory_table.name_rva as usize,
            sections,
            file_alignment,
            opts,
        )
        .and_then(|offset| bytes.pread(offset).ok());

        Ok(ExportData {
            name,
            export_directory_table,
            export_name_pointer_table,
            export_ordinal_table,
            export_address_table,
        })
    }
}

#[derive(Debug)]
/// PE binaries have two kinds of reexports, either specifying the dll's name, or the ordinal value of the dll
pub enum Reexport<'a> {
    DLLName { export: &'a str, lib: &'a str },
    DLLOrdinal { ordinal: usize, lib: &'a str },
}

impl<'a> scroll::ctx::TryFromCtx<'a, scroll::Endian> for Reexport<'a> {
    type Error = crate::error::Error;
    #[inline]
    fn try_from_ctx(bytes: &'a [u8], _ctx: scroll::Endian) -> Result<(Self, usize), Self::Error> {
        let reexport = bytes.pread::<&str>(0)?;
        let reexport_len = reexport.len();
        debug!("reexport: {}", &reexport);
        for o in 0..reexport_len {
            let c: u8 = bytes.pread(o)?;
            debug!("reexport offset: {:#x} char: {:#x}", o, c);
            if c == b'.' {
                let dll: &'a str = bytes.pread_with(0, scroll::ctx::StrCtx::Length(o))?;
                debug!("dll: {:?}", &dll);
                if o + 1 == reexport_len {
                    break;
                }
                let len = reexport_len - o - 1;
                let rest: &'a [u8] = bytes.pread_with(o + 1, len)?;
                debug!("rest: {:?}", &rest);
                if rest[0] == b'#' {
                    let ordinal =
                        rest.pread_with::<&str>(1, scroll::ctx::StrCtx::Length(len - 1))?;
                    let ordinal = ordinal.parse::<u32>().map_err(|_e| {
                        error::Error::Malformed(format!(
                            "Cannot parse reexport ordinal from {} bytes",
                            bytes.len()
                        ))
                    })?;
                    return Ok((
                        Reexport::DLLOrdinal {
                            ordinal: ordinal as usize,
                            lib: dll,
                        },
                        reexport_len + 1,
                    ));
                } else {
                    let export = rest.pread_with::<&str>(0, scroll::ctx::StrCtx::Length(len))?;
                    return Ok((Reexport::DLLName { export, lib: dll }, reexport_len + 1));
                }
            }
        }
        Err(error::Error::Malformed(format!(
            "Reexport {:#} is malformed",
            reexport
        )))
    }
}

impl<'a> Reexport<'a> {
    pub fn parse(bytes: &'a [u8], offset: usize) -> crate::error::Result<Reexport<'a>> {
        bytes.pread(offset)
    }
}

#[derive(Debug, Default)]
/// An exported symbol in this binary, contains synthetic data (name offset, etc., are computed)
pub struct Export<'a> {
    pub name: Option<&'a str>,
    pub offset: Option<usize>,
    pub rva: usize,
    pub size: usize,
    pub reexport: Option<Reexport<'a>>,
}

#[derive(Debug, Copy, Clone)]
struct ExportCtx<'a> {
    pub ptr: u32,
    pub idx: usize,
    pub sections: &'a [section_table::SectionTable],
    pub file_alignment: u32,
    pub addresses: &'a ExportAddressTable,
    pub ordinals: &'a ExportOrdinalTable,
    pub opts: options::ParseOptions,
}

impl<'a, 'b> scroll::ctx::TryFromCtx<'a, ExportCtx<'b>> for Export<'a> {
    type Error = error::Error;
    #[inline]
    fn try_from_ctx(
        bytes: &'a [u8],
        ExportCtx {
            ptr,
            idx,
            sections,
            file_alignment,
            addresses,
            ordinals,
            opts,
        }: ExportCtx<'b>,
    ) -> Result<(Self, usize), Self::Error> {
        use self::ExportAddressTableEntry::*;

        let name = utils::find_offset(ptr as usize, sections, file_alignment, &opts)
            .and_then(|offset| bytes.pread::<&str>(offset).ok());

        if let Some(ordinal) = ordinals.get(idx) {
            if let Some(rva) = addresses.get(*ordinal as usize) {
                match *rva {
                    ExportRVA(rva) => {
                        let rva = rva as usize;
                        let offset = utils::find_offset(rva, sections, file_alignment, &opts);
                        Ok((
                            Export {
                                name,
                                offset,
                                rva,
                                reexport: None,
                                size: 0,
                            },
                            0,
                        ))
                    }

                    ForwarderRVA(rva) => {
                        let rva = rva as usize;
                        let offset = utils::find_offset_or(
                            rva,
                            sections,
                            file_alignment,
                            &opts,
                            &format!(
                                "cannot map RVA ({:#x}) of export ordinal {} into offset",
                                rva, ordinal
                            ),
                        )?;
                        let reexport = Reexport::parse(bytes, offset)?;
                        Ok((
                            Export {
                                name,
                                offset: Some(offset),
                                rva,
                                reexport: Some(reexport),
                                size: 0,
                            },
                            0,
                        ))
                    }
                }
            } else {
                Err(error::Error::Malformed(format!(
                    "cannot get RVA of export ordinal {}",
                    ordinal
                )))
            }
        } else {
            Err(error::Error::Malformed(format!(
                "cannot get ordinal of export name entry {}",
                idx
            )))
        }
    }
}

impl<'a> Export<'a> {
    pub fn parse(
        bytes: &'a [u8],
        export_data: &ExportData,
        sections: &[section_table::SectionTable],
        file_alignment: u32,
    ) -> error::Result<Vec<Export<'a>>> {
        Self::parse_with_opts(
            bytes,
            export_data,
            sections,
            file_alignment,
            &options::ParseOptions::default(),
        )
    }

    pub fn parse_with_opts(
        bytes: &'a [u8],
        export_data: &ExportData,
        sections: &[section_table::SectionTable],
        file_alignment: u32,
        opts: &options::ParseOptions,
    ) -> error::Result<Vec<Export<'a>>> {
        let pointers = &export_data.export_name_pointer_table;
        let addresses = &export_data.export_address_table;
        let ordinals = &export_data.export_ordinal_table;

        let mut exports = Vec::with_capacity(pointers.len());
        for (idx, &ptr) in pointers.iter().enumerate() {
            if let Ok(export) = bytes.pread_with(
                0,
                ExportCtx {
                    ptr,
                    idx,
                    sections,
                    file_alignment,
                    addresses,
                    ordinals,
                    opts: *opts,
                },
            ) {
                exports.push(export);
            }
        }

        Ok(exports)
    }
}
