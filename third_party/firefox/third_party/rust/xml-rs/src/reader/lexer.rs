//! Contains simple lexer for XML documents.
//!
//! This module is for internal use. Use `xml::pull` module to do parsing.

use std::fmt;
use std::collections::VecDeque;
use std::io::Read;
use std::result;
use std::borrow::Cow;

use common::{Position, TextPosition, is_whitespace_char, is_name_char};
use reader::Error;
use util;

/// `Token` represents a single lexeme of an XML document. These lexemes
/// are used to perform actual parsing.
#[derive(Copy, Clone, PartialEq, Eq, Debug)]
pub enum Token {
    /// `<?`
    ProcessingInstructionStart,
    /// `?>`
    ProcessingInstructionEnd,
    /// `<!DOCTYPE
    DoctypeStart,
    /// `<`
    OpeningTagStart,
    /// `</`
    ClosingTagStart,
    /// `>`
    TagEnd,
    /// `/>`
    EmptyTagEnd,
    /// `<!--`
    CommentStart,
    /// `-->`
    CommentEnd,
    /// A chunk of characters, used for errors recovery.
    Chunk(&'static str),
    /// Any non-special character except whitespace.
    Character(char),
    /// Whitespace character.
    Whitespace(char),
    /// `=`
    EqualsSign,
    /// `'`
    SingleQuote,
    /// `"`
    DoubleQuote,
    /// `<![CDATA[`
    CDataStart,
    /// `]]>`
    CDataEnd,
    /// `&`
    ReferenceStart,
    /// `;`
    ReferenceEnd,
}

impl fmt::Display for Token {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        match *self {
            Token::Chunk(s)                            => write!(f, "{}", s),
            Token::Character(c) | Token::Whitespace(c) => write!(f, "{}", c),
            other => write!(f, "{}", match other {
                Token::OpeningTagStart            => "<",
                Token::ProcessingInstructionStart => "<?",
                Token::DoctypeStart               => "<!DOCTYPE",
                Token::ClosingTagStart            => "</",
                Token::CommentStart               => "<!--",
                Token::CDataStart                 => "<![CDATA[",
                Token::TagEnd                     => ">",
                Token::EmptyTagEnd                => "/>",
                Token::ProcessingInstructionEnd   => "?>",
                Token::CommentEnd                 => "-->",
                Token::CDataEnd                   => "]]>",
                Token::ReferenceStart             => "&",
                Token::ReferenceEnd               => ";",
                Token::EqualsSign                 => "=",
                Token::SingleQuote                => "'",
                Token::DoubleQuote                => "\"",
                _                          => unreachable!()
            })
        }
    }
}

impl Token {
    pub fn as_static_str(&self) -> Option<&'static str> {
        match *self {
            Token::OpeningTagStart            => Some("<"),
            Token::ProcessingInstructionStart => Some("<?"),
            Token::DoctypeStart               => Some("<!DOCTYPE"),
            Token::ClosingTagStart            => Some("</"),
            Token::CommentStart               => Some("<!--"),
            Token::CDataStart                 => Some("<![CDATA["),
            Token::TagEnd                     => Some(">"),
            Token::EmptyTagEnd                => Some("/>"),
            Token::ProcessingInstructionEnd   => Some("?>"),
            Token::CommentEnd                 => Some("-->"),
            Token::CDataEnd                   => Some("]]>"),
            Token::ReferenceStart             => Some("&"),
            Token::ReferenceEnd               => Some(";"),
            Token::EqualsSign                 => Some("="),
            Token::SingleQuote                => Some("'"),
            Token::DoubleQuote                => Some("\""),
            Token::Chunk(s)                   => Some(s),
            _                                 => None
        }
    }

    pub fn push_to_string(&self, target: &mut String) {
        match self.as_static_str() {
            Some(s) => { target.push_str(s); }
            None => {
                match *self {
                    Token::Character(c) | Token::Whitespace(c) => target.push(c),
                    _ => unreachable!()
                }
            }
        }
    }

    /// Returns `true` if this token contains data that can be interpreted
    /// as a part of the text. Surprisingly, this also means '>' and '=' and '"' and "'" and '-->'.
    #[inline]
    pub fn contains_char_data(&self) -> bool {
        match *self {
            Token::Whitespace(_) | Token::Chunk(_) | Token::Character(_) | Token::CommentEnd |
            Token::TagEnd | Token::EqualsSign | Token::DoubleQuote | Token::SingleQuote | Token::CDataEnd | 
            Token::ProcessingInstructionEnd | Token::EmptyTagEnd => true,
            _ => false
        }
    }

    /// Returns `true` if this token corresponds to a white space character.
    #[inline]
    pub fn is_whitespace(&self) -> bool {
        match *self {
            Token::Whitespace(_) => true,
            _ => false
        }
    }
}

