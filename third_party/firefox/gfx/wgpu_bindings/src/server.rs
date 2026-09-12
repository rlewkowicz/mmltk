/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use crate::{
    error::{error_to_string, ErrMsg, ErrorBuffer, ErrorBufferType, OwnedErrorBuffer},
    make_byte_buf, wgpu_string, AdapterInformation, BufferMapResult, ByteBuf, CommandEncoderAction,
    DeviceAction, FfiSlice, Message, PipelineError, QueueWriteAction, QueueWriteDataSource,
    ServerMessage, ShaderModuleCompilationMessage, SwapChainId, TextureAction,
};

#[path = "mmltk_workspace_channel.rs"]
mod mmltk_workspace_channel;

/// The reserved texture label prefix. A page names an allocation the host
/// admitted by creating an ordinary texture labelled with this prefix and the
/// allocation identifier; stock WebGPU offers no other way to name one.
const MMLTK_WORKSPACE_LABEL_PREFIX: &str = "mmltk-surface-v4/";

use nsstring::{nsACString, nsCString};

use wgc::id;
use wgc::{pipeline::CreateShaderModuleError, resource::BufferAccessError};
#[allow(unused_imports)]
use wgh::Instance;
use wgt::error::{ErrorType, WebGpuError};

use std::borrow::Cow;
use std::collections::{HashMap, VecDeque};
use std::fmt::Write;

#[allow(unused_imports)]
use std::mem;
use std::os::fd::{AsRawFd, FromRawFd, IntoRawFd, OwnedFd, RawFd};
use std::os::raw::c_char;
use std::ptr;
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, Ordering};
use std::sync::{mpsc, Arc, Mutex, OnceLock};
use std::thread;
use std::time::Duration;

#[allow(unused_imports)]
use std::ffi::CString;

use ash::{khr, vk};

/// We limit the size of buffer allocations for stability reason.
/// We can reconsider this limit in the future. Note that some drivers (mesa for example),
/// have issues when the size of a buffer, mapping or copy command does not fit into a
/// signed 32 bits integer, so beyond a certain size, large allocations will need some form
/// of driver allow/blocklist.
///
/// Buffer sizes don't have to be aligned, but storage bindings do, and we clamp
/// maxStorageBufferBindingSize to MAX_BUFFER_SIZE, so use an aligned limit.
pub const MAX_BUFFER_SIZE: wgt::BufferAddress = (1u64 << 31) - 4;

const MAX_TEXTURE_EXTENT: u32 = std::i16::MAX as u32;

const MAX_BINDINGS_PER_RESOURCE_TYPE: u32 = 64;

fn emit_critical_invalid_note(what: &'static str) {
    let msg = CString::new(format!("{what} is invalid")).unwrap();
    unsafe { gfx_critical_note(msg.as_ptr()) }
}

fn get_linux_dmabuf_modifiers() -> Option<Vec<u64>> {
    let mut modifiers_ptr: *const u64 = ptr::null();
    let mut modifier_count = 0u32;

    let ok =
        unsafe { wgpu_server_get_linux_dmabuf_modifiers(&mut modifiers_ptr, &mut modifier_count) };
    if !ok {
        return None;
    }
    if modifier_count == 0 {
        return Some(Vec::new());
    }
    if modifiers_ptr.is_null() {
        return None;
    }

    let modifiers = unsafe { std::slice::from_raw_parts(modifiers_ptr, modifier_count as usize) };
    Some(modifiers.to_vec())
}

fn restrict_limits(limits: wgt::Limits) -> wgt::Limits {
    wgt::Limits {
        max_buffer_size: limits.max_buffer_size.min(MAX_BUFFER_SIZE),
        max_texture_dimension_1d: limits.max_texture_dimension_1d.min(MAX_TEXTURE_EXTENT),
        max_texture_dimension_2d: limits.max_texture_dimension_2d.min(MAX_TEXTURE_EXTENT),
        max_texture_dimension_3d: limits.max_texture_dimension_3d.min(MAX_TEXTURE_EXTENT),
        max_sampled_textures_per_shader_stage: limits
            .max_sampled_textures_per_shader_stage
            .min(MAX_BINDINGS_PER_RESOURCE_TYPE),
        max_samplers_per_shader_stage: limits
            .max_samplers_per_shader_stage
            .min(MAX_BINDINGS_PER_RESOURCE_TYPE),
        max_storage_textures_per_shader_stage: limits
            .max_storage_textures_per_shader_stage
            .min(MAX_BINDINGS_PER_RESOURCE_TYPE),
        max_uniform_buffers_per_shader_stage: limits
            .max_uniform_buffers_per_shader_stage
            .min(MAX_BINDINGS_PER_RESOURCE_TYPE),
        max_storage_buffers_per_shader_stage: limits
            .max_storage_buffers_per_shader_stage
            .min(MAX_BINDINGS_PER_RESOURCE_TYPE),
        max_uniform_buffer_binding_size: limits
            .max_uniform_buffer_binding_size
            .min(MAX_BUFFER_SIZE),
        max_storage_buffer_binding_size: limits
            .max_storage_buffer_binding_size
            .min(MAX_BUFFER_SIZE),
        max_non_sampler_bindings: 500_000,
        ..limits
    }
}

/// Opaque pointer to `mozilla::webgpu::WebGPUParent`.
#[derive(Debug, Clone, Copy)]
#[repr(transparent)]
pub struct WebGPUParentPtr(*mut core::ffi::c_void);

// The dispatcher carries this identity across threads only to enqueue a C++
// callback onto WebGPUParent's owning serial event target. Rust never
// dereferences the pointer and shutdown joins the dispatcher before Global
// releases the parent-owned context.
unsafe impl Send for WebGPUParentPtr {}
unsafe impl Sync for WebGPUParentPtr {}

pub struct Global {
    owner: WebGPUParentPtr,
    global: Arc<wgc::global::Global>,
    device_poll_workers: DevicePollWorkers,
    swap_chain_configs: Mutex<HashMap<SwapChainId, SwapChainConfig>>,
    /// One registration per browser sample texture, keyed by the private texture
    /// the page samples. One dispatcher owns every frame edge and raw blit.
    mmltk_workspace_mirrors: Mutex<HashMap<id::TextureId, MmltkWorkspaceMirror>>,
    mmltk_workspace_dispatcher: MmltkWorkspaceDispatcher,
}

const DEVICE_POLL_COMPLETION_TIMEOUT: Duration = Duration::from_secs(60);
const DEVICE_POLL_SHUTDOWN_TIMEOUT: Duration = Duration::from_secs(6);

fn abort_device_poll_driver(
    device_id: Option<id::DeviceId>,
    submission_index: wgc::SubmissionIndex,
    detail: &'static str,
) -> ! {
    log::error!(
        "WebGPU completion driver failure: {detail}; device={device_id:?}, submission={submission_index}"
    );
    std::process::abort();
}

#[derive(Default)]
struct DevicePollDriverState {
    pending_submission: Mutex<Option<wgc::SubmissionIndex>>,
    shutdown: AtomicBool,
}

struct DevicePollDriver {
    device_id: id::DeviceId,
    state: Arc<DevicePollDriverState>,
    wake_sender: mpsc::SyncSender<()>,
    finished_receiver: mpsc::Receiver<()>,
    thread: Option<thread::JoinHandle<()>>,
}

impl DevicePollDriver {
    fn new(global: Arc<wgc::global::Global>, device_id: id::DeviceId) -> Self {
        let state = Arc::new(DevicePollDriverState::default());
        let worker_state = state.clone();
        let (wake_sender, wake_receiver) = mpsc::sync_channel(1);
        let (finished_sender, finished_receiver) = mpsc::sync_channel(1);
        let thread = thread::Builder::new()
            .name(format!("WebGPU completion {device_id:?}"))
            .spawn(move || {
                let outcome = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| loop {
                    let submission_index = match worker_state.pending_submission.lock() {
                        Ok(mut pending) => pending.take(),
                        Err(_) => abort_device_poll_driver(
                            Some(device_id),
                            0,
                            "completion state mutex was poisoned",
                        ),
                    };
                    if let Some(submission_index) = submission_index {
                        match global.device_poll(
                            device_id,
                            wgt::PollType::Wait {
                                submission_index: Some(submission_index),
                                timeout: Some(DEVICE_POLL_COMPLETION_TIMEOUT),
                            },
                        ) {
                            Err(wgc::device::WaitIdleError::Timeout) => abort_device_poll_driver(
                                Some(device_id),
                                submission_index,
                                "GPU completion deadline expired",
                            ),
                            Err(error) => {
                                log::error!(
                                    "WebGPU completion driver could not settle submission: device={device_id:?}, submission={submission_index}, error={error:?}"
                                );
                                abort_device_poll_driver(
                                    Some(device_id),
                                    submission_index,
                                    "device polling failed",
                                );
                            }
                            Ok(_) => {}
                        }
                        continue;
                    }

                    if worker_state.shutdown.load(Ordering::Acquire) {
                        break;
                    }
                    if wake_receiver.recv().is_err() {
                        abort_device_poll_driver(
                            Some(device_id),
                            0,
                            "completion wake channel disconnected",
                        );
                    }
                }));
                if outcome.is_err() {
                    abort_device_poll_driver(Some(device_id), 0, "completion worker panicked");
                }
                let _ = finished_sender.try_send(());
            })
            .unwrap_or_else(|_| {
                abort_device_poll_driver(
                    Some(device_id),
                    0,
                    "completion worker could not start",
                );
            });
        Self {
            device_id,
            state,
            wake_sender,
            finished_receiver,
            thread: Some(thread),
        }
    }

    fn wait_for(&self, submission_index: wgc::SubmissionIndex) {
        let device_id = self.device_id;
        if self.state.shutdown.load(Ordering::Acquire) {
            abort_device_poll_driver(
                Some(device_id),
                submission_index,
                "completion submitted after device shutdown",
            );
        }
        match self.state.pending_submission.lock() {
            Ok(mut pending) => {
                *pending =
                    Some(pending.map_or(submission_index, |current| current.max(submission_index)));
            }
            Err(_) => abort_device_poll_driver(
                Some(device_id),
                submission_index,
                "completion state mutex was poisoned",
            ),
        }
        match self.wake_sender.try_send(()) {
            Ok(()) | Err(mpsc::TrySendError::Full(())) => {}
            Err(mpsc::TrySendError::Disconnected(())) => abort_device_poll_driver(
                Some(device_id),
                submission_index,
                "completion worker exited before accepting work",
            ),
        }
    }

    fn request_shutdown(&self) {
        self.state.shutdown.store(true, Ordering::Release);
        match self.wake_sender.try_send(()) {
            Ok(()) | Err(mpsc::TrySendError::Full(())) => {}
            Err(mpsc::TrySendError::Disconnected(())) => {}
        }
    }

    fn join(&mut self) {
        let Some(thread) = self.thread.take() else {
            return;
        };
        match self
            .finished_receiver
            .recv_timeout(DEVICE_POLL_SHUTDOWN_TIMEOUT)
        {
            Ok(()) => {}
            Err(mpsc::RecvTimeoutError::Timeout) => abort_device_poll_driver(
                Some(self.device_id),
                0,
                "completion worker did not quiesce during shutdown",
            ),
            Err(mpsc::RecvTimeoutError::Disconnected) => {}
        }
        if thread.join().is_err() {
            abort_device_poll_driver(Some(self.device_id), 0, "completion worker join failed");
        }
    }
}

impl Drop for DevicePollDriver {
    fn drop(&mut self) {
        self.request_shutdown();
        self.join();
    }
}

#[derive(Default)]
struct DevicePollWorkersState {
    drivers: HashMap<id::DeviceId, DevicePollDriver>,
    shutting_down: bool,
}

struct DevicePollWorkers {
    global: Arc<wgc::global::Global>,
    state: Mutex<DevicePollWorkersState>,
}

impl DevicePollWorkers {
    fn new(global: Arc<wgc::global::Global>) -> Self {
        Self {
            global,
            state: Mutex::new(DevicePollWorkersState::default()),
        }
    }

    fn wait_for(&self, device_id: id::DeviceId, submission_index: wgc::SubmissionIndex) {
        let mut state = match self.state.lock() {
            Ok(state) => state,
            Err(_) => abort_device_poll_driver(
                Some(device_id),
                submission_index,
                "completion owner mutex was poisoned",
            ),
        };
        if state.shutting_down {
            abort_device_poll_driver(
                Some(device_id),
                submission_index,
                "completion submitted after global shutdown",
            );
        }
        state
            .drivers
            .entry(device_id)
            .or_insert_with(|| DevicePollDriver::new(self.global.clone(), device_id))
            .wait_for(submission_index);
    }

    fn stop_device(&self, device_id: id::DeviceId) {
        let driver = match self.state.lock() {
            Ok(mut state) => state.drivers.remove(&device_id),
            Err(_) => {
                abort_device_poll_driver(Some(device_id), 0, "completion owner mutex was poisoned")
            }
        };
        drop(driver);
    }

    fn shutdown_all(&self) {
        let mut drivers = match self.state.lock() {
            Ok(mut state) => {
                state.shutting_down = true;
                std::mem::take(&mut state.drivers)
            }
            Err(_) => abort_device_poll_driver(
                None,
                0,
                "completion owner mutex was poisoned during global shutdown",
            ),
        };
        for driver in drivers.values() {
            driver.request_shutdown();
        }
        for driver in drivers.values_mut() {
            driver.join();
        }
    }
}

impl Drop for DevicePollWorkers {
    fn drop(&mut self) {
        self.shutdown_all();
    }
}

/// Values for the descriptor when creating textures for an active swap chain.
#[derive(Clone)]
struct SwapChainConfig {
    size: wgt::Extent3d,
    format: wgt::TextureFormat,
    usage: wgt::TextureUsages,
    view_formats: Vec<wgt::TextureFormat>,
}

impl SwapChainConfig {
    fn to_texture_descriptor(&self) -> wgc::resource::TextureDescriptor<'static> {
        wgt::TextureDescriptor {
            label: Some(Cow::Borrowed("swap chain texture")),
            size: self.size,
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgt::TextureDimension::D2,
            format: self.format,
            usage: self.usage,
            view_formats: self.view_formats.clone(),
        }
    }
}

impl std::ops::Deref for Global {
    type Target = wgc::global::Global;
    fn deref(&self) -> &Self::Target {
        &self.global
    }
}

impl Drop for Global {
    fn drop(&mut self) {
        // Every mirror holds raw Vulkan objects owned by devices this is about
        // to release, so the mirrors retire first.
        self.stop_all_mmltk_workspace_mirrors();
        self.device_poll_workers.shutdown_all();
    }
}

#[no_mangle]
pub extern "C" fn wgpu_server_new(owner: WebGPUParentPtr) -> *mut Global {
    mmltk_workspace_channel::initialize_diagnostics();
    MMLTK_WORKSPACE_ACCEPTANCE_TRACE.get_or_init(|| {
        mmltk_workspace_channel::workspace_diagnostics_enabled()
            && std::env::var("MMLTK_RUN_WORKSPACE_WAYLAND_INTEGRATION").as_deref() == Ok("1")
    });
    log::info!("Initializing WGPU server");
    let backends = wgt::Backends::VULKAN;

    let mut instance_flags = (wgt::InstanceFlags::from_build_config()
        | wgt::InstanceFlags::AUTOMATIC_TIMESTAMP_NORMALIZATION
        | wgt::InstanceFlags::STRICT_WEBGPU_COMPLIANCE)
        .with_env();
    if !static_prefs::pref!("dom.webgpu.hal-labels") {
        instance_flags.insert(wgt::InstanceFlags::DISCARD_HAL_LABELS);
    }

    let global = Arc::new(wgc::global::Global::new(
        "wgpu",
        wgt::InstanceDescriptor {
            backends,
            flags: instance_flags,
            backend_options: wgt::BackendOptions::default(),
            memory_budget_thresholds: wgt::MemoryBudgetThresholds {
                for_resource_creation: Some(95),
                for_device_loss: Some(99),
            },
            display: None,
        },
        None,
    ));
    let device_poll_workers = DevicePollWorkers::new(global.clone());
    let global = Global {
        owner,
        global,
        device_poll_workers,
        swap_chain_configs: Mutex::new(HashMap::new()),
        mmltk_workspace_mirrors: Mutex::new(HashMap::new()),
        mmltk_workspace_dispatcher: MmltkWorkspaceDispatcher::new(owner),
    };
    Box::into_raw(Box::new(global))
}

/// # Safety
///
/// This function is unsafe because improper use may lead to memory
/// problems. For example, a double-free may occur if the function is called
/// twice on the same raw pointer.
#[no_mangle]
pub unsafe extern "C" fn wgpu_server_delete(global: *mut Global) {
    log::info!("Terminating WGPU server");
    let global = Box::from_raw(global);
    global.device_poll_workers.shutdown_all();
}

#[no_mangle]
pub extern "C" fn wgpu_server_device_poll(
    global: &Global,
    device_id: id::DeviceId,
    force_wait: bool,
) {
    let maintain = if force_wait {
        wgt::PollType::Wait {
            submission_index: None,
            timeout: Some(DEVICE_POLL_COMPLETION_TIMEOUT),
        }
    } else {
        wgt::PollType::Poll
    };
    global.device_poll(device_id, maintain).unwrap();
}

fn support_use_shared_texture_in_swap_chain(
    global: &Global,
    self_id: id::AdapterId,
    backend: wgt::Backend,
    is_hardware: bool,
) -> bool {
    let _ = is_hardware;
    if backend != wgt::Backend::Vulkan {
        log::info!(concat!(
            "WebGPU: disabling SharedTexture swapchain: \n",
            "wgpu backend is not Vulkan"
        ));
        return false;
    }

    let Some(hal_adapter) = (unsafe { global.adapter_as_hal::<wgc::api::Vulkan>(self_id) }) else {
        unreachable!("given adapter ID was actually for a different backend");
    };

    let capabilities = hal_adapter.physical_device_capabilities();
    static REQUIRED: &[&'static std::ffi::CStr] = &[
        khr::external_memory_fd::NAME,
        ash::ext::external_memory_dma_buf::NAME,
        ash::ext::image_drm_format_modifier::NAME,
        khr::external_semaphore_fd::NAME,
    ];
    let all_extensions_supported = REQUIRED.iter().all(|&extension| {
        let supported = capabilities.supports_extension(extension);
        if !supported {
            log::info!(
                concat!(
                    "WebGPU: disabling SharedTexture swapchain: \n",
                    "Vulkan extension not supported: {:?}",
                ),
                extension.to_string_lossy()
            );
        }
        supported
    });
    if !all_extensions_supported {
        return false;
    }

    let semaphore_info = vk::PhysicalDeviceExternalSemaphoreInfo::default()
        .handle_type(vk::ExternalSemaphoreHandleTypeFlags::OPAQUE_FD);
    let mut semaphore_props = vk::ExternalSemaphoreProperties::default();
    unsafe {
        hal_adapter
            .shared_instance()
            .raw_instance()
            .get_physical_device_external_semaphore_properties(
                hal_adapter.raw_physical_device(),
                &semaphore_info,
                &mut semaphore_props,
            );
    }
    if !semaphore_props
        .external_semaphore_features
        .contains(vk::ExternalSemaphoreFeatureFlags::EXPORTABLE)
    {
        log::info!(
            "WebGPU: disabling ExternalTexture swapchain: \n\
                        device can't export opaque file descriptor semaphores"
        );
        return false;
    }

    true
}

unsafe fn adapter_request_device(
    global: &Global,
    self_id: id::AdapterId,
    desc: wgc::device::DeviceDescriptor,
    new_device_id: id::DeviceId,
    new_queue_id: id::QueueId,
) -> Option<String> {
    let mut sanitized_desc = {
        let wgc::device::DeviceDescriptor {
            label,
            required_features,
            required_limits,
            experimental_features,
            memory_hints,
            trace,
        } = desc;

        assert_eq!(required_features.features_wgpu, wgt::FeaturesWGPU::empty());
        assert!(!experimental_features.is_enabled());
        assert!(matches!(memory_hints, wgt::MemoryHints::Performance));
        assert!(matches!(trace, wgt::Trace::Off));

        wgc::device::DeviceDescriptor {
            label,
            required_features,
            required_limits,
            experimental_features: wgt::ExperimentalFeatures::disabled(),
            memory_hints: wgt::MemoryHints::MemoryUsage,
            trace: wgt::Trace::Off,
        }
    };

    if wgpu_parent_is_external_texture_enabled() {
        for feature in [
            wgt::Features::EXTERNAL_TEXTURE,
            wgt::Features::TEXTURE_FORMAT_NV12,
            wgt::Features::TEXTURE_FORMAT_P010,
            wgt::Features::TEXTURE_FORMAT_16BIT_NORM,
        ] {
            if global.adapter_features(self_id).contains(feature) {
                sanitized_desc.required_features.insert(feature);
            }
        }
    }

    // The reserved-label import path exposes Firefox-owned workspace storage as
    // a sampled texture. Source backing is exported to native CUDA, which needs
    // the external-memory descriptor feature whenever the adapter offers it.
    if global
        .adapter_features(self_id)
        .contains(wgt::Features::VULKAN_EXTERNAL_MEMORY_FD)
    {
        sanitized_desc
            .required_features
            .insert(wgt::Features::VULKAN_EXTERNAL_MEMORY_FD);
    }

    let res = global.adapter_request_device(
        self_id,
        &sanitized_desc,
        Some(new_device_id),
        Some(new_queue_id),
    );
    if let Err(err) = res {
        return Some(format!("{err}"));
    }
    None
}

#[repr(C)]
pub struct DeviceLostClosure {
    pub callback: unsafe extern "C" fn(user_data: *mut u8, reason: u8, message: *const c_char),
    pub cleanup_callback: unsafe extern "C" fn(user_data: *mut u8),
    pub user_data: *mut u8,
}

impl DeviceLostClosure {
    fn call(self, reason: wgt::DeviceLostReason, message: String) {
        let message = std::ffi::CString::new(message).unwrap();
        unsafe {
            (self.callback)(self.user_data, reason as u8, message.as_ptr());
        }
        core::mem::forget(self);
    }
}

