use crate::SmolStr;
use crate::keyboard::key;
use crate::keyboard::{Key, Location, Modifiers};

#[derive(Debug, Clone, PartialEq, Eq)]
pub enum Event {
        KeyPressed {
                key: Key,

                modified_key: Key,

                physical_key: key::Physical,

                location: Location,

                modifiers: Modifiers,

                text: Option<SmolStr>,

                repeat: bool,
    },

        KeyReleased {
                key: Key,

                modified_key: Key,

                physical_key: key::Physical,

                location: Location,

                modifiers: Modifiers,
    },

        ModifiersChanged(Modifiers),
}
