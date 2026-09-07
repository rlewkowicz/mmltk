
/*!
A thread safe memory pool.

The principal type in this module is a [`Pool`]. It main use case is for
holding a thread safe collection of mutable scratch spaces (usually called
`Cache` in this crate) that regex engines need to execute a search. This then
permits sharing the same read-only regex object across multiple threads while
having a quick way of reusing scratch space in a thread safe way. This avoids
needing to re-create the scratch space for every search, which could wind up
being quite expensive.
*/

/// A thread safe pool that works in an `alloc`-only context.
///
/// Getting a value out comes with a guard. When that guard is dropped, the
/// value is automatically put back in the pool. The guard provides both a
/// `Deref` and a `DerefMut` implementation for easy access to an underlying
/// `T`.
///
/// A `Pool` impls `Sync` when `T` is `Send` (even if `T` is not `Sync`). This
/// is possible because a pool is guaranteed to provide a value to exactly one
/// thread at any time.
///
/// Currently, a pool never contracts in size. Its size is proportional to the
/// maximum number of simultaneous uses. This may change in the future.
///
/// A `Pool` is a particularly useful data structure for this crate because
/// many of the regex engines require a mutable "cache" in order to execute
/// a search. Since regexes themselves tend to be global, the problem is then:
/// how do you get a mutable cache to execute a search? You could:
///
/// 1. Use a `thread_local!`, which requires the standard library and requires
/// that the regex pattern be statically known.
/// 2. Use a `Pool`.
/// 3. Make the cache an explicit dependency in your code and pass it around.
/// 4. Put the cache state in a `Mutex`, but this means only one search can
/// execute at a time.
/// 5. Create a new cache for every search.
///
/// A `thread_local!` is perhaps the best choice if it works for your use case.
/// Putting the cache in a mutex or creating a new cache for every search are
/// perhaps the worst choices. Of the remaining two choices, whether you use
/// this `Pool` or thread through a cache explicitly in your code is a matter
/// of taste and depends on your code architecture.
///
/// # Warning: may use a spin lock
///
/// When this crate is compiled _without_ the `std` feature, then this type
/// may used a spin lock internally. This can have subtle effects that may
/// be undesirable. See [Spinlocks Considered Harmful][spinharm] for a more
/// thorough treatment of this topic.
///
/// [spinharm]: https://matklad.github.io/2020/01/02/spinlocks-considered-harmful.html
///
/// # Example
///
/// This example shows how to share a single hybrid regex among multiple
/// threads, while also safely getting exclusive access to a hybrid's
/// [`Cache`](crate::hybrid::regex::Cache) without preventing other searches
/// from running while your thread uses the `Cache`.
///
/// ```
/// use regex_automata::{
///     hybrid::regex::{Cache, Regex},
///     util::{lazy::Lazy, pool::Pool},
///     Match,
/// };
///
/// static RE: Lazy<Regex> =
///     Lazy::new(|| Regex::new("foo[0-9]+bar").unwrap());
/// static CACHE: Lazy<Pool<Cache>> =
///     Lazy::new(|| Pool::new(|| RE.create_cache()));
///
/// let expected = Some(Match::must(0, 3..14));
/// assert_eq!(expected, RE.find(&mut CACHE.get(), b"zzzfoo12345barzzz"));
/// ```
pub struct Pool<T, F = fn() -> T>(alloc::boxed::Box<inner::Pool<T, F>>);

impl<T, F> Pool<T, F> {
    /// Create a new pool. The given closure is used to create values in
    /// the pool when necessary.
    pub fn new(create: F) -> Pool<T, F> {
        Pool(alloc::boxed::Box::new(inner::Pool::new(create)))
    }
}

