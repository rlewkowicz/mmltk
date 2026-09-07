use std::borrow::{Borrow, Cow};
use std::collections::btree_map::{BTreeMap, Entry};
use std::mem::ManuallyDrop;
use std::ops::Deref;
use std::path::{Path, PathBuf};
use std::sync::{Arc, OnceLock};
use std::{env, fs};

use parser::node::Whitespace;
use parser::{ParseError, Parsed, Syntax, SyntaxBuilder};
use proc_macro2::Span;
#[cfg(feature = "config")]
use serde_derive::Deserialize;

use crate::{CompileError, FileInfo, OnceMap};

#[derive(Debug)]
pub(crate) struct Config {
    pub(crate) dirs: Vec<PathBuf>,
    pub(crate) syntaxes: BTreeMap<String, SyntaxAndCache<'static>>,
    pub(crate) default_syntax: &'static str,
    pub(crate) escapers: Vec<(Vec<Cow<'static, str>>, Cow<'static, str>)>,
    pub(crate) whitespace: Whitespace,
    pub(crate) full_config_path: Option<PathBuf>,
    _key: OwnedConfigKey,
}

impl Drop for Config {
    #[track_caller]
    fn drop(&mut self) {
        panic!();
    }
}

#[derive(Debug, PartialEq, Eq, Hash, Clone, Copy)]
struct OwnedConfigKey(&'static ConfigKey<'static>);

#[derive(Debug, PartialEq, Eq, Hash)]
struct ConfigKey<'a> {
    root: Cow<'a, Path>,
    source: Cow<'a, str>,
    config_path: Option<Cow<'a, str>>,
    template_whitespace: Option<Whitespace>,
}

impl ToOwned for ConfigKey<'_> {
    type Owned = OwnedConfigKey;

    fn to_owned(&self) -> Self::Owned {
        let owned_key = ConfigKey {
            root: Cow::Owned(self.root.as_ref().to_owned()),
            source: Cow::Owned(self.source.as_ref().to_owned()),
            config_path: self
                .config_path
                .as_ref()
                .map(|s| Cow::Owned(s.as_ref().to_owned())),
            template_whitespace: self.template_whitespace,
        };
        OwnedConfigKey(Box::leak(Box::new(owned_key)))
    }
}

impl<'a> Borrow<ConfigKey<'a>> for OwnedConfigKey {
    #[inline]
    fn borrow(&self) -> &ConfigKey<'a> {
        self.0
    }
}

impl Config {
    pub(crate) fn new(
        source: &str,
        config_path: Option<&str>,
        template_whitespace: Option<Whitespace>,
        config_span: Option<Span>,
        full_config_path: Option<PathBuf>,
    ) -> Result<&'static Config, CompileError> {
        static CACHE: ManuallyDrop<OnceLock<OnceMap<OwnedConfigKey, &'static Config>>> =
            ManuallyDrop::new(OnceLock::new());
        CACHE.get_or_init(OnceMap::default).get_or_try_insert(
            &ConfigKey {
                root: Cow::Owned(manifest_root()),
                source: source.into(),
                config_path: config_path.map(Cow::Borrowed),
                template_whitespace,
            },
            |key| {
                let config = Config::new_uncached(key.to_owned(), config_span, full_config_path)?;
                let config = &*Box::leak(Box::new(config));
                Ok((config._key, config))
            },
            |config| *config,
        )
    }
}

