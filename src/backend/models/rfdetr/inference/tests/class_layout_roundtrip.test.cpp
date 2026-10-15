#include "src/backend/ml/torch/tests/catch_support.h"
#include "src/backend/models/rfdetr/core/tests/class_artifact_fixture.h"
#include "src/common/io/file_digest.h"
#include "src/backend/models/rfdetr/core/class_artifact.h"
#include "src/backend/models/rfdetr/core/detail/class_artifact_files.h"
#include <filesystem>
#include <fstream>
#include <future>
#include <limits>
#include <chrono>
#include <stop_token>
#include "src/test_support/filesystem_test_utils.hpp"
#include "src/backend/models/rfdetr/core/class_layout.h"
#include "src/backend/models/rfdetr/core/artifact_publication.h"
#include "src/backend/models/rfdetr/inference/prediction_capacity.h"
#include <algorithm>
#include <catch2/generators/catch_generators.hpp>
#if MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER
#include "src/backend/models/rfdetr/core/tests/checkpoint_fixture_support/upstream_oracle_fixture.h"
#include "src/backend/models/rfdetr/core/postprocess.h"
#include "src/backend/models/rfdetr/core/evaluation.h"
#include "src/test_support/cuda_test_utils.hpp"
#include <c10/cuda/CUDAGuard.h>
#include <numeric>
#include <cstring>
#endif
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
 const r::ResolvedClassLayout empty(r::native_training_class_layout(c::ClassCatalog{}));
 const auto capacity = r::PredictionCapacity::Resolve(500, 2, 300, empty.output_width(), 64, 64, true, empty.eligible_count());
 CHECK(capacity.candidates == 0);
 CHECK(capacity.mask_bytes == 0);
 CHECK(r::PredictionCapacity::Resolve(0, 2, 300, 1, 64, 64, true, 0).candidates == 0);
 CHECK_THROWS(r::PredictionCapacity::Resolve(500, 2, 300, 1, 64, 64, false, 2));
 CHECK_THROWS(r::PredictionCapacity::Resolve(500, 2, 0, 1, 64, 64, false, 0));
 CHECK_THROWS(r::PredictionCapacity::Resolve(500, 2, 300, 0, 64, 64, false, 0));
 CHECK_THROWS(r::PredictionCapacity::Resolve(500, std::numeric_limits<std::size_t>::max(), 300, 1, 64, 64, false, 0));
 CHECK_THROWS(r::PredictionCapacity::Resolve(500, 2, 300, 1, 65536, 65536, true, 0));
}
TEST_CASE("Nonempty class layouts retain full physical ranking capacity", "[model][rfdetr][layout][capacity]") {
 auto permutation = r::native_training_class_layout(c::ClassCatalog({"cat", "dog"}));
 permutation.slots = {{r::ClassSlotRole::Foreground, 1U}, {r::ClassSlotRole::Unused, {}}, {r::ClassSlotRole::Foreground, 0U}};
 for (const auto& record : {permutation, r::coco_class_layout({r::ClassLayoutOrigin::VerifiedAsset, "fixture", {}}), r::unresolved_class_layout(91)}) {
  const r::ResolvedClassLayout layout(record);
  const auto physical = 2 * layout.output_width();
  const auto capacity = r::PredictionCapacity::Resolve(500, 1, 2, layout.output_width(), 64, 64, true, layout.eligible_count());
  CHECK(capacity.candidates == physical);
  CHECK(capacity.mask_bytes == 64U * 64U);
  CHECK(r::PredictionCapacity::Resolve(1, 1, 2, layout.output_width(), 64, 64, false, layout.eligible_count()).candidates == 1);
 }
}
TEST_CASE("RF-DETR publication restores the complete bundle at every cancellation boundary", "[model][rfdetr][layout][publication]") {
 const mmltk::testsupport::ScopedTempDir root("class-bundle-publication");
 // CLEANUP-IGNORE: Artifact, companion path, and class layout are explicit inputs to different publication cancellation and competing-reader cases.
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
 // CLEANUP-IGNORE: Artifact, companion path, and class layout are explicit inputs to different publication cancellation and competing-reader cases.
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
  reader = std::async(std::launch::async, [&] { return r::ClassArtifactAdmission(artifact, {}, std::make_shared<const io::FileDigests>(original)).Resolve(2, {}); });
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
 const auto layout = r::native_training_class_layout(c::ClassCatalog({"cat"}));
 const r::test_support::ClassArtifactFixture bundle(artifact, "previous engine", layout);
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
 bundle.CheckPreserved();
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

TEST_CASE("Engine companions bind joint task schema and reject competing contracts", "[model][rfdetr][layout][artifact][keypoints]") {
 const mmltk::testsupport::ScopedTempDir root("joint-engine-contract");
 const auto artifact = root.path() / "model.engine";
 { std::ofstream output(artifact); output << "joint engine fixture"; }
 c::OrderedClassCatalog names;
 names.names = {{"marker"}};
 names.keypoints = std::vector{c::ClassKeypointSchema{{"left","right"},{{0,1}},{1,0},{.1,.1}}};
 r::ModelClassDescriptor descriptor;
 descriptor.artifact_sha256 = io::sha256_hex(io::sha256_file(artifact));
 descriptor.layout = r::native_training_class_layout(c::ClassCatalog(names));
 descriptor.output_roles = {{"pred_logits",r::RfdetrOutputRole::Logits},{"pred_boxes",r::RfdetrOutputRole::Boxes},{"pred_keypoints",r::RfdetrOutputRole::Keypoints}};
 if (GENERATE(false, true)) {
  for (auto& output : descriptor.output_roles) output.name = "producer_" + output.name;
  std::ranges::reverse(descriptor.output_roles);
 }
 descriptor.output_contract = r::RfdetrOutputContract{1U,r::ModelTask::Keypoints,{2}};
 CHECK(r::decode_class_descriptor(r::encode_class_descriptor(descriptor)) == descriptor);
 const auto write = [&](const std::filesystem::path& destination) { std::ofstream output(destination); output << r::encode_class_descriptor(descriptor); };
 write(artifact.string() + ".classes.json");
 const r::ClassArtifactAdmission admitted(artifact);
 CHECK(admitted.output_contract() == descriptor.output_contract);
 CHECK(admitted.output_roles() == descriptor.output_roles);
 CHECK(admitted.Resolve(2,{}) == descriptor.layout);
 const auto selected = root.path() / "selected.classes.json";
 descriptor.output_contract->keypoints_per_class.push_back(0);
 write(selected);
 CHECK_THROWS(r::ClassArtifactAdmission(artifact,selected).output_contract());
 descriptor.output_contract->keypoints_per_class = {1};
 CHECK_THROWS(r::encode_class_descriptor(descriptor));
}

#if MMLTK_RFDETR_PYTHON_CHECKPOINT_LOADER
TEST_CASE("Pinned upstream tied selection retains physical schema and original-space joints", "[model][rfdetr][layout][keypoints][upstream_oracle][cuda]") {
 namespace support = r::testsupport;
 using Policy = support::OracleComparisonPolicy;
 const auto requested = support::UpstreamOracle::requested();
 REQUIRE(requested);
 REQUIRE(mmltk::testsupport::checked_cuda_device_count() > 0);
 c10::cuda::CUDAGuard guard(0);
 const support::UpstreamOracle oracle(*requested);
 const auto archive = oracle.keypoint_archive();
 support::OracleComparison comparison(*requested,"keypoint-selection",torch::kFloat32,"actual pinned stable selection, class slots, pixel coordinates and covariance");
 for (const auto& item : oracle.keypoint_cases()) {
  const auto id = item.at("id").get<std::string>(), prefix = id + "/selection/";
  const auto counts = item.at("counts").get<std::vector<std::uint16_t>>();
  r::OutputTensors outputs{archive.tensor(prefix+"logits",torch::kCUDA),archive.tensor(prefix+"input_boxes",torch::kCUDA),{},archive.tensor(id+"/padded",torch::kCUDA),counts};
  // Physical top-k is observed before any named-layout filtering. This keeps
  // the upstream tie ordering independent of the application class mapping.
  const auto selected = r::select_output_batch_fixed_size(outputs,200,400,2*counts.size());
  comparison.compare(prefix+"queries",selected.query_indices,archive.tensor(prefix+"queries"),Policy::Exact);
  comparison.compare(prefix+"scores",selected.scores[0],archive.tensor(prefix+"scores"),Policy::Value);
  comparison.compare(prefix+"labels",selected.labels[0],archive.tensor(prefix+"labels"),Policy::Exact);
  comparison.compare(prefix+"boxes",selected.boxes[0],archive.tensor(prefix+"boxes"),Policy::Value);
  REQUIRE(selected.keypoints);
  const auto packets = selected.keypoints->squeeze(0).cpu().contiguous();
  const auto active = torch::arange(packets.size(1),torch::kInt32).unsqueeze(0).lt(selected.keypoint_counts[0].cpu().unsqueeze(1));
  using Packet = r::KeypointPredictionPacket;
  const auto coordinates = torch::stack({Packet::select<^^r::KeypointPrediction::x>(packets,-1),Packet::select<^^r::KeypointPrediction::y>(packets,-1),Packet::select<^^r::KeypointPrediction::findable_logit>(packets,-1).sigmoid().masked_fill(~active,0)},-1);
  comparison.compare(prefix+"keypoints",coordinates,archive.tensor(prefix+"keypoints"),Policy::Value);
  auto covariance = torch::empty({packets.size(0),packets.size(1),2,2},torch::kFloat32);
  for (int64_t detection=0;detection<packets.size(0);++detection) {
   for (int64_t joint=0;joint<packets.size(1);++joint) {
    r::KeypointPrediction packet;
    std::memcpy(&packet,packets[detection][joint].data_ptr<float>(),sizeof(packet));
    const auto value = r::keypoint_covariance(r::decode_prediction_keypoint(packet));
    covariance[detection][joint].copy_(torch::tensor({value[0],value[1],value[1],value[2]}).reshape({2,2}));
   }
  }
  const auto expected_covariance = archive.tensor(prefix+"covariance"), finite = torch::isfinite(expected_covariance);
  comparison.compare(prefix+"covariance-availability",torch::isfinite(covariance),finite,Policy::Exact);
  comparison.compare(prefix+"covariance-values",covariance.masked_select(finite),expected_covariance.masked_select(finite),Policy::Value);
  if (!item.contains("classes")) continue;
  // Names are fixture inputs in the upstream record. Native admission owns the
  // semantic mapping; native output is compared with the upstream physical
  // rows filtered and mapped through that explicit external declaration.
  r::ModelClassLayout layout;
  layout.no_object=r::NoObjectEncoding::AllNegative;
  layout.provenance={r::ClassLayoutOrigin::Embedded,"upstream-schema-fixture",{}};
  layout.foreground.keypoints.emplace();
  for (std::size_t slot=0;slot<counts.size();++slot) {
   if (item.at("classes").at(slot).is_null()) {
    layout.slots.push_back({r::ClassSlotRole::Background,{}});
    continue;
   }
   layout.slots.push_back({r::ClassSlotRole::Foreground,static_cast<std::uint32_t>(layout.foreground.names.size())});
   layout.foreground.names.push_back({item.at("classes").at(slot).get<std::string>()});
   c::ClassKeypointSchema joints;
   joints.names=item.at("joint_names").at(slot).get<std::vector<std::string>>();
   joints.oks_sigmas.assign(joints.names.size(),.1);
   joints.horizontal_flip.resize(joints.names.size());
   std::iota(joints.horizontal_flip.begin(),joints.horizontal_flip.end(),0);
   layout.foreground.keypoints->push_back(std::move(joints));
  }
  auto admitted=std::make_shared<const r::ResolvedClassLayout>(r::decode_class_layout(r::encode_class_layout(layout)));
  r::KeypointTensorLayout(counts).require_class_layout(*admitted);
  CHECK(admitted->record()==layout);
  r::PostprocessLane lane(admitted);
  const auto semantic=r::select_output_batch_fixed_size(outputs,200,400,2*counts.size(),false,&lane);
  const auto references=admitted->physical_references();
  const auto mapped=torch::tensor(std::vector<int64_t>(references.begin(),references.end()),torch::kInt64).index_select(0,archive.tensor(prefix+"labels"));
  const auto eligible=torch::nonzero(mapped.ge(0)).flatten();
  REQUIRE(semantic.counts.item<int64_t>()==eligible.numel());
  comparison.compare(prefix+"semantic-labels",semantic.labels[0].narrow(0,0,eligible.numel()),mapped.index_select(0,eligible),Policy::Exact);
  comparison.compare(prefix+"semantic-scores",semantic.scores[0].narrow(0,0,eligible.numel()),archive.tensor(prefix+"scores").index_select(0,eligible),Policy::Value);
  comparison.compare(prefix+"semantic-boxes",semantic.boxes[0].narrow(0,0,eligible.numel()),archive.tensor(prefix+"boxes").index_select(0,eligible),Policy::Value);
 }
}
#endif
