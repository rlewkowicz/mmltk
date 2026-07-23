// Licensed under the Apache License, Version 2.0 <LICENSE-APACHE or
// http://www.apache.org/licenses/LICENSE-2.0> or the MIT license
// <LICENSE-MIT or http://opensource.org/licenses/MIT>, at your
// option. This file may not be copied, modified, or distributed
// except according to those terms.

use std::{
    fmt::{self, Display, Formatter},
    mem,
    ops::{Deref, Div as _},
};

use neqo_common::{Header, qtrace};

use crate::{
    Error, Res,
    prefix::{
        BASE_PREFIX_NEGATIVE, BASE_PREFIX_POSITIVE, HEADER_FIELD_INDEX_DYNAMIC,
        HEADER_FIELD_INDEX_DYNAMIC_POST, HEADER_FIELD_INDEX_STATIC,
        HEADER_FIELD_LITERAL_NAME_LITERAL, HEADER_FIELD_LITERAL_NAME_REF_DYNAMIC,
        HEADER_FIELD_LITERAL_NAME_REF_DYNAMIC_POST, HEADER_FIELD_LITERAL_NAME_REF_STATIC,
        NO_PREFIX,
    },
    qpack_send_buf::Encoder as _,
    reader::{LiteralReader, ReceiverBufferWrapper, parse_utf8},
    table::{ADDITIONAL_TABLE_ENTRY_SIZE, HeaderTable},
};

#[derive(Default, Debug, PartialEq, Eq)]
pub struct HeaderEncoder {
    buf: neqo_common::Encoder,
    base: u64,
    use_huffman: bool,
    max_entries: u64,
    max_dynamic_index_ref: Option<u64>,
}

impl Display for HeaderEncoder {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        write!(f, "HeaderEncoder")
    }
}

impl HeaderEncoder {
    pub fn new(base: u64, use_huffman: bool, max_entries: u64) -> Self {
        Self {
            buf: neqo_common::Encoder::default(),
            base,
            use_huffman,
            max_entries,
            max_dynamic_index_ref: None,
        }
    }

    pub fn encode_indexed_static(&mut self, index: u64) {
        qtrace!("[{self}] encode static index {index}");
        self.buf
            .encode_prefixed_encoded_int(HEADER_FIELD_INDEX_STATIC, index);
    }

    const fn new_ref(&mut self, index: u64) {
        if let Some(r) = self.max_dynamic_index_ref {
            if r < index {
                self.max_dynamic_index_ref = Some(index);
            }
        } else {
            self.max_dynamic_index_ref = Some(index);
        }
    }

    pub fn encode_indexed_dynamic(&mut self, index: u64) {
        qtrace!("[{self}] encode dynamic index {index}");
        if index < self.base {
            self.buf
                .encode_prefixed_encoded_int(HEADER_FIELD_INDEX_DYNAMIC, self.base - index - 1);
        } else {
            self.buf
                .encode_prefixed_encoded_int(HEADER_FIELD_INDEX_DYNAMIC_POST, index - self.base);
        }
        self.new_ref(index);
    }

    pub fn encode_literal_with_name_ref(&mut self, is_static: bool, index: u64, value: &[u8]) {
        qtrace!(
            "[{self}] encode literal with name ref - index={index}, static={is_static}, value={value:x?}"
        );
        if is_static {
            self.buf
                .encode_prefixed_encoded_int(HEADER_FIELD_LITERAL_NAME_REF_STATIC, index);
        } else if index < self.base {
            self.buf.encode_prefixed_encoded_int(
                HEADER_FIELD_LITERAL_NAME_REF_DYNAMIC,
                self.base - index - 1,
            );
            self.new_ref(index);
        } else {
            self.buf.encode_prefixed_encoded_int(
                HEADER_FIELD_LITERAL_NAME_REF_DYNAMIC_POST,
                index - self.base,
            );
            self.new_ref(index);
        }

        self.buf.encode_literal(self.use_huffman, NO_PREFIX, value);
    }

    pub fn encode_literal_with_name_literal(&mut self, name: &[u8], value: &[u8]) {
        qtrace!("[{self}] encode literal with name literal - name={name:x?}, value={value:x?}");
        self.buf
            .encode_literal(self.use_huffman, HEADER_FIELD_LITERAL_NAME_LITERAL, name);
        self.buf.encode_literal(self.use_huffman, NO_PREFIX, value);
    }

