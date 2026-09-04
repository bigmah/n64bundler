// SPDX-License-Identifier: GPL-3.0-or-later
//! What the window remembers about how a game should start.
//!
//! Deliberately small. Everything about how a game looks -- internal
//! resolution, antialiasing, aspect -- is RT64's, and RT64 keeps its own
//! configuration file that it writes when the in-game overlay changes it.
//! Duplicating those here would give the user two places to set one thing.
//!
//! What is left is what the host has to be told on the command line, because
//! it happens before the renderer exists: whether to open fullscreen, and
//! whether to bring up RT64's developer overlay.

use serde::{Deserialize, Serialize};
use std::collections::BTreeMap;
use std::path::PathBuf;

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct Options {
    #[serde(default)]
    pub fullscreen: bool,
    #[serde(default)]
    pub developer: bool,
}

#[derive(Clone, Debug, Default, Serialize, Deserialize)]
pub struct Store {
    #[serde(default)]
    pub defaults: Options,
    /// Per-cartridge overrides, keyed by game ID.
    #[serde(default)]
    pub games: BTreeMap<String, Options>,
}

impl Store {
    /// What this game actually starts with.
    pub fn resolve(&self, game_id: &str) -> Options {
        self.games.get(game_id).cloned().unwrap_or_else(|| self.defaults.clone())
    }

    pub fn is_customised(&self, game_id: &str) -> bool {
        self.games.get(game_id).is_some_and(|o| *o != self.defaults)
    }
}

fn path() -> PathBuf {
    super::library::support_dir().join("settings.json")
}

pub fn load() -> Store {
    std::fs::read_to_string(path())
        .ok()
        .and_then(|text| serde_json::from_str(&text).ok())
        .unwrap_or_default()
}

pub fn save(store: &Store) -> std::io::Result<()> {
    let file = path();
    if let Some(parent) = file.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let text = serde_json::to_string_pretty(store).unwrap_or_else(|_| "{}".into());
    std::fs::write(file, text + "\n")
}
