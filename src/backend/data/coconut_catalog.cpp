#include "detail/coconut_catalog.h"
#include <array>
#include <filesystem>
#include <utility>
#include <stdexcept>
#include <string>
namespace mmltk::backend::data::benchmark_internal {
namespace {
CatalogArtifact hf(std::string_view repo, std::string_view revision, std::string file, std::uint64_t bytes, std::string hash) {
 return {"coconut-" + std::string(repo) + "-" + std::filesystem::path(file).filename().string(),
  "https://huggingface.co/datasets/xdeng77/" + std::string(repo) + "/resolve/" + std::string(revision) + "/" + file, std::filesystem::path(file).filename().string(), bytes, std::move(hash),
  BenchmarkDatasetSource::kCoconut};
}
}  // namespace
std::span<const CoconutReleaseComponent> coconut_release_catalog() {
 static const std::array components{
  CoconutReleaseComponent{CoconutEdition::Base, "coconut_b", "f9180881fc6cb441a5d7deeba59a3e929387ee41", 241602U,
   {hf("coconut_b", "f9180881fc6cb441a5d7deeba59a3e929387ee41", "data/train-00000-of-00004.parquet", 417218921U, "cfddd65e62ffeafe32229a43f06c475facfb039d2107be02b380134cb4be38ae"),
    hf("coconut_b", "f9180881fc6cb441a5d7deeba59a3e929387ee41", "data/train-00001-of-00004.parquet", 417769003U, "dc2b2620aa21cf5e65aad124cbc340c6497ddbb3df4d52d9aaea6a8ba045b5b0"),
    hf("coconut_b", "f9180881fc6cb441a5d7deeba59a3e929387ee41", "data/train-00002-of-00004.parquet", 400871427U, "da7a7d97b0673f40f5a8beccab1c83fa5775d75896f2b64c109090f159074290"),
    hf("coconut_b", "f9180881fc6cb441a5d7deeba59a3e929387ee41", "data/train-00003-of-00004.parquet", 405293848U, "77795dd8a8f2b356f45be708bb161d9a24aa74d11ef8ef93cacc958c4b4ae8ec")}},
  CoconutReleaseComponent{CoconutEdition::RelabeledValidation, "relabeled_coco_val", "f41d331a4f6e906579bf13c9e379d84e273a23f7", 5000U,
   {hf("relabeled_coco_val", "f41d331a4f6e906579bf13c9e379d84e273a23f7", "data/train-00000-of-00001.parquet", 32911960U, "c789a84761f6f7c94ad893ae014fc60f816876afa2177c745a230fb9baf4d109")}},
  CoconutReleaseComponent{CoconutEdition::Large, "coconut_large", "ce8605783330498d380d2ef2e52fcbe5194419b3", 0U,
   {hf("coconut_large", "ce8605783330498d380d2ef2e52fcbe5194419b3", "panoptic_object365.tar", 1081620480U, "dbe35c1fdc82e1b42482c4cfb127b1c3dfe2ce2b2fe19300507cb0258e4a4a61"),
    hf("coconut_large", "ce8605783330498d380d2ef2e52fcbe5194419b3", "panseg_object365_train_v2.json", 269109522U, "a1fa2f5791a02ae49c76eb854c5cbc32e8397278f8d37031ad11e6a1a832ed90")}},
  CoconutReleaseComponent{CoconutEdition::XLarge, "coconut_xlarge", "2f9c829748bc52b186b4ae180a9917fcdd7840ab", 0U,
   {hf("coconut_xlarge", "2f9c829748bc52b186b4ae180a9917fcdd7840ab", "coconuts_xlarge.tar", 3186698240U, "eee06af317b733e779fb656462a9213dcb9526d7a2d1dd7fdef470bfb3a0dcbc")}},
  CoconutReleaseComponent{CoconutEdition::ObjectsValidation, "coconut_val", "e41441b35b54a7db685cec2d284bbd9102b5a00b", 0U,
   {hf("coconut_val", "e41441b35b54a7db685cec2d284bbd9102b5a00b", "coconut_val.json", 98286577U, "bdec55c675c8cc25741d772f3b8143c01cd4c5f223274d4800346293ae3d2ec9"),
    hf("coconut_val", "e41441b35b54a7db685cec2d284bbd9102b5a00b", "coconut_val.tar", 262789120U, "c38d687dd89fac616148ad0ebc7bb7c206fa36c9f883f909f05510f458d963b4")}}};
 return components;
}
const CoconutReleaseComponent& coconut_release_component(CoconutEdition edition) {
 for (const auto& component : coconut_release_catalog())
  if (component.edition == edition) return component;
 throw std::invalid_argument("unknown COCONut edition");
}
const CatalogArtifact& coconut_unlabeled_images_artifact() {
 static const CatalogArtifact artifact{"coco-unlabeled2017", "http://images.cocodataset.org/zips/unlabeled2017.zip", "unlabeled2017.zip", 20126613414ULL, ""};
 return artifact;
}
const CatalogArtifact& coconut_validation_images_artifact() {
 static const CatalogArtifact artifact{"coconut-objects365-validation-images", "https://drive.usercontent.google.com/download?id=1-wzLtddJucBVBJ67ailLrfMNmLGFag4i&export=download&confirm=t",
  "object365_val_images.tar", 5084467200ULL, "", BenchmarkDatasetSource::kObjects365V1};
 return artifact;
}
std::span<const unsigned> coconut_objects_training_shards(CoconutEdition edition) {
 static constexpr std::array large{32U, 35U, 40U, 50U};
 static constexpr std::array xlarge{17U, 23U, 25U, 28U, 38U, 42U, 44U};
 if (edition == CoconutEdition::Large) return large;
 if (edition == CoconutEdition::XLarge) return xlarge;
 throw std::invalid_argument("COCONut edition has no Objects365 training patches");
}
std::vector<CatalogArtifact> coconut_objects_training_artifacts(CoconutEdition edition) {
 const auto all = objects365_train_image_artifacts();
 std::vector<CatalogArtifact> result;
 for (const auto patch : coconut_objects_training_shards(edition)) result.push_back(all.at(patch));
 return result;
}
std::string_view coconut_namespace_name(CoconutImageNamespace source) {
 switch (source) {
  case CoconutImageNamespace::CocoTrain: return "coco-train2017";
  case CoconutImageNamespace::CocoUnlabeled: return "coco-unlabeled2017";
  case CoconutImageNamespace::CocoValidation: return "coco-val2017";
  case CoconutImageNamespace::Objects365V1: return "objects365-v1";
  case CoconutImageNamespace::Objects365V2: return "objects365-v2";
 }
 throw std::invalid_argument("invalid COCONut image namespace");
}
}  // namespace mmltk::backend::data::benchmark_internal
