#include "src/controller/services/file_dialog_catalog.h"
#include <inplace_vector>
#include <ranges>
#include <span>
#include "src/controller/browser/application_schema.h"
#include "src/controller/contracts/model_selection.h"
#include "src/frameworks/reflection/reflection_metadata.h"
#include "src/frameworks/serialization/serialization.h"
namespace mmltk::controller::services {
namespace {
using FileDialogDescriptorStorage = std::inplace_vector<FileDialogDescriptor, kFileDialogCatalogCapacity>;
void append_model_artifact_dialogs(FileDialogDescriptorStorage& dialogs) {
    mmltk::controller::contracts::ModelSelectionRelation::VisitRows(
        [&]<class Relation>(const mmltk::controller::contracts::ModelSelectionCompatibility& compatibility) {
            if (!compatibility.custom_allowed) return;
            constexpr auto artifact_path =
                mmltk::frameworks::reflection::reflected_member_path<mmltk::controller::contracts::GuiSettingsState, Relation::artifact>();
            const std::uint64_t stable_id = mmltk::controller::browser::application_settings_field_stable_id(artifact_path.view());
            if (std::ranges::any_of(dialogs, [stable_id](const auto& dialog) { return dialog.stable_id == stable_id; })) return;
            if (dialogs.size() == dialogs.capacity()) throw std::logic_error("file-dialog declarations exceed fixed catalog capacity");
            FileDialogDescriptor descriptor{};
            descriptor.stable_id = stable_id;
            descriptor.field_path = decltype(descriptor.field_path)::From(artifact_path.view());
            descriptor.workflows.workflows[0] = compatibility.workflow;
            descriptor.workflows.count = 1U;
            descriptor.mode = mmltk::controller::contracts::FileDialogMode::OpenFile;
            descriptor.model_input = compatibility.input;
            descriptor.title = decltype(descriptor.title)::From(compatibility.dialog_title);
            descriptor.filter.name = decltype(descriptor.filter.name)::From(compatibility.dialog_filter);
            descriptor.filter.pattern = decltype(descriptor.filter.pattern)::From(compatibility.dialog_pattern);
            dialogs.push_back(std::move(descriptor));
        });
}
}  // namespace
FileDialogCatalog FileDialogCatalog::Build() {
    FileDialogDescriptorStorage dialogs;
    mmltk::controller::browser::VisitSettingsLeaves<mmltk::controller::contracts::GuiSettingsState>(
        [&]<class Owner, class Declaration, class Member>(const mmltk::controller::browser::ApplicationSettingsLeafFact& field) {
            if (!field.file_dialog) return;
            if (dialogs.size() == dialogs.capacity()) throw std::logic_error("file-dialog declarations exceed fixed catalog capacity");
            FileDialogDescriptor descriptor{};
            descriptor.stable_id = field.stable_id;
            descriptor.field_path = decltype(descriptor.field_path)::From(field.path);
            descriptor.workflows = field.workflows;
            descriptor.title = decltype(descriptor.title)::From(field.file_dialog->title);
            descriptor.filter.name = decltype(descriptor.filter.name)::From(field.file_dialog->filter);
            descriptor.filter.pattern = decltype(descriptor.filter.pattern)::From(field.file_dialog->pattern);
            descriptor.mode = field.file_dialog->mode;
            dialogs.push_back(std::move(descriptor));
        });
    append_model_artifact_dialogs(dialogs);
    return Create(dialogs);
}
FileDialogCatalog FileDialogCatalog::Create(const std::span<const FileDialogDescriptor> entries) {
    if (entries.empty() || entries.size() > kFileDialogCatalogCapacity) throw std::logic_error("file-dialog catalog has invalid capacity");
    FileDialogCatalog result{};
    result.size_ = static_cast<std::uint16_t>(entries.size());
    std::copy(entries.begin(), entries.end(), result.entries_.begin());
    for (std::size_t left = 0U; left < entries.size(); ++left) {
        const auto& entry = entries[left];
        if (entry.stable_id == 0U || entry.stable_id != mmltk::controller::browser::application_settings_field_stable_id(entry.field_path.view()) ||
            !entry.field_path.valid() || !entry.workflows.valid() || !entry.title.valid() || !entry.filter.name.valid() || !entry.filter.pattern.valid() ||
            !mmltk::frameworks::reflection::enum_contains(entry.mode) ||
            (entry.model_input && !mmltk::frameworks::reflection::enum_contains(*entry.model_input))) {
            throw std::logic_error("file-dialog catalog has invalid declaration metadata");
        }
        for (std::size_t right = left + 1U; right < entries.size(); ++right) {
            if (entry.stable_id == entries[right].stable_id || entry.field_path.view() == entries[right].field_path.view()) {
                throw std::logic_error("file-dialog catalog has a duplicate declaration");
            }
        }
    }
    return result;
}
std::optional<ResolvedFileDialog> FileDialogCatalog::resolve(const FileDialogOpen request) const noexcept {
    if (!valid_file_dialog_target(request.target)) return std::nullopt;
    const auto stable_id = file_dialog_stable_id(request.target);
    const auto found = std::ranges::find(entries(), stable_id, &FileDialogDescriptor::stable_id);
    if (found == entries().end()) return std::nullopt;
    if (const auto* target = std::get_if<ModelArtifactTarget>(&request.target.value)) {
        if (!found->model_input) return std::nullopt;
        const auto* compatibility = mmltk::controller::contracts::find_model_selection_compatibility(target->workflow, target->input);
        if (compatibility == nullptr || !compatibility->custom_allowed ||
            found->stable_id != mmltk::controller::browser::application_settings_field_stable_id(compatibility->artifact_field_path) ||
            !found->workflows.allows(target->workflow) || found->model_input != target->input)
            return std::nullopt;
    } else if (found->model_input) {
        return std::nullopt;
    }
    return ResolvedFileDialog{*found, request.target};
}
const FileDialogCatalog& file_dialog_catalog() {
    static const FileDialogCatalog catalog = FileDialogCatalog::Build();
    return catalog;
}
std::optional<mmltk::controller::contracts::SettingsValueUpdate> resolve_file_dialog_path(const std::uint64_t stable_field_id, const std::string_view path) {
    if (stable_field_id == 0U || path.empty() || path.size() > mmltk::frameworks::reflection::kMaximumPathBytes) return std::nullopt;
    const auto entries = file_dialog_catalog().entries();
    const auto entry =
        std::ranges::find_if(entries, [stable_field_id](const auto& value) { return value.stable_id == stable_field_id && !value.defer_apply(); });
    if (entry == entries.end()) return std::nullopt;
    auto value = mmltk::frameworks::serialization::wire::FlatValue::text(path, mmltk::frameworks::reflection::kMaximumPathBytes);
    if (!value) return std::nullopt;
    return mmltk::controller::contracts::SettingsValueUpdate{.path = std::string(entry->field_path.view()), .value = std::move(*value)};
}
}  // namespace mmltk::controller::services
