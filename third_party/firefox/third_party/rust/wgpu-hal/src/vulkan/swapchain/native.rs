//! Vulkan Surface and Swapchain implementation using native Vulkan surfaces.

use alloc::{boxed::Box, sync::Arc, vec::Vec};
use core::any::Any;

use ash::{khr, vk};
use parking_lot::{Mutex, MutexGuard};

use crate::vulkan::{
    conv, map_host_device_oom_and_lost_err,
    semaphore_list::SemaphoreType,
    swapchain::{Surface, SurfaceTextureMetadata, Swapchain, SwapchainSubmissionSemaphoreGuard},
    DeviceShared, InstanceShared,
};

pub(crate) struct NativeSurface {
    raw: vk::SurfaceKHR,
    functor: khr::surface::Instance,
    instance: Arc<InstanceShared>,
}

impl NativeSurface {
    pub fn from_vk_surface_khr(instance: &crate::vulkan::Instance, raw: vk::SurfaceKHR) -> Self {
        let functor = khr::surface::Instance::new(&instance.shared.entry, &instance.shared.raw);
        Self {
            raw,
            functor,
            instance: Arc::clone(&instance.shared),
        }
    }

    pub fn as_raw(&self) -> vk::SurfaceKHR {
        self.raw
    }
}

impl Drop for NativeSurface {
    fn drop(&mut self) {
        unsafe {
            self.functor.destroy_surface(self.raw, None);
        }
    }
}

impl Surface for NativeSurface {
    fn surface_capabilities(
        &self,
        adapter: &crate::vulkan::Adapter,
    ) -> Option<crate::SurfaceCapabilities> {
        if !adapter.private_caps.can_present {
            return None;
        }
        let queue_family_index = 0; 
        {
            match unsafe {
                self.functor.get_physical_device_surface_support(
                    adapter.raw,
                    queue_family_index,
                    self.raw,
                )
            } {
                Ok(true) => (),
                Ok(false) => return None,
                Err(e) => {
                    log::error!("get_physical_device_surface_support: {e}");
                    return None;
                }
            }
        }

        let caps = {
            match unsafe {
                self.functor
                    .get_physical_device_surface_capabilities(adapter.raw, self.raw)
            } {
                Ok(caps) => caps,
                Err(e) => {
                    log::error!("get_physical_device_surface_capabilities: {e}");
                    return None;
                }
            }
        };

        let max_image_count = if caps.max_image_count == 0 {
            !0
        } else {
            caps.max_image_count
        };

        let current_extent = if caps.current_extent.width != !0 && caps.current_extent.height != !0
        {
            Some(wgt::Extent3d {
                width: caps.current_extent.width,
                height: caps.current_extent.height,
                depth_or_array_layers: 1,
            })
        } else {
            None
        };

        let raw_present_modes = {
            match unsafe {
                self.functor
                    .get_physical_device_surface_present_modes(adapter.raw, self.raw)
            } {
                Ok(present_modes) => present_modes,
                Err(e) => {
                    log::error!("get_physical_device_surface_present_modes: {e}");
                    return None;
                }
            }
        };

        let raw_surface_formats = {
            match unsafe {
                self.functor
                    .get_physical_device_surface_formats(adapter.raw, self.raw)
            } {
                Ok(formats) => formats,
                Err(e) => {
                    log::error!("get_physical_device_surface_formats: {e}");
                    return None;
                }
            }
        };

        let mut formats: Vec<wgt::SurfaceFormatCapabilities> = Vec::new();
        for (format, color_space) in raw_surface_formats
            .into_iter()
            .filter_map(conv::map_vk_surface_formats)
        {
            let color_spaces = color_space.to_color_spaces().unwrap();
            match formats.iter_mut().find(|fc| fc.format == format) {
                Some(fc) => fc.color_spaces |= color_spaces,
                None => formats.push(wgt::SurfaceFormatCapabilities {
                    format,
                    color_spaces,
                }),
            }
        }
        Some(crate::SurfaceCapabilities {
            formats,
            maximum_frame_latency: (caps.min_image_count - 1)..=(max_image_count - 1), 
            current_extent,
            usage: conv::map_vk_image_usage(caps.supported_usage_flags),
            present_modes: raw_present_modes
                .into_iter()
                .flat_map(conv::map_vk_present_mode)
                .collect(),
            composite_alpha_modes: conv::map_vk_composite_alpha(caps.supported_composite_alpha),
        })
    }

