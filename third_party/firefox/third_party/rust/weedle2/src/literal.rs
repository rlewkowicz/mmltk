ast_types! {
    /// Represents an integer value
    #[derive(Copy)]
    enum IntegerLit<'a> {
        /// Parses `-?[1-9][0-9]*`
        #[derive(Copy)]
        Dec(struct DecLit<'a>(
            &'a str = crate::whitespace::ws(nom::combinator::recognize(nom::sequence::tuple((
                nom::combinator::opt(nom::character::complete::char('-')),
                nom::character::complete::one_of("123456789"),
                nom::bytes::complete::take_while(nom::AsChar::is_dec_digit)
            )))),
        )),
        /// Parses `-?0[Xx][0-9A-Fa-f]+)`
        #[derive(Copy)]
        Hex(struct HexLit<'a>(
            &'a str = crate::whitespace::ws(nom::combinator::recognize(nom::sequence::tuple((
                nom::combinator::opt(nom::character::complete::char('-')),
                nom::character::complete::char('0'),
                nom::character::complete::one_of("xX"),
                nom::bytes::complete::take_while(nom::AsChar::is_hex_digit)
            )))),
        )),
        /// Parses `-?0[0-7]*`
        #[derive(Copy)]
        Oct(struct OctLit<'a>(
            &'a str = crate::whitespace::ws(nom::combinator::recognize(nom::sequence::tuple((
                nom::combinator::opt(nom::character::complete::char('-')),
                nom::character::complete::char('0'),
                nom::bytes::complete::take_while(nom::AsChar::is_oct_digit)
            )))),
        )),
    }

    /// Represents a string value
    ///
    /// Follow `/"[^"]*"/`
    #[derive(Copy)]
    struct StringLit<'a>(
        &'a str = crate::whitespace::ws(nom::sequence::delimited(
            nom::character::complete::char('"'),
            nom::bytes::complete::take_while(|c| c != '"'),
            nom::character::complete::char('"'),
        )),
    )

    /// Represents a default literal value. Ex: `34|34.23|"value"|[ ]|true|false|null`
    #[derive(Copy)]
    enum DefaultValue<'a> {
        Boolean(BooleanLit),
        /// Represents `[ ]`
        #[derive(Copy, Default)]
        EmptyArray(struct EmptyArrayLit {
            open_bracket: term!(OpenBracket),
            close_bracket: term!(CloseBracket),
        }),
        /// Represents `{ }`
        #[derive(Copy, Default)]
        EmptyDictionary(struct EmptyDictionaryLit {
            open_brace: term!(OpenBrace),
            close_brace: term!(CloseBrace),
        }),
        Float(FloatLit<'a>),
        Integer(IntegerLit<'a>),
        Null(term!(null)),
        String(StringLit<'a>),
    }

    /// Represents `true`, `false`, `34.23`, `null`, `56`, ...
    #[derive(Copy)]
    enum ConstValue<'a> {
        Boolean(BooleanLit),
        Float(FloatLit<'a>),
        Integer(IntegerLit<'a>),
        Null(term!(null)),
    }

    /// Represents either `true` or `false`
    #[derive(Copy)]
    struct BooleanLit(
        bool = nom::branch::alt((
            nom::combinator::value(true, weedle!(term!(true))),
            nom::combinator::value(false, weedle!(term!(false))),
        )),
    )

    /// Represents a floating point value, `NaN`, `Infinity`, '+Infinity`
    #[derive(Copy)]
    enum FloatLit<'a> {
        /// Parses `/-?(([0-9]+\.[0-9]*|[0-9]*\.[0-9]+)([Ee][+-]?[0-9]+)?|[0-9]+[Ee][+-]?[0-9]+)/`
        #[derive(Copy)]
        Value(struct FloatValueLit<'a>(
            &'a str = crate::whitespace::ws(nom::combinator::recognize(nom::sequence::tuple((
                nom::combinator::opt(nom::character::complete::char('-')),
                nom::branch::alt((
                    nom::combinator::value((), nom::sequence::tuple((
                        nom::branch::alt((
                            nom::sequence::tuple((
                                nom::bytes::complete::take_while1(nom::AsChar::is_dec_digit),
                                nom::character::complete::char('.'),
                                nom::bytes::complete::take_while(nom::AsChar::is_dec_digit),
                            )),
                            nom::sequence::tuple((
                                nom::bytes::complete::take_while(nom::AsChar::is_dec_digit),
                                nom::character::complete::char('.'),
                                nom::bytes::complete::take_while1(nom::AsChar::is_dec_digit),
                            )),
                        )),
                        nom::combinator::opt(nom::sequence::tuple((
                            nom::character::complete::one_of("eE"),
                            nom::combinator::opt(nom::character::complete::one_of("+-")),
                            nom::bytes::complete::take_while1(nom::AsChar::is_dec_digit),
                        ))),
                    ))),
                    nom::combinator::value((), nom::sequence::tuple((
                        nom::bytes::complete::take_while1(nom::AsChar::is_dec_digit),
                        nom::character::complete::one_of("eE"),
                        nom::combinator::opt(nom::character::complete::one_of("+-")),
                        nom::bytes::complete::take_while1(nom::AsChar::is_dec_digit),
                    ))),
                )),
            )))),
        )),
        NegInfinity(term!(-Infinity)),
        Infinity(term!(Infinity)),
        NaN(term!(NaN)),
    }
}