impl<T: Send, F: Fn() -> T> Pool<T, F> {
    /// Get a value from the pool. The caller is guaranteed to have
    /// exclusive access to the given value. Namely, it is guaranteed that
    /// this will never return a value that was returned by another call to
    /// `get` but was not put back into the pool.
    ///
    /// When the guard goes out of scope and its destructor is called, then
    /// it will automatically be put back into the pool. Alternatively,
    /// [`PoolGuard::put`] may be used to explicitly put it back in the pool
    /// without relying on its destructor.
    ///
    /// Note that there is no guarantee provided about which value in the
    /// pool is returned. That is, calling get, dropping the guard (causing
    /// the value to go back into the pool) and then calling get again is
    /// *not* guaranteed to return the same value received in the first `get`
    /// call.
    #[inline]
    pub fn get(&self) -> PoolGuard<'_, T, F> {
        PoolGuard(self.0.get())
    }
}

impl<T: core::fmt::Debug, F> core::fmt::Debug for Pool<T, F> {
    fn fmt(&self, f: &mut core::fmt::Formatter) -> core::fmt::Result {
        f.debug_tuple("Pool").field(&self.0).finish()
    }
}

/// A guard that is returned when a caller requests a value from the pool.
///
/// The purpose of the guard is to use RAII to automatically put the value
/// back in the pool once it's dropped.
pub struct PoolGuard<'a, T: Send, F: Fn() -> T>(inner::PoolGuard<'a, T, F>);

impl<'a, T: Send, F: Fn() -> T> PoolGuard<'a, T, F> {
    /// Consumes this guard and puts it back into the pool.
    ///
    /// This circumvents the guard's `Drop` implementation. This can be useful
    /// in circumstances where the automatic `Drop` results in poorer codegen,
    /// such as calling non-inlined functions.
    #[inline]
    pub fn put(this: PoolGuard<'_, T, F>) {
        inner::PoolGuard::put(this.0);
    }
}

impl<'a, T: Send, F: Fn() -> T> core::ops::Deref for PoolGuard<'a, T, F> {
    type Target = T;

    #[inline]
    fn deref(&self) -> &T {
        self.0.value()
    }
}

impl<'a, T: Send, F: Fn() -> T> core::ops::DerefMut for PoolGuard<'a, T, F> {
    #[inline]
    fn deref_mut(&mut self) -> &mut T {
        self.0.value_mut()
    }
}

impl<'a, T: Send + core::fmt::Debug, F: Fn() -> T> core::fmt::Debug
    for PoolGuard<'a, T, F>
{
    fn fmt(&self, f: &mut core::fmt::Formatter) -> core::fmt::Result {
        f.debug_tuple("PoolGuard").field(&self.0).finish()
    }
}

#[cfg(feature = "std")]
mod inner {
    use core::{
        cell::UnsafeCell,
        panic::{RefUnwindSafe, UnwindSafe},
        sync::atomic::{AtomicUsize, Ordering},
    };

    use alloc::{boxed::Box, vec, vec::Vec};

    use std::{sync::Mutex, thread_local};

    /// An atomic counter used to allocate thread IDs.
    ///
    /// We specifically start our counter at 3 so that we can use the values
    /// less than it as sentinels.
    static COUNTER: AtomicUsize = AtomicUsize::new(3);

    /// A thread ID indicating that there is no owner. This is the initial
    /// state of a pool. Once a pool has an owner, there is no way to change
    /// it.
    static THREAD_ID_UNOWNED: usize = 0;

    /// A thread ID indicating that the special owner value is in use and not
    /// available. This state is useful for avoiding a case where the owner
    /// of a pool calls `get` before putting the result of a previous `get`
    /// call back into the pool.
    static THREAD_ID_INUSE: usize = 1;

    /// This sentinel is used to indicate that a guard has already been dropped
    /// and should not be re-dropped. We use this because our drop code can be
    /// called outside of Drop and thus there could be a bug in the internal
    /// implementation that results in trying to put the same guard back into
    /// the same pool multiple times, and *that* could result in UB if we
    /// didn't mark the guard as already having been put back in the pool.
    ///
    /// So this isn't strictly necessary, but this let's us define some
    /// routines as safe (like PoolGuard::put_imp) that we couldn't otherwise
    /// do.
    static THREAD_ID_DROPPED: usize = 2;