    pub fn encode_header_block_prefix(&mut self) {
        let tmp = mem::take(&mut self.buf);
        let (enc_insert_cnt, delta, prefix) =
            self.max_dynamic_index_ref
                .map_or((0, self.base, BASE_PREFIX_POSITIVE), |r| {
                    let req_insert_cnt = r + 1;
                    if req_insert_cnt <= self.base {
                        (
                            req_insert_cnt % (2 * self.max_entries) + 1,
                            self.base - req_insert_cnt,
                            BASE_PREFIX_POSITIVE,
                        )
                    } else {
                        (
                            req_insert_cnt % (2 * self.max_entries) + 1,
                            req_insert_cnt - self.base - 1,
                            BASE_PREFIX_NEGATIVE,
                        )
                    }
                });
        qtrace!(
            "[{self}] encode header block prefix max_dynamic_index_ref={:?}, base={}, enc_insert_cnt={enc_insert_cnt}, delta={delta}, prefix={prefix:?}",
            self.max_dynamic_index_ref,
            self.base
        );

        self.buf
            .encode_prefixed_encoded_int(NO_PREFIX, enc_insert_cnt);
        self.buf.encode_prefixed_encoded_int(prefix, delta);

        self.buf.encode(tmp);
    }
}

impl Deref for HeaderEncoder {
    type Target = [u8];
    fn deref(&self) -> &Self::Target {
        self.buf.as_ref()
    }
}

pub struct HeaderDecoder<'a> {
    buf: ReceiverBufferWrapper<'a>,
    base: u64,
    req_insert_cnt: u64,
}

impl Display for HeaderDecoder<'_> {
    fn fmt(&self, f: &mut Formatter) -> fmt::Result {
        write!(f, "HeaderDecoder")
    }
}

#[derive(Debug, PartialEq, Eq)]
pub enum HeaderDecoderResult {
    Blocked(u64),
    Headers(Vec<Header>),
}

impl<'a> HeaderDecoder<'a> {
    pub const fn new(buf: &'a [u8]) -> Self {
        Self {
            buf: ReceiverBufferWrapper::new(buf),
            base: 0,
            req_insert_cnt: 0,
        }
    }

    pub fn refers_dynamic_table(
        &mut self,
        max_entries: u64,
        total_num_of_inserts: u64,
    ) -> Res<bool> {
        Error::map_error(
            self.read_base(max_entries, total_num_of_inserts),
            Error::Decompression,
        )?;
        Ok(self.req_insert_cnt != 0)
    }

    pub fn decode_header_block(
        &mut self,
        table: &HeaderTable,
        max_entries: u64,
        total_num_of_inserts: u64,
    ) -> Res<HeaderDecoderResult> {
        Error::map_error(
            self.read_base(max_entries, total_num_of_inserts),
            Error::Decompression,
        )?;

        if table.base() < self.req_insert_cnt {
            qtrace!(
                "[{self}] decoding is blocked, requested inserts count={}",
                self.req_insert_cnt
            );
            return Ok(HeaderDecoderResult::Blocked(self.req_insert_cnt));
        }
        let mut h: Vec<Header> = Vec::new();
        let mut remaining = LiteralReader::MAX_LEN;

        while !self.buf.done() {
            let b = Error::map_error(self.buf.peek(), Error::Decompression)?;
            let header = if HEADER_FIELD_INDEX_STATIC.cmp_prefix(b) {
                Error::map_error(self.read_indexed_static(), Error::Decompression)?
            } else if HEADER_FIELD_INDEX_DYNAMIC.cmp_prefix(b) {
                Error::map_error(self.read_indexed_dynamic(table), Error::Decompression)?
            } else if HEADER_FIELD_INDEX_DYNAMIC_POST.cmp_prefix(b) {
                Error::map_error(self.read_indexed_dynamic_post(table), Error::Decompression)?
            } else if HEADER_FIELD_LITERAL_NAME_REF_STATIC.cmp_prefix(b) {
                Error::map_error(
                    self.read_literal_with_name_ref_static(),
                    Error::Decompression,
                )?
            } else if HEADER_FIELD_LITERAL_NAME_REF_DYNAMIC.cmp_prefix(b) {
                Error::map_error(
                    self.read_literal_with_name_ref_dynamic(table),
                    Error::Decompression,
                )?
            } else if HEADER_FIELD_LITERAL_NAME_LITERAL.cmp_prefix(b) {
                Error::map_error(self.read_literal_with_name_literal(), Error::Decompression)?
            } else if HEADER_FIELD_LITERAL_NAME_REF_DYNAMIC_POST.cmp_prefix(b) {
                Error::map_error(
                    self.read_literal_with_name_ref_dynamic_post(table),
                    Error::Decompression,
                )?
            } else {
                unreachable!("All prefixes are covered");
            };
            remaining = remaining
                .checked_sub(
                    header.name().len() + header.value().len() + ADDITIONAL_TABLE_ENTRY_SIZE,
                )
                .ok_or(Error::Decompression)?;
            h.push(header);
        }

        qtrace!("[{self}] done decoding header block");
        Ok(HeaderDecoderResult::Headers(h))
    }

    pub const fn get_req_insert_cnt(&self) -> u64 {
        self.req_insert_cnt
    }

