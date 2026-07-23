// Copyright (c) the JPEG XL Project Authors. All rights reserved.
// license that can be found in the LICENSE file.

use crate::util::f16;

use crate::{
    api::{Endianness, JxlDataFormat, JxlOutputBuffer},
    error::Result,
    image::Image,
    render::save::SaveStage,
};

impl SaveStage {
    pub(super) fn save_simple(
        &self,
        data: &[Image<f64>],
        buffers: &mut [Option<JxlOutputBuffer>],
    ) -> Result<()> {
        for i in self.channels.iter().skip(1) {
            assert_eq!(data[self.channels[0]].size(), data[*i].size());
        }
        let Some(buf) = buffers[self.output_buffer_index].as_mut() else {
            return Ok(());
        };
        let size = data[0].size();

        self.check_buffer_size(size, Some(buf))?;

        let output_channels = self.output_channels();

        for (c, &chan) in self.channels.iter().enumerate() {
            for y in 0..size.1 {
                let src_row = data[chan].row(y);

                for (x, &px) in src_row.iter().enumerate() {
                    let (dx, dy) = self.orientation.display_pixel((x, y), size);
                    let dx = dx * output_channels + c;
                    let bps = self.data_format.bytes_per_sample();

                    macro_rules! write_pixel {
                        ($px: expr, $endianness: expr) => {
                            let px = $px;
                            let px_bytes = if $endianness == Endianness::LittleEndian {
                                px.to_le_bytes()
                            } else {
                                px.to_be_bytes()
                            };
                            buf.write_bytes(dy, dx * bps, &px_bytes);
                        };
                    }

                    match self.data_format {
                        JxlDataFormat::U8 { .. } => {
                            write_pixel!(px as u8, Endianness::LittleEndian);
                        }
                        JxlDataFormat::U16 { endianness, .. } => {
                            write_pixel!(px as u16, endianness);
                        }
                        JxlDataFormat::F32 { endianness } => {
                            write_pixel!(px as f32, endianness);
                        }
                        JxlDataFormat::F16 { endianness } => {
                            write_pixel!(f16::from_f64(px), endianness);
                        }
                    }
                }
            }
        }

        if self.fill_opaque_alpha {
            let alpha_channel = self.channels.len(); 
            let opaque_bytes = self.data_format.opaque_alpha_bytes();
            for y in 0..size.1 {
                for x in 0..size.0 {
                    let (dx, dy) = self.orientation.display_pixel((x, y), size);
                    let dx = dx * output_channels + alpha_channel;
                    let bps = self.data_format.bytes_per_sample();
                    buf.write_bytes(dy, dx * bps, &opaque_bytes);
                }
            }
        }

        Ok(())
    }
}
