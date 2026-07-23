// Copyright 2018-2019 Mozilla
// Licensed under the Apache License, Version 2.0 (the "License"); you may not use
// this file except in compliance with the License. You may obtain a copy of the
// License at http://www.apache.org/licenses/LICENSE-2.0
// Unless required by applicable law or agreed to in writing, software distributed
// under the License is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
// CONDITIONS OF ANY KIND, either express or implied. See the License for the
// specific language governing permissions and limitations under the License.

use std::marker::PhantomData;

use crate::{
    backend::{BackendDatabase, BackendRwTransaction},
    error::StoreError,
    readwrite::{Readable, Writer},
    store::{
        keys::{Key, PrimitiveInt},
        single::SingleStore,
    },
    value::Value,
};

type EmptyResult = Result<(), StoreError>;

#[derive(Debug, Eq, PartialEq, Copy, Clone)]
pub struct IntegerStore<D, K> {
    inner: SingleStore<D>,
    phantom: PhantomData<K>,
}

impl<D, K> IntegerStore<D, K>
where
    D: BackendDatabase,
    K: PrimitiveInt,
{
    pub(crate) fn new(db: D) -> IntegerStore<D, K> {
        IntegerStore {
            inner: SingleStore::new(db),
            phantom: PhantomData,
        }
    }

    pub fn get<'r, R>(&self, reader: &'r R, k: K) -> Result<Option<Value<'r>>, StoreError>
    where
        R: Readable<'r, Database = D>,
    {
        self.inner.get(reader, Key::new(&k)?)
    }

    pub fn put<T>(&self, writer: &mut Writer<T>, k: K, v: &Value) -> EmptyResult
    where
        T: BackendRwTransaction<Database = D>,
    {
        self.inner.put(writer, Key::new(&k)?, v)
    }

    pub fn delete<T>(&self, writer: &mut Writer<T>, k: K) -> EmptyResult
    where
        T: BackendRwTransaction<Database = D>,
    {
        self.inner.delete(writer, Key::new(&k)?)
    }

    pub fn clear<T>(&self, writer: &mut Writer<T>) -> EmptyResult
    where
        T: BackendRwTransaction<Database = D>,
    {
        self.inner.clear(writer)
    }
}
