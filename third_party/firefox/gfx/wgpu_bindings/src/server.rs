/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

use crate::{
    error::{error_to_string, ErrMsg, ErrorBuffer, ErrorBufferType, OwnedErrorBuffer},
    make_byte_buf,
    wgpu_string, AdapterInformation, BufferMapResult, ByteBuf, CommandEncoderAction, DeviceAction,
    FfiSlice, Message, PipelineError, QueueWriteAction, QueueWriteDataSource, ServerMessage,
    ShaderModuleCompilationMessage, SwapChainId, TextureAction,
};

use nsstring::{nsACString, nsCString};

use wgc::id;
use wgc::{pipeline::CreateShaderModuleError, resource::BufferAccessError};
#[allow(unused_imports)]
use wgh::Instance;
use wgt::error::{ErrorType, WebGpuError};

use std::borrow::Cow;
use std::collections::HashMap;

use std::fs::{File, OpenOptions};

use std::io::Write;
#[allow(unused_imports)]
use std::mem;

use std::os::fd::{FromRawFd, IntoRawFd, OwnedFd, RawFd};

use std::os::unix::fs::OpenOptionsExt;
use std::os::raw::c_char;
use std::ptr;
use std::sync::{Arc, Mutex, OnceLock, Weak};
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


fn mmltk_workspace_trace_file() -> Option<&'static Mutex<File>> {
    static TRACE_FILE: OnceLock<Option<Mutex<File>>> = OnceLock::new();
    TRACE_FILE
        .get_or_init(|| {
            let path = std::env::var_os("MMLTK_GUI_TRACE_FILE")?;
            let file = OpenOptions::new()
                .create(true)
                .append(true)
                .mode(0o600)
                .custom_flags(libc::O_CLOEXEC)
                .open(path)
                .ok()?;
            Some(Mutex::new(file))
        })
        .as_ref()
}


fn write_mmltk_workspace_trace(
    file: &Mutex<File>,
    event: &'static str,
    generation: u64,
    slot: u32,
    revision: u64,
    value: u64,
    submit_ns: Option<u64>,
) {
    let line = match submit_ns {
        Some(submit_ns) => format!(
            "{{\"source\":\"firefox\",\"level\":\"trace\",\"event\":\"{event}\",\"fields\":{{\"generation\":{generation},\"slot\":{slot},\"revision\":{revision},\"value\":{value},\"submit_ns\":{submit_ns}}}}}\n"
        ),
        None => format!(
            "{{\"source\":\"firefox\",\"level\":\"trace\",\"event\":\"{event}\",\"fields\":{{\"generation\":{generation},\"slot\":{slot},\"revision\":{revision},\"value\":{value}}}}}\n"
        ),
    };
    if let Ok(mut file) = file.lock() {
        let _ = file.write_all(line.as_bytes());
    }
}


fn trace_mmltk_workspace(
    event: &'static str,
    generation: u64,
    slot: u32,
    revision: u64,
    value: u64,
) {
    let Some(file) = mmltk_workspace_trace_file() else {
        return;
    };
    write_mmltk_workspace_trace(file, event, generation, slot, revision, value, None);
}


fn trace_mmltk_workspace_submission(
    generation: u64,
    slot: u32,
    revision: u64,
) {
    let Some(file) = mmltk_workspace_trace_file() else {
        return;
    };
    let mut timestamp = libc::timespec {
        tv_sec: 0,
        tv_nsec: 0,
    };
    let submit_ns = if unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut timestamp) } == 0 {
        Some((timestamp.tv_sec as u64) * 1_000_000_000u64 + timestamp.tv_nsec as u64)
    } else {
        None
    };
    write_mmltk_workspace_trace(
        file,
        "workspace.timeline_wait_injected",
        generation,
        slot,
        revision,
        revision,
        submit_ns,
    );
}
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

pub struct Global {
    owner: WebGPUParentPtr,
    global: wgc::global::Global,
    swap_chain_configs: Mutex<HashMap<SwapChainId, SwapChainConfig>>,

    mmltk_device_queues: Mutex<HashMap<id::DeviceId, id::QueueId>>,

    mmltk_workspace_textures: Mutex<HashMap<id::TextureId, MmltkWorkspaceTexture>>,
}


struct MmltkWorkspaceTexture {
    generation: u64,
    slot: u32,
    device_id: id::DeviceId,
    semaphore: vk::Semaphore,
    last_waited_revision: u64,
    texture: Weak<wgc::resource::Texture>,
    dropped: bool,
}


#[derive(Clone, Copy, Default)]
#[repr(C)]
struct MmltkWorkspaceSlot {
    width: u32,
    height: u32,
    stride: u64,
    offset: u64,
    allocation_size: u64,
    drm_modifier: u64,
    drm_format: u32,
    slot_count: u32,
}