    unsafe fn create_swapchain(
        &self,
        device: &crate::vulkan::Device,
        config: &crate::SurfaceConfiguration,
        provided_old_swapchain: Option<Box<dyn Swapchain>>,
    ) -> Result<Box<dyn Swapchain>, crate::SurfaceError> {
        let functor = khr::swapchain::Device::new(&self.instance.raw, &device.shared.raw);

        let old_swapchain = provided_old_swapchain
            .as_ref()
            .map(|osc| osc.as_any().downcast_ref::<NativeSwapchain>().unwrap().raw)
            .unwrap_or(vk::SwapchainKHR::null());

        let color_space = conv::map_surface_color_space(config.color_space);

        let original_format = device.shared.private_caps.map_texture_format(config.format);
        let mut raw_flags = vk::SwapchainCreateFlagsKHR::empty();
        let mut raw_view_formats: Vec<vk::Format> = vec![];
        if !config.view_formats.is_empty() {
            raw_flags |= vk::SwapchainCreateFlagsKHR::MUTABLE_FORMAT;
            raw_view_formats = config
                .view_formats
                .iter()
                .map(|f| device.shared.private_caps.map_texture_format(*f))
                .collect();
            raw_view_formats.push(original_format);
        }

        let mut info = vk::SwapchainCreateInfoKHR::default()
            .flags(raw_flags)
            .surface(self.raw)
            .min_image_count(config.maximum_frame_latency + 1) 
            .image_format(original_format)
            .image_color_space(color_space)
            .image_extent(vk::Extent2D {
                width: config.extent.width,
                height: config.extent.height,
            })
            .image_array_layers(config.extent.depth_or_array_layers)
            .image_usage(conv::map_texture_usage(config.usage))
            .image_sharing_mode(vk::SharingMode::EXCLUSIVE)
            .pre_transform(vk::SurfaceTransformFlagsKHR::IDENTITY)
            .composite_alpha(conv::map_composite_alpha_mode(config.composite_alpha_mode))
            .present_mode(conv::map_present_mode(config.present_mode))
            .clipped(true)
            .old_swapchain(old_swapchain);

        let mut format_list_info = vk::ImageFormatListCreateInfo::default();
        if !raw_view_formats.is_empty() {
            format_list_info = format_list_info.view_formats(&raw_view_formats);
            info = info.push_next(&mut format_list_info);
        }

        let result = {
            unsafe { functor.create_swapchain(&info, None) }
        };

        let raw = match result {
            Ok(swapchain) => swapchain,
            Err(error) => {
                return Err(match error {
                    vk::Result::ERROR_SURFACE_LOST_KHR
                    | vk::Result::ERROR_INITIALIZATION_FAILED => crate::SurfaceError::Lost,
                    vk::Result::ERROR_NATIVE_WINDOW_IN_USE_KHR => {
                        crate::SurfaceError::Other("Native window is in use")
                    }
                    other => map_host_device_oom_and_lost_err(other).into(),
                });
            }
        };

        let images = unsafe { functor.get_swapchain_images(raw) }
            .map_err(crate::vulkan::map_host_device_oom_err)?;

        let fence = unsafe {
            device
                .shared
                .raw
                .create_fence(&vk::FenceCreateInfo::default(), None)
                .map_err(crate::vulkan::map_host_device_oom_err)?
        };

        let acquire_semaphores = (0..images.len())
            .map(|i| {
                SwapchainAcquireSemaphore::new(&device.shared, i)
                    .map(Mutex::new)
                    .map(Arc::new)
            })
            .collect::<Result<Vec<_>, _>>()?;

        let present_semaphores = (0..images.len())
            .map(|i| Arc::new(Mutex::new(SwapchainPresentSemaphores::new(i))))
            .collect::<Vec<_>>();

        Ok(Box::new(NativeSwapchain {
            raw,
            functor,
            device: Arc::clone(&device.shared),
            images,
            fence,
            config: config.clone(),
            acquire_semaphores,
            next_acquire_index: 0,
            present_semaphores,
            next_present_time: None,
        }))
    }

