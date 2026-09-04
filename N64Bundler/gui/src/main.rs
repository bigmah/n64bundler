// SPDX-License-Identifier: GPL-3.0-or-later
//! N64Bundler - a window over the n64rip / N64Recomp / ModernReality pipeline.
//!
//! Pick or drop an N64 ROM; the app recovers the metadata the image does not
//! carry, statically recompiles its MIPS code to native arm64, and adds the
//! game to the library. Building a per-game .app is opt-in, per game.
//!
//! All of the actual work happens in `recompn64`, which lives beside this
//! binary in Contents/Resources.

#![cfg_attr(not(debug_assertions), windows_subsystem = "windows")]

mod library;
mod pipeline;
mod settings;

use dioxus::desktop::tao::event::Event;
use dioxus::desktop::{Config, LogicalSize, WindowBuilder};
use dioxus::html::HasFileData;
use dioxus::prelude::*;
use futures_channel::mpsc::{UnboundedReceiver, UnboundedSender};
use futures_util::StreamExt;
use library::Game;
use pipeline::Msg;
use std::path::{Path, PathBuf};
use std::sync::{Mutex, OnceLock};

const STYLE: &str = include_str!("../assets/style.css");
const MAX_LOG_LINES: usize = 4000;
const ROM_EXTENSIONS: [&str; 4] = ["z64", "n64", "v64", "rom"];

/// ROMs handed to us by Finder. macOS delivers those as `application:openURLs:`
/// rather than as arguments, so they arrive on the tao event loop before the
/// component tree exists and have to be queued.
static OPENED_TX: OnceLock<UnboundedSender<PathBuf>> = OnceLock::new();
static OPENED_RX: Mutex<Option<UnboundedReceiver<PathBuf>>> = Mutex::new(None);

fn main() {
    let (tx, rx) = futures_channel::mpsc::unbounded::<PathBuf>();
    let _ = OPENED_TX.set(tx);
    *OPENED_RX.lock().unwrap() = Some(rx);

    // `N64Bundler game.z64` from a shell starts a job too.
    for argument in std::env::args().skip(1) {
        let path = PathBuf::from(argument);
        if path.is_file() {
            queue_opened(path);
        }
    }

    let window = WindowBuilder::new()
        .with_title("N64Bundler")
        .with_inner_size(LogicalSize::new(1140.0, 760.0))
        .with_min_inner_size(LogicalSize::new(900.0, 560.0));

    let config = Config::new()
        .with_window(window)
        .with_background_color((0x14, 0x14, 0x17, 0xff))
        .with_disable_context_menu(true)
        .with_custom_event_handler(|event, _| {
            if let Event::Opened { urls } = event {
                for url in urls {
                    if let Ok(path) = url.to_file_path() {
                        queue_opened(path);
                    }
                }
            }
        });

    dioxus::LaunchBuilder::desktop().with_cfg(config).launch(app);
}

fn queue_opened(path: PathBuf) {
    if let Some(tx) = OPENED_TX.get() {
        let _ = tx.unbounded_send(path);
    }
}

/// Absolute path to Contents/Resources, where `recompn64` and the toolchain
/// configuration live. Falls back to the source tree for `cargo run`.
fn resources_dir() -> Option<PathBuf> {
    let bundled = std::env::current_exe()
        .ok()
        .and_then(|exe| Some(exe.parent()?.parent()?.join("Resources")));
    let dev = PathBuf::from(env!("CARGO_MANIFEST_DIR"))
        .parent()
        .map(|root| root.join("N64Bundler.app/Contents/Resources"));

    [bundled, dev]
        .into_iter()
        .flatten()
        .find(|dir| dir.join("recompn64").is_file() && dir.join("toolchain.conf").is_file())
}

fn is_rom(path: &Path) -> bool {
    path.extension()
        .and_then(|ext| ext.to_str())
        .map(|ext| ROM_EXTENSIONS.contains(&ext.to_ascii_lowercase().as_str()))
        .unwrap_or(false)
}

#[derive(Clone, PartialEq)]
struct Job {
    rom: String,
    step: u32,
    total: u32,
    title: String,
    running: bool,
    error: Option<String>,
    /// What the analyser reported while this ran, shown under the progress bar
    /// because it is the interesting part: how much of the game was found.
    detail: String,
}

