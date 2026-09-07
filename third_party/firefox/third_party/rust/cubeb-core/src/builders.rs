// Copyright © 2017-2018 Mozilla Foundation
// This program is made available under an ISC-style license.  See the
// accompanying file LICENSE for details.

use ffi;
use {ChannelLayout, InputProcessingParams, SampleFormat, StreamParams, StreamPrefs};

#[derive(Debug)]
pub struct StreamParamsBuilder(ffi::cubeb_stream_params);

impl Default for StreamParamsBuilder {
    fn default() -> Self {
        StreamParamsBuilder(ffi::cubeb_stream_params {
            format: ffi::CUBEB_SAMPLE_S16NE,
            ..Default::default()
        })
    }
}

impl StreamParamsBuilder {
    pub fn new() -> Self {
        Default::default()
    }

    pub fn format(mut self, format: SampleFormat) -> Self {
        self.0.format = format.into();
        self
    }

    pub fn rate(mut self, rate: u32) -> Self {
        self.0.rate = rate;
        self
    }

    pub fn channels(mut self, channels: u32) -> Self {
        self.0.channels = channels;
        self
    }

    pub fn layout(mut self, layout: ChannelLayout) -> Self {
        self.0.layout = layout.into();
        self
    }

    pub fn prefs(mut self, prefs: StreamPrefs) -> Self {
        self.0.prefs = prefs.bits();
        self
    }

    pub fn input_params(mut self, input_params: InputProcessingParams) -> Self {
        self.0.input_params = input_params.bits();
        self
    }

    pub fn take(&self) -> StreamParams {
        StreamParams::from(self.0)
    }
}
