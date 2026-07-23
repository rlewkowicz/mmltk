use crate::attribute::ExtendedAttributeList;
use crate::common::{Default, Docstring, Identifier};
use crate::types::Type;

/// Parses dictionary members
pub type DictionaryMembers<'a> = Vec<DictionaryMember<'a>>;

ast_types! {
    /// Parses dictionary member `[attributes]? required? type identifier ( = default )?;`
    struct DictionaryMember<'a> {
        docstring: Option<Docstring>,
        attributes: Option<ExtendedAttributeList<'a>>,
        required: Option<term!(required)>,
        type_: Type<'a>,
        identifier: Identifier<'a>,
        default: Option<Default<'a>>,
        semi_colon: term!(;),
    }
}
