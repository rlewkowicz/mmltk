/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#![allow(unsafe_code)]

use crate::applicable_declarations::CascadePriority;
use crate::shared_lock::StylesheetGuards;
use crate::stylesheets::layer_rule::LayerOrder;
use malloc_size_of::{MallocShallowSizeOf, MallocSizeOf, MallocSizeOfOps};
use parking_lot::RwLock;
use smallvec::SmallVec;
use std::fmt;
use std::hash;
use std::io::Write;
use std::mem;
use std::ptr;
use std::sync::atomic::{self, AtomicPtr, AtomicUsize, Ordering};

use super::map::{Entry, Map};
use super::unsafe_box::UnsafeBox;
use super::{CascadeLevel, CascadeOrigin, RuleCascadeFlags, StyleSource};

/// The rule tree, the structure servo uses to preserve the results of selector
/// matching.
///
/// This is organized as a tree of rules. When a node matches a set of rules,
/// they're inserted in order in the tree, starting with the less specific one.
///
/// When a rule is inserted in the tree, other elements may share the path up to
/// a given rule. If that's the case, we don't duplicate child nodes, but share
/// them.
///
/// When the rule node refcount drops to zero, it doesn't get freed. It gets
/// instead put into a free list, and it is potentially GC'd after a while.
///
/// That way, a rule node that represents a likely-to-match-again rule (like a
/// :hover rule) can be reused if we haven't GC'd it yet.
#[derive(Debug)]
pub struct RuleTree {
    root: StrongRuleNode,
}

impl Drop for RuleTree {
    fn drop(&mut self) {
        unsafe { self.swap_free_list_and_gc(ptr::null_mut()) }
    }
}

impl MallocSizeOf for RuleTree {
    fn size_of(&self, ops: &mut MallocSizeOfOps) -> usize {
        let mut n = 0;
        let mut stack = SmallVec::<[_; 32]>::new();
        stack.push(self.root.clone());

        while let Some(node) = stack.pop() {
            n += unsafe { ops.malloc_size_of(&*node.p) };
            let children = node.p.children.read();
            children.shallow_size_of(ops);
            for c in &*children {
                stack.push(unsafe { c.upgrade() });
            }
        }

        n
    }
}

#[derive(Clone, Copy, Debug, Eq, Hash, PartialEq)]
struct ChildKey(CascadePriority, ptr::NonNull<()>);
unsafe impl Send for ChildKey {}
unsafe impl Sync for ChildKey {}

impl RuleTree {
    /// Construct a new rule tree.
    pub fn new() -> Self {
        RuleTree {
            root: StrongRuleNode::new(Box::new(RuleNode::root())),
        }
    }

    /// Get the root rule node.
    pub fn root(&self) -> &StrongRuleNode {
        &self.root
    }

    /// This can only be called when no other threads is accessing this tree.
    pub fn gc(&self) {
        unsafe { self.swap_free_list_and_gc(RuleNode::DANGLING_PTR) }
    }

    /// This can only be called when no other threads is accessing this tree.
    pub fn maybe_gc(&self) {
        #[cfg(debug_assertions)]
        self.maybe_dump_stats();

        if self.root.p.approximate_free_count.load(Ordering::Relaxed) > RULE_TREE_GC_INTERVAL {
            self.gc();
        }
    }

    #[cfg(debug_assertions)]
    fn maybe_dump_stats(&self) {
        use itertools::Itertools;
        use std::cell::Cell;
        use std::time::{Duration, Instant};

        if !log_enabled!(log::Level::Trace) {
            return;
        }

        const RULE_TREE_STATS_INTERVAL: Duration = Duration::from_secs(2);

        thread_local! {
            pub static LAST_STATS: Cell<Instant> = Cell::new(Instant::now());
        };

        let should_dump = LAST_STATS.with(|s| {
            let now = Instant::now();
            if now.duration_since(s.get()) < RULE_TREE_STATS_INTERVAL {
                return false;
            }
            s.set(now);
            true
        });

        if !should_dump {
            return;
        }

        let mut children_count = rustc_hash::FxHashMap::default();

        let mut stack = SmallVec::<[_; 32]>::new();
        stack.push(self.root.clone());
        while let Some(node) = stack.pop() {
            let children = node.p.children.read();
            *children_count.entry(children.len()).or_insert(0) += 1;
            for c in &*children {
                stack.push(unsafe { c.upgrade() });
            }
        }

        trace!("Rule tree stats:");
        let counts = children_count.keys().sorted();
        for count in counts {
            trace!(" {} - {}", count, children_count[count]);
        }
    }