    fn read_base(&mut self, max_entries: u64, total_num_of_inserts: u64) -> Res<()> {
        let insert_cnt = self.buf.read_prefixed_int(0)?;
        self.req_insert_cnt =
            HeaderDecoder::calc_req_insert_cnt(insert_cnt, max_entries, total_num_of_inserts)?;

        let s = self.buf.peek()? & 0x80 != 0;
        let base_delta = self.buf.read_prefixed_int(1)?;
        self.base = if s {
            if self.req_insert_cnt <= base_delta {
                return Err(Error::Decompression);
            }
            self.req_insert_cnt - base_delta - 1
        } else {
            self.req_insert_cnt
                .checked_add(base_delta)
                .ok_or(Error::Decompression)?
        };
        qtrace!(
            "[{self}] requested inserts count is {} and base is {}",
            self.req_insert_cnt,
            self.base
        );
        Ok(())
    }

    fn calc_req_insert_cnt(encoded: u64, max_entries: u64, total_num_of_inserts: u64) -> Res<u64> {
        if encoded == 0 {
            Ok(0)
        } else if max_entries == 0 {
            Err(Error::Decompression)
        } else {
            let full_range = 2 * max_entries;
            if encoded > full_range {
                return Err(Error::Decompression);
            }
            let max_value = total_num_of_inserts + max_entries;
            let max_wrapped = max_value.div(full_range) * full_range;
            let mut req_insert_cnt = max_wrapped + encoded - 1;
            if req_insert_cnt > max_value {
                if req_insert_cnt < full_range {
                    return Err(Error::Decompression);
                }
                req_insert_cnt -= full_range;
            }
            Ok(req_insert_cnt)
        }
    }

    fn read_indexed_static(&mut self) -> Res<Header> {
        let index = self
            .buf
            .read_prefixed_int(HEADER_FIELD_INDEX_STATIC.len())?;
        qtrace!("[{self}] decoder static indexed {index}");
        let entry = HeaderTable::get_static(index)?;
        Ok(Header::new(parse_utf8(entry.name())?, entry.value()))
    }

    fn read_indexed_dynamic(&mut self, table: &HeaderTable) -> Res<Header> {
        let index = self
            .buf
            .read_prefixed_int(HEADER_FIELD_INDEX_DYNAMIC.len())?;
        qtrace!("[{self}] decoder dynamic indexed {index}");
        let entry = table.get_dynamic(index, self.base, false)?;
        Ok(Header::new(parse_utf8(entry.name())?, entry.value()))
    }

    fn read_indexed_dynamic_post(&mut self, table: &HeaderTable) -> Res<Header> {
        let index = self
            .buf
            .read_prefixed_int(HEADER_FIELD_INDEX_DYNAMIC_POST.len())?;
        qtrace!("[{self}] decode post-based {index}");
        let entry = table.get_dynamic(index, self.base, true)?;
        Ok(Header::new(parse_utf8(entry.name())?, entry.value()))
    }

    fn read_literal_with_name_ref_static(&mut self) -> Res<Header> {
        qtrace!("[{self}] read literal with name reference to the static table");

        let index = self
            .buf
            .read_prefixed_int(HEADER_FIELD_LITERAL_NAME_REF_STATIC.len())?;

        Ok(Header::new(
            parse_utf8(HeaderTable::get_static(index)?.name())?,
            self.buf.read_literal_from_buffer(0)?,
        ))
    }

    fn read_literal_with_name_ref_dynamic(&mut self, table: &HeaderTable) -> Res<Header> {
        qtrace!("[{self}] read literal with name reference of the dynamic table");

        let index = self
            .buf
            .read_prefixed_int(HEADER_FIELD_LITERAL_NAME_REF_DYNAMIC.len())?;

        Ok(Header::new(
            parse_utf8(table.get_dynamic(index, self.base, false)?.name())?,
            self.buf.read_literal_from_buffer(0)?,
        ))
    }

    fn read_literal_with_name_ref_dynamic_post(&mut self, table: &HeaderTable) -> Res<Header> {
        qtrace!("[{self}] decoder literal with post-based index");

        let index = self
            .buf
            .read_prefixed_int(HEADER_FIELD_LITERAL_NAME_REF_DYNAMIC_POST.len())?;

        Ok(Header::new(
            parse_utf8(table.get_dynamic(index, self.base, true)?.name())?,
            self.buf.read_literal_from_buffer(0)?,
        ))
    }

    fn read_literal_with_name_literal(&mut self) -> Res<Header> {
        qtrace!("[{self}] decode literal with name literal");

        let name_bytes = self
            .buf
            .read_literal_from_buffer(HEADER_FIELD_LITERAL_NAME_LITERAL.len())?;

        let name = parse_utf8(&name_bytes)?.to_string();

        Ok(Header::new(name, self.buf.read_literal_from_buffer(0)?))
    }
}
