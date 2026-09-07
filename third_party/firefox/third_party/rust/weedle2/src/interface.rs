use crate::argument::ArgumentList;
use crate::attribute::ExtendedAttributeList;
use crate::common::{Docstring, Generics, Identifier, Parenthesized};
use crate::literal::ConstValue;
use crate::types::{AttributedType, ConstType, ReturnType};

/// Parses interface members
pub type InterfaceMembers<'a> = Vec<InterfaceMember<'a>>;

ast_types! {
    /// Parses inheritance clause `: identifier`
    #[derive(Copy)]
    struct Inheritance<'a> {
        colon: term!(:),
        identifier: Identifier<'a>,
    }

    /// Parses one of the interface member variants
    enum InterfaceMember<'a> {
        /// Parses a const interface member `[attributes]? const type identifier = value;`
        Const(struct ConstMember<'a> {
            attributes: Option<ExtendedAttributeList<'a>>,
            const_: term!(const),
            const_type: ConstType<'a>,
            identifier: Identifier<'a>,
            assign: term!(=),
            const_value: ConstValue<'a>,
            semi_colon: term!(;),
        }),
        /// Parses `[attributes]? (stringifier|inherit|static)? readonly? attribute attributedtype identifier;`
        Attribute(struct AttributeInterfaceMember<'a> {
            attributes: Option<ExtendedAttributeList<'a>>,
            modifier: Option<StringifierOrInheritOrStatic>,
            readonly: Option<term!(readonly)>,
            attribute: term!(attribute),
            type_: AttributedType<'a>,
            identifier: Identifier<'a>,
            semi_colon: term!(;),
        }),
        /// Parses `[attributes]? constructor(( args ));`
        ///
        /// (( )) means ( ) chars
        Constructor(struct ConstructorInterfaceMember<'a> {
            docstring: Option<Docstring>,
            attributes: Option<ExtendedAttributeList<'a>>,
            constructor: term!(constructor),
            args: Parenthesized<ArgumentList<'a>>,
            semi_colon: term!(;),
        }),
        /// Parses `[attributes]? (stringifier|static)? special? returntype identifier? (( args ));`
        ///
        /// (( )) means ( ) chars
        Operation(struct OperationInterfaceMember<'a> {
            docstring: Option<Docstring>,
            attributes: Option<ExtendedAttributeList<'a>>,
            modifier: Option<StringifierOrStatic>,
            special: Option<Special>,
            return_type: ReturnType<'a>,
            identifier: Option<Identifier<'a>>,
            args: Parenthesized<ArgumentList<'a>>,
            semi_colon: term!(;),
        }),
        /// Parses an iterable declaration `[attributes]? (iterable<attributedtype> | iterable<attributedtype, attributedtype>) ;`
        Iterable(enum IterableInterfaceMember<'a> {
            /// Parses an iterable declaration `[attributes]? iterable<attributedtype>;`
            Single(struct SingleTypedIterable<'a> {
                attributes: Option<ExtendedAttributeList<'a>>,
                iterable: term!(iterable),
                generics: Generics<AttributedType<'a>>,
                semi_colon: term!(;),
            }),
            /// Parses an iterable declaration `[attributes]? iterable<attributedtype, attributedtype>;`
            Double(struct DoubleTypedIterable<'a> {
                attributes: Option<ExtendedAttributeList<'a>>,
                iterable: term!(iterable),
                generics: Generics<(AttributedType<'a>, term!(,), AttributedType<'a>)>,
                semi_colon: term!(;),
            }),
        }),
        /// Parses an async iterable declaration `[attributes]? async (iterable<attributedtype> | iterable<attributedtype, attributedtype>) (( args ))? ;`
        AsyncIterable(enum AsyncIterableInterfaceMember<'a> {
            /// Parses an async iterable declaration `[attributes]? async iterable<attributedtype> (( args ))? ;`
            Single(struct SingleTypedAsyncIterable<'a> {
                attributes: Option<ExtendedAttributeList<'a>>,
                async_iterable: (term!(async), term!(iterable)),
                generics: Generics<AttributedType<'a>>,
                args: Option<Parenthesized<ArgumentList<'a>>>,
                semi_colon: term!(;),
            }),
            /// Parses an async iterable declaration `[attributes]? async iterable<attributedtype, attributedtype> (( args ))? ;`
            Double(struct DoubleTypedAsyncIterable<'a> {
                attributes: Option<ExtendedAttributeList<'a>>,
                async_iterable: (term!(async), term!(iterable)),
                generics: Generics<(AttributedType<'a>, term!(,), AttributedType<'a>)>,
                args: Option<Parenthesized<ArgumentList<'a>>>,
                semi_colon: term!(;),
            }),
        }),
        /// Parses an maplike declaration `[attributes]? readonly? maplike<attributedtype, attributedtype>;`
        Maplike(struct MaplikeInterfaceMember<'a> {
            attributes: Option<ExtendedAttributeList<'a>>,
            readonly: Option<term!(readonly)>,
            maplike: term!(maplike),
            generics: Generics<(AttributedType<'a>, term!(,), AttributedType<'a>)>,
            semi_colon: term!(;),
        }),
        Setlike(struct SetlikeInterfaceMember<'a> {
            attributes: Option<ExtendedAttributeList<'a>>,
            readonly: Option<term!(readonly)>,
            setlike: term!(setlike),
            generics: Generics<AttributedType<'a>>,
            semi_colon: term!(;),
        }),
        /// Parses `stringifier;`
        #[derive(Default)]
        Stringifier(struct StringifierMember<'a> {
            attributes: Option<ExtendedAttributeList<'a>>,
            stringifier: term!(stringifier),
            semi_colon: term!(;),
        }),
    }

    /// Parses one of the special keyword `getter|setter|deleter`
    #[derive(Copy)]
    enum Special {
        Getter(term!(getter)),
        Setter(term!(setter)),
        Deleter(term!(deleter)),
        LegacyCaller(term!(legacycaller)),
    }

    /// Parses `stringifier|inherit|static`
    #[derive(Copy)]
    enum StringifierOrInheritOrStatic {
        Stringifier(term!(stringifier)),
        Inherit(term!(inherit)),
        Static(term!(static)),
    }

    /// Parses `stringifier|static`
    #[derive(Copy)]
    enum StringifierOrStatic {
        Stringifier(term!(stringifier)),
        Static(term!(static)),
    }
}
