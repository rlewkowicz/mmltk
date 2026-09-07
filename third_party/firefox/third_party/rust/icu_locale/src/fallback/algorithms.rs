// called LICENSE at the top level of the ICU4X source tree
// (online at: https://github.com/unicode-org/icu4x/blob/main/LICENSE ).

use super::LocaleFallbackPriority;
use icu_locale_core::subtags::{Language, Region, Script};

use super::*;

impl LocaleFallbackerWithConfig<'_> {
    pub(crate) fn normalize(&self, locale: &mut DataLocale, default_script: &mut Option<Script>) {
        if let Some(subdivision) = locale.subdivision.take() {
            if let Some(region) = locale.region {
                if subdivision
                    .as_str()
                    .starts_with(region.to_tinystr().to_ascii_lowercase().as_str())
                {
                    locale.subdivision = Some(subdivision);
                }
            }
        }
        let language = locale.language;
        if self.config.priority == LocaleFallbackPriority::Region && locale.region.is_none() {
            if let Some(script) = locale.script {
                locale.region = self
                    .likely_subtags
                    .language_script
                    .get(&(
                        language.to_tinystr().to_unvalidated(),
                        script.to_tinystr().to_unvalidated(),
                    ))
                    .copied();
            }
            if locale.region.is_none() {
                locale.region = self
                    .likely_subtags
                    .language
                    .get_copied(&language.to_tinystr().to_unvalidated())
                    .map(|(_s, r)| r);
            }
        }
        if locale.script.is_some() || self.config.priority == LocaleFallbackPriority::Script {
            *default_script = locale
                .region
                .and_then(|region| {
                    self.likely_subtags.language_region.get_copied(&(
                        language.to_tinystr().to_unvalidated(),
                        region.to_tinystr().to_unvalidated(),
                    ))
                })
                .or_else(|| {
                    self.likely_subtags
                        .language
                        .get_copied(&language.to_tinystr().to_unvalidated())
                        .map(|(s, _r)| s)
                });
            if locale.script == *default_script {
                locale.script = None;
            }
        }
    }
}

impl LocaleFallbackIteratorInner<'_> {
    pub fn step(&mut self, locale: &mut DataLocale) {
        match self.config.priority {
            LocaleFallbackPriority::Language => self.step_language(locale),
            LocaleFallbackPriority::Script => self.step_script(locale),
            LocaleFallbackPriority::Region => self.step_region(locale),
            _ => {
                debug_assert!(
                    false,
                    "Unknown LocaleFallbackPriority: {:?}",
                    self.config.priority
                );
                *locale = Default::default()
            }
        }
    }

    fn step_language(&mut self, locale: &mut DataLocale) {
        if let Some(value) = locale.subdivision.take() {
            self.backup_subdivision = Some(value);
            return;
        }
        if let Some(single_variant) = locale.variant.take() {
            self.backup_variant = Some(single_variant);
            return;
        }
        if let Some((language, script, region)) = self.get_explicit_parent(locale) {
            locale.language = language;
            locale.script = script;
            locale.region = region;
            locale.variant = self.backup_variant.take();
            return;
        }
        if let Some(region) = locale.region {
            if locale.script.is_none() {
                let language = locale.language;
                if let Some(script) = self.likely_subtags.language_region.get_copied(&(
                    language.to_tinystr().to_unvalidated(),
                    region.to_tinystr().to_unvalidated(),
                )) {
                    locale.script = Some(script);
                }
            }
            locale.region = None;
            locale.variant = self.backup_variant.take();
            return;
        }
        debug_assert!(!locale.language.is_unknown() || locale.script.is_some()); 
        locale.script = None;
        locale.language = Language::UNKNOWN;
    }

    fn step_region(&mut self, locale: &mut DataLocale) {
        if let Some(value) = locale.subdivision.take() {
            self.backup_subdivision = Some(value);
            return;
        }
        if let Some(variant) = locale.variant.take() {
            self.backup_variant = Some(variant);
            return;
        }
        if !locale.language.is_unknown() || locale.script.is_some() {
            locale.script = None;
            locale.language = Language::UNKNOWN;
            if locale.region.is_some() {
                locale.variant = self.backup_variant.take();
                locale.subdivision = self.backup_subdivision.take();
            }
            return;
        }
        debug_assert!(locale.region.is_some()); 
        locale.region = None;
    }

    fn step_script(&mut self, locale: &mut DataLocale) {
        if let Some(value) = locale.subdivision.take() {
            self.backup_subdivision = Some(value);
            return;
        }
        if let Some(variant) = locale.variant.take() {
            self.backup_variant = Some(variant);
            return;
        }
        if let Some((language, script, region)) = self.get_explicit_parent(locale) {
            locale.language = language;
            locale.script = script;
            locale.region = region;
            locale.variant = self.backup_variant.take();
            return;
        }
        if let Some(region) = locale.region {
            self.backup_region = Some(region);
            let language_implied_script = self
                .likely_subtags
                .language
                .get_copied(&locale.language.to_tinystr().to_unvalidated())
                .map(|(s, _r)| s);
            if language_implied_script != self.max_script {
                locale.script = self.max_script;
            }
            locale.region = None;
            locale.variant = self.backup_variant.take();
            return;
        }

        if !locale.language.is_unknown() {
            let language_implied_script = self
                .likely_subtags
                .language
                .get_copied(&locale.language.to_tinystr().to_unvalidated())
                .map(|(s, _r)| s);
            if locale.script.is_some() && language_implied_script == locale.script {
                locale.script = None;
                if let Some(region) = self.backup_region.take() {
                    locale.region = Some(region);
                    locale.subdivision = self.backup_subdivision.take();
                    locale.variant = self.backup_variant.take();
                }
                return;
            } else {
                locale.language = Language::UNKNOWN;
                locale.script = self.max_script;
                if locale.script.is_some() {
                    locale.variant = self.backup_variant.take();
                }
                return;
            }
        }


        if locale.script.is_some() {
            locale.script = None;
        }
    }

    fn get_explicit_parent(
        &self,
        locale: &DataLocale,
    ) -> Option<(Language, Option<Script>, Option<Region>)> {
        self.parents
            .parents
            .get_copied_by(|uvstr| locale.strict_cmp(uvstr).reverse())
    }
}