    /// Steals the free list and drops its contents.
    unsafe fn swap_free_list_and_gc(&self, ptr: *mut RuleNode) {
        let root = &self.root.p;

        debug_assert!(!root.next_free.load(Ordering::Relaxed).is_null());

        root.approximate_free_count.store(0, Ordering::Relaxed);

        let mut head = root.next_free.swap(ptr, Ordering::Acquire);

        while head != RuleNode::DANGLING_PTR {
            debug_assert!(!head.is_null());

            let mut node = UnsafeBox::from_raw(head);

            debug_assert!(node.root.is_some());

            debug_assert!(node.refcount.load(Ordering::Relaxed) > 0);

            head = node.next_free.swap(ptr::null_mut(), Ordering::Relaxed);

            if node.refcount.fetch_sub(1, Ordering::Release) == 1 {
                RuleNode::pretend_to_be_on_free_list(&node);

                RuleNode::drop_without_free_list(&mut node);
            }
        }
    }
}

/// The number of RuleNodes added to the free list before we will consider
/// doing a GC when calling maybe_gc().  (The value is copied from Gecko,
/// where it likely did not result from a rigorous performance analysis.)
const RULE_TREE_GC_INTERVAL: usize = 300;

/// A node in the rule tree.
struct RuleNode {
    /// The root node. Only the root has no root pointer, for obvious reasons.
    root: Option<WeakRuleNode>,

    /// The parent rule node. Only the root has no parent.
    parent: Option<StrongRuleNode>,

    /// The actual style source, either coming from a selector in a StyleRule,
    /// or a raw property declaration block (like the style attribute).
    ///
    /// None for the root node.
    source: Option<StyleSource>,

    /// The cascade level + layer order this rule is positioned at.
    cascade_priority: CascadePriority,

    /// The refcount of this node.
    ///
    /// Starts at one. Incremented in `StrongRuleNode::clone` and
    /// `WeakRuleNode::upgrade`. Decremented in `StrongRuleNode::drop`
    /// and `RuleTree::swap_free_list_and_gc`.
    ///
    /// If a non-root node's refcount reaches zero, it is incremented back to at
    /// least one in `RuleNode::pretend_to_be_on_free_list` until the caller who
    /// observed it dropping to zero had a chance to try to remove it from its
    /// parent's children list.
    ///
    /// The refcount should never be decremented to zero if the value in
    /// `next_free` is not null.
    refcount: AtomicUsize,

    /// Only used for the root, stores the number of free rule nodes that are
    /// around.
    approximate_free_count: AtomicUsize,

    /// The children of a given rule node. Children remove themselves from here
    /// when they go away.
    children: RwLock<Map<ChildKey, WeakRuleNode>>,

    /// This field has two different meanings depending on whether this is the
    /// root node or not.
    ///
    /// If it is the root, it represents the head of the free list. It may be
    /// null, which means the free list is gone because the tree was dropped,
    /// and it may be `RuleNode::DANGLING_PTR`, which means the free list is
    /// empty.
    ///
    /// If it is not the root node, this field is either null if the node is
    /// not on the free list, `RuleNode::DANGLING_PTR` if it is the last item
    /// on the free list or the node is pretending to be on the free list, or
    /// any valid non-null pointer representing the next item on the free list
    /// after this one.
    ///
    /// See `RuleNode::push_on_free_list`, `swap_free_list_and_gc`, and
    /// `WeakRuleNode::upgrade`.
    ///
    /// Two threads should never attempt to put the same node on the free list
    /// both at the same time.
    next_free: AtomicPtr<RuleNode>,
}

#[cfg(feature = "gecko_refcount_logging")]
mod gecko_leak_checking {
    use super::RuleNode;
    use std::mem::size_of;
    use std::os::raw::{c_char, c_void};

    extern "C" {
        fn NS_LogCtor(aPtr: *mut c_void, aTypeName: *const c_char, aSize: u32);
        fn NS_LogDtor(aPtr: *mut c_void, aTypeName: *const c_char, aSize: u32);
    }
    static NAME: &'static [u8] = b"RuleNode\0";

    /// Logs the creation of a heap-allocated object to Gecko's leak-checking machinery.
    pub(super) fn log_ctor(ptr: *const RuleNode) {
        let s = NAME as *const [u8] as *const u8 as *const c_char;
        unsafe {
            NS_LogCtor(ptr as *mut c_void, s, size_of::<RuleNode>() as u32);
        }
    }