enum State {
    /// Triggered on '<'
    TagStarted,
    /// Triggered on '<!'
    CommentOrCDataOrDoctypeStarted,
    /// Triggered on '<!-'
    CommentStarted,
    /// Triggered on '<!D' up to '<!DOCTYPE'
    DoctypeStarted(DoctypeStartedSubstate),
    /// Triggered after DoctypeStarted to handle sub elements
    DoctypeFinishing(u8),
    /// Triggered on '<![' up to '<![CDATA'
    CDataStarted(CDataStartedSubstate),
    /// Triggered on '?'
    ProcessingInstructionClosing,
    /// Triggered on '/'
    EmptyTagClosing,
    /// Triggered on '-' up to '--'
    CommentClosing(ClosingSubstate),
    /// Triggered on ']' up to ']]'
    CDataClosing(ClosingSubstate),
    /// Default state
    Normal
}

#[derive(Copy, Clone)]
enum ClosingSubstate {
    First, Second
}

#[derive(Copy, Clone)]
enum DoctypeStartedSubstate {
    D, DO, DOC, DOCT, DOCTY, DOCTYP
}

#[derive(Copy, Clone)]
enum CDataStartedSubstate {
    E, C, CD, CDA, CDAT, CDATA
}

/// `Result` represents lexing result. It is either a token or an error message.
pub type Result = result::Result<Option<Token>, Error>;

/// Helps to set up a dispatch table for lexing large unambigous tokens like
/// `<![CDATA[` or `<!DOCTYPE `.
macro_rules! dispatch_on_enum_state(
    ($_self:ident, $s:expr, $c:expr, $is:expr,
     $($st:ident; $stc:expr ; $next_st:ident ; $chunk:expr),+;
     $end_st:ident ; $end_c:expr ; $end_chunk:expr ; $e:expr) => (
        match $s {
            $(
            $st => match $c {
                $stc => $_self.move_to($is($next_st)),
                _  => $_self.handle_error($chunk, $c)
            },
            )+
            $end_st => match $c {
                $end_c => $e,
                _      => $_self.handle_error($end_chunk, $c)
            }
        }
    )
);

/// `Lexer` is a lexer for XML documents, which implements pull API.
///
/// Main method is `next_token` which accepts an `std::io::Read` instance and
/// tries to read the next lexeme from it.
///
/// When `skip_errors` flag is set, invalid lexemes will be returned as `Chunk`s.
/// When it is not set, errors will be reported as `Err` objects with a string message.
/// By default this flag is not set. Use `enable_errors` and `disable_errors` methods
/// to toggle the behavior.
pub struct Lexer {
    pos: TextPosition,
    head_pos: TextPosition,
    char_queue: VecDeque<char>,
    st: State,
    skip_errors: bool,
    inside_comment: bool,
    inside_token: bool,
    eof_handled: bool
}

impl Position for Lexer {
    #[inline]
    /// Returns the position of the last token produced by the lexer
    fn position(&self) -> TextPosition { self.pos }
}

