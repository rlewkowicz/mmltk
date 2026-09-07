use crate::application_codec::Value;

#[derive(Debug, Clone, PartialEq)]
pub struct IntentField {
    pub field_id: u64,
    pub value: Value,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Intent {
    pub correlation: u64,
    pub endpoint_id: u64,
    pub fields: Vec<IntentField>,
}

#[derive(Debug, Clone, PartialEq)]
pub struct Interaction {
    pub endpoint_id: u64,
    pub value: Value,
}
