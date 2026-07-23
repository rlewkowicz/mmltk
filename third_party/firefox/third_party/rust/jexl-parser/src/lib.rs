/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

pub mod ast;
pub mod lexer;
#[rustfmt::skip]
mod parser;

pub use crate::lexer::{LexError, Token};
pub use lalrpop_util::ParseError;

pub struct Parser {}

impl Parser {
    pub fn parse(input: &str) -> Result<ast::Expression, ParseError<usize, Token, LexError>> {
        let lexer = lexer::Lexer::new(input);
        Ok(*parser::ExpressionParser::new().parse(lexer)?)
    }
}
