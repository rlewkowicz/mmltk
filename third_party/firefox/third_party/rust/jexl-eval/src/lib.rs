/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

//! A JEXL evaluator written in Rust
//! This crate depends on a JEXL parser crate that handles all the parsing
//! and is a part of the same workspace.
//! JEXL is an expression language used by Mozilla, you can find more information here: https://github.com/mozilla/mozjexl
//!
//! # How to use
//! The access point for this crate is the `eval` functions of the Evaluator Struct
//! You can use the `eval` function directly to evaluate standalone statements
//!
//! For example:
//! ```rust
//! use jexl_eval::Evaluator;
//! use serde_json::json as value;
//! let evaluator = Evaluator::new();
//! assert_eq!(evaluator.eval("'Hello ' + 'World'").unwrap(), value!("Hello World"));
//! ```
//!
//! You can also run the statements against a context using the `eval_in_context` function
//! The context can be any type that implements the `serde::Serializable` trait
//! and the function will return errors if the statement doesn't match the context
//!
//! For example:
//! ```rust
//! use jexl_eval::Evaluator;
//! use serde_json::json as value;
//! let context = value!({"a": {"b": 2.0}});
//! let evaluator = Evaluator::new();
//! assert_eq!(evaluator.eval_in_context("a.b", context).unwrap(), value!(2.0));
//! ```
//!

use jexl_parser::{
    ast::{Expression, OpCode},
    Parser,
};
use serde_json::{json as value, Value};

pub mod error;
use error::*;
use std::collections::HashMap;

const EPSILON: f64 = 0.000001f64;

trait Truthy {
    fn is_truthy(&self) -> bool;
}

impl Truthy for Value {
    fn is_truthy(&self) -> bool {
        match self {
            Value::Bool(b) => *b,
            Value::Null => false,
            Value::Number(f) => f.as_f64().unwrap() != 0.0,
            Value::String(s) => !s.is_empty(),
            Value::Array(_) => true,
            Value::Object(_) => true,
        }
    }
}

impl<'b> Truthy for Result<'b, Value> {
    fn is_truthy(&self) -> bool {
        match self {
            Ok(v) => v.is_truthy(),
            _ => false,
        }
    }
}

type Context = Value;

/// TransformFn represents an arbitrary transform function
/// Transform functions take an arbitrary number of `serde_json::Value`to represent their arguments
/// and return a `serde_json::Value`.
/// the transform function itself is responsible for checking if the format and number of
/// the arguments is correct
///
/// Returns a Result with an `anyhow::Error`. This allows consumers to return their own custom errors
/// in the closure, and use `.into` to convert it into an `anyhow::Error`. The error message will be perserved
pub type TransformFn<'a> = Box<dyn Fn(&[Value]) -> Result<Value, anyhow::Error> + Send + Sync + 'a>;

#[derive(Default)]
pub struct Evaluator<'a> {
    transforms: HashMap<String, TransformFn<'a>>,
}

impl<'a> Evaluator<'a> {
    pub fn new() -> Self {
        Self::default()
    }

    /// Adds a custom transform function
    /// This is meant as a way to allow consumers to add their own custom functionality
    /// to the expression language.
    /// Note that the name added here has to match with
    /// the name that the transform will have when it's a part of the expression statement
    ///
    /// # Arguments:
    /// - `name`: The name of the transfrom
    /// - `transform`: The actual function. A closure the implements Fn(&[serde_json::Value]) -> Result<Value, anyhow::Error>
    ///
    /// # Example:
    ///
    /// ```rust
    /// use jexl_eval::Evaluator;
    /// use serde_json::{json as value, Value};
    ///
    /// let mut evaluator = Evaluator::new().with_transform("lower", |v: &[Value]| {
    ///    let s = v
    ///            .first()
    ///            .expect("Should have 1 argument!")
    ///            .as_str()
    ///            .expect("Should be a string!");
    ///       Ok(value!(s.to_lowercase()))
    ///  });
    ///
    /// assert_eq!(evaluator.eval("'JOHN DOe'|lower").unwrap(), value!("john doe"))
    /// ```
    pub fn with_transform<F>(mut self, name: &str, transform: F) -> Self
    where
        F: Fn(&[Value]) -> Result<Value, anyhow::Error> + Send + Sync + 'a,
    {
        self.transforms
            .insert(name.to_string(), Box::new(transform));
        self
    }