    fn as_any(&self) -> &dyn Any {
        self
    }
}

pub(crate) struct NativeSwapchain {
    raw: vk::SwapchainKHR,
    functor: khr::swapchain::Device,
    device: Arc<DeviceShared>,
    images: Vec<vk::Image>,
    /// Fence used to wait on the acquired image.
    fence: vk::Fence,
    config: crate::SurfaceConfiguration,

    /// Semaphores used between image acquisition and the first submission
    /// that uses that image. This is indexed using [`next_acquire_index`].
    ///
    /// Because we need to provide this to [`vkAcquireNextImageKHR`], we haven't
    /// received the swapchain image index for the frame yet, so we cannot use
    /// that to index it.
    ///
    /// Before we pass this to [`vkAcquireNextImageKHR`], we ensure that we wait on
    /// the submission indicated by [`previously_used_submission_index`]. This ensures
    /// the semaphore is no longer in use before we use it.
    ///
    /// [`next_acquire_index`]: NativeSwapchain::next_acquire_index
    /// [`vkAcquireNextImageKHR`]: https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#vkAcquireNextImageKHR
    /// [`previously_used_submission_index`]: SwapchainAcquireSemaphore::previously_used_submission_index
    acquire_semaphores: Vec<Arc<Mutex<SwapchainAcquireSemaphore>>>,
    /// The index of the next acquire semaphore to use.
    ///
    /// This is incremented each time we acquire a new image, and wraps around
    /// to 0 when it reaches the end of [`acquire_semaphores`].
    ///
    /// [`acquire_semaphores`]: NativeSwapchain::acquire_semaphores
    next_acquire_index: usize,

    /// Semaphore sets used between all submissions that write to an image and
    /// the presentation of that image.
    ///
    /// This is indexed by the swapchain image index returned by
    /// [`vkAcquireNextImageKHR`].
    ///
    /// We know it is safe to use these semaphores because use them
    /// _after_ the acquire semaphore. Because the acquire semaphore
    /// has been signaled, the previous presentation using that image
    /// is known-finished, so this semaphore is no longer in use.
    ///
    /// [`vkAcquireNextImageKHR`]: https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#vkAcquireNextImageKHR
    present_semaphores: Vec<Arc<Mutex<SwapchainPresentSemaphores>>>,

    /// The present timing information which will be set in the next call to [`present()`](crate::Queue::present()).
    ///
    /// # Safety
    ///
    /// This must only be set if [`wgt::Features::VULKAN_GOOGLE_DISPLAY_TIMING`] is enabled, and
    /// so the VK_GOOGLE_display_timing extension is present.
    next_present_time: Option<vk::PresentTimeGOOGLE>,
}

impl Drop for NativeSwapchain {
    fn drop(&mut self) {
        unsafe {
            self.functor.destroy_swapchain(self.raw, None);
        }
    }
}

impl Swapchain for NativeSwapchain {
    unsafe fn release_resources(&mut self, device: &crate::vulkan::Device) {
        {
            let _ = unsafe {
                device
                    .shared
                    .raw
                    .device_wait_idle()
                    .map_err(map_host_device_oom_and_lost_err)
            };
        };

        unsafe { device.shared.raw.destroy_fence(self.fence, None) }

        for semaphore in self.acquire_semaphores.drain(..) {
            let arc_removed = Arc::into_inner(semaphore).expect(
                "Trying to destroy a SwapchainAcquireSemaphore that is still in use by a SurfaceTexture",
            );
            let mutex_removed = arc_removed.into_inner();

            unsafe { mutex_removed.destroy(&device.shared.raw) };
        }

        for semaphore in self.present_semaphores.drain(..) {
            let arc_removed = Arc::into_inner(semaphore).expect(
                "Trying to destroy a SwapchainPresentSemaphores that is still in use by a SurfaceTexture",
            );
            let mutex_removed = arc_removed.into_inner();

            unsafe { mutex_removed.destroy(&device.shared.raw) };
        }
    }

