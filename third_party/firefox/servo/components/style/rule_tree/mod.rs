/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#![deny(unsafe_code)]

//! The rule tree.

use crate::applicable_declarations::{ApplicableDeclarationList, CascadePriority};
use crate::properties::{LonghandIdSet, PropertyDeclarationBlock};
use crate::shared_lock::{Locked, StylesheetGuards};
use crate::stylesheets::layer_rule::LayerOrder;
use servo_arc::ArcBorrow;
use smallvec::SmallVec;
use std::io::{self, Write};

mod core;
pub mod level;
mod map;
mod source;
mod unsafe_box;

pub use self::core::{RuleTree, StrongRuleNode};
pub use self::level::{CascadeLevel, CascadeOrigin, ShadowCascadeOrder};
pub use self::source::StyleSource;

bitflags! {
    /// Flags that are part of the cascade priority, and that we use to track
    /// information about where the rule came from.
    #[derive(Clone, Copy, Debug, Default, Eq, Hash, PartialEq)]
    pub struct RuleCascadeFlags: u8 {
        /// Whether the rule is inside a @starting-style block.
        const STARTING_STYLE = 1 << 0;
        /// Whether the rule is inside an @appearance-base block.
        const APPEARANCE_BASE = 1 << 1;
    }
}

malloc_size_of::malloc_size_of_is_0!(RuleCascadeFlags);

impl RuleTree {
    fn dump<W: Write>(&self, guards: &StylesheetGuards, writer: &mut W) {
        let _ = writeln!(writer, " + RuleTree");
        self.root().dump(guards, writer, 0);
    }

    /// Dump the rule tree to stdout.
    pub fn dump_stdout(&self, guards: &StylesheetGuards) {
        let mut stdout = io::stdout();
        self.dump(guards, &mut stdout);
    }

    /// Inserts the given rules, that must be in proper order by specifity, and
    /// returns the corresponding rule node representing the last inserted one.
    ///
    /// !important rules are detected and inserted into the appropriate position
    /// in the rule tree. This allows selector matching to ignore importance,
    /// while still maintaining the appropriate cascade order in the rule tree.
    pub fn insert_ordered_rules_with_important<'a, I>(
        &self,
        iter: I,
        guards: &StylesheetGuards,
    ) -> StrongRuleNode
    where
        I: Iterator<Item = (StyleSource, CascadePriority)>,
    {
        let mut current = self.root().clone();

        let mut found_important = false;

        let mut important_author = SmallVec::<[(StyleSource, CascadePriority); 4]>::new();
        let mut important_user = SmallVec::<[(StyleSource, CascadePriority); 4]>::new();
        let mut important_ua = SmallVec::<[(StyleSource, CascadePriority); 4]>::new();
        let mut transition = None;

        for (source, priority) in iter {
            let level = priority.cascade_level();
            debug_assert!(!level.is_important(), "Important levels handled internally");

            let any_important = {
                let pdb = source.read(level.guard(guards));
                pdb.any_important()
            };

            if any_important {
                found_important = true;
                match level.origin() {
                    CascadeOrigin::Author => {
                        important_author.push((source.clone(), priority.important()))
                    },
                    CascadeOrigin::UA => important_ua.push((source.clone(), priority.important())),
                    CascadeOrigin::User => {
                        important_user.push((source.clone(), priority.important()))
                    },
                    _ => {},
                };
            }

            if level.origin() == CascadeOrigin::Transitions && found_important {
                debug_assert!(transition.is_none());
                transition = Some(source);
            } else {
                current = current.ensure_child(self.root(), source, priority);
            }
        }

        if !found_important {
            return current;
        }

        if !important_author.is_empty()
            && important_author.first().unwrap().1 != important_author.last().unwrap().1
        {
            important_author.sort_by_key(|&(_, priority)| priority);
        }

        for (source, priority) in important_author.drain(..) {
            current = current.ensure_child(self.root(), source, priority);
        }

        for (source, priority) in important_user.drain(..) {
            current = current.ensure_child(self.root(), source, priority);
        }

        for (source, priority) in important_ua.drain(..) {
            current = current.ensure_child(self.root(), source, priority);
        }

        if let Some(source) = transition {
            current = current.ensure_child(
                self.root(),
                source,
                CascadePriority::new(
                    CascadeLevel::new(CascadeOrigin::Transitions),
                    LayerOrder::root(),
                    RuleCascadeFlags::empty(),
                ),
            );
        }

        current
    }

    /// Given a list of applicable declarations, insert the rules and return the
    /// corresponding rule node.
    pub fn compute_rule_node(
        &self,
        applicable_declarations: &mut ApplicableDeclarationList,
        guards: &StylesheetGuards,
    ) -> StrongRuleNode {
        self.insert_ordered_rules_with_important(
            applicable_declarations.drain(..).map(|d| d.for_rule_tree()),
            guards,
        )
    }

    /// Insert the given rules, that must be in proper order by specifity, and
    /// return the corresponding rule node representing the last inserted one.
    pub fn insert_ordered_rules<'a, I>(&self, iter: I) -> StrongRuleNode
    where
        I: Iterator<Item = (StyleSource, CascadePriority)>,
    {
        self.insert_ordered_rules_from(self.root().clone(), iter)
    }

    fn insert_ordered_rules_from<'a, I>(&self, from: StrongRuleNode, iter: I) -> StrongRuleNode
    where
        I: Iterator<Item = (StyleSource, CascadePriority)>,
    {
        let mut current = from;
        for (source, priority) in iter {
            current = current.ensure_child(self.root(), source, priority);
        }
        current
    }

    /// Replaces a rule in a given level (if present) for another rule.
    ///
    /// Returns the resulting node that represents the new path, or None if
    /// the old path is still valid.
    pub fn update_rule_at_level(
        &self,
        level: CascadeLevel,
        layer_order: LayerOrder,
        pdb: Option<ArcBorrow<Locked<PropertyDeclarationBlock>>>,
        path: &StrongRuleNode,
        guards: &StylesheetGuards,
        important_rules_changed: &mut bool,
    ) -> Option<StrongRuleNode> {
        let mut current = path.clone();
        *important_rules_changed = false;

        let mut children = SmallVec::<[_; 10]>::new();
        while current.cascade_priority().cascade_level() > level {
            children.push((
                current.style_source().unwrap().clone(),
                current.cascade_priority(),
            ));
            current = current.parent().unwrap().clone();
        }

        let cascade_priority = CascadePriority::new(level, layer_order, RuleCascadeFlags::empty());

        if current.cascade_priority() == cascade_priority {
            *important_rules_changed |= level.is_important();

            let current_decls = current.style_source().unwrap().get();

            if let Some(ref pdb) = pdb {
                let is_here_already = ArcBorrow::ptr_eq(pdb, &current_decls.borrow_arc());
                if is_here_already {
                    debug!("Picking the fast path in rule replacement");
                    return None;
                }
            }

            current = current.parent().unwrap().clone();
        }

        if let Some(pdb) = pdb {
            if level.is_important() {
                if pdb.read_with(level.guard(guards)).any_important() {
                    current = current.ensure_child(
                        self.root(),
                        StyleSource::from_declarations(pdb.clone_arc()),
                        cascade_priority,
                    );
                    *important_rules_changed = true;
                }
            } else {
                if pdb.read_with(level.guard(guards)).any_normal() {
                    current = current.ensure_child(
                        self.root(),
                        StyleSource::from_declarations(pdb.clone_arc()),
                        cascade_priority,
                    );
                }
            }
        }

        let rule = self.insert_ordered_rules_from(current, children.drain(..).rev());
        Some(rule)
    }

    /// Returns whether this rule node has any @starting-style rule.
    pub fn has_starting_style(path: &StrongRuleNode) -> bool {
        path.self_and_ancestors().any(|node| {
            node.cascade_priority()
                .flags()
                .intersects(RuleCascadeFlags::STARTING_STYLE)
        })
    }

    /// Returns new rule nodes without Transitions level rule.
    pub fn remove_transition_rule_if_applicable(path: &StrongRuleNode) -> StrongRuleNode {
        if path.cascade_level().origin() != CascadeOrigin::Transitions {
            return path.clone();
        }

        path.parent().unwrap().clone()
    }

    /// Returns new rule node without rules from declarative animations.
    pub fn remove_animation_rules(&self, path: &StrongRuleNode) -> StrongRuleNode {
        if !path.has_animation_or_transition_rules() {
            return path.clone();
        }

        let iter = path.self_and_ancestors().take_while(|node| {
            node.cascade_level() >= CascadeLevel::new(CascadeOrigin::SMILOverride)
        });
        let mut last = path;
        let mut children = SmallVec::<[_; 10]>::new();
        for node in iter {
            if !node.cascade_level().is_animation() {
                children.push((
                    node.style_source().unwrap().clone(),
                    node.cascade_priority(),
                ));
            }
            last = node;
        }

        let rule = self
            .insert_ordered_rules_from(last.parent().unwrap().clone(), children.drain(..).rev());
        rule
    }
}

