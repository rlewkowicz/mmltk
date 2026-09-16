#pragma once
#include "src/controller/presentation/presentation_system.h"
#include "src/controller/services/file_dialog_system.h"
#include "src/controller/services/settings_system.h"
#include "src/controller/subsystems/annotation/annotation_system.h"
#include "src/controller/subsystems/explore/explore_system.h"
#include "src/controller/subsystems/live/live_system.h"
#include "src/controller/subsystems/validate/validation_system.h"
#include "src/controller/subsystems/export/export_system.h"
#include "src/controller/subsystems/system/predict_system.h"
#include "src/controller/subsystems/system/dataset_system.h"
#include "src/controller/subsystems/system/model_system.h"
#include "src/controller/subsystems/train/training_system.h"
#include "src/controller/subsystems/upscale/upscale_system.h"
namespace mmltk::controller {
// The application contract is reflected from this composition. A member name
// is the stable system identity and the pointed-to ordinary system owns its
// annotated intents, snapshot, and event variant. ApplicationShell supplies
// the live objects; this declaration owns no lifetime.
struct ApplicationSystems final {
    SettingsSystem* settings = nullptr;
    FileDialogSystem* file_dialog = nullptr;
    DatasetSystem* dataset = nullptr;
    ModelSystem* model = nullptr;
    TrainingSystem* training = nullptr;
    ValidationSystem* validation = nullptr;
    ExportSystem* export_system = nullptr;
    PredictSystem* predict = nullptr;
    ExploreSystem* explore = nullptr;
    AnnotationSystem* annotation = nullptr;
    UpscaleSystem* upscale = nullptr;
    LiveSystem* live = nullptr;
    PresentationSystem* presentation = nullptr;
};
}  // namespace mmltk::controller