    /// Logs the destruction of a heap-allocated object to Gecko's leak-checking machinery.
    pub(super) fn log_dtor(ptr: *const RuleNode) {
        let s = NAME as *const [u8] as *const u8 as *const c_char;
        unsafe {
            NS_LogDtor(ptr as *mut c_void, s, size_of::<RuleNode>() as u32);
        }
    }
}

#[inline(always)]
fn log_new(_ptr: *const RuleNode) {
    #[cfg(feature = "gecko_refcount_logging")]
    gecko_leak_checking::log_ctor(_ptr);
}

#[inline(always)]
fn log_drop(_ptr: *const RuleNode) {
    #[cfg(feature = "gecko_refcount_logging")]
    gecko_leak_checking::log_dtor(_ptr);
}

impl RuleNode {
    const DANGLING_PTR: *mut Self = ptr::NonNull::dangling().as_ptr();

    unsafe fn new(
        root: WeakRuleNode,
        parent: StrongRuleNode,
        source: StyleSource,
        cascade_priority: CascadePriority,
    ) -> Self {
        debug_assert!(root.p.parent.is_none());
        source.mark_in_rule_tree();
        RuleNode {
            root: Some(root),
            parent: Some(parent),
            source: Some(source),
            cascade_priority,
            refcount: AtomicUsize::new(1),
            children: Default::default(),
            approximate_free_count: AtomicUsize::new(0),
            next_free: AtomicPtr::new(ptr::null_mut()),
        }
    }

    fn root() -> Self {
        RuleNode {
            root: None,
            parent: None,
            source: None,
            cascade_priority: CascadePriority::new(
                CascadeLevel::new(CascadeOrigin::UA),
                LayerOrder::root(),
                RuleCascadeFlags::empty(),
            ),
            refcount: AtomicUsize::new(1),
            approximate_free_count: AtomicUsize::new(0),
            children: Default::default(),
            next_free: AtomicPtr::new(RuleNode::DANGLING_PTR),
        }
    }

    fn key(&self) -> ChildKey {
        ChildKey(
            self.cascade_priority,
            self.source
                .as_ref()
                .expect("Called key() on the root node")
                .key(),
        )
    }

    /// Drops a node without ever putting it on the free list.
    ///
    /// Note that the node may not be dropped if we observe that its refcount
    /// isn't zero anymore when we write-lock its parent's children map to
    /// remove it.
    ///
    /// This loops over parents of dropped nodes if their own refcount reaches
    /// zero to avoid recursion when dropping deep hierarchies of nodes.
    ///
    /// For non-root nodes, this should always be preceded by a call of
    /// `RuleNode::pretend_to_be_on_free_list`.
    unsafe fn drop_without_free_list(this: &mut UnsafeBox<Self>) {
        let mut this = UnsafeBox::clone(this);
        loop {
            if let Some(parent) = this.parent.as_ref() {
                debug_assert!(!this.next_free.load(Ordering::Relaxed).is_null());

                let mut children = parent.p.children.write();

                this.next_free.store(ptr::null_mut(), Ordering::Relaxed);

                let old_refcount = this.refcount.fetch_sub(1, Ordering::Release);
                debug_assert!(old_refcount != 0);
                if old_refcount != 1 {
                    return;
                }

                debug!(
                    "Remove from child list: {:?}, parent: {:?}",
                    this.as_mut_ptr(),
                    this.parent.as_ref().map(|p| p.p.as_mut_ptr())
                );
                let weak = children.remove(&this.key(), |node| node.p.key()).unwrap();
                assert_eq!(weak.p.as_mut_ptr(), this.as_mut_ptr());
            } else {
                debug_assert_eq!(this.next_free.load(Ordering::Relaxed), ptr::null_mut());
                debug_assert_eq!(this.refcount.load(Ordering::Relaxed), 0);
            }

            atomic::fence(Ordering::Acquire);

            let parent = UnsafeBox::deref_mut(&mut this).parent.take();

            log_drop(&*this);
            UnsafeBox::drop(&mut this);

            if let Some(parent) = parent {
                this = UnsafeBox::clone(&parent.p);
                mem::forget(parent);
                if this.refcount.fetch_sub(1, Ordering::Release) == 1 {
                    debug_assert_eq!(this.next_free.load(Ordering::Relaxed), ptr::null_mut());
                    if this.root.is_some() {
                        RuleNode::pretend_to_be_on_free_list(&this);
                    }
                    continue;
                }
            }

            return;
        }
    }

