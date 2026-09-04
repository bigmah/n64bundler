// SPDX-License-Identifier: GPL-3.0-or-later
//! The library index that `recompn64` writes after each successful recompile.
//!
//! `make_game_app.py` owns the file; this module only reads it, plus removes
//! entries the user drops from the list. An entry is playable on its own -- the
//! `app` field is empty unless the user opted into building a bundle.

use base64::Engine;
use serde::{Deserialize, Serialize};
use std::path::PathBuf;

#[derive(Clone, Debug, Default, PartialEq, Serialize, Deserialize)]
pub struct Game {
    #[serde(default)]
    pub game_id: String,
    #[serde(default)]
    pub name: String,
    #[serde(default)]
    pub rom: String,
    #[serde(default)]
    pub module: String,
    #[serde(default)]
    pub app: String,
    #[serde(default)]
    pub cover: String,
    #[serde(default)]
    pub added: String,

    /// Cover art as a data URI. Filled in on load; never written back.
    #[serde(skip)]
    pub cover_uri: String,
    /// Whether the recompiled module and the ROM are both still on disk. The
    /// .app is deliberately not part of this: a game is playable from the
    /// library without one.
    #[serde(skip)]
    pub ready: bool,
    /// Whether this game also has a .app bundle built for it.
    #[serde(skip)]
    pub has_app: bool,
    /// What the analyser recovered, read back from its info file so a card can
    /// say how much of the game was found.
    #[serde(skip)]
    pub functions: u64,
    #[serde(skip)]
    pub coverage: f64,
}

#[derive(Default, Serialize, Deserialize)]
struct Index {
    #[serde(default)]
    games: Vec<Game>,
}

pub fn support_dir() -> PathBuf {
    let home = std::env::var("HOME").unwrap_or_else(|_| "/tmp".into());
    PathBuf::from(home).join("Library/Application Support/N64Bundler")
}

pub fn index_path() -> PathBuf {
    support_dir().join("library.json")
}

pub fn load() -> Vec<Game> {
    let text = match std::fs::read_to_string(index_path()) {
        Ok(text) => text,
        Err(_) => return Vec::new(),
    };
    let index: Index = serde_json::from_str(&text).unwrap_or_default();
    index
        .games
        .into_iter()
        .map(|mut game| {
            game.cover_uri = read_cover(&game.cover);
            game.ready = !game.module.is_empty()
                && PathBuf::from(&game.module).is_file()
                && PathBuf::from(&game.rom).is_file();
            game.has_app = !game.app.is_empty() && PathBuf::from(&game.app).is_dir();
            let (functions, coverage) = read_analysis(&game.game_id);
            game.functions = functions;
            game.coverage = coverage;
            game
        })
        .collect()
}

pub fn save(games: &[Game]) -> std::io::Result<()> {
    let path = index_path();
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let index = Index {
        games: games.to_vec(),
    };
    let text = serde_json::to_string_pretty(&index).unwrap_or_else(|_| "{}".into());
    std::fs::write(path, text + "\n")
}

/// The webview cannot read file:// paths from this page, so cover art rides
/// along as a data URI. The images are a few KB each.
fn read_cover(path: &str) -> String {
    if path.is_empty() {
        return String::new();
    }
    match std::fs::read(path) {
        Ok(bytes) => format!(
            "data:image/png;base64,{}",
            base64::engine::general_purpose::STANDARD.encode(bytes)
        ),
        Err(_) => String::new(),
    }
}

/// How many functions the analyser recovered and what share of the ROM's calls
/// landed on one, out of the info file it wrote beside the analysis.
///
/// Scraped rather than parsed as JSON: two numbers out of a flat object this
/// project writes itself is not worth a schema, and a missing or malformed
/// file has to degrade to "no numbers" rather than to an error either way.
fn read_analysis(game_id: &str) -> (u64, f64) {
    let path = support_dir()
        .join("games")
        .join(game_id)
        .join("analysis")
        .join(format!("{game_id}.info.json"));
    let Ok(text) = std::fs::read_to_string(path) else {
        return (0, 0.0);
    };
    let number = |key: &str| -> f64 {
        text.split(&format!("\"{key}\":"))
            .nth(1)
            .and_then(|rest| {
                let trimmed = rest.trim_start();
                let end = trimmed
                    .find(|c: char| !c.is_ascii_digit() && c != '.')
                    .unwrap_or(trimmed.len());
                trimmed[..end].parse().ok()
            })
            .unwrap_or(0.0)
    };
    let functions = number("functions") as u64;
    let on = number("calls_on_boundary");
    let off = number("calls_off_boundary");
    let coverage = if on + off > 0.0 { on / (on + off) * 100.0 } else { 0.0 };
    (functions, coverage)
}

/// Last `lines` lines of a game's runtime log.
pub fn tail_log(game_id: &str, lines: usize) -> Vec<String> {
    let path = support_dir().join("logs").join(format!("{game_id}.log"));
    match std::fs::read_to_string(&path) {
        Ok(text) => {
            let all: Vec<&str> = text.lines().collect();
            all[all.len().saturating_sub(lines)..]
                .iter()
                .map(|line| line.to_string())
                .collect()
        }
        Err(err) => vec![format!("no log at {}: {err}", path.display())],
    }
}
