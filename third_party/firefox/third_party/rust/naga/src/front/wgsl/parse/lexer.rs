use super::{number::consume_number, Error, ExpectedToken, Result};
use crate::front::wgsl::error::NumberError;
use crate::front::wgsl::parse::directive::enable_extension::{
    EnableExtensions, ImplementedEnableExtension,
};
use crate::front::wgsl::parse::Number;
use crate::Span;

use alloc::{boxed::Box, vec::Vec};

pub type TokenSpan<'a> = (Token<'a>, Span);

#[derive(Copy, Clone, Debug, PartialEq)]
pub enum Token<'a> {
    /// A separator character: `:;,`, and `.` when not part of a numeric
    /// literal.
    Separator(char),

    /// A parenthesis-like character: `()[]{}`, and also `<>`.
    ///
    /// Note that `<>` representing template argument brackets are distinguished
    /// using WGSL's [template list discovery algorithm][tlda], and are returned
    /// as [`Token::TemplateArgsStart`] and [`Token::TemplateArgsEnd`]. That is,
    /// we use `Paren` for `<>` when they are *not* parens.
    ///
    /// [tlda]: https://gpuweb.github.io/gpuweb/wgsl/#template-list-discovery
    Paren(char),

    /// The attribute introduction character `@`.
    Attribute,

    /// A numeric literal, either integral or floating-point, including any
    /// type suffix.
    Number(core::result::Result<Number, NumberError>),

    /// An identifier, possibly a reserved word.
    Word(&'a str),

    /// A miscellaneous single-character operator, like an arithmetic unary or
    /// binary operator. This includes `=`, for assignment and initialization.
    Operation(char),

    /// Certain multi-character logical operators: `!=`, `==`, `&&`,
    /// `||`, `<=` and `>=`. The value gives the operator's first
    /// character.
    ///
    /// For `<` and `>` operators, see [`Token::Paren`].
    LogicalOperation(char),

    /// A shift operator: `>>` or `<<`.
    ShiftOperation(char),

    /// A compound assignment operator like `+=`.
    ///
    /// When the given character is `<` or `>`, those represent the left shift
    /// and right shift assignment operators, `<<=` and `>>=`.
    AssignmentOperation(char),

    /// The `++` operator.
    IncrementOperation,

    /// The `--` operator.
    DecrementOperation,

    /// The `->` token.
    Arrow,

    /// A `<` representing the start of a template argument list, according to
    /// WGSL's [template list discovery algorithm][tlda].
    ///
    /// [tlda]: https://gpuweb.github.io/gpuweb/wgsl/#template-list-discovery
    TemplateArgsStart,

    /// A `>` representing the end of a template argument list, according to
    /// WGSL's [template list discovery algorithm][tlda].
    ///
    /// [tlda]: https://gpuweb.github.io/gpuweb/wgsl/#template-list-discovery
    TemplateArgsEnd,

    /// A character that does not represent a legal WGSL token.
    Unknown(char),

    /// Comment or whitespace.
    Trivia,

    /// A doc comment, beginning with `///` or `/**`.
    DocComment(&'a str),

    /// A module-level doc comment, beginning with `//!` or `/*!`.
    ModuleDocComment(&'a str),

    /// A block comment that is incomplete, and has not been closed with */.
    ///
    /// It's expected that the parser will consider this to be an error.
    UnterminatedBlockComment(&'a str),

    /// The end of the input.
    End,
}

fn consume_any(input: &str, what: impl Fn(char) -> bool) -> (&str, &str) {
    let pos = input.find(|c| !what(c)).unwrap_or(input.len());
    input.split_at(pos)
}

struct UnclosedCandidate {
    index: usize,
    depth: usize,
}

/// Produce at least one token, distinguishing [template lists] from other uses
/// of `<` and `>`.
///
/// Consume one or more tokens from `input` and store them in `tokens`, updating
/// `input` to refer to the remaining text. Apply WGSL's [template list
/// discovery algorithm] to decide what sort of tokens `<` and `>` characters in
/// the input actually represent.
///
/// Store the tokens in `tokens` in the *reverse* of the order they appear in
/// the text, such that the caller can pop from the end of the vector to see the
/// tokens in textual order.
///
/// The `tokens` vector must be empty on entry. The idea is for the caller to
/// use it as a buffer of unconsumed tokens, and call this function to refill it
/// when it's empty.
///
/// The `source` argument must be the whole original source code, used to
/// compute spans.
///
/// If `ignore_doc_comments` is true, then doc comments are returned as
/// [`Token::Trivia`], like ordinary comments.
///
/// [template lists]: https://gpuweb.github.io/gpuweb/wgsl/#template-lists-sec
/// [template list discovery algorithm]: https://gpuweb.github.io/gpuweb/wgsl/#template-list-discovery
fn discover_template_lists<'a>(
    tokens: &mut Vec<(TokenSpan<'a>, &'a str)>,
    source: &'a str,
    mut input: &'a str,
    ignore_doc_comments: bool,
) {
    assert!(tokens.is_empty());

    let mut looking_for_template_start = false;
    let mut pending: Vec<UnclosedCandidate> = Vec::new();

    let mut depth = 0;

    fn pop_until(pending: &mut Vec<UnclosedCandidate>, depth: usize) {
        while pending
            .last()
            .map(|candidate| candidate.depth >= depth)
            .unwrap_or(false)
        {
            pending.pop();
        }
    }

    loop {
        let waiting_for_template_end = pending
            .last()
            .is_some_and(|candidate| candidate.depth == depth);

        let (token, rest) = consume_token(input, waiting_for_template_end, ignore_doc_comments);
        let span = Span::from(source.len() - input.len()..source.len() - rest.len());
        tokens.push(((token, span), rest));
        input = rest;

        match token {
            Token::Word(_) => {
                looking_for_template_start = true;
                continue;
            }
            Token::Trivia | Token::DocComment(_) | Token::ModuleDocComment(_)
                if looking_for_template_start =>
            {
                continue;
            }
            Token::Paren('<') if looking_for_template_start => {
                pending.push(UnclosedCandidate {
                    index: tokens.len() - 1,
                    depth,
                });
            }
            Token::TemplateArgsEnd => {
                let candidate = pending.pop().unwrap();
                let &mut ((ref mut token, _), _) = tokens.get_mut(candidate.index).unwrap();
                *token = Token::TemplateArgsStart;
            }
            Token::Paren('(' | '[') => {
                depth += 1;
            }
            Token::Paren(')' | ']') => {
                pop_until(&mut pending, depth);
                depth = depth.saturating_sub(1);
            }
            Token::Operation('=') | Token::Separator(':' | ';') | Token::Paren('{') => {
                pending.clear();
                depth = 0;
            }
            Token::LogicalOperation('&') | Token::LogicalOperation('|') => {
                pop_until(&mut pending, depth);
            }
            Token::End => break,
            _ => {}
        }

        looking_for_template_start = false;

        if pending.is_empty() {
            break;
        }
    }

    tokens.reverse();
}

/// Return the token at the start of `input`.
///
/// The `waiting_for_template_end` flag enables some special handling to help out
/// `discover_template_lists`:
///
/// - If `waiting_for_template_end` is `true`, then return text starting with
///   '>` as [`Token::TemplateArgsEnd`] and consume only the `>` character,
///   regardless of what characters follow it. This is required by the [template
///   list discovery algorithm][tlda] when the `>` would end a template argument list.
///
/// - If `waiting_for_template_end` is false, recognize multi-character tokens
///   beginning with `>` as usual.
///
/// If `ignore_doc_comments` is true, then doc comments are returned as
/// [`Token::Trivia`], like ordinary comments.
///
/// [tlda]: https://gpuweb.github.io/gpuweb/wgsl/#template-list-discovery
fn consume_token(
    input: &str,
    waiting_for_template_end: bool,
    ignore_doc_comments: bool,
) -> (Token<'_>, &str) {
    let mut chars = input.chars();
    let cur = match chars.next() {
        Some(c) => c,
        None => return (Token::End, ""),
    };
    match cur {
        ':' | ';' | ',' => (Token::Separator(cur), chars.as_str()),
        '.' => {
            let og_chars = chars.as_str();
            match chars.next() {
                Some('0'..='9') => consume_number(input),
                _ => (Token::Separator(cur), og_chars),
            }
        }
        '@' => (Token::Attribute, chars.as_str()),
        '(' | ')' | '{' | '}' | '[' | ']' => (Token::Paren(cur), chars.as_str()),
        '<' | '>' => {
            let og_chars = chars.as_str();
            if cur == '>' && waiting_for_template_end {
                return (Token::TemplateArgsEnd, og_chars);
            }
            match chars.next() {
                Some('=') => (Token::LogicalOperation(cur), chars.as_str()),
                Some(c) if c == cur => {
                    let og_chars = chars.as_str();
                    match chars.next() {
                        Some('=') => (Token::AssignmentOperation(cur), chars.as_str()),
                        _ => (Token::ShiftOperation(cur), og_chars),
                    }
                }
                _ => (Token::Paren(cur), og_chars),
            }
        }
        '0'..='9' => consume_number(input),
        '/' => {
            let og_chars = chars.as_str();
            match chars.next() {
                Some('/') => {
                    let mut input_chars = input.char_indices();
                    let doc_comment_end = input_chars
                        .find_map(|(index, c)| is_comment_end(c).then_some(index))
                        .unwrap_or(input.len());
                    let token = match chars.next() {
                        Some('/') if !ignore_doc_comments => {
                            Token::DocComment(&input[..doc_comment_end])
                        }
                        Some('!') if !ignore_doc_comments => {
                            Token::ModuleDocComment(&input[..doc_comment_end])
                        }
                        _ => Token::Trivia,
                    };
                    (token, input_chars.as_str())
                }
                Some('*') => {
                    let next_c = chars.next();

                    enum CommentType {
                        Doc,
                        ModuleDoc,
                        Normal,
                    }
                    let comment_type = match next_c {
                        Some('*') if !ignore_doc_comments => CommentType::Doc,
                        Some('!') if !ignore_doc_comments => CommentType::ModuleDoc,
                        _ => CommentType::Normal,
                    };

                    let mut depth = 1;
                    let mut prev = next_c;

                    for c in &mut chars {
                        match (prev, c) {
                            (Some('*'), '/') => {
                                prev = None;
                                depth -= 1;
                                if depth == 0 {
                                    let rest = chars.as_str();
                                    let token = match comment_type {
                                        CommentType::Doc => {
                                            let doc_comment_end = input.len() - rest.len();
                                            Token::DocComment(&input[..doc_comment_end])
                                        }
                                        CommentType::ModuleDoc => {
                                            let doc_comment_end = input.len() - rest.len();
                                            Token::ModuleDocComment(&input[..doc_comment_end])
                                        }
                                        CommentType::Normal => Token::Trivia,
                                    };
                                    return (token, rest);
                                }
                            }
                            (Some('/'), '*') => {
                                prev = None;
                                depth += 1;
                            }
                            _ => {
                                prev = Some(c);
                            }
                        }
                    }

                    (Token::UnterminatedBlockComment(input), "")
                }
                Some('=') => (Token::AssignmentOperation(cur), chars.as_str()),
                _ => (Token::Operation(cur), og_chars),
            }
        }
        '-' => {
            let og_chars = chars.as_str();
            match chars.next() {
                Some('>') => (Token::Arrow, chars.as_str()),
                Some('-') => (Token::DecrementOperation, chars.as_str()),
                Some('=') => (Token::AssignmentOperation(cur), chars.as_str()),
                _ => (Token::Operation(cur), og_chars),
            }
        }
        '+' => {
            let og_chars = chars.as_str();
            match chars.next() {
                Some('+') => (Token::IncrementOperation, chars.as_str()),
                Some('=') => (Token::AssignmentOperation(cur), chars.as_str()),
                _ => (Token::Operation(cur), og_chars),
            }
        }
        '*' | '%' | '^' => {
            let og_chars = chars.as_str();
            match chars.next() {
                Some('=') => (Token::AssignmentOperation(cur), chars.as_str()),
                _ => (Token::Operation(cur), og_chars),
            }
        }
        '~' => (Token::Operation(cur), chars.as_str()),
        '=' | '!' => {
            let og_chars = chars.as_str();
            match chars.next() {
                Some('=') => (Token::LogicalOperation(cur), chars.as_str()),
                _ => (Token::Operation(cur), og_chars),
            }
        }
        '&' | '|' => {
            let og_chars = chars.as_str();
            match chars.next() {
                Some(c) if c == cur => (Token::LogicalOperation(cur), chars.as_str()),
                Some('=') => (Token::AssignmentOperation(cur), chars.as_str()),
                _ => (Token::Operation(cur), og_chars),
            }
        }
        _ if is_blankspace(cur) => {
            let (_, rest) = consume_any(input, is_blankspace);
            (Token::Trivia, rest)
        }
        _ if is_word_start(cur) => {
            let (word, rest) = consume_any(input, is_word_part);
            (Token::Word(word), rest)
        }
        _ => (Token::Unknown(cur), chars.as_str()),
    }
}

/// Returns whether or not a char is a comment end
/// (Unicode Pattern_White_Space excluding U+0020, U+0009, U+200E and U+200F)
/// <https://www.w3.org/TR/WGSL/#line-break>
const fn is_comment_end(c: char) -> bool {
    match c {
        '\u{000a}'..='\u{000d}' | '\u{0085}' | '\u{2028}' | '\u{2029}' => true,
        _ => false,
    }
}

/// Returns whether or not a char is a blankspace (Unicode Pattern_White_Space)
const fn is_blankspace(c: char) -> bool {
    match c {
        '\u{0020}'
        | '\u{0009}'..='\u{000d}'
        | '\u{0085}'
        | '\u{200e}'
        | '\u{200f}'
        | '\u{2028}'
        | '\u{2029}' => true,
        _ => false,
    }
}

/// Returns whether or not a char is a word start (Unicode XID_Start + '_')
fn is_word_start(c: char) -> bool {
    c == '_' || unicode_ident::is_xid_start(c)
}

/// Returns whether or not a char is a word part (Unicode XID_Continue)
fn is_word_part(c: char) -> bool {
    unicode_ident::is_xid_continue(c)
}

pub(in crate::front::wgsl) struct Lexer<'a> {
    /// The remaining unconsumed input.
    input: &'a str,

    /// The full original source code.
    ///
    /// We compare `input` against this to compute the lexer's current offset in
    /// the source.
    pub(in crate::front::wgsl) source: &'a str,

    /// The byte offset of the end of the most recently returned non-trivia
    /// token.
    ///
    /// This is consulted by the `span_from` function, for finding the
    /// end of the span for larger structures like expressions or
    /// statements.
    last_end_offset: usize,

    /// A stack of unconsumed tokens to which template list discovery has been
    /// applied.
    ///
    /// This is a stack: the next token is at the *end* of the vector, not the
    /// start. So tokens appear here in the reverse of the order they appear in
    /// the source.
    ///
    /// This doesn't contain the whole source, only those tokens produced by
    /// [`discover_template_lists`]'s look-ahead, or that have been produced by
    /// other look-ahead functions like `peek` and `next_if`. When this is empty,
    /// we call [`discover_template_lists`] to get more.
    tokens: Vec<(TokenSpan<'a>, &'a str)>,

    /// Whether or not to ignore doc comments.
    /// If `true`, doc comments are treated as [`Token::Trivia`].
    ignore_doc_comments: bool,

    /// The set of [enable-extensions] present in the module, determined in a pre-pass.
    ///
    /// [enable-extensions]: https://gpuweb.github.io/gpuweb/wgsl/#enable-extensions-sec
    pub(in crate::front::wgsl) enable_extensions: EnableExtensions,
}

impl<'a> Lexer<'a> {
    pub(in crate::front::wgsl) const fn new(input: &'a str, ignore_doc_comments: bool) -> Self {
        Lexer {
            input,
            source: input,
            last_end_offset: 0,
            tokens: Vec::new(),
            enable_extensions: EnableExtensions::empty(),
            ignore_doc_comments,
        }
    }

    /// Check that `extension` is enabled in `self`.
    pub(in crate::front::wgsl) fn require_enable_extension(
        &self,
        extension: ImplementedEnableExtension,
        span: Span,
    ) -> Result<'static, ()> {
        self.enable_extensions.require(extension, span)
    }

    /// Calls the function with a lexer and returns the result of the function as well as the span for everything the function parsed
    ///
    /// # Examples
    /// ```ignore
    /// let lexer = Lexer::new("5");
    /// let (value, span) = lexer.capture_span(Lexer::next_uint_literal);
    /// assert_eq!(value, 5);
    /// ```
    #[inline]
    pub fn capture_span<T, E>(
        &mut self,
        inner: impl FnOnce(&mut Self) -> core::result::Result<T, E>,
    ) -> core::result::Result<(T, Span), E> {
        let start = self.current_byte_offset();
        let res = inner(self)?;
        let end = self.current_byte_offset();
        Ok((res, Span::from(start..end)))
    }

    pub(in crate::front::wgsl) fn start_byte_offset(&mut self) -> usize {
        loop {
            let (token, rest) = consume_token(self.input, false, true);
            if let Token::Trivia = token {
                self.input = rest;
            } else {
                return self.current_byte_offset();
            }
        }
    }

    /// Collect all module doc comments until a non doc token is found.
    pub(in crate::front::wgsl) fn accumulate_module_doc_comments(&mut self) -> Vec<&'a str> {
        let mut doc_comments = Vec::new();
        loop {
            self.input = consume_any(self.input, is_blankspace).1;

            let (token, rest) = consume_token(self.input, false, self.ignore_doc_comments);
            if let Token::ModuleDocComment(doc_comment) = token {
                self.input = rest;
                doc_comments.push(doc_comment);
            } else {
                return doc_comments;
            }
        }
    }

    /// Collect all doc comments until a non doc token is found.
    pub(in crate::front::wgsl) fn accumulate_doc_comments(&mut self) -> Vec<&'a str> {
        let mut doc_comments = Vec::new();
        loop {
            self.input = consume_any(self.input, is_blankspace).1;

            let (token, rest) = consume_token(self.input, false, self.ignore_doc_comments);
            if let Token::DocComment(doc_comment) = token {
                self.input = rest;
                doc_comments.push(doc_comment);
            } else {
                return doc_comments;
            }
        }
    }

    const fn current_byte_offset(&self) -> usize {
        self.source.len() - self.input.len()
    }

    pub(in crate::front::wgsl) fn span_from(&self, offset: usize) -> Span {
        Span::from(offset..self.last_end_offset)
    }
    pub(in crate::front::wgsl) fn span_with_start(&self, span: Span) -> Span {
        span.until(&Span::from(0..self.last_end_offset))
    }

    /// Return the next non-whitespace token from `self`.
    ///
    /// Assume we are a parse state where bit shift operators may
    /// occur, but not angle brackets.
    #[must_use]
    pub(in crate::front::wgsl) fn next(&mut self) -> TokenSpan<'a> {
        self.next_impl(true)
    }


    /// Return the next non-whitespace token from `self`, with a span.
    fn next_impl(&mut self, ignore_doc_comments: bool) -> TokenSpan<'a> {
        loop {
            if self.tokens.is_empty() {
                discover_template_lists(
                    &mut self.tokens,
                    self.source,
                    self.input,
                    ignore_doc_comments || self.ignore_doc_comments,
                );
            }
            assert!(!self.tokens.is_empty());
            let (token, rest) = self.tokens.pop().unwrap();

            self.input = rest;
            self.last_end_offset = self.current_byte_offset();

            match token.0 {
                Token::Trivia => {}
                _ => return token,
            }
        }
    }

    #[must_use]
    pub(in crate::front::wgsl) fn peek(&mut self) -> TokenSpan<'a> {
        let input = self.input;
        let last_end_offset = self.last_end_offset;
        let token = self.next();
        self.tokens.push((token, self.input));
        self.input = input;
        self.last_end_offset = last_end_offset;
        token
    }

    /// If the next token matches it's consumed and true is returned
    pub(in crate::front::wgsl) fn next_if(&mut self, what: Token<'_>) -> bool {
        let input = self.input;
        let last_end_offset = self.last_end_offset;
        let token = self.next();
        if token.0 == what {
            true
        } else {
            self.tokens.push((token, self.input));
            self.input = input;
            self.last_end_offset = last_end_offset;
            false
        }
    }

    pub(in crate::front::wgsl) fn expect_span(&mut self, expected: Token<'a>) -> Result<'a, Span> {
        let next = self.next();
        if next.0 == expected {
            Ok(next.1)
        } else {
            Err(Box::new(Error::Unexpected(
                next.1,
                ExpectedToken::Token(expected),
            )))
        }
    }

    pub(in crate::front::wgsl) fn expect(&mut self, expected: Token<'a>) -> Result<'a, ()> {
        self.expect_span(expected)?;
        Ok(())
    }

    pub(in crate::front::wgsl) fn next_ident_with_span(&mut self) -> Result<'a, (&'a str, Span)> {
        match self.next() {
            (Token::Word("_"), span) => Err(Box::new(Error::InvalidIdentifierUnderscore(span))),
            (Token::Word(word), span) => {
                if word.starts_with("__") {
                    Err(Box::new(Error::ReservedIdentifierPrefix(span)))
                } else {
                    Ok((word, span))
                }
            }
            (_, span) => Err(Box::new(Error::Unexpected(span, ExpectedToken::Identifier))),
        }
    }

    pub(in crate::front::wgsl) fn next_ident(&mut self) -> Result<'a, super::ast::Ident<'a>> {
        self.next_ident_with_span()
            .and_then(|(word, span)| Self::word_as_ident(word, span))
            .map(|(name, span)| super::ast::Ident { name, span })
    }

    fn word_as_ident(word: &'a str, span: Span) -> Result<'a, (&'a str, Span)> {
        if crate::keywords::wgsl::RESERVED.contains(&word) {
            Err(Box::new(Error::ReservedKeyword(span)))
        } else {
            Ok((word, span))
        }
    }

    pub(in crate::front::wgsl) fn open_arguments(&mut self) -> Result<'a, ()> {
        self.expect(Token::Paren('('))
    }

    pub(in crate::front::wgsl) fn next_argument(&mut self) -> Result<'a, bool> {
        let paren = Token::Paren(')');
        if self.next_if(Token::Separator(',')) {
            Ok(!self.next_if(paren))
        } else {
            self.expect(paren).map(|()| false)
        }
    }
}
