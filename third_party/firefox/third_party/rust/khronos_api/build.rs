// Copyright 2015 Brendan Zabarauskas and the gl-rs developers
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//     http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

use std::env;
use std::fs::File;
use std::io::Write;
use std::path::*;

fn main() {
    let dest = env::var("OUT_DIR").unwrap();
    let mut file = File::create(&Path::new(&dest).join("webgl_exts.rs")).unwrap();

    let root = env::current_dir().unwrap().join("api_webgl/extensions");

    writeln!(file, "&[").unwrap();

    for entry in root.read_dir().unwrap() {
        let entry = entry.unwrap();
        let path = entry.path();
        let ext_name = path.file_name().unwrap().to_str().unwrap();

        if path.is_dir() && ext_name != "template" {
            let ext_path = path.join("extension.xml");
            if ext_path.is_file() {
                writeln!(file, "&*include_bytes!({:?}),", ext_path.to_str().unwrap()).unwrap();
            }
        }
    }

    writeln!(file, "]").unwrap();
}