    unsafe fn acquire(
        &mut self,
        timeout: Option<core::time::Duration>,
        fence: &crate::vulkan::Fence,
    ) -> Result<crate::AcquiredSurfaceTexture<crate::api::Vulkan>, crate::SurfaceError> {
        let timeout_ns = match timeout {
            Some(duration) => duration.as_nanos() as u64,
            None => u64::MAX,
        };

        let acquire_semaphore_arc = self.get_acquire_semaphore();
        let acquire_semaphore_guard = acquire_semaphore_arc
            .try_lock()
            .expect("Failed to lock a SwapchainSemaphores.");

        let completed = self.device.wait_for_fence(
            fence,
            acquire_semaphore_guard.previously_used_submission_index,
            timeout_ns,
        )?;
        if !completed {
            return Err(crate::SurfaceError::Timeout);
        }

        let (index, suboptimal) = match unsafe {
            self.functor.acquire_next_image(
                self.raw,
                timeout_ns,
                acquire_semaphore_guard.acquire,
                self.fence,
            )
        } {
            Ok(pair) => pair,
            Err(error) => {
                return match error {
                    vk::Result::TIMEOUT => Err(crate::SurfaceError::Timeout),
                    vk::Result::NOT_READY | vk::Result::ERROR_OUT_OF_DATE_KHR => {
                        Err(crate::SurfaceError::Outdated)
                    }
                    vk::Result::ERROR_SURFACE_LOST_KHR => Err(crate::SurfaceError::Lost),
                    other => Err(map_host_device_oom_and_lost_err(other).into()),
                };
            }
        };

        drop(acquire_semaphore_guard);
        self.advance_acquire_semaphore();

        let present_semaphore_arc = self.get_present_semaphores(index);

        if self.device.vendor_id == crate::auxil::db::intel::VENDOR && index > 0x100 {
            return Err(crate::SurfaceError::Outdated);
        }

        let identity = self.device.texture_identity_factory.next();

        let texture = crate::vulkan::SurfaceTexture {
            index,
            texture: crate::vulkan::Texture {
                raw: self.images[index as usize],
                drop_guard: None,
                memory: crate::vulkan::TextureMemory::External,
                format: self.config.format,
                copy_size: crate::CopyExtent {
                    width: self.config.extent.width,
                    height: self.config.extent.height,
                    depth: 1,
                },
                identity,
            },
            metadata: Box::new(NativeSurfaceTextureMetadata {
                acquire_semaphores: acquire_semaphore_arc,
                present_semaphores: present_semaphore_arc,
            }),
        };
        Ok(crate::AcquiredSurfaceTexture {
            texture,
            suboptimal,
        })
    }

    unsafe fn discard_texture(
        &mut self,
        _texture: crate::vulkan::SurfaceTexture,
    ) -> Result<(), crate::SurfaceError> {
        Ok(())
    }

