use crate::core::{Font, Size};
use crate::text;

use rustc_hash::{FxHashMap, FxHashSet, FxHasher};
use std::collections::hash_map;
use std::hash::{Hash, Hasher};

#[derive(Debug, Default)]
pub struct Cache {
    entries: FxHashMap<KeyHash, Entry>,
    aliases: FxHashMap<KeyHash, KeyHash>,
    recently_used: FxHashSet<KeyHash>,
}

impl Cache {
        pub fn new() -> Self {
        Self::default()
    }

        pub fn get(&self, key: &KeyHash) -> Option<&Entry> {
        self.entries.get(key)
    }

        pub fn allocate(
        &mut self,
        font_system: &mut cosmic_text::FontSystem,
        key: Key<'_>,
    ) -> (KeyHash, &mut Entry) {
        let hash = key.hash(FxHasher::default());

        if let Some(hash) = self.aliases.get(&hash) {
            let _ = self.recently_used.insert(*hash);

            return (*hash, self.entries.get_mut(hash).unwrap());
        }

        if let hash_map::Entry::Vacant(entry) = self.entries.entry(hash) {
            let metrics =
                cosmic_text::Metrics::new(key.size, key.line_height.max(f32::MIN_POSITIVE));
            let mut buffer = cosmic_text::Buffer::new(font_system, metrics);

            let max_height = key.bounds.height.max(key.line_height);

            buffer.set_size(Some(key.bounds.width), Some(max_height));

            buffer.set_wrap(text::to_wrap(key.wrapping));
            buffer.set_ellipsize(text::to_ellipsize(key.ellipsis, max_height));

            buffer.set_text(
                key.content,
                &text::to_attributes(key.font),
                text::to_shaping(key.shaping, key.content),
                None,
            );
            buffer.shape_until_scroll(font_system, false);

            let bounds = text::align(&mut buffer, font_system, key.align_x);

            let _ = entry.insert(Entry {
                buffer,
                min_bounds: bounds,
            });

            for bounds in [
                bounds,
                Size {
                    width: key.bounds.width,
                    ..bounds
                },
            ] {
                if key.bounds != bounds {
                    let _ = self
                        .aliases
                        .insert(Key { bounds, ..key }.hash(FxHasher::default()), hash);
                }
            }
        }

        let _ = self.recently_used.insert(hash);

        (hash, self.entries.get_mut(&hash).unwrap())
    }

                pub fn trim(&mut self) {
        self.entries
            .retain(|key, _| self.recently_used.contains(key));

        self.aliases
            .retain(|_, value| self.recently_used.contains(value));

        self.recently_used.clear();
    }
}

#[derive(Debug, Clone, Copy)]
pub struct Key<'a> {
        pub content: &'a str,
        pub size: f32,
        pub line_height: f32,
        pub font: Font,
        pub bounds: Size,
        pub shaping: text::Shaping,
        pub align_x: text::Alignment,
        pub wrapping: text::Wrapping,
        pub ellipsis: text::Ellipsis,
}

impl Key<'_> {
    fn hash<H: Hasher>(self, mut hasher: H) -> KeyHash {
        self.content.hash(&mut hasher);
        self.size.to_bits().hash(&mut hasher);
        self.line_height.to_bits().hash(&mut hasher);
        self.font.hash(&mut hasher);
        self.bounds.width.to_bits().hash(&mut hasher);
        self.bounds.height.to_bits().hash(&mut hasher);
        self.shaping.hash(&mut hasher);
        self.align_x.hash(&mut hasher);

        hasher.finish()
    }
}

pub type KeyHash = u64;

#[derive(Debug)]
pub struct Entry {
        pub buffer: cosmic_text::Buffer,
        pub min_bounds: Size,
}