impl Config {
    fn new_uncached(
        key: OwnedConfigKey,
        config_span: Option<Span>,
        full_config_path: Option<PathBuf>,
    ) -> Result<Config, CompileError> {
        let s = key.0.source.as_ref();
        let config_path = key.0.config_path.as_deref();
        let root = key.0.root.as_ref();

        let default_dirs = vec![root.join("templates")];

        let mut syntaxes = BTreeMap::new();
        syntaxes.insert(DEFAULT_SYNTAX_NAME.to_string(), SyntaxAndCache::default());

        let raw = if s.is_empty() {
            RawConfig::default()
        } else {
            RawConfig::from_toml_str(s)?
        };

        let (dirs, default_syntax, whitespace) = match raw.general {
            Some(General {
                dirs,
                default_syntax,
                whitespace,
            }) => (
                dirs.map_or(default_dirs, |v| {
                    v.into_iter().map(|dir| root.join(dir)).collect()
                }),
                default_syntax.unwrap_or(DEFAULT_SYNTAX_NAME),
                whitespace,
            ),
            None => (default_dirs, DEFAULT_SYNTAX_NAME, Whitespace::default()),
        };
        let file_info = config_path.map(|path| FileInfo::new(Path::new(path), None, None));
        let whitespace = key.0.template_whitespace.unwrap_or(whitespace);

        if let Some(raw_syntaxes) = raw.syntax {
            for raw_s in raw_syntaxes {
                let name = raw_s.name;
                match syntaxes.entry(name.to_string()) {
                    Entry::Vacant(entry) => {
                        entry.insert(raw_s.to_syntax().map(SyntaxAndCache::new).map_err(
                            |err| CompileError::new_with_span(err, file_info, config_span),
                        )?);
                    }
                    Entry::Occupied(_) => {
                        return Err(CompileError::new(
                            format_args!("syntax {name:?} is already defined"),
                            file_info,
                        ));
                    }
                }
            }
        }

        if !syntaxes.contains_key(default_syntax) {
            return Err(CompileError::new(
                format_args!("default syntax \"{default_syntax}\" not found"),
                file_info,
            ));
        }

        let mut escapers = Vec::new();
        if let Some(configured) = raw.escaper {
            for escaper in configured {
                escapers.push((str_set(&escaper.extensions), escaper.path.into()));
            }
        }
        for (extensions, name) in DEFAULT_ESCAPERS {
            escapers.push((
                str_set(extensions),
                format!("askama::filters::{name}").into(),
            ));
        }

        Ok(Config {
            dirs,
            syntaxes,
            default_syntax,
            escapers,
            whitespace,
            full_config_path,
            _key: key,
        })
    }

    pub(crate) fn find_template(
        &self,
        path: &str,
        start_at: Option<&Path>,
        file_info: Option<FileInfo<'_>>,
    ) -> Result<Arc<Path>, CompileError> {
        let path = 'find_path: {
            if let Some(root) = start_at {
                let relative = root.with_file_name(path);
                if relative.exists() {
                    break 'find_path relative;
                }
            }
            for dir in &self.dirs {
                let rooted = dir.join(path);
                if rooted.exists() {
                    break 'find_path rooted;
                }
            }
            return Err(CompileError::new(
                format_args!(
                    "template {:?} not found in directories {:?}",
                    path, self.dirs,
                ),
                file_info,
            ));
        };
        match path.canonicalize() {
            Ok(path) => Ok(path.into()),
            Err(err) => Err(CompileError::new(
                format_args!("could not canonicalize path {path:?}: {err}"),
                file_info,
            )),
        }
    }
}

#[derive(Debug, Default)]
pub(crate) struct SyntaxAndCache<'a> {
    syntax: Syntax<'a>,
    cache: OnceMap<OwnedSyntaxAndCacheKey, Arc<Parsed>>,
}

impl<'a> Deref for SyntaxAndCache<'a> {
    type Target = Syntax<'a>;

    fn deref(&self) -> &Self::Target {
        &self.syntax
    }
}

#[derive(Debug, Clone, Hash, PartialEq, Eq)]
struct OwnedSyntaxAndCacheKey(SyntaxAndCacheKey<'static>);

impl Deref for OwnedSyntaxAndCacheKey {
    type Target = SyntaxAndCacheKey<'static>;

    fn deref(&self) -> &Self::Target {
        &self.0
    }
}

#[derive(Debug, Clone, Hash, PartialEq, Eq)]
struct SyntaxAndCacheKey<'a> {
    source: Cow<'a, Arc<str>>,
    source_path: Option<Cow<'a, Arc<Path>>>,
}

impl<'a> Borrow<SyntaxAndCacheKey<'a>> for OwnedSyntaxAndCacheKey {
    fn borrow(&self) -> &SyntaxAndCacheKey<'a> {
        &self.0
    }
}