struct MmltkWorkspaceSlotClaim {
    owner: WebGPUParentPtr,
    generation: u64,
    slot: u32,
    armed: bool,
}


impl MmltkWorkspaceSlotClaim {
    fn new(owner: WebGPUParentPtr, generation: u64, slot: u32) -> Self {
        Self {
            owner,
            generation,
            slot,
            armed: true,
        }
    }

    fn commit(&mut self) {
        self.armed = false;
    }
}


impl Drop for MmltkWorkspaceSlotClaim {
    fn drop(&mut self) {
        if self.armed {
            unsafe {
                wgpu_server_mmltk_workspace_release_slot(
                    self.owner,
                    self.generation,
                    self.slot,
                )
            };
        }
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

#[no_mangle]
pub extern "C" fn wgpu_server_new(owner: WebGPUParentPtr) -> *mut Global {
    log::info!("Initializing WGPU server");
    let backends = wgt::Backends::VULKAN;

    let mut instance_flags = (wgt::InstanceFlags::from_build_config()
        | wgt::InstanceFlags::AUTOMATIC_TIMESTAMP_NORMALIZATION
        | wgt::InstanceFlags::STRICT_WEBGPU_COMPLIANCE)
        .with_env();
    if !static_prefs::pref!("dom.webgpu.hal-labels") {
        instance_flags.insert(wgt::InstanceFlags::DISCARD_HAL_LABELS);
    }

    let global = wgc::global::Global::new(
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
    );
    let global = Global {
        owner,
        global,
        swap_chain_configs: Mutex::new(HashMap::new()),

        mmltk_device_queues: Mutex::new(HashMap::new()),

        mmltk_workspace_textures: Mutex::new(HashMap::new()),
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

    {
        let device_ids = global
            .mmltk_device_queues
            .lock()
            .unwrap()
            .keys()
            .copied()
            .collect::<Vec<_>>();
        for device_id in device_ids {
            global.drop_mmltk_workspace_device(device_id);
        }
    }
}

#[no_mangle]
pub extern "C" fn wgpu_server_poll_all_devices(global: &Global, force_wait: bool) {
    global.poll_all_devices(force_wait).unwrap();

    global.release_dropped_mmltk_workspace_textures(None);
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
            timeout: Some(Duration::from_secs(60)),
        }
    } else {
        wgt::PollType::Poll
    };
    global.device_poll(device_id, maintain).unwrap();

    global.release_dropped_mmltk_workspace_textures(Some(device_id));
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


    {
        let hal_adapter = global.adapter_as_hal::<wgc::api::Vulkan>(self_id);
        let supports_workspace_interop = hal_adapter.as_ref().is_some_and(|hal_adapter| {
            let capabilities = hal_adapter.physical_device_capabilities();
            capabilities.supports_extension(khr::external_memory_fd::NAME)
                && capabilities.supports_extension(ash::ext::external_memory_dma_buf::NAME)
                && capabilities.supports_extension(ash::ext::image_drm_format_modifier::NAME)
                && capabilities.supports_extension(ash::ext::physical_device_drm::NAME)
                && capabilities.supports_extension(khr::external_semaphore_fd::NAME)
        });
        if supports_workspace_interop {
            sanitized_desc
                .required_features
                .insert(wgt::Features::VULKAN_EXTERNAL_MEMORY_DMA_BUF);
        }
    }

    let res = global.adapter_request_device(
        self_id,
        &sanitized_desc,
        Some(new_device_id),
        Some(new_queue_id),
    );
    if let Err(err) = res {
        return Some(format!("{err}"));
    } else {

        {
            global
                .mmltk_device_queues
                .lock()
                .unwrap()
                .insert(new_device_id, new_queue_id);
            if !global.configure_mmltk_workspace_adapter(self_id) {
                trace_mmltk_workspace("workspace.adapter_rejected", 0, 0, 0, 0);
                log::warn!("Firefox WebGPU adapter is incompatible with the native workspace bridge");
            }
        }
        return None;
    }
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
            Err(futures_channel::oneshot::Canceled) => {
            }
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
    let result = global.buffer_map_async(buffer_id, start, Some(size), operation);

    if let Err(error) = result {
        error_buf.init(error, device_id);
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
    let status_passback = Arc::new(OnceLock::new());
    let op = wgc::resource::BufferMapOperation {
        host: map_mode,
        callback: Some(Box::new({
            let status_passback = Arc::clone(&status_passback);
            move |status| {
                status_passback.set(status).unwrap();
            }
        })),
    };

    let submission_index;
    match global.buffer_map_async(buffer_id, offset, Some(size), op) {
        Ok(i) => {
            submission_index = i;
        }
        Err(err) => {
            return BufferMapAsyncStatus::from(Err(err));
        }
    }

    let poll_type = wgt::PollType::Wait {
        submission_index: Some(submission_index),
        timeout: Some(Duration::from_secs(60)),
    };
    if let Err(err) = global.device_poll(device_id, poll_type) {
        return BufferMapAsyncStatus::from(Err(err));
    }


    let status_oncelock = Arc::into_inner(status_passback).unwrap();

    let status_result = status_oncelock.into_inner().unwrap();

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
    global.texture_destroy(id);
}

#[no_mangle]
pub extern "C" fn wgpu_server_texture_drop(global: &Global, id: id::TextureId) {
    global.texture_drop(id);

    global.drop_mmltk_workspace_texture(id);
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

    global.release_dropped_mmltk_workspace_textures(None);
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

        let mut usage_flags = vk::ImageUsageFlags::empty();
        usage_flags |= vk::ImageUsageFlags::COLOR_ATTACHMENT;

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

        let result = device.bind_image_memory(image, memory,  0);
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

    fn wgpu_server_mmltk_workspace_configure_adapter(
        parent: WebGPUParentPtr,
        render_major: u32,
        render_minor: u32,
        device_uuid: *const u8,
        modifiers: *const u64,
        modifier_count: usize,
        timeline_semaphore: bool,
    ) -> bool;

    fn wgpu_server_mmltk_workspace_take_slot(
        parent: WebGPUParentPtr,
        generation: u64,
        slot: u32,
        descriptor: *mut MmltkWorkspaceSlot,
        dma_buf_fd: *mut i32,
    ) -> bool;

    fn wgpu_server_mmltk_workspace_send_slot_ready(
        parent: WebGPUParentPtr,
        generation: u64,
        slot: u32,
        semaphore_fd: i32,
    ) -> bool;

    fn wgpu_server_mmltk_workspace_release_slot(
        parent: WebGPUParentPtr,
        generation: u64,
        slot: u32,
    );

    fn wgpu_server_mmltk_workspace_present_revision(
        parent: WebGPUParentPtr,
        generation: u64,
        slot: u32,
    ) -> u64;
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

impl Global {

    fn configure_mmltk_workspace_adapter(&self, adapter_id: id::AdapterId) -> bool {
        let Some(adapter) = (unsafe { self.adapter_as_hal::<wgc::api::Vulkan>(adapter_id) }) else {
            return false;
        };
        let capabilities = adapter.physical_device_capabilities();
        if ![
            khr::external_memory_fd::NAME,
            ash::ext::external_memory_dma_buf::NAME,
            ash::ext::image_drm_format_modifier::NAME,
            ash::ext::physical_device_drm::NAME,
            khr::external_semaphore_fd::NAME,
        ]
        .iter()
        .all(|extension| capabilities.supports_extension(extension))
        {
            return false;
        }

        let instance = adapter.shared_instance().raw_instance();
        let physical_device = adapter.raw_physical_device();
        let mut drm_properties = vk::PhysicalDeviceDrmPropertiesEXT::default();
        let mut id_properties = vk::PhysicalDeviceIDProperties::default();
        let mut properties = vk::PhysicalDeviceProperties2::default()
            .push_next(&mut drm_properties)
            .push_next(&mut id_properties);
        unsafe { instance.get_physical_device_properties2(physical_device, &mut properties) };
        if drm_properties.has_render == 0
            || drm_properties.render_major <= 0
            || drm_properties.render_minor < 0
        {
            return false;
        }

        let mut timeline_features = vk::PhysicalDeviceTimelineSemaphoreFeatures::default();
        let mut features = vk::PhysicalDeviceFeatures2::default().push_next(&mut timeline_features);
        unsafe { instance.get_physical_device_features2(physical_device, &mut features) };
        let mut timeline_type = vk::SemaphoreTypeCreateInfo::default()
            .semaphore_type(vk::SemaphoreType::TIMELINE)
            .initial_value(0);
        let semaphore_info = vk::PhysicalDeviceExternalSemaphoreInfo::default()
            .handle_type(vk::ExternalSemaphoreHandleTypeFlags::OPAQUE_FD)
            .push_next(&mut timeline_type);
        let mut semaphore_properties = vk::ExternalSemaphoreProperties::default();
        unsafe {
            instance.get_physical_device_external_semaphore_properties(
                physical_device,
                &semaphore_info,
                &mut semaphore_properties,
            )
        };
        let timeline_supported = timeline_features.timeline_semaphore != 0
            && semaphore_properties
                .external_semaphore_features
                .contains(vk::ExternalSemaphoreFeatureFlags::EXPORTABLE);
        if !timeline_supported {
            return false;
        }

        let mut modifier_list = vk::DrmFormatModifierPropertiesListEXT::default();
        let mut format_properties =
            vk::FormatProperties2::default().push_next(&mut modifier_list);
        unsafe {
            instance.get_physical_device_format_properties2(
                physical_device,
                vk::Format::R8G8B8A8_UNORM,
                &mut format_properties,
            )
        };
        let modifier_count = modifier_list.drm_format_modifier_count.min(256) as usize;
        if modifier_count == 0 {
            return false;
        }
        let mut modifier_properties =
            vec![vk::DrmFormatModifierPropertiesEXT::default(); modifier_count];
        let mut modifier_list = vk::DrmFormatModifierPropertiesListEXT::default()
            .drm_format_modifier_properties(&mut modifier_properties);
        let mut format_properties =
            vk::FormatProperties2::default().push_next(&mut modifier_list);
        unsafe {
            instance.get_physical_device_format_properties2(
                physical_device,
                vk::Format::R8G8B8A8_UNORM,
                &mut format_properties,
            )
        };
        let mut modifiers = modifier_properties
            .into_iter()
            .filter(|property| {
                property.drm_format_modifier_plane_count == 1
                    && property
                        .drm_format_modifier_tiling_features
                        .contains(vk::FormatFeatureFlags::SAMPLED_IMAGE)
                    && unsafe {
                        is_dmabuf_supported(
                            instance,
                            physical_device,
                            vk::Format::R8G8B8A8_UNORM,
                            property.drm_format_modifier,
                            vk::ImageUsageFlags::SAMPLED,
                        )
                    }
            })
            .map(|property| property.drm_format_modifier)
            .collect::<Vec<_>>();
        modifiers.sort_unstable();
        modifiers.dedup();
        if modifiers.is_empty() {
            return false;
        }
        let configured = unsafe {
            wgpu_server_mmltk_workspace_configure_adapter(
                self.owner,
                drm_properties.render_major as u32,
                drm_properties.render_minor as u32,
                id_properties.device_uuid.as_ptr(),
                modifiers.as_ptr(),
                modifiers.len(),
                true,
            )
        };
        if configured {
            trace_mmltk_workspace(
                "workspace.adapter_configured",
                0,
                drm_properties.render_major as u32,
                drm_properties.render_minor as u64,
                modifiers.len() as u64,
            );
        }
        configured
    }


    fn parse_mmltk_workspace_label(
        &self,
        desc: &wgc::resource::TextureDescriptor,
    ) -> Result<Option<(u64, u32)>, String> {
        let Some(label) = desc.label.as_deref() else {
            return Ok(None);
        };
        if !label.starts_with("mmltk-workspace-v1/") {
            return Ok(None);
        }
        let mut parts = label.split('/');
        if parts.next() != Some("mmltk-workspace-v1") {
            return Err("reserved workspace texture label is malformed".into());
        }
        let token = std::env::var("MMLTK_WORKSPACE_BROKER_TOKEN")
            .map_err(|_| "workspace launch authentication is unavailable".to_string())?;
        let origin = std::env::var("MMLTK_WORKSPACE_BROKER_ORIGIN")
            .map_err(|_| "workspace launch origin is unavailable".to_string())?;
        let Some(loopback) = origin.strip_prefix("http://127.0.0.1:") else {
            return Err("workspace launch origin is not loopback".into());
        };
        let port = loopback.strip_suffix('/').unwrap_or(loopback);
        if port.parse::<u16>().is_err()
            || token.len() != 64
            || !token.bytes().all(|byte| byte.is_ascii_hexdigit())
        {
            return Err("workspace launch authentication is invalid".into());
        }
        if parts.next() != Some(token.as_str()) || parts.clone().count() != 2 {
            return Err("reserved workspace texture label is unauthorized".into());
        }
        let generation = parts
            .next()
            .ok_or_else(|| "workspace texture generation is missing".to_string())?
            .parse()
            .map_err(|_| "workspace texture generation is invalid".to_string())?;
        let slot = parts
            .next()
            .ok_or_else(|| "workspace texture slot is missing".to_string())?
            .parse()
            .map_err(|_| "workspace texture slot is invalid".to_string())?;
        Ok(Some((generation, slot)))
    }


    unsafe fn transition_mmltk_workspace_texture(
        &self,
        device_id: id::DeviceId,
        queue_id: id::QueueId,
        image: vk::Image,
        generation: u64,
        slot: u32,
    ) -> Result<(), String> {
        let hal_device = self
            .device_as_hal::<wgc::api::Vulkan>(device_id)
            .ok_or_else(|| "workspace texture device is not Vulkan".to_string())?;
        let hal_queue = self
            .queue_as_hal::<wgc::api::Vulkan>(queue_id)
            .ok_or_else(|| "workspace texture queue is not Vulkan".to_string())?;
        let device = hal_device.raw_device();
        let pool_info = vk::CommandPoolCreateInfo::default()
            .flags(vk::CommandPoolCreateFlags::TRANSIENT)
            .queue_family_index(hal_queue.family_index());
        let pool = device
            .create_command_pool(&pool_info, None)
            .map_err(|error| format!("workspace command pool creation failed: {error:?}"))?;
        let allocate_info = vk::CommandBufferAllocateInfo::default()
            .command_pool(pool)
            .level(vk::CommandBufferLevel::PRIMARY)
            .command_buffer_count(1);
        let command = match device.allocate_command_buffers(&allocate_info) {
            Ok(commands) => commands[0],
            Err(error) => {
                device.destroy_command_pool(pool, None);
                return Err(format!(
                    "workspace command buffer allocation failed: {error:?}"
                ));
            }
        };
        let result = (|| {
            device
                .begin_command_buffer(command, &vk::CommandBufferBeginInfo::default())
                .map_err(|error| format!("workspace command begin failed: {error:?}"))?;
            let barrier = vk::ImageMemoryBarrier::default()
                .old_layout(vk::ImageLayout::UNDEFINED)
                .new_layout(vk::ImageLayout::SHADER_READ_ONLY_OPTIMAL)
                .src_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
                .dst_queue_family_index(vk::QUEUE_FAMILY_IGNORED)
                .image(image)
                .subresource_range(
                    vk::ImageSubresourceRange::default()
                        .aspect_mask(vk::ImageAspectFlags::COLOR)
                        .level_count(1)
                        .layer_count(1),
                )
                .dst_access_mask(vk::AccessFlags::SHADER_READ);
            device.cmd_pipeline_barrier(
                command,
                vk::PipelineStageFlags::TOP_OF_PIPE,
                vk::PipelineStageFlags::FRAGMENT_SHADER,
                vk::DependencyFlags::empty(),
                &[],
                &[],
                &[barrier],
            );
            device
                .end_command_buffer(command)
                .map_err(|error| format!("workspace command end failed: {error:?}"))?;
            let commands = [command];
            let submit = vk::SubmitInfo::default().command_buffers(&commands);
            device
                .queue_submit(hal_queue.as_raw(), &[submit], vk::Fence::null())
                .map_err(|error| format!("workspace layout submission failed: {error:?}"))?;
            device
                .queue_wait_idle(hal_queue.as_raw())
                .map_err(|error| format!("workspace layout wait failed: {error:?}"))?;
            trace_mmltk_workspace("workspace.layout_initialized", generation, slot, 0, 0);
            Ok(())
        })();
        device.destroy_command_pool(pool, None);
        result
    }


    fn create_mmltk_workspace_texture(
        &self,
        device_id: id::DeviceId,
        texture_id: id::TextureId,
        desc: &wgc::resource::TextureDescriptor,
        generation: u64,
        slot: u32,
    ) -> Result<(), String> {
        if desc.format != wgt::TextureFormat::Rgba8Unorm
            || desc.dimension != wgt::TextureDimension::D2
            || desc.mip_level_count != 1
            || desc.sample_count != 1
            || desc.size.depth_or_array_layers != 1
            || desc.usage != wgt::TextureUsages::TEXTURE_BINDING
            || !desc.view_formats.is_empty()
        {
            return Err("workspace texture descriptor is incompatible".into());
        }

        let mut slot_descriptor = MmltkWorkspaceSlot::default();
        let mut raw_fd = -1;
        if !unsafe {
            wgpu_server_mmltk_workspace_take_slot(
                self.owner,
                generation,
                slot,
                &mut slot_descriptor,
                &mut raw_fd,
            )
        } || raw_fd < 0
        {
            return Err("workspace DMA-BUF slot is unavailable".into());
        }
        let mut slot_claim = MmltkWorkspaceSlotClaim::new(self.owner, generation, slot);
        let owned_fd = unsafe { OwnedFd::from_raw_fd(raw_fd as RawFd) };
        if slot_descriptor.width != desc.size.width
            || slot_descriptor.height != desc.size.height
            || slot_descriptor.drm_format != 0x34324241
            || slot >= slot_descriptor.slot_count
        {
            return Err("workspace DMA-BUF metadata does not match the texture".into());
        }
        let queue_id = *self
            .mmltk_device_queues
            .lock()
            .unwrap()
            .get(&device_id)
            .ok_or_else(|| "workspace WebGPU queue is unavailable".to_string())?;
        let hal_device = unsafe {
            self.device_as_hal::<wgc::api::Vulkan>(device_id)
                .ok_or_else(|| "workspace WebGPU device is not Vulkan".to_string())?
        };
        let hal_desc = wgh::TextureDescriptor {
            label: None,
            size: desc.size,
            mip_level_count: 1,
            sample_count: 1,
            dimension: wgt::TextureDimension::D2,
            format: wgt::TextureFormat::Rgba8Unorm,
            usage: wgt::TextureUses::RESOURCE,
            memory_flags: wgh::MemoryFlags::empty(),
            view_formats: vec![],
        };
        let hal_texture = unsafe {
            hal_device
                .texture_from_dmabuf_fd(
                    owned_fd,
                    &hal_desc,
                    slot_descriptor.drm_modifier,
                    slot_descriptor.stride,
                    slot_descriptor.offset,
                )
                .map_err(|error| format!("workspace DMA-BUF import failed: {error:?}"))?
        };
        trace_mmltk_workspace(
            "workspace.dmabuf_imported",
            generation,
            slot,
            0,
            slot_descriptor.drm_modifier,
        );
        unsafe {
            self.transition_mmltk_workspace_texture(
                device_id,
                queue_id,
                hal_texture.raw_handle(),
                generation,
                slot,
            )?;
        }
        let device = hal_device.raw_device();
        let mut timeline_info = vk::SemaphoreTypeCreateInfo::default()
            .semaphore_type(vk::SemaphoreType::TIMELINE)
            .initial_value(0);
        let mut export_info = vk::ExportSemaphoreCreateInfo::default()
            .handle_types(vk::ExternalSemaphoreHandleTypeFlags::OPAQUE_FD);
        let semaphore_info = vk::SemaphoreCreateInfo::default()
            .push_next(&mut timeline_info)
            .push_next(&mut export_info);
        let semaphore = unsafe {
            device
                .create_semaphore(&semaphore_info, None)
                .map_err(|error| format!("workspace timeline creation failed: {error:?}"))?
        };
        let instance = hal_device.shared_instance().raw_instance();
        let external_semaphore_fd = khr::external_semaphore_fd::Device::new(instance, device);
        let get_fd_info = vk::SemaphoreGetFdInfoKHR::default()
            .semaphore(semaphore)
            .handle_type(vk::ExternalSemaphoreHandleTypeFlags::OPAQUE_FD);
        let semaphore_fd = match unsafe { external_semaphore_fd.get_semaphore_fd(&get_fd_info) } {
            Ok(fd) => fd,
            Err(error) => {
                unsafe { device.destroy_semaphore(semaphore, None) };
                return Err(format!("workspace timeline export failed: {error:?}"));
            }
        };
        trace_mmltk_workspace("workspace.timeline_exported", generation, slot, 0, 0);
        let (_, create_error) = unsafe {
            self.create_texture_from_hal(
                Box::new(hal_texture),
                device_id,
                desc,
                wgt::TextureUses::RESOURCE,
                Some(texture_id),
            )
        };
        if let Some(error) = create_error {
            unsafe {
                libc::close(semaphore_fd);
                device.destroy_semaphore(semaphore, None);
            }
            return Err(format!("workspace wgpu texture creation failed: {error:?}"));
        }
        self.mmltk_workspace_textures.lock().unwrap().insert(
            texture_id,
            MmltkWorkspaceTexture {
                generation,
                slot,
                device_id,
                semaphore,
                last_waited_revision: 0,
                texture: self.texture_weak_ref(texture_id),
                dropped: false,
            },
        );
        if !unsafe {
            wgpu_server_mmltk_workspace_send_slot_ready(
                self.owner,
                generation,
                slot,
                semaphore_fd,
            )
        } {
            self.mmltk_workspace_textures
                .lock()
                .unwrap()
                .remove(&texture_id);
            self.texture_drop(texture_id);
            unsafe { device.destroy_semaphore(semaphore, None) };
            return Err("workspace timeline descriptor delivery failed".into());
        }
        slot_claim.commit();
        trace_mmltk_workspace("workspace.texture_ready", generation, slot, 0, 0);
        Ok(())
    }


    fn prepare_mmltk_workspace_waits(
        &self,
        device_id: id::DeviceId,
        queue_id: id::QueueId,
        _texture_ids: &[id::TextureId],
    ) {
        let Some(hal_queue) = (unsafe { self.queue_as_hal::<wgc::api::Vulkan>(queue_id) }) else {
            return;
        };
        let mut textures = self.mmltk_workspace_textures.lock().unwrap();
        for texture in textures.values_mut() {
            if texture.device_id != device_id {
                continue;
            }
            let revision = unsafe {
                wgpu_server_mmltk_workspace_present_revision(
                    self.owner,
                    texture.generation,
                    texture.slot,
                )
            };
            if revision <= texture.last_waited_revision {
                continue;
            }
            hal_queue.add_wait_semaphore(
                texture.semaphore,
                Some(revision),
                vk::PipelineStageFlags::FRAGMENT_SHADER,
            );
            texture.last_waited_revision = revision;
            trace_mmltk_workspace_submission(texture.generation, texture.slot, revision);
        }
    }


    fn drop_mmltk_workspace_texture(&self, texture_id: id::TextureId) {
        let mut textures = self.mmltk_workspace_textures.lock().unwrap();
        let Some(texture) = textures.get_mut(&texture_id) else {
            return;
        };
        texture.dropped = true;
        trace_mmltk_workspace(
            "workspace.texture_drop_requested",
            texture.generation,
            texture.slot,
            texture.last_waited_revision,
            0,
        );
        drop(textures);
        self.release_dropped_mmltk_workspace_textures(None);
    }


    fn release_dropped_mmltk_workspace_textures(&self, device_id: Option<id::DeviceId>) {
        let dropped_devices = self
            .mmltk_workspace_textures
            .lock()
            .unwrap()
            .values()
            .filter_map(|texture| {
                (texture.dropped
                    && device_id.map_or(true, |device_id| texture.device_id == device_id))
                .then_some(texture.device_id)
            })
            .collect::<std::collections::HashSet<_>>();
        let queues = self.mmltk_device_queues.lock().unwrap();
        for dropped_device in dropped_devices {
            let _ = self.device_poll(
                dropped_device,
                wgt::PollType::Wait {
                    submission_index: None,
                    timeout: Some(Duration::from_secs(60)),
                },
            );
        }
        let releasable = {
            let mut textures = self.mmltk_workspace_textures.lock().unwrap();
            let releasable_ids = textures
                .iter()
                .filter_map(|(texture_id, texture)| {
                    (texture.dropped
                        && texture.texture.strong_count() == 0
                        && device_id.map_or(true, |device_id| texture.device_id == device_id))
                    .then_some(*texture_id)
                })
                .collect::<Vec<_>>();
            releasable_ids
                .into_iter()
                .filter_map(|texture_id| {
                    textures
                        .remove(&texture_id)
                        .map(|texture| (texture_id, texture))
                })
                .collect::<Vec<_>>()
        };
        if releasable.is_empty() {
            return;
        }
        for (texture_id, texture) in releasable {
            let queue_id = queues.get(&texture.device_id).copied();
            let mut released = false;
            unsafe {
                if let Some(queue_id) = queue_id {
                    if let Some(queue) = self.queue_as_hal::<wgc::api::Vulkan>(queue_id) {
                        queue.remove_wait_semaphore(texture.semaphore);
                        let _ = queue.raw_device().queue_wait_idle(queue.as_raw());
                        queue
                            .raw_device()
                            .destroy_semaphore(texture.semaphore, None);
                        released = true;
                    }
                }
                if !released {
                    if let Some(device) =
                        self.device_as_hal::<wgc::api::Vulkan>(texture.device_id)
                    {
                        device
                            .raw_device()
                            .destroy_semaphore(texture.semaphore, None);
                        released = true;
                    }
                }
                if released {
                    wgpu_server_mmltk_workspace_release_slot(
                        self.owner,
                        texture.generation,
                        texture.slot,
                    );
                    trace_mmltk_workspace(
                        "workspace.texture_released",
                        texture.generation,
                        texture.slot,
                        texture.last_waited_revision,
                        0,
                    );
                }
            }
            if !released {
                self.mmltk_workspace_textures
                    .lock()
                    .unwrap()
                    .insert(texture_id, texture);
            }
        }
    }


    fn mark_mmltk_workspace_device_dropped(&self, device_id: id::DeviceId) {
        let texture_ids = self
            .mmltk_workspace_textures
            .lock()
            .unwrap()
            .iter()
            .filter_map(|(texture_id, texture)| {
                (texture.device_id == device_id).then_some(*texture_id)
            })
            .collect::<Vec<_>>();
        for texture_id in texture_ids {
            self.drop_mmltk_workspace_texture(texture_id);
        }
        self.release_dropped_mmltk_workspace_textures(Some(device_id));
    }


    fn drop_mmltk_workspace_device(&self, device_id: id::DeviceId) {
        trace_mmltk_workspace("workspace.device_dropped", 0, 0, 0, 0);
        self.mark_mmltk_workspace_device_dropped(device_id);
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
            let mut usage_flags = vk::ImageUsageFlags::empty();
            usage_flags |= vk::ImageUsageFlags::COLOR_ATTACHMENT;

            let mut external_image_create_info = vk::ExternalMemoryImageCreateInfo::default()
                .handle_types(vk::ExternalMemoryHandleTypeFlags::DMA_BUF_EXT);

            let memory_plane_layouts: Vec<_> = vk_image_wrapper
                .layouts
                .iter()
                .map(|layout| vk::SubresourceLayout { size: 0, ..*layout })
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

            match device.bind_image_memory(image, memory,  0) {
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
                usage: wgt::TextureUses::COPY_DST | wgt::TextureUses::COLOR_TARGET,
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


                match self.parse_mmltk_workspace_label(&desc) {
                    Ok(Some((generation, slot))) => {
                        if let Err(message) = self.create_mmltk_workspace_texture(
                            device_id,
                            id,
                            &desc,
                            generation,
                            slot,
                        ) {
                            trace_mmltk_workspace(
                                "workspace.texture_import_failed",
                                generation,
                                slot,
                                0,
                                0,
                            );
                            self.create_texture_error(device_id, Some(id), &desc);
                            error_buf.init(
                                ErrMsg {
                                    message: message.into(),
                                    r#type: ErrorType::Internal,
                                },
                                device_id,
                            );
                        }
                        return;
                    }
                    Err(message) => {
                        self.create_texture_error(device_id, Some(id), &desc);
                        error_buf.init(
                            ErrMsg {
                                message: message.into(),
                                r#type: ErrorType::Validation,
                            },
                            device_id,
                        );
                        return;
                    }
                    Ok(None) => {}
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
                        CreateShaderModuleError::Parsing(_) => "Parsing error".to_string(),
                        CreateShaderModuleError::Validation(_) => {
                            "Shader validation error".to_string()
                        }
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
    let result = global.buffer_map_async(buffer_id, offset, Some(size), operation);

    if let Err(error) = result {
        error_buf.init(error, device_id);
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

            global.prepare_mmltk_workspace_waits(device_id, queue_id, &texture_ids);
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
            )
        }
        Message::QueueOnSubmittedWorkDone {
            device_id: _,
            queue_id,
        } => {
            let closure = wgpu_parent_build_submitted_work_done_closure(global.owner, queue_id);
            let (work_done_sender, work_done_receiver) = futures_channel::oneshot::channel();
            moz_task::spawn_local("WebGPU onSubmittedWorkDone callback", async move {
                work_done_receiver.await.unwrap();
                (closure.callback)(closure.user_data)
            })
            .detach();
            global.queue_on_submitted_work_done(
                queue_id,
                Box::new(move || {
                    work_done_sender.send(()).unwrap();
                }),
            );
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
            global.texture_destroy(id)
        }
        Message::DestroyExternalTexture(id) => global.external_texture_destroy(id),
        Message::DestroyExternalTextureSource(id) => {
            wgpu_parent_destroy_external_texture_source(global.owner, id)
        }
        Message::DestroyDevice(id) => global.device_destroy(id),

        Message::DropAdapter(id) => global.adapter_drop(id),
        Message::DropDevice(id) => {
            wgpu_server_pre_device_drop(global.owner, id);

            global.drop_mmltk_workspace_device(id);
            global.device_drop(id)
        }
        Message::DropQueue(id) => global.queue_drop(id),
        Message::DropBuffer(id) => {
            wgpu_server_dealloc_buffer_shmem(global.owner, id);
            global.buffer_drop(id)
        }
        Message::DropCommandEncoder(id) => {
            global.command_encoder_drop(id);

            global.release_dropped_mmltk_workspace_textures(None);
        }
        Message::DropRenderPassEncoder(_id) => {}
        Message::DropComputePassEncoder(_id) => {}
        Message::DropRenderBundleEncoder(_id) => {}
        Message::DropCommandBuffer(id) => {
            global.command_buffer_drop(id);

            global.release_dropped_mmltk_workspace_textures(None);
        }
        Message::DropRenderBundle(id) => {
            global.render_bundle_drop(id);

            global.release_dropped_mmltk_workspace_textures(None);
        }
        Message::DropBindGroupLayout(id) => global.bind_group_layout_drop(id),
        Message::DropPipelineLayout(id) => global.pipeline_layout_drop(id),
        Message::DropBindGroup(id) => {
            global.bind_group_drop(id);

            global.release_dropped_mmltk_workspace_textures(None);
        }
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
        Ok(wrapped_index) => wrapped_index,
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
                let _ = hal_queue.raw_device().queue_wait_idle(hal_queue.as_raw());
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
