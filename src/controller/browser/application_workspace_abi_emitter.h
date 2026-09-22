#pragma once
#include <cctype>
#include <cstdint>
#include <meta>
#include <ostream>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include "src/controller/presentation/abi/workspace_surface_import_abi.h"
#include "src/controller/presentation/abi/workspace_frame_signal.h"
#include "src/controller/presentation/workspace_presentation_types.h"
#include "src/frameworks/gpu/image_workspace.h"
namespace mmltk::controller::browser {
// This projection deliberately has no application codec vocabulary. Untrusted
// enum fields use their integer wire representation, never Rust discriminants.
class ApplicationWorkspaceAbiEmitter final {
public:
 explicit ApplicationWorkspaceAbiEmitter(std::ostream& output) : output_(output) {}
 void Emit() {
  namespace abi = presentation::detail::workspace_surface_import;
  output_ << "// Generated from native graphics declarations. Do not edit.\n";
  Constant("ABI_VERSION", "u32", abi::kAbiVersion);
  Constant("MODIFIER_LINEAR", "u64", abi::kModifierLinear);
  Constant("READY_MEMORY_DESCRIPTOR", "usize", abi::kReadyMemoryDescriptor);
  Constant("ALLOCATE_FRAME_EDGE_DESCRIPTOR", "usize", abi::kAllocateFrameEdgeDescriptor);
  Constant("ALLOCATE_FRAME_SIGNAL_DESCRIPTOR", "usize", abi::kAllocateFrameSignalDescriptor);
  Constant("ALLOCATE_ACCESS_DESCRIPTOR", "usize", abi::kAllocateAccessDescriptor);
  Constant("ALLOCATE_DESCRIPTOR_COUNT", "usize", abi::kAllocateDescriptorCount);
  Constant("READY_TIMELINE_DESCRIPTOR", "usize", abi::kReadyTimelineDescriptor);
  Constant("READY_DESCRIPTOR_COUNT", "usize", abi::kReadyDescriptorCount);
  Enum<abi::Opcode>("OPCODE_");
  Enum<abi::FailureCode>("FAILED_");
  Enum<presentation::WorkspacePresentationLayer>("LAYER_");
  Record<abi::Record>();
  Record<abi::LayoutPacket>();
  Record<presentation::WorkspaceContentIdentity>();
  Record<presentation::detail::WorkspaceFrameSignal>();
  Constant("WORKSPACE_METADATA_BYTE_CAPACITY", "usize", presentation::detail::kWorkspaceMetadataByteCapacity);
  Constant("WORKSPACE_FRAME_MAPPING_BYTES", "usize", presentation::detail::kWorkspaceFrameMappingBytes);
  namespace gpu = frameworks::gpu;
  Constant("WORKSPACE_ACCESS_EMPTY", "u64", gpu::kWorkspaceAccessEmpty);
  Constant("WORKSPACE_ACCESS_WRITING", "u64", gpu::kWorkspaceAccessWriting);
  Constant("WORKSPACE_ACCESS_AVAILABLE", "u64", gpu::kWorkspaceAccessAvailable);
  Constant("WORKSPACE_ACCESS_READING", "u64", gpu::kWorkspaceAccessReading);
  Constant("WORKSPACE_ACCESS_MASK", "u64", gpu::kWorkspaceAccessMask);
  Constant("WORKSPACE_ACCESS_REVOKED", "u64", gpu::kWorkspaceAccessRevoked);
  Record<gpu::ImageWorkspaceAccessSignal>();
 }

private:
 static std::string UpperSnake(std::string_view name) {
  std::string result;
  for (const char character : name) {
   if (!result.empty() && character >= 'A' && character <= 'Z') result += '_';
   result += static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
  }
  return result;
 }
 void Reserve(std::string_view name) {
  if (!symbols_.emplace(name).second) throw std::runtime_error("duplicate graphics projection symbol");
 }
 void Constant(std::string_view name, std::string_view type, std::uint64_t value) {
  Reserve(name);
  output_ << "pub const " << name << ": " << type << " = " << value << ";\n";
 }
 template <class T>
 static std::string WireType() {
  if constexpr (std::is_enum_v<T>)
   return WireType<std::underlying_type_t<T>>();
  else if constexpr (std::is_same_v<T, std::uint64_t>)
   return "u64";
  else if constexpr (std::is_same_v<T, std::uint32_t>)
   return "u32";
  else if constexpr (std::is_same_v<T, std::uint16_t>)
   return "u16";
  else if constexpr (std::is_same_v<T, std::uint8_t>)
   return "u8";
  else if constexpr (std::is_array_v<T>)
   return "[" + WireType<std::remove_extent_t<T>>() + "; " + std::to_string(std::extent_v<T>) + "]";
  else
   static_assert(sizeof(T) == 0, "graphics ABI field requires a fixed-width data projection");
 }
 template <class T>
 void Enum(std::string_view prefix) {
  constexpr auto name = std::meta::identifier_of(^^T);
  Reserve(name);
  output_ << "#[repr(" << WireType<T>() << ")]\n#[derive(Clone, Copy, Debug, PartialEq, Eq)]\npub enum " << name << " {\n";
  template for (constexpr auto entry : std::define_static_array(std::meta::enumerators_of(^^T))) {
   output_ << std::meta::identifier_of(entry) << " = " << static_cast<std::uint64_t>([:entry:]) << ",\n";
  }
  output_ << "}\n";
  template for (constexpr auto entry : std::define_static_array(std::meta::enumerators_of(^^T))) {
   Constant(std::string(prefix) + UpperSnake(std::meta::identifier_of(entry)), WireType<T>(), static_cast<std::uint64_t>([:entry:]));
  }
 }
 template <class T>
 void Record() {
  static_assert(std::is_standard_layout_v<T> && std::is_trivially_copyable_v<T>);
  constexpr auto name = std::meta::identifier_of(^^T);
  static constexpr auto members = std::define_static_array(std::meta::nonstatic_data_members_of(^^T, std::meta::access_context::current()));
  Reserve(name);
  output_ << "#[repr(C, align(" << alignof(T) << "))]\n#[derive(Clone, Copy, Default)]\npub struct " << name << " {\n";
  template for (constexpr auto member : members) {
   using Field = [:std::meta::type_of(member):];
   output_ << "pub " << std::meta::identifier_of(member) << ": " << WireType<Field>() << ",\n";
  }
  output_ << "}\nconst _: () = assert!(std::mem::size_of::<" << name << ">() == " << sizeof(T) << ");\n"
          << "const _: () = assert!(std::mem::align_of::<" << name << ">() == " << alignof(T) << ");\n";
  template for (constexpr auto member : members) {
   output_ << "const _: () = assert!(std::mem::offset_of!(" << name << ", " << std::meta::identifier_of(member) << ") == " << std::meta::offset_of(member).bytes << ");\n";
  }
 }
 std::ostream& output_;
 std::set<std::string, std::less<>> symbols_;
};
}  // namespace mmltk::controller::browser
