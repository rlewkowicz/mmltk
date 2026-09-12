use crate::fluent_theme::Element;
use crate::generated::FeatureId;
use crate::presentation_surface::Surface;
use crate::view::{annotation, explore, export, live, navigation, predict, train, validate};
use crate::view_model::ApplicationModel;

#[derive(Debug, Clone)]
pub enum Message {
    Navigation(navigation::Message),
    Train(train::Message),
    Validate(validate::Message),
    Predict(predict::Message),
    Live(live::Message),
    Export(export::Message),
    Explore(explore::Message),
    Annotation(annotation::Message),
}

#[derive(Debug, Clone)]
pub enum Outcome {
    FeatureSelected(FeatureId),
    SettingsRequested,
    Train(train::Outcome),
    Validate(validate::Outcome),
    Predict(predict::Outcome),
    Live(live::Outcome),
    Export(export::Outcome),
    Explore(explore::Outcome),
    Annotation(annotation::Outcome),
}

pub struct Router {
    active: FeatureId,
    train: train::Component,
    validate: validate::Component,
    predict: predict::Component,
    live: live::Component,
    export: export::Component,
    explore: explore::Component,
    annotation: annotation::Component,
}

impl Default for Router {
    fn default() -> Self {
        Self {
            active: FeatureId::Train,
            train: train::Component::default(),
            validate: validate::Component::default(),
            predict: predict::Component::default(),
            live: live::Component,
            export: export::Component::default(),
            explore: explore::Component::default(),
            annotation: annotation::Component::default(),
        }
    }
}

impl Router {
    #[cfg(test)]
    pub(crate) fn test_install_annotation_displayed(&self, model: &ApplicationModel) {
        self.annotation
            .test_install_displayed(model.annotation.snapshot.as_ref().unwrap());
    }

    pub const fn active(&self) -> FeatureId {
        self.active
    }

    pub fn select(&mut self, feature: FeatureId) {
        self.active = feature;
    }

    pub fn rebase(&mut self, feature: FeatureId, model: &ApplicationModel) {
        self.active = feature;
        self.train.rebase(model);
    }

    pub fn install_authoritative_components(&mut self, model: &ApplicationModel) {
        self.explore.rebase(model);
        self.annotation.rebase(model);
    }

    pub fn set_annotation_connection(
        &self,
        connection: Option<crate::transport_connection::Connection>,
    ) {
        self.annotation.set_connection(connection);
    }

    pub fn rebase_annotation(&mut self, model: &ApplicationModel) {
        self.annotation.rebase(model);
    }

    pub fn bootstrap_components(&mut self, model: &ApplicationModel) {
        self.explore.bootstrap(model);
        self.annotation.rebase(model);
    }

    pub fn reset_transport(&mut self, model: &ApplicationModel) {
        self.active = FeatureId::Train;
        self.train = train::Component::default();
        self.validate = validate::Component::default();
        self.predict = predict::Component::default();
        self.export = export::Component::default();
        self.bootstrap_components(model);
    }

    pub fn explore_record_submission(&mut self, request: crate::generated::ExploreFilterUpdate) {
        self.explore.record_submission(request);
    }

    pub fn explore_record_admission(&mut self, revision: u64) {
        self.explore.record_admission(revision);
    }

    pub fn explore_abandon_submission(&mut self) {
        self.explore.abandon_submission();
    }

    pub fn explore_request_viewport(
        &mut self,
        snapshot: Option<&crate::generated::ExploreSnapshot>,
        request: crate::generated::ExploreViewportUpdate,
    ) {
        self.explore.request_viewport(snapshot, request);
    }

    pub fn explore_dispatchable_viewport(&self) -> Option<crate::generated::ExploreViewportUpdate> {
        self.explore.dispatchable_viewport()
    }

    pub fn explore_viewport_queued(&mut self, request: crate::generated::ExploreViewportUpdate) {
        self.explore.viewport_queued(request);
    }

    pub fn explore_arm_writable_wait(&mut self) -> bool {
        self.explore.arm_viewport_writable_wait()
    }

    pub fn explore_viewport_writable(&mut self) {
        self.explore.viewport_writable();
    }

    pub fn explore_clear_viewport_admission(&mut self) {
        self.explore.clear_viewport_admission();
    }

    pub fn explore_measured_viewport(
        &self,
        columns: u32,
        first_row: u32,
        matching_count: u32,
    ) -> Option<crate::generated::ExploreViewport> {
        self.explore
            .measured_viewport(columns, first_row, matching_count)
    }

    pub(crate) fn explore_gallery_size(&self) -> Option<iced::Size> {
        self.explore.gallery_size()
    }