    /// Pushes this node on the tree's free list. Returns false if the free list
    /// is gone. Should only be called after we decremented a node's refcount
    /// to zero and pretended to be on the free list.
    unsafe fn push_on_free_list(this: &UnsafeBox<Self>) -> bool {
        let root = &this.root.as_ref().unwrap().p;

        debug_assert!(this.refcount.load(Ordering::Relaxed) > 0);
        debug_assert_eq!(this.next_free.load(Ordering::Relaxed), Self::DANGLING_PTR);

        root.approximate_free_count.fetch_add(1, Ordering::Relaxed);

        let mut head = root.next_free.load(Ordering::Relaxed);

        while !head.is_null() {
            debug_assert_ne!(head, this.as_mut_ptr());

            this.next_free.store(head, Ordering::Relaxed);

            match root.next_free.compare_exchange_weak(
                head,
                this.as_mut_ptr(),
                Ordering::Release,
                Ordering::Relaxed,
            ) {
                Ok(_) => {
                    return true;
                },
                Err(new_head) => head = new_head,
            }
        }

        false
    }

    /// Makes the node pretend to be on the free list. This will increment the
    /// refcount by 1 and store `Self::DANGLING_PTR` in `next_free`. This
    /// method should only be called after caller decremented the refcount to
    /// zero, with the null pointer stored in `next_free`.
    unsafe fn pretend_to_be_on_free_list(this: &UnsafeBox<Self>) {
        debug_assert_eq!(this.next_free.load(Ordering::Relaxed), ptr::null_mut());
        this.refcount.fetch_add(1, Ordering::Relaxed);
        this.next_free.store(Self::DANGLING_PTR, Ordering::Release);
    }

    fn as_mut_ptr(&self) -> *mut RuleNode {
        self as *const RuleNode as *mut RuleNode
    }
}

pub(crate) struct WeakRuleNode {
    p: UnsafeBox<RuleNode>,
}

/// A strong reference to a rule node.
pub struct StrongRuleNode {
    p: UnsafeBox<RuleNode>,
}

#[cfg(feature = "servo")]
malloc_size_of::malloc_size_of_is_0!(StrongRuleNode);

impl StrongRuleNode {
    fn new(n: Box<RuleNode>) -> Self {
        debug_assert_eq!(n.parent.is_none(), !n.source.is_some());

        log_new(&*n);

        debug!("Creating rule node: {:p}", &*n);

        Self {
            p: UnsafeBox::from_box(n),
        }
    }

    unsafe fn from_unsafe_box(p: UnsafeBox<RuleNode>) -> Self {
        Self { p }
    }

    unsafe fn downgrade(&self) -> WeakRuleNode {
        WeakRuleNode {
            p: UnsafeBox::clone(&self.p),
        }
    }

    /// Get the parent rule node of this rule node.
    pub fn parent(&self) -> Option<&StrongRuleNode> {
        self.p.parent.as_ref()
    }

    pub(super) fn ensure_child(
        &self,
        root: &StrongRuleNode,
        source: StyleSource,
        cascade_priority: CascadePriority,
    ) -> StrongRuleNode {
        debug_assert!(
            self.p.cascade_priority <= cascade_priority,
            "Should be ordered (instead {:?} > {:?}), from {:?} and {:?}",
            self.p.cascade_priority,
            cascade_priority,
            self.p.source,
            source,
        );

        let key = ChildKey(cascade_priority, source.key());
        {
            let children = self.p.children.read();
            if let Some(child) = children.get(&key, |node| node.p.key()) {
                return unsafe { child.upgrade() };
            }
        }
        let mut children = self.p.children.write();
        match children.entry(key, |node| node.p.key()) {
            Entry::Occupied(child) => {
                unsafe { child.upgrade() }
            },
            Entry::Vacant(entry) => unsafe {
                let node = StrongRuleNode::new(Box::new(RuleNode::new(
                    root.downgrade(),
                    self.clone(),
                    source,
                    cascade_priority,
                )));
                entry.insert(node.downgrade());
                node
            },
        }
    }

    /// Get the style source corresponding to this rule node. May return `None`
    /// if it's the root node, which means that the node hasn't matched any
    /// rules.
    pub fn style_source(&self) -> Option<&StyleSource> {
        self.p.source.as_ref()
    }