    /// The number of stacks we use inside of the pool. These are only used for
    /// non-owners. That is, these represent the "slow" path.
    ///
    /// In the original implementation of this pool, we only used a single
    /// stack. While this might be okay for a couple threads, the prevalence of
    /// 32, 64 and even 128 core CPUs has made it untenable. The contention
    /// such an environment introduces when threads are doing a lot of searches
    /// on short haystacks (a not uncommon use case) is palpable and leads to
    /// huge slowdowns.
    ///
    /// This constant reflects a change from using one stack to the number of
    /// stacks that this constant is set to. The stack for a particular thread
    /// is simply chosen by `thread_id % MAX_POOL_STACKS`. The idea behind
    /// this setup is that there should be a good chance that accesses to the
    /// pool will be distributed over several stacks instead of all of them
    /// converging to one.
    ///
    /// This is not a particularly smart or dynamic strategy. Fixing this to a
    /// specific number has at least two downsides. First is that it will help,
    /// say, an 8 core CPU more than it will a 128 core CPU. (But, crucially,
    /// it will still help the 128 core case.) Second is that this may wind
    /// up being a little wasteful with respect to memory usage. Namely, if a
    /// regex is used on one thread and then moved to another thread, then it
    /// could result in creating a new copy of the data in the pool even though
    /// only one is actually needed.
    ///
    /// And that memory usage bit is why this is set to 8 and not, say, 64.
    /// Keeping it at 8 limits, to an extent, how much unnecessary memory can
    /// be allocated.
    ///
    /// In an ideal world, we'd be able to have something like this:
    ///
    /// * Grow the number of stacks as the number of concurrent callers
    /// increases. I spent a little time trying this, but even just adding an
    /// atomic addition/subtraction for each pop/push for tracking concurrent
    /// callers led to a big perf hit. Since even more work would seemingly be
    /// required than just an addition/subtraction, I abandoned this approach.
    /// * The maximum amount of memory used should scale with respect to the
    /// number of concurrent callers and *not* the total number of existing
    /// threads. This is primarily why the `thread_local` crate isn't used, as
    /// as some environments spin up a lot of threads. This led to multiple
    /// reports of extremely high memory usage (often described as memory
    /// leaks).
    /// * Even more ideally, the pool should contract in size. That is, it
    /// should grow with bursts and then shrink. But this is a pretty thorny
    /// issue to tackle and it might be better to just not.
    /// * It would be nice to explore the use of, say, a lock-free stack
    /// instead of using a mutex to guard a `Vec` that is ultimately just
    /// treated as a stack. The main thing preventing me from exploring this
    /// is the ABA problem. The `crossbeam` crate has tools for dealing with
    /// this sort of problem (via its epoch based memory reclamation strategy),
    /// but I can't justify bringing in all of `crossbeam` as a dependency of
    /// `regex` for this.
    ///
    /// See this issue for more context and discussion:
    /// https://github.com/rust-lang/regex/issues/934
    const MAX_POOL_STACKS: usize = 8;

    thread_local!(
        /// A thread local used to assign an ID to a thread.
        static THREAD_ID: usize = {
            let next = COUNTER.fetch_add(1, Ordering::Relaxed);
            if next == 0 {
                panic!("regex: thread ID allocation space exhausted");
            }
            next
        };
    );

    /// This puts each stack in the pool below into its own cache line. This is
    /// an absolutely critical optimization that tends to have the most impact
    /// in high contention workloads. Without forcing each mutex protected
    /// into its own cache line, high contention exacerbates the performance
    /// problem by causing "false sharing." By putting each mutex in its own
    /// cache-line, we avoid the false sharing problem and the affects of
    /// contention are greatly reduced.
    #[derive(Debug)]
    #[repr(C, align(64))]
    struct CacheLine<T>(T);