    unsafe fn present(
        &mut self,
        queue: &crate::vulkan::Queue,
        texture: crate::vulkan::SurfaceTexture,
    ) -> Result<(), crate::SurfaceError> {
        let metadata = texture
            .metadata
            .as_any()
            .downcast_ref::<NativeSurfaceTextureMetadata>()
            .unwrap();
        let mut acquire_semaphore = metadata.acquire_semaphores.lock();
        let mut present_semaphores = metadata.present_semaphores.lock();

        let wait_semaphores = present_semaphores.get_present_wait_semaphores();

        acquire_semaphore.end_semaphore_usage();
        present_semaphores.end_semaphore_usage();

        drop(acquire_semaphore);

        let swapchains = [self.raw];
        let image_indices = [texture.index];
        let vk_info = vk::PresentInfoKHR::default()
            .swapchains(&swapchains)
            .image_indices(&image_indices)
            .wait_semaphores(&wait_semaphores);

        let mut display_timing;
        let present_times;
        let vk_info = if let Some(present_time) = self.next_present_time.take() {
            debug_assert!(
                self.device
                    .features
                    .contains(wgt::Features::VULKAN_GOOGLE_DISPLAY_TIMING),
                "`next_present_time` should only be set if `VULKAN_GOOGLE_DISPLAY_TIMING` is enabled"
            );
            present_times = [present_time];
            display_timing = vk::PresentTimesInfoGOOGLE::default().times(&present_times);
            vk_info.push_next(&mut display_timing)
        } else {
            vk_info
        };

        let suboptimal = {
            unsafe { self.functor.queue_present(queue.raw, &vk_info) }.map_err(|error| {
                match error {
                    vk::Result::ERROR_OUT_OF_DATE_KHR => crate::SurfaceError::Outdated,
                    vk::Result::ERROR_SURFACE_LOST_KHR => crate::SurfaceError::Lost,
                    _ => map_host_device_oom_and_lost_err(error).into(),
                }
            })?
        };
        if suboptimal {
            log::debug!("Suboptimal present of frame {}", texture.index);
        }
        Ok(())
    }

    fn as_any(&self) -> &dyn Any {
        self
    }

    fn as_any_mut(&mut self) -> &mut dyn Any {
        self
    }
}

impl NativeSwapchain {
    pub(crate) fn as_raw(&self) -> vk::SwapchainKHR {
        self.raw
    }

    pub fn set_next_present_time(&mut self, present_timing: vk::PresentTimeGOOGLE) {
        let features = wgt::Features::VULKAN_GOOGLE_DISPLAY_TIMING;
        if self.device.features.contains(features) {
            self.next_present_time = Some(present_timing);
        } else {
            panic!(
                concat!(
                    "Tried to set display timing properties ",
                    "without the corresponding feature ({:?}) enabled."
                ),
                features
            );
        }
    }

    /// Mark the current frame finished, advancing to the next acquire semaphore.
    fn advance_acquire_semaphore(&mut self) {
        let semaphore_count = self.acquire_semaphores.len();
        self.next_acquire_index = (self.next_acquire_index + 1) % semaphore_count;
    }

    /// Get the next acquire semaphore that should be used with this swapchain.
    fn get_acquire_semaphore(&self) -> Arc<Mutex<SwapchainAcquireSemaphore>> {
        self.acquire_semaphores[self.next_acquire_index].clone()
    }

    /// Get the set of present semaphores that should be used with the given image index.
    fn get_present_semaphores(&self, index: u32) -> Arc<Mutex<SwapchainPresentSemaphores>> {
        self.present_semaphores[index as usize].clone()
    }
}

/// Semaphore used to acquire a swapchain image.
#[derive(Debug)]
struct SwapchainAcquireSemaphore {
    /// A semaphore that is signaled when this image is safe for us to modify.
    ///
    /// When [`vkAcquireNextImageKHR`] returns the index of the next swapchain
    /// image that we should use, that image may actually still be in use by the
    /// presentation engine, and is not yet safe to modify. However, that
    /// function does accept a semaphore that it will signal when the image is
    /// indeed safe to begin messing with.
    ///
    /// This semaphore is:
    ///
    /// - waited for by the first queue submission to operate on this image
    ///   since it was acquired, and
    ///
    /// - signaled by [`vkAcquireNextImageKHR`] when the acquired image is ready
    ///   for us to use.
    ///
    /// [`vkAcquireNextImageKHR`]: https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#vkAcquireNextImageKHR
    acquire: vk::Semaphore,

    /// True if the next command submission operating on this image should wait
    /// for [`acquire`].
    ///
    /// We must wait for `acquire` before drawing to this swapchain image, but
    /// because `wgpu-hal` queue submissions are always strongly ordered, only
    /// the first submission that works with a swapchain image actually needs to
    /// wait. We set this flag when this image is acquired, and clear it the
    /// first time it's passed to [`Queue::submit`] as a surface texture.
    ///
    /// Additionally, semaphores can only be waited on once, so we need to ensure
    /// that we only actually pass this semaphore to the first submission that
    /// uses that image.
    ///
    /// [`acquire`]: SwapchainAcquireSemaphore::acquire
    /// [`Queue::submit`]: crate::Queue::submit
    should_wait_for_acquire: bool,