impl Lexer {
    /// Returns a new lexer with default state.
    pub fn new() -> Lexer {
        Lexer {
            pos: TextPosition::new(),
            head_pos: TextPosition::new(),
            char_queue: VecDeque::with_capacity(4),  
            st: State::Normal,
            skip_errors: false,
            inside_comment: false,
            inside_token: false,
            eof_handled: false
        }
    }

    /// Enables error handling so `next_token` will return `Some(Err(..))`
    /// upon invalid lexeme.
    #[inline]
    pub fn enable_errors(&mut self) { self.skip_errors = false; }

    /// Disables error handling so `next_token` will return `Some(Chunk(..))`
    /// upon invalid lexeme with this lexeme content.
    #[inline]
    pub fn disable_errors(&mut self) { self.skip_errors = true; }

    /// Enables special handling of some lexemes which should be done when we're parsing comment
    /// internals.
    #[inline]
    pub fn inside_comment(&mut self) { self.inside_comment = true; }

    /// Disables the effect of `inside_comment()` method.
    #[inline]
    pub fn outside_comment(&mut self) { self.inside_comment = false; }

    /// Reset the eof handled flag of the lexer.
    #[inline]
    pub fn reset_eof_handled(&mut self) { self.eof_handled = false; }

    /// Tries to read the next token from the buffer.
    ///
    /// It is possible to pass different instaces of `BufReader` each time
    /// this method is called, but the resulting behavior is undefined in this case.
    ///
    /// Return value:
    /// * `Err(reason) where reason: reader::Error` - when an error occurs;
    /// * `Ok(None)` - upon end of stream is reached;
    /// * `Ok(Some(token)) where token: Token` - in case a complete-token has been read from the stream.
    pub fn next_token<B: Read>(&mut self, b: &mut B) -> Result {
        if self.eof_handled {
            return Ok(None);
        }

        if !self.inside_token {
            self.pos = self.head_pos;
            self.inside_token = true;
        }

        while let Some(c) = self.char_queue.pop_front() {
            match try!(self.read_next_token(c)) {
                Some(t) => {
                    self.inside_token = false;
                    return Ok(Some(t));
                }
                None => {}  
            }
        }

        loop {
            let c = match try!(util::next_char_from(b)) {
                Some(c) => c,   
                None => break,  
            };

            match try!(self.read_next_token(c)) {
                Some(t) => {
                    self.inside_token = false;
                    return Ok(Some(t));
                }
                None => {
                }
            }
        }

        self.eof_handled = true;
        self.pos = self.head_pos;
        match self.st {
            State::TagStarted | State::CommentOrCDataOrDoctypeStarted |
            State::CommentStarted | State::CDataStarted(_)| State::DoctypeStarted(_) |
            State::CommentClosing(ClosingSubstate::Second) |
            State::DoctypeFinishing(_) =>
                Err(self.error("Unexpected end of stream")),
            State::ProcessingInstructionClosing =>
                Ok(Some(Token::Character('?'))),
            State::EmptyTagClosing =>
                Ok(Some(Token::Character('/'))),
            State::CommentClosing(ClosingSubstate::First) =>
                Ok(Some(Token::Character('-'))),
            State::CDataClosing(ClosingSubstate::First) =>
                Ok(Some(Token::Character(']'))),
            State::CDataClosing(ClosingSubstate::Second) =>
                Ok(Some(Token::Chunk("]]"))),
            State::Normal =>
                Ok(None)
        }
    }