    /// A thread safe pool utilizing std-only features.
    ///
    /// The main difference between this and the simplistic alloc-only pool is
    /// the use of std::sync::Mutex and an "owner thread" optimization that
    /// makes accesses by the owner of a pool faster than all other threads.
    /// This makes the common case of running a regex within a single thread
    /// faster by avoiding mutex unlocking.
    pub(super) struct Pool<T, F> {
        /// A function to create more T values when stack is empty and a caller
        /// has requested a T.
        create: F,
        /// Multiple stacks of T values to hand out. These are used when a Pool
        /// is accessed by a thread that didn't create it.
        ///
        /// Conceptually this is `Mutex<Vec<Box<T>>>`, but sharded out to make
        /// it scale better under high contention work-loads. We index into
        /// this sequence via `thread_id % stacks.len()`.
        stacks: Vec<CacheLine<Mutex<Vec<Box<T>>>>>,
        /// The ID of the thread that owns this pool. The owner is the thread
        /// that makes the first call to 'get'. When the owner calls 'get', it
        /// gets 'owner_val' directly instead of returning a T from 'stack'.
        /// See comments elsewhere for details, but this is intended to be an
        /// optimization for the common case that makes getting a T faster.
        ///
        /// It is initialized to a value of zero (an impossible thread ID) as a
        /// sentinel to indicate that it is unowned.
        owner: AtomicUsize,
        /// A value to return when the caller is in the same thread that
        /// first called `Pool::get`.
        ///
        /// This is set to None when a Pool is first created, and set to Some
        /// once the first thread calls Pool::get.
        owner_val: UnsafeCell<Option<T>>,
    }

    unsafe impl<T: Send, F: Send + Sync> Sync for Pool<T, F> {}

    impl<T: UnwindSafe, F: UnwindSafe + RefUnwindSafe> UnwindSafe for Pool<T, F> {}

    impl<T: UnwindSafe, F: UnwindSafe + RefUnwindSafe> RefUnwindSafe
        for Pool<T, F>
    {
    }

    impl<T, F> Pool<T, F> {
        /// Create a new pool. The given closure is used to create values in
        /// the pool when necessary.
        pub(super) fn new(create: F) -> Pool<T, F> {
            let mut stacks = Vec::with_capacity(MAX_POOL_STACKS);
            for _ in 0..stacks.capacity() {
                stacks.push(CacheLine(Mutex::new(vec![])));
            }
            let owner = AtomicUsize::new(THREAD_ID_UNOWNED);
            let owner_val = UnsafeCell::new(None); 
            Pool { create, stacks, owner, owner_val }
        }
    }

    impl<T: Send, F: Fn() -> T> Pool<T, F> {
        /// Get a value from the pool. This may block if another thread is also
        /// attempting to retrieve a value from the pool.
        #[inline]
        pub(super) fn get(&self) -> PoolGuard<'_, T, F> {
            let caller = THREAD_ID.with(|id| *id);
            let owner = self.owner.load(Ordering::Acquire);
            if caller == owner {
                self.owner.store(THREAD_ID_INUSE, Ordering::Release);
                return self.guard_owned(caller);
            }
            self.get_slow(caller, owner)
        }