impl StrongRuleNode {
    /// Get an iterator for this rule node and its ancestors.
    pub fn self_and_ancestors(&self) -> SelfAndAncestors<'_> {
        SelfAndAncestors {
            current: Some(self),
        }
    }

    /// Returns true if there is either animation or transition level rule.
    pub fn has_animation_or_transition_rules(&self) -> bool {
        self.self_and_ancestors()
            .take_while(|node| {
                node.cascade_level() >= CascadeLevel::new(CascadeOrigin::SMILOverride)
            })
            .any(|node| node.cascade_level().is_animation())
    }

    /// Get a set of properties whose CascadeLevel are higher than Animations
    /// but not equal to Transitions.
    ///
    /// If there are any custom properties, we set the boolean value of the
    /// returned tuple to true.
    pub fn get_properties_overriding_animations(
        &self,
        guards: &StylesheetGuards,
    ) -> (LonghandIdSet, bool) {
        use crate::properties::PropertyDeclarationId;

        let iter = self
            .self_and_ancestors()
            .skip_while(|node| node.cascade_level().origin() == CascadeOrigin::Transitions)
            .take_while(|node| node.cascade_level() > CascadeLevel::new(CascadeOrigin::Animations));
        let mut result = (LonghandIdSet::new(), false);
        for node in iter {
            let style = node.style_source().unwrap();
            for (decl, important) in style
                .read(node.cascade_level().guard(guards))
                .declaration_importance_iter()
            {
                if important.important() {
                    match decl.id() {
                        PropertyDeclarationId::Longhand(id) => result.0.insert(id),
                        PropertyDeclarationId::Custom(_) => result.1 = true,
                    }
                }
            }
        }
        result
    }
}

/// An iterator over a rule node and its ancestors.
#[derive(Clone)]
pub struct SelfAndAncestors<'a> {
    current: Option<&'a StrongRuleNode>,
}

impl<'a> Iterator for SelfAndAncestors<'a> {
    type Item = &'a StrongRuleNode;

    fn next(&mut self) -> Option<Self::Item> {
        self.current.map(|node| {
            self.current = node.parent();
            node
        })
    }
}
