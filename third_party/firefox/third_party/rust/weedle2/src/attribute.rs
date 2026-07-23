use crate::argument::ArgumentList;
use crate::common::{Bracketed, Identifier, Parenthesized, Punctuated};
use crate::literal::StringLit;

/// Parses a list of attributes. Ex: `[ attribute1, attribute2 ]`
pub type ExtendedAttributeList<'a> = Bracketed<Punctuated<ExtendedAttribute<'a>, term!(,)>>;

/// Matches comma separated identifier list
pub type IdentifierList<'a> = Punctuated<Identifier<'a>, term!(,)>;

ast_types! {
    /// Parses on of the forms of attribute
    enum ExtendedAttribute<'a> {
        /// Parses an argument list. Ex: `Constructor((double x, double y))`
        ///
        /// (( )) means ( ) chars
        ArgList(struct ExtendedAttributeArgList<'a> {
            identifier: Identifier<'a>,
            args: Parenthesized<ArgumentList<'a>>,
        }),
        /// Parses a named argument list. Ex: `NamedConstructor=Image((DOMString src))`
        ///
        /// (( )) means ( ) chars
        NamedArgList(struct ExtendedAttributeNamedArgList<'a> {
            lhs_identifier: Identifier<'a>,
            assign: term!(=),
            rhs_identifier: Identifier<'a>,
            args: Parenthesized<ArgumentList<'a>>,

        }),
        /// Parses an identifier list. Ex: `Exposed=((Window,Worker))`
        ///
        /// (( )) means ( ) chars
        IdentList(struct ExtendedAttributeIdentList<'a> {
            identifier: Identifier<'a>,
            assign: term!(=),
            list: Parenthesized<IdentifierList<'a>>,
        }),
        /// Parses an attribute with an identifier. Ex: `PutForwards=name`
        #[derive(Copy)]
        Ident(struct ExtendedAttributeIdent<'a> {
            lhs_identifier: Identifier<'a>,
            assign: term!(=),
            rhs: IdentifierOrString<'a>,
        }),
        /// Parses a plain attribute. Ex: `Replaceable`
        #[derive(Copy)]
        NoArgs(struct ExtendedAttributeNoArgs<'a>(
            Identifier<'a>,
        )),
    }

    /// Parses `stringifier|static`
    #[derive(Copy)]
    enum IdentifierOrString<'a> {
        Identifier(Identifier<'a>),
        String(StringLit<'a>),
    }
}