        /// This is the "slow" version that goes through a mutex to pop an
        /// allocated value off a stack to return to the caller. (Or, if the
        /// stack is empty, a new value is created.)
        ///
        /// If the pool has no owner, then this will set the owner.
        #[cold]
        fn get_slow(
            &self,
            caller: usize,
            owner: usize,
        ) -> PoolGuard<'_, T, F> {
            if owner == THREAD_ID_UNOWNED {
                let res = self.owner.compare_exchange(
                    THREAD_ID_UNOWNED,
                    THREAD_ID_INUSE,
                    Ordering::AcqRel,
                    Ordering::Acquire,
                );
                if res.is_ok() {
                    unsafe {
                        *self.owner_val.get() = Some((self.create)());
                    }
                    return self.guard_owned(caller);
                }
            }
            let stack_id = caller % self.stacks.len();
            for _ in 0..1 {
                let mut stack = match self.stacks[stack_id].0.try_lock() {
                    Err(_) => continue,
                    Ok(stack) => stack,
                };
                if let Some(value) = stack.pop() {
                    return self.guard_stack(value);
                }
                drop(stack);
                let value = Box::new((self.create)());
                return self.guard_stack(value);
            }
            self.guard_stack_transient(Box::new((self.create)()))
        }

        /// Puts a value back into the pool. Callers don't need to call this.
        /// Once the guard that's returned by 'get' is dropped, it is put back
        /// into the pool automatically.
        #[inline]
        fn put_value(&self, value: Box<T>) {
            let caller = THREAD_ID.with(|id| *id);
            let stack_id = caller % self.stacks.len();
            for _ in 0..10 {
                let mut stack = match self.stacks[stack_id].0.try_lock() {
                    Err(_) => continue,
                    Ok(stack) => stack,
                };
                stack.push(value);
                return;
            }
        }

        /// Create a guard that represents the special owned T.
        #[inline]
        fn guard_owned(&self, caller: usize) -> PoolGuard<'_, T, F> {
            PoolGuard { pool: self, value: Err(caller), discard: false }
        }

        /// Create a guard that contains a value from the pool's stack.
        #[inline]
        fn guard_stack(&self, value: Box<T>) -> PoolGuard<'_, T, F> {
            PoolGuard { pool: self, value: Ok(value), discard: false }
        }

        /// Create a guard that contains a value from the pool's stack with an
        /// instruction to throw away the value instead of putting it back
        /// into the pool.
        #[inline]
        fn guard_stack_transient(&self, value: Box<T>) -> PoolGuard<'_, T, F> {
            PoolGuard { pool: self, value: Ok(value), discard: true }
        }
    }

    impl<T: core::fmt::Debug, F> core::fmt::Debug for Pool<T, F> {
        fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
            f.debug_struct("Pool")
                .field("stacks", &self.stacks)
                .field("owner", &self.owner)
                .field("owner_val", &self.owner_val)
                .finish()
        }
    }

    /// A guard that is returned when a caller requests a value from the pool.
    pub(super) struct PoolGuard<'a, T: Send, F: Fn() -> T> {
        /// The pool that this guard is attached to.
        pool: &'a Pool<T, F>,
        /// This is Err when the guard represents the special "owned" value.
        /// In which case, the value is retrieved from 'pool.owner_val'. And
        /// in the special case of `Err(THREAD_ID_DROPPED)`, it means the
        /// guard has been put back into the pool and should no longer be used.
        value: Result<Box<T>, usize>,
        /// When true, the value should be discarded instead of being pushed
        /// back into the pool. We tend to use this under high contention, and
        /// this allows us to avoid inflating the size of the pool. (Because
        /// under contention, we tend to create more values instead of waiting
        /// for access to a stack of existing values.)
        discard: bool,
    }

    impl<'a, T: Send, F: Fn() -> T> PoolGuard<'a, T, F> {
        /// Return the underlying value.
        #[inline]
        pub(super) fn value(&self) -> &T {
            match self.value {
                Ok(ref v) => v,
                Err(id) => unsafe {
                    debug_assert_ne!(THREAD_ID_DROPPED, id);
                    (*self.pool.owner_val.get()).as_ref().unwrap_unchecked()
                },
            }
        }

        /// Return the underlying value as a mutable borrow.
        #[inline]
        pub(super) fn value_mut(&mut self) -> &mut T {
            match self.value {
                Ok(ref mut v) => v,
                Err(id) => unsafe {
                    debug_assert_ne!(THREAD_ID_DROPPED, id);
                    (*self.pool.owner_val.get()).as_mut().unwrap_unchecked()
                },
            }
        }

        /// Consumes this guard and puts it back into the pool.
        #[inline]
        pub(super) fn put(this: PoolGuard<'_, T, F>) {
            let mut this = core::mem::ManuallyDrop::new(this);
            this.put_imp();
        }

        /// Puts this guard back into the pool by only borrowing the guard as
        /// mutable. This should be called at most once.
        #[inline(always)]
        fn put_imp(&mut self) {
            match core::mem::replace(&mut self.value, Err(THREAD_ID_DROPPED)) {
                Ok(value) => {
                    if self.discard {
                        return;
                    }
                    self.pool.put_value(value);
                }
                Err(owner) => {
                    assert_ne!(THREAD_ID_DROPPED, owner);
                    self.pool.owner.store(owner, Ordering::Release);
                }
            }
        }
    }

    impl<'a, T: Send, F: Fn() -> T> Drop for PoolGuard<'a, T, F> {
        #[inline]
        fn drop(&mut self) {
            self.put_imp();
        }
    }

    impl<'a, T: Send + core::fmt::Debug, F: Fn() -> T> core::fmt::Debug
        for PoolGuard<'a, T, F>
    {
        fn fmt(&self, f: &mut core::fmt::Formatter) -> core::fmt::Result {
            f.debug_struct("PoolGuard")
                .field("pool", &self.pool)
                .field("value", &self.value)
                .finish()
        }
    }
}