    #[inline]
    fn error<M: Into<Cow<'static, str>>>(&self, msg: M) -> Error {
        (self, msg).into()
    }

    #[inline]
    fn read_next_token(&mut self, c: char) -> Result {
        let res = self.dispatch_char(c);
        if self.char_queue.is_empty() {
            if c == '\n' {
                self.head_pos.new_line();
            } else {
                self.head_pos.advance(1);
            }
        }
        res
    }

    fn dispatch_char(&mut self, c: char) -> Result {
        match self.st {
            State::Normal                         => self.normal(c),
            State::TagStarted                     => self.tag_opened(c),
            State::CommentOrCDataOrDoctypeStarted => self.comment_or_cdata_or_doctype_started(c),
            State::CommentStarted                 => self.comment_started(c),
            State::CDataStarted(s)                => self.cdata_started(c, s),
            State::DoctypeStarted(s)              => self.doctype_started(c, s),
            State::DoctypeFinishing(d)            => self.doctype_finishing(c, d),
            State::ProcessingInstructionClosing   => self.processing_instruction_closing(c),
            State::EmptyTagClosing                => self.empty_element_closing(c),
            State::CommentClosing(s)              => self.comment_closing(c, s),
            State::CDataClosing(s)                => self.cdata_closing(c, s)
        }
    }

    #[inline]
    fn move_to(&mut self, st: State) -> Result {
        self.st = st;
        Ok(None)
    }

    #[inline]
    fn move_to_with(&mut self, st: State, token: Token) -> Result {
        self.st = st;
        Ok(Some(token))
    }

    #[inline]
    fn move_to_with_unread(&mut self, st: State, cs: &[char], token: Token) -> Result {
        self.char_queue.extend(cs.iter().cloned());
        self.move_to_with(st, token)
    }

    fn handle_error(&mut self, chunk: &'static str, c: char) -> Result {
        self.char_queue.push_back(c);
        if self.skip_errors || (self.inside_comment && chunk != "--") {  
            self.move_to_with(State::Normal, Token::Chunk(chunk))
        } else {
            Err(self.error(format!("Unexpected token '{}' before '{}'", chunk, c)))
        }
    }

    /// Encountered a char
    fn normal(&mut self, c: char) -> Result {
        match c {
            '<'                        => self.move_to(State::TagStarted),
            '>'                        => Ok(Some(Token::TagEnd)),
            '/'                        => self.move_to(State::EmptyTagClosing),
            '='                        => Ok(Some(Token::EqualsSign)),
            '"'                        => Ok(Some(Token::DoubleQuote)),
            '\''                       => Ok(Some(Token::SingleQuote)),
            '?'                        => self.move_to(State::ProcessingInstructionClosing),
            '-'                        => self.move_to(State::CommentClosing(ClosingSubstate::First)),
            ']'                        => self.move_to(State::CDataClosing(ClosingSubstate::First)),
            '&'                        => Ok(Some(Token::ReferenceStart)),
            ';'                        => Ok(Some(Token::ReferenceEnd)),
            _ if is_whitespace_char(c) => Ok(Some(Token::Whitespace(c))),
            _                          => Ok(Some(Token::Character(c)))
        }
    }

    /// Encountered '<'
    fn tag_opened(&mut self, c: char) -> Result {
        match c {
            '?'                        => self.move_to_with(State::Normal, Token::ProcessingInstructionStart),
            '/'                        => self.move_to_with(State::Normal, Token::ClosingTagStart),
            '!'                        => self.move_to(State::CommentOrCDataOrDoctypeStarted),
            _ if is_whitespace_char(c) => self.move_to_with_unread(State::Normal, &[c], Token::OpeningTagStart),
            _ if is_name_char(c)       => self.move_to_with_unread(State::Normal, &[c], Token::OpeningTagStart),
            _                          => self.handle_error("<", c)
        }
    }

    /// Encountered '<!'
    fn comment_or_cdata_or_doctype_started(&mut self, c: char) -> Result {
        match c {
            '-' => self.move_to(State::CommentStarted),
            '[' => self.move_to(State::CDataStarted(CDataStartedSubstate::E)),
            'D' => self.move_to(State::DoctypeStarted(DoctypeStartedSubstate::D)),
            _   => self.handle_error("<!", c)
        }
    }

    /// Encountered '<!-'
    fn comment_started(&mut self, c: char) -> Result {
        match c {
            '-' => self.move_to_with(State::Normal, Token::CommentStart),
            _   => self.handle_error("<!-", c)
        }
    }

    /// Encountered '<!['
    fn cdata_started(&mut self, c: char, s: CDataStartedSubstate) -> Result {
        use self::CDataStartedSubstate::{E, C, CD, CDA, CDAT, CDATA};
        dispatch_on_enum_state!(self, s, c, State::CDataStarted,
            E     ; 'C' ; C     ; "<![",
            C     ; 'D' ; CD    ; "<![C",
            CD    ; 'A' ; CDA   ; "<![CD",
            CDA   ; 'T' ; CDAT  ; "<![CDA",
            CDAT  ; 'A' ; CDATA ; "<![CDAT";
            CDATA ; '[' ; "<![CDATA" ; self.move_to_with(State::Normal, Token::CDataStart)
        )
    }

    /// Encountered '<!D'
    fn doctype_started(&mut self, c: char, s: DoctypeStartedSubstate) -> Result {
        use self::DoctypeStartedSubstate::{D, DO, DOC, DOCT, DOCTY, DOCTYP};
        dispatch_on_enum_state!(self, s, c, State::DoctypeStarted,
            D      ; 'O' ; DO     ; "<!D",
            DO     ; 'C' ; DOC    ; "<!DO",
            DOC    ; 'T' ; DOCT   ; "<!DOC",
            DOCT   ; 'Y' ; DOCTY  ; "<!DOCT",
            DOCTY  ; 'P' ; DOCTYP ; "<!DOCTY";
            DOCTYP ; 'E' ; "<!DOCTYP" ; self.move_to_with(State::DoctypeFinishing(1), Token::DoctypeStart)
        )
    }

    /// State used while awaiting the closing bracket for the <!DOCTYPE tag
    fn doctype_finishing(&mut self, c: char, d: u8) -> Result {
        match c {
            '<' => self.move_to(State::DoctypeFinishing(d + 1)),
            '>' if d == 1 => self.move_to_with(State::Normal, Token::TagEnd),
            '>' => self.move_to(State::DoctypeFinishing(d - 1)),
            _ => Ok(None),
        }
    }

    /// Encountered '?'
    fn processing_instruction_closing(&mut self, c: char) -> Result {
        match c {
            '>' => self.move_to_with(State::Normal, Token::ProcessingInstructionEnd),
            _   => self.move_to_with_unread(State::Normal, &[c], Token::Character('?')),
        }
    }

    /// Encountered '/'
    fn empty_element_closing(&mut self, c: char) -> Result {
        match c {
            '>' => self.move_to_with(State::Normal, Token::EmptyTagEnd),
            _   => self.move_to_with_unread(State::Normal, &[c], Token::Character('/')),
        }
    }

    /// Encountered '-'
    fn comment_closing(&mut self, c: char, s: ClosingSubstate) -> Result {
        match s {
            ClosingSubstate::First => match c {
                '-' => self.move_to(State::CommentClosing(ClosingSubstate::Second)),
                _   => self.move_to_with_unread(State::Normal, &[c], Token::Character('-'))
            },
            ClosingSubstate::Second => match c {
                '>'                      => self.move_to_with(State::Normal, Token::CommentEnd),
                _ if self.inside_comment => self.handle_error("--", c),
                _                        => self.move_to_with_unread(State::Normal, &[c], Token::Chunk("--"))
            }
        }
    }

    /// Encountered ']'
    fn cdata_closing(&mut self, c: char, s: ClosingSubstate) -> Result {
        match s {
            ClosingSubstate::First => match c {
                ']' => self.move_to(State::CDataClosing(ClosingSubstate::Second)),
                _   => self.move_to_with_unread(State::Normal, &[c], Token::Character(']'))
            },
            ClosingSubstate::Second => match c {
                '>' => self.move_to_with(State::Normal, Token::CDataEnd),
                _   => self.move_to_with_unread(State::Normal, &[']', c], Token::Character(']'))
            }
        }
    }
}
