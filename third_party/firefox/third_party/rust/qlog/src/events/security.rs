// Copyright (C) 2021, Cloudflare, Inc.
// All rights reserved.
// Redistribution and use in source and binary forms, with or without
//     * Redistributions of source code must retain the above copyright notice,
//     * Redistributions in binary form must reproduce the above copyright
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS
// IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
// PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR

use serde::Deserialize;
use serde::Serialize;

use super::Bytes;

#[derive(Serialize, Deserialize, Clone, PartialEq, Eq, Debug, Default)]
#[serde(rename_all = "snake_case")]
pub enum KeyType {
    ServerInitialSecret,
    ClientInitialSecret,

    ServerHandshakeSecret,
    ClientHandshakeSecret,

    #[serde(rename = "server_0rtt_secret")]
    Server0RttSecret,
    #[serde(rename = "client_0rtt_secret")]
    Client0RttSecret,
    #[serde(rename = "server_1rtt_secret")]
    Server1RttSecret,
    #[serde(rename = "client_1rtt_secret")]
    Client1RttSecret,

    #[default]
    Unknown,
}

#[derive(Serialize, Deserialize, Clone, PartialEq, Eq, Debug)]
#[serde(rename_all = "snake_case")]
pub enum KeyUpdateOrRetiredTrigger {
    Tls,
    RemoteUpdate,
    LocalUpdate,
}

#[serde_with::skip_serializing_none]
#[derive(Serialize, Deserialize, Clone, PartialEq, Eq, Debug, Default)]
pub struct KeyUpdated {
    pub key_type: KeyType,

    pub old: Option<Bytes>,
    pub new: Bytes,

    pub generation: Option<u32>,

    pub trigger: Option<KeyUpdateOrRetiredTrigger>,
}

#[serde_with::skip_serializing_none]
#[derive(Serialize, Deserialize, Clone, PartialEq, Eq, Debug)]
pub struct KeyDiscarded {
    pub key_type: KeyType,
    pub key: Option<Bytes>,

    pub generation: Option<u32>,

    pub trigger: Option<KeyUpdateOrRetiredTrigger>,
}
