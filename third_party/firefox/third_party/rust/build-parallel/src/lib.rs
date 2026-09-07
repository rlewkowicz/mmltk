use crossbeam_utils::thread;
use std::any::Any;
use std::env;
use std::io;

/// Represents the types of errors that may occur while using build-parallel.
#[derive(Debug)]
pub enum Error<E> {
    /// Error occurred while internally performing I/O.
    IOError(io::Error),
    /// Error occurred during build callback.
    BuildError(E),
    /// Panic occurred during build callback.
    BuildPanic(Box<dyn Any + Send + 'static>),
}

fn compile_object<T, R, E, F>(f: F, obj: &T) -> Result<R, Error<E>>
where
    T: 'static + Sync,
    R: 'static + Sync + Send,
    E: 'static + Sync + Send,
    F: Fn(&T) -> Result<R, E> + Sync + Send,
{
    f(obj).map_err(Error::BuildError)
}

pub fn compile_objects<T, R, E, F>(f: &F, objs: &[T]) -> Result<Vec<R>, Error<E>>
where
    T: 'static + Sync,
    R: 'static + Sync + Send,
    E: 'static + Sync + Send,
    F: Fn(&T) -> Result<R, E> + Sync + Send,
{
    use std::sync::atomic::{AtomicBool, Ordering::SeqCst};
    use std::sync::Once;

    let server = jobserver();
    let reacquire = server.release_raw().is_ok();

    let res = thread::scope(|s| {
        let error = AtomicBool::new(false);
        let mut handles = Vec::new();
        for obj in objs {
            if error.load(SeqCst) {
                break;
            }
            let token = server.acquire().map_err(Error::IOError)?;
            let state = State { obj, error: &error };
            let state = unsafe { std::mem::transmute::<State<T>, State<'static, T>>(state) };
            handles.push(s.spawn(|_| {
                let state: State<T> = state; 
                let result = compile_object(f, state.obj);
                if result.is_err() {
                    state.error.store(true, SeqCst);
                }
                drop(token); 
                result
            }));
        }

        let mut output = Vec::new();
        for handle in handles {
            match handle.join().map_err(Error::BuildPanic)? {
                Ok(r) => output.push(r),
                Err(err) => return Err(err),
            }
        }

        Ok(output)
    })
    .map_err(Error::BuildPanic)?;

    if reacquire {
        server.acquire_raw().map_err(Error::IOError)?;
    }

    return res;

    /// Shared state from the parent thread to the child thread. This
    /// package of pointers is temporarily transmuted to a `'static`
    /// lifetime to cross the thread boundary and then once the thread is
    /// running we erase the `'static` to go back to an anonymous lifetime.
    struct State<'a, O> {
        obj: &'a O,
        error: &'a AtomicBool,
    }

    /// Returns a suitable `jobserver::Client` used to coordinate
    /// parallelism between build scripts.
    fn jobserver() -> &'static jobserver::Client {
        static INIT: Once = Once::new();
        static mut JOBSERVER: Option<jobserver::Client> = None;

        fn _assert_sync<T: Sync>() {}
        _assert_sync::<jobserver::Client>();

        unsafe {
            INIT.call_once(|| {
                let server = default_jobserver();
                JOBSERVER = Some(server);
            });
            JOBSERVER.as_ref().unwrap()
        }
    }

    unsafe fn default_jobserver() -> jobserver::Client {
        if let Some(client) = jobserver::Client::from_env() {
            return client;
        }

        let mut parallelism = num_cpus::get();
        if let Ok(amt) = env::var("NUM_JOBS") {
            if let Ok(amt) = amt.parse() {
                parallelism = amt;
            }
        }

        let client = jobserver::Client::new(parallelism).expect("failed to create jobserver");
        client.acquire_raw().expect("failed to acquire initial");
        client
    }
}