    /// The cascade priority.
    #[inline]
    pub fn cascade_priority(&self) -> CascadePriority {
        self.p.cascade_priority
    }

    /// The cascade level.
    #[inline]
    pub fn cascade_level(&self) -> CascadeLevel {
        self.cascade_priority().cascade_level()
    }

    /// The importance.
    #[inline]
    pub fn importance(&self) -> crate::properties::Importance {
        self.cascade_level().importance()
    }

    /// Returns whether this node has any child, only intended for testing
    /// purposes.
    pub unsafe fn has_children_for_testing(&self) -> bool {
        !self.p.children.read().is_empty()
    }

    pub(super) fn dump<W: Write>(&self, guards: &StylesheetGuards, writer: &mut W, indent: usize) {
        const INDENT_INCREMENT: usize = 4;

        for _ in 0..indent {
            let _ = write!(writer, " ");
        }

        let _ = writeln!(
            writer,
            " - {:p} (ref: {:?}, parent: {:?})",
            &*self.p,
            self.p.refcount.load(Ordering::Relaxed),
            self.parent().map(|p| &*p.p as *const RuleNode)
        );

        for _ in 0..indent {
            let _ = write!(writer, " ");
        }

        if let Some(source) = self.style_source() {
            source.dump(self.cascade_level().guard(guards), writer);
        } else {
            if indent != 0 {
                warn!("How has this happened?");
            }
            let _ = write!(writer, "(root)");
        }

        let _ = write!(writer, "\n");
        for child in &*self.p.children.read() {
            unsafe {
                child
                    .upgrade()
                    .dump(guards, writer, indent + INDENT_INCREMENT);
            }
        }
    }
}

impl Clone for StrongRuleNode {
    fn clone(&self) -> Self {
        debug!(
            "{:p}: {:?}+",
            &*self.p,
            self.p.refcount.load(Ordering::Relaxed)
        );
        debug_assert!(self.p.refcount.load(Ordering::Relaxed) > 0);
        self.p.refcount.fetch_add(1, Ordering::Relaxed);
        unsafe { StrongRuleNode::from_unsafe_box(UnsafeBox::clone(&self.p)) }
    }
}

impl Drop for StrongRuleNode {
    #[cfg_attr(feature = "servo", allow(unused_mut))]
    fn drop(&mut self) {
        let node = &*self.p;
        debug!("{:p}: {:?}-", node, node.refcount.load(Ordering::Relaxed));
        debug!(
            "Dropping node: {:p}, root: {:?}, parent: {:?}",
            node,
            node.root.as_ref().map(|r| &*r.p as *const RuleNode),
            node.parent.as_ref().map(|p| &*p.p as *const RuleNode)
        );

        let should_drop = {
            debug_assert!(node.refcount.load(Ordering::Relaxed) > 0);
            node.refcount.fetch_sub(1, Ordering::Release) == 1
        };

        if !should_drop {
            return;
        }

        unsafe {
            if node.root.is_some() {
                RuleNode::pretend_to_be_on_free_list(&self.p);

                if RuleNode::push_on_free_list(&self.p) {
                    return;
                }
            }

            RuleNode::drop_without_free_list(&mut self.p);
        }
    }
}

impl WeakRuleNode {
    /// Upgrades this weak node reference, returning a strong one.
    ///
    /// Must be called with items stored in a node's children list. The children
    /// list must at least be read-locked when this is called.
    unsafe fn upgrade(&self) -> StrongRuleNode {
        debug!("Upgrading weak node: {:p}", &*self.p);

        if self.p.refcount.fetch_add(1, Ordering::Relaxed) == 0 {
            atomic::fence(Ordering::Acquire);
            while self.p.next_free.load(Ordering::Relaxed).is_null() {}
        }
        StrongRuleNode::from_unsafe_box(UnsafeBox::clone(&self.p))
    }
}

impl fmt::Debug for StrongRuleNode {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        (&*self.p as *const RuleNode).fmt(f)
    }
}

impl Eq for StrongRuleNode {}
impl PartialEq for StrongRuleNode {
    fn eq(&self, other: &Self) -> bool {
        &*self.p as *const RuleNode == &*other.p
    }
}

impl hash::Hash for StrongRuleNode {
    fn hash<H>(&self, state: &mut H)
    where
        H: hash::Hasher,
    {
        (&*self.p as *const RuleNode).hash(state)
    }
}

size_of_test!(RuleNode, 80);
size_of_test!(Option<StrongRuleNode>, 8);
