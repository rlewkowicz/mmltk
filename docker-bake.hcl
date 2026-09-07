# Standalone invocation uses supported environment input and jsondecode:
# MMLTK_NVIDIA_MANIFEST="$(cat docker/nvidia-payload.json)" docker buildx bake toolchain
variable "MMLTK_NVIDIA_MANIFEST" {
  default = "{}"
}
variable "MMLTK_NVIDIA_DONOR_IMAGE" {
  default = jsondecode(MMLTK_NVIDIA_MANIFEST).donor
}
variable "MMLTK_NVIDIA_PAYLOAD_IMAGE" {
  default = "mmltk-nvidia-payload:latest"
}
variable "MMLTK_TOOLCHAIN_BASE_IMAGE" {
  default = "mmltk-toolchain-base:latest"
}
variable "MMLTK_GCC_IMAGE" {
  default = "mmltk-gcc16:latest"
}
variable "MMLTK_BUILD_IMAGE" {
  default = "mmltk-build:latest"
}

function "nvidia" {
  params = []
  result = jsondecode(MMLTK_NVIDIA_MANIFEST)
}

target "nvidia-configuration" {
  args = {
    MMLTK_NVIDIA_DONOR_IMAGE = MMLTK_NVIDIA_DONOR_IMAGE
    MMLTK_CUDA_ROOT = nvidia().cuda_root
    MMLTK_PYTHON_ROOT = nvidia().python_root
    MMLTK_CUDA_VERSION = nvidia().versions.cuda
    MMLTK_TORCH_VERSION = nvidia().versions.torch
    MMLTK_TENSORRT_VERSION = nvidia().versions.tensorrt
    MMLTK_NVIDIA_LIBRARY_PATH = join(":", [for key in nvidia().library_order : "${nvidia()[key]}/${key == "cuda_root" ? "lib64" : "lib"}"])
  }
}

target "nvidia-payload" {
  context = "."
  dockerfile = "docker/Dockerfile.nvidia"
  tags = [MMLTK_NVIDIA_PAYLOAD_IMAGE]
  platforms = [nvidia().platform]
  args = {
    MMLTK_NVIDIA_DONOR_IMAGE = MMLTK_NVIDIA_DONOR_IMAGE
  }
}

target "gcc16" {
  context = "."
  dockerfile = "docker/Dockerfile.gcc"
  tags = [MMLTK_GCC_IMAGE]
  args = {
    MMLTK_GCC_BUILD_IMAGE = "ubuntu:24.04"
  }
}

target "ubuntu" {
  context = "."
  dockerfile = "docker/Dockerfile.ubuntu"
  contexts = {
    mmltk_ubuntu = "docker-image://ubuntu:24.04"
  }
}

target "onnxruntime" {
  context = "."
  dockerfile = "docker/Dockerfile.onnxruntime"
  contexts = {
    mmltk_ubuntu = "target:ubuntu"
  }
}

target "toolchain-base" {
  context = "."
  dockerfile = "docker/Dockerfile.core"
  target = "toolchain"
  tags = [MMLTK_TOOLCHAIN_BASE_IMAGE]
  contexts = {
    mmltk_gcc16 = "target:gcc16"
    mmltk_ubuntu = "target:ubuntu"
  }
}

target "dependencies" {
  context = "."
  dockerfile = "docker/Dockerfile.dependencies"
  contexts = {
    mmltk_toolchain_base = "target:toolchain-base"
  }
}

target "toolchain" {
  inherits = ["nvidia-configuration"]
  context = "."
  dockerfile = "docker/Dockerfile.toolchain"
  tags = [MMLTK_BUILD_IMAGE]
  # Standalone Bake builds producers; mmltk uses direct Buildx image contexts.
  contexts = {
    mmltk_dependencies = "target:dependencies"
    mmltk_nvidia_payload = "target:nvidia-payload"
    mmltk_onnxruntime = "target:onnxruntime"
  }
}

target "runtime-base" {
  inherits = ["nvidia-configuration"]
  context = "."
  dockerfile = "docker/Dockerfile.runtime-base"
  contexts = {
    mmltk_ubuntu = "target:ubuntu"
    mmltk_nvidia_payload = "target:nvidia-payload"
    mmltk_onnxruntime = "target:onnxruntime"
  }
}