#[derive(Clone, PartialEq)]
struct Line {
    text: String,
    kind: &'static str,
}

impl Line {
    /// Colours a line of tool output by what it looks like. The tools mark
    /// their own headings and errors; everything else is plain.
    fn from_output(text: String) -> Self {
        let trimmed = text.trim_start();
        let kind = if trimmed.starts_with("==>") {
            "head"
        } else if trimmed.starts_with("error") || trimmed.starts_with("warning") {
            "bad"
        } else if trimmed.starts_with("note:") {
            "dim"
        } else {
            ""
        };
        Self { text, kind }
    }
}

/// Appends a line of tool output, dropping the oldest once the console is
/// full. Every long-running tool's output arrives this way.
fn push_output(log: &mut Signal<Vec<Line>>, text: String) {
    let mut lines = log.write();
    lines.push(Line::from_output(text));
    let overflow = lines.len().saturating_sub(MAX_LOG_LINES);
    if overflow > 0 {
        lines.drain(..overflow);
    }
}

/// What the modal over the window is open on. There is at most one.
#[derive(Clone, PartialEq)]
enum Editing {
    Global,
    Game(Game),
}

fn app() -> Element {
    let resources = use_hook(resources_dir);
    // Held in a signal so the closures below stay Copy and can be shared
    // between the button, the file dialog, and the drop handler.
    let recompn64 = use_signal(|| {
        resources
            .clone()
            .map(|dir| dir.join("recompn64"))
            .unwrap_or_default()
    });
    let mut games = use_signal(library::load);
    let mut log = use_signal(Vec::<Line>::new);
    let mut job = use_signal(|| None::<Job>);
    // Game ID of the game currently having a bundle built, so Create App
    // cannot be double-clicked into two concurrent builders.
    let mut making_app = use_signal(|| None::<String>);
    let mut dragging = use_signal(|| false);
    let mut store = use_signal(settings::load);
    let mut editing = use_signal(|| None::<Editing>);

    // Keep the console pinned to the newest line.
    use_effect(move || {
        let _ = log.read().len();
        spawn(async move {
            let _ = document::eval(
                "const el = document.getElementById('console-body');
                 if (el) el.scrollTop = el.scrollHeight;",
            )
            .await;
        });
    });

    if resources.is_none() {
        return rsx! {
            style { dangerous_inner_html: STYLE }
            div { class: "setup",
                h2 { "N64Bundler is not set up yet" }
                p { "The analyser, the recompiler and the runtime have not been built.
                     Run the build script once and then reopen this app." }
                code { "cd n64bundler && ./N64Bundler/build.sh" }
            }
        };
    }

    let launch = move |target: Game| {
        let options = store.read().resolve(&target.game_id);
        // With a bundle, launch it so the game gets its own Dock icon; without
        // one, run the same command the bundle would have run.
        let launched = if target.has_app {
            std::process::Command::new("open")
                .arg(&target.app)
                .spawn()
                .map(|_| ())
        } else {
            let tool = recompn64.peek().clone();
            let mut args = vec![
                "play".to_string(),
                "--game-id".into(), target.game_id.clone(),
                "--rom".into(), target.rom.clone(),
                "--module".into(), target.module.clone(),
                "--title".into(), target.name.clone(),
            ];
            if options.fullscreen {
                args.push("--fullscreen".into());
            }
            std::process::Command::new(tool)
                .args(args)
                .stdin(std::process::Stdio::null())
                .spawn()
                .map(|_| ())
        };
        match launched {
            Ok(()) => log.write().push(Line {
                text: format!("Launched {}.", target.name),
                kind: "good",
            }),
            Err(err) => log.write().push(Line {
                text: format!("Could not launch {}: {err}", target.name),
                kind: "bad",
            }),
        }
    };

    let mut start = move |rom: PathBuf| {
        if job.read().as_ref().is_some_and(|current| current.running) {
            log.write().push(Line {
                text: "Already working on a ROM; this one will have to wait.".into(),
                kind: "dim",
            });
            return;
        }
        let label = rom
            .file_name()
            .map(|name| name.to_string_lossy().into_owned())
            .unwrap_or_else(|| rom.display().to_string());

        log.write().push(Line {
            text: format!("==> {label}"),
            kind: "head",
        });
        job.set(Some(Job {
            rom: label,
            step: 0,
            total: 4,
            title: "Starting".into(),
            running: true,
            error: None,
            detail: String::new(),
        }));

        let (tx, mut rx) = futures_channel::mpsc::unbounded::<Msg>();
        pipeline::run(recompn64.peek().clone(), rom, tx);

        spawn(async move {
            let mut functions = String::new();
            let mut coverage = String::new();
            while let Some(message) = rx.next().await {
                match message {
                    Msg::Line(text) => {
                        if text.trim().is_empty() {
                            continue;
                        }
                        push_output(&mut log, text);
                    }
                    Msg::Step { index, total, title } => {
                        log.write().push(Line {
                            text: format!("==> {index}/{total}  {title}"),
                            kind: "head",
                        });
                        if let Some(current) = job.write().as_mut() {
                            current.step = index;
                            current.total = total;
                            current.title = title;
                        }
                    }
                    Msg::Info { key, value } => {
                        match key.as_str() {
                            // The ROM names itself once the header has been
                            // read, which beats the filename it was dropped as.
                            "name" => {
                                if let Some(current) = job.write().as_mut() {
                                    current.rom = value.clone();
                                }
                            }
                            "functions" => functions = value.clone(),
                            "coverage" => coverage = value.clone(),
                            _ => {}
                        }
                        if !functions.is_empty() {
                            let detail = if coverage.is_empty() {
                                format!("{functions} functions recovered")
                            } else {
                                format!("{functions} functions recovered, {coverage}% of calls placed")
                            };
                            if let Some(current) = job.write().as_mut() {
                                current.detail = detail;
                            }
                        }
                    }
                    Msg::Failed(reason) => {
                        if let Some(current) = job.write().as_mut() {
                            current.error = Some(reason.clone());
                            current.running = false;
                        }
                        log.write().push(Line { text: reason, kind: "bad" });
                    }
                    Msg::Finished(success) => {
                        if let Some(current) = job.write().as_mut() {
                            current.running = false;
                            if !success && current.error.is_none() {
                                current.error = Some("the pipeline exited early".into());
                            }
                            if success {
                                current.step = current.total;
                                current.title = "Done".into();
                            }
                        }
                        if success {
                            games.set(library::load());
                        }
                        break;
                    }
                }
            }
        });
    };

    // ROMs opened through Finder or passed on the command line.
    use_future(move || async move {
        let queued = OPENED_RX.lock().ok().and_then(|mut slot| slot.take());
        let Some(mut queued) = queued else { return };
        while let Some(path) = queued.next().await {
            if is_rom(&path) {
                start(path);
            } else {
                log.write().push(Line {
                    text: format!("Ignored {}: not an N64 ROM.", path.display()),
                    kind: "bad",
                });
            }
        }
    });

    let busy = job.read().as_ref().is_some_and(|current| current.running);

    let pick = move || {
        spawn(async move {
            let chosen = rfd::AsyncFileDialog::new()
                .set_title("Choose an N64 ROM")
                .add_filter("N64 ROM", &ROM_EXTENSIONS)
                .pick_file()
                .await;
            if let Some(file) = chosen {
                start(file.path().to_path_buf());
            }
        });
    };

    rsx! {
        style { dangerous_inner_html: STYLE }
        div {
            class: "shell",
            ondragover: move |event| {
                event.prevent_default();
                if !dragging() { dragging.set(true); }
            },
            ondragleave: move |_| dragging.set(false),
            ondrop: move |event| {
                event.prevent_default();
                dragging.set(false);
                let roms: Vec<PathBuf> = event
                    .files()
                    .iter()
                    .map(|file| file.path())
                    .filter(|path| is_rom(path))
                    .collect();
                match roms.into_iter().next() {
                    Some(rom) => start(rom),
                    None => log.write().push(Line {
                        text: "That is not an N64 ROM. Expected .z64, .n64, or .v64.".into(),
                        kind: "bad",
                    }),
                }
            },

            header { class: "header",
                div {
                    div { class: "wordmark", "N64Bundler" }
                    div { class: "tagline", "N64 cartridges, recompiled to native macOS code" }
                }
                div { class: "spacer" }
                button {
                    title: "What every game starts from",
                    onclick: move |_| editing.set(Some(Editing::Global)),
                    "Settings"
                }
                button {
                    class: "primary",
                    disabled: busy,
                    onclick: move |_| pick(),
                    if busy { "Working…" } else { "Add ROM…" }
                }
            }

            if let Some(current) = job.read().clone() {
                JobStrip { job: current }
            }

            div { class: "body",
                div { class: "library",
                    p { class: "section-label", "Library" }
                    if games.read().is_empty() {
                        div { class: "empty",
                            strong { "No games yet" }
                            "Drop a ROM anywhere in this window, or use Add ROM…"
                            br {}
                            "An N64 ROM carries no symbols, so the first pass has to work out where the code is before it can recompile it. That takes seconds, not minutes, and it is cached afterwards."
                        }
                    }
                    for game in games.read().iter().cloned() {
                        GameCard {
                            key: "{game.game_id}",
                            game: game.clone(),
                            busy,
                            making_app: making_app.read().as_deref() == Some(game.game_id.as_str()),
                            customised: store.read().is_customised(&game.game_id),
                            on_settings: move |target: Game| editing.set(Some(Editing::Game(target))),
                            on_play: launch,
                            on_make_app: move |target: Game| {
                                if making_app.peek().is_some() {
                                    return;
                                }
                                making_app.set(Some(target.game_id.clone()));
                                let tool = recompn64.peek().clone();
                                log.write().push(Line {
                                    text: format!("==> Building {}.app", target.name),
                                    kind: "head",
                                });
                                let (tx, mut rx) = futures_channel::mpsc::unbounded::<Msg>();
                                pipeline::run_args(
                                    tool,
                                    vec![
                                        "make-app".into(),
                                        "--porcelain".into(),
                                        "--game-id".into(), target.game_id.clone(),
                                        "--rom".into(), target.rom.clone(),
                                        "--module".into(), target.module.clone(),
                                        "--title".into(), target.name.clone(),
                                    ],
                                    tx,
                                );
                                let name = target.name.clone();
                                spawn(async move {
                                    while let Some(message) = rx.next().await {
                                        match message {
                                            Msg::Line(text) if !text.trim().is_empty() => {
                                                push_output(&mut log, text);
                                            }
                                            Msg::Failed(reason) => log.write().push(Line {
                                                text: format!("Could not build {name}.app: {reason}"),
                                                kind: "bad",
                                            }),
                                            Msg::Finished(success) => {
                                                if success {
                                                    games.set(library::load());
                                                    log.write().push(Line {
                                                        text: format!("{name}.app is in Applications."),
                                                        kind: "good",
                                                    });
                                                }
                                                making_app.set(None);
                                                break;
                                            }
                                            _ => {}
                                        }
                                    }
                                });
                            },
                            on_log: move |target: Game| {
                                let mut lines = log.write();
                                lines.push(Line {
                                    text: format!("==> Runtime log for {}", target.name),
                                    kind: "head",
                                });
                                for text in library::tail_log(&target.game_id, 200) {
                                    lines.push(Line::from_output(text));
                                }
                            },
                            on_reveal: move |target: Game| {
                                let path = if target.has_app { &target.app } else { &target.rom };
                                let _ = std::process::Command::new("open")
                                    .arg("-R")
                                    .arg(path)
                                    .spawn();
                            },
                            on_forget: move |target: Game| {
                                let kept: Vec<Game> = games
                                    .read()
                                    .iter()
                                    .filter(|entry| entry.game_id != target.game_id)
                                    .cloned()
                                    .collect();
                                let _ = library::save(&kept);
                                games.set(library::load());
                                store.write().games.remove(&target.game_id);
                                let _ = settings::save(&store.read());
                                log.write().push(Line {
                                    text: if target.has_app {
                                        format!(
                                            "Removed {} from the library. {}.app is still in Applications.",
                                            target.name, target.name
                                        )
                                    } else {
                                        format!(
                                            "Removed {} from the library. Nothing on disk was deleted.",
                                            target.name
                                        )
                                    },
                                    kind: "dim",
                                });
                            },
                        }
                    }
                }

                div { class: "console",
                    div { class: "console-head",
                        span { class: "section-label", style: "margin: 0;", "Console" }
                        div { class: "spacer" }
                        button {
                            disabled: log.read().is_empty(),
                            onclick: move |_| log.write().clear(),
                            "Clear"
                        }
                    }
                    div { class: "console-body", id: "console-body",
                        if log.read().is_empty() {
                            div { class: "console-empty",
                                "Output from the analyser, the recompiler, and the games shows up here."
                            }
                        }
                        for line in log.read().iter() {
                            div { class: "line {line.kind}", "{line.text}" }
                        }
                    }
                }
            }

            if let Some(target) = editing.read().clone() {
                SettingsPanel {
                    title: match &target {
                        Editing::Game(game) => game.name.clone(),
                        Editing::Global => "Default settings".to_string(),
                    },
                    subtitle: match &target {
                        Editing::Game(game) => format!("{} — only this game", game.game_id),
                        Editing::Global =>
                            "What every game starts from. A game can override any of it."
                                .to_string(),
                    },
                    per_game: matches!(target, Editing::Game(_)),
                    initial: match &target {
                        Editing::Game(game) => store.read().resolve(&game.game_id),
                        Editing::Global => store.read().defaults.clone(),
                    },
                    on_close: move |_| editing.set(None),
                    on_save: move |options: settings::Options| {
                        let saved = match &target {
                            Editing::Game(game) => {
                                let defaults = store.peek().defaults.clone();
                                {
                                    let mut current = store.write();
                                    if options == defaults {
                                        current.games.remove(&game.game_id);
                                    } else {
                                        current.games.insert(game.game_id.clone(), options.clone());
                                    }
                                }
                                if options == defaults {
                                    format!("{} is back on the defaults.", game.name)
                                } else {
                                    format!("{} has its own settings.", game.name)
                                }
                            }
                            Editing::Global => {
                                store.write().defaults = options.clone();
                                "Default settings saved.".to_string()
                            }
                        };
                        match settings::save(&store.read()) {
                            Ok(()) => log.write().push(Line { text: saved, kind: "good" }),
                            Err(err) => log.write().push(Line {
                                text: format!("Could not save settings: {err}"),
                                kind: "bad",
                            }),
                        }
                        editing.set(None);
                    },
                }
            }

            if dragging() {
                div { class: "drop-veil", "Drop a ROM to recompile it" }
            }
        }
    }
}

#[component]
fn JobStrip(job: Job) -> Element {
    let failed = job.error.is_some();
    let done = !job.running && !failed;
    let percent = if job.total == 0 {
        0.0
    } else {
        (job.step as f32 / job.total as f32) * 100.0
    };
    let fill_class = if failed {
        "fill failed"
    } else if done {
        "fill done"
    } else {
        "fill"
    };

    rsx! {
        div { class: if failed { "job failed" } else { "job" },
            div { class: "job-row",
                if job.running {
                    div { class: "spinner" }
                }
                div { class: "job-title",
                    if let Some(reason) = job.error.clone() {
                        "Failed: {reason}"
                    } else if done {
                        "{job.rom} is ready"
                    } else {
                        "{job.title}"
                    }
                }
                div { class: "spacer" }
                div { class: "job-sub",
                    if job.running { "{job.rom} · step {job.step} of {job.total}" } else { "{job.rom}" }
                }
            }
            div { class: "track",
                div { class: fill_class, style: "width: {percent}%;" }
            }
            if !job.detail.is_empty() {
                div { class: "job-sub", style: "margin-top: 7px;", "{job.detail}" }
            }
        }
    }
}

#[component]
fn GameCard(
    game: Game,
    busy: bool,
    making_app: bool,
    customised: bool,
    on_play: EventHandler<Game>,
    on_settings: EventHandler<Game>,
    on_make_app: EventHandler<Game>,
    on_log: EventHandler<Game>,
    on_reveal: EventHandler<Game>,
    on_forget: EventHandler<Game>,
) -> Element {
    let recovered = if game.functions == 0 {
        String::new()
    } else {
        format!(" · {} functions, {:.0}% of calls placed", game.functions, game.coverage)
    };

    rsx! {
        div { class: "card",
            if game.cover_uri.is_empty() {
                div { class: "cover blank", "{game.game_id}" }
            } else {
                img { class: "cover", src: "{game.cover_uri}", alt: "{game.name}" }
            }
            div { class: "card-text",
                div { class: "card-name", "{game.name}" }
                div { class: "card-meta",
                    "{game.game_id}{recovered}"
                    if customised {
                        span { class: "tag", "custom settings" }
                    }
                }
                if !game.ready {
                    div { class: "warn",
                        "The ROM or the recompiled module is missing. Add the ROM again."
                    }
                }
            }
            div { class: "card-actions",
                button {
                    class: "play",
                    disabled: !game.ready || busy,
                    onclick: {
                        let game = game.clone();
                        move |_| on_play.call(game.clone())
                    },
                    "Play"
                }
                button {
                    title: "How this game starts",
                    onclick: {
                        let game = game.clone();
                        move |_| on_settings.call(game.clone())
                    },
                    "Settings"
                }
                if !game.has_app {
                    button {
                        disabled: !game.ready || busy || making_app,
                        title: "Build a double-clickable .app in ~/Applications",
                        onclick: {
                            let game = game.clone();
                            move |_| on_make_app.call(game.clone())
                        },
                        if making_app { "Building…" } else { "Create App" }
                    }
                }
                button {
                    onclick: {
                        let game = game.clone();
                        move |_| on_log.call(game.clone())
                    },
                    "Log"
                }
                button {
                    onclick: {
                        let game = game.clone();
                        move |_| on_reveal.call(game.clone())
                    },
                    "Reveal"
                }
                button {
                    onclick: {
                        let game = game.clone();
                        move |_| on_forget.call(game.clone())
                    },
                    "Remove"
                }
            }
        }
    }
}

#[component]
fn SettingsPanel(
    title: String,
    subtitle: String,
    per_game: bool,
    initial: settings::Options,
    on_close: EventHandler<()>,
    on_save: EventHandler<settings::Options>,
) -> Element {
    let mut draft = use_signal(|| initial.clone());

    rsx! {
        div { class: "modal-veil", onclick: move |_| on_close.call(()),
            div { class: "modal", onclick: move |event| event.stop_propagation(),
                div { class: "modal-head",
                    div { class: "modal-title", "{title}" }
                    div { class: "modal-sub", "{subtitle}" }
                }
                div { class: "modal-body",
                    div { class: "notice",
                        "Everything about how a game looks — internal resolution, antialiasing, aspect ratio — belongs to RT64, and RT64 keeps its own settings. Press F1 in a running game to change them there."
                    }
                    div { class: "setting",
                        div { class: "setting-label",
                            div { "Start fullscreen" }
                            div { class: "setting-hint", "F11 or Esc toggles it while playing." }
                        }
                        div { class: "setting-control",
                            input {
                                r#type: "checkbox",
                                checked: draft.read().fullscreen,
                                onchange: move |event| draft.write().fullscreen = event.checked(),
                            }
                        }
                    }
                    div { class: "setting",
                        div { class: "setting-label",
                            div { "Developer overlay" }
                            div { class: "setting-hint",
                                "RT64's inspector: the display list, the framebuffers, and what the renderer made of them."
                            }
                        }
                        div { class: "setting-control",
                            input {
                                r#type: "checkbox",
                                checked: draft.read().developer,
                                onchange: move |event| draft.write().developer = event.checked(),
                            }
                        }
                    }
                }
                div { class: "modal-foot",
                    if per_game {
                        div { class: "setting-hint",
                            "Matching the defaults removes this game's override."
                        }
                    }
                    div { class: "spacer" }
                    button { onclick: move |_| on_close.call(()), "Cancel" }
                    button {
                        class: "primary",
                        onclick: move |_| on_save.call(draft.read().clone()),
                        "Save"
                    }
                }
            }
        }
    }
}
