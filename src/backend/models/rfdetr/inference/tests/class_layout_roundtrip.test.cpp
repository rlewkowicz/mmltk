#include "src/common/io/file_digest.h"
#include "src/backend/models/rfdetr/core/class_artifact.h"
#include "src/backend/models/rfdetr/core/detail/class_artifact_files.h"
#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <chrono>
#include <stop_token>
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/backend/models/rfdetr/core/artifact_publication.h"
#include "src/backend/models/rfdetr/inference/prediction_capacity.h"
namespace r = mmltk::backend::models::rfdetr;
namespace c = mmltk::backend::data::catalog;
namespace io = mmltk::common::io;
TEST_CASE("Digest-bound class bundles survive renaming and reject stale companions", "[model][rfdetr][layout][artifact]") {
    const auto root = mmltk::testsupport::make_temp_root("class-layout-bundle");
    const auto artifact = root / "model.engine";
    {
        std::ofstream output(artifact);
        output << "engine bytes for descriptor admission";
    }
    const auto digest = *io::try_file_digests(artifact, false);
    const auto layout = r::native_training_class_layout(c::ClassCatalog({"background", "cat"}));
    const auto write = [&](const std::filesystem::path& path, const r::ModelClassDescriptor& descriptor) {
        std::ofstream output(path);
        output << r::encode_class_descriptor(descriptor);
    };
    const r::ModelClassDescriptor descriptor{1, io::sha256_hex(digest.sha256), layout};
    write(artifact.string() + ".classes.json", descriptor);
    CHECK(r::ClassArtifactAdmission(artifact, {}, std::make_shared<const io::FileDigests>(digest)).Resolve(3, {}) == layout);
    const auto admitted = r::ClassArtifactSnapshot::Read(artifact);
    const auto renamed = root / "renamed.engine";
    std::filesystem::copy_file(artifact, renamed);
    std::filesystem::copy_file(artifact.string() + ".classes.json", renamed.string() + ".classes.json");
    CHECK(r::ClassArtifactAdmission(renamed).Resolve(3, {}) == layout);
    CHECK_THROWS(r::ClassArtifactAdmission(artifact, {}, std::make_shared<const io::FileDigests>(digest)).Resolve(4, {}));
    auto other = descriptor;
    other.layout = r::native_training_class_layout(c::ClassCatalog({"cat", "background"}));
    const auto explicit_path = root / "selected.classes.json";
    write(explicit_path, other);
    CHECK_THROWS(r::ClassArtifactAdmission(artifact, explicit_path, std::make_shared<const io::FileDigests>(digest)).Resolve(3, {}));
    std::filesystem::remove(artifact.string() + ".classes.json");
    CHECK(r::ClassArtifactSnapshot::Read(artifact) != admitted);
    const r::ResolvedClassLayout raw(r::ClassArtifactAdmission(artifact, {}, std::make_shared<const io::FileDigests>(digest)).Resolve(3, {}));
    CHECK(raw.domain() == c::ClassReferenceDomain::RawOutputSlot);
    write(artifact.string() + ".classes.json", descriptor);
    {
        std::ofstream output(artifact, std::ios::trunc);
        output << "replacement engine";
    }
    CHECK_THROWS(r::ClassArtifactAdmission(artifact, {}, std::make_shared<const io::FileDigests>(digest)).Resolve(3, {}));
    CHECK_THROWS(r::ClassArtifactAdmission(artifact).Resolve(3, {}));
    std::filesystem::remove_all(root);
}
TEST_CASE("Zero foreground preserves physical logits bounds without detection allocation", "[model][rfdetr][layout][capacity]") {
    const auto capacity = r::PredictionCapacity::Resolve(500, 2, 300, 1, 64, 64, true, 0);
    CHECK(capacity.candidates == 0);
    CHECK(capacity.mask_bytes == 0);
    CHECK(r::PredictionCapacity::Resolve(0, 2, 300, 1, 64, 64, true, 0).candidates == 0);
    CHECK_THROWS(r::PredictionCapacity::Resolve(500, 2, 300, 1, 64, 64, false, 2));
}
TEST_CASE("RF-DETR publication restores the complete bundle at every cancellation boundary", "[model][rfdetr][layout][publication]") {
    const mmltk::testsupport::ScopedTempDir root("class-bundle-publication");
    const auto artifact = root.path() / "model.engine";
    const auto companion = std::filesystem::path(artifact.string() + ".classes.json");
    const auto layout = r::native_training_class_layout(c::ClassCatalog({"cat"}));
    for (int stop_at = 1; stop_at <= 7; ++stop_at) {
        {
            std::ofstream output(artifact);
            output << "previous complete artifact";
        }
        const auto old_digest = io::sha256_file(artifact);
        const auto descriptor = r::encode_class_descriptor({1, io::sha256_hex(old_digest), layout});
        {
            std::ofstream output(companion);
            output << descriptor;
        }
        {
            r::ClassArtifactPublication publication(artifact);
            {
                std::ofstream output(publication.staged_artifact());
                output << "replacement artifact";
            }
            int boundary = 0;
            CHECK_THROWS(publication.Publish(r::ModelClassDescriptor{1, {}, layout}, [&] { return ++boundary == stop_at; }));
        }
        CHECK(io::sha256_file(artifact) == old_digest);
        CHECK(r::detail::read_class_descriptor(companion).artifact_sha256 == io::sha256_hex(old_digest));
        for (const auto& entry : std::filesystem::directory_iterator(root.path())) CHECK_FALSE(entry.is_directory());
    }
    CHECK_THROWS(r::ClassArtifactPublication(artifact, companion));
    {
        r::ClassArtifactPublication missing_serialization(artifact);
        CHECK_THROWS(missing_serialization.Publish());
    }
    CHECK(r::detail::read_class_descriptor(companion).artifact_sha256 == io::sha256_hex(io::sha256_file(artifact)));
    {
        r::ClassArtifactPublication unfinished(artifact);
        std::ofstream output(unfinished.staged_artifact());
        output << "partial serialization";
    }
    CHECK(r::detail::read_class_descriptor(companion).artifact_sha256 == io::sha256_hex(io::sha256_file(artifact)));
    {
        r::ClassArtifactPublication embedded(artifact);
        {
            std::ofstream output(embedded.staged_artifact());
            output << "new embedded layout artifact";
        }
        embedded.Publish();
    }
    CHECK_FALSE(std::filesystem::exists(companion));
}
TEST_CASE("Class artifact admission waits for a complete published companion", "[model][rfdetr][layout][publication]") {
    const mmltk::testsupport::ScopedTempDir root("class-bundle-read");
    const auto artifact = root.path() / "model.engine";
    const auto layout = r::native_training_class_layout(c::ClassCatalog({"cat"}));
    std::promise<void> entered;
    auto started = entered.get_future();
    std::future<r::ModelClassLayout> admitted;
    {
        r::ClassArtifactPublication publication(artifact);
        {
            std::ofstream output(publication.staged_artifact());
            output << "new engine";
        }
        int boundary = 0;
        publication.Publish(r::ModelClassDescriptor{1, {}, layout}, [&] {
            if (++boundary == 5) {
                admitted = std::async(std::launch::async, [&] {
                    const auto digest = *io::try_file_digests(artifact, false);
                    entered.set_value();
                    return r::ClassArtifactAdmission(artifact, {}, std::make_shared<const io::FileDigests>(digest)).Resolve(2, {});
                });
                started.get();
                CHECK(admitted.wait_for(std::chrono::seconds(0)) == std::future_status::timeout);
            }
            return false;
        });
    }
    CHECK(admitted.get() == layout);
}
TEST_CASE("Staged class publication permits old readers and rejects a competing commit", "[model][rfdetr][layout][publication]") {
    const mmltk::testsupport::ScopedTempDir root("class-bundle-staged-read");
    const auto artifact = root.path() / "model.engine";
    const auto companion = std::filesystem::path(artifact.string() + ".classes.json");
    const auto layout = r::native_training_class_layout(c::ClassCatalog({"cat"}));
    {
        std::ofstream output(artifact);
        output << "old complete engine";
    }
    const auto original = *io::try_file_digests(artifact, false);
    {
        std::ofstream output(companion);
        output << r::encode_class_descriptor({1, io::sha256_hex(original.sha256), layout});
    }
    std::future<r::ModelClassLayout> reader;
    {
        r::ClassArtifactPublication first(artifact), competing(artifact);
        {
            std::ofstream output(first.staged_artifact());
            output << "first replacement";
        }
        {
            std::ofstream output(competing.staged_artifact());
            output << "competing replacement";
        }
        reader = std::async(std::launch::async,
                            [&] { return r::ClassArtifactAdmission(artifact, {}, std::make_shared<const io::FileDigests>(original)).Resolve(2, {}); });
        // Both producers still own staging here. The reader must finish before
        // either producer enters the named replacement window.
        REQUIRE(reader.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        CHECK(reader.get() == layout);
        first.Publish(r::ModelClassDescriptor{1, {}, layout});
        const auto winner = io::sha256_file(artifact);
        CHECK_THROWS(competing.Publish(r::ModelClassDescriptor{1, {}, layout}));
        CHECK_THROWS(competing.LockPreviousArtifact());
        CHECK(io::sha256_file(artifact) == winner);
        CHECK(r::detail::read_class_descriptor(companion).artifact_sha256 == io::sha256_hex(winner));
        CHECK_THROWS(r::ClassArtifactAdmission(artifact, {}, std::make_shared<const io::FileDigests>(original)).Resolve(2, {}));
    }
    CHECK(r::ClassArtifactAdmission(artifact).Resolve(2, {}) == layout);
}
TEST_CASE("A live stop after staging preserves the prior complete class bundle", "[model][rfdetr][layout][publication]") {
    const mmltk::testsupport::ScopedTempDir root("class-bundle-staged-stop");
    const auto artifact = root.path() / "model.engine";
    const auto companion = std::filesystem::path(artifact.string() + ".classes.json");
    const auto layout = r::native_training_class_layout(c::ClassCatalog({"cat"}));
    {
        std::ofstream output(artifact);
        output << "previous engine";
    }
    const auto original = io::sha256_file(artifact);
    {
        std::ofstream output(companion);
        output << r::encode_class_descriptor({1, io::sha256_hex(original), layout});
    }
    const auto original_companion = io::sha256_file(companion);
    std::stop_source source;
    const auto stop = source.get_token();
    {
        r::ClassArtifactPublication publication(artifact);
        {
            std::ofstream output(publication.staged_artifact());
            output << "finished candidate";
        }
        source.request_stop();
        CHECK_THROWS_AS(publication.Publish(r::ModelClassDescriptor{1, {}, layout}, [&] { return stop.stop_requested(); }), r::ArtifactPublicationCancelled);
    }
    CHECK(io::sha256_file(artifact) == original);
    CHECK(io::sha256_file(companion) == original_companion);
    for (const auto& entry : std::filesystem::directory_iterator(root.path())) CHECK_FALSE(entry.is_directory());
}
TEST_CASE("Retained class admission rejects changes across model construction", "[model][rfdetr][layout][artifact]") {
    const mmltk::testsupport::ScopedTempDir root("retained-class-admission");
    const auto artifact = root.path() / "model.engine";
    const auto companion = std::filesystem::path(artifact.string() + ".classes.json");
    const auto selected = root.path() / "selected.json";
    {
        std::ofstream output(artifact);
        output << "admitted model bytes";
    }
    const auto proof = std::make_shared<const io::FileDigests>(*io::try_file_digests(artifact, false));
    const auto layout = r::native_training_class_layout(c::ClassCatalog({"cat"}));
    const auto encoded = r::encode_class_descriptor({1, io::sha256_hex(proof->sha256), layout});
    const auto write_descriptor = [&](const std::filesystem::path& path) {
        std::ofstream output(path);
        output << encoded;
    };
    write_descriptor(companion);
    write_descriptor(selected);
    const r::ClassArtifactAdmission admission(artifact, selected, proof);
    CHECK(admission.file() == proof);
    CHECK(admission.artifact_path() == std::filesystem::absolute(artifact).lexically_normal());
    CHECK(admission.descriptor_path() == std::filesystem::absolute(selected).lexically_normal());
    CHECK(admission.Matches(root.path() / "." / "model.engine", selected));
    CHECK(admission.Resolve(2, layout) == layout);
    SECTION("unchanged proof and parsed facts are reused") {
        CHECK_NOTHROW(admission.RequireUnchanged());
        CHECK(admission.Resolve(2, {}) == layout);
        CHECK(admission.file() == proof);
    }
    SECTION("identical artifact replacement is a new admission") {
        const auto replacement = root.path() / "replacement";
        std::filesystem::copy_file(artifact, replacement);
        std::filesystem::rename(replacement, artifact);
        CHECK_FALSE(admission.Matches(artifact, selected));
        CHECK_THROWS(admission.Resolve(2, layout));
        CHECK_THROWS(r::ClassArtifactAdmission(artifact, selected, proof));
    }
    SECTION("artifact removal invalidates completion") {
        std::filesystem::remove(artifact);
        CHECK_THROWS(admission.RequireUnchanged());
    }
    for (const auto& path : {companion, selected}) {
        DYNAMIC_SECTION("descriptor replacement " << path.filename()) {
            const auto replacement = root.path() / "replacement.json";
            write_descriptor(replacement);
            std::filesystem::rename(replacement, path);
            CHECK_FALSE(admission.Matches(artifact, selected));
            CHECK_THROWS(admission.Resolve(2, layout));
        }
        DYNAMIC_SECTION("descriptor removal " << path.filename()) {
            std::filesystem::remove(path);
            if (path == selected)
                CHECK_THROWS(admission.Matches(artifact, selected));
            else
                CHECK_FALSE(admission.Matches(artifact, selected));
            CHECK_THROWS(admission.Resolve(2, layout));
        }
    }
}
TEST_CASE("Class admission records absent descriptors and rejects conflicting facts", "[model][rfdetr][layout][artifact]") {
    const mmltk::testsupport::ScopedTempDir root("class-admission-absence");
    const auto artifact = root.path() / "model.engine";
    const auto companion = std::filesystem::path(artifact.string() + ".classes.json");
    const auto selected = root.path() / "selected.json";
    {
        std::ofstream output(artifact);
        output << "external model";
    }
    const r::ClassArtifactAdmission absent(artifact);
    CHECK_FALSE(r::ResolvedClassLayout(absent.Resolve(2, {})).semantic());
    CHECK_THROWS(r::ClassArtifactAdmission(artifact, selected, absent.file()));
    const auto layout = r::native_training_class_layout(c::ClassCatalog({"cat"}));
    const r::ModelClassDescriptor descriptor{1, io::sha256_hex(absent.file()->sha256), layout};
    {
        std::ofstream output(companion);
        output << r::encode_class_descriptor(descriptor);
    }
    CHECK_FALSE(absent.Matches(artifact));
    CHECK_THROWS(absent.Resolve(2, {}));
    {
        std::ofstream output(selected);
        output << r::encode_class_descriptor(descriptor);
    }
    const r::ClassArtifactAdmission matching(artifact, selected, absent.file());
    CHECK(matching.file() == absent.file());
    CHECK(matching.Resolve(2, layout) == layout);
    CHECK_THROWS(matching.Resolve(2, r::native_training_class_layout(c::ClassCatalog({"dog"}))));
    auto conflicting = descriptor;
    conflicting.layout = r::native_training_class_layout(c::ClassCatalog({"dog"}));
    {
        std::ofstream output(selected);
        output << r::encode_class_descriptor(conflicting);
    }
    CHECK_THROWS(r::ClassArtifactAdmission(artifact, selected, absent.file()).Resolve(2, {}));
    CHECK_THROWS(matching.Resolve(2, layout));
}
TEST_CASE("Class admission observes live cancellation at digest and completion boundaries", "[model][rfdetr][layout][cancellation]") {
    const mmltk::testsupport::ScopedTempDir root("class-admission-stop");
    const auto artifact = root.path() / "model.engine";
    {
        std::ofstream output(artifact);
        output << "external model";
    }
    std::stop_source source;
    const auto stop = source.get_token();
    SECTION("before hashing") {
        source.request_stop();
        CHECK_THROWS_AS(r::ClassArtifactAdmission(artifact, {}, {}, stop), r::ArtifactPublicationCancelled);
    }
    SECTION("the shared digest reader observes cancellation after opening") {
        int checkpoints = 0;
        const auto digest = io::try_file_digests(artifact, false, [&] {
            if (++checkpoints == 2) source.request_stop();
            return stop.stop_requested();
        });
        CHECK_FALSE(digest);
        CHECK(checkpoints == 2);
        CHECK_THROWS_AS(r::ClassArtifactAdmission(artifact, {}, {}, stop), r::ArtifactPublicationCancelled);
    }
    SECTION("after opaque construction") {
        const r::ClassArtifactAdmission admission(artifact, {}, {}, stop);
        const auto loaded_layout = r::unresolved_class_layout(2);
        source.request_stop();
        CHECK_THROWS_AS(admission.Resolve(2, loaded_layout, stop), r::ArtifactPublicationCancelled);
        CHECK_NOTHROW(admission.RequireUnchanged());
    }
}
