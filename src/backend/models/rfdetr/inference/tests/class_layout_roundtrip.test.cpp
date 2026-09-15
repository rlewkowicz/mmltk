#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <future>
#include <chrono>
#include <stop_token>
#include "filesystem_test_utils.hpp"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/backend/models/rfdetr/core/artifact_publication.h"
#include "src/backend/models/rfdetr/inference/prediction_capacity.h"
namespace r = mmltk::backend::models::rfdetr;
namespace c = mmltk::backend::data::catalog;
namespace io = mmltk::common::io;
TEST_CASE("Digest-bound class bundles survive renaming and reject stale companions", "[model][rfdetr][layout][artifact]") {
    const auto root = mmltk::testsupport::make_temp_root("class-layout-bundle");
    const auto artifact = root / "model.engine";
    { std::ofstream output(artifact); output << "engine bytes for descriptor admission"; }
    const auto digest = *io::try_file_digests(artifact, false);
    const auto layout = r::native_training_class_layout(c::ClassCatalog({"background", "cat"}));
    const auto write = [&](const std::filesystem::path& path, const r::ModelClassDescriptor& descriptor) {
        std::ofstream output(path);
        output << r::encode_class_descriptor(descriptor);
    };
    const r::ModelClassDescriptor descriptor{1, io::sha256_hex(digest.sha256), layout};
    write(artifact.string() + ".classes.json", descriptor);
    CHECK(r::admit_artifact_class_layout(artifact, 3, {}, digest) == layout);
    const auto admitted = r::ClassArtifactSnapshot::Read(artifact);
    const auto renamed = root / "renamed.engine";
    std::filesystem::copy_file(artifact, renamed);
    std::filesystem::copy_file(artifact.string() + ".classes.json", renamed.string() + ".classes.json");
    CHECK(r::admit_artifact_class_layout(renamed, 3, {}, *io::try_file_digests(renamed, false)) == layout);
    CHECK_THROWS(r::admit_artifact_class_layout(artifact, 4, {}, digest));
    auto other = descriptor;
    other.layout = r::native_training_class_layout(c::ClassCatalog({"cat", "background"}));
    const auto explicit_path = root / "selected.classes.json";
    write(explicit_path, other);
    CHECK_THROWS(r::admit_artifact_class_layout(artifact, 3, {}, digest, explicit_path));
    std::filesystem::remove(artifact.string() + ".classes.json");
    CHECK(r::ClassArtifactSnapshot::Read(artifact) != admitted);
    const r::ResolvedClassLayout raw(r::admit_artifact_class_layout(artifact, 3, {}, digest));
    CHECK(raw.domain() == c::ClassReferenceDomain::RawOutputSlot);
    write(artifact.string() + ".classes.json", descriptor);
    { std::ofstream output(artifact, std::ios::trunc); output << "replacement engine"; }
    CHECK_THROWS(r::admit_artifact_class_layout(artifact, 3, {}, digest));
    CHECK_THROWS(r::admit_artifact_class_layout(artifact, 3, {}, *io::try_file_digests(artifact, false)));
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
        { std::ofstream output(artifact); output << "previous complete artifact"; }
        const auto old_digest = io::sha256_file(artifact);
        const auto descriptor = r::encode_class_descriptor({1, io::sha256_hex(old_digest), layout});
        { std::ofstream output(companion); output << descriptor; }
        {
            r::ClassArtifactPublication publication(artifact);
            { std::ofstream output(publication.staged_artifact()); output << "replacement artifact"; }
            int boundary = 0;
            CHECK_THROWS(publication.Publish(r::ModelClassDescriptor{1, {}, layout}, [&] { return ++boundary == stop_at; }));
        }
        CHECK(io::sha256_file(artifact) == old_digest);
        CHECK(r::read_class_descriptor(companion).artifact_sha256 == io::sha256_hex(old_digest));
        for (const auto& entry : std::filesystem::directory_iterator(root.path()))
            CHECK_FALSE(entry.is_directory());
    }
    CHECK_THROWS(r::ClassArtifactPublication(artifact, companion));
    {
        r::ClassArtifactPublication missing_serialization(artifact);
        CHECK_THROWS(missing_serialization.Publish());
    }
    CHECK(r::read_class_descriptor(companion).artifact_sha256 == io::sha256_hex(io::sha256_file(artifact)));
    {
        r::ClassArtifactPublication unfinished(artifact);
        std::ofstream output(unfinished.staged_artifact()); output << "partial serialization";
    }
    CHECK(r::read_class_descriptor(companion).artifact_sha256 == io::sha256_hex(io::sha256_file(artifact)));
    {
        r::ClassArtifactPublication embedded(artifact);
        { std::ofstream output(embedded.staged_artifact()); output << "new embedded layout artifact"; }
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
        { std::ofstream output(publication.staged_artifact()); output << "new engine"; }
        int boundary = 0;
        publication.Publish(r::ModelClassDescriptor{1, {}, layout}, [&] {
            if (++boundary == 5) {
                admitted = std::async(std::launch::async, [&] {
                    const auto digest = *io::try_file_digests(artifact, false);
                    entered.set_value();
                    return r::admit_artifact_class_layout(artifact, 2, {}, digest);
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
    { std::ofstream output(artifact); output << "old complete engine"; }
    const auto original = *io::try_file_digests(artifact, false);
    { std::ofstream output(companion); output << r::encode_class_descriptor({1, io::sha256_hex(original.sha256), layout}); }
    std::future<r::ModelClassLayout> reader;
    {
        r::ClassArtifactPublication first(artifact), competing(artifact);
        { std::ofstream output(first.staged_artifact()); output << "first replacement"; }
        { std::ofstream output(competing.staged_artifact()); output << "competing replacement"; }
        reader = std::async(std::launch::async, [&] { return r::admit_artifact_class_layout(artifact, 2, {}, original); });
        // Both producers still own staging here. The reader must finish before
        // either producer enters the named replacement window.
        REQUIRE(reader.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
        CHECK(reader.get() == layout);
        first.Publish(r::ModelClassDescriptor{1, {}, layout});
        const auto winner = io::sha256_file(artifact);
        CHECK_THROWS(competing.Publish(r::ModelClassDescriptor{1, {}, layout}));
        CHECK_THROWS(competing.LockPreviousArtifact());
        CHECK(io::sha256_file(artifact) == winner);
        CHECK(r::read_class_descriptor(companion).artifact_sha256 == io::sha256_hex(winner));
        CHECK_THROWS(r::admit_artifact_class_layout(artifact, 2, {}, original));
    }
    CHECK(r::admit_artifact_class_layout(artifact, 2, {}, *io::try_file_digests(artifact, false)) == layout);
}

TEST_CASE("A live stop after staging preserves the prior complete class bundle", "[model][rfdetr][layout][publication]") {
    const mmltk::testsupport::ScopedTempDir root("class-bundle-staged-stop");
    const auto artifact = root.path() / "model.engine";
    const auto companion = std::filesystem::path(artifact.string() + ".classes.json");
    const auto layout = r::native_training_class_layout(c::ClassCatalog({"cat"}));
    { std::ofstream output(artifact); output << "previous engine"; }
    const auto original = io::sha256_file(artifact);
    { std::ofstream output(companion); output << r::encode_class_descriptor({1, io::sha256_hex(original), layout}); }
    const auto original_companion = io::sha256_file(companion);
    std::stop_source source;
    const auto stop = source.get_token();
    {
        r::ClassArtifactPublication publication(artifact);
        { std::ofstream output(publication.staged_artifact()); output << "finished candidate"; }
        source.request_stop();
        CHECK_THROWS_AS(publication.Publish(r::ModelClassDescriptor{1, {}, layout}, [&] { return stop.stop_requested(); }), r::ArtifactPublicationCancelled);
    }
    CHECK(io::sha256_file(artifact) == original);
    CHECK(io::sha256_file(companion) == original_companion);
    for (const auto& entry : std::filesystem::directory_iterator(root.path())) CHECK_FALSE(entry.is_directory());
}