impl Drop for DeviceLostClosure {
    fn drop(&mut self) {
        unsafe {
            (self.cleanup_callback)(self.user_data);
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn wgpu_server_set_device_lost_callback(
    global: &Global,
    self_id: id::DeviceId,
    closure: DeviceLostClosure,
) {
    let (device_lost_sender, device_lost_receiver) = futures_channel::oneshot::channel();

    moz_task::spawn_local("device lost callback", async move {
        match device_lost_receiver.await {
            Ok((reason, message)) => {
                closure.call(reason, message);
            }
            Err(futures_channel::oneshot::Canceled) => {}
        }
    })
    .detach();

    global.device_set_device_lost_closure(
        self_id,
        Box::new(move |reason, message| {
            device_lost_sender.send((reason, message)).unwrap();
        }),
    );
}

impl ShaderModuleCompilationMessage {
    fn new(error: &CreateShaderModuleError, source: &str) -> Self {
        let line_number;
        let line_pos;
        let utf16_offset;
        let utf16_length;

        let location = match error {
            CreateShaderModuleError::Parsing(e) => e.inner.location(source),
            CreateShaderModuleError::Validation(e) => e.inner.location(source),
            _ => None,
        };

        if let Some(location) = location {
            let len_utf16 = |s: &str| s.chars().map(|c| c.len_utf16() as u64).sum();
            let start = location.offset as usize;
            let end = start + location.length as usize;
            utf16_offset = len_utf16(&source[0..start]);
            utf16_length = len_utf16(&source[start..end]);

            line_number = location.line_number as u64;
            let line_start = source[0..start].rfind('\n').map(|pos| pos + 1).unwrap_or(0);
            line_pos = len_utf16(&source[line_start..start]) + 1;
        } else {
            line_number = 0;
            line_pos = 0;
            utf16_offset = 0;
            utf16_length = 0;
        }

        let message = error.to_string();

        Self {
            line_number,
            line_pos,
            utf16_offset,
            utf16_length,
            message,
        }
    }
}

#[no_mangle]
pub extern "C" fn wgpu_server_device_create_buffer(
    global: &Global,
    device_id: id::DeviceId,
    buffer_id: id::BufferId,
    label: Option<&nsACString>,
    size: wgt::BufferAddress,
    usage: u32,
    mapped_at_creation: bool,
    mut error_buf: ErrorBuffer,
) {
    let utf8_label = label.map(|utf16| utf16.to_string());
    let label = utf8_label.as_ref().map(|s| Cow::from(&s[..]));
    let usage = wgt::BufferUsages::from_bits_retain(usage);

    let desc = wgc::resource::BufferDescriptor {
        label,
        size,
        usage,
        mapped_at_creation,
    };

    let (_, error) = global.device_create_buffer(device_id, &desc, Some(buffer_id));
    if let Some(err) = error {
        error_buf.init(err, device_id);
    }
}

/// The status code provided to the buffer mapping closure.
///
/// This is very similar to `BufferAccessResult`, except that this is FFI-friendly.
#[repr(C)]
pub enum BufferMapAsyncStatus {
    /// The Buffer is successfully mapped, `get_mapped_range` can be called.
    ///
    /// All other variants of this enum represent failures to map the buffer.
    Success,
    /// The buffer is already mapped.
    ///
    /// While this is treated as an error, it does not prevent mapped range from being accessed.
    AlreadyMapped,
    /// Mapping was already requested.
    MapAlreadyPending,
    /// An unknown error.
    Error,
    /// The context is Lost.
    ContextLost,
    /// The buffer is in an invalid state.
    Invalid,
    /// The range isn't fully contained in the buffer.
    InvalidRange,
    /// The range isn't properly aligned.
    InvalidAlignment,
    /// Incompatible usage flags.
    InvalidUsageFlags,
}

impl From<Result<(), BufferAccessError>> for BufferMapAsyncStatus {
    fn from(result: Result<(), BufferAccessError>) -> Self {
        match result {
            Ok(_) => BufferMapAsyncStatus::Success,
            Err(BufferAccessError::Device(_)) => BufferMapAsyncStatus::ContextLost,
            Err(BufferAccessError::InvalidResource(_))
            | Err(BufferAccessError::DestroyedResource(_)) => BufferMapAsyncStatus::Invalid,
            Err(BufferAccessError::AlreadyMapped) => BufferMapAsyncStatus::AlreadyMapped,
            Err(BufferAccessError::MapAlreadyPending) => BufferMapAsyncStatus::MapAlreadyPending,
            Err(BufferAccessError::MissingBufferUsage(_)) => {
                BufferMapAsyncStatus::InvalidUsageFlags
            }
            Err(BufferAccessError::UnalignedRange)
            | Err(BufferAccessError::UnalignedRangeSize { .. })
            | Err(BufferAccessError::UnalignedOffset { .. }) => {
                BufferMapAsyncStatus::InvalidAlignment
            }
            Err(BufferAccessError::OutOfBoundsStartOffsetUnderrun { .. })
            | Err(BufferAccessError::OutOfBoundsStartOffsetOverrun { .. })
            | Err(BufferAccessError::OutOfBoundsEndOffsetOverrun { .. })
            | Err(BufferAccessError::MapStartOffsetOverrun { .. })
            | Err(BufferAccessError::MapEndOffsetOverrun { .. }) => {
                BufferMapAsyncStatus::InvalidRange
            }
            Err(BufferAccessError::Failed)
            | Err(BufferAccessError::NotMapped)
            | Err(BufferAccessError::MapAborted) => BufferMapAsyncStatus::Error,
            Err(_) => BufferMapAsyncStatus::Invalid,
        }
    }
}

impl From<Result<(), wgc::device::WaitIdleError>> for BufferMapAsyncStatus {
    fn from(result: Result<(), wgc::device::WaitIdleError>) -> Self {
        match result {
            Ok(()) => BufferMapAsyncStatus::Success,
            Err(err) => match err {
                wgc::device::WaitIdleError::Device(_) => BufferMapAsyncStatus::ContextLost,
                wgc::device::WaitIdleError::WrongSubmissionIndex(_, _)
                | wgc::device::WaitIdleError::Timeout => BufferMapAsyncStatus::Error,
                _ => BufferMapAsyncStatus::Error,
            },
        }
    }
}

#[repr(C)]
pub struct BufferMapClosure {
    pub callback: unsafe extern "C" fn(user_data: *mut u8, status: BufferMapAsyncStatus),
    pub user_data: *mut u8,
}

/// # Safety
///
/// Callers are responsible for ensuring `closure` is well-formed.
#[no_mangle]
pub unsafe extern "C" fn wgpu_server_buffer_map(
    global: &Global,
    device_id: id::DeviceId,
    buffer_id: id::BufferId,
    start: wgt::BufferAddress,
    size: wgt::BufferAddress,
    map_mode: wgc::device::HostMap,
    closure: BufferMapClosure,
    mut error_buf: ErrorBuffer,
) {
    let (map_result_sender, map_result_receiver) = futures_channel::oneshot::channel();

    moz_task::spawn_local("wgpu_server_buffer_map callback", async move {
        let result = map_result_receiver.await.unwrap();
        (closure.callback)(closure.user_data, BufferMapAsyncStatus::from(result));
    })
    .detach();

    let operation = wgc::resource::BufferMapOperation {
        host: map_mode,
        callback: Some(Box::new(move |result| {
            map_result_sender.send(result).unwrap();
        })),
    };
    match global.buffer_map_async(buffer_id, start, Some(size), operation) {
        Ok(submission_index) => {
            global
                .device_poll_workers
                .wait_for(device_id, submission_index);
        }
        Err(error) => {
            error_buf.init(error, device_id);
        }
    }
}

/// Map a buffer, blocking until it is ready for access.
///
/// Map the `size` bytes starting at `offset` in `buffer_id` to be accessed
/// according to `map_mode`, blocking the calling thread until the mapping is
/// ready.
///
/// This function actually blocks the calling thread until the GPU has completed
/// all previously submitted work, even if the buffer is actually available
/// right now. In practice, this function is generally used immediately after
/// submitted work that writes data to the buffer, so this shouldn't be much of
/// a problem.
///
/// All ids are looked up using `global`. The buffer `buffer_id` must belong to
/// `device_id`.
///
/// Return a `BufferMapAsyncStatus` to indicate success or failure.
#[no_mangle]
pub extern "C" fn wgpu_server_buffer_map_blocking(
    global: &Global,
    device_id: id::DeviceId,
    buffer_id: id::BufferId,
    offset: wgt::BufferAddress,
    size: wgt::BufferAddress,
    map_mode: wgc::device::HostMap,
) -> BufferMapAsyncStatus {
    let (status_sender, status_receiver) = mpsc::sync_channel(1);
    let op = wgc::resource::BufferMapOperation {
        host: map_mode,
        callback: Some(Box::new(move |status| {
            mmltk_workspace_channel::write_diagnostic(|line| write!(line,
                "{{\"event\":\"firefox.webgpu.blocking_map\",\"detail\":\"callback\",\"buffer\":\"{buffer_id:?}\"}}"
            ));
            if status_sender.send(status).is_ok() {
                mmltk_workspace_channel::write_diagnostic(|line| write!(line,
                    "{{\"event\":\"firefox.webgpu.blocking_map\",\"detail\":\"callback_sent\",\"buffer\":\"{buffer_id:?}\"}}"
                ));
            }
        })),
    };

    mmltk_workspace_channel::write_diagnostic(|line| write!(line,
        "{{\"event\":\"firefox.webgpu.blocking_map\",\"detail\":\"admission\",\"buffer\":\"{buffer_id:?}\"}}"
    ));
    let submission_index;
    match global.buffer_map_async(buffer_id, offset, Some(size), op) {
        Ok(i) => {
            submission_index = i;
            mmltk_workspace_channel::write_diagnostic(|line| write!(line,
                "{{\"event\":\"firefox.webgpu.blocking_map\",\"detail\":\"admitted\",\"buffer\":\"{buffer_id:?}\",\"submission\":\"{submission_index:?}\"}}"
            ));
        }
        Err(err) => {
            mmltk_workspace_channel::write_diagnostic(|line| write!(line,
                "{{\"event\":\"firefox.webgpu.blocking_map\",\"detail\":\"rejected\",\"buffer\":\"{buffer_id:?}\",\"error\":\"{err:?}\"}}"
            ));
            return BufferMapAsyncStatus::from(Err(err));
        }
    }

    let poll_type = wgt::PollType::Wait {
        submission_index: Some(submission_index),
        timeout: Some(Duration::from_secs(60)),
    };
    mmltk_workspace_channel::write_diagnostic(|line| write!(line,
        "{{\"event\":\"firefox.webgpu.blocking_map\",\"detail\":\"poll_started\",\"buffer\":\"{buffer_id:?}\"}}"
    ));
    if let Err(err) = global.device_poll(device_id, poll_type) {
        mmltk_workspace_channel::write_diagnostic(|line| write!(line,
            "{{\"event\":\"firefox.webgpu.blocking_map\",\"detail\":\"poll_failed\",\"buffer\":\"{buffer_id:?}\",\"error\":\"{err:?}\"}}"
        ));
        return BufferMapAsyncStatus::from(Err(err));
    }
    mmltk_workspace_channel::write_diagnostic(|line| write!(line,
        "{{\"event\":\"firefox.webgpu.blocking_map\",\"detail\":\"poll_completed\",\"buffer\":\"{buffer_id:?}\"}}"
    ));

    mmltk_workspace_channel::write_diagnostic(|line| write!(line,
        "{{\"event\":\"firefox.webgpu.blocking_map\",\"detail\":\"callback_wait\",\"buffer\":\"{buffer_id:?}\"}}"
    ));
    let status_result = match status_receiver.recv_timeout(Duration::from_secs(60)) {
        Ok(status) => status,
        Err(error) => {
            mmltk_workspace_channel::write_diagnostic(|line| write!(line,
                "{{\"event\":\"firefox.webgpu.blocking_map\",\"detail\":\"callback_wait_failed\",\"buffer\":\"{buffer_id:?}\",\"error\":\"{error}\"}}"
            ));
            return BufferMapAsyncStatus::Error;
        }
    };

    mmltk_workspace_channel::write_diagnostic(|line| write!(line,
        "{{\"event\":\"firefox.webgpu.blocking_map\",\"detail\":\"settled\",\"buffer\":\"{buffer_id:?}\"}}"
    ));
    BufferMapAsyncStatus::from(status_result)
}

#[repr(C)]
pub struct MappedBufferSlice {
    pub ptr: *mut u8,
    pub length: u64,
}

/// # Safety
///
/// This function is unsafe as there is no guarantee that the given pointer is
/// valid for `size` elements.
#[no_mangle]
pub unsafe extern "C" fn wgpu_server_buffer_get_mapped_range(
    global: &Global,
    device_id: id::DeviceId,
    buffer_id: id::BufferId,
    start: wgt::BufferAddress,
    size: wgt::BufferAddress,
    mut error_buf: ErrorBuffer,
) -> MappedBufferSlice {
    let result = global.buffer_get_mapped_range(buffer_id, start, Some(size));

    let (ptr, length) = result
        .map(|(ptr, len)| (ptr.as_ptr(), len))
        .unwrap_or_else(|error| {
            error_buf.init(error, device_id);
            (std::ptr::null_mut(), 0)
        });
    MappedBufferSlice { ptr, length }
}

#[no_mangle]
pub extern "C" fn wgpu_server_buffer_unmap(
    global: &Global,
    device_id: id::DeviceId,
    buffer_id: id::BufferId,
    mut error_buf: ErrorBuffer,
) {
    if let Err(e) = global.buffer_unmap(buffer_id) {
        match e {
            BufferAccessError::InvalidResource(_) => (),
            other => error_buf.init(other, device_id),
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn wgpu_server_device_create_texture(
    global: &Global,
    device_id: id::DeviceId,
    id_in: id::TextureId,
    desc: &wgt::TextureDescriptor<Option<&nsACString>, crate::FfiSlice<wgt::TextureFormat>>,
    mut error_buf: ErrorBuffer,
) {
    let desc = desc.map_label_and_view_formats(|l| wgpu_string(*l), |v| v.as_slice().to_vec());
    let (_, err) = global.device_create_texture(device_id, &desc, Some(id_in));
    if let Some(err) = err {
        error_buf.init(err, device_id);
    }
}

#[no_mangle]
pub extern "C" fn wgpu_server_texture_destroy(global: &Global, id: id::TextureId) {
    // The watch blits into this texture's image, so it retires before `wgpu`
    // is allowed to free it — destroy releases the image exactly like drop.
    global.stop_mmltk_workspace_mirror(id);
    global.texture_destroy(id);
}

#[no_mangle]
pub extern "C" fn wgpu_server_texture_drop(global: &Global, id: id::TextureId) {
    // The watch blits into this texture's image, so it retires before `wgpu`
    // is allowed to free it.
    global.stop_mmltk_workspace_mirror(id);
    global.texture_drop(id);
}

#[no_mangle]
pub extern "C" fn wgpu_server_attach_workspace_source(global: &Global, high: u64, low: u64) {
    let id = mmltk_workspace_channel::SurfaceId { high, low };
    match global.attach_mmltk_workspace_source(id) {
        Ok(()) => {},
        Err(error) => mmltk_workspace_channel::send_failed(id, error.code, error.stride, error.size),
    }
}

#[no_mangle]
pub extern "C" fn wgpu_server_workspace_request(global: &Global, device: id::DeviceId,
    high: u64, low: u64, width: u32, height: u32) {
    let surface = mmltk_workspace_channel::SurfaceId { high, low };
    if let Err(error) = global.request_mmltk_workspace(device, surface, width, height) {
        mmltk_workspace_channel::send_failed(surface, error.code, error.stride, error.size);
    }
}

#[no_mangle]
pub extern "C" fn wgpu_server_workspace_acquire(global: &Global, device_id: id::DeviceId,
    high: u64, low: u64, session: u64, sequence: u64, publication: u64) {
    let Some(identity) = MmltkWorkspaceFrameIdentity::new(0, session, sequence, publication) else { return; };
    global.mmltk_workspace_dispatcher.shared.enqueue(MmltkWorkspaceDispatcherCommand::Acquire {
        device_id, surface_id: mmltk_workspace_channel::SurfaceId { high, low }, identity,
    });
}

#[no_mangle]
pub extern "C" fn wgpu_server_external_texture_slot_release(
    global: &Global,
    surface_id_high: u64,
    surface_id_low: u64,
    layer: u64,
    slot: u32,
    content_session: u64,
    content_sequence: u64,
    presentation_revision: u64,
) {
    let surface_id = mmltk_workspace_channel::SurfaceId {
        high: surface_id_high,
        low: surface_id_low,
    };
    let Some(identity) = MmltkWorkspaceFrameIdentity::new(
        layer,
        content_session,
        content_sequence,
        presentation_revision,
    ) else {
        return;
    };
    global
        .mmltk_workspace_dispatcher
        .shared
        .release_slot(surface_id, identity, slot);
}

#[no_mangle]
pub unsafe extern "C" fn wgpu_server_texture_create_view(
    global: &Global,
    device_id: id::DeviceId,
    texture_id: id::TextureId,
    id_in: id::TextureViewId,
    desc: &crate::TextureViewDescriptor,
    mut error_buf: ErrorBuffer,
) {
    let desc = wgc::resource::TextureViewDescriptor {
        label: wgpu_string(desc.label),
        format: desc.format.cloned(),
        dimension: desc.dimension.cloned(),
        range: wgt::ImageSubresourceRange {
            aspect: desc.aspect,
            base_mip_level: desc.base_mip_level,
            mip_level_count: desc.mip_level_count.map(|ptr| *ptr),
            base_array_layer: desc.base_array_layer,
            array_layer_count: desc.array_layer_count.map(|ptr| *ptr),
        },
        usage: Some(desc.usage),
    };
    let (_, err) = global.texture_create_view(texture_id, &desc, Some(id_in));
    if let Some(err) = err {
        error_buf.init(err, device_id);
    }
}

#[no_mangle]
pub extern "C" fn wgpu_server_texture_view_drop(global: &Global, id: id::TextureViewId) {
    global.texture_view_drop(id);
}

#[derive(Debug)]
#[repr(C)]
pub struct DMABufInfo {
    pub is_valid: bool,
    pub modifier: u64,
    pub plane_count: u32,
    pub offsets: [u64; 3],
    pub strides: [u64; 3],
}

#[derive(Debug)]

pub struct VkImageHandle {
    pub device: vk::Device,
    pub image: vk::Image,
    pub memory: vk::DeviceMemory,
    pub memory_size: u64,
    pub memory_type_index: u32,
    pub modifier: u64,
    pub layouts: Vec<vk::SubresourceLayout>,
}

// Shared canvas textures support rendering and the copies used by WebGPU
// initialization and canvas readback. The exported allocation and its imported
// image must negotiate the same usages.
const SHARED_CANVAS_IMAGE_USAGE: vk::ImageUsageFlags = vk::ImageUsageFlags::from_raw(
    vk::ImageUsageFlags::COLOR_ATTACHMENT.as_raw()
        | vk::ImageUsageFlags::TRANSFER_SRC.as_raw()
        | vk::ImageUsageFlags::TRANSFER_DST.as_raw(),
);

impl VkImageHandle {
    fn destroy(&self, global: &Global, device_id: id::DeviceId) {
        unsafe {
            let Some(hal_device) = global.device_as_hal::<wgc::api::Vulkan>(device_id) else {
                return;
            };

            let device = hal_device.raw_device();

            (device.fp_v1_0().destroy_image)(self.device, self.image, ptr::null());
            (device.fp_v1_0().free_memory)(self.device, self.memory, ptr::null());
        };
    }
}

#[no_mangle]

pub extern "C" fn wgpu_vkimage_create_with_dma_buf(
    global: &Global,
    device_id: id::DeviceId,
    width: u32,
    height: u32,
    out_memory_size: *mut u64,
) -> *mut VkImageHandle {
    unsafe {
        let Some(hal_device) = global.device_as_hal::<wgc::api::Vulkan>(device_id) else {
            emit_critical_invalid_note("Vulkan device");
            return ptr::null_mut();
        };

        let device = hal_device.raw_device();
        let physical_device = hal_device.raw_physical_device();
        let instance = hal_device.shared_instance().raw_instance();

        let count = {
            let mut drm_format_modifier_props_list =
                vk::DrmFormatModifierPropertiesListEXT::default();
            let mut format_properties_2 =
                vk::FormatProperties2::default().push_next(&mut drm_format_modifier_props_list);

            instance.get_physical_device_format_properties2(
                physical_device,
                vk::Format::B8G8R8A8_UNORM,
                &mut format_properties_2,
            );
            drm_format_modifier_props_list.drm_format_modifier_count
        };

        if count == 0 {
            let msg = c"get_physical_device_format_properties2() failed";
            gfx_critical_note(msg.as_ptr());
            return ptr::null_mut();
        }

        let mut modifier_props =
            vec![vk::DrmFormatModifierPropertiesEXT::default(); count as usize];

        let mut drm_format_modifier_props_list = vk::DrmFormatModifierPropertiesListEXT::default()
            .drm_format_modifier_properties(&mut modifier_props);
        let mut format_properties_2 =
            vk::FormatProperties2::default().push_next(&mut drm_format_modifier_props_list);

        instance.get_physical_device_format_properties2(
            physical_device,
            vk::Format::B8G8R8A8_UNORM,
            &mut format_properties_2,
        );

        let usage_flags = SHARED_CANVAS_IMAGE_USAGE;

        modifier_props.retain(|modifier_prop| {
            is_dmabuf_supported(
                instance,
                physical_device,
                vk::Format::B8G8R8A8_UNORM,
                modifier_prop.drm_format_modifier,
                usage_flags,
            )
        });

        if modifier_props.is_empty() {
            let msg = c"format not supported for dmabuf import";
            gfx_critical_note(msg.as_ptr());
            return ptr::null_mut();
        }

        let Some(consumer_modifiers) = get_linux_dmabuf_modifiers() else {
            let msg = c"failed to get consumer dmabuf modifiers";
            gfx_critical_note(msg.as_ptr());
            return ptr::null_mut();
        };

        modifier_props.retain(|modifier_prop| {
            consumer_modifiers.contains(&modifier_prop.drm_format_modifier)
        });

        if modifier_props.is_empty() {
            let msg = c"no common dmabuf modifier found for WebGPU shared-texture swapchain";
            gfx_critical_note(msg.as_ptr());
            return ptr::null_mut();
        }

        let modifiers: Vec<u64> = modifier_props
            .iter()
            .map(|modifier_prop| modifier_prop.drm_format_modifier)
            .collect();

        let mut modifier_list =
            vk::ImageDrmFormatModifierListCreateInfoEXT::default().drm_format_modifiers(&modifiers);

        let extent = vk::Extent3D {
            width,
            height,
            depth: 1,
        };

        let mut external_image_create_info = vk::ExternalMemoryImageCreateInfo::default()
            .handle_types(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT);

        let mut export_memory_alloc_info = vk::ExportMemoryAllocateInfo::default()
            .handle_types(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT);

        let vk_info = vk::ImageCreateInfo::default()
            .flags(vk::ImageCreateFlags::ALIAS)
            .image_type(vk::ImageType::TYPE_2D)
            .format(vk::Format::B8G8R8A8_UNORM)
            .extent(extent)
            .mip_levels(1)
            .array_layers(1)
            .samples(vk::SampleCountFlags::TYPE_1)
            .tiling(vk::ImageTiling::DRM_FORMAT_MODIFIER_EXT)
            .usage(usage_flags)
            .sharing_mode(vk::SharingMode::EXCLUSIVE)
            .initial_layout(vk::ImageLayout::UNDEFINED)
            .push_next(&mut modifier_list)
            .push_next(&mut external_image_create_info);

        let image = match device.create_image(&vk_info, None) {
            Err(err) => {
                let msg = CString::new(format!("create_image() failed: {:?}", err)).unwrap();
                gfx_critical_note(msg.as_ptr());
                return ptr::null_mut();
            }
            Ok(image) => image,
        };

        let mut image_modifier_properties = vk::ImageDrmFormatModifierPropertiesEXT::default();
        let image_drm_format_modifier =
            ash::ext::image_drm_format_modifier::Device::new(instance, device);
        let ret = image_drm_format_modifier
            .get_image_drm_format_modifier_properties(image, &mut image_modifier_properties);
        if ret.is_err() {
            let msg = CString::new(format!(
                "get_image_drm_format_modifier_properties() failed: {:?}",
                ret
            ))
            .unwrap();
            gfx_critical_note(msg.as_ptr());
            return ptr::null_mut();
        }

        let memory_req = device.get_image_memory_requirements(image);

        let mem_properties = instance.get_physical_device_memory_properties(physical_device);

        let index = mem_properties
            .memory_types
            .iter()
            .enumerate()
            .position(|(i, t)| {
                ((1 << i) & memory_req.memory_type_bits) != 0
                    && t.property_flags
                        .contains(vk::MemoryPropertyFlags::DEVICE_LOCAL)
            });

        let Some(index) = index else {
            let msg = c"Failed to get DEVICE_LOCAL memory index";
            gfx_critical_note(msg.as_ptr());
            return ptr::null_mut();
        };

        let mut dedicated_memory_info = vk::MemoryDedicatedAllocateInfo::default().image(image);

        let memory_allocate_info = vk::MemoryAllocateInfo::default()
            .allocation_size(memory_req.size)
            .memory_type_index(index as u32)
            .push_next(&mut dedicated_memory_info)
            .push_next(&mut export_memory_alloc_info);

        let memory = match device.allocate_memory(&memory_allocate_info, None) {
            Err(err) => {
                let msg = CString::new(format!("allocate_memory() failed: {:?}", err)).unwrap();
                gfx_critical_note(msg.as_ptr());
                return ptr::null_mut();
            }
            Ok(memory) => memory,
        };

        let result = device.bind_image_memory(image, memory, 0);
        if result.is_err() {
            let msg = CString::new(format!("bind_image_memory() failed: {:?}", result)).unwrap();
            gfx_critical_note(msg.as_ptr());
            return ptr::null_mut();
        }

        *out_memory_size = memory_req.size;

        let modifier_prop = modifier_props
            .iter()
            .find(|prop| prop.drm_format_modifier == image_modifier_properties.drm_format_modifier);
        let Some(modifier_prop) = modifier_prop else {
            let msg = c"failed to find modifier_prop";
            gfx_critical_note(msg.as_ptr());
            return ptr::null_mut();
        };

        let plane_count = modifier_prop.drm_format_modifier_plane_count;

        let mut layouts = Vec::new();
        for i in 0..plane_count {
            let flag = match i {
                0 => vk::ImageAspectFlags::MEMORY_PLANE_0_EXT,
                1 => vk::ImageAspectFlags::MEMORY_PLANE_1_EXT,
                2 => vk::ImageAspectFlags::MEMORY_PLANE_2_EXT,
                _ => unreachable!(),
            };
            let subresource = vk::ImageSubresource::default().aspect_mask(flag);
            let layout = device.get_image_subresource_layout(image, subresource);
            layouts.push(layout);
        }

        let image_handle = VkImageHandle {
            device: device.handle(),
            image,
            memory,
            memory_size: memory_req.size,
            memory_type_index: index as u32,
            modifier: image_modifier_properties.drm_format_modifier,
            layouts,
        };

        Box::into_raw(Box::new(image_handle))
    }
}

#[no_mangle]

pub unsafe extern "C" fn wgpu_vkimage_destroy(
    global: &Global,
    device_id: id::DeviceId,
    handle: &VkImageHandle,
) {
    handle.destroy(global, device_id);
}

#[no_mangle]

pub unsafe extern "C" fn wgpu_vkimage_delete(handle: *mut VkImageHandle) {
    let _ = Box::from_raw(handle);
}

#[no_mangle]

pub extern "C" fn wgpu_vkimage_get_file_descriptor(
    global: &Global,
    device_id: id::DeviceId,
    handle: &VkImageHandle,
) -> i32 {
    unsafe {
        let Some(hal_device) = global.device_as_hal::<wgc::api::Vulkan>(device_id) else {
            emit_critical_invalid_note("Vulkan device");
            return -1;
        };

        let device = hal_device.raw_device();
        let instance = hal_device.shared_instance().raw_instance();

        let get_fd_info = vk::MemoryGetFdInfoKHR::default()
            .memory(handle.memory)
            .handle_type(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT);

        let loader = khr::external_memory_fd::Device::new(instance, device);

        loader.get_memory_fd(&get_fd_info).unwrap_or(-1)
    }
}

#[no_mangle]

pub extern "C" fn wgpu_vkimage_get_dma_buf_info(handle: &VkImageHandle) -> DMABufInfo {
    let mut offsets: [u64; 3] = [0; 3];
    let mut strides: [u64; 3] = [0; 3];
    let plane_count = handle.layouts.len();
    for i in 0..plane_count {
        offsets[i] = handle.layouts[i].offset;
        strides[i] = handle.layouts[i].row_pitch;
    }

    DMABufInfo {
        is_valid: true,
        modifier: handle.modifier,
        plane_count: plane_count as u32,
        offsets,
        strides,
    }
}

extern "C" {
    #[allow(dead_code)]
    fn gfx_critical_note(msg: *const c_char);
    fn wgpu_server_use_shared_texture_for_swap_chain(
        parent: WebGPUParentPtr,
        swap_chain_id: SwapChainId,
    ) -> bool;
    fn wgpu_server_disable_shared_texture_for_swap_chain(
        parent: WebGPUParentPtr,
        swap_chain_id: SwapChainId,
    );

    #[allow(dead_code)]
    fn wgpu_server_ensure_shared_texture_for_swap_chain(
        parent: WebGPUParentPtr,
        swap_chain_id: SwapChainId,
        device_id: id::DeviceId,
        texture_id: id::TextureId,
        width: u32,
        height: u32,
        format: wgt::TextureFormat,
        usage: wgt::TextureUsages,
    ) -> bool;
    fn wgpu_server_ensure_shared_texture_for_readback(
        parent: WebGPUParentPtr,
        swap_chain_id: SwapChainId,
        device_id: id::DeviceId,
        texture_id: id::TextureId,
        width: u32,
        height: u32,
        format: wgt::TextureFormat,
        usage: wgt::TextureUsages,
    );

    #[allow(improper_ctypes)]
    fn wgpu_server_get_vk_image_handle(
        parent: WebGPUParentPtr,
        texture_id: id::TextureId,
    ) -> *const VkImageHandle;

    fn wgpu_server_get_dma_buf_fd(parent: WebGPUParentPtr, id: id::TextureId) -> i32;

    fn wgpu_server_get_linux_dmabuf_modifiers(
        modifiers: *mut *const u64,
        modifier_count: *mut u32,
    ) -> bool;
    fn wgpu_server_remove_shared_texture(parent: WebGPUParentPtr, id: id::TextureId);
    fn wgpu_parent_is_external_texture_enabled() -> bool;
    fn wgpu_parent_external_texture_frame_ready(
        parent: WebGPUParentPtr,
        device_id: id::DeviceId,
        surface_id_high: u64,
        surface_id_low: u64,
        layer: u64,
        slot: u32,
        content_session: u64,
        content_sequence: u64,
        presentation_revision: u64,
        content_width: u32,
        content_height: u32,
        copy_complete: bool,
        source_high: u64,
        source_low: u64,
        direct_sampling: bool,
    );
    fn wgpu_parent_workspace_source_requested(parent: WebGPUParentPtr, source_high: u64, source_low: u64);
    fn wgpu_parent_workspace_request_ready(parent: WebGPUParentPtr, device: id::DeviceId,
        high: u64, low: u64, width: u32, height: u32);
    fn wgpu_parent_external_texture_import_ready(
        parent: WebGPUParentPtr,
        device_id: id::DeviceId,
        surface_id_high: u64,
        surface_id_low: u64,
    );
    fn wgpu_parent_external_texture_source_get_external_texture_descriptor<'a>(
        parent: WebGPUParentPtr,
        id: crate::ExternalTextureSourceId,
        dest_color_space: crate::PredefinedColorSpace,
    ) -> crate::ExternalTextureDescriptorFromSource<'a>;
    fn wgpu_parent_destroy_external_texture_source(
        parent: WebGPUParentPtr,
        id: crate::ExternalTextureSourceId,
    );
    fn wgpu_parent_drop_external_texture_source(
        parent: WebGPUParentPtr,
        id: crate::ExternalTextureSourceId,
    );
    fn wgpu_server_dealloc_buffer_shmem(parent: WebGPUParentPtr, id: id::BufferId);
    fn wgpu_server_pre_device_drop(parent: WebGPUParentPtr, id: id::DeviceId);
    fn wgpu_server_set_buffer_map_data(
        parent: WebGPUParentPtr,
        device_id: id::DeviceId,
        buffer_id: id::BufferId,
        has_map_flags: bool,
        mapped_offset: u64,
        mapped_size: u64,
        shmem_index: usize,
    );
    fn wgpu_server_device_push_error_scope(
        parent: WebGPUParentPtr,
        device_id: id::DeviceId,
        filter: u8,
    );
    fn wgpu_server_device_pop_error_scope(
        parent: WebGPUParentPtr,
        device_id: id::DeviceId,
        out_type: *mut u8,
        out_message: *mut nsCString,
    );
    fn wgpu_parent_buffer_unmap(
        parent: WebGPUParentPtr,
        device_id: id::DeviceId,
        buffer_id: id::BufferId,
        flush: bool,
    );
    fn wgpu_parent_queue_submit(
        parent: WebGPUParentPtr,
        device_id: id::DeviceId,
        queue_id: id::QueueId,
        command_buffer_ids: *const id::CommandBufferId,
        command_buffer_ids_length: usize,
        texture_ids: *const id::TextureId,
        texture_ids_length: usize,
        external_texture_source_ids: *const crate::ExternalTextureSourceId,
        external_texture_source_ids_length: usize,
    );
    fn wgpu_parent_create_swap_chain(
        parent: WebGPUParentPtr,
        device_id: id::DeviceId,
        queue_id: id::QueueId,
        width: i32,
        height: i32,
        format: crate::SurfaceFormat,
        buffer_ids: *const id::BufferId,
        buffer_ids_length: usize,
        remote_texture_owner_id: crate::RemoteTextureOwnerId,
        use_shared_texture_in_swap_chain: bool,
    );
    fn wgpu_parent_swap_chain_present(
        parent: WebGPUParentPtr,
        texture_id: id::TextureId,
        command_encoder_id: id::CommandEncoderId,
        command_buffer_id: id::CommandBufferId,
        remote_texture_id: crate::RemoteTextureId,
        remote_texture_owner_id: crate::RemoteTextureOwnerId,
    );
    fn wgpu_parent_swap_chain_drop(
        parent: WebGPUParentPtr,
        remote_texture_owner_id: crate::RemoteTextureOwnerId,
        txn_type: crate::RemoteTextureTxnType,
        txn_id: crate::RemoteTextureTxnId,
    );
    fn wgpu_parent_post_request_device(parent: WebGPUParentPtr, device_id: id::DeviceId);
    fn wgpu_parent_build_buffer_map_closure(
        parent: WebGPUParentPtr,
        device_id: id::DeviceId,
        buffer_id: id::BufferId,
        mode: wgc::device::HostMap,
        offset: u64,
        size: u64,
    ) -> BufferMapClosure;
    fn wgpu_parent_build_submitted_work_done_closure(
        parent: WebGPUParentPtr,
        queue_id: id::QueueId,
    ) -> SubmittedWorkDoneClosure;
    fn wgpu_parent_handle_error(
        parent: WebGPUParentPtr,
        device_id: id::DeviceId,
        ty: ErrorBufferType,
        message: &nsCString,
    );
    fn wgpu_parent_send_server_message(parent: WebGPUParentPtr, message: &mut ByteBuf);
    fn wgpu_texture_format_is_valid_for_webidl(format: *const nsCString) -> bool;
}

pub unsafe fn is_dmabuf_supported(
    instance: &ash::Instance,
    physical_device: vk::PhysicalDevice,
    format: vk::Format,
    modifier: u64,
    usage: vk::ImageUsageFlags,
) -> bool {
    let mut drm_props = vk::ExternalImageFormatProperties::default();
    let mut props = vk::ImageFormatProperties2::default().push_next(&mut drm_props);

    let mut modifier_info =
        vk::PhysicalDeviceImageDrmFormatModifierInfoEXT::default().drm_format_modifier(modifier);

    let mut external_format_info = vk::PhysicalDeviceExternalImageFormatInfo::default()
        .handle_type(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT);

    let format_info = vk::PhysicalDeviceImageFormatInfo2::default()
        .format(format)
        .ty(vk::ImageType::TYPE_2D)
        .usage(usage)
        .tiling(vk::ImageTiling::DRM_FORMAT_MODIFIER_EXT)
        .push_next(&mut external_format_info)
        .push_next(&mut modifier_info);

    match instance.get_physical_device_image_format_properties2(
        physical_device,
        &format_info,
        &mut props,
    ) {
        Ok(_) => (),
        Err(_) => {
            return false;
        }
    }

    drm_props
        .external_memory_properties
        .compatible_handle_types
        .contains(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT)
        && drm_props
            .external_memory_properties
            .external_memory_features
            .contains(vk::ExternalMemoryFeatureFlags::IMPORTABLE)
}

pub fn select_memory_type(
    props: &vk::PhysicalDeviceMemoryProperties,
    flags: vk::MemoryPropertyFlags,
    memory_type_bits: Option<u32>,
) -> Option<u32> {
    for i in 0..props.memory_type_count {
        if let Some(mask) = memory_type_bits {
            if mask & (1 << i) == 0 {
                continue;
            }
        }

        if flags.is_empty()
            || props.memory_types[i as usize]
                .property_flags
                .contains(flags)
        {
            return Some(i);
        }
    }

    None
}

/// Why an import produced no texture, in the shape the host channel reports.
/// `stride` and `size` are meaningful only for the layout-mismatch code, where
/// they describe what this device would have accepted.
struct MmltkWorkspaceImportError {
    code: u32,
    stride: u64,
    size: u64,
}

impl MmltkWorkspaceImportError {
    fn code(code: u32) -> Self {
        Self {
            code,
            stride: 0,
            size: 0,
        }
    }
}

const MMLTK_WORKSPACE_DISPATCH_TIMEOUT: Duration = Duration::from_secs(6);
const MMLTK_WORKSPACE_QUEUE_TIMEOUT_NS: u64 = 6_000_000_000;

fn submit_mmltk_queue_and_wait(
    device: &ash::Device,
    queue: vk::Queue,
    queue_gate: &Arc<Mutex<()>>,
    command_buffers: &[vk::CommandBuffer],
) -> Result<(), vk::Result> {
    let fence = unsafe { device.create_fence(&vk::FenceCreateInfo::default(), None) }?;
    let submission = [vk::SubmitInfo::default().command_buffers(command_buffers)];
    let submit_result = {
        let _serialized = queue_gate
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        unsafe { device.queue_submit(queue, &submission, fence) }
    };
    if let Err(error) = submit_result {
        unsafe { device.destroy_fence(fence, None) };
        return Err(error);
    }
    if unsafe { device.wait_for_fences(&[fence], true, MMLTK_WORKSPACE_QUEUE_TIMEOUT_NS) }.is_err()
    {
        // The fence and any resources protected by it may still be in use. A
        // bounded process exit is the only safe terminal; the host owns
        // peer-loss recovery for its imported allocation.
        log::error!("WebGPU queue boundary did not complete within the safety deadline");
        std::process::abort();
    }
    unsafe { device.destroy_fence(fence, None) };
    Ok(())
}

/// A registered private texture. All frame edges are consumed by the one
/// bridge dispatcher; this handle only provides synchronous retirement before
/// `wgpu` can destroy the destination image.
struct MmltkWorkspaceImage {
    device: ash::Device,
    image: vk::Image,
    memory: vk::DeviceMemory,
    direct_memory: Option<Box<MmltkWorkspaceDirectMemoryFacts>>,
    _device_custody: Arc<dyn std::any::Any + Send + Sync>,
}

impl Drop for MmltkWorkspaceImage {
    fn drop(&mut self) {
        unsafe {
            self.device.destroy_image(self.image, None);
            if self.memory != vk::DeviceMemory::null() {
                self.device.free_memory(self.memory, None);
            }
        }
    }
}

// Opt-in facts of the exact source allocation. A buffer alias cannot provide
// defined content after this image's external ownership acquire, even in GENERAL.
// Keep the reason and allocation context without creating or reading an alias.
struct MmltkWorkspaceDirectMemoryFacts {
    layout: mmltk_workspace_channel::Record,
    memory_type_index: u32,
}

impl MmltkWorkspaceDirectMemoryFacts {
    fn new(image: vk::Image, memory: vk::DeviceMemory, memory_type_index: u32,
           layout: mmltk_workspace_channel::Record) -> Self {
        let facts = Self { layout, memory_type_index };
        mmltk_workspace_channel::write_diagnostic(|line| {
            let source = mmltk_workspace_channel::SurfaceId { high: layout.id_high, low: layout.id_low };
            let surface = mmltk_workspace_channel::SurfaceId { high: layout.arena_high, low: layout.arena_low };
            write!(line, "{{\"event\":\"firefox.workspace.direct_memory_alias\",\"source\":\"{source}\",\"surface\":\"{surface}\",\"reason\":\"{}\"",
                facts.reason())?;
            facts.write_fields(line, image, memory)?;
            write!(line, "}}")
        });
        facts
    }

    fn reason(&self) -> &'static str {
        if self.layout.dedicated != 0 {
            "dedicated_image_allocation"
        } else {
            "image_ownership_transfer_invalidates_buffer_alias"
        }
    }

    fn write_fields(&self, line: &mut String, image: vk::Image, memory: vk::DeviceMemory) -> std::fmt::Result {
        write!(line, ",\"workspace_allocation\":{},\"vk_image\":\"{image:?}\",\"vk_memory\":\"{memory:?}\",\"memory_alias_supported\":false,\"memory_type_index\":{},\"dedicated\":{},\"memory_size\":{},\"image_offset\":{},\"row_pitch\":{},\"capacity_width\":{},\"capacity_height\":{}",
            self.layout.allocation_identity, self.memory_type_index,
            self.layout.dedicated, self.layout.size, self.layout.offset, self.layout.stride,
            self.layout.width, self.layout.height)
    }
}

struct MmltkWorkspaceArena {
    device_id: id::DeviceId,
    texture_id: Mutex<Option<id::TextureId>>,
    surface_id: mmltk_workspace_channel::SurfaceId,
    destination: vk::Image,
    storage: Option<Arc<MmltkWorkspaceImage>>,
    initial_source: Mutex<Option<MmltkWorkspaceImage>>,
    device: ash::Device,
    queue: vk::Queue,
    queue_family: u32,
    queue_gate: Arc<Mutex<()>>,
    ready: AtomicBool,
    closed: AtomicBool,
    extent: vk::Extent3D,
    layout: mmltk_workspace_channel::Record,
    mailboxes: Mutex<MmltkWorkspaceMailboxes>,
}

struct MmltkWorkspaceMirror {
    arena: Arc<MmltkWorkspaceArena>,
    dispatcher: Arc<MmltkWorkspaceDispatcherShared>,
    active: bool,
}

impl MmltkWorkspaceMirror {
    fn stop(&mut self) {
        if !self.active {
            return;
        }
        self.active = false;
        mmltk_workspace_channel::trace_state("mirror_stop", self.arena.surface_id, "texture_drop", false);
        self.dispatcher.enqueue(MmltkWorkspaceDispatcherCommand::DetachDestination {
            surface_id: self.arena.surface_id,
        });
    }
}

impl Drop for MmltkWorkspaceMirror {
    fn drop(&mut self) {
        self.stop();
    }
}

fn write_mmltk_workspace_eventfd(fd: RawFd) {
    let edge: u64 = 1;
    loop {
        let written = unsafe {
            libc::write(
                fd,
                &edge as *const u64 as *const libc::c_void,
                mem::size_of::<u64>(),
            )
        };
        if written >= 0 || std::io::Error::last_os_error().raw_os_error() != Some(libc::EINTR) {
            return;
        }
    }
}

fn read_mmltk_workspace_eventfd(fd: RawFd) -> u64 {
    let mut edges: u64 = 0;
    loop {
        let read = unsafe {
            libc::read(
                fd,
                &mut edges as *mut u64 as *mut libc::c_void,
                mem::size_of::<u64>(),
            )
        };
        if read == mem::size_of::<u64>() as isize {
            return edges;
        }
        match std::io::Error::last_os_error().raw_os_error() {
            Some(libc::EINTR) => continue,
            Some(libc::EAGAIN) => return 0,
            _ => return 0,
        }
    }
}

fn valid_mmltk_workspace_eventfd(fd: RawFd) -> bool {
    let flags = unsafe { libc::fcntl(fd, libc::F_GETFL) };
    flags >= 0 && flags & libc::O_NONBLOCK != 0
}

const MMLTK_WORKSPACE_LAYER_COUNT: usize = 1;
const MMLTK_WORKSPACE_MAILBOX_SLOTS: usize = 2;
const MMLTK_WORKSPACE_MAILBOX_COUNT: usize =
    MMLTK_WORKSPACE_LAYER_COUNT * MMLTK_WORKSPACE_MAILBOX_SLOTS;

enum MmltkWorkspaceDispatcherCommand {
    RegisterArena { arena: Arc<MmltkWorkspaceArena> },
    Acquire {
        device_id: id::DeviceId,
        surface_id: mmltk_workspace_channel::SurfaceId,
        identity: MmltkWorkspaceFrameIdentity,
    },
    Register {
        arena: Arc<MmltkWorkspaceArena>,
        device_id: id::DeviceId,
        surface_id: mmltk_workspace_channel::SurfaceId,
        frame: OwnedFd,
        frame_signal: MmltkWorkspaceFrameSignal,
        blit: MmltkWorkspaceBlit,
        allocation: mmltk_workspace_channel::Allocation,
        timeline_descriptor: OwnedFd,
    },
    DetachDestination { surface_id: mmltk_workspace_channel::SurfaceId },
    ReleaseSlot {
        surface_id: mmltk_workspace_channel::SurfaceId,
        identity: MmltkWorkspaceFrameIdentity,
        slot: u32,
    },
    RetireDevice { device_id: id::DeviceId },
    RetireAll,
    Shutdown { response: mpsc::SyncSender<()> },
}

use mmltk_workspace_channel::graphics_abi::WorkspaceFrameSignal as MmltkWorkspaceFrameSignalLayout;

struct MmltkWorkspaceFrameSignal {
    mapping: *mut MmltkWorkspaceFrameSignalLayout,
    access: *mut mmltk_workspace_channel::graphics_abi::ImageWorkspaceAccessSignal,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct MmltkWorkspaceFrameSnapshot {
    timeline_ready: u64,
    transfer_sequence: u64,
    layer: u64,
    content_session: u64,
    content_sequence: u64,
    presentation_revision: u64,
    content_width: u32,
    content_height: u32,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct MmltkWorkspaceFrameIdentity {
    layer: u32,
    session: u64,
    sequence: u64,
    presentation_revision: u64,
}

impl MmltkWorkspaceFrameIdentity {
    fn new(layer: u64, session: u64, sequence: u64, presentation_revision: u64) -> Option<Self> {
        (layer < MMLTK_WORKSPACE_LAYER_COUNT as u64
            && (session != 0 || sequence != 0)
            && presentation_revision != 0)
            .then_some(Self {
                layer: layer as u32,
                session,
                sequence,
                presentation_revision,
            })
    }

    fn copy_index(self, slot: u32) -> Option<usize> {
        (slot < MMLTK_WORKSPACE_MAILBOX_SLOTS as u32)
            .then_some(self.layer as usize * MMLTK_WORKSPACE_MAILBOX_SLOTS + slot as usize)
    }
}

#[derive(Clone, Copy)]
struct MmltkWorkspacePixelReceipt {
    snapshot: MmltkWorkspaceFrameSnapshot,
    coordinates: [(u32, u32); 25],
    copy_index: usize,
    release: u64,
}

#[derive(Clone, Copy)]
struct MmltkWorkspaceMailboxReceipt {
    identity: MmltkWorkspaceFrameIdentity,
    pixels: Option<MmltkWorkspacePixelReceipt>,
}

struct MmltkWorkspacePhysicalReceipt {
    access: u64,
    slot: u32,
    snapshot: MmltkWorkspaceFrameSnapshot,
    frame: MmltkWorkspaceMailboxReceipt,
}

#[derive(Default)]
struct MmltkWorkspaceMailboxes {
    occupied: [[Option<MmltkWorkspaceMailboxReceipt>; MMLTK_WORKSPACE_MAILBOX_SLOTS];
        MMLTK_WORKSPACE_LAYER_COUNT],
    next: [usize; MMLTK_WORKSPACE_LAYER_COUNT],
    latest_presented_revision: [u64; MMLTK_WORKSPACE_LAYER_COUNT],
    retry_pending: [Option<MmltkWorkspaceFrameIdentity>; MMLTK_WORKSPACE_LAYER_COUNT],
}

impl MmltkWorkspaceMailboxes {
    fn already_presented(&self, identity: MmltkWorkspaceFrameIdentity) -> bool {
        identity.presentation_revision <= self.latest_presented_revision[identity.layer as usize]
    }

    fn writable_slot(&self, identity: MmltkWorkspaceFrameIdentity) -> Option<u32> {
        if self.already_presented(identity) {
            return None;
        }
        let layer = identity.layer as usize;
        let start = self.next[layer];
        (0..MMLTK_WORKSPACE_MAILBOX_SLOTS)
            .map(|offset| (start + offset) % MMLTK_WORKSPACE_MAILBOX_SLOTS)
            .find(|slot| self.occupied[layer][*slot].is_none())
            .map(|slot| slot as u32)
    }

    fn occupy(&mut self, receipt: MmltkWorkspaceMailboxReceipt, slot: u32) -> bool {
        let identity = receipt.identity;
        let layer = identity.layer as usize;
        let slot = slot as usize;
        if self.already_presented(identity) || self.occupied[layer][slot].is_some() {
            return false;
        }
        self.occupied[layer][slot] = Some(receipt);
        // Transfer retries can arrive after a page release. Publication revisions
        // remain monotonic for this surface even when no slot is occupied.
        self.latest_presented_revision[layer] = identity.presentation_revision;
        self.next[layer] = (slot + 1) % MMLTK_WORKSPACE_MAILBOX_SLOTS;
        true
    }

    fn release(&mut self, identity: MmltkWorkspaceFrameIdentity, slot: u32) -> Option<MmltkWorkspaceMailboxReceipt> {
        if slot >= MMLTK_WORKSPACE_MAILBOX_SLOTS as u32 {
            return None;
        }
        let occupied = &mut self.occupied[identity.layer as usize][slot as usize];
        if occupied.as_ref().map(|receipt| receipt.identity) != Some(identity) {
            return None;
        }
        occupied.take()
    }

    fn note_capacity_exhausted(&mut self, identity: MmltkWorkspaceFrameIdentity) -> bool {
        let layer = identity.layer as usize;
        if self.already_presented(identity) {
            return false;
        }
        self.retry_pending[layer] = Some(identity);
        true
    }

    fn take_retry_after_release(&mut self, released: MmltkWorkspaceFrameIdentity) -> bool {
        let layer = released.layer as usize;
        self.retry_pending[layer]
            .take()
            .is_some_and(|pending| !self.already_presented(pending))
    }

    fn drain(&mut self, mut release: impl FnMut(MmltkWorkspaceFrameIdentity, u32)) {
        for (layer, slots) in self.occupied.iter_mut().enumerate() {
            for (slot, occupied) in slots.iter_mut().enumerate() {
                if let Some(receipt) = occupied.take() {
                    let identity = receipt.identity;
                    debug_assert_eq!(identity.layer as usize, layer);
                    release(identity, slot as u32);
                }
            }
        }
        self.next.fill(0);
        self.latest_presented_revision.fill(0);
        self.retry_pending.fill(None);
    }
}

static MMLTK_WORKSPACE_ACCEPTANCE_TRACE: OnceLock<bool> = OnceLock::new();

fn mmltk_workspace_acceptance_trace_enabled() -> bool {
    MMLTK_WORKSPACE_ACCEPTANCE_TRACE.get().copied().unwrap_or(false)
}

impl MmltkWorkspaceFrameSnapshot {
    fn identity(self) -> Option<MmltkWorkspaceFrameIdentity> {
        MmltkWorkspaceFrameIdentity::new(
            self.layer,
            self.content_session,
            self.content_sequence,
            self.presentation_revision,
        )
    }
}

unsafe impl Send for MmltkWorkspaceFrameSignal {}

impl MmltkWorkspaceFrameSignal {
    fn map_shared<T>(descriptor: OwnedFd) -> Option<*mut T> {
        let mut status = mem::MaybeUninit::<libc::stat>::uninit();
        if unsafe { libc::fstat(descriptor.as_raw_fd(), status.as_mut_ptr()) } != 0 {
            return None;
        }
        let status = unsafe { status.assume_init() };
        let required_seals = libc::F_SEAL_GROW | libc::F_SEAL_SHRINK | libc::F_SEAL_SEAL;
        let seals = unsafe { libc::fcntl(descriptor.as_raw_fd(), libc::F_GET_SEALS) };
        if status.st_size != mem::size_of::<T>() as libc::off_t
            || status.st_mode & libc::S_IFMT != libc::S_IFREG
            || seals < 0
            || seals & required_seals != required_seals
        {
            return None;
        }
        let mapping = unsafe {
            libc::mmap(
                ptr::null_mut(),
                mem::size_of::<T>(),
                // Atomic references require writable mapping validity; this reader only loads.
                libc::PROT_READ | libc::PROT_WRITE,
                libc::MAP_SHARED,
                descriptor.as_raw_fd(),
                0,
            )
        };
        if mapping == libc::MAP_FAILED {
            return None;
        }
        Some(mapping.cast())
    }
    fn map(descriptor: OwnedFd, access: OwnedFd, allocation: u64) -> Option<Self> {
        let mapping = Self::map_shared::<MmltkWorkspaceFrameSignalLayout>(descriptor)?;
        let Some(access) = Self::map_shared::<mmltk_workspace_channel::graphics_abi::ImageWorkspaceAccessSignal>(access) else {
            unsafe { libc::munmap(mapping.cast(), mem::size_of::<MmltkWorkspaceFrameSignalLayout>()) };
            return None;
        };
        let result = Self { mapping, access };
        (unsafe { (*access).allocation_identity } == allocation && allocation != 0).then_some(result)
    }
    fn acquire(&self, snapshot: MmltkWorkspaceFrameSnapshot) -> Option<u64> {
        use mmltk_workspace_channel::graphics_abi::{
            WORKSPACE_ACCESS_AVAILABLE, WORKSPACE_ACCESS_READING, WORKSPACE_ACCESS_MASK, WORKSPACE_ACCESS_REVOKED,
        };
        let access = unsafe { AtomicU64::from_ptr(ptr::addr_of_mut!((*self.access).access)) };
        let expected = access.load(Ordering::Acquire);
        if expected & WORKSPACE_ACCESS_MASK != WORKSPACE_ACCESS_AVAILABLE || expected & WORKSPACE_ACCESS_REVOKED != 0 {
            return None;
        }
        let generation = unsafe { AtomicU64::from_ptr(ptr::addr_of_mut!((*self.access).generation)).load(Ordering::Acquire) };
        let offered = unsafe { AtomicU64::from_ptr(ptr::addr_of_mut!((*self.mapping).physical_revision)).load(Ordering::SeqCst) };
        if generation == 0 || generation != offered || self.read() != Some(snapshot) {
            return None;
        }
        let reading = (expected & !WORKSPACE_ACCESS_MASK) | WORKSPACE_ACCESS_READING;
        access.compare_exchange(expected, reading, Ordering::AcqRel, Ordering::Acquire).ok().map(|_| reading)
    }

    fn complete_terminal_read(&self, reading: u64) {
        use mmltk_workspace_channel::graphics_abi::WORKSPACE_ACCESS_REVOKED;
        let access = unsafe { AtomicU64::from_ptr(ptr::addr_of_mut!((*self.access).access)) };
        let revoked = reading | WORKSPACE_ACCESS_REVOKED;
        // This is called only after the physical owner's terminal GPU wait.
        // Never let an earlier completion receipt authorize another read, even
        // when the product generation itself did not advance.
        if access.compare_exchange(reading, revoked, Ordering::AcqRel, Ordering::Acquire)
            .is_err_and(|actual| actual != revoked) {
            return;
        }
        unsafe { AtomicU64::from_ptr(ptr::addr_of_mut!((*self.access).terminal_read_complete))
            .store(revoked, Ordering::Release) };
    }
    fn abandon_acquisition(&self) {
        use mmltk_workspace_channel::graphics_abi::{
            WORKSPACE_ACCESS_MASK, WORKSPACE_ACCESS_AVAILABLE, WORKSPACE_ACCESS_EMPTY, WORKSPACE_ACCESS_REVOKED,
        };
        let access = unsafe { AtomicU64::from_ptr(ptr::addr_of_mut!((*self.access).access)) };
        // Native withdrawal can race a failed GPU submission. Preserve its
        // sticky revocation while returning the never-submitted physical read.
        let _ = access.fetch_update(Ordering::AcqRel, Ordering::Acquire, |value| {
            let role = if value & WORKSPACE_ACCESS_REVOKED != 0 {
                WORKSPACE_ACCESS_EMPTY
            } else { WORKSPACE_ACCESS_AVAILABLE };
            Some((value & !WORKSPACE_ACCESS_MASK) | role)
        });
    }

    fn read(&self) -> Option<MmltkWorkspaceFrameSnapshot> {
        for _ in 0..4 {
            let begin = unsafe { AtomicU64::from_ptr(ptr::addr_of_mut!((*self.mapping).sequence_lock)).load(Ordering::SeqCst) };
            if begin & 1 != 0 {
                continue;
            }
            let timeline_ready = unsafe { AtomicU64::from_ptr(ptr::addr_of_mut!((*self.mapping).timeline_ready)).load(Ordering::SeqCst) };
            let transfer_sequence = unsafe { AtomicU64::from_ptr(ptr::addr_of_mut!((*self.mapping).transfer_sequence)).load(Ordering::SeqCst) };
            let layer = unsafe { AtomicU64::from_ptr(ptr::addr_of_mut!((*self.mapping).layer)).load(Ordering::SeqCst) };
            let content_session = unsafe { AtomicU64::from_ptr(ptr::addr_of_mut!((*self.mapping).content_session)).load(Ordering::SeqCst) };
            let content_sequence = unsafe { AtomicU64::from_ptr(ptr::addr_of_mut!((*self.mapping).content_sequence)).load(Ordering::SeqCst) };
            let presentation_revision = unsafe { AtomicU64::from_ptr(ptr::addr_of_mut!((*self.mapping).presentation_revision)).load(Ordering::SeqCst) };
            let content_width = unsafe { AtomicU32::from_ptr(ptr::addr_of_mut!((*self.mapping).content_width)).load(Ordering::SeqCst) };
            let content_height = unsafe { AtomicU32::from_ptr(ptr::addr_of_mut!((*self.mapping).content_height)).load(Ordering::SeqCst) };
            let end = unsafe { AtomicU64::from_ptr(ptr::addr_of_mut!((*self.mapping).sequence_lock)).load(Ordering::SeqCst) };
            if begin == end
                && end & 1 == 0
                && timeline_ready & 1 != 0
                && transfer_sequence != 0
                && content_width != 0
                && content_height != 0
                && (layer < MMLTK_WORKSPACE_LAYER_COUNT as u64 || layer == u64::MAX)
            {
                return Some(MmltkWorkspaceFrameSnapshot {
                    timeline_ready,
                    transfer_sequence,
                    layer,
                    content_session,
                    content_sequence,
                    presentation_revision,
                    content_width,
                    content_height,
                });
            }
        }
        None
    }
}

impl Drop for MmltkWorkspaceFrameSignal {
    fn drop(&mut self) {
        unsafe {
            libc::munmap(
                self.mapping.cast(),
                mem::size_of::<MmltkWorkspaceFrameSignalLayout>(),
            )
        };
        unsafe { libc::munmap(self.access.cast(),
            mem::size_of::<mmltk_workspace_channel::graphics_abi::ImageWorkspaceAccessSignal>()) };
    }
}

#[cfg(test)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum MmltkWorkspaceTransferPlan {
    ReleaseOnly {
        allow_preconsumed: bool,
        terminal_on_settlement: bool,
    },
    Current {
        snapshot: MmltkWorkspaceFrameSnapshot,
        identity: MmltkWorkspaceFrameIdentity,
    },
}

#[cfg(test)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
struct MmltkWorkspaceSettledTransfer {
    ready: u64,
    release: u64,
    copied_slot: Option<usize>,
}

#[cfg(test)]
fn expected_mmltk_workspace_ready(next_transfer_sequence: u64) -> Result<u64, vk::Result> {
    next_transfer_sequence
        .checked_mul(2)
        .and_then(|value| value.checked_sub(1))
        .ok_or(vk::Result::ERROR_UNKNOWN)
}

#[cfg(test)]
fn plan_mmltk_workspace_transfer(
    next_transfer_sequence: u64,
    snapshot: Option<MmltkWorkspaceFrameSnapshot>,
) -> Result<MmltkWorkspaceTransferPlan, vk::Result> {
    let Some(snapshot) = snapshot else {
        return Ok(MmltkWorkspaceTransferPlan::ReleaseOnly {
            allow_preconsumed: false,
            terminal_on_settlement: true,
        });
    };
    if snapshot.transfer_sequence < next_transfer_sequence {
        return Ok(MmltkWorkspaceTransferPlan::ReleaseOnly {
            allow_preconsumed: true,
            terminal_on_settlement: true,
        });
    }
    if snapshot.transfer_sequence != next_transfer_sequence
        || snapshot.timeline_ready != expected_mmltk_workspace_ready(next_transfer_sequence)?
    {
        return Ok(MmltkWorkspaceTransferPlan::ReleaseOnly {
            allow_preconsumed: false,
            terminal_on_settlement: true,
        });
    }
    let Some(identity) = snapshot.identity() else {
        return Ok(MmltkWorkspaceTransferPlan::ReleaseOnly {
            allow_preconsumed: false,
            terminal_on_settlement: false,
        });
    };
    Ok(MmltkWorkspaceTransferPlan::Current { snapshot, identity })
}

#[cfg(test)]
fn settle_mmltk_workspace_cursor(
    next_transfer_sequence: &mut u64,
    submitted: Option<(u64, Option<usize>)>,
    expected_copy: Option<usize>,
    allow_preconsumed: bool,
) -> Result<Option<MmltkWorkspaceSettledTransfer>, vk::Result> {
    let Some((ready, copied_slot)) = submitted else {
        return if allow_preconsumed {
            Ok(None)
        } else {
            Err(vk::Result::ERROR_UNKNOWN)
        };
    };
    if ready != expected_mmltk_workspace_ready(*next_transfer_sequence)?
        || copied_slot != expected_copy
    {
        return Err(vk::Result::ERROR_UNKNOWN);
    }
    let release = ready.checked_add(1).ok_or(vk::Result::ERROR_UNKNOWN)?;
    *next_transfer_sequence = (*next_transfer_sequence)
        .checked_add(1)
        .ok_or(vk::Result::ERROR_UNKNOWN)?;
    Ok(Some(MmltkWorkspaceSettledTransfer {
        ready,
        release,
        copied_slot,
    }))
}

struct MmltkWorkspaceDispatchEntry {
    device_id: id::DeviceId,
    surface_id: mmltk_workspace_channel::SurfaceId,
    frame: OwnedFd,
    frame_signal: MmltkWorkspaceFrameSignal,
    blit: MmltkWorkspaceBlit,
    arena: Arc<MmltkWorkspaceArena>,
    next_transfer_sequence: u64,
    physical_read: Option<MmltkWorkspacePhysicalReceipt>,
}

impl Drop for MmltkWorkspaceDispatchEntry {
    fn drop(&mut self) {
        if let Some(receipt) = self.physical_read.take() {
            // Runs on the existing retirement worker, after its registry locks
            // are released. A queued Acquired is never evidence of GPU safety.
            self.blit.retire();
            self.frame_signal.complete_terminal_read(receipt.access);
            mmltk_workspace_channel::complete_terminal_read(self.surface_id);
        }
    }
}

fn submit_mmltk_workspace_transfer(
    shared: &MmltkWorkspaceDispatcherShared,
    entry: &mut MmltkWorkspaceDispatchEntry,
    snapshot: Option<MmltkWorkspaceFrameSnapshot>,
) -> Result<bool, vk::Result> {
    let Some(snapshot) = snapshot else { return Ok(false); };
    let Some(identity) = snapshot.identity() else { return Ok(false); };
    if snapshot.transfer_sequence < entry.next_transfer_sequence || entry.physical_read.is_some() {
        return Ok(false);
    }
    let arena = entry.arena.clone();
    if arena.closed.load(Ordering::Acquire) { return Ok(false); }
    let mut mailboxes = arena.mailboxes.lock().unwrap();
    let Some(slot) = mailboxes.writable_slot(identity) else {
        mailboxes.note_capacity_exhausted(identity);
        return Ok(false);
    };
    let next_transfer = snapshot.transfer_sequence.checked_add(1).ok_or(vk::Result::ERROR_UNKNOWN)?;
    let release = snapshot.timeline_ready.checked_add(1).ok_or(vk::Result::ERROR_UNKNOWN)?;
    let copy_slot = identity.copy_index(slot);
    let direct = arena.layout.direct_sampling != 0;
    let mut pixels = if direct && entry.blit.pixels.is_some()
        && snapshot.content_width != 0 && snapshot.content_height != 0
        && snapshot.content_width <= entry.blit.extent.width && snapshot.content_height <= entry.blit.extent.height {
        let receipt = MmltkWorkspacePixelReceipt {
            snapshot,
            coordinates: std::array::from_fn(|index| (
                MmltkWorkspacePixels::coordinate(index % 5, snapshot.content_width),
                MmltkWorkspacePixels::coordinate(index / 5, snapshot.content_height),
            )),
            copy_index: 0,
            release: 0,
        };
        match entry.blit.record_direct_probe(&receipt.coordinates) {
            Ok(()) => Some(receipt),
            Err(vk::Result::ERROR_DEVICE_LOST) => return Err(vk::Result::ERROR_DEVICE_LOST),
            Err(_) => None,
        }
    } else if !direct && entry.blit.pixels.is_some() {
        if let Some(slot) = copy_slot {
            let extent = entry.blit.extent;
            if snapshot.content_width == 0 || snapshot.content_height == 0
                || snapshot.content_width > extent.width || snapshot.content_height > extent.height
                || !entry.blit.probe_slot_reusable(slot)? {
                // Only this physical slot misses evidence. Its immutable
                // product command remains available without resetting probes.
                None
            } else {
                let receipt = MmltkWorkspacePixelReceipt {
                    snapshot,
                    coordinates: std::array::from_fn(|index| (
                        MmltkWorkspacePixels::coordinate(index % 5, snapshot.content_width),
                        MmltkWorkspacePixels::coordinate(index / 5, snapshot.content_height),
                    )),
                    copy_index: slot,
                    release: 0,
                };
                // The exact last submitted even release established completion;
                // a diagnostic receipt is never needed for resource reuse.
                let command = entry.blit.pixels.as_ref().unwrap().commands[slot];
                let preparation = MmltkWorkspacePixels::check_failure(MmltkWorkspaceProbeFailure::Reset)
                    .and_then(|()| unsafe { entry.blit.device.reset_command_buffer(
                        command, vk::CommandBufferResetFlags::empty()) })
                    .map_err(|error| (MmltkWorkspaceProbeFailure::Reset, error))
                    .and_then(|()| entry.blit.record_copy(command, entry.blit.destination, extent,
                        slot, Some(&receipt.coordinates)));
                match preparation {
                    Ok(()) => Some(receipt),
                    Err((boundary, error)) => {
                        if error == vk::Result::ERROR_DEVICE_LOST { return Err(error); }
                        MmltkWorkspacePixels::report_failure(arena.surface_id, entry.surface_id, boundary, Some((identity, slot, snapshot)));
                        None
                    }
                }
            }
        } else { None }
    } else { None };
    let command = if direct {
        pixels.as_ref().map_or(entry.blit.copy_commands[0], |_| entry.blit.pixels.as_ref().unwrap().commands[0])
    } else {
        pixels.as_ref().map(|_| entry.blit.pixels.as_ref().unwrap().commands[slot as usize])
            .unwrap_or(entry.blit.copy_commands[slot as usize])
    };
    if !mmltk_workspace_channel::reserve_acquisition(entry.surface_id, identity.session, identity.sequence,
        identity.presentation_revision, snapshot.transfer_sequence) {
        return Ok(false);
    }
    let Some(access) = entry.frame_signal.acquire(snapshot) else {
        mmltk_workspace_channel::cancel_acquisition(entry.surface_id);
        return Ok(false);
    };
    entry.blit.trace_binding("acquire_submit_started", &arena, entry.surface_id, &entry.frame_signal,
        Some(snapshot), None, true);
    let submitted = match entry.blit.submission().acquire_completed(snapshot.timeline_ready, command,
        (!direct).then_some(slot as usize)) {
        Ok(submitted) => submitted,
        Err(error) => {
            entry.blit.trace_binding("acquire_submit_failed", &arena, entry.surface_id, &entry.frame_signal,
                Some(snapshot), None, false);
            entry.frame_signal.abandon_acquisition();
            mmltk_workspace_channel::cancel_acquisition(entry.surface_id);
            return Err(error);
        }
    };
    entry.blit.trace_binding("acquire_submitted", &arena, entry.surface_id, &entry.frame_signal,
        Some(snapshot), None, true);
    entry.next_transfer_sequence = next_transfer;
    if let Some(receipt) = pixels.as_mut() {
        receipt.release = release;
    }
    if let (false, Some(probe), Some(slot)) = (direct, entry.blit.pixels.as_mut(), copy_slot) {
        // Track every use, including a copy whose diagnostic receipt was
        // dropped. A later free-slot admission can recover without polling.
        probe.submitted_release[slot] = release;
    }
    let receipt = MmltkWorkspaceMailboxReceipt { identity, pixels };
    entry.physical_read = Some(MmltkWorkspacePhysicalReceipt { access, slot, snapshot, frame: receipt });
    // Acquisition retains CPU custody. Copy submission also contains the
    // final release; direct sampling retains the read until ReleaseSlot.
    if !mmltk_workspace_channel::submit_acquisition(entry.surface_id) {
        return Err(vk::Result::ERROR_UNKNOWN);
    }
    if !direct && !mmltk_workspace_channel::submit_release(entry.surface_id) {
        return Err(vk::Result::ERROR_UNKNOWN);
    }
    if !mailboxes.occupy(receipt, slot) {
        return Err(vk::Result::ERROR_UNKNOWN);
    }
    mmltk_workspace_channel::send_mailbox_presented(
        arena.surface_id,
        identity.layer,
        slot,
        identity.session,
        identity.sequence,
        identity.presentation_revision,
    );
    if mmltk_workspace_acceptance_trace_enabled() {
        mmltk_workspace_channel::write_diagnostic(|line| {
            write!(line,
            "{{\"event\":\"firefox.workspace.frame_forwarded\",\"source\":\"{}\",\"surface\":\"{}\",\"layer\":{},\"slot\":{},\"content_session\":{},\"content_sequence\":{},\"presentation_revision\":{},\"content_width\":{},\"content_height\":{},\"transfer_sequence\":{},\"timeline_ready\":{},\"timeline_release\":{},\"pixel_probe\":{},\"direct_sampling\":{}",
            entry.surface_id,
            arena.surface_id,
            identity.layer,
            slot,
            identity.session,
            identity.sequence,
            identity.presentation_revision,
            snapshot.content_width,
            snapshot.content_height,
            snapshot.transfer_sequence,
            submitted.ready(),
            submitted.ready() + 1,
            pixels.is_some(), direct)?;
            if let Some(facts) = &entry.blit.storage.direct_memory {
                facts.write_fields(line, entry.blit.storage.image, entry.blit.storage.memory)?;
            }
            write!(line, "}}")
        });
    }
    unsafe {
        wgpu_parent_external_texture_frame_ready(
            shared.owner,
            entry.device_id,
            arena.surface_id.high,
            arena.surface_id.low,
            u64::from(identity.layer),
            slot,
            identity.session,
            identity.sequence,
            identity.presentation_revision,
            snapshot.content_width,
            snapshot.content_height,
            false,
            entry.surface_id.high,
            entry.surface_id.low,
            direct,
        )
    };
    Ok(true)
}

struct MmltkWorkspaceDispatcherShared {
    owner: WebGPUParentPtr,
    wake: OwnedFd,
    commands: Mutex<VecDeque<MmltkWorkspaceDispatcherCommand>>,
    arenas: Mutex<HashMap<mmltk_workspace_channel::SurfaceId, Arc<MmltkWorkspaceArena>>>,
    entries: Mutex<HashMap<RawFd, MmltkWorkspaceDispatchEntry>>,
    pending_requests: Mutex<HashMap<mmltk_workspace_channel::SurfaceId, (id::DeviceId, u32, u32)>>,
}

impl MmltkWorkspaceDispatcherShared {
    fn enqueue(&self, command: MmltkWorkspaceDispatcherCommand) {
        let mut commands = self.commands
            .lock()
            .unwrap_or_else(|_| {
                log::error!("WebGPU workspace dispatcher command queue was poisoned");
                std::process::abort();
            });
        // Three arenas, 24 admitted physical sources, and six page read slots
        // bound ordinary commands. Repeated terminal/release notifications
        // share their existing queued operation.
        if commands.iter().any(|pending| match (pending, &command) {
            (MmltkWorkspaceDispatcherCommand::ReleaseSlot { surface_id: a, identity: ai, slot: aslot },
             MmltkWorkspaceDispatcherCommand::ReleaseSlot { surface_id: b, identity: bi, slot: bslot }) =>
                a == b && ai == bi && aslot == bslot,
            (MmltkWorkspaceDispatcherCommand::Acquire { device_id: a, surface_id: sa, identity: ia },
             MmltkWorkspaceDispatcherCommand::Acquire { device_id: b, surface_id: sb, identity: ib }) =>
                a == b && sa == sb && ia == ib,
            (MmltkWorkspaceDispatcherCommand::RetireDevice { device_id: a },
             MmltkWorkspaceDispatcherCommand::RetireDevice { device_id: b }) => a == b,
            (MmltkWorkspaceDispatcherCommand::DetachDestination { surface_id: a },
             MmltkWorkspaceDispatcherCommand::DetachDestination { surface_id: b }) => a == b,
            (MmltkWorkspaceDispatcherCommand::RetireAll, MmltkWorkspaceDispatcherCommand::RetireAll) => true,
            _ => false,
        }) { return; }
        if commands.len() == 128 {
            // Never drop a GPU retirement owner onto the IPC/render caller or
            // permit unbounded ownership growth after a broken protocol.
            std::process::abort();
        }
        commands.push_back(command);
        drop(commands);
        write_mmltk_workspace_eventfd(self.wake.as_raw_fd());
    }

    fn register_arena(&self, arena: Arc<MmltkWorkspaceArena>) {
        self.enqueue(MmltkWorkspaceDispatcherCommand::RegisterArena { arena });
    }
    fn register(
        self: &Arc<Self>, arena: Arc<MmltkWorkspaceArena>, surface_id: mmltk_workspace_channel::SurfaceId,
        frame: OwnedFd, frame_signal: MmltkWorkspaceFrameSignal, blit: MmltkWorkspaceBlit,
        allocation: mmltk_workspace_channel::Allocation,
        timeline_descriptor: OwnedFd,
    ) {
        self.enqueue(MmltkWorkspaceDispatcherCommand::Register {
            device_id: arena.device_id, arena,
            surface_id, frame, frame_signal, blit, allocation, timeline_descriptor,
        });
    }

    fn release_slot(
        &self,
        surface_id: mmltk_workspace_channel::SurfaceId,
        identity: MmltkWorkspaceFrameIdentity,
        slot: u32,
    ) {
        self.enqueue(MmltkWorkspaceDispatcherCommand::ReleaseSlot {
            surface_id,
            identity,
            slot,
        });
    }

    fn retire_device(&self, device_id: id::DeviceId) {
        self.enqueue(MmltkWorkspaceDispatcherCommand::RetireDevice { device_id });
    }

    fn retire_all(&self) {
        self.enqueue(MmltkWorkspaceDispatcherCommand::RetireAll);
    }

}

struct MmltkWorkspaceDispatcher {
    shared: Arc<MmltkWorkspaceDispatcherShared>,
    thread: Option<thread::JoinHandle<()>>,
}

impl MmltkWorkspaceDispatcher {
    fn new(owner: WebGPUParentPtr) -> Self {
        let wake = unsafe { libc::eventfd(0, libc::EFD_NONBLOCK | libc::EFD_CLOEXEC) };
        if wake < 0 {
            log::error!("WebGPU workspace dispatcher could not create its wake eventfd");
            std::process::abort();
        }
        let wake = unsafe { OwnedFd::from_raw_fd(wake) };
        let channel_wake = wake.try_clone().unwrap_or_else(|_| {
            log::error!("WebGPU workspace dispatcher could not clone its wake eventfd");
            std::process::abort();
        });
        mmltk_workspace_channel::install_dispatch_wake(channel_wake);
        let shared = Arc::new(MmltkWorkspaceDispatcherShared {
            owner,
            wake,
            commands: Mutex::new(VecDeque::new()),
            arenas: Mutex::new(HashMap::new()),
            entries: Mutex::new(HashMap::new()),
            pending_requests: Mutex::new(HashMap::new()),
        });
        let worker = shared.clone();
        let thread = thread::Builder::new()
            .name("WebGPU workspace bridge".into())
            .spawn(move || run_mmltk_workspace_dispatcher(worker))
            .unwrap_or_else(|_| {
                log::error!("WebGPU workspace dispatcher could not start");
                std::process::abort();
            });
        Self {
            shared,
            thread: Some(thread),
        }
    }
}

impl Drop for MmltkWorkspaceDispatcher {
    fn drop(&mut self) {
        let Some(thread) = self.thread.take() else {
            return;
        };
        let (response, result) = mpsc::sync_channel(1);
        self.shared
            .enqueue(MmltkWorkspaceDispatcherCommand::Shutdown { response });
        result
            .recv_timeout(MMLTK_WORKSPACE_DISPATCH_TIMEOUT)
            .unwrap_or_else(|_| {
                log::error!("WebGPU workspace dispatcher shutdown timed out");
                std::process::abort();
            });
        if thread.join().is_err() {
            log::error!("WebGPU workspace dispatcher panicked");
            std::process::abort();
        }
    }
}

fn update_mmltk_workspace_channel_interest(epoll: RawFd, registered: &mut Option<(RawFd, u32)>) {
    let next = mmltk_workspace_channel::poll_state();
    match (*registered, next) {
        (None, None) => {}
        (Some((fd, _)), None) => {
            unsafe { libc::epoll_ctl(epoll, libc::EPOLL_CTL_DEL, fd, ptr::null_mut()) };
            *registered = None;
        }
        (None, Some((fd, events))) | (Some((fd, _)), Some((_, events))) => {
            let operation = if registered.is_some() {
                libc::EPOLL_CTL_MOD
            } else {
                libc::EPOLL_CTL_ADD
            };
            let mut event = libc::epoll_event {
                events,
                u64: fd as u64,
            };
            if unsafe { libc::epoll_ctl(epoll, operation, fd, &mut event) } == 0 {
                *registered = Some((fd, events));
            }
        }
    }
}

fn service_mmltk_workspace_admissions(
    shared: &MmltkWorkspaceDispatcherShared,
    surface_frames: &HashMap<mmltk_workspace_channel::SurfaceId, RawFd>,
    entries: &mut HashMap<RawFd, MmltkWorkspaceDispatchEntry>,
) {
    shared.pending_requests.lock().unwrap().retain(|surface, (device, width, height)| {
        if !mmltk_workspace_channel::arena_admitted(*surface) { return true; }
        unsafe { wgpu_parent_workspace_request_ready(shared.owner, *device, surface.high, surface.low, *width, *height) };
        false
    });
    for source in mmltk_workspace_channel::take_source_requests() {
        unsafe { wgpu_parent_workspace_source_requested(shared.owner, source.high, source.low) };
    }
    for (source, session, sequence, revision, transfer) in mmltk_workspace_channel::take_read_settlements() {
        let Some(entry) = surface_frames.get(&source).and_then(|fd| entries.get_mut(fd)) else {
            mmltk_workspace_channel::fail(); return;
        };
        // The physical source admits one GPU read at a time. Its exact receipt
        // rejects duplicate and mismatched settlement without counting offers.
        let Some(receipt) = entry.physical_read.as_ref() else {
            mmltk_workspace_channel::fail(); return;
        };
        let snapshot = receipt.snapshot;
        if transfer >= entry.next_transfer_sequence || transfer != snapshot.transfer_sequence
            || snapshot.content_session != session || snapshot.content_sequence != sequence
            || snapshot.presentation_revision != revision || receipt.slot >= MMLTK_WORKSPACE_MAILBOX_SLOTS as u32
            || snapshot.identity() != Some(receipt.frame.identity) {
            mmltk_workspace_channel::fail(); return;
        }
        let receipt = entry.physical_read.take().unwrap();
        let slot = receipt.slot;
        let arena = &entry.arena;
        let frame = receipt.frame;
        if let (Some(pixels), Some(probe)) = (frame.pixels, &entry.blit.pixels) {
            let completed = entry.blit.submission().submitted_release_complete(pixels.release);
            if entry.blit.direct {
                probe.report_direct(source, arena.surface_id, frame.identity, slot, pixels, completed, &entry.blit.storage);
            } else {
                probe.report(source, arena.surface_id, frame.identity, slot, pixels, completed);
            }
        }
        unsafe { wgpu_parent_external_texture_frame_ready(shared.owner, arena.device_id,
            arena.surface_id.high, arena.surface_id.low, 0, slot, session, sequence, revision,
            snapshot.content_width, snapshot.content_height, true, entry.surface_id.high, entry.surface_id.low, entry.blit.direct) };
        mmltk_workspace_channel::complete_read_settlement(source);
    }
}

fn retire_settled_mmltk_workspace_releases(
    epoll: RawFd,
    surface_frames: &mut HashMap<mmltk_workspace_channel::SurfaceId, RawFd>,
    entries: &mut HashMap<RawFd, MmltkWorkspaceDispatchEntry>,
    retired: &mut Vec<MmltkWorkspaceDispatchEntry>,
    completed: &mut Vec<mmltk_workspace_channel::SurfaceId>,
) {
    for surface_id in mmltk_workspace_channel::take_settled_releases() {
        if surface_frames.get(&surface_id).and_then(|fd| entries.get(fd))
            .is_some_and(|entry| entry.physical_read.is_some()) {
            mmltk_workspace_channel::fail(); return;
        }
        if let Some(frame_fd) = surface_frames.remove(&surface_id) {
            unsafe { libc::epoll_ctl(epoll, libc::EPOLL_CTL_DEL, frame_fd, ptr::null_mut()) };
            if let Some(entry) = entries.remove(&frame_fd) {
                retired.push(entry);
            }
        }
        completed.push(surface_id);
    }
}

fn run_mmltk_workspace_dispatcher(shared: Arc<MmltkWorkspaceDispatcherShared>) {
    let epoll = unsafe { libc::epoll_create1(libc::EPOLL_CLOEXEC) };
    if epoll < 0 {
        log::error!("WebGPU workspace dispatcher could not create epoll");
        std::process::abort();
    }
    let wake_fd = shared.wake.as_raw_fd();
    let mut wake_event = libc::epoll_event {
        events: libc::EPOLLIN as u32,
        u64: wake_fd as u64,
    };
    if unsafe { libc::epoll_ctl(epoll, libc::EPOLL_CTL_ADD, wake_fd, &mut wake_event) } != 0 {
        log::error!("WebGPU workspace dispatcher could not register its wake eventfd");
        std::process::abort();
    }
    let mut channel = None;
    let mut surface_frames = HashMap::new();
    let mut events = [libc::epoll_event { events: 0, u64: 0 }; 16];
    let mut running = true;
    while running {
        update_mmltk_workspace_channel_interest(epoll, &mut channel);
        let ready = unsafe {
            libc::epoll_wait(epoll, events.as_mut_ptr(), events.len() as libc::c_int, -1)
        };
        if ready < 0 {
            if std::io::Error::last_os_error().raw_os_error() == Some(libc::EINTR) {
                continue;
            }
            log::error!("WebGPU workspace dispatcher lost epoll");
            std::process::abort();
        }
        let mut retired_blits = Vec::new();
        let mut retired_entries = Vec::new();
        let mut completed_retirements = Vec::new();
        let mut close_channel = false;
        let mut shutdown = None;
        let mut arenas = shared.arenas.lock().unwrap();
        let mut entries = shared.entries.lock().unwrap();
        for event in &events[..ready as usize] {
            let fd = event.u64 as RawFd;
            if fd == wake_fd {
                let _ = read_mmltk_workspace_eventfd(fd);
                mmltk_workspace_channel::service(libc::EPOLLIN as u32);
                service_mmltk_workspace_admissions(&shared, &surface_frames, &mut entries);
                let commands: Vec<_> = shared
                    .commands
                    .lock()
                    .unwrap_or_else(|_| {
                        log::error!("WebGPU workspace dispatcher command queue was poisoned");
                        std::process::abort();
                    })
                    .drain(..)
                    .collect();
                for command in commands {
                    match command {
                        MmltkWorkspaceDispatcherCommand::RegisterArena { arena } => {
                            let accepted = arenas.len() < 3 && !arenas.contains_key(&arena.surface_id);
                            drop(entries);
                            drop(arenas);
                            let initialized = accepted && Global::initialize_mmltk_workspace_arena(&arena).is_ok();
                            arenas = shared.arenas.lock().unwrap();
                            entries = shared.entries.lock().unwrap();
                            if initialized {
                                shared.pending_requests.lock().unwrap().remove(&arena.surface_id);
                                arena.ready.store(true, Ordering::Release);
                                arenas.insert(arena.surface_id, arena.clone());
                                mmltk_workspace_channel::trace_state("registry_inserted", arena.surface_id, "dispatcher_owned", false);
                                unsafe { wgpu_parent_external_texture_import_ready(shared.owner, arena.device_id,
                                    arena.surface_id.high, arena.surface_id.low) };
                                mmltk_workspace_channel::trace_state("import_ready_emitted", arena.surface_id, "layout_negotiated", false);
                                mmltk_workspace_channel::send_arena_ready(arena.surface_id, arena.layout);
                            } else {
                                mmltk_workspace_channel::send_failed(arena.surface_id,
                                    mmltk_workspace_channel::FAILED_IMPORT, 0, 0);
                            }
                        }
                        MmltkWorkspaceDispatcherCommand::Acquire { device_id, surface_id, identity } => {
                            let newest = entries.iter().filter_map(|(fd, entry)| {
                                let snapshot = entry.frame_signal.read()?;
                                (entry.device_id == device_id && entry.arena.surface_id == surface_id &&
                                    snapshot.identity() == Some(identity)).then_some((*fd, snapshot))
                            }).max_by_key(|(_, snapshot)| snapshot.presentation_revision);
                            let acquired = if let Some((fd, snapshot)) = newest {
                                let entry = entries.get_mut(&fd).unwrap();
                                match submit_mmltk_workspace_transfer(&shared, entry, Some(snapshot)) {
                                    Ok(acquired) => acquired,
                                    Err(_) => { mmltk_workspace_channel::fail(); false }
                                }
                            } else { false };
                            if !acquired {
                                unsafe { wgpu_parent_external_texture_frame_ready(shared.owner, device_id,
                                    surface_id.high, surface_id.low, 0, u32::MAX, identity.session,
                                    identity.sequence, identity.presentation_revision, 0, 0, false, 0, 0, false) };
                            }
                        }
                        MmltkWorkspaceDispatcherCommand::Register {
                            arena,
                            device_id,
                            surface_id,
                            frame,
                            frame_signal,
                            blit,
                            allocation,
                            timeline_descriptor,
                        } => {
                            let frame_fd = frame.as_raw_fd();
                            let mut frame_event = libc::epoll_event {
                                events: (libc::EPOLLIN | libc::EPOLLERR | libc::EPOLLHUP) as u32,
                                u64: frame_fd as u64,
                            };
                            let duplicate = !arenas.contains_key(&arena.surface_id)
                                || surface_frames.contains_key(&surface_id)
                                || entries.contains_key(&frame_fd);
                            let cancelled = arena.closed.load(Ordering::Acquire);
                            drop(entries);
                            drop(arenas);
                            blit.trace_binding("attached", &arena, surface_id, &frame_signal, None, None, !cancelled && !duplicate);
                            let failed = cancelled || duplicate || {
                                    blit.trace_binding("initialization_started", &arena, surface_id, &frame_signal, None, None, true);
                                    let result = blit.initialize();
                                    blit.trace_binding("initialization_completed", &arena, surface_id, &frame_signal, None, None, result.is_ok());
                                    result.is_err()
                                }
                                || (if blit.direct {
                                    blit.record_acquire().is_err()
                                } else {
                                    blit.copy_commands.iter().enumerate().any(|(slot, &command)|
                                        blit.record_copy(command, arena.destination, arena.extent, slot, None).is_err())
                                })
                                || blit.record_read_release().is_err()
                                || unsafe {
                                    libc::epoll_ctl(
                                        epoll,
                                        libc::EPOLL_CTL_ADD,
                                        frame_fd,
                                        &mut frame_event,
                                    )
                                } != 0;
                            arenas = shared.arenas.lock().unwrap();
                            entries = shared.entries.lock().unwrap();
                            if failed {
                                retired_blits.push(blit);
                                mmltk_workspace_channel::send_failed(surface_id,
                                    if cancelled { mmltk_workspace_channel::FAILED_NOT_ADMITTED }
                                    else { mmltk_workspace_channel::FAILED_IMPORT }, 0, 0);
                            } else {
                                surface_frames.insert(surface_id, frame_fd);
                                entries.insert(
                                    frame_fd,
                                    MmltkWorkspaceDispatchEntry {
                                        device_id,
                                        surface_id,
                                        frame,
                                        frame_signal,
                                        blit,
                                        arena,
                                        next_transfer_sequence: 1,
                                        physical_read: None,
                                    },
                                );
                                mmltk_workspace_channel::send_ready(surface_id, allocation, timeline_descriptor);
                            }
                        }
                        MmltkWorkspaceDispatcherCommand::DetachDestination { surface_id } => {
                            if let Some(arena) = arenas.remove(&surface_id) {
                                arena.closed.store(true, Ordering::Release);
                                arena.mailboxes.lock().unwrap().drain(|identity, slot|
                                    mmltk_workspace_channel::send_mailbox_completed(surface_id, identity.layer, slot,
                                        identity.session, identity.sequence, identity.presentation_revision));
                                match mmltk_workspace_channel::release_live(surface_id) {
                                    mmltk_workspace_channel::LiveRelease::Withdrawn =>
                                        mmltk_workspace_channel::complete_retirement(surface_id),
                                    _ => {}
                                }
                            }
                        }
                        MmltkWorkspaceDispatcherCommand::ReleaseSlot { surface_id, identity, slot } => {
                            if let Some(arena) = arenas.get(&surface_id) {
                                let mut mailboxes = arena.mailboxes.lock().unwrap();
                                if mailboxes.release(identity, slot).is_some() {
                                    for entry in entries.values_mut().filter(|entry|
                                        entry.arena.surface_id == surface_id && entry.blit.direct) {
                                        if let Some(receipt) = entry.physical_read.as_ref().filter(|receipt|
                                            receipt.slot == slot && receipt.frame.identity == identity) {
                                            let ready = receipt.snapshot.timeline_ready;
                                            if entry.blit.submission().release_acquired(ready).is_err() {
                                                mmltk_workspace_channel::fail();
                                            } else if !mmltk_workspace_channel::submit_release(entry.surface_id) {
                                                mmltk_workspace_channel::fail();
                                            }
                                        }
                                    }
                                    mmltk_workspace_channel::send_mailbox_completed(surface_id, identity.layer, slot,
                                        identity.session, identity.sequence, identity.presentation_revision);
                                    unsafe { wgpu_parent_external_texture_import_ready(shared.owner, arena.device_id,
                                        surface_id.high, surface_id.low) };
                                    if mailboxes.take_retry_after_release(identity) {
                                        mmltk_workspace_channel::send_mailbox_available(surface_id, identity.layer, slot,
                                            identity.session, identity.sequence, identity.presentation_revision);
                                    }
                                }
                            }
                        }
                        MmltkWorkspaceDispatcherCommand::RetireDevice { device_id } => {
                            shared.pending_requests.lock().unwrap().retain(|_, (device, _, _)| *device != device_id);
                            let frame_fds: Vec<_> = entries
                                .iter()
                                .filter_map(|(frame_fd, entry)| {
                                    (entry.device_id == device_id).then_some(*frame_fd)
                                })
                                .collect();
                            for frame_fd in frame_fds {
                                unsafe {
                                    libc::epoll_ctl(
                                        epoll,
                                        libc::EPOLL_CTL_DEL,
                                        frame_fd,
                                        ptr::null_mut(),
                                    )
                                };
                                if let Some(entry) = entries.remove(&frame_fd) {
                                    surface_frames.remove(&entry.surface_id);
                                    retired_entries.push(entry);
                                }
                            }
                            arenas.retain(|_, arena| arena.device_id != device_id);
                        }
                        MmltkWorkspaceDispatcherCommand::RetireAll => {
                            shared.pending_requests.lock().unwrap().clear();
                            for frame_fd in entries.keys().copied().collect::<Vec<_>>() {
                                unsafe {
                                    libc::epoll_ctl(
                                        epoll,
                                        libc::EPOLL_CTL_DEL,
                                        frame_fd,
                                        ptr::null_mut(),
                                    )
                                };
                            }
                            arenas.clear();
                            surface_frames.clear();
                            retired_entries.extend(entries.drain().map(|(_, entry)| entry));
                            close_channel = true;
                        }
                        MmltkWorkspaceDispatcherCommand::Shutdown { response } => {
                            shared.pending_requests.lock().unwrap().clear();
                            for frame_fd in entries.keys().copied().collect::<Vec<_>>() {
                                unsafe {
                                    libc::epoll_ctl(
                                        epoll,
                                        libc::EPOLL_CTL_DEL,
                                        frame_fd,
                                        ptr::null_mut(),
                                    )
                                };
                            }
                            arenas.clear();
                            surface_frames.clear();
                            retired_entries.extend(entries.drain().map(|(_, entry)| entry));
                            shutdown = Some(response);
                            running = false;
                        }
                    }
                }
                service_mmltk_workspace_admissions(&shared, &surface_frames, &mut entries);
                retire_settled_mmltk_workspace_releases(
                    epoll,
                    &mut surface_frames,
                    &mut entries,
                    &mut retired_entries,
                    &mut completed_retirements,
                );
                update_mmltk_workspace_channel_interest(epoll, &mut channel);
                continue;
            }
            if channel.is_some_and(|(channel_fd, _)| channel_fd == fd) {
                mmltk_workspace_channel::service(event.events);
                service_mmltk_workspace_admissions(&shared, &surface_frames, &mut entries);
                retire_settled_mmltk_workspace_releases(
                    epoll,
                    &mut surface_frames,
                    &mut entries,
                    &mut retired_entries,
                    &mut completed_retirements,
                );
                update_mmltk_workspace_channel_interest(epoll, &mut channel);
                continue;
            }
            // Observe the edge before draining the socket: native sent the
            // preceding physical completion before this edge, including an edge
            // that arrived just after the preceding dispatcher service call.
            let Some(entry) = entries.get(&fd) else {
                continue;
            };
            let edges = if event.events & libc::EPOLLIN as u32 != 0 {
                read_mmltk_workspace_eventfd(entry.frame.as_raw_fd())
            } else {
                0
            };
            if edges != 0 {
                mmltk_workspace_channel::service(libc::EPOLLIN as u32);
                service_mmltk_workspace_admissions(&shared, &surface_frames, &mut entries);
                retire_settled_mmltk_workspace_releases(epoll, &mut surface_frames, &mut entries, &mut retired_entries, &mut completed_retirements);
                let Some(entry) = entries.get_mut(&fd) else { continue; };
                let snapshot = entry.frame_signal.read();
                if mmltk_workspace_acceptance_trace_enabled() {
                    mmltk_workspace_channel::write_diagnostic(|line| write!(line,
                        "{{\"event\":\"firefox.workspace.source.frame_edge\",\"surface\":\"{}\",\"edges\":{},\"snapshot\":\"{:?}\"}}",
                        entry.surface_id, edges, snapshot
                    ));
                }
                unsafe { wgpu_parent_external_texture_import_ready(shared.owner, entry.device_id,
                    entry.arena.surface_id.high, entry.arena.surface_id.low) };
            }
        }
        for id in mmltk_workspace_channel::withdrawn_arenas() {
            let disposable = arenas.get(&id).is_some_and(|arena|
                (arena.storage.is_none() || arena.texture_id.lock().unwrap().is_none()) &&
                arena.mailboxes.lock().unwrap().occupied.iter().flatten().all(Option::is_none));
            if disposable {
                if let Some(arena) = arenas.remove(&id) {
                    arena.closed.store(true, Ordering::Release);
                    if matches!(mmltk_workspace_channel::release_live(id), mmltk_workspace_channel::LiveRelease::Withdrawn) {
                        completed_retirements.push(id);
                    }
                }
            }
        }
        drop(entries);
        drop(arenas);
        drop(retired_blits);
        drop(retired_entries);
        for id in completed_retirements { mmltk_workspace_channel::complete_retirement(id); }
        if close_channel { mmltk_workspace_channel::close_after_resource_shutdown(); }
        if let Some(response) = shutdown { let _ = response.try_send(()); }
    }
    unsafe { libc::close(epoll) };
}

/// Everything one shared source allocation needs to reach the page's texture,
/// owned by the shell dispatcher for as long as the mirror is active.
///
/// This is raw Vulkan rather than `wgpu`: `wgpu-core` identifiers are allocated
/// by the host process and mixing in internally allocated ones is a hard error,
/// so this shell cannot create a command encoder of its own.
#[derive(Clone, Copy)]
struct MmltkWorkspaceImageTransition {
    src_access: vk::AccessFlags,
    dst_access: vk::AccessFlags,
    old_layout: vk::ImageLayout,
    new_layout: vk::ImageLayout,
    src_queue_family: u32,
    dst_queue_family: u32,
}

fn mmltk_workspace_image_barrier(
    transition: MmltkWorkspaceImageTransition,
    image: vk::Image,
    subresource: vk::ImageSubresourceRange,
) -> vk::ImageMemoryBarrier<'static> {
    vk::ImageMemoryBarrier::default()
        .src_access_mask(transition.src_access)
        .dst_access_mask(transition.dst_access)
        .old_layout(transition.old_layout)
        .new_layout(transition.new_layout)
        .src_queue_family_index(transition.src_queue_family)
        .dst_queue_family_index(transition.dst_queue_family)
        .image(image)
        .subresource_range(subresource)
}

fn mmltk_workspace_copy_transitions(
    queue_family: u32,
) -> (
    [MmltkWorkspaceImageTransition; 2],
    [MmltkWorkspaceImageTransition; 2],
) {
    (
        [
            MmltkWorkspaceImageTransition {
                src_access: vk::AccessFlags::MEMORY_WRITE,
                dst_access: vk::AccessFlags::TRANSFER_READ,
                old_layout: vk::ImageLayout::GENERAL,
                new_layout: vk::ImageLayout::GENERAL,
                src_queue_family: vk::QUEUE_FAMILY_EXTERNAL,
                dst_queue_family: queue_family,
            },
            MmltkWorkspaceImageTransition {
                src_access: vk::AccessFlags::SHADER_READ,
                dst_access: vk::AccessFlags::TRANSFER_WRITE,
                old_layout: vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
                new_layout: vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                src_queue_family: vk::QUEUE_FAMILY_IGNORED,
                dst_queue_family: vk::QUEUE_FAMILY_IGNORED,
            },
        ],
        [
            MmltkWorkspaceImageTransition {
                src_access: vk::AccessFlags::TRANSFER_WRITE,
                dst_access: vk::AccessFlags::SHADER_READ,
                old_layout: vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                new_layout: vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
                src_queue_family: vk::QUEUE_FAMILY_IGNORED,
                dst_queue_family: vk::QUEUE_FAMILY_IGNORED,
            },
            MmltkWorkspaceImageTransition {
                src_access: vk::AccessFlags::TRANSFER_READ,
                dst_access: vk::AccessFlags::empty(),
                old_layout: vk::ImageLayout::GENERAL,
                new_layout: vk::ImageLayout::GENERAL,
                src_queue_family: queue_family,
                dst_queue_family: vk::QUEUE_FAMILY_EXTERNAL,
            },
        ],
    )
}

fn mmltk_workspace_release_transitions(queue_family: u32) -> [MmltkWorkspaceImageTransition; 2] {
    [
        MmltkWorkspaceImageTransition {
            src_access: vk::AccessFlags::MEMORY_WRITE,
            dst_access: vk::AccessFlags::MEMORY_READ,
            old_layout: vk::ImageLayout::GENERAL,
            new_layout: vk::ImageLayout::GENERAL,
            src_queue_family: vk::QUEUE_FAMILY_EXTERNAL,
            dst_queue_family: queue_family,
        },
        MmltkWorkspaceImageTransition {
            src_access: vk::AccessFlags::MEMORY_READ,
            dst_access: vk::AccessFlags::empty(),
            old_layout: vk::ImageLayout::GENERAL,
            new_layout: vk::ImageLayout::GENERAL,
            src_queue_family: queue_family,
            dst_queue_family: vk::QUEUE_FAMILY_EXTERNAL,
        },
    ]
}

fn mmltk_workspace_initial_transitions(
    queue_family: u32,
) -> (
    [MmltkWorkspaceImageTransition; 2],
    MmltkWorkspaceImageTransition,
) {
    (
        [
            MmltkWorkspaceImageTransition {
                src_access: vk::AccessFlags::MEMORY_WRITE,
                dst_access: vk::AccessFlags::empty(),
                old_layout: vk::ImageLayout::GENERAL,
                new_layout: vk::ImageLayout::GENERAL,
                src_queue_family: queue_family,
                dst_queue_family: vk::QUEUE_FAMILY_EXTERNAL,
            },
            MmltkWorkspaceImageTransition {
                src_access: vk::AccessFlags::empty(),
                dst_access: vk::AccessFlags::TRANSFER_WRITE,
                old_layout: vk::ImageLayout::UNDEFINED,
                new_layout: vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                src_queue_family: vk::QUEUE_FAMILY_IGNORED,
                dst_queue_family: vk::QUEUE_FAMILY_IGNORED,
            },
        ],
        MmltkWorkspaceImageTransition {
            src_access: vk::AccessFlags::TRANSFER_WRITE,
            dst_access: vk::AccessFlags::SHADER_READ,
            old_layout: vk::ImageLayout::TRANSFER_DST_OPTIMAL,
            new_layout: vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL,
            src_queue_family: vk::QUEUE_FAMILY_IGNORED,
            dst_queue_family: vk::QUEUE_FAMILY_IGNORED,
        },
    )
}

#[cfg(test)]
mod mmltk_workspace_transition_tests {
    use super::*;

    fn frame(
        transfer_sequence: u64,
        timeline_ready: u64,
        layer: u64,
        content_session: u64,
        content_sequence: u64,
    ) -> MmltkWorkspaceFrameSnapshot {
        MmltkWorkspaceFrameSnapshot {
            timeline_ready,
            transfer_sequence,
            layer,
            content_session,
            content_sequence,
            presentation_revision: transfer_sequence,
            content_width: 640,
            content_height: 480,
        }
    }

    #[test]
    fn transfer_snapshot_retains_exact_layer_and_content_identity() {
        let r1 = MmltkWorkspaceFrameSnapshot {
            timeline_ready: 1,
            transfer_sequence: 1,
            layer: 1,
            content_session: 41,
            content_sequence: 7,
            presentation_revision: 19,
            content_width: 640,
            content_height: 480,
        };
        assert_eq!(
            r1.identity(),
            Some(MmltkWorkspaceFrameIdentity {
                layer: 1,
                session: 41,
                sequence: 7,
                presentation_revision: 19,
            })
        );
        assert_eq!(
            r1.identity().and_then(|identity| identity.copy_index(1)),
            Some(3)
        );
    }

    #[test]
    fn mailbox_is_bounded_and_release_requires_exact_slot_content() {
        let r1 = MmltkWorkspaceFrameIdentity {
            layer: 1,
            session: 41,
            sequence: 7,
            presentation_revision: 19,
        };
        let r2 = MmltkWorkspaceFrameIdentity {
            presentation_revision: 20,
            ..r1
        };
        let r3 = MmltkWorkspaceFrameIdentity {
            presentation_revision: 21,
            ..r1
        };
        let mut mailboxes = MmltkWorkspaceMailboxes::default();
        let receipt = |identity: MmltkWorkspaceFrameIdentity| MmltkWorkspaceMailboxReceipt {
            identity,
            pixels: Some(MmltkWorkspacePixelReceipt {
                snapshot: MmltkWorkspaceFrameSnapshot {
                    timeline_ready: (identity.presentation_revision - 18) * 2 - 1,
                    transfer_sequence: identity.presentation_revision - 18,
                    layer: identity.layer as u64,
                    content_session: identity.session,
                    content_sequence: identity.sequence,
                    presentation_revision: identity.presentation_revision,
                    content_width: 384,
                    content_height: 384,
                },
                coordinates: std::array::from_fn(|index| (
                    MmltkWorkspacePixels::coordinate(index % 5, 384),
                    MmltkWorkspacePixels::coordinate(index / 5, 384),
                )),
                copy_index: identity.copy_index(if identity == r2 { 1 } else { 0 }).unwrap(),
                release: (identity.presentation_revision - 18) * 2,
            }),
        };
        assert_eq!(mailboxes.writable_slot(r1), Some(0));
        assert!(mailboxes.occupy(receipt(r1), 0));
        assert_eq!(mailboxes.writable_slot(r1), None);
        assert_eq!(mailboxes.writable_slot(r2), Some(1));
        assert!(mailboxes.occupy(receipt(r2), 1));
        assert_eq!(mailboxes.writable_slot(r3), None);
        assert!(!mailboxes.note_capacity_exhausted(r2));
        assert!(mailboxes.release(r2, 0).is_none());
        let released = mailboxes.release(r1, 0).unwrap().pixels.unwrap();
        // The older physical copy keeps its own completion value and logical
        // coordinates even while the other mailbox holds a newer submission.
        assert_eq!(released.release, 2);
        assert_eq!(released.coordinates[24], (383, 383));
        assert_eq!(released.coordinates[18], (191, 191));
        assert_eq!(released.copy_index, 2);
        assert!(!mailboxes.take_retry_after_release(r1));
        assert_eq!(mailboxes.writable_slot(r1), None);
        assert!(!mailboxes.occupy(receipt(r1), 0));
        assert!(!mailboxes.note_capacity_exhausted(r1));
        assert_eq!(mailboxes.writable_slot(r3), Some(0));
        assert!(mailboxes.note_capacity_exhausted(r3));
        assert!(!mailboxes.note_capacity_exhausted(r2));
        assert!(mailboxes.release(r2, 1).is_some());
        assert!(mailboxes.take_retry_after_release(r2));
        assert!(!mailboxes.take_retry_after_release(r2));
        assert!(mailboxes.occupy(receipt(r3), 0));
        assert!(mailboxes.release(r3, 0).is_some());
        assert_eq!(mailboxes.writable_slot(r3), None);
    }

    #[test]
    fn failed_optional_work_preserves_mailbox_progress_and_later_receipts() {
        let mut mailboxes = MmltkWorkspaceMailboxes::default();
        let failed = frame(1, 1, 0, 11, 21);
        let identity = failed.identity().unwrap();
        let slot = mailboxes.writable_slot(identity).unwrap();
        assert!(mailboxes.occupy(MmltkWorkspaceMailboxReceipt { identity, pixels: None }, slot));
        // An unrelated physical slot remains available before this one releases.
        let overlapping = frame(2, 3, 0, 11, 22).identity().unwrap();
        assert_ne!(mailboxes.writable_slot(overlapping), Some(slot));
        assert!(mailboxes.release(identity, slot).unwrap().pixels.is_none());
        let recovered = frame(3, 5, 0, 11, 23);
        let recovered_identity = recovered.identity().unwrap();
        let receipt = MmltkWorkspacePixelReceipt {
            snapshot: recovered,
            coordinates: std::array::from_fn(|index| (
                MmltkWorkspacePixels::coordinate(index % 5, recovered.content_width),
                MmltkWorkspacePixels::coordinate(index / 5, recovered.content_height))),
            copy_index: recovered_identity.copy_index(slot).unwrap(),
            release: 6,
        };
        assert!(mailboxes.occupy(MmltkWorkspaceMailboxReceipt {
            identity: recovered_identity, pixels: Some(receipt),
        }, slot));
        assert!(mailboxes.release(identity, slot).is_none());
        assert_eq!(mailboxes.release(recovered_identity, slot).unwrap().pixels.unwrap().release, 6);
        mailboxes.drain(|_, _| panic!("released products must not be drained again"));
    }

    #[test]
    fn pixel_slot_completion_misses_are_local_and_recover_on_exact_reuse() {
        let mut releases = [0; MMLTK_WORKSPACE_MAILBOX_COUNT];
        releases[0] = 2;
        let completed = 0;
        assert!(!MmltkWorkspacePixels::reusable(releases[0], |value| value <= completed));
        assert!(MmltkWorkspacePixels::reusable(releases[1], |_| false));
        releases[1] = 4;
        assert!(MmltkWorkspacePixels::reusable(releases[0], |value| value <= 2));
        assert!(!MmltkWorkspacePixels::reusable(releases[1], |value| value <= 2));
        // A probe-skipped product submission still advances this command
        // slot's completion requirement. Reuse never tests an older receipt.
        releases[0] = 6;
        assert!(!MmltkWorkspacePixels::reusable(releases[0], |value| value <= 4));
        assert!(MmltkWorkspacePixels::reusable(releases[0], |value| value <= 6));
        assert_eq!(releases.len(), MMLTK_WORKSPACE_MAILBOX_COUNT);
        let mut mailboxes = MmltkWorkspaceMailboxes::default();
        mailboxes.drain(|_, _| {});
        // Page drain does not erase in-flight command ownership.
        assert_eq!(releases[0], 6);
    }

    #[test]
    fn dispatcher_release_only_matrix_advances_exact_timeline_and_converges_producer_slots() {
        let mut missing_cursor = 1;
        assert_eq!(
            plan_mmltk_workspace_transfer(missing_cursor, None).unwrap(),
            MmltkWorkspaceTransferPlan::ReleaseOnly {
                allow_preconsumed: false,
                terminal_on_settlement: true,
            }
        );
        let missing =
            settle_mmltk_workspace_cursor(&mut missing_cursor, Some((1, None)), None, false)
                .unwrap()
                .unwrap();
        assert_eq!(missing.ready, 1);
        assert_eq!(missing.release, 2);
        assert_eq!(missing.copied_slot, None);
        assert_eq!(missing_cursor, 2);

        let invalid = frame(1, 1, u64::MAX, 0, 0);
        let mut invalid_cursor = 1;
        assert_eq!(
            plan_mmltk_workspace_transfer(invalid_cursor, Some(invalid)).unwrap(),
            MmltkWorkspaceTransferPlan::ReleaseOnly {
                allow_preconsumed: false,
                terminal_on_settlement: false,
            }
        );
        let invalid_settlement =
            settle_mmltk_workspace_cursor(&mut invalid_cursor, Some((1, None)), None, false)
                .unwrap()
                .unwrap();
        assert_eq!(invalid_settlement.release, 2);
        assert_eq!(invalid_cursor, 2);

        let stale = frame(1, 1, 1, 41, 7);
        let mut stale_cursor = 2;
        assert_eq!(
            plan_mmltk_workspace_transfer(stale_cursor, Some(stale)).unwrap(),
            MmltkWorkspaceTransferPlan::ReleaseOnly {
                allow_preconsumed: true,
                terminal_on_settlement: true,
            }
        );
        assert_eq!(
            settle_mmltk_workspace_cursor(&mut stale_cursor, None, None, true).unwrap(),
            None
        );
        assert_eq!(stale_cursor, 2);
        let mut stale_uncredited_cursor = 2;
        let stale_settlement = settle_mmltk_workspace_cursor(
            &mut stale_uncredited_cursor,
            Some((3, None)),
            None,
            true,
        )
        .unwrap()
        .unwrap();
        assert_eq!(stale_settlement.release, 4);
        assert_eq!(stale_uncredited_cursor, 3);

        let forward_gap = frame(2, 3, 1, 41, 8);
        let mut gap_cursor = 1;
        assert_eq!(
            plan_mmltk_workspace_transfer(gap_cursor, Some(forward_gap)).unwrap(),
            MmltkWorkspaceTransferPlan::ReleaseOnly {
                allow_preconsumed: false,
                terminal_on_settlement: true,
            }
        );
        let gap_settlement =
            settle_mmltk_workspace_cursor(&mut gap_cursor, Some((1, None)), None, false)
                .unwrap()
                .unwrap();
        assert_eq!(gap_settlement.ready, 1);
        assert_eq!(gap_settlement.release, 2);
        assert_eq!(gap_settlement.copied_slot, None);
        assert_eq!(gap_cursor, 2);
    }

    #[test]
    fn reusable_copy_round_trips_external_source_and_private_destination_layouts() {
        let queue = 7;
        let (acquire, release) = mmltk_workspace_copy_transitions(queue);
        assert_eq!(acquire[0].src_queue_family, vk::QUEUE_FAMILY_EXTERNAL);
        assert_eq!(acquire[0].dst_queue_family, queue);
        assert_eq!(acquire[0].old_layout, vk::ImageLayout::GENERAL);
        assert_eq!(acquire[0].new_layout, vk::ImageLayout::TRANSFER_SRC_OPTIMAL);
        assert_eq!(
            acquire[1].old_layout,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL
        );
        assert_eq!(acquire[1].new_layout, vk::ImageLayout::TRANSFER_DST_OPTIMAL);
        assert_eq!(release[0].old_layout, vk::ImageLayout::TRANSFER_DST_OPTIMAL);
        assert_eq!(
            release[0].new_layout,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL
        );
        assert_eq!(release[1].old_layout, vk::ImageLayout::TRANSFER_SRC_OPTIMAL);
        assert_eq!(release[1].new_layout, vk::ImageLayout::GENERAL);
        assert_eq!(release[1].src_queue_family, queue);
        assert_eq!(release[1].dst_queue_family, vk::QUEUE_FAMILY_EXTERNAL);
    }

    #[test]
    fn release_only_tombstone_round_trips_source_ownership() {
        let queue = 11;
        let transitions = mmltk_workspace_release_transitions(queue);
        assert_eq!(transitions[0].src_queue_family, vk::QUEUE_FAMILY_EXTERNAL);
        assert_eq!(transitions[0].dst_queue_family, queue);
        assert_eq!(transitions[1].src_queue_family, queue);
        assert_eq!(transitions[1].dst_queue_family, vk::QUEUE_FAMILY_EXTERNAL);
        assert!(transitions.iter().all(|transition| transition.old_layout
            == vk::ImageLayout::GENERAL
            && transition.new_layout == vk::ImageLayout::GENERAL));
    }

    #[test]
    fn import_initialization_leaves_private_texture_shader_readable() {
        let queue = 13;
        let (initial, destination_ready) = mmltk_workspace_initial_transitions(queue);
        assert_eq!(initial[0].src_queue_family, queue);
        assert_eq!(initial[0].dst_queue_family, vk::QUEUE_FAMILY_EXTERNAL);
        assert_eq!(initial[1].old_layout, vk::ImageLayout::UNDEFINED);
        assert_eq!(initial[1].new_layout, vk::ImageLayout::TRANSFER_DST_OPTIMAL);
        assert_eq!(
            destination_ready.old_layout,
            vk::ImageLayout::TRANSFER_DST_OPTIMAL
        );
        assert_eq!(
            destination_ready.new_layout,
            vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL
        );
    }
}

// Fixed browser-owned evidence for the shared source and completed mailbox.
// Direct sampling reads 25 image pixels without a mailbox image.
// Commands and coherent mappings are reused for the lifetime of the source.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum MmltkWorkspaceProbeFailure { Allocation, Reset, Begin, End }

struct MmltkWorkspacePixels {
    device: ash::Device,
    pool: vk::CommandPool,
    commands: Vec<vk::CommandBuffer>,
    buffer: vk::Buffer,
    memory: vk::DeviceMemory,
    mapped: usize,
    submitted_release: [u64; MMLTK_WORKSPACE_MAILBOX_COUNT],
}

impl MmltkWorkspacePixels {
    const SAMPLES: usize = 25;
    const SLOT_BYTES: usize = (Self::SAMPLES * 2 + 1) * 4;
    const DIRECT_BYTES: usize = (Self::SAMPLES + 1) * 4;
    const COMPLETED_MARKER: u32 = 0x4d4d4c54;

    fn check_failure(boundary: MmltkWorkspaceProbeFailure) -> Result<(), vk::Result> {
        static TARGET: OnceLock<Option<MmltkWorkspaceProbeFailure>> = OnceLock::new();
        static CONSUMED: std::sync::atomic::AtomicBool = std::sync::atomic::AtomicBool::new(false);
        let target = TARGET.get_or_init(|| {
            if !mmltk_workspace_acceptance_trace_enabled()
                || !mmltk_workspace_channel::workspace_pixel_probes_enabled() {
                return None;
            }
            match std::env::var("MMLTK_RUN_WORKSPACE_WAYLAND_PROBE_FAILURE").as_deref() {
                Ok("allocation") => Some(MmltkWorkspaceProbeFailure::Allocation),
                Ok("reset") => Some(MmltkWorkspaceProbeFailure::Reset),
                Ok("begin") => Some(MmltkWorkspaceProbeFailure::Begin),
                Ok("end") => Some(MmltkWorkspaceProbeFailure::End),
                _ => None,
            }
        });
        if *target == Some(boundary) && !CONSUMED.swap(true, std::sync::atomic::Ordering::Relaxed) {
            return Err(vk::Result::ERROR_OUT_OF_HOST_MEMORY);
        }
        Ok(())
    }

    fn report_failure(surface: mmltk_workspace_channel::SurfaceId,
                      source: mmltk_workspace_channel::SurfaceId,
                      boundary: MmltkWorkspaceProbeFailure,
                      frame: Option<(MmltkWorkspaceFrameIdentity, usize, MmltkWorkspaceFrameSnapshot)>) {
        if !mmltk_workspace_acceptance_trace_enabled() { return; }
        if let Some((identity, index, snapshot)) = frame {
            mmltk_workspace_channel::write_diagnostic(|line| write!(line,
                "{{\"event\":\"firefox.workspace.probe_failed\",\"boundary\":\"{boundary:?}\",\"surface\":\"{surface}\",\"source\":\"{source}\",\"layer\":{},\"slot\":{},\"presentation_revision\":{},\"transfer_sequence\":{},\"content_session\":{},\"content_sequence\":{}}}",
                identity.layer, index % MMLTK_WORKSPACE_MAILBOX_SLOTS, identity.presentation_revision,
                snapshot.transfer_sequence, identity.session, identity.sequence));
        } else {
            mmltk_workspace_channel::write_diagnostic(|line| write!(line,
                "{{\"event\":\"firefox.workspace.probe_failed\",\"boundary\":\"{boundary:?}\",\"surface\":\"{surface}\",\"source\":\"{source}\"}}"));
        }
    }

    fn new(hal: &wgh::vulkan::Device) -> Result<Self, vk::Result> {
        Self::check_failure(MmltkWorkspaceProbeFailure::Allocation)?;
        Self::allocate(hal, MMLTK_WORKSPACE_MAILBOX_COUNT, Self::SLOT_BYTES)
    }

    fn allocate(hal: &wgh::vulkan::Device, slots: usize, slot_bytes: usize) -> Result<Self, vk::Result> {
        let device = hal.raw_device();
        let mut probe = Self {
            device: device.clone(), buffer: vk::Buffer::null(), memory: vk::DeviceMemory::null(),
            pool: vk::CommandPool::null(), commands: Vec::new(),
            mapped: 0, submitted_release: [0; MMLTK_WORKSPACE_MAILBOX_COUNT],
        };
        probe.pool = unsafe { device.create_command_pool(&vk::CommandPoolCreateInfo::default()
            .flags(vk::CommandPoolCreateFlags::RESET_COMMAND_BUFFER)
            .queue_family_index(hal.queue_family_index()), None) }?;
        probe.commands = unsafe { device.allocate_command_buffers(&vk::CommandBufferAllocateInfo::default()
            .command_pool(probe.pool).level(vk::CommandBufferLevel::PRIMARY)
            .command_buffer_count(slots as u32)) }?;
        probe.buffer = unsafe { device.create_buffer(&vk::BufferCreateInfo::default()
            .size((slot_bytes * slots) as u64)
            .usage(vk::BufferUsageFlags::TRANSFER_DST)
            .sharing_mode(vk::SharingMode::EXCLUSIVE), None) }?;
        let requirements = unsafe { device.get_buffer_memory_requirements(probe.buffer) };
        let properties = unsafe { hal.shared_instance().raw_instance()
            .get_physical_device_memory_properties(hal.raw_physical_device()) };
        let memory_type = select_memory_type(&properties,
            vk::MemoryPropertyFlags::HOST_VISIBLE | vk::MemoryPropertyFlags::HOST_COHERENT,
            Some(requirements.memory_type_bits)).ok_or(vk::Result::ERROR_FEATURE_NOT_PRESENT)?;
        probe.memory = unsafe { device.allocate_memory(&vk::MemoryAllocateInfo::default()
            .allocation_size(requirements.size).memory_type_index(memory_type), None) }?;
        unsafe { device.bind_buffer_memory(probe.buffer, probe.memory, 0) }?;
        probe.mapped = unsafe { device.map_memory(probe.memory, 0, requirements.size,
            vk::MemoryMapFlags::empty()) }? as usize;
        Ok(probe)
    }

    fn coordinate(index: usize, size: u32) -> u32 {
        [0, 191.min(size - 1), 383.min(size - 1), (size - 1) / 2, size - 1][index]
    }

    fn reusable(release: u64, complete: impl FnOnce(u64) -> bool) -> bool {
        release == 0 || complete(release)
    }

    fn record(&self, commands: vk::CommandBuffer, image: vk::Image,
              layout: vk::ImageLayout, layer: u32, slot: usize, boundary: usize,
              coordinates: &[(u32, u32); Self::SAMPLES]) {
        let regions: [vk::BufferImageCopy; Self::SAMPLES] = std::array::from_fn(|index| {
            vk::BufferImageCopy::default()
                .buffer_offset((slot * Self::SLOT_BYTES + (boundary * Self::SAMPLES + index) * 4) as u64)
                .image_subresource(vk::ImageSubresourceLayers::default()
                    .aspect_mask(vk::ImageAspectFlags::COLOR).base_array_layer(layer).layer_count(1))
                .image_offset(vk::Offset3D { x: coordinates[index].0 as i32,
                    y: coordinates[index].1 as i32, z: 0 })
                .image_extent(vk::Extent3D { width: 1, height: 1, depth: 1 })
        });
        unsafe { self.device.cmd_copy_image_to_buffer(commands, image, layout, self.buffer, &regions) };
    }

    fn report(&self, source: mmltk_workspace_channel::SurfaceId,
              surface: mmltk_workspace_channel::SurfaceId, identity: MmltkWorkspaceFrameIdentity,
              slot: u32, receipt: MmltkWorkspacePixelReceipt, completed: Result<bool, vk::Result>) {
        // Caller established completion and still owns this exact mailbox.
        let samples = unsafe { std::slice::from_raw_parts(
            (self.mapped + receipt.copy_index * Self::SLOT_BYTES) as *const u32, Self::SAMPLES * 2 + 1) };
        mmltk_workspace_channel::write_diagnostic(|line| write!(line,
            "{{\"event\":\"firefox.workspace.pixel_completion\",\"source\":\"{source}\",\"surface\":\"{surface}\",\"presentation_revision\":{},\"transfer_sequence\":{},\"timeline_release\":{},\"completion\":\"{completed:?}\",\"readback_marker\":{},\"expected_marker\":{}}}",
            identity.presentation_revision, receipt.snapshot.transfer_sequence, receipt.release,
            samples[Self::SAMPLES * 2], Self::COMPLETED_MARKER));
        for (index, rgba) in samples[..Self::SAMPLES * 2].iter().enumerate() {
            mmltk_workspace_channel::write_diagnostic(|line| write!(line,
                "{{\"event\":\"firefox.workspace.pixel\",\"boundary\":\"{}\",\"surface\":\"{}\",\"layer\":{},\"slot\":{},\"content_session\":{},\"content_sequence\":{},\"presentation_revision\":{},\"content_width\":{},\"content_height\":{},\"transfer_sequence\":{},\"timeline_ready\":{},\"timeline_release\":{},\"sample_index\":{},\"sample_x\":{},\"sample_y\":{},\"sample_rgba\":{}}}",
                if index < Self::SAMPLES { "import" } else { "mailbox" }, surface,
                identity.layer, slot, identity.session, identity.sequence, identity.presentation_revision,
                receipt.snapshot.content_width, receipt.snapshot.content_height,
                receipt.snapshot.transfer_sequence, receipt.snapshot.timeline_ready, receipt.release, index % Self::SAMPLES,
                receipt.coordinates[index % Self::SAMPLES].0,
                receipt.coordinates[index % Self::SAMPLES].1, rgba,
            ));
        }
    }

    fn report_direct(&self, source: mmltk_workspace_channel::SurfaceId,
                     surface: mmltk_workspace_channel::SurfaceId, identity: MmltkWorkspaceFrameIdentity,
                     slot: u32, receipt: MmltkWorkspacePixelReceipt, completed: Result<bool, vk::Result>,
                     storage: &MmltkWorkspaceImage) {
        let samples = unsafe { std::slice::from_raw_parts(self.mapped as *const u32, Self::SAMPLES + 1) };
        for (index, rgba) in samples[..Self::SAMPLES].iter().enumerate() {
            mmltk_workspace_channel::write_diagnostic(|line| {
                write!(line,
                    "{{\"event\":\"firefox.workspace.direct_pixel\",\"source\":\"{source}\",\"surface\":\"{surface}\",\"layer\":{},\"slot\":{},\"content_session\":{},\"content_sequence\":{},\"presentation_revision\":{},\"content_width\":{},\"content_height\":{},\"transfer_sequence\":{},\"timeline_ready\":{},\"timeline_release\":{},\"sample_index\":{index},\"sample_x\":{},\"sample_y\":{},\"sample_rgba\":{rgba},\"completion\":\"{completed:?}\",\"readback_marker\":{},\"expected_marker\":{}",
                    identity.layer, slot, identity.session, identity.sequence, identity.presentation_revision,
                    receipt.snapshot.content_width, receipt.snapshot.content_height, receipt.snapshot.transfer_sequence,
                    receipt.snapshot.timeline_ready, receipt.release, receipt.coordinates[index].0, receipt.coordinates[index].1,
                    samples[Self::SAMPLES], Self::COMPLETED_MARKER)?;
                if let Some(facts) = &storage.direct_memory {
                    facts.write_fields(line, storage.image, storage.memory)?;
                }
                write!(line, "}}")
            });
        }
        if let Some(facts) = &storage.direct_memory {
            mmltk_workspace_channel::write_diagnostic(|line| {
                write!(line, "{{\"event\":\"firefox.workspace.direct_memory_status\",\"source\":\"{source}\",\"surface\":\"{surface}\",\"layer\":{},\"slot\":{},\"content_session\":{},\"content_sequence\":{},\"presentation_revision\":{},\"transfer_sequence\":{},\"timeline_ready\":{},\"timeline_release\":{},\"completion\":\"{completed:?}\",\"reason\":\"{}\"",
                    identity.layer, slot, identity.session, identity.sequence, identity.presentation_revision,
                    receipt.snapshot.transfer_sequence, receipt.snapshot.timeline_ready, receipt.release, facts.reason())?;
                facts.write_fields(line, storage.image, storage.memory)?;
                write!(line, "}}")
            });
        }
    }
}

impl Drop for MmltkWorkspacePixels {
    fn drop(&mut self) {
        unsafe {
            self.device.destroy_command_pool(self.pool, None);
            if self.mapped != 0 { self.device.unmap_memory(self.memory); }
            self.device.destroy_buffer(self.buffer, None);
            self.device.free_memory(self.memory, None);
        }
    }
}

struct MmltkWorkspaceBlit {
    device: ash::Device,
    queue: vk::Queue,
    queue_family_index: u32,
    /// The device's single queue is externally synchronised. Every WebGPU and
    /// bridge queue operation takes this wgpu-hal-owned gate.
    queue_gate: Arc<Mutex<()>>,
    pool: vk::CommandPool,
    copy_commands: Box<[vk::CommandBuffer]>,
    release_commands: vk::CommandBuffer,
    /// CUDA completes odd values before acquisition. The browser signals the
    /// next even value after its actual read, independently for each source.
    timeline: vk::Semaphore,
    submission: Option<Arc<wgh::vulkan::ExternalTimelineQueueSubmission>>,
    source: vk::Image,
    storage: Arc<MmltkWorkspaceImage>,
    direct: bool,
    pixels: Option<MmltkWorkspacePixels>,
    destination: vk::Image,
    extent: vk::Extent3D,
}

impl MmltkWorkspaceBlit {
    fn trace_binding(&self, phase: &str, arena: &MmltkWorkspaceArena,
        source: mmltk_workspace_channel::SurfaceId, signal: &MmltkWorkspaceFrameSignal,
        snapshot: Option<MmltkWorkspaceFrameSnapshot>, texture: Option<id::TextureId>, outcome: bool) {
        if !self.direct || !mmltk_workspace_acceptance_trace_enabled() { return; }
        // Allocation identity is immutable for the lifetime of this mapping.
        let allocation = unsafe { (*signal.access).allocation_identity };
        mmltk_workspace_channel::write_diagnostic(|line| write!(line,
            "{{\"event\":\"firefox.workspace.direct_binding\",\"phase\":\"{}\",\"source\":\"{}\",\"surface\":\"{}\",\"workspace_allocation\":{},\"device_id\":\"{:?}\",\"vk_device\":\"{:?}\",\"vk_queue\":\"{:?}\",\"queue_family\":{},\"vk_image\":\"{:?}\",\"storage_image\":\"{:?}\",\"vk_memory\":\"{:?}\",\"vk_semaphore\":\"{:?}\",\"texture_id\":\"{:?}\",\"chosen_layout\":\"GENERAL\",\"width\":{},\"height\":{},\"pitch\":{},\"offset\":{},\"timeline_ready\":{},\"transfer_sequence\":{},\"content_session\":{},\"content_sequence\":{},\"presentation_revision\":{},\"outcome\":{}}}",
            phase, source, arena.surface_id, allocation, arena.device_id, self.device.handle(), self.queue,
            self.queue_family_index, self.source, self.storage.image, self.storage.memory, self.timeline, texture,
            self.extent.width, self.extent.height, arena.layout.stride, arena.layout.offset,
            snapshot.map_or(0, |frame| frame.timeline_ready), snapshot.map_or(0, |frame| frame.transfer_sequence),
            snapshot.map_or(0, |frame| frame.content_session), snapshot.map_or(0, |frame| frame.content_sequence),
            snapshot.map_or(0, |frame| frame.presentation_revision), outcome));
    }
    fn record_acquire(&self) -> Result<(), vk::Result> {
        let range = vk::ImageSubresourceRange::default().aspect_mask(vk::ImageAspectFlags::COLOR)
            .level_count(1).layer_count(1);
        let mut transition = mmltk_workspace_release_transitions(self.queue_family_index)[0];
        transition.dst_access = vk::AccessFlags::SHADER_READ;
        let barrier = mmltk_workspace_image_barrier(transition, self.source, range);
        let command = self.copy_commands[0];
        unsafe {
            self.device.begin_command_buffer(command, &vk::CommandBufferBeginInfo::default())?;
            self.device.cmd_pipeline_barrier(command, vk::PipelineStageFlags::ALL_COMMANDS,
                vk::PipelineStageFlags::ALL_COMMANDS, vk::DependencyFlags::empty(), &[], &[], &[barrier]);
            self.device.end_command_buffer(command)
        }
    }

    fn record_direct_probe(&self, coordinates: &[(u32, u32); MmltkWorkspacePixels::SAMPLES]) -> Result<(), vk::Result> {
        let probe = self.pixels.as_ref().expect("enabled direct source probe");
        let command = probe.commands[0];
        let range = vk::ImageSubresourceRange::default().aspect_mask(vk::ImageAspectFlags::COLOR)
            .level_count(1).layer_count(1);
        let mut transition = mmltk_workspace_release_transitions(self.queue_family_index)[0];
        transition.dst_access = vk::AccessFlags::SHADER_READ | vk::AccessFlags::TRANSFER_READ;
        let acquire = mmltk_workspace_image_barrier(transition, self.source, range);
        // The sole prior source read has physically settled before this command
        // is reset. The ordinary pre-recorded acquire remains the failure fallback.
        unsafe {
            self.device.reset_command_buffer(command, vk::CommandBufferResetFlags::empty())?;
            self.device.begin_command_buffer(command, &vk::CommandBufferBeginInfo::default())?;
            self.device.cmd_pipeline_barrier(command, vk::PipelineStageFlags::ALL_COMMANDS,
                vk::PipelineStageFlags::ALL_COMMANDS, vk::DependencyFlags::empty(), &[], &[], &[acquire]);
            probe.record(command, self.source, vk::ImageLayout::GENERAL, 0, 0, 0, coordinates);
            self.device.cmd_fill_buffer(command, probe.buffer, (MmltkWorkspacePixels::SAMPLES * 4) as u64,
                4, MmltkWorkspacePixels::COMPLETED_MARKER);
            self.device.cmd_pipeline_barrier(command, vk::PipelineStageFlags::TRANSFER,
                vk::PipelineStageFlags::HOST, vk::DependencyFlags::empty(),
                &[vk::MemoryBarrier::default().src_access_mask(vk::AccessFlags::TRANSFER_WRITE)
                    .dst_access_mask(vk::AccessFlags::HOST_READ)], &[], &[]);
            self.device.end_command_buffer(command)
        }
    }
    fn probe_slot_reusable(&self, slot: usize) -> Result<bool, vk::Result> {
        let release = self.pixels.as_ref().unwrap().submitted_release[slot];
        let mut observed = Ok(false);
        let reusable = MmltkWorkspacePixels::reusable(release, |release| {
            observed = self.submission().submitted_release_complete(release);
            observed.unwrap_or(false)
        });
        if observed == Err(vk::Result::ERROR_DEVICE_LOST) { return Err(vk::Result::ERROR_DEVICE_LOST); }
        Ok(reusable)
    }

    fn submission(&self) -> &wgh::vulkan::ExternalTimelineQueueSubmission {
        self.submission
            .as_deref()
            .expect("workspace timeline registration must outlive its blit")
    }

    /// Records the immutable normal command or a separate optional command.
    /// Only the optional command is reset after its exact slot completes.
    ///
    /// The destination returns from WebGPU in `SHADER_READ_ONLY_OPTIMAL`, is
    /// overwritten completely, then returns to the same layout tracked by
    /// WebGPU. The shared linear source stays in `GENERAL` across CUDA and
    /// Vulkan ownership. Queue serialization makes the one recording reusable.
    fn record_copy(
        &self,
        commands: vk::CommandBuffer,
        destination: vk::Image,
        extent: vk::Extent3D,
        slot: usize,
        coordinates: Option<&[(u32, u32); 25]>,
    ) -> Result<(), (MmltkWorkspaceProbeFailure, vk::Result)> {
        let source_subresource = vk::ImageSubresourceRange::default()
            .aspect_mask(vk::ImageAspectFlags::COLOR)
            .level_count(1)
            .layer_count(1);
        let destination_subresource = source_subresource.base_array_layer(slot as u32);
        let (acquire_transitions, release_transitions) =
            mmltk_workspace_copy_transitions(self.queue_family_index);
        let acquire = [
            mmltk_workspace_image_barrier(acquire_transitions[0], self.source, source_subresource),
            mmltk_workspace_image_barrier(
                acquire_transitions[1],
                destination,
                destination_subresource,
            ),
        ];
        let release = [
            mmltk_workspace_image_barrier(
                release_transitions[0],
                destination,
                destination_subresource,
            ),
            mmltk_workspace_image_barrier(release_transitions[1], self.source, source_subresource),
        ];
        let source_layers = vk::ImageSubresourceLayers::default()
            .aspect_mask(vk::ImageAspectFlags::COLOR)
            .layer_count(1);
        let destination_layers = source_layers.base_array_layer(slot as u32);
        let region = [vk::ImageCopy::default()
            .src_subresource(source_layers)
            .dst_subresource(destination_layers)
            .extent(extent)];
        unsafe {
            if coordinates.is_some() {
                MmltkWorkspacePixels::check_failure(MmltkWorkspaceProbeFailure::Begin)
                    .map_err(|error| (MmltkWorkspaceProbeFailure::Begin, error))?;
            }
            self.device
                .begin_command_buffer(commands, &vk::CommandBufferBeginInfo::default())
                .map_err(|error| (MmltkWorkspaceProbeFailure::Begin, error))?;
            self.device.cmd_pipeline_barrier(
                commands,
                vk::PipelineStageFlags::ALL_COMMANDS,
                vk::PipelineStageFlags::TRANSFER,
                vk::DependencyFlags::empty(),
                &[],
                &[],
                &acquire,
            );
            self.device.cmd_copy_image(
                commands,
                self.source,
                vk::ImageLayout::GENERAL,
                destination,
                vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                &region,
            );
            if let (Some(probe), Some(coordinates)) = (&self.pixels, coordinates) {
                probe.record(commands, self.source, vk::ImageLayout::GENERAL, 0, slot, 0, coordinates);
                let readable = vk::ImageMemoryBarrier::default()
                    .src_access_mask(vk::AccessFlags::TRANSFER_WRITE)
                    .dst_access_mask(vk::AccessFlags::TRANSFER_READ)
                    .old_layout(vk::ImageLayout::TRANSFER_DST_OPTIMAL)
                    .new_layout(vk::ImageLayout::TRANSFER_SRC_OPTIMAL)
                    .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
                    .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
                    .image(destination).subresource_range(destination_subresource);
                self.device.cmd_pipeline_barrier(commands, vk::PipelineStageFlags::TRANSFER,
                    vk::PipelineStageFlags::TRANSFER, vk::DependencyFlags::empty(), &[], &[], &[readable]);
                probe.record(commands, destination, vk::ImageLayout::TRANSFER_SRC_OPTIMAL, slot as u32, slot, 1, coordinates);
                self.device.cmd_fill_buffer(commands, probe.buffer,
                    (slot * MmltkWorkspacePixels::SLOT_BYTES + MmltkWorkspacePixels::SAMPLES * 2 * 4) as u64,
                    4, MmltkWorkspacePixels::COMPLETED_MARKER);
                let writable = vk::ImageMemoryBarrier::default()
                    .src_access_mask(vk::AccessFlags::TRANSFER_READ)
                    .dst_access_mask(vk::AccessFlags::TRANSFER_WRITE)
                    .old_layout(vk::ImageLayout::TRANSFER_SRC_OPTIMAL)
                    .new_layout(vk::ImageLayout::TRANSFER_DST_OPTIMAL)
                    .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
                    .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
                    .image(destination).subresource_range(destination_subresource);
                self.device.cmd_pipeline_barrier(commands, vk::PipelineStageFlags::TRANSFER,
                    vk::PipelineStageFlags::TRANSFER | vk::PipelineStageFlags::HOST,
                    vk::DependencyFlags::empty(),
                    &[vk::MemoryBarrier::default().src_access_mask(vk::AccessFlags::TRANSFER_WRITE)
                        .dst_access_mask(vk::AccessFlags::HOST_READ)], &[], &[writable]);
            }
            self.device.cmd_pipeline_barrier(
                commands,
                vk::PipelineStageFlags::TRANSFER,
                vk::PipelineStageFlags::ALL_COMMANDS,
                vk::DependencyFlags::empty(),
                &[],
                &[],
                &release,
            );
            if coordinates.is_some() {
                MmltkWorkspacePixels::check_failure(MmltkWorkspaceProbeFailure::End)
                    .map_err(|error| (MmltkWorkspaceProbeFailure::End, error))?;
            }
            self.device.end_command_buffer(commands)
                .map_err(|error| (MmltkWorkspaceProbeFailure::End, error))
        }
    }

    /// Returns direct source ownership after the final page and GPU readers.
    fn record_read_release(&self) -> Result<(), vk::Result> {
        let source_subresource = vk::ImageSubresourceRange::default()
            .aspect_mask(vk::ImageAspectFlags::COLOR)
            .level_count(1)
            .layer_count(1);
        let transitions = mmltk_workspace_release_transitions(self.queue_family_index);
        let acquire = [mmltk_workspace_image_barrier(
            transitions[0],
            self.source,
            source_subresource,
        )];
        let release = [mmltk_workspace_image_barrier(
            transitions[1],
            self.source,
            source_subresource,
        )];
        unsafe {
            self.device.begin_command_buffer(
                self.release_commands,
                &vk::CommandBufferBeginInfo::default(),
            )?;
            if !self.direct {
                self.device.cmd_pipeline_barrier(
                    self.release_commands, vk::PipelineStageFlags::ALL_COMMANDS,
                    vk::PipelineStageFlags::ALL_COMMANDS, vk::DependencyFlags::empty(),
                    &[], &[], &acquire);
            }
            self.device.cmd_pipeline_barrier(
                self.release_commands,
                vk::PipelineStageFlags::ALL_COMMANDS,
                vk::PipelineStageFlags::ALL_COMMANDS,
                vk::DependencyFlags::empty(),
                &[],
                &[],
                &release,
            );
            self.device.end_command_buffer(self.release_commands)
        }
    }

    /// Establishes external producer ownership before its first write. The
    /// independently owned Copy destination was initialized with its arena.
    fn initialize(&self) -> Result<(), vk::Result> {
        let range = vk::ImageSubresourceRange::default().aspect_mask(vk::ImageAspectFlags::COLOR).level_count(1).layer_count(1);
        // Finish the potentially discarding layout transition locally before
        // handing memory to CUDA. A layout transition attached to an ownership
        // transfer spans its release/acquire pair; all external handoffs must
        // instead preserve the producer's GENERAL-layout contents.
        let layout = mmltk_workspace_image_barrier(MmltkWorkspaceImageTransition {
            src_access: vk::AccessFlags::empty(),
            dst_access: vk::AccessFlags::MEMORY_READ | vk::AccessFlags::MEMORY_WRITE,
            old_layout: vk::ImageLayout::UNDEFINED,
            new_layout: vk::ImageLayout::GENERAL,
            src_queue_family: vk::QUEUE_FAMILY_IGNORED,
            dst_queue_family: vk::QUEUE_FAMILY_IGNORED,
        }, self.source, range);
        let (initial, _) = mmltk_workspace_initial_transitions(self.queue_family_index);
        let barrier = mmltk_workspace_image_barrier(initial[0], self.source, range);
        unsafe {
            self.device.begin_command_buffer(self.copy_commands[0], &vk::CommandBufferBeginInfo::default()
                .flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT))?;
            self.device.cmd_pipeline_barrier(self.copy_commands[0], vk::PipelineStageFlags::TOP_OF_PIPE,
                vk::PipelineStageFlags::ALL_COMMANDS, vk::DependencyFlags::empty(), &[], &[], &[layout]);
            self.device.cmd_pipeline_barrier(self.copy_commands[0], vk::PipelineStageFlags::ALL_COMMANDS,
                vk::PipelineStageFlags::ALL_COMMANDS, vk::DependencyFlags::empty(), &[], &[], &[barrier]);
            self.device.end_command_buffer(self.copy_commands[0])?;
        }
        self.submit_initialization()?;
        unsafe { self.device.reset_command_buffer(self.copy_commands[0], vk::CommandBufferResetFlags::empty()) }
    }

    fn submit_initialization(&self) -> Result<(), vk::Result> {
        let buffers = [self.copy_commands[0]];
        submit_mmltk_queue_and_wait(&self.device, self.queue, &self.queue_gate, &buffers)
    }
}

/// Releases everything the blit owns, on whichever path abandons it. The
/// destination image and its memory belong to `wgpu` and are deliberately
/// absent: the owner retires the watch before destroying them.
impl MmltkWorkspaceBlit {
    fn retire(&self) {
        // Retirement shares the HAL queue gate with submit/present/idle. It
        // releases one final producer-owned frame without touching the page
        // destination, deactivates the registration, and waits for its release.
        if self
            .submission()
            .retire(MMLTK_WORKSPACE_QUEUE_TIMEOUT_NS)
            .is_err()
        {
            log::error!("WebGPU workspace mirror could not retire safely");
            // The destination remains page-owned and may be destroyed as soon
            // as this Drop returns. Continuing after a failed queue/release
            // fence would therefore permit an in-flight raw command buffer to
            // address freed memory. Process death is bounded and lets the host
            // use its explicit peer-loss rescue path.
            std::process::abort();
        }
    }
}

impl Drop for MmltkWorkspaceBlit {
    fn drop(&mut self) {
        self.retire();
        drop(self.submission.take());
        // The registration is inactive and its last submission is complete;
        // take the gate once more before the raw objects disappear.
        let _serialized = self
            .queue_gate
            .lock()
            .unwrap_or_else(|poisoned| poisoned.into_inner());
        unsafe {
            self.device.destroy_semaphore(self.timeline, None);
            self.device.destroy_command_pool(self.pool, None);
        }
        drop(self.pixels.take());
    }
}

impl Global {
    /// Reads the identifier out of a reserved texture label. The label is the
    /// only page-visible way to name an allocation the host admitted, because
    /// stock WebGPU cannot name a foreign texture. Admission itself is checked
    /// against the host channel, not against the label.
    fn parse_mmltk_workspace_label(
        &self,
        desc: &wgc::resource::TextureDescriptor,
    ) -> Option<mmltk_workspace_channel::SurfaceId> {
        mmltk_workspace_channel::SurfaceId::parse(
            desc.label
                .as_deref()?
                .strip_prefix(MMLTK_WORKSPACE_LABEL_PREFIX)?,
        )
    }

    /// Gives the page its own texture for the allocation the host described
    /// under `id`, and starts the watch that keeps it current.
    ///
    /// The page owns one two-slot destination independently of Firefox-allocated
    /// source backing shared with native CUDA. Source attachment initializes
    /// external ownership before the producer fills its workspace; only the
    /// queued source blit reads it here.
    fn request_mmltk_workspace(
        &self, device_id: id::DeviceId, id: mmltk_workspace_channel::SurfaceId, width: u32, height: u32,
    ) -> Result<(), MmltkWorkspaceImportError> {
        let unavailable = || MmltkWorkspaceImportError::code(mmltk_workspace_channel::FAILED_NOT_ADMITTED);
        let unimportable = || MmltkWorkspaceImportError::code(mmltk_workspace_channel::FAILED_IMPORT);
        let shared = &self.mmltk_workspace_dispatcher.shared;
        if let Some(arena) = shared.arenas.lock().unwrap().get(&id) {
            return if arena.device_id == device_id && arena.extent.width == width && arena.extent.height == height {
                Ok(())
            } else { Err(unavailable()) };
        }
        if !mmltk_workspace_channel::arena_admitted(id) {
            let mut pending = shared.pending_requests.lock().unwrap();
            if pending.len() >= 3 && !pending.contains_key(&id) { return Err(unavailable()); }
            if pending.get(&id).is_some_and(|request| *request != (device_id, width, height)) { return Err(unavailable()); }
            pending.insert(id, (device_id, width, height));
            write_mmltk_workspace_eventfd(shared.wake.as_raw_fd());
            return Ok(());
        }
        shared.pending_requests.lock().unwrap().remove(&id);
        let admission = mmltk_workspace_channel::take_admission(id, width, height).ok_or_else(unavailable)?;
        if admission.layout.opcode != mmltk_workspace_channel::OPCODE_ARENA { return Err(unavailable()); }
        let hal = unsafe { self.device_as_hal::<wgc::api::Vulkan>(device_id) }.ok_or_else(unavailable)?;
        let device = hal.raw_device();
        let extent = vk::Extent3D { width, height, depth: 1 };
        let (image, layout) = self.create_mmltk_workspace_source(&hal, device_id, extent, None)?;
        // This is the first physical producer image, retained unbound until its
        // allocation arrives. Negotiation creates no dummy WebGPU resource.
        let initial_source = MmltkWorkspaceImage { device: device.clone(), image, memory: vk::DeviceMemory::null(), direct_memory: None,
            _device_custody: hal.external_image_custody() };
        let storage = if layout.direct_sampling == 0 {
            let properties = unsafe { hal.shared_instance().raw_instance()
                .get_physical_device_memory_properties(hal.raw_physical_device()) };
            let (image, memory) = self.create_mmltk_workspace_destination(device, &properties, extent).ok_or_else(unimportable)?;
            Some(Arc::new(MmltkWorkspaceImage { device: device.clone(), image, memory, direct_memory: None,
                _device_custody: hal.external_image_custody() }))
        } else { None };
        let arena = Arc::new(MmltkWorkspaceArena {
            device_id, texture_id: Mutex::new(None), surface_id: id,
            destination: storage.as_ref().map_or(vk::Image::null(), |storage| storage.image), storage,
            initial_source: Mutex::new(Some(initial_source)), device: device.clone(), queue: hal.raw_queue(),
            queue_family: hal.queue_family_index(), queue_gate: hal.queue_operation_gate(), ready: AtomicBool::new(false), closed: AtomicBool::new(false),
            extent, layout, mailboxes: Mutex::new(MmltkWorkspaceMailboxes::default()),
        });
        self.mmltk_workspace_dispatcher.shared.register_arena(arena);
        Ok(())
    }

    fn create_mmltk_workspace_texture(
        &self, device_id: id::DeviceId, texture_id: id::TextureId,
        desc: &wgc::resource::TextureDescriptor, id: mmltk_workspace_channel::SurfaceId,
    ) -> Result<(), MmltkWorkspaceImportError> {
        let unsupported = || MmltkWorkspaceImportError::code(mmltk_workspace_channel::FAILED_UNSUPPORTED_DESCRIPTOR);
        let unavailable = || MmltkWorkspaceImportError::code(mmltk_workspace_channel::FAILED_NOT_ADMITTED);
        if desc.format != wgt::TextureFormat::Rgba8Unorm || desc.dimension != wgt::TextureDimension::D2
            || desc.mip_level_count != 1 || desc.sample_count != 1
            || desc.usage != wgt::TextureUsages::TEXTURE_BINDING || !desc.view_formats.is_empty() {
            return Err(unsupported());
        }
        let arena = self.mmltk_workspace_dispatcher.shared.arenas.lock().unwrap().get(&id).cloned();
        let Some(arena) = arena else {
            let source = self.mmltk_workspace_dispatcher.shared.entries.lock().unwrap().values().find(|entry|
                    entry.surface_id == id && entry.device_id == device_id && entry.blit.direct)
                    .map(|entry| {
                        if mmltk_workspace_acceptance_trace_enabled() {
                            entry.blit.trace_binding("page_texture_binding", &entry.arena, entry.surface_id,
                                &entry.frame_signal, entry.physical_read.as_ref().map(|read| read.snapshot), Some(texture_id), true);
                        }
                        (entry.blit.storage.clone(), entry.arena.extent)
                    }).ok_or_else(unavailable)?;
            if desc.size.width != source.1.width || desc.size.height != source.1.height ||
                desc.size.depth_or_array_layers != 1 { return Err(unsupported()); }
            let hal = unsafe { self.device_as_hal::<wgc::api::Vulkan>(device_id) }.ok_or_else(unavailable)?;
            let hal_desc = wgh::TextureDescriptor { label: None, size: desc.size, mip_level_count: 1, sample_count: 1,
                dimension: wgt::TextureDimension::D2, format: wgt::TextureFormat::Rgba8Unorm,
                usage: wgt::TextureUses::RESOURCE, memory_flags: wgh::MemoryFlags::empty(), view_formats: vec![] };
            let storage = source.0;
            let mut texture = unsafe { hal.texture_from_raw(storage.image, &hal_desc,
                Some(Box::new(move || drop(storage))), wgh::vulkan::TextureMemory::External) };
            unsafe { texture.use_external_general_layout(); }
            let (_, error) = unsafe { self.create_texture_from_hal(Box::new(texture), device_id, desc,
                wgt::TextureUses::RESOURCE, Some(texture_id)) };
            if mmltk_workspace_acceptance_trace_enabled() {
                mmltk_workspace_channel::write_diagnostic(|line| write!(line,
                    "{{\"event\":\"firefox.workspace.direct_texture_import\",\"source\":\"{}\",\"texture_id\":\"{:?}\",\"view_layers\":1,\"chosen_layout\":\"GENERAL\",\"outcome\":{}}}",
                    id, texture_id, error.is_none()));
            }
            return if error.is_none() { Ok(()) } else { Err(unavailable()) };
        };
        if arena.device_id != device_id || !arena.ready.load(Ordering::Acquire)
            || desc.size.width != arena.extent.width || desc.size.height != arena.extent.height
            || desc.size.depth_or_array_layers != MMLTK_WORKSPACE_MAILBOX_COUNT as u32 {
            return Err(unsupported());
        }
        let storage = arena.storage.clone().ok_or_else(unavailable)?;
        let hal = unsafe { self.device_as_hal::<wgc::api::Vulkan>(device_id) }.ok_or_else(unavailable)?;
        let hal_desc = wgh::TextureDescriptor { label: None, size: desc.size, mip_level_count: 1, sample_count: 1,
            dimension: wgt::TextureDimension::D2, format: wgt::TextureFormat::Rgba8Unorm,
            usage: wgt::TextureUses::COPY_DST | wgt::TextureUses::RESOURCE,
            memory_flags: wgh::MemoryFlags::empty(), view_formats: vec![] };
        let texture = unsafe { hal.texture_from_raw(storage.image, &hal_desc,
            Some(Box::new(move || drop(storage))), wgh::vulkan::TextureMemory::External) };
        let (_, error) = unsafe { self.create_texture_from_hal(Box::new(texture), device_id, desc,
            wgt::TextureUses::RESOURCE, Some(texture_id)) };
        if error.is_some() { return Err(unavailable()); }
        *arena.texture_id.lock().unwrap() = Some(texture_id);
        self.mmltk_workspace_mirrors.lock().unwrap().insert(texture_id, MmltkWorkspaceMirror {
            arena, dispatcher: self.mmltk_workspace_dispatcher.shared.clone(), active: true });
        Ok(())
    }

    fn create_mmltk_workspace_source(&self, hal: &wgh::vulkan::Device, device_id: id::DeviceId,
        extent: vk::Extent3D, required_direct: Option<bool>)
        -> Result<(vk::Image, mmltk_workspace_channel::Record), MmltkWorkspaceImportError> {
        let unsupported = || MmltkWorkspaceImportError::code(mmltk_workspace_channel::FAILED_UNSUPPORTED_DESCRIPTOR);
        let instance = hal.shared_instance().raw_instance();
        let format_features = unsafe {
            instance.get_physical_device_format_properties(hal.raw_physical_device(), vk::Format::R8G8B8A8_UNORM)
        }.linear_tiling_features;
        let mut supported = None;
        for direct in [true, false] {
            if required_direct.is_some_and(|required| direct != required) { continue; }
            if direct && !format_features.contains(
                vk::FormatFeatureFlags::SAMPLED_IMAGE | vk::FormatFeatureFlags::SAMPLED_IMAGE_FILTER_LINEAR
            ) { continue; }
            let usage = vk::ImageUsageFlags::TRANSFER_SRC
                | if direct { vk::ImageUsageFlags::SAMPLED } else { vk::ImageUsageFlags::empty() };
            let mut external = vk::PhysicalDeviceExternalImageFormatInfo::default()
                .handle_type(vk::ExternalMemoryHandleTypeFlags::OPAQUE_FD);
            let query = vk::PhysicalDeviceImageFormatInfo2::default().format(vk::Format::R8G8B8A8_UNORM)
                .ty(vk::ImageType::TYPE_2D).tiling(vk::ImageTiling::LINEAR).usage(usage)
                .push_next(&mut external);
            let mut external_properties = vk::ExternalImageFormatProperties::default();
            let mut properties = vk::ImageFormatProperties2::default().push_next(&mut external_properties);
            if unsafe { instance.get_physical_device_image_format_properties2(hal.raw_physical_device(), &query, &mut properties) }.is_err() {
                continue;
            }
            let format = properties.image_format_properties;
            let external_memory = external_properties.external_memory_properties;
            if external_memory.external_memory_features.contains(vk::ExternalMemoryFeatureFlags::EXPORTABLE)
                && external_memory.compatible_handle_types.contains(vk::ExternalMemoryHandleTypeFlags::OPAQUE_FD)
                && format.sample_counts.contains(vk::SampleCountFlags::TYPE_1)
                && format.max_mip_levels >= 1 && format.max_array_layers >= 1
                && extent.width <= format.max_extent.width && extent.height <= format.max_extent.height {
                supported = Some((direct, usage, external_memory));
                break;
            }
        }
        let (direct, usage, external_memory) = supported.ok_or_else(unsupported)?;
        let mut external_image = vk::ExternalMemoryImageCreateInfo::default().handle_types(vk::ExternalMemoryHandleTypeFlags::OPAQUE_FD);
        let info = vk::ImageCreateInfo::default().image_type(vk::ImageType::TYPE_2D).format(vk::Format::R8G8B8A8_UNORM)
            .extent(extent).mip_levels(1).array_layers(1).samples(vk::SampleCountFlags::TYPE_1)
            .tiling(vk::ImageTiling::LINEAR).usage(usage)
            .sharing_mode(vk::SharingMode::EXCLUSIVE).initial_layout(vk::ImageLayout::UNDEFINED).push_next(&mut external_image);
        let device = hal.raw_device();
        let image = unsafe { device.create_image(&info, None) }.map_err(|_| unsupported())?;
        let mut dedicated = vk::MemoryDedicatedRequirements::default();
        let mut requirements = vk::MemoryRequirements2::default().push_next(&mut dedicated);
        unsafe { device.get_image_memory_requirements2(&vk::ImageMemoryRequirementsInfo2::default().image(image), &mut requirements) };
        let memory = requirements.memory_requirements;
        let layout = unsafe { device.get_image_subresource_layout(image,
            vk::ImageSubresource::default().aspect_mask(vk::ImageAspectFlags::COLOR)) };
        let mut identifiers = vk::PhysicalDeviceIDProperties::default();
        let mut device_properties = vk::PhysicalDeviceProperties2::default().push_next(&mut identifiers);
        unsafe { instance.get_physical_device_properties2(hal.raw_physical_device(), &mut device_properties) };
        let Some(required) = layout.row_pitch.checked_mul(u64::from(extent.height)).and_then(|size| size.checked_add(layout.offset)) else {
            unsafe { device.destroy_image(image, None) }; return Err(unsupported());
        };
        let (device_index, device_epoch) = device_id.into_raw().unzip();
        let packet = mmltk_workspace_channel::Record { width: extent.width, height: extent.height,
            stride: layout.row_pitch, size: memory.size.max(required), offset: layout.offset, alignment: memory.alignment,
            device_uuid: identifiers.device_uuid,
            device_incarnation: (u64::from(device_epoch) << 32) | u64::from(device_index),
            memory_type_bits: memory.memory_type_bits,
            dedicated: u32::from(dedicated.requires_dedicated_allocation != 0 ||
                external_memory.external_memory_features.contains(vk::ExternalMemoryFeatureFlags::DEDICATED_ONLY)),
            direct_sampling: u64::from(direct),
            ..Default::default() };
        mmltk_workspace_channel::write_diagnostic(|line| {
            write!(line,
                "{{\"event\":\"firefox.workspace.image_created\",\"browser_process_id\":{},\"vk_device\":\"{:?}\",\"vk_physical_device\":\"{:?}\",\"vk_image\":\"{image:?}\",\"device_incarnation\":{},\"device_uuid\":\"",
                std::process::id(), device.handle(), hal.raw_physical_device(), packet.device_incarnation)?;
            for byte in packet.device_uuid { write!(line, "{byte:02x}")?; }
            write!(line,
                "\",\"capacity_width\":{},\"capacity_height\":{},\"image_depth\":{},\"image_type\":\"{:?}\",\"image_format\":\"{:?}\",\"image_tiling\":\"{:?}\",\"image_usage\":{},\"image_flags\":{},\"image_mip_levels\":{},\"image_array_layers\":{},\"image_samples\":{},\"image_sharing_mode\":\"{:?}\",\"image_initial_layout\":\"{:?}\",\"external_handle_type\":\"OPAQUE_FD\",\"requirements_size\":{},\"requirements_alignment\":{},\"requirements_memory_type_bits\":{},\"requires_dedicated\":{},\"prefers_dedicated\":{},\"external_memory_features\":{},\"row_pitch\":{},\"image_offset\":{},\"subresource_size\":{},\"array_pitch\":{},\"depth_pitch\":{},\"direct_sampling\":{}}}",
                info.extent.width, info.extent.height, info.extent.depth, info.image_type, info.format, info.tiling,
                info.usage.as_raw(), info.flags.as_raw(), info.mip_levels, info.array_layers, info.samples.as_raw(),
                info.sharing_mode, info.initial_layout, memory.size, memory.alignment, memory.memory_type_bits,
                dedicated.requires_dedicated_allocation != 0, dedicated.prefers_dedicated_allocation != 0,
                external_memory.external_memory_features.as_raw(), layout.row_pitch, layout.offset, layout.size,
                layout.array_pitch, layout.depth_pitch, direct)
        });
        Ok((image, packet))
    }

    fn initialize_mmltk_workspace_arena(arena: &MmltkWorkspaceArena) -> Result<(), vk::Result> {
        if arena.storage.is_none() { return Ok(()); }
        let image = arena.destination;
        let device = &arena.device;
        let pool = unsafe { device.create_command_pool(&vk::CommandPoolCreateInfo::default()
            .queue_family_index(arena.queue_family), None) }?;
        let result = (|| {
            let commands = unsafe { device.allocate_command_buffers(&vk::CommandBufferAllocateInfo::default()
                .command_pool(pool).level(vk::CommandBufferLevel::PRIMARY).command_buffer_count(1)) }?;
            let command = commands[0];
            let range = vk::ImageSubresourceRange::default().aspect_mask(vk::ImageAspectFlags::COLOR)
                .level_count(1).layer_count(MMLTK_WORKSPACE_MAILBOX_COUNT as u32);
            let (initial, ready) = mmltk_workspace_initial_transitions(arena.queue_family);
            unsafe {
                device.begin_command_buffer(command, &vk::CommandBufferBeginInfo::default()
                    .flags(vk::CommandBufferUsageFlags::ONE_TIME_SUBMIT))?;
                device.cmd_pipeline_barrier(command, vk::PipelineStageFlags::TOP_OF_PIPE, vk::PipelineStageFlags::TRANSFER,
                    vk::DependencyFlags::empty(), &[], &[], &[mmltk_workspace_image_barrier(initial[1], image, range)]);
                device.cmd_clear_color_image(command, image, vk::ImageLayout::TRANSFER_DST_OPTIMAL,
                    &vk::ClearColorValue { float32: [0.0, 0.0, 0.0, 1.0] }, &[range]);
                device.cmd_pipeline_barrier(command, vk::PipelineStageFlags::TRANSFER, vk::PipelineStageFlags::ALL_COMMANDS,
                    vk::DependencyFlags::empty(), &[], &[], &[mmltk_workspace_image_barrier(ready, image, range)]);
                device.end_command_buffer(command)?;
            }
            submit_mmltk_queue_and_wait(device, arena.queue, &arena.queue_gate, &[command])
        })();
        unsafe { device.destroy_command_pool(pool, None) };
        result
    }

    fn attach_mmltk_workspace_source(&self, id: mmltk_workspace_channel::SurfaceId) -> Result<(), MmltkWorkspaceImportError> {
        let unavailable = || MmltkWorkspaceImportError::code(mmltk_workspace_channel::FAILED_NOT_ADMITTED);
        let invalid = || MmltkWorkspaceImportError::code(mmltk_workspace_channel::FAILED_UNSUPPORTED_DESCRIPTOR);
        let mut admission = mmltk_workspace_channel::take_source_admission(id).ok_or_else(unavailable)?;
        let arena_id = mmltk_workspace_channel::SurfaceId { high: admission.layout.arena_high, low: admission.layout.arena_low };
        let arena = self.mmltk_workspace_dispatcher.shared.arenas.lock().unwrap().get(&arena_id).cloned().ok_or_else(unavailable)?;
        let hal = unsafe { self.device_as_hal::<wgc::api::Vulkan>(arena.device_id) }.ok_or_else(unavailable)?;
        let (source, actual) = if let Some(mut initial) = arena.initial_source.lock().unwrap().take() {
            (std::mem::replace(&mut initial.image, vk::Image::null()), arena.layout)
        } else {
            self.create_mmltk_workspace_source(&hal, arena.device_id, arena.extent, Some(arena.layout.direct_sampling != 0))?
        };
        let described = admission.layout;
        if actual.device_uuid != arena.layout.device_uuid || actual.device_incarnation != arena.layout.device_incarnation
            || described.width != actual.width || described.height != actual.height || described.stride != actual.stride
            || described.offset != actual.offset || described.alignment != actual.alignment || described.dedicated != actual.dedicated
            || described.direct_sampling != actual.direct_sampling
            || described.memory_type_bits != actual.memory_type_bits || described.device_uuid != actual.device_uuid
            || described.device_incarnation != actual.device_incarnation || described.size < actual.size
            || described.allocation_identity == 0 {
            unsafe { hal.raw_device().destroy_image(source, None) }; return Err(invalid());
        }
        let properties = unsafe { hal.shared_instance().raw_instance().get_physical_device_memory_properties(hal.raw_physical_device()) };
        let (storage, allocation) = match self.allocate_mmltk_workspace_memory(&hal, &properties, source, &admission) {
            Ok(storage) => storage,
            Err(error) => return Err(error),
        };
        let edge = admission.take_frame_edge();
        let signal = admission.take_frame_signal().zip(admission.take_access_signal())
            .and_then(|(signal, access)| MmltkWorkspaceFrameSignal::map(signal, access, described.allocation_identity));
        let (Some(edge), Some(signal)) = (edge, signal) else {
            return Err(invalid());
        };
        if !valid_mmltk_workspace_eventfd(edge.as_raw_fd()) {
            return Err(invalid());
        }
        self.start_mmltk_workspace_mirror(&hal, arena, id, storage, allocation, edge, signal)
            .ok_or_else(|| MmltkWorkspaceImportError::code(mmltk_workspace_channel::FAILED_IMPORT))
    }

    fn create_mmltk_workspace_destination(
        &self,
        device: &ash::Device,
        memory_properties: &vk::PhysicalDeviceMemoryProperties,
        extent: vk::Extent3D,
    ) -> Option<(vk::Image, vk::DeviceMemory)> {
        let image_info = vk::ImageCreateInfo::default()
            .image_type(vk::ImageType::TYPE_2D)
            .format(vk::Format::R8G8B8A8_UNORM)
            .extent(extent)
            .mip_levels(1)
            .array_layers(MMLTK_WORKSPACE_MAILBOX_COUNT as u32)
            .samples(vk::SampleCountFlags::TYPE_1)
            .tiling(vk::ImageTiling::OPTIMAL)
            .usage(vk::ImageUsageFlags::TRANSFER_DST | vk::ImageUsageFlags::SAMPLED
                | if mmltk_workspace_channel::workspace_pixel_probes_enabled() {
                    vk::ImageUsageFlags::TRANSFER_SRC
                } else { vk::ImageUsageFlags::empty() })
            .sharing_mode(vk::SharingMode::EXCLUSIVE)
            .initial_layout(vk::ImageLayout::UNDEFINED);
        let image = unsafe { device.create_image(&image_info, None) }.ok()?;
        let requirements = unsafe { device.get_image_memory_requirements(image) };
        let allocate = |image: vk::Image| -> Option<vk::DeviceMemory> {
            let memory_type = select_memory_type(
                memory_properties,
                vk::MemoryPropertyFlags::DEVICE_LOCAL,
                Some(requirements.memory_type_bits),
            )?;
            let mut dedicated = vk::MemoryDedicatedAllocateInfo::default().image(image);
            let allocate_info = vk::MemoryAllocateInfo::default()
                .allocation_size(requirements.size)
                .memory_type_index(memory_type)
                .push_next(&mut dedicated);
            let memory = unsafe { device.allocate_memory(&allocate_info, None) }.ok()?;
            if unsafe { device.bind_image_memory(image, memory, 0) }.is_err() {
                unsafe { device.free_memory(memory, None) };
                return None;
            }
            Some(memory)
        };
        match allocate(image) {
            Some(memory) => Some((image, memory)),
            None => {
                unsafe { device.destroy_image(image, None) };
                None
            }
        }
    }

    /// Records one reusable copy per mailbox slot and registers them with the
    /// shell dispatcher, which selects one on frame edges. Ownership of the
    /// source image and its Firefox-allocated memory transfers in on every path: a
    /// failure before the blit exists releases them here, and once the blit
    /// exists its Drop does. The caller keeps owning only the destination.
    #[allow(clippy::too_many_arguments)]
    fn start_mmltk_workspace_mirror(
        &self,
        hal_device: &wgh::vulkan::Device,
        arena: Arc<MmltkWorkspaceArena>,
        surface_id: mmltk_workspace_channel::SurfaceId,
        storage: MmltkWorkspaceImage,
        allocation: mmltk_workspace_channel::Allocation,
        frame_edge: OwnedFd,
        frame_signal: MmltkWorkspaceFrameSignal,
    ) -> Option<()> {
        let destination = arena.destination;
        let extent = arena.extent;
        let device = hal_device.raw_device();
        let source = storage.image;
        let pool_info = vk::CommandPoolCreateInfo::default()
            .flags(vk::CommandPoolCreateFlags::RESET_COMMAND_BUFFER)
            .queue_family_index(hal_device.queue_family_index());
        let pool = match unsafe { device.create_command_pool(&pool_info, None) } {
            Ok(pool) => pool,
            Err(_) => {
                return None;
            }
        };
        let mut export_info = vk::ExportSemaphoreCreateInfo::default()
            .handle_types(vk::ExternalSemaphoreHandleTypeFlags::OPAQUE_FD);
        let mut timeline_info = vk::SemaphoreTypeCreateInfo::default()
            .semaphore_type(vk::SemaphoreType::TIMELINE)
            .initial_value(0);
        let semaphore_info = vk::SemaphoreCreateInfo::default()
            .push_next(&mut export_info)
            .push_next(&mut timeline_info);
        let timeline = match unsafe { device.create_semaphore(&semaphore_info, None) } {
            Ok(semaphore) => semaphore,
            Err(_) => {
                unsafe { device.destroy_command_pool(pool, None) };
                return None;
            }
        };
        let external_semaphore_fd = khr::external_semaphore_fd::Device::new(
            hal_device.shared_instance().raw_instance(),
            device,
        );
        let get_fd = vk::SemaphoreGetFdInfoKHR::default()
            .semaphore(timeline)
            .handle_type(vk::ExternalSemaphoreHandleTypeFlags::OPAQUE_FD);
        let timeline_descriptor = match unsafe { external_semaphore_fd.get_semaphore_fd(&get_fd) } {
            Ok(descriptor) => unsafe { OwnedFd::from_raw_fd(descriptor) },
            Err(_) => {
                unsafe {
                    device.destroy_semaphore(timeline, None);
                    device.destroy_command_pool(pool, None);
                }
                return None;
            }
        };
        let blit = (|| -> Option<MmltkWorkspaceBlit> {
            let read_commands = if arena.layout.direct_sampling != 0 { 1 } else { MMLTK_WORKSPACE_MAILBOX_COUNT };
            let allocate_info = vk::CommandBufferAllocateInfo::default()
                .command_pool(pool)
                .level(vk::CommandBufferLevel::PRIMARY)
                .command_buffer_count((read_commands + 1) as u32);
            let allocated = unsafe { device.allocate_command_buffers(&allocate_info) }.ok()?;
            if allocated.len() != read_commands + 1 {
                return None;
            }
            let copy_commands = allocated[..read_commands]
                .to_vec()
                .into_boxed_slice();
            let release_commands = allocated[read_commands];
            let pixels = if mmltk_workspace_channel::workspace_pixel_probes_enabled() {
                let allocation = if arena.layout.direct_sampling != 0 {
                    MmltkWorkspacePixels::allocate(hal_device, 1, MmltkWorkspacePixels::DIRECT_BYTES)
                } else {
                    MmltkWorkspacePixels::new(hal_device)
                };
                match allocation {
                    Ok(probe) => Some(probe),
                    Err(vk::Result::ERROR_DEVICE_LOST) => return None,
                    Err(_) => {
                        if arena.layout.direct_sampling == 0 {
                            MmltkWorkspacePixels::report_failure(arena.surface_id, surface_id, MmltkWorkspaceProbeFailure::Allocation, None);
                        }
                        None
                    }
                }
            } else { None };
            Some(MmltkWorkspaceBlit {
                device: device.clone(),
                queue: hal_device.raw_queue(),
                queue_family_index: hal_device.queue_family_index(),
                queue_gate: hal_device.queue_operation_gate(),
                pool,
                copy_commands,
                release_commands,
                timeline,
                submission: Some(hal_device.register_external_timeline_submission(
                    MMLTK_WORKSPACE_MAILBOX_COUNT,
                    release_commands,
                    timeline,
                )),
                source,
                storage: Arc::new(storage),
                direct: arena.layout.direct_sampling != 0,
                pixels,
                destination,
                extent,
            })
        })();
        let Some(blit) = blit else {
            unsafe {
                device.destroy_semaphore(timeline, None);
                device.destroy_command_pool(pool, None);
            }
            return None;
        };
        self.mmltk_workspace_dispatcher.shared.register(arena, surface_id, frame_edge, frame_signal, blit, allocation, timeline_descriptor);
        Some(())
    }

    /// Retires the mirror on `texture_id`, if it has one, before anything can
    /// destroy the image it blits into.
    fn stop_mmltk_workspace_mirror(&self, texture_id: id::TextureId) {
        let mirror = self
            .mmltk_workspace_mirrors
            .lock()
            .unwrap()
            .remove(&texture_id);
        drop(mirror);
    }

    /// Retires every watch on `device_id`. A device that is going away takes
    /// its images with it, so no mirror may outlive it.
    fn stop_mmltk_workspace_mirrors_for_device(&self, device_id: id::DeviceId) {
        let mut retired: Vec<MmltkWorkspaceMirror> = {
            let mut mirrors = self.mmltk_workspace_mirrors.lock().unwrap();
            let claimed: Vec<id::TextureId> = mirrors
                .iter()
                .filter(|(_, mirror)| mirror.arena.device_id == device_id)
                .map(|(texture_id, _)| *texture_id)
                .collect();
            claimed
                .into_iter()
                .filter_map(|texture_id| mirrors.remove(&texture_id))
                .collect()
        };
        mmltk_workspace_channel::fail();
        self.mmltk_workspace_dispatcher.shared.retire_device(device_id);
        for mirror in &mut retired {
            mirror.active = false;
        }
    }

    fn stop_all_mmltk_workspace_mirrors(&self) {
        let mut retired = {
            let mut mirrors = self.mmltk_workspace_mirrors.lock().unwrap();
            mem::take(&mut *mirrors)
        };
        self.mmltk_workspace_dispatcher.shared.retire_all();
        for mirror in retired.values_mut() {
            mirror.active = false;
        }
        drop(retired);
    }

    /// One independent Vulkan allocation per physical native slot. Exporting an
    /// opaque FD gives native retention its own backing reference after retirement.
    fn allocate_mmltk_workspace_memory(
        &self,
        hal_device: &wgh::vulkan::Device,
        memory_properties: &vk::PhysicalDeviceMemoryProperties,
        image: vk::Image,
        admission: &mmltk_workspace_channel::Admission,
    ) -> Result<(MmltkWorkspaceImage, mmltk_workspace_channel::Allocation), MmltkWorkspaceImportError> {
        let unavailable = || MmltkWorkspaceImportError::code(mmltk_workspace_channel::FAILED_IMPORT);
        let invalid = || MmltkWorkspaceImportError::code(mmltk_workspace_channel::FAILED_UNSUPPORTED_DESCRIPTOR);
        let device = hal_device.raw_device();
        let mut storage = MmltkWorkspaceImage {
            device: device.clone(), image, memory: vk::DeviceMemory::null(), direct_memory: None,
            _device_custody: hal_device.external_image_custody(),
        };
        let requirements = unsafe { device.get_image_memory_requirements(image) };
        // Dedicated allocation still permits a larger native minimum (VUID
        // VkMemoryDedicatedAllocateInfo-image-02964). Report the allocated size,
        // and bind only this image to its independent backing.
        if admission.size < requirements.size {
            return Err(invalid());
        }
        let memory_type = select_memory_type(memory_properties, vk::MemoryPropertyFlags::DEVICE_LOCAL,
            Some(requirements.memory_type_bits)).ok_or_else(unavailable)?;
        let mut export = vk::ExportMemoryAllocateInfo::default()
            .handle_types(vk::ExternalMemoryHandleTypeFlags::OPAQUE_FD);
        let mut dedicated = vk::MemoryDedicatedAllocateInfo::default().image(image);
        let mut info = vk::MemoryAllocateInfo::default().allocation_size(admission.size)
            .memory_type_index(memory_type).push_next(&mut export);
        if admission.layout.dedicated != 0 { info = info.push_next(&mut dedicated); }
        let observe = |operation: &str, memory: vk::DeviceMemory, status: vk::Result| {
            mmltk_workspace_channel::write_diagnostic(|line| write!(line,
                "{{\"event\":\"firefox.workspace.{operation}\",\"source\":\"{:016x}{:016x}\",\"surface\":\"{:016x}{:016x}\",\"workspace_allocation\":{},\"browser_process_id\":{},\"vk_device\":\"{:?}\",\"vk_image\":\"{image:?}\",\"vk_memory\":\"{memory:?}\",\"memory_size\":{},\"dedicated\":{},\"vk_status\":{}}}",
                admission.layout.id_high, admission.layout.id_low, admission.layout.arena_high, admission.layout.arena_low,
                admission.layout.allocation_identity, std::process::id(), device.handle(), admission.size,
                admission.layout.dedicated, status.as_raw()));
        };
        let allocation = unsafe { device.allocate_memory(&info, None) };
        let (memory, status) = match &allocation {
            Ok(memory) => (*memory, vk::Result::SUCCESS),
            Err(status) => (vk::DeviceMemory::null(), *status),
        };
        observe("memory_allocation", memory, status);
        storage.memory = allocation.map_err(|_| unavailable())?;
        let bound = unsafe { device.bind_image_memory(image, memory, 0) };
        observe("memory_bound", memory, bound.as_ref().err().copied().unwrap_or(vk::Result::SUCCESS));
        bound.map_err(|_| unavailable())?;
        let external = khr::external_memory_fd::Device::new(hal_device.shared_instance().raw_instance(), device);
        let get_fd = vk::MemoryGetFdInfoKHR::default().memory(memory)
            .handle_type(vk::ExternalMemoryHandleTypeFlags::OPAQUE_FD);
        let descriptor = match unsafe { external.get_memory_fd(&get_fd) } {
            Ok(fd) => unsafe { OwnedFd::from_raw_fd(fd) },
            Err(status) => { observe("memory_export_failed", memory, status); return Err(unavailable()); },
        };
        mmltk_workspace_channel::trace_loaded_libraries();
        mmltk_workspace_channel::write_diagnostic(|line| {
            write!(line,
                "{{\"event\":\"firefox.workspace.memory_export\",\"source\":\"{:016x}{:016x}\",\"surface\":\"{:016x}{:016x}\",\"workspace_allocation\":{},\"browser_process_id\":{},\"vk_device\":\"{:?}\",\"vk_image\":\"{image:?}\",\"vk_memory\":\"{memory:?}\",\"workspace_descriptor\":{},\"memory_size\":{},\"row_pitch\":{},\"image_offset\":{},\"dedicated\":{},\"memory_type_index\":{memory_type},\"external_handle_type\":\"OPAQUE_FD\"",
                admission.layout.id_high, admission.layout.id_low, admission.layout.arena_high, admission.layout.arena_low,
                admission.layout.allocation_identity, std::process::id(), device.handle(), descriptor.as_raw_fd(),
                admission.size, admission.stride, admission.layout.offset, admission.layout.dedicated)?;
            mmltk_workspace_channel::write_descriptor_facts(line, descriptor.as_raw_fd(), None)?;
            write!(line, "}}")
        });
        if admission.layout.direct_sampling != 0 && mmltk_workspace_channel::workspace_pixel_probes_enabled() {
            storage.direct_memory = Some(Box::new(MmltkWorkspaceDirectMemoryFacts::new(image, memory, memory_type, admission.layout)));
        }
        Ok((storage, mmltk_workspace_channel::Allocation { layout: admission.layout, memory: descriptor }))
    }

    fn drop_device_after_completion_shutdown(&self, device_id: id::DeviceId) {
        self.stop_mmltk_workspace_mirrors_for_device(device_id);
        self.device_poll_workers.stop_device(device_id);
        self.global.device_drop(device_id);
    }

    fn create_texture_with_shared_texture_dmabuf(
        &self,
        device_id: id::DeviceId,
        texture_id: id::TextureId,
        desc: &wgc::resource::TextureDescriptor,
        swap_chain_id: Option<SwapChainId>,
    ) -> bool {
        unsafe {
            let ret = wgpu_server_ensure_shared_texture_for_swap_chain(
                self.owner,
                swap_chain_id.unwrap(),
                device_id,
                texture_id,
                desc.size.width,
                desc.size.height,
                desc.format,
                desc.usage,
            );
            if ret != true {
                let msg = c"Failed to create shared texture";
                gfx_critical_note(msg.as_ptr());
                return false;
            }

            let handle = wgpu_server_get_vk_image_handle(self.owner, texture_id);
            if handle.is_null() {
                let msg = c"Failed to get VkImageHandle";
                gfx_critical_note(msg.as_ptr());
                return false;
            }

            let vk_image_wrapper = &*handle;

            let fd = wgpu_server_get_dma_buf_fd(self.owner, texture_id);
            if fd < 0 {
                let msg = c"Failed to get DMABuf fd";
                gfx_critical_note(msg.as_ptr());
                return false;
            }

            let owned_fd = OwnedFd::from_raw_fd(fd as RawFd);

            let Some(hal_device) = self.device_as_hal::<wgc::api::Vulkan>(device_id) else {
                emit_critical_invalid_note("Vulkan device");
                return false;
            };

            let device = hal_device.raw_device();

            let extent = vk::Extent3D {
                width: desc.size.width,
                height: desc.size.height,
                depth: 1,
            };
            let usage_flags = SHARED_CANVAS_IMAGE_USAGE;

            let mut external_image_create_info = vk::ExternalMemoryImageCreateInfo::default()
                .handle_types(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT);

            let memory_plane_layouts: Vec<_> = vk_image_wrapper
                .layouts
                .iter()
                .map(|layout| vk::SubresourceLayout {
                    offset: layout.offset,
                    row_pitch: layout.row_pitch,
                    size: 0,
                    array_pitch: 0,
                    depth_pitch: 0,
                })
                .collect();

            let mut modifier_list = vk::ImageDrmFormatModifierExplicitCreateInfoEXT::default()
                .drm_format_modifier(vk_image_wrapper.modifier)
                .plane_layouts(&memory_plane_layouts);

            let vk_info = vk::ImageCreateInfo::default()
                .flags(vk::ImageCreateFlags::ALIAS)
                .image_type(vk::ImageType::TYPE_2D)
                .format(vk::Format::B8G8R8A8_UNORM)
                .extent(extent)
                .mip_levels(1)
                .array_layers(1)
                .samples(vk::SampleCountFlags::TYPE_1)
                .tiling(vk::ImageTiling::DRM_FORMAT_MODIFIER_EXT)
                .usage(usage_flags)
                .sharing_mode(vk::SharingMode::EXCLUSIVE)
                .initial_layout(vk::ImageLayout::UNDEFINED)
                .push_next(&mut modifier_list)
                .push_next(&mut external_image_create_info);

            let image = match device.create_image(&vk_info, None) {
                Err(err) => {
                    let msg = CString::new(format!(
                        "Failed to get vk::Image: create_image() failed: {:?}",
                        err
                    ))
                    .unwrap();
                    gfx_critical_note(msg.as_ptr());
                    return false;
                }
                Ok(image) => image,
            };

            let memory_req = device.get_image_memory_requirements(image);
            if memory_req.size > vk_image_wrapper.memory_size {
                let msg = c"Invalid memory size";
                gfx_critical_note(msg.as_ptr());
                return false;
            }

            let mut dedicated_memory_info = vk::MemoryDedicatedAllocateInfo::default().image(image);

            let mut import_memory_fd_info = vk::ImportMemoryFdInfoKHR::default()
                .handle_type(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT)
                .fd(owned_fd.into_raw_fd());

            let memory_allocate_info = vk::MemoryAllocateInfo::default()
                .allocation_size(vk_image_wrapper.memory_size)
                .memory_type_index(vk_image_wrapper.memory_type_index)
                .push_next(&mut dedicated_memory_info)
                .push_next(&mut import_memory_fd_info);

            let memory = match device.allocate_memory(&memory_allocate_info, None) {
                Err(err) => {
                    let msg = CString::new(format!(
                        "Failed to get vk::Image: allocate_memory() failed: {:?}",
                        err
                    ))
                    .unwrap();
                    gfx_critical_note(msg.as_ptr());
                    return false;
                }
                Ok(memory) => memory,
            };

            match device.bind_image_memory(image, memory, 0) {
                Ok(()) => {}
                Err(err) => {
                    let msg = CString::new(format!(
                        "Failed to get vk::Image: bind_image_memory() failed: {:?}",
                        err
                    ))
                    .unwrap();
                    gfx_critical_note(msg.as_ptr());
                    return false;
                }
            }

            let hal_desc = wgh::TextureDescriptor {
                label: None,
                size: desc.size,
                mip_level_count: desc.mip_level_count,
                sample_count: desc.sample_count,
                dimension: desc.dimension,
                format: desc.format,
                usage: wgt::TextureUses::COPY_SRC | wgt::TextureUses::COPY_DST | wgt::TextureUses::COLOR_TARGET,
                memory_flags: wgh::MemoryFlags::empty(),
                view_formats: vec![],
            };

            let hal_texture = <wgh::api::Vulkan as wgh::Api>::Device::texture_from_raw(
                &hal_device,
                image,
                &hal_desc,
                None,
                wgh::vulkan::TextureMemory::Dedicated(memory),
            );

            let (_, error) = self.create_texture_from_hal(
                Box::new(hal_texture),
                device_id,
                &desc,
                wgt::TextureUses::UNINITIALIZED,
                Some(texture_id),
            );
            if let Some(err) = error {
                let msg =
                    CString::new(format!("create_texture_from_hal() failed: {:?}", err)).unwrap();
                gfx_critical_note(msg.as_ptr());
                return false;
            }

            true
        }
    }

    fn device_action(
        &self,
        device_id: id::DeviceId,
        action: DeviceAction,
        shmem_mappings: FfiSlice<'_, FfiSlice<'_, u8>>,
        response_byte_buf: &mut ByteBuf,
        error_buf: &mut OwnedErrorBuffer,
    ) {
        match action {
            DeviceAction::CreateBuffer {
                buffer_id,
                desc,
                shmem_handle_index,
            } => {
                let has_map_flags = desc
                    .usage
                    .intersects(wgt::BufferUsages::MAP_READ | wgt::BufferUsages::MAP_WRITE);
                let needs_shmem = has_map_flags || desc.mapped_at_creation;

                let shmem_data =
                    unsafe { shmem_mappings.as_slice()[shmem_handle_index].as_slice() };

                let shmem_size = shmem_data.len();

                let shmem_allocation_failed = needs_shmem && (shmem_size as u64) < desc.size;
                if shmem_allocation_failed {
                    assert_eq!(shmem_size, 0);
                }

                if shmem_allocation_failed || desc.size > MAX_BUFFER_SIZE {
                    error_buf.init(ErrMsg::oom(), device_id);
                    self.create_buffer_error(device_id, Some(buffer_id), &desc);
                    return;
                }

                if needs_shmem {
                    unsafe {
                        wgpu_server_set_buffer_map_data(
                            self.owner,
                            device_id,
                            buffer_id,
                            has_map_flags,
                            0,
                            if desc.mapped_at_creation {
                                desc.size
                            } else {
                                0
                            },
                            shmem_handle_index,
                        );
                    }
                }

                let (_, error) = self.device_create_buffer(device_id, &desc, Some(buffer_id));
                if let Some(err) = error {
                    error_buf.init(err, device_id);
                }
            }
            #[allow(unused_variables)]
            DeviceAction::CreateTexture(id, desc, swap_chain_id) => {
                let desc = if let Some(swap_chain_id) = swap_chain_id {
                    self.swap_chain_configs
                        .lock()
                        .unwrap()
                        .get(&swap_chain_id)
                        .cloned()
                        .expect("CreateTexture for unknown swap chain {swap_chain_id:?}")
                        .to_texture_descriptor()
                } else {
                    desc
                };

                unsafe {
                    assert!(wgpu_texture_format_is_valid_for_webidl(&nsCString::from(
                        serde_json::to_value(&desc.format)
                            .unwrap()
                            .as_str()
                            .unwrap(),
                    ),));
                }

                let max = MAX_TEXTURE_EXTENT;
                if desc.size.width > max
                    || desc.size.height > max
                    || desc.size.depth_or_array_layers > max
                {
                    self.create_texture_error(device_id, Some(id), &desc);
                    error_buf.init(ErrMsg::oom(), device_id);
                    return;
                }

                if [
                    desc.size.width,
                    desc.size.height,
                    desc.size.depth_or_array_layers,
                ]
                .contains(&0)
                {
                    self.create_texture_error(device_id, Some(id), &desc);
                    error_buf.init(
                        ErrMsg {
                            message: "size is zero".into(),
                            r#type: ErrorType::Validation,
                        },
                        device_id,
                    );
                    return;
                }

                if let Some(workspace_id) = self.parse_mmltk_workspace_label(&desc) {
                    match self.create_mmltk_workspace_texture(device_id, id, &desc, workspace_id) {
                        Ok(()) => {},
                        Err(error) => {
                            if mmltk_workspace_channel::workspace_diagnostics_enabled() {
                                mmltk_workspace_channel::write_diagnostic(|line| write!(line,
                                    "{{\"event\":\"firefox.workspace.texture_creation_failed\",\"surface\":\"{workspace_id}\",\"code\":{},\"stride\":{},\"size\":{}}}",
                                    error.code, error.stride, error.size
                                ));
                            }
                            mmltk_workspace_channel::send_failed(
                                workspace_id,
                                error.code,
                                error.stride,
                                error.size,
                            );
                            self.create_texture_error(device_id, Some(id), &desc);
                            // A layout reply is the agreed way this device names
                            // the pitch it requires, and the host answers it with
                            // a fresh allocation. Reporting a device error for a
                            // negotiation that is proceeding would fail the page
                            // for a step that is working.
                            if error.code != mmltk_workspace_channel::FAILED_LAYOUT {
                                error_buf.init(
                                    ErrMsg {
                                        message: format!(
                                            "workspace allocation {workspace_id} was not imported ({})",
                                            error.code
                                        )
                                        .into(),
                                        r#type: ErrorType::Internal,
                                    },
                                    device_id,
                                );
                            }
                        }
                    }
                    return;
                }

                let use_shared_texture = if let Some(id) = swap_chain_id {
                    unsafe { wgpu_server_use_shared_texture_for_swap_chain(self.owner, id) }
                } else {
                    false
                };

                if use_shared_texture {
                    let limits = self.device_limits(device_id);
                    if desc.size.width > limits.max_texture_dimension_2d
                        || desc.size.height > limits.max_texture_dimension_2d
                    {
                        self.create_texture_error(device_id, Some(id), &desc);
                        error_buf.init(
                            ErrMsg {
                                message: "size exceeds limits.max_texture_dimension_2d".into(),
                                r#type: ErrorType::Validation,
                            },
                            device_id,
                        );
                        return;
                    }

                    let features = self.device_features(device_id);
                    if desc.format == wgt::TextureFormat::Bgra8Unorm
                        && desc.usage.contains(wgt::TextureUsages::STORAGE_BINDING)
                        && !features.contains(wgt::Features::BGRA8UNORM_STORAGE)
                    {
                        self.create_texture_error(device_id, Some(id), &desc);
                        error_buf.init(
                            ErrMsg {
                                message: concat!(
                                    "Bgra8Unorm with GPUStorageBinding usage ",
                                    "with BGRA8UNORM_STORAGE disabled"
                                )
                                .into(),
                                r#type: ErrorType::Validation,
                            },
                            device_id,
                        );
                        return;
                    }

                    {
                        let is_created = self.create_texture_with_shared_texture_dmabuf(
                            device_id,
                            id,
                            &desc,
                            swap_chain_id,
                        );
                        if is_created {
                            return;
                        }
                    }

                    unsafe {
                        wgpu_server_disable_shared_texture_for_swap_chain(
                            self.owner,
                            swap_chain_id.unwrap(),
                        )
                    };
                }

                if let Some(swap_chain_id) = swap_chain_id {
                    unsafe {
                        wgpu_server_ensure_shared_texture_for_readback(
                            self.owner,
                            swap_chain_id,
                            device_id,
                            id,
                            desc.size.width,
                            desc.size.height,
                            desc.format,
                            desc.usage,
                        )
                    };
                }

                let (_, error) = self.device_create_texture(device_id, &desc, Some(id));
                if let Some(err) = error {
                    error_buf.init(err, device_id);
                }
            }
            DeviceAction::CreateExternalTexture(id, desc) => {
                let source_desc = desc.source.and_then(|source| {
                    let source_desc = unsafe {
                        wgpu_parent_external_texture_source_get_external_texture_descriptor(
                            self.owner,
                            source,
                            desc.color_space,
                        )
                    };
                    let planes = unsafe { source_desc.planes.as_slice() };
                    if planes.is_empty() {
                        None
                    } else {
                        Some(source_desc)
                    }
                });
                match source_desc {
                    Some(source_desc) => {
                        let planes = unsafe { source_desc.planes.as_slice() };
                        let desc = wgt::ExternalTextureDescriptor {
                            label: desc.label,
                            width: source_desc.width,
                            height: source_desc.height,
                            format: source_desc.format,
                            yuv_conversion_matrix: source_desc.yuv_conversion_matrix,
                            gamut_conversion_matrix: source_desc.gamut_conversion_matrix,
                            src_transfer_function: source_desc.src_transfer_function,
                            dst_transfer_function: source_desc.dst_transfer_function,
                            sample_transform: source_desc.sample_transform,
                            load_transform: source_desc.load_transform,
                        };
                        let (_, error) =
                            self.device_create_external_texture(device_id, &desc, planes, Some(id));
                        if let Some(err) = error {
                            error_buf.init(err, device_id);
                        }
                    }
                    None => {
                        let desc = wgt::ExternalTextureDescriptor {
                            label: desc.label,
                            width: 0,
                            height: 0,
                            format: wgt::ExternalTextureFormat::Rgba,
                            yuv_conversion_matrix: Default::default(),
                            gamut_conversion_matrix: Default::default(),
                            src_transfer_function: Default::default(),
                            dst_transfer_function: Default::default(),
                            sample_transform: Default::default(),
                            load_transform: Default::default(),
                        };
                        self.create_external_texture_error(device_id, Some(id), &desc);
                    }
                }
            }
            DeviceAction::CreateSampler(id, desc) => {
                let (_, error) = self.device_create_sampler(device_id, &desc, Some(id));
                if let Some(err) = error {
                    error_buf.init(err, device_id);
                }
            }
            DeviceAction::CreateBindGroupLayout(id, desc) => {
                let (_, error) = self.device_create_bind_group_layout(device_id, &desc, Some(id));
                if let Some(err) = error {
                    error_buf.init(err, device_id);
                }
            }
            DeviceAction::CreateBindGroupLayoutError(id, label) => {
                self.create_bind_group_layout_error(device_id, Some(id), label);
            }
            DeviceAction::RenderPipelineGetBindGroupLayout(pipeline_id, index, bgl_id) => {
                let (_, error) =
                    self.render_pipeline_get_bind_group_layout(pipeline_id, index, Some(bgl_id));
                if let Some(err) = error {
                    error_buf.init(err, device_id);
                }
            }
            DeviceAction::ComputePipelineGetBindGroupLayout(pipeline_id, index, bgl_id) => {
                let (_, error) =
                    self.compute_pipeline_get_bind_group_layout(pipeline_id, index, Some(bgl_id));
                if let Some(err) = error {
                    error_buf.init(err, device_id);
                }
            }
            DeviceAction::CreatePipelineLayout(id, desc) => {
                let (_, error) = self.device_create_pipeline_layout(device_id, &desc, Some(id));
                if let Some(err) = error {
                    error_buf.init(err, device_id);
                }
            }
            DeviceAction::CreateBindGroup(id, desc) => {
                let (_, error) = self.device_create_bind_group(device_id, &desc, Some(id));
                if let Some(err) = error {
                    error_buf.init(err, device_id);
                }
            }
            DeviceAction::CreateShaderModule(id, label, code) => {
                let desc = wgc::pipeline::ShaderModuleDescriptor {
                    label,
                    runtime_checks: wgt::ShaderRuntimeChecks::checked(),
                };
                let source = wgc::pipeline::ShaderModuleSource::Wgsl(Cow::Borrowed(code.as_ref()));
                let (_, error) =
                    self.device_create_shader_module(device_id, &desc, source, Some(id));

                let compilation_messages = if let Some(err) = error {
                    let message = match &err {
                        CreateShaderModuleError::Parsing(error) => error.to_string(),
                        CreateShaderModuleError::Validation(error) => error.to_string(),
                        CreateShaderModuleError::Device(device_err) => format!("{device_err:?}"),
                        _ => format!("{err:?}"),
                    };

                    error_buf.init(
                        ErrMsg {
                            message: format!("Shader module creation failed: {message}").into(),
                            r#type: err.webgpu_error_type(),
                        },
                        device_id,
                    );

                    vec![ShaderModuleCompilationMessage::new(&err, code.as_ref())]
                } else {
                    Vec::new()
                };

                *response_byte_buf = make_byte_buf(&ServerMessage::CreateShaderModuleResponse(
                    id,
                    compilation_messages,
                ));
            }
            DeviceAction::CreateComputePipeline(id, desc, is_async) => {
                let (_, error) = self.device_create_compute_pipeline(device_id, &desc, Some(id));

                if is_async {
                    let error = error
                        .filter(|e| !matches!(e.webgpu_error_type(), ErrorType::DeviceLost))
                        .map(|e| -> _ {
                            let is_validation_error =
                                matches!(e.webgpu_error_type(), ErrorType::Validation);
                            PipelineError {
                                is_validation_error,
                                error: error_to_string(e),
                            }
                        });
                    *response_byte_buf =
                        make_byte_buf(&ServerMessage::CreateComputePipelineResponse {
                            pipeline_id: id,
                            error,
                        });
                } else {
                    if let Some(err) = error {
                        error_buf.init(err, device_id);
                    }
                }
            }
            DeviceAction::CreateRenderPipeline(id, desc, is_async) => {
                let (_, error) = self.device_create_render_pipeline(device_id, &desc, Some(id));

                if is_async {
                    let error = error
                        .filter(|e| !matches!(e.webgpu_error_type(), ErrorType::DeviceLost))
                        .map(|e| -> _ {
                            let is_validation_error =
                                matches!(e.webgpu_error_type(), ErrorType::Validation);
                            PipelineError {
                                is_validation_error,
                                error: error_to_string(e),
                            }
                        });
                    *response_byte_buf =
                        make_byte_buf(&ServerMessage::CreateRenderPipelineResponse {
                            pipeline_id: id,
                            error,
                        });
                } else {
                    if let Some(err) = error {
                        error_buf.init(err, device_id);
                    }
                }
            }
            DeviceAction::CreateRenderBundle(id, mut encoder, desc) => {
                let (_, error) = self.render_bundle_encoder_finish(&mut encoder, &desc, Some(id));
                if let Some(err) = error {
                    error_buf.init(err, device_id);
                }
            }
            DeviceAction::CreateRenderBundleError(buffer_id, label) => {
                self.create_render_bundle_error(
                    device_id,
                    Some(buffer_id),
                    &wgt::RenderBundleDescriptor { label },
                );
            }
            DeviceAction::CreateQuerySet(id, desc) => {
                let (_, error) = self.device_create_query_set(device_id, &desc, Some(id));
                if let Some(err) = error {
                    error_buf.init(err, device_id);
                }
            }
            DeviceAction::CreateCommandEncoder(id, desc) => {
                let (_, error) = self.device_create_command_encoder(device_id, &desc, Some(id));
                if let Some(err) = error {
                    error_buf.init(err, device_id);
                }
            }
            DeviceAction::Error { message, r#type } => {
                error_buf.init(
                    ErrMsg {
                        message: message.into(),
                        r#type,
                    },
                    device_id,
                );
            }
            DeviceAction::PushErrorScope(filter) => {
                unsafe { wgpu_server_device_push_error_scope(self.owner, device_id, filter) };
            }
            DeviceAction::PopErrorScope => {
                let mut ty = 0;
                let mut message = nsCString::new();
                unsafe {
                    wgpu_server_device_pop_error_scope(self.owner, device_id, &mut ty, &mut message)
                };
                let message = message.to_utf8();

                *response_byte_buf = make_byte_buf(&ServerMessage::PopErrorScopeResponse(
                    device_id, ty, message,
                ));
            }
        }
    }

    fn texture_action(
        &self,
        device_id: id::DeviceId,
        self_id: id::TextureId,
        action: TextureAction,
        error_buf: &mut OwnedErrorBuffer,
    ) {
        match action {
            TextureAction::CreateView(id, desc) => {
                let (_, error) = self.texture_create_view(self_id, &desc, Some(id));
                if let Some(err) = error {
                    error_buf.init(err, device_id);
                }
            }
        }
    }

    fn command_encoder_action(
        &self,
        device_id: id::DeviceId,
        self_id: id::CommandEncoderId,
        action: CommandEncoderAction,
        error_buf: &mut OwnedErrorBuffer,
    ) {
        match action {
            CommandEncoderAction::CopyBufferToBuffer {
                src,
                src_offset,
                dst,
                dst_offset,
                size,
            } => {
                if let Err(err) = self.command_encoder_copy_buffer_to_buffer(
                    self_id, src, src_offset, dst, dst_offset, size,
                ) {
                    error_buf.init(err, device_id);
                }
            }
            CommandEncoderAction::CopyBufferToTexture { src, dst, size } => {
                if let Err(err) =
                    self.command_encoder_copy_buffer_to_texture(self_id, &src, &dst, &size)
                {
                    error_buf.init(err, device_id);
                }
            }
            CommandEncoderAction::CopyTextureToBuffer { src, dst, size } => {
                if let Err(err) =
                    self.command_encoder_copy_texture_to_buffer(self_id, &src, &dst, &size)
                {
                    error_buf.init(err, device_id);
                }
            }
            CommandEncoderAction::CopyTextureToTexture { src, dst, size } => {
                if let Err(err) =
                    self.command_encoder_copy_texture_to_texture(self_id, &src, &dst, &size)
                {
                    error_buf.init(err, device_id);
                }
            }
            CommandEncoderAction::RunComputePass { .. } => unimplemented!(),
            CommandEncoderAction::WriteTimestamp {
                query_set,
                query_index,
            } => {
                if let Err(err) =
                    self.command_encoder_write_timestamp(self_id, query_set, query_index)
                {
                    error_buf.init(err, device_id);
                }
            }
            CommandEncoderAction::ResolveQuerySet {
                query_set,
                start_query,
                query_count,
                destination,
                destination_offset,
            } => {
                if let Err(err) = self.command_encoder_resolve_query_set(
                    self_id,
                    query_set,
                    start_query,
                    query_count,
                    destination,
                    destination_offset,
                ) {
                    error_buf.init(err, device_id);
                }
            }
            CommandEncoderAction::RunRenderPass { .. } => unimplemented!(),
            CommandEncoderAction::ClearBuffer { dst, offset, size } => {
                if let Err(err) = self.command_encoder_clear_buffer(self_id, dst, offset, size) {
                    error_buf.init(err, device_id);
                }
            }
            CommandEncoderAction::ClearTexture {
                dst,
                ref subresource_range,
            } => {
                if let Err(err) =
                    self.command_encoder_clear_texture(self_id, dst, subresource_range)
                {
                    error_buf.init(err, device_id);
                }
            }
            CommandEncoderAction::PushDebugGroup(marker) => {
                if let Err(err) = self.command_encoder_push_debug_group(self_id, &marker) {
                    error_buf.init(err, device_id);
                }
            }
            CommandEncoderAction::PopDebugGroup => {
                if let Err(err) = self.command_encoder_pop_debug_group(self_id) {
                    error_buf.init(err, device_id);
                }
            }
            CommandEncoderAction::InsertDebugMarker(marker) => {
                if let Err(err) = self.command_encoder_insert_debug_marker(self_id, &marker) {
                    error_buf.init(err, device_id);
                }
            }
            CommandEncoderAction::BuildAccelerationStructures { .. } => {
                unreachable!("internal error: attempted to build acceleration structures")
            }
            CommandEncoderAction::TransitionResources { .. } => {
                unreachable!("internal error: attempted to transition resources")
            }
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn wgpu_server_pack_buffer_map_success(
    buffer_id: id::BufferId,
    is_writable: bool,
    offset: u64,
    size: u64,
    bb: &mut ByteBuf,
) {
    let result = BufferMapResult::Success {
        is_writable,
        offset,
        size,
    };
    *bb = make_byte_buf(&ServerMessage::BufferMapResponse(buffer_id, result));
}

#[no_mangle]
pub unsafe extern "C" fn wgpu_server_pack_buffer_map_error(
    buffer_id: id::BufferId,
    error: &nsACString,
    bb: &mut ByteBuf,
) {
    let error = error.to_utf8();
    let result = BufferMapResult::Error(error);
    *bb = make_byte_buf(&ServerMessage::BufferMapResponse(buffer_id, result));
}

#[no_mangle]
pub unsafe extern "C" fn wgpu_server_pack_work_done(bb: &mut ByteBuf, queue_id: id::QueueId) {
    *bb = make_byte_buf(&ServerMessage::QueueOnSubmittedWorkDoneResponse(queue_id));
}

/// # Panics
///
/// If the size of `buffer_ids` is not [`crate::MAX_SWAPCHAIN_BUFFER_COUNT`].
#[no_mangle]
pub unsafe extern "C" fn wgpu_server_pack_free_swap_chain_buffer_ids(
    bb: &mut ByteBuf,
    buffer_ids: FfiSlice<'_, id::BufferId>,
) {
    *bb = make_byte_buf(&ServerMessage::FreeSwapChainBufferIds(
        buffer_ids.as_slice().try_into().unwrap(),
    ));
}

#[no_mangle]
pub unsafe extern "C" fn wgpu_server_messages(
    global: &Global,
    nr_of_messages: u32,
    serialized_messages: &ByteBuf,
    data_buffers: FfiSlice<'_, ByteBuf>,
    shmem_mappings: FfiSlice<'_, FfiSlice<'_, u8>>,
) {
    let serialized_messages = serialized_messages.as_slice();
    let data_buffers = data_buffers.as_slice();

    use bincode::Options;
    let options = bincode::DefaultOptions::new()
        .with_fixint_encoding()
        .allow_trailing_bytes();
    let mut deserializer = bincode::Deserializer::from_slice(serialized_messages, options);

    for _ in 0..nr_of_messages {
        let message: Message = serde::Deserialize::deserialize(&mut deserializer).unwrap();
        process_message(global, data_buffers, shmem_mappings, message);
    }
}

fn process_buffer_map(
    global: &Global,
    msg: Message,
    response_byte_buf: &mut ByteBuf,
    error_buf: &mut OwnedErrorBuffer,
) {
    let Message::BufferMap {
        device_id,
        buffer_id,
        mode,
        offset,
        size,
    } = msg
    else {
        unreachable!();
    };
    let mode = match mode {
        1 => wgc::device::HostMap::Read,
        2 => wgc::device::HostMap::Write,
        _ => {
            let message = concat!(
                "GPUBuffer.mapAsync 'mode' argument must be ",
                "either GPUMapMode.READ or GPUMapMode.WRITE"
            );

            error_buf.init(
                ErrMsg {
                    message: message.into(),
                    r#type: ErrorType::Validation,
                },
                device_id,
            );

            let response = BufferMapResult::Error(message.into());
            *response_byte_buf =
                make_byte_buf(&ServerMessage::BufferMapResponse(buffer_id, response));
            return;
        }
    };

    let closure = unsafe {
        wgpu_parent_build_buffer_map_closure(global.owner, device_id, buffer_id, mode, offset, size)
    };

    let (map_result_sender, map_result_receiver) = futures_channel::oneshot::channel();

    moz_task::spawn_local("process_buffer_map callback", async move {
        let result = map_result_receiver.await.unwrap();
        unsafe {
            (closure.callback)(closure.user_data, BufferMapAsyncStatus::from(result));
        }
    })
    .detach();

    let operation = wgc::resource::BufferMapOperation {
        host: mode,
        callback: Some(Box::new(move |result| {
            map_result_sender.send(result).unwrap();
        })),
    };
    match global.buffer_map_async(buffer_id, offset, Some(size), operation) {
        Ok(submission_index) => {
            global
                .device_poll_workers
                .wait_for(device_id, submission_index);
        }
        Err(error) => {
            error_buf.init(error, device_id);
        }
    }
}

unsafe fn process_message(
    global: &Global,
    data_buffers: &[ByteBuf],
    shmem_mappings: FfiSlice<'_, FfiSlice<'_, u8>>,
    message: Message,
) {
    let response_byte_buf = &mut ByteBuf::new();
    let error_buf = &mut OwnedErrorBuffer::new();

    match message {
        Message::RequestAdapter {
            adapter_id,
            power_preference,
            force_fallback_adapter,
        } => {
            let desc = wgt::RequestAdapterOptions {
                power_preference,
                force_fallback_adapter,
                compatible_surface: None,
                apply_limit_buckets: false,
            };
            let created =
                match global.request_adapter(&desc, wgt::Backends::VULKAN, Some(adapter_id)) {
                    Ok(_) => true,
                    Err(e) => {
                        log::warn!("{e}");
                        false
                    }
                };

            let response = if created {
                let wgt::AdapterInfo {
                    name,
                    vendor,
                    device,
                    device_type,
                    driver,
                    driver_info,
                    backend,
                    transient_saves_memory: _,
                    device_pci_bus_id: _,
                    subgroup_min_size,
                    subgroup_max_size,
                    limit_bucket: _,
                } = global.adapter_get_info(adapter_id);

                let is_hardware = match device_type {
                    wgt::DeviceType::IntegratedGpu | wgt::DeviceType::DiscreteGpu => true,
                    _ => false,
                };

                let support_use_shared_texture_in_swap_chain =
                    support_use_shared_texture_in_swap_chain(
                        global,
                        adapter_id,
                        backend,
                        is_hardware,
                    );

                let info = AdapterInformation {
                    id: adapter_id,
                    limits: restrict_limits(global.adapter_limits(adapter_id)),
                    features: global.adapter_features(adapter_id).features_webgpu,
                    name: Cow::Owned(name),
                    vendor,
                    device,
                    device_type,
                    driver: Cow::Owned(driver),
                    driver_info: Cow::Owned(driver_info),
                    backend,
                    support_use_shared_texture_in_swap_chain,
                    subgroup_min_size,
                    subgroup_max_size,
                };
                Some(info)
            } else {
                None
            };

            *response_byte_buf =
                make_byte_buf(&ServerMessage::RequestAdapterResponse(adapter_id, response));
        }
        Message::RequestDevice {
            adapter_id,
            device_id,
            queue_id,
            desc,
        } => {
            let error = adapter_request_device(global, adapter_id, desc, device_id, queue_id);

            if error.is_none() {
                wgpu_parent_post_request_device(global.owner, device_id);
            }

            *response_byte_buf = make_byte_buf(&ServerMessage::RequestDeviceResponse(
                device_id, queue_id, error,
            ));
        }
        Message::Device(id, action) => {
            global.device_action(id, action, shmem_mappings, response_byte_buf, error_buf)
        }
        Message::Texture(device_id, id, action) => {
            global.texture_action(device_id, id, action, error_buf)
        }
        Message::CommandEncoder(device_id, id, action) => {
            global.command_encoder_action(device_id, id, action, error_buf)
        }
        Message::CommandEncoderFinish(device_id, command_encoder_id, command_buffer_id, desc) => {
            let (_, label_and_error) =
                global.command_encoder_finish(command_encoder_id, &desc, Some(command_buffer_id));
            if let Some((_label, err)) = label_and_error {
                error_buf.init(err, device_id);
            }
        }
        Message::ReplayRenderPass(device_id, id, pass) => {
            crate::command::replay_render_pass(global, device_id, id, &pass, error_buf);
        }
        Message::ReplayComputePass(device_id, id, pass) => {
            crate::command::replay_compute_pass(global, device_id, id, &pass, error_buf);
        }
        Message::QueueWrite {
            device_id,
            queue_id,
            data_source,
            action,
        } => {
            let data = match data_source {
                QueueWriteDataSource::DataBuffer(data_buffer_index) => {
                    data_buffers[data_buffer_index].as_slice()
                }
                QueueWriteDataSource::Shmem(shmem_handle_index) => {
                    shmem_mappings.as_slice()[shmem_handle_index].as_slice()
                }
            };
            let result = match action {
                QueueWriteAction::Buffer { dst, offset } => {
                    global.queue_write_buffer(queue_id, dst, offset, data)
                }
                QueueWriteAction::Texture { dst, layout, size } => {
                    global.queue_write_texture(queue_id, &dst, data, &layout, &size)
                }
            };
            if let Err(err) = result {
                error_buf.init(err, device_id);
            }
        }
        msg @ Message::BufferMap { .. } => {
            process_buffer_map(global, msg, response_byte_buf, error_buf);
        }
        Message::BufferUnmap(device_id, buffer_id, flush) => {
            wgpu_parent_buffer_unmap(global.owner, device_id, buffer_id, flush);
        }
        Message::QueueSubmit(
            device_id,
            queue_id,
            command_buffer_ids,
            texture_ids,
            external_texture_source_ids,
        ) => {
            wgpu_parent_queue_submit(
                global.owner,
                device_id,
                queue_id,
                command_buffer_ids.as_ptr(),
                command_buffer_ids.len(),
                texture_ids.as_ptr(),
                texture_ids.len(),
                external_texture_source_ids.as_ptr(),
                external_texture_source_ids.len(),
            );
        }
        Message::QueueOnSubmittedWorkDone {
            device_id,
            queue_id,
        } => {
            let closure = wgpu_parent_build_submitted_work_done_closure(global.owner, queue_id);
            let (work_done_sender, work_done_receiver) = futures_channel::oneshot::channel::<()>();
            moz_task::spawn_local("WebGPU onSubmittedWorkDone callback", async move {
                work_done_receiver.await.unwrap();
                (closure.callback)(closure.user_data)
            })
            .detach();
            let completion_submission = global.queue_on_submitted_work_done(
                queue_id,
                Box::new(move || {
                    let _ = work_done_sender.send(());
                }),
            );
            global
                .device_poll_workers
                .wait_for(device_id, completion_submission);
        }

        Message::CreateSwapChain {
            device_id,
            queue_id,
            width,
            height,
            format,
            texture_format,
            usage,
            view_formats,
            buffer_ids,
            remote_texture_owner_id,
            use_shared_texture_in_swap_chain,
        } => {
            global.swap_chain_configs.lock().unwrap().insert(
                SwapChainId(remote_texture_owner_id.0),
                SwapChainConfig {
                    size: wgt::Extent3d {
                        width: width as u32,
                        height: height as u32,
                        depth_or_array_layers: 1,
                    },
                    format: texture_format,
                    usage,
                    view_formats,
                },
            );
            wgpu_parent_create_swap_chain(
                global.owner,
                device_id,
                queue_id,
                width,
                height,
                format,
                buffer_ids.as_ptr(),
                buffer_ids.len(),
                remote_texture_owner_id,
                use_shared_texture_in_swap_chain,
            );
        }
        Message::SwapChainPresent {
            texture_id,
            command_encoder_id,
            command_buffer_id,
            remote_texture_id,
            remote_texture_owner_id,
        } => {
            wgpu_parent_swap_chain_present(
                global.owner,
                texture_id,
                command_encoder_id,
                command_buffer_id,
                remote_texture_id,
                remote_texture_owner_id,
            );
        }
        Message::SwapChainDrop {
            remote_texture_owner_id,
            txn_type,
            txn_id,
        } => {
            global
                .swap_chain_configs
                .lock()
                .unwrap()
                .remove(&SwapChainId(remote_texture_owner_id.0));
            wgpu_parent_swap_chain_drop(global.owner, remote_texture_owner_id, txn_type, txn_id);
        }

        Message::DestroyBuffer(id) => {
            wgpu_server_dealloc_buffer_shmem(global.owner, id);
            global.buffer_destroy(id)
        }
        Message::DestroyTexture(id) => {
            wgpu_server_remove_shared_texture(global.owner, id);
            // Routes through the entry point that retires the workspace mirror
            // before the image it blits into can be freed, exactly like
            // DropTexture below.
            wgpu_server_texture_destroy(global, id);
        }
        Message::DestroyExternalTexture(id) => global.external_texture_destroy(id),
        Message::DestroyExternalTextureSource(id) => {
            wgpu_parent_destroy_external_texture_source(global.owner, id)
        }
        Message::DestroyDevice(id) => global.device_destroy(id),

        Message::DropAdapter(id) => global.adapter_drop(id),
        Message::DropDevice(id) => {
            wgpu_server_pre_device_drop(global.owner, id);
            global.drop_device_after_completion_shutdown(id)
        }
        Message::DropQueue(id) => global.queue_drop(id),
        Message::DropBuffer(id) => {
            wgpu_server_dealloc_buffer_shmem(global.owner, id);
            global.buffer_drop(id)
        }
        Message::DropCommandEncoder(id) => global.command_encoder_drop(id),
        Message::DropRenderPassEncoder(_id) => {}
        Message::DropComputePassEncoder(_id) => {}
        Message::DropRenderBundleEncoder(_id) => {}
        Message::DropCommandBuffer(id) => global.command_buffer_drop(id),
        Message::DropRenderBundle(id) => global.render_bundle_drop(id),
        Message::DropBindGroupLayout(id) => global.bind_group_layout_drop(id),
        Message::DropPipelineLayout(id) => global.pipeline_layout_drop(id),
        Message::DropBindGroup(id) => global.bind_group_drop(id),
        Message::DropShaderModule(id) => global.shader_module_drop(id),
        Message::DropComputePipeline(id) => global.compute_pipeline_drop(id),
        Message::DropRenderPipeline(id) => global.render_pipeline_drop(id),
        Message::DropTexture(id) => {
            wgpu_server_remove_shared_texture(global.owner, id);
            wgpu_server_texture_drop(global, id);
        }
        Message::DropTextureView(id) => wgpu_server_texture_view_drop(global, id),
        Message::DropExternalTexture(id) => global.external_texture_drop(id),
        Message::DropExternalTextureSource(id) => {
            wgpu_parent_drop_external_texture_source(global.owner, id)
        }
        Message::DropSampler(id) => global.sampler_drop(id),
        Message::DropQuerySet(id) => global.query_set_drop(id),
    }

    if let Some((device_id, ty, message)) = error_buf.get_inner_data() {
        wgpu_parent_handle_error(global.owner, device_id, ty, message);
    }
    if !response_byte_buf.is_empty() {
        wgpu_parent_send_server_message(global.owner, response_byte_buf);
    }
}

#[no_mangle]
pub extern "C" fn wgpu_server_device_create_encoder(
    global: &Global,
    device_id: id::DeviceId,
    desc: &wgt::CommandEncoderDescriptor<Option<&nsACString>>,
    new_id: id::CommandEncoderId,
    mut error_buf: ErrorBuffer,
) {
    let utf8_label = desc.label.map(|utf16| utf16.to_string());
    let label = utf8_label.as_ref().map(|s| Cow::from(&s[..]));

    let desc = desc.map_label(|_| label);
    let (_, error) = global.device_create_command_encoder(device_id, &desc, Some(new_id));
    if let Some(err) = error {
        error_buf.init(err, device_id);
    }
}

#[no_mangle]
pub extern "C" fn wgpu_server_encoder_finish(
    global: &Global,
    device_id: id::DeviceId,
    command_encoder_id: id::CommandEncoderId,
    command_buffer_id: id::CommandBufferId,
    desc: &wgt::CommandBufferDescriptor<Option<&nsACString>>,
    mut error_buf: ErrorBuffer,
) {
    let label = wgpu_string(desc.label);
    let desc = desc.map_label(|_| label);
    let (_, label_and_error) =
        global.command_encoder_finish(command_encoder_id, &desc, Some(command_buffer_id));
    if let Some((_label, err)) = label_and_error {
        error_buf.init(err, device_id);
    }
}

#[no_mangle]
pub unsafe extern "C" fn wgpu_server_encoder_copy_texture_to_buffer(
    global: &Global,
    device_id: id::DeviceId,
    self_id: id::CommandEncoderId,
    source: &wgc::command::TexelCopyTextureInfo,
    dst_buffer: wgc::id::BufferId,
    dst_layout: &crate::TexelCopyBufferLayout,
    size: &wgt::Extent3d,
    mut error_buf: ErrorBuffer,
) {
    let destination = wgc::command::TexelCopyBufferInfo {
        buffer: dst_buffer,
        layout: dst_layout.into_wgt(),
    };
    if let Err(err) =
        global.command_encoder_copy_texture_to_buffer(self_id, source, &destination, size)
    {
        error_buf.init(err, device_id);
    }
}

#[no_mangle]
pub unsafe extern "C" fn wgpu_server_queue_write_texture(
    global: &Global,
    device_id: id::DeviceId,
    queue_id: id::QueueId,
    destination: &wgt::TexelCopyTextureInfo<id::TextureId>,
    data: FfiSlice<u8>,
    data_layout: &crate::TexelCopyBufferLayout,
    size: &wgt::Extent3d,
    mut error_buf: ErrorBuffer,
) {
    let data = data.as_slice();
    let data_layout = data_layout.into_wgt();
    if let Err(err) = global.queue_write_texture(queue_id, destination, data, &data_layout, size) {
        error_buf.init(err, device_id);
    }
}

#[no_mangle]
pub unsafe extern "C" fn wgpu_server_queue_submit(
    global: &Global,
    device_id: id::DeviceId,
    self_id: id::QueueId,
    command_buffers: FfiSlice<'_, id::CommandBufferId>,
    mut error_buf: ErrorBuffer,
) -> u64 {
    let result = global.queue_submit(self_id, command_buffers.as_slice());

    match result {
        Err((_index, err)) => {
            error_buf.init(err, device_id);
            return 0;
        }
        Ok(wrapped_index) => {
            global
                .device_poll_workers
                .wait_for(device_id, wrapped_index);
            wrapped_index
        }
    }
}

#[repr(C)]
pub struct SubmittedWorkDoneClosure {
    pub callback: unsafe extern "C" fn(user_data: *mut u8),
    pub user_data: *mut u8,
}

#[derive(Debug)]

pub struct VkSemaphoreHandle {
    pub semaphore: vk::Semaphore,
    queue_id: id::QueueId,
}

#[no_mangle]

pub extern "C" fn wgpu_vksemaphore_create_signal_semaphore(
    global: &Global,
    queue_id: id::QueueId,
) -> *mut VkSemaphoreHandle {
    let semaphore_handle = unsafe {
        let Some(hal_queue) = global.queue_as_hal::<wgc::api::Vulkan>(queue_id) else {
            emit_critical_invalid_note("Vulkan queue");
            return ptr::null_mut();
        };
        let device = hal_queue.raw_device();

        let mut export_semaphore_create_info = vk::ExportSemaphoreCreateInfo::default()
            .handle_types(vk::ExternalSemaphoreHandleTypeFlags::OPAQUE_FD);
        let create_info =
            vk::SemaphoreCreateInfo::default().push_next(&mut export_semaphore_create_info);
        let semaphore = match device.create_semaphore(&create_info, None) {
            Err(err) => {
                let msg = CString::new(format!("create_semaphore() failed: {:?}", err)).unwrap();
                gfx_critical_note(msg.as_ptr());
                return ptr::null_mut();
            }
            Ok(semaphore) => semaphore,
        };

        hal_queue.add_signal_semaphore(semaphore, None);

        VkSemaphoreHandle {
            semaphore,
            queue_id,
        }
    };

    Box::into_raw(Box::new(semaphore_handle))
}

#[no_mangle]

pub unsafe extern "C" fn wgpu_vksemaphore_get_file_descriptor(
    global: &Global,
    device_id: id::DeviceId,
    handle: &VkSemaphoreHandle,
) -> i32 {
    let file_descriptor = unsafe {
        match global.device_as_hal::<wgc::api::Vulkan>(device_id) {
            None => {
                emit_critical_invalid_note("Vulkan device");
                None
            }
            Some(hal_device) => {
                let device = hal_device.raw_device();
                let instance = hal_device.shared_instance().raw_instance();

                let external_semaphore_fd =
                    khr::external_semaphore_fd::Device::new(instance, device);
                let get_fd_info = vk::SemaphoreGetFdInfoKHR::default()
                    .semaphore(handle.semaphore)
                    .handle_type(vk::ExternalSemaphoreHandleTypeFlags::OPAQUE_FD);

                external_semaphore_fd.get_semaphore_fd(&get_fd_info).ok()
            }
        }
    };

    file_descriptor.unwrap_or(-1)
}

#[no_mangle]

pub unsafe extern "C" fn wgpu_vksemaphore_destroy(
    global: &Global,
    device_id: id::DeviceId,
    handle: &VkSemaphoreHandle,
) {
    unsafe {
        if let Some(hal_queue) = global.queue_as_hal::<wgc::api::Vulkan>(handle.queue_id) {
            if !hal_queue.remove_signal_semaphore(handle.semaphore) {
                let queue_gate = hal_queue.queue_operation_gate();
                if submit_mmltk_queue_and_wait(
                    hal_queue.raw_device(),
                    hal_queue.as_raw(),
                    &queue_gate,
                    &[],
                )
                .is_err()
                {
                    std::process::abort();
                }
            }
        }

        let Some(hal_device) = global.device_as_hal::<wgc::api::Vulkan>(device_id) else {
            emit_critical_invalid_note("Vulkan device");
            return;
        };
        let device = hal_device.raw_device();
        device.destroy_semaphore(handle.semaphore, None);
    };
}

#[no_mangle]

pub unsafe extern "C" fn wgpu_vksemaphore_delete(handle: *mut VkSemaphoreHandle) {
    let _ = Box::from_raw(handle);
}

#[no_mangle]
pub extern "C" fn wgpu_server_buffer_drop(global: &Global, self_id: id::BufferId) {
    global.buffer_drop(self_id);
}

#[no_mangle]
pub extern "C" fn wgpu_server_command_encoder_drop(global: &Global, self_id: id::CommandEncoderId) {
    global.command_encoder_drop(self_id);
}

#[no_mangle]
pub extern "C" fn wgpu_server_command_buffer_drop(global: &Global, self_id: id::CommandBufferId) {
    global.command_buffer_drop(self_id);
}