#[cfg(not(feature = "std"))]
mod inner {
    use core::{
        cell::UnsafeCell,
        panic::{RefUnwindSafe, UnwindSafe},
        sync::atomic::{AtomicBool, Ordering},
    };

    use alloc::{boxed::Box, vec, vec::Vec};

    /// A thread safe pool utilizing alloc-only features.
    ///
    /// Unlike the std version, it doesn't seem possible(?) to implement the
    /// "thread owner" optimization because alloc-only doesn't have any concept
    /// of threads. So the best we can do is just a normal stack. This will
    /// increase latency in alloc-only environments.
    pub(super) struct Pool<T, F> {
        /// A stack of T values to hand out. These are used when a Pool is
        /// accessed by a thread that didn't create it.
        stack: Mutex<Vec<Box<T>>>,
        /// A function to create more T values when stack is empty and a caller
        /// has requested a T.
        create: F,
    }

    impl<T: UnwindSafe, F: UnwindSafe> RefUnwindSafe for Pool<T, F> {}

    impl<T, F> Pool<T, F> {
        /// Create a new pool. The given closure is used to create values in
        /// the pool when necessary.
        pub(super) const fn new(create: F) -> Pool<T, F> {
            Pool { stack: Mutex::new(vec![]), create }
        }
    }

    impl<T: Send, F: Fn() -> T> Pool<T, F> {
        /// Get a value from the pool. This may block if another thread is also
        /// attempting to retrieve a value from the pool.
        #[inline]
        pub(super) fn get(&self) -> PoolGuard<'_, T, F> {
            let mut stack = self.stack.lock();
            let value = match stack.pop() {
                None => Box::new((self.create)()),
                Some(value) => value,
            };
            PoolGuard { pool: self, value: Some(value) }
        }

        #[inline]
        fn put(&self, guard: PoolGuard<'_, T, F>) {
            let mut guard = core::mem::ManuallyDrop::new(guard);
            if let Some(value) = guard.value.take() {
                self.put_value(value);
            }
        }