    #[cfg(test)]
    pub fn explore_measure_gallery(
        &mut self,
        width: f32,
        height: f32,
        maximum_extent: crate::generated::VisualExtent,
        columns: u32,
    ) -> bool {
        self.explore
            .measure_gallery(width, height, maximum_extent, columns)
    }

    pub fn explore_measured_layout_request(
        &self,
        snapshot: Option<&crate::generated::ExploreSnapshot>,
        columns: u32,
        matching_count: u32,
    ) -> Option<crate::generated::ExploreViewportUpdate> {
        self.explore
            .measured_layout_request(snapshot, columns, matching_count)
    }

    pub fn update(
        &mut self,
        model: &mut ApplicationModel,
        settings: &mut crate::view::settings::Component,
        message: Message,
    ) -> Result<Option<Outcome>, String> {
        let outcome = match message {
            Message::Navigation(message) => match navigation::update(message) {
                navigation::Outcome::PageSelected(feature) => Outcome::FeatureSelected(feature),
                navigation::Outcome::SettingsRequested => Outcome::SettingsRequested,
            },
            Message::Train(message) => {
                let Some(outcome) = self.train.update(settings.state_mut(), message)? else {
                    return Ok(None);
                };
                Outcome::Train(outcome)
            }
            Message::Validate(message) => {
                let Some(outcome) = self.validate.update(settings.state_mut(), message)? else {
                    return Ok(None);
                };
                Outcome::Validate(outcome)
            }
            Message::Predict(message) => {
                let Some(outcome) = self.predict.update(settings.state_mut(), message)? else {
                    return Ok(None);
                };
                Outcome::Predict(outcome)
            }
            Message::Live(message) => {
                let Some(outcome) = self.live.update(settings.state_mut(), message)? else {
                    return Ok(None);
                };
                Outcome::Live(outcome)
            }
            Message::Export(message) => {
                let Some(outcome) = self.export.update(settings.state_mut(), message)? else {
                    return Ok(None);
                };
                Outcome::Export(outcome)
            }
            Message::Explore(message) => {
                let Some(outcome) = self.explore.update(model, settings.state_mut(), message)?
                else {
                    return Ok(None);
                };
                Outcome::Explore(outcome)
            }
            Message::Annotation(message) => {
                let Some(outcome) = self
                    .annotation
                    .update(model, settings.state_mut(), message)?
                else {
                    return Ok(None);
                };
                Outcome::Annotation(outcome)
            }
        };
        Ok(Some(outcome))
    }

    pub fn navigation(
        &self,
        connection: &'static str,
        typography: crate::view_model::Typography,
    ) -> Element<'static, Message> {
        navigation::view(self.active, connection, typography).map(Message::Navigation)
    }

    pub fn view<'a>(
        &'a self,
        model: &'a ApplicationModel,
        settings: &'a crate::view::settings::Component,
        surface: Option<Surface>,
        width: f32,
    ) -> Element<'a, Message> {
        match self.active {
            FeatureId::Train => self
                .train
                .view(model, settings.state(), surface, width)
                .map(Message::Train),
            FeatureId::Validate => self
                .validate
                .view(model, settings.state(), surface, width)
                .map(Message::Validate),
            FeatureId::Predict => self
                .predict
                .view(model, settings.state(), surface, width)
                .map(Message::Predict),
            FeatureId::Live => self
                .live
                .view(model, settings.state(), surface, width)
                .map(Message::Live),
            FeatureId::Export => self
                .export
                .view(model, settings.state(), surface, width)
                .map(Message::Export),
            FeatureId::Explore => self
                .explore
                .view(model, settings.state(), surface, width)
                .map(Message::Explore),
            FeatureId::Annotate => self
                .annotation
                .view(model, settings.state(), surface, width)
                .map(Message::Annotation),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn persistent_router_accepts_every_generated_feature() {
        let mut router = Router::default();
        let mut model = ApplicationModel::default();
        let mut settings = crate::view::settings::Component::default();
        for feature in crate::generated::FEATURE_ID_VALUES {
            let previous = router.active();
            let outcome = router
                .update(
                    &mut model,
                    &mut settings,
                    Message::Navigation(navigation::Message::PageSelected(*feature)),
                )
                .unwrap();
            assert!(
                matches!(outcome, Some(Outcome::FeatureSelected(selected)) if selected == *feature)
            );
            assert_eq!(router.active(), previous);
            router.select(*feature);
            assert_eq!(router.active(), *feature);
            drop(router.view(
                &model,
                &crate::view::settings::Component::default(),
                None,
                1280.0,
            ));
        }
    }
}
