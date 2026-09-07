/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

use crate::derives::*;
use crate::values::AtomIdent;
use crate::Atom;

type Mapping<'a> = (&'a str, &'a str);

#[derive(Clone, Debug, MallocSizeOf)]
pub struct ShadowParts {
    mappings: Vec<(Atom, Atom)>,
}

/// <https://drafts.csswg.org/css-shadow-parts/#parsing-mapping>
///
/// Returns `None` in the failure case.
pub fn parse_part_mapping(input: &str) -> Option<Mapping<'_>> {

    let input = input.trim_start_matches(|c| c == ' ');

    let space_or_colon_position = input
        .char_indices()
        .find(|(_, c)| matches!(c, ' ' | ':'))
        .map(|(index, _)| index)
        .unwrap_or(input.len());
    let (first_token, input) = input.split_at(space_or_colon_position);

    if first_token.is_empty() {
        return None;
    }

    let input = input.trim_start_matches(|c| c == ' ');

    if input.is_empty() {
        return Some((first_token, first_token));
    }

    let Some(input) = input.strip_prefix(':') else {
        return None;
    };

    let input = input.trim_start_matches(|c| c == ' ');

    let space_or_colon_position = input
        .char_indices()
        .find(|(_, c)| matches!(c, ' ' | ':'))
        .map(|(index, _)| index)
        .unwrap_or(input.len());
    let (second_token, input) = input.split_at(space_or_colon_position);

    if second_token.is_empty() {
        return None;
    }

    let input = input.trim_start_matches(|c| c == ' ');

    if !input.is_empty() {
        return None;
    }

    Some((first_token, second_token))
}

/// <https://drafts.csswg.org/css-shadow-parts/#parsing-mapping-list>
fn parse_mapping_list(input: &str) -> impl Iterator<Item = Mapping<'_>> {
    let unparsed_mappings = input.split(',');


    unparsed_mappings.filter_map(|unparsed_mapping| {
        if unparsed_mapping.chars().all(|c| c == ' ') {
            return None;
        }

        parse_part_mapping(unparsed_mapping)
    })
}

impl ShadowParts {
    pub fn parse(input: &str) -> Self {
        Self {
            mappings: parse_mapping_list(input)
                .map(|(first, second)| (first.into(), second.into()))
                .collect(),
        }
    }

    /// Call the provided callback for each exported part with the given name.
    pub fn for_each_exported_part<F>(&self, name: &Atom, mut callback: F)
    where
        F: FnMut(&AtomIdent),
    {
        for (from, to) in &self.mappings {
            if from == name {
                callback(AtomIdent::cast(to));
            }
        }
    }

    pub fn imported_part(&self, name: &Atom) -> Option<&Atom> {
        self.mappings
            .iter()
            .find(|(_, to)| to == name)
            .map(|(from, _)| from)
    }
}
