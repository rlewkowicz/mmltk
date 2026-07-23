use crate::literal::DefaultValue;
use crate::{term, IResult, Parse};

pub(crate) fn is_alphanum_underscore_dash(token: char) -> bool {
    nom::AsChar::is_alphanum(token) || matches!(token, '_' | '-')
}

fn marker<S>(i: &str) -> IResult<&str, S>
where
    S: ::std::default::Default,
{
    Ok((i, S::default()))
}

impl<'a, T: Parse<'a>> Parse<'a> for Option<T> {
    parser!(nom::combinator::opt(weedle!(T)));
}

impl<'a, T: Parse<'a>> Parse<'a> for Box<T> {
    parser!(nom::combinator::map(weedle!(T), Box::new));
}

/// Parses `item1 item2 item3...`
impl<'a, T: Parse<'a>> Parse<'a> for Vec<T> {
    parser!(nom::multi::many0(T::parse));
}

impl<'a, T: Parse<'a>, U: Parse<'a>> Parse<'a> for (T, U) {
    parser!(nom::sequence::tuple((T::parse, U::parse)));
}

impl<'a, T: Parse<'a>, U: Parse<'a>, V: Parse<'a>> Parse<'a> for (T, U, V) {
    parser!(nom::sequence::tuple((T::parse, U::parse, V::parse)));
}

pub(crate) fn docstring(input: &str) -> IResult<&str, String> {
    nom::multi::many1(nom::sequence::preceded(
        nom::character::complete::multispace0,
        nom::sequence::delimited(
            nom::bytes::complete::tag("///"),
            nom::bytes::complete::take_until("\n"),
            nom::bytes::complete::tag("\n"),
        ),
    ))(input)
    .map(|io| (io.0, io.1.join("\n")))
}

ast_types! {
    /// Parses `( body )`
    #[derive(Copy, Default)]
    struct Parenthesized<T> where [T: Parse<'a>] {
        open_paren: term::OpenParen,
        body: T,
        close_paren: term::CloseParen,
    }

    /// Parses `[ body ]`
    #[derive(Copy, Default)]
    struct Bracketed<T> where [T: Parse<'a>] {
        open_bracket: term::OpenBracket,
        body: T,
        close_bracket: term::CloseBracket,
    }

    /// Parses `{ body }`
    #[derive(Copy, Default)]
    struct Braced<T> where [T: Parse<'a>] {
        open_brace: term::OpenBrace,
        body: T,
        close_brace: term::CloseBrace,
    }

    /// Parses `< body >`
    #[derive(Copy, Default)]
    struct Generics<T> where [T: Parse<'a>] {
        open_angle: term::LessThan,
        body: T,
        close_angle: term::GreaterThan,
    }

    /// Parses `(item1, item2, item3,...)?`
    struct Punctuated<T, S> where [T: Parse<'a>, S: Parse<'a> + ::std::default::Default] {
        list: Vec<T> = nom::multi::separated_list0(weedle!(S), weedle!(T)),
        separator: S = marker,
    }

    /// Parses `item1, item2, item3, ...`
    struct PunctuatedNonEmpty<T, S> where [T: Parse<'a>, S: Parse<'a> + ::std::default::Default] {
        list: Vec<T> = nom::sequence::terminated(
            nom::multi::separated_list1(weedle!(S), weedle!(T)),
            nom::combinator::opt(weedle!(S))
        ),
        separator: S = marker,
    }

    /// Represents an identifier
    ///
    /// Follows `/_?[A-Za-z][0-9A-Z_a-z-]*/`
    #[derive(Copy)]
    struct Identifier<'a>(
        &'a str = crate::whitespace::ws(nom::sequence::preceded(
            nom::combinator::opt(nom::character::complete::char('_')),
            nom::combinator::recognize(nom::sequence::tuple((
                nom::bytes::complete::take_while1(nom::AsChar::is_alphanum),
                nom::bytes::complete::take_while(is_alphanum_underscore_dash),
            )))
        )),
    )

    /// Parses rhs of an assignment expression. Ex: `= 45`
    #[derive(Copy)]
    struct Default<'a> {
        assign: term!(=),
        value: DefaultValue<'a>,
    }

    /// Represents consecutive comment lines starting with `///`, joined by `\n`.
    struct Docstring(
        String = docstring,
    )
}