impl<'a> SyntaxAndCache<'a> {
    fn new(syntax: Syntax<'a>) -> Self {
        Self {
            syntax,
            cache: OnceMap::default(),
        }
    }

    pub(crate) fn parse(
        &self,
        source: Arc<str>,
        source_path: Option<Arc<Path>>,
    ) -> Result<Arc<Parsed>, ParseError> {
        self.cache.get_or_try_insert(
            &SyntaxAndCacheKey {
                source: Cow::Owned(source),
                source_path: source_path.map(Cow::Owned),
            },
            |key| {
                let key = OwnedSyntaxAndCacheKey(SyntaxAndCacheKey {
                    source: Cow::Owned(Arc::clone(key.source.as_ref())),
                    source_path: key
                        .source_path
                        .as_deref()
                        .map(|v| Cow::Owned(Arc::clone(v))),
                });
                let parsed = Parsed::new(
                    Arc::clone(key.source.as_ref()),
                    key.source_path.as_deref().map(Arc::clone),
                    &self.syntax,
                )?;
                Ok((key, Arc::new(parsed)))
            },
            Arc::clone,
        )
    }
}

#[cfg_attr(feature = "config", derive(Deserialize))]
#[derive(Default)]
struct RawConfig<'a> {
    #[cfg_attr(feature = "config", serde(borrow))]
    general: Option<General<'a>>,
    syntax: Option<Vec<SyntaxBuilder<'a>>>,
    escaper: Option<Vec<RawEscaper<'a>>>,
}

impl RawConfig<'_> {
    #[cfg(feature = "config")]
    fn from_toml_str(s: &str) -> Result<RawConfig<'_>, CompileError> {
        basic_toml::from_str(s).map_err(|e| {
            CompileError::no_file_info(format!("invalid TOML in {CONFIG_FILE_NAME}: {e}"), None)
        })
    }

    #[cfg(not(feature = "config"))]
    fn from_toml_str(_: &str) -> Result<RawConfig<'_>, CompileError> {
        Err(CompileError::no_file_info(
            "TOML support not available",
            None,
        ))
    }
}

#[cfg_attr(feature = "config", derive(Deserialize))]
struct General<'a> {
    #[cfg_attr(feature = "config", serde(borrow))]
    dirs: Option<Vec<&'a str>>,
    default_syntax: Option<&'a str>,
    #[cfg_attr(feature = "config", serde(default))]
    whitespace: Whitespace,
}

#[cfg_attr(feature = "config", derive(Deserialize))]
struct RawEscaper<'a> {
    path: &'a str,
    extensions: Vec<&'a str>,
}

pub(crate) fn read_config_file(
    config_path: Option<&str>,
    span: Option<Span>,
) -> Result<(String, Option<PathBuf>), CompileError> {
    let root = manifest_root();
    let filename = match config_path {
        Some(config_path) => root.join(config_path),
        None => root.join(CONFIG_FILE_NAME),
    };

    if filename.exists() {
        let content = fs::read_to_string(&filename).map_err(|err| {
            CompileError::no_file_info(
                format_args!("unable to read {}: {err}", filename.display()),
                span,
            )
        })?;
        Ok((content, filename.canonicalize().ok()))
    } else if config_path.is_some() {
        Err(CompileError::no_file_info(
            format_args!("`{}` does not exist", filename.display()),
            span,
        ))
    } else {
        Ok((String::new(), None))
    }
}

fn manifest_root() -> PathBuf {
    env::var_os("CARGO_MANIFEST_DIR").map_or_else(|| PathBuf::from("."), PathBuf::from)
}

fn str_set(vals: &[&'static str]) -> Vec<Cow<'static, str>> {
    vals.iter().map(|s| Cow::Borrowed(*s)).collect()
}

static CONFIG_FILE_NAME: &str = "askama.toml";
static DEFAULT_SYNTAX_NAME: &str = "default";
static DEFAULT_ESCAPERS: &[(&[&str], &str)] = &[
    (
        &[
            "askama", "html", "htm", "j2", "jinja", "jinja2", "rinja", "svg", "xml",
        ],
        "Html",
    ),
    (&["md", "none", "txt", "yml", ""], "Text"),
];