    pub fn eval<'b>(&self, input: &'b str) -> Result<'b, Value> {
        let context = value!({});
        self.eval_in_context(input, &context)
    }

    pub fn eval_in_context<'b, T: serde::Serialize>(
        &self,
        input: &'b str,
        context: T,
    ) -> Result<'b, Value> {
        let tree = Parser::parse(input)?;
        let context = serde_json::to_value(context)?;
        if !context.is_object() {
            return Err(EvaluationError::InvalidContext);
        }
        self.eval_ast(tree, &context)
    }

    fn eval_ast<'b>(&self, ast: Expression, context: &Context) -> Result<'b, Value> {
        match ast {
            Expression::Number(n) => Ok(value!(n)),
            Expression::Boolean(b) => Ok(value!(b)),
            Expression::String(s) => Ok(value!(s)),
            Expression::Array(xs) => xs.into_iter().map(|x| self.eval_ast(*x, context)).collect(),
            Expression::Null => Ok(Value::Null),

            Expression::Object(items) => {
                let mut map = serde_json::Map::with_capacity(items.len());
                for (key, expr) in items.into_iter() {
                    if map.contains_key(&key) {
                        return Err(EvaluationError::DuplicateObjectKey(key));
                    }
                    let value = self.eval_ast(*expr, context)?;
                    map.insert(key, value);
                }
                Ok(Value::Object(map))
            }

            Expression::Identifier(inner) => match context.get(&inner) {
                Some(v) => Ok(v.clone()),
                _ => Err(EvaluationError::UndefinedIdentifier(inner.clone())),
            },

            Expression::DotOperation { subject, ident } => {
                let subject = self.eval_ast(*subject, context)?;
                Ok(subject.get(&ident).unwrap_or(&value!(null)).clone())
            }

            Expression::IndexOperation { subject, index } => {
                let subject = self.eval_ast(*subject, context)?;
                if let Expression::Filter { ident, op, right } = *index {
                    let subject_arr = subject.as_array().ok_or(EvaluationError::InvalidFilter)?;
                    let right = self.eval_ast(*right, context)?;
                    let filtered = subject_arr
                        .iter()
                        .filter(|e| {
                            let left = e.get(&ident).unwrap_or(&value!(null));
                            Self::apply_op(op, left.clone(), right.clone())
                                .unwrap_or(value!(false))
                                .is_truthy()
                        })
                        .collect::<Vec<_>>();
                    return Ok(value!(filtered));
                }

                let index = self.eval_ast(*index, context)?;
                match index {
                    Value::String(inner) => {
                        Ok(subject.get(&inner).unwrap_or(&value!(null)).clone())
                    }
                    Value::Number(inner) => Ok(subject
                        .get(inner.as_f64().unwrap().floor() as usize)
                        .unwrap_or(&value!(null))
                        .clone()),
                    _ => Err(EvaluationError::InvalidIndexType),
                }
            }

            Expression::BinaryOperation {
                left,
                right,
                operation,
            } => self.eval_op(operation, left, right, context),
            Expression::Transform {
                name,
                subject,
                args,
            } => {
                let subject = self.eval_ast(*subject, context)?;
                let mut args_arr = Vec::new();
                args_arr.push(subject);
                if let Some(args) = args {
                    for arg in args {
                        args_arr.push(self.eval_ast(*arg, context)?);
                    }
                }
                let f = self
                    .transforms
                    .get(&name)
                    .ok_or(EvaluationError::UnknownTransform(name))?;
                f(&args_arr).map_err(|e| e.into())
            }

            Expression::Conditional {
                left,
                truthy,
                falsy,
            } => {
                if self.eval_ast(*left, context).is_truthy() {
                    self.eval_ast(*truthy, context)
                } else {
                    self.eval_ast(*falsy, context)
                }
            }

            Expression::Filter {
                ident: _,
                op: _,
                right: _,
            } => {
                return Err(EvaluationError::InvalidFilter);
            }
        }
    }

    fn eval_op<'b>(
        &self,
        operation: OpCode,
        left: Box<Expression>,
        right: Box<Expression>,
        context: &Context,
    ) -> Result<'b, Value> {
        let left = self.eval_ast(*left, context);

        let eval_right = || self.eval_ast(*right, context);
        Ok(match operation {
            OpCode::Or => {
                if left.is_truthy() {
                    left?
                } else {
                    eval_right()?
                }
            }
            OpCode::And => {
                if left.is_truthy() {
                    eval_right()?
                } else {
                    left?
                }
            }
            _ => Self::apply_op(operation, left?, eval_right()?)?,
        })
    }

    fn apply_op<'b>(operation: OpCode, left: Value, right: Value) -> Result<'b, Value> {
        match (operation, left, right) {
            (OpCode::NotEqual, a, b) => {
                let value = Self::apply_op(OpCode::Equal, a, b)?;
                let equality = value
                    .as_bool()
                    .unwrap_or_else(|| unreachable!("Equality always returns a bool"));
                Ok(value!(!equality))
            }

            (OpCode::And, a, b) => Ok(if a.is_truthy() { b } else { a }),
            (OpCode::Or, a, b) => Ok(if a.is_truthy() { a } else { b }),

            (op, Value::Number(a), Value::Number(b)) => {
                let left = a.as_f64().unwrap();
                let right = b.as_f64().unwrap();
                Ok(match op {
                    OpCode::Add => value!(left + right),
                    OpCode::Subtract => value!(left - right),
                    OpCode::Multiply => value!(left * right),
                    OpCode::Divide => value!(left / right),
                    OpCode::FloorDivide => value!((left / right).floor()),
                    OpCode::Modulus => value!(left % right),
                    OpCode::Exponent => value!(left.powf(right)),
                    OpCode::Less => value!(left < right),
                    OpCode::Greater => value!(left > right),
                    OpCode::LessEqual => value!(left <= right),
                    OpCode::GreaterEqual => value!(left >= right),
                    OpCode::Equal => value!((left - right).abs() < EPSILON),
                    OpCode::NotEqual => value!((left - right).abs() >= EPSILON),
                    OpCode::In => value!(false),
                    OpCode::And | OpCode::Or => {
                        unreachable!("Covered by previous case in parent match")
                    }
                })
            }

            (op, Value::String(a), Value::String(b)) => match op {
                OpCode::Equal => Ok(value!(a == b)),

                OpCode::Add => Ok(value!(format!("{}{}", a, b))),
                OpCode::In => Ok(value!(b.contains(&a))),

                OpCode::Less => Ok(value!(a < b)),
                OpCode::Greater => Ok(value!(a > b)),
                OpCode::LessEqual => Ok(value!(a <= b)),
                OpCode::GreaterEqual => Ok(value!(a >= b)),

                _ => Err(EvaluationError::InvalidBinaryOp {
                    operation,
                    left: value!(a),
                    right: value!(b),
                }),
            },

            (OpCode::In, left, Value::Array(v)) => Ok(value!(v.contains(&left))),
            (OpCode::In, Value::String(left), Value::Object(v)) => {
                Ok(value!(v.contains_key(&left)))
            }

            (OpCode::Equal, a, b) => match (a, b) {
                (Value::Bool(a), Value::Bool(b)) => Ok(value!(a == b)),
                (Value::Null, Value::Null) => Ok(value!(true)),
                (Value::Array(a), Value::Array(b)) => Ok(value!(a == b)),
                (Value::Object(a), Value::Object(b)) => Ok(value!(a == b)),
                _ => Ok(value!(false)),
            },

            (operation, left, right) => Err(EvaluationError::InvalidBinaryOp {
                operation,
                left,
                right,
            }),
        }
    }
}