    /// The fence value of the last command submission that wrote to this image.
    ///
    /// The next time we try to acquire this image, we'll block until
    /// this submission finishes, proving that [`acquire`] is ready to
    /// pass to `vkAcquireNextImageKHR` again.
    ///
    /// [`acquire`]: SwapchainAcquireSemaphore::acquire
    previously_used_submission_index: crate::FenceValue,
}

impl SwapchainAcquireSemaphore {
    fn new(device: &DeviceShared, index: usize) -> Result<Self, crate::DeviceError> {
        Ok(Self {
            acquire: device
                .new_binary_semaphore(&format!("SwapchainImageSemaphore: Index {index} acquire"))?,
            should_wait_for_acquire: true,
            previously_used_submission_index: 0,
        })
    }

    /// Sets the fence value which the next acquire will wait for. This prevents
    /// the semaphore from being used while the previous submission is still in flight.
    fn set_used_fence_value(&mut self, value: crate::FenceValue) {
        self.previously_used_submission_index = value;
    }

    /// Return the semaphore that commands drawing to this image should wait for, if any.
    ///
    /// This only returns `Some` once per acquisition; see
    /// [`SwapchainAcquireSemaphore::should_wait_for_acquire`] for details.
    fn get_acquire_wait_semaphore(&mut self) -> Option<vk::Semaphore> {
        if self.should_wait_for_acquire {
            self.should_wait_for_acquire = false;
            Some(self.acquire)
        } else {
            None
        }
    }

    /// Indicates the cpu-side usage of this semaphore has finished for the frame,
    /// so reset internal state to be ready for the next frame.
    fn end_semaphore_usage(&mut self) {
        self.should_wait_for_acquire = true;
    }

    unsafe fn destroy(&self, device: &ash::Device) {
        unsafe {
            device.destroy_semaphore(self.acquire, None);
        }
    }
}

#[derive(Debug)]
struct SwapchainPresentSemaphores {
    /// A pool of semaphores for ordering presentation after drawing.
    ///
    /// The first [`present_index`] semaphores in this vector are:
    ///
    /// - all waited on by the call to [`vkQueuePresentKHR`] that presents this
    ///   image, and
    ///
    /// - each signaled by some [`vkQueueSubmit`] queue submission that draws to
    ///   this image, when the submission finishes execution.
    ///
    /// This vector accumulates one semaphore per submission that writes to this
    /// image. This is awkward, but hard to avoid: [`vkQueuePresentKHR`]
    /// requires a semaphore to order it with respect to drawing commands, and
    /// we can't attach new completion semaphores to a command submission after
    /// it's been submitted. This means that, at submission time, we must create
    /// the semaphore we might need if the caller's next action is to enqueue a
    /// presentation of this image.
    ///
    /// An alternative strategy would be for presentation to enqueue an empty
    /// submit, ordered relative to other submits in the usual way, and
    /// signaling a single presentation semaphore. But we suspect that submits
    /// are usually expensive enough, and semaphores usually cheap enough, that
    /// performance-sensitive users will avoid making many submits, so that the
    /// cost of accumulated semaphores will usually be less than the cost of an
    /// additional submit.
    ///
    /// Only the first [`present_index`] semaphores in the vector are actually
    /// going to be signalled by submitted commands, and need to be waited for
    /// by the next present call. Any semaphores beyond that index were created
    /// for prior presents and are simply being retained for recycling.
    ///
    /// [`present_index`]: SwapchainPresentSemaphores::present_index
    /// [`vkQueuePresentKHR`]: https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#vkQueuePresentKHR
    /// [`vkQueueSubmit`]: https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#vkQueueSubmit
    present: Vec<vk::Semaphore>,

    /// The number of semaphores in [`present`] to be signalled for this submission.
    ///
    /// [`present`]: SwapchainPresentSemaphores::present
    present_index: usize,