        /// Puts a value back into the pool. Callers don't need to call this.
        /// Once the guard that's returned by 'get' is dropped, it is put back
        /// into the pool automatically.
        #[inline]
        fn put_value(&self, value: Box<T>) {
            let mut stack = self.stack.lock();
            stack.push(value);
        }
    }

    impl<T: core::fmt::Debug, F> core::fmt::Debug for Pool<T, F> {
        fn fmt(&self, f: &mut core::fmt::Formatter<'_>) -> core::fmt::Result {
            f.debug_struct("Pool").field("stack", &self.stack).finish()
        }
    }

    /// A guard that is returned when a caller requests a value from the pool.
    pub(super) struct PoolGuard<'a, T: Send, F: Fn() -> T> {
        /// The pool that this guard is attached to.
        pool: &'a Pool<T, F>,
        /// This is None after the guard has been put back into the pool.
        value: Option<Box<T>>,
    }

    impl<'a, T: Send, F: Fn() -> T> PoolGuard<'a, T, F> {
        /// Return the underlying value.
        #[inline]
        pub(super) fn value(&self) -> &T {
            self.value.as_deref().unwrap()
        }

        /// Return the underlying value as a mutable borrow.
        #[inline]
        pub(super) fn value_mut(&mut self) -> &mut T {
            self.value.as_deref_mut().unwrap()
        }

        /// Consumes this guard and puts it back into the pool.
        #[inline]
        pub(super) fn put(this: PoolGuard<'_, T, F>) {
            let mut this = core::mem::ManuallyDrop::new(this);
            this.put_imp();
        }

        /// Puts this guard back into the pool by only borrowing the guard as
        /// mutable. This should be called at most once.
        #[inline(always)]
        fn put_imp(&mut self) {
            if let Some(value) = self.value.take() {
                self.pool.put_value(value);
            }
        }
    }

    impl<'a, T: Send, F: Fn() -> T> Drop for PoolGuard<'a, T, F> {
        #[inline]
        fn drop(&mut self) {
            self.put_imp();
        }
    }

    impl<'a, T: Send + core::fmt::Debug, F: Fn() -> T> core::fmt::Debug
        for PoolGuard<'a, T, F>
    {
        fn fmt(&self, f: &mut core::fmt::Formatter) -> core::fmt::Result {
            f.debug_struct("PoolGuard")
                .field("pool", &self.pool)
                .field("value", &self.value)
                .finish()
        }
    }

    /// A spin-lock based mutex. Yes, I have read spinlocks considered
    /// harmful[1], and if there's a reasonable alternative choice, I'll
    /// happily take it.
    ///
    /// I suspect the most likely alternative here is a Treiber stack, but
    /// implementing one correctly in a way that avoids the ABA problem looks
    /// subtle enough that I'm not sure I want to attempt that. But otherwise,
    /// we only need a mutex in order to implement our pool, so if there's
    /// something simpler we can use that works for our `Pool` use case, then
    /// that would be great.
    ///
    /// Note that this mutex does not do poisoning.
    ///
    /// [1]: https://matklad.github.io/2020/01/02/spinlocks-considered-harmful.html
    #[derive(Debug)]
    struct Mutex<T> {
        locked: AtomicBool,
        data: UnsafeCell<T>,
    }

    unsafe impl<T: Send> Sync for Mutex<T> {}

    impl<T> Mutex<T> {
        /// Create a new mutex for protecting access to the given value across
        /// multiple threads simultaneously.
        const fn new(value: T) -> Mutex<T> {
            Mutex {
                locked: AtomicBool::new(false),
                data: UnsafeCell::new(value),
            }
        }

        /// Lock this mutex and return a guard providing exclusive access to
        /// `T`. This blocks if some other thread has already locked this
        /// mutex.
        #[inline]
        fn lock(&self) -> MutexGuard<'_, T> {
            while self
                .locked
                .compare_exchange(
                    false,
                    true,
                    Ordering::AcqRel,
                    Ordering::Acquire,
                )
                .is_err()
            {
                core::hint::spin_loop();
            }
            let data = unsafe { &mut *self.data.get() };
            MutexGuard { locked: &self.locked, data }
        }
    }

    /// A guard that derefs to &T and &mut T. When it's dropped, the lock is
    /// released.
    #[derive(Debug)]
    struct MutexGuard<'a, T> {
        locked: &'a AtomicBool,
        data: &'a mut T,
    }

    impl<'a, T> core::ops::Deref for MutexGuard<'a, T> {
        type Target = T;

        #[inline]
        fn deref(&self) -> &T {
            self.data
        }
    }

    impl<'a, T> core::ops::DerefMut for MutexGuard<'a, T> {
        #[inline]
        fn deref_mut(&mut self) -> &mut T {
            self.data
        }
    }

    impl<'a, T> Drop for MutexGuard<'a, T> {
        #[inline]
        fn drop(&mut self) {
            self.locked.store(false, Ordering::Release);
        }
    }
}
