use crate::attribute::ExtendedAttributeList;
use crate::common::{Generics, Identifier, Parenthesized, Punctuated};
use crate::term;
use crate::Parse;

/// Parses a union of types
pub type UnionType<'a> = Parenthesized<Punctuated<UnionMemberType<'a>, term!(or)>>;

ast_types! {
    /// Parses either single type or a union type
    enum Type<'a> {
        /// Parses one of the single types
        Single(enum SingleType<'a> {
            Any(term!(any)),
            NonAny(NonAnyType<'a>),
        }),
        Union(MayBeNull<UnionType<'a>>),
    }

    enum NonAnyType<'a> {
        Promise(PromiseType<'a>),
        Integer(MayBeNull<IntegerType>),
        FloatingPoint(MayBeNull<FloatingPointType>),
        Boolean(MayBeNull<term!(boolean)>),
        Byte(MayBeNull<term!(byte)>),
        Octet(MayBeNull<term!(octet)>),
        ByteString(MayBeNull<term!(ByteString)>),
        DOMString(MayBeNull<term!(DOMString)>),
        USVString(MayBeNull<term!(USVString)>),
        Sequence(MayBeNull<SequenceType<'a>>),
        Object(MayBeNull<term!(object)>),
        Symbol(MayBeNull<term!(symbol)>),
        Error(MayBeNull<term!(Error)>),
        ArrayBuffer(MayBeNull<term!(ArrayBuffer)>),
        DataView(MayBeNull<term!(DataView)>),
        Int8Array(MayBeNull<term!(Int8Array)>),
        Int16Array(MayBeNull<term!(Int16Array)>),
        Int32Array(MayBeNull<term!(Int32Array)>),
        Uint8Array(MayBeNull<term!(Uint8Array)>),
        Uint16Array(MayBeNull<term!(Uint16Array)>),
        Uint32Array(MayBeNull<term!(Uint32Array)>),
        Uint8ClampedArray(MayBeNull<term!(Uint8ClampedArray)>),
        Float32Array(MayBeNull<term!(Float32Array)>),
        Float64Array(MayBeNull<term!(Float64Array)>),
        ArrayBufferView(MayBeNull<term!(ArrayBufferView)>),
        BufferSource(MayBeNull<term!(BufferSource)>),
        FrozenArrayType(MayBeNull<FrozenArrayType<'a>>),
        RecordType(MayBeNull<RecordType<'a>>),
        Identifier(MayBeNull<Identifier<'a>>),
    }

    /// Parses `sequence<Type>`
    struct SequenceType<'a> {
        sequence: term!(sequence),
        generics: Generics<Box<Type<'a>>>,
    }

    /// Parses `FrozenArray<Type>`
    struct FrozenArrayType<'a> {
        frozen_array: term!(FrozenArray),
        generics: Generics<Box<Type<'a>>>,
    }

    /// Parses a nullable type. Ex: `object | object??`
    ///
    /// `??` means an actual ? not an optional requirement
    #[derive(Copy)]
    struct MayBeNull<T> where [T: Parse<'a>] {
        type_: T,
        q_mark: Option<term::QMark>,
    }

    /// Parses a `Promise<Type|undefined>` type
    struct PromiseType<'a> {
        promise: term!(Promise),
        generics: Generics<Box<ReturnType<'a>>>,
    }

    /// Parses `unsigned? short|long|(long long)`
    #[derive(Copy)]
    enum IntegerType {
        /// Parses `unsigned? long long`
        #[derive(Copy)]
        LongLong(struct LongLongType {
            unsigned: Option<term!(unsigned)>,
            long_long: (term!(long), term!(long)),
        }),
        /// Parses `unsigned? long`
        #[derive(Copy)]
        Long(struct LongType {
            unsigned: Option<term!(unsigned)>,
            long: term!(long),
        }),
        /// Parses `unsigned? short`
        #[derive(Copy)]
        Short(struct ShortType {
            unsigned: Option<term!(unsigned)>,
            short: term!(short),
        }),
    }

    /// Parses `unrestricted? float|double`
    #[derive(Copy)]
    enum FloatingPointType {
        /// Parses `unrestricted? float`
        #[derive(Copy)]
        Float(struct FloatType {
            unrestricted: Option<term!(unrestricted)>,
            float: term!(float),
        }),
        /// Parses `unrestricted? double`
        #[derive(Copy)]
        Double(struct DoubleType {
            unrestricted: Option<term!(unrestricted)>,
            double: term!(double),
        }),
    }

    /// Parses `record<StringType, Type>`
    struct RecordType<'a> {
        record: term!(record),
        generics: Generics<(Box<RecordKeyType<'a>>, term!(,), Box<Type<'a>>)>,
    }

    /// Parses one of the string types `ByteString|DOMString|USVString` or any other type.
    enum RecordKeyType<'a> {
        Byte(term!(ByteString)),
        DOM(term!(DOMString)),
        USV(term!(USVString)),
        NonAny(NonAnyType<'a>),
    }

    /// Parses one of the member of a union type
    enum UnionMemberType<'a> {
        Single(AttributedNonAnyType<'a>),
        Union(MayBeNull<UnionType<'a>>),
    }

    /// Parses a const type
    enum ConstType<'a> {
        Integer(MayBeNull<IntegerType>),
        FloatingPoint(MayBeNull<FloatingPointType>),
        Boolean(MayBeNull<term!(boolean)>),
        Byte(MayBeNull<term!(byte)>),
        Octet(MayBeNull<term!(octet)>),
        Identifier(MayBeNull<Identifier<'a>>),
    }

    /// Parses the return type which may be `undefined` or any given Type
    enum ReturnType<'a> {
        Undefined(term!(undefined)),
        Type(Type<'a>),
    }

    /// Parses `[attributes]? type`
    struct AttributedType<'a> {
        attributes: Option<ExtendedAttributeList<'a>>,
        type_: Type<'a>,
    }

    /// Parses `[attributes]? type` where the type is a single non-any type
    struct AttributedNonAnyType<'a> {
        attributes: Option<ExtendedAttributeList<'a>>,
        type_: NonAnyType<'a>,
    }
}