    /// Which image this semaphore set is used for.
    frame_index: usize,
}

impl SwapchainPresentSemaphores {
    pub fn new(frame_index: usize) -> Self {
        Self {
            present: Vec::new(),
            present_index: 0,
            frame_index,
        }
    }

    /// Return the semaphore that the next submission that writes to this image should
    /// signal when it's done.
    ///
    /// See [`SwapchainPresentSemaphores::present`] for details.
    fn get_submit_signal_semaphore(
        &mut self,
        device: &DeviceShared,
    ) -> Result<vk::Semaphore, crate::DeviceError> {
        let sem = match self.present.get(self.present_index) {
            Some(sem) => *sem,
            None => {
                let sem = device.new_binary_semaphore(&format!(
                    "SwapchainImageSemaphore: Image {} present semaphore {}",
                    self.frame_index, self.present_index
                ))?;
                self.present.push(sem);
                sem
            }
        };

        self.present_index += 1;

        Ok(sem)
    }

    /// Indicates the cpu-side usage of this semaphore has finished for the frame,
    /// so reset internal state to be ready for the next frame.
    fn end_semaphore_usage(&mut self) {
        self.present_index = 0;
    }

    /// Return the semaphores that a presentation of this image should wait on.
    ///
    /// Return a slice of semaphores that the call to [`vkQueueSubmit`] that
    /// ends this image's acquisition should wait for. See
    /// [`SwapchainPresentSemaphores::present`] for details.
    ///
    /// Reset `self` to be ready for the next acquisition cycle.
    ///
    /// [`vkQueueSubmit`]: https://registry.khronos.org/vulkan/specs/1.3-extensions/html/vkspec.html#vkQueueSubmit
    fn get_present_wait_semaphores(&mut self) -> Vec<vk::Semaphore> {
        self.present[0..self.present_index].to_vec()
    }

    unsafe fn destroy(&self, device: &ash::Device) {
        unsafe {
            for sem in &self.present {
                device.destroy_semaphore(*sem, None);
            }
        }
    }
}

#[derive(Debug)]
struct NativeSurfaceTextureMetadata {
    acquire_semaphores: Arc<Mutex<SwapchainAcquireSemaphore>>,
    present_semaphores: Arc<Mutex<SwapchainPresentSemaphores>>,
}

impl SurfaceTextureMetadata for NativeSurfaceTextureMetadata {
    fn get_semaphore_guard(&self) -> Box<dyn SwapchainSubmissionSemaphoreGuard + '_> {
        Box::new(NativeSwapchainSubmissionSemaphoreGuard {
            acquire_semaphore_guard: self
                .acquire_semaphores
                .try_lock()
                .expect("Failed to lock surface acquire semaphore"),
            present_semaphores_guard: self
                .present_semaphores
                .try_lock()
                .expect("Failed to lock surface present semaphores"),
        })
    }

    fn as_any(&self) -> &dyn Any {
        self
    }
}

struct NativeSwapchainSubmissionSemaphoreGuard<'a> {
    acquire_semaphore_guard: MutexGuard<'a, SwapchainAcquireSemaphore>,
    present_semaphores_guard: MutexGuard<'a, SwapchainPresentSemaphores>,
}

impl<'a> SwapchainSubmissionSemaphoreGuard for NativeSwapchainSubmissionSemaphoreGuard<'a> {
    fn set_used_fence_value(&mut self, value: u64) {
        self.acquire_semaphore_guard.set_used_fence_value(value);
    }

    fn get_acquire_wait_semaphore(&mut self) -> Option<SemaphoreType> {
        self.acquire_semaphore_guard
            .get_acquire_wait_semaphore()
            .map(SemaphoreType::Binary)
    }

    fn get_submit_signal_semaphore(
        &mut self,
        device: &DeviceShared,
    ) -> Result<SemaphoreType, crate::DeviceError> {
        self.present_semaphores_guard
            .get_submit_signal_semaphore(device)
            .map(SemaphoreType::Binary)
    }
}
