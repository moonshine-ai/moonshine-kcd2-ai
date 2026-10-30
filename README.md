# Moonshine AI for Kingdom Come: Deliverance II

This mod gives every unnamed villager, beggar, miner and varlet in Kuttenberg and Trosky a
name and a *Talk* option. Talk to them and they answer in character, in their own voice,
about their own work and neighborhood, and sometimes they have something for you to do.
Everything runs on your own PC; nothing you say leaves it.

Originally I wrote this for myself as "fan fiction" after I'd completed the game and wanted
to explore more of the world's stories. It uses AI heavily, but it's intended as a tribute
to the amazing work by the whole Warhorse team, not a template for replacing human
creativity. The code is open source under an MIT license, available at
https://github.com/moonshine-ai/moonshine-kcd.

It is an early build: it will do weird things, and I very much appreciate all the bug
reports you can share. The AI-generated speech also sounds rough, especially for a game
that's known for its voice acting. I am in the process of hiring professional actors for
the dialog that's known ahead of time, and licensing their voices for better clones for
freeform speech.

How to install and play it follows. Building it from source is covered under *The
repository* in the project's README at https://github.com/moonshine-ai/moonshine-kcd-ai.

## Installing

Two pieces go on your computer, and one installer puts both in place:

1. **The mod**, a small folder in the game's `mods` folder. It gives people their names
   and the *Talk* option.
2. **The Moonshine app**, about 4 GB, most of it the language model and the
   characters' knowledge, which the installer downloads while it runs. It runs beside the
   game in its own window.

The mod without the app renames people but they have nothing to say. The app without the
mod has nobody to talk to.

Each step below ends with what you should see. If you see something else, go to
*If something goes wrong* below.

### Before you start

- **Windows 10 or 11**, 64-bit.
- **Kingdom Come: Deliverance II on Steam**, updated to the current version (1.5.6). The
  GOG and Epic versions are not supported.
- **16 GB of RAM** at least; **32 GB is recommended**. With 16 GB the game and the model
  share memory and the first answer each session comes slower. The app uses the CPU, not
  the graphics card, so the game keeps the GPU to itself.
- **10 GB free** on your Windows drive (the app installs under your user folder, not the
  game's).
- **An internet connection** for the install: about 4 GB is downloaded. On a slow line
  this is the longest part, and the installer can be left to it.
- **A microphone**, if you want to speak to people rather than type. Optional.
- A Steam account that owns the game, signed in on this PC.

Start the game once on its own before installing anything, so Steam has finished
updating it and the game has written its settings. Then quit it; the installer wants the
game closed.

### Step 1: Download the installer

Download [download.moonshine.ai/kcd2/0.9.99/Moonshine-0.9.99-setup.exe](https://download.moonshine.ai/kcd2/0.9.99/Moonshine-0.9.99-setup.exe); it is around
40 MB. It is the only file you need: the models and the rest come down while it runs.

**You should see** `Moonshine-<version>-setup.exe` in your Downloads folder.

### Step 2: Run the installer

1. Double-click `Moonshine-<version>-setup.exe`.
2. Windows shows a blue **"Windows protected your PC"** box, because the installer is
   not signed with a certificate Windows recognises. Click **More info**, then
   **Run anyway**.
3. A small box asks whether to install for all users or only for you. Choose
   **Install for me only**. (Installing for all users needs administrator rights and is
   not needed.)
4. If a box says the game was not found in a Steam library, something is off: the
   installer cannot put the mod in place without the game. Click **No**, make sure the
   game is installed through Steam and has been started once, and run the installer
   again. (If the game really is somewhere unusual, see *If something goes wrong*.)
5. **Welcome**: click **Next**.
6. **License Agreement**: this lists the open-source software and model weights inside
   the app and their licences. Choose **I accept the agreement**, then **Next**.
7. **Information**: notes about the build. **Next**.
8. **Select Destination Location**: leave the default,
   `C:\Users\<you>\AppData\Local\Moonshine`. **Next**.
9. **Select Additional Tasks**:
   - *Put a Moonshine shortcut on the desktop*: leave ticked.
   - *Install the mod into the game's mods folder*: leave **ticked**. Untick it only if
     you chose the Workshop route below instead.

   **Next**, then **Install**.
10. The program is copied, then the installer downloads the model, voice and data files,
    about 4 GB in all. The status line names the file it is on and the bar covers the
    whole download; on a 100 Mbit/s line expect about six minutes, on a slower one
    longer. Leave it to run.

    If the connection drops, the installer says which file failed and offers **Retry**.
    Retry when the connection is back. If you **Cancel** instead, nothing is lost: run
    the installer again later and it continues with the files it does not have yet.
11. Near the end, if your PC does not already have the Microsoft Visual C++ runtime, a
    Windows **User Account Control** prompt asks for permission to install it. Click
    **Yes**. This is the only step that asks for administrator rights.
12. **Finished**: leave **Check the installation now (recommended)** ticked. *View
    README.txt* can be unticked. Click **Finish**.

**You should see** a window titled *Moonshine* listing everything the app found: the
game folder, the model, the voices, the database. The last line is `ready`. Close the
window.

If the last line is `NOT READY`, look a few lines up for something marked `MISSING` and
see *If something goes wrong*.

### Step 3: Play

Always start the game **through the Moonshine app**, not from Steam, because the mod
needs the companion app running to handle the local AI model generation.

1. Double-click **Moonshine** on the desktop (or search for the Moonshine app after
   pressing the Windows key).
2. A console window opens. It checks the installation, asks Steam to start the game, and
   loads the model, which takes ten to thirty seconds. Leave this window open while you
   play; closing it stops the voices, and the game carries on without them.
3. When the game is up, load a save in Kuttenberg or anywhere around Trosky.
4. Walk up to anyone who used to be called just *Beggar*, *Villager*, *Miner* or the
   like.

**You should see** a name over them instead of their trade, and **Talk** (or **Chat**)
in the interaction menu. Choose it.

- They greet you; the words appear on screen and are spoken aloud. The first line of a
  session takes a few seconds while the model warms up; after that answers take two to
  five seconds.
- A menu follows with a few things Henry might say, then **(Say something else)** and
  **(End dialog)**.
- To speak, **hold the right trigger** (or **V** on the keyboard) while the menu is up
  and say your line. The words appear on screen as they are heard; let go when you are
  done and the line is sent as soon as the app hears you have finished. Keep holding to
  carry on talking. **B** (or **Escape**) cancels and leaves the menu as it was.
- To type instead, choose **(Say something else)**: a line opens for the keyboard, and
  only the keyboard. Press **Enter** (or **A** on a controller) to send. **Escape** (or
  **B**) clears the line; Escape on an empty line brings the menu back.
- **F9** opens the same typed line at any time during a conversation.
- Tap the skip button to cut a line short. **(End dialog)**, or holding skip, leaves.

Some people offer work. Take it or not; they remember either way, and they remember what
you said to them last time.

Things that are known and not worth reporting: a voice that does not quite match the
face; an answer that repeats a phrase; a long pause before the first line after a load;
the lips moving roughly rather than exactly.

To stop: type `/quit` in the Moonshine window, or close it. The game is not affected.

### If something goes wrong

**The installer said the game was not found, or the check ends in `NOT READY` with the
game marked `MISSING`.**
The game is somewhere Steam does not record. Install the app anyway (click **Yes** at
that box), then tell it where the game is: open the Moonshine folder (paste
`%LOCALAPPDATA%\Moonshine` into Explorer's address bar), open `moonshine.cfg` in Notepad
(make it if it is not there) and add one line with your game's folder:

```
root = D:\SteamLibrary\steamapps\common\KingdomComeDeliverance2
```

Save, then run Start Menu > Moonshine > **Check the Moonshine installation**. The mod
was not copied in this case, so get it from the Workshop (below) instead.

**The check ends in `NOT READY` and something else is `MISSING`.**
A download did not finish. Run Start Menu > Moonshine > **Download or repair Moonshine
files**: it fetches whatever is missing or damaged and leaves the rest alone. Running the
installer again does the same.

**The installer stops with "Verification of the source file failed".**
The file that arrived is not the one expected, usually a download cut short by a proxy
or a flaky connection. Choose **Try Again**; if it fails the same way twice, Cancel and
send the installer's log (it is in your `%TEMP%` folder, named `Setup Log <date>.txt`).

**"Windows protected your PC" has no *Run anyway*.**
Click **More info** first; the button appears under the text.

**Nobody has a name.**
The mod is not loading. Check the game's **Mods** menu, and `mod_order.txt` if you have
one: that file is a **whitelist**, and a mod not named in it does not load, Workshop or
not, so add a line `moonshine_names` to it or delete the file if you were not using it
for anything. If *Moonshine NPC Names* is not listed at all, the installer did not copy
it: look for a `moonshine_names` folder inside the game's `mods` folder (in Steam,
right-click the game > Manage > Browse local files). If it is not there, run the
installer again with the mod task ticked, or subscribe on the Workshop (below).

**The game shows the mod twice, or warns about it.**
It is installed both ways. Either unsubscribe from the Workshop item or delete the
`moonshine_names` folder from the game's `mods` folder; keep one.

**Names, but no *Talk* option; or *Talk* and then silence.**
The app is not running or cannot see the game. Is the Moonshine window open, and has it
said it is following the game's log? If you started the game from Steam, quit it and
start it from the Moonshine shortcut instead.

**They do not hear me.**
Windows may be blocking the microphone. Settings > Privacy & security > Microphone: turn
on *Microphone access* and *Let desktop apps access your microphone*. If the app cannot
open the microphone at all, the trigger does nothing and the Moonshine window says so;
you can always type instead through **(Say something else)**.

**The Moonshine window says the app keeps stopping.**
It restarts the app automatically and gives up after five quick failures. Collect the
logs (below) and send them.

**The voice lags far behind the text, or the game stutters while people speak.**
The model is running on your CPU, and some are slower than others. Collect the logs,
which record your CPU, and send them.

#### Collecting logs

Start Menu > Moonshine > **Collect Moonshine logs** (or run `Collect-Logs.ps1` in the
Moonshine folder). It writes `Moonshine-logs-<date>.zip` to your desktop: the app's logs,
the game's `kcd.log`, what the app found of the installation and a few lines about your
PC. No saves, and nothing personal beyond what you typed into conversations. Send that
zip with a sentence about what you were doing when it went wrong.

### Updating

Start Menu > Moonshine > **Update Moonshine** looks for a newer version. If there is
one, it downloads the new installer and starts it; the installer keeps every model and
data file that has not changed, so an update is usually much smaller than the first
install. If you already have the newest version, it checks the files instead and fetches
any that are missing.

### Removing Moonshine

- **The app**: Start Menu > Moonshine > **Uninstall Moonshine**, or Settings > Apps >
  Installed apps > Moonshine > Uninstall. It removes the Moonshine folder and its logs.
- **The mod**: delete the `moonshine_names` folder from the game's `mods` folder (in
  Steam, right-click the game > Manage > Browse local files). The uninstaller leaves it
  alone on purpose, so that removing the app does not change what loads in the game. If
  you used the Workshop, unsubscribe from the item instead.

Saves made with the mod load fine without it; the names simply go back to *Beggar* and
*Villager*.

### What is in it

The language model is Qwen3, the voices are Phonon's British English catalogue, the ear
is Moonshine Voice; all run locally. The list of third-party software and model weights
and their licences is in `LICENSES\README.txt` in the Moonshine folder.

## The repository

Two pieces, built here:

- **The mod** (`mod/`): a deterministic pak writer, `kcd2modgen`, that turns the database
  of souls into a KCD2 mod -- names, dialogue graphs, quests, and one Lua file. No DLL and
  no injector; uninstalling is deleting one folder.
- **The app** (`app/`): `moonshine_app`, a C++ program that runs beside the game, follows
  its log, and talks to the mod through files. It runs the model (llama.cpp), the voices
  (Phonon, through its Rust runtime), the ear (Moonshine Voice), and the knowledge base
  each soul answers from.

And the installer (`installer/`) that packs the app, the models and the data for players.
The part of this file above this heading is the player's guide; `installer\Stage.ps1`
cuts it off here and ships it as the package's `README.txt`, which the installer shows.

The *data*, the names, biographies, relationships, knowledge, quests and routines of
some 5,800 souls, is not made here. It is generated in a separate
repository and arrives as a **drop**: a folder of TSV and JSONL tables, one int8 matrix
of knowledge vectors, the voices, and the game-derived assets, under a manifest of hashes.
`data/DROP` pins the drop this checkout builds from; `tools/assemble_db.py` turns it into
the SQLite file both the app and the generator read, and a second beside it with the
knowledge vectors (EmbeddingGemma's 768 components as int8, a quarter of the float32
matrix) that only the app reads. This repository assembles and
integrates; it does not generate.

## Building

### What you need

- CMake 3.20+, a C++20 compiler (Apple clang, MSVC 2022, or GCC 12+), git, Python 3.10+.
- Rust (`cargo`, from https://rustup.rs) on Windows and Linux, for the voice: the Phonon
  runtime is built from the `app/vendor/xn-ptts` submodule at configure time. Without it
  the app builds and talks but does not speak.
- About 7 GB of disk for the drop and the models.
- Optional: `lua` and `luajit` for the mod's Lua checks (`brew install lua luajit`).

Clone with the submodules (`app/vendor/llama.cpp` and `app/vendor/xn-ptts` are pinned to
upstream commits; the patches in `app/vendor/patches/` are applied at configure time):

```sh
git clone --recurse-submodules <this repository>
cd moonshine-kcd2-ai
```

### The data and the models

```sh
scripts/fetch_drop.sh --from <where drops are kept>     # data/kcd2/npcs.sqlite, npcs-vectors.sqlite, voices
scripts/fetch_models.sh                                 # models/, about 2.9 GB
scripts/fetch_moonshine.sh                              # third_party/moonshine, built
```

`fetch_drop` takes a directory, an `s3://` prefix on any S3-compatible store (R2, S3, B2,
MinIO; `S3_ENDPOINT`, `AWS_ACCESS_KEY_ID` and `AWS_SECRET_ACCESS_KEY` say which, signed
by `tools/s3_sign.py` with no SDK), a `gs://` prefix (through `gcloud`) or an `https://`
prefix holding `<drop name>/`; `MOONSHINE_DROP_BASE` sets the default. It fetches only
files whose hash differs, verifies every one against the manifest, and assembles the
database. `fetch_models` reads `scripts/models.tsv`, which pins each weight file to a
Hugging Face revision (or Moonshine's CDN) and a sha256. EmbeddingGemma comes under the
Gemma Terms of Use; the script says so before fetching it. `fetch_moonshine` clones
Moonshine Voice at its pin and builds the library the app's ear links; without it the app
builds and talks but does not hear. On Windows the three scripts have `.ps1` twins.

### The app

```sh
cmake -S app -B app/build
cmake --build app/build -j8
app/build/moonshine_tests                      # the unit tests
app/build/moonshine_app --soul kkut_additive_man_47 --json   # one soul's sheet
```

`-DMOONSHINE_LLAMA=OFF` builds without llama.cpp (the app then needs a running Ollama,
`--ollama`); `-DMOONSHINE_TTS=OFF -DMOONSHINE_STT=OFF` build without Moonshine Voice;
`-DMOONSHINE_PHONON=OFF` builds without the voice (it is off on macOS regardless, where
the runtime is Core ML only). `-DMOONSHINE_CLONE=ON` builds the earlier Qwen3-TTS engine
back in, beside Phonon, for comparison; it is kept in the tree and off. On
Windows, `scripts\build_windows.bat` finds Visual Studio, configures with Ninja, builds
and runs the tests; `scripts\build_windows.bat release` is the CPU-only build the
installer ships.

### The mod

```sh
scripts/build_mod.sh out/moonshine_names
scripts/build_mod.sh out/moonshine_names --game "/path/to/KingdomComeDeliverance2"
```

This builds `kcd2modgen`, lays the hand-authored assets (`mod/assets/pak`) over the drop's
generated ones, generates the mod with dialogue for every soul, parses the emitted Lua with
`luac`, and plays it through the harness under `mod/harness/` against a stub of the engine.
With `--game` it also writes the level's scheduler override, an edit of that install's own
63 MB `scheduler.xml` for the quests that change a giver's day; without it that step is
skipped with a warning, and the mod works but she keeps her usual day. Output is
deterministic: timestamps inside the paks are pinned to the DOS epoch, so the same drop
gives byte-identical paks on any machine.

### The installer (Windows)

A release is one command from this checkout, with ssh to the Windows build machine
(`wl1`, its copy of the tree under `kcd2ai`) and the R2 token in `CLOUDFLARE_*`:

```sh
scripts/release.sh              # the next number; --version 1.0.0 for a particular one
scripts/release.sh --stage-only # build and stage, run the verifiers, then --publish-only
```

It bumps `VERSION` (and the installer link in this file's Step 1), copies the tracked
files over, runs `scripts\build_windows.bat release` and `installer\Stage.ps1` there,
uploads with `installer\publish.py`, and commits the bump as `Release <version>`, tagged.
The pieces run on their own too. With the release build, the drop, the models and a mod
folder under `release\mod\moonshine_names` in place on the build machine:

```bat
powershell -ExecutionPolicy Bypass -File installer\Stage.ps1 -Version 0.9.123 -DownloadBase https://download.moonshine.ai/kcd2
```

stages the package under `release\stage\`, runs its own preflight from inside it, and
builds `release\out\<version>\Moonshine-<version>-setup.exe` with Inno Setup 6.5+
(installed through winget if missing). The installer is small, about 40 MB: the models,
the database, the voices and the mod it downloads itself while installing, from `[Files]`
entries Stage.ps1 generates (`release\stage\download_files.iss`) out of
`release\stage\Moonshine\manifest.json`. The models come from where `scripts/models.tsv`
pins them (Hugging Face, download.moonshine.ai); the database, voices and mod from
`<DownloadBase>/blobs/<sha256>/<name>`, each file addressed by its own hash. So a file
that has not changed since the last release is the same URL and is never uploaded
twice, a changed one lands beside the old rather than over it, and every installer ever
handed out keeps finding exactly the bytes its manifest names; nothing is deleted. Per
release there are only `<DownloadBase>/<version>/manifest.json` and the installer beside
it, and `<DownloadBase>/latest/manifest.json`, which the installed `Moonshine.ps1 -Update`
reads to find a newer build. `py -3 installer\publish.py --version <version>` (standard
library only; the credentials from the environment, or on stdin from release.sh)
hash-checks the stage against the manifest, uploads the blobs the bucket lacks, then the
installer and manifest, then `latest`; `--check` reports what it would move. The same
manifest drives `Fetch-Package.ps1`, the Start Menu's "Download or repair" and the
update; an in-app Update would read it too. `scripts/release_version.sh` prints the
version a build carries: `VERSION` at the root, unless `MOONSHINE_VERSION` says otherwise.
`installer\Verify-Clean.ps1` runs the installer as a fresh local account and talks to the
result, with `/MIRROR=release\stage\Moonshine` so nothing is fetched (`-Download` to test
the hosting too); `installer\Verify-Install.ps1` does the same from a player's link.

## Installing the mod by hand

The whole of the build output belongs in `<game>\mods\moonshine_names\`, and nothing else
has to be touched. The installer carries a copy and puts it there by default; the Steam
Workshop item is the alternative, for anyone who wants Steam to update the mod on its own
(not both at once: two mods, one id). By hand: copy the folder. If you have other mods and
a `mod_order.txt`, note that the file is a whitelist and must name `moonshine_names` or
the item is skipped.

## Checking that it worked

`kcd.log` in the game root is the first place to look. On a working install it says:

```
[Mod] 'mods/moonshine_names' supports game version '1.5.6' by wildcard '1.5.*', it will be enabled
Pak 'mods\moonshine_names\data\moonshine_names.pak' is opened, root: 'data\'
[Mod] Loading localization patch 'Localization\text__moonshine_names.xml'
Mods used while saving (1):
* mod 'Moonshine NPC Names', author: '...', ver: '...'
```

The last two lines come from the save rather than the loader, so they prove the mod was
active and not merely found. There should be no `hash clash`, the error a wrongly named
localization file raises. Beyond the log the check is visual: a nameplate that read
`Beggar` reads something like `Cross-eyed Adam`, and the interaction menu has **Chat**
(**Talk** on a giver with a favour to ask). The app, started with `--game`, logs
`BRIDGE begin npc=...` when a conversation reaches it and `BRIDGE beat N` while one is up.

## Notes on the format

Three details are easy to get wrong and hard to debug:

- **`<kcd_version>` must match `wh_sys_version`, which is `1.5.6`, not `1.5`.** A value
  that matches neither exactly nor by wildcard makes the engine log `doesn't support game
  version` and disable the mod, which looks exactly like the mod not working. The default
  is `1.5.*`.
- **Paks must not carry ZIP extra fields.** CryPak rejects the NTFS and extended timestamp
  fields that 7-Zip and WinRAR add by default. Entries are stored uncompressed with
  general-purpose flag 0 and sizes in the local header, which is what the game's own paks
  look like. miniz's zip writer always appends a data descriptor and sets flag bit 3, so
  `mod/src/pak.cpp` writes the headers itself and uses miniz only for CRC32.
- **Localization files are read as UTF-8 with no BOM and no XML declaration.** Each row has
  three cells and the middle one is ignored; the third is what players see. The file must
  be named `text__<modid>.xml`.

## Layout

```
app/            the companion app: src/, tests/, vendor/ (llama.cpp and xn-ptts submodules, qwen3-tts, opus, headers)
mod/            kcd2modgen: src/, vendor/miniz, harness/ (the Lua engine stub and its cases), assets/
installer/      Stage.ps1, publish.py, moonshine.iss, the launcher (Moonshine.ps1), the verifiers, LICENSES/
scripts/        release.sh, fetch_*, build_*, release_version.sh, the scheduler and mouth-pak builders, fake_game.py
tools/          assemble_db.py, fetch_drop.py, s3_sign.py
data/           schema.sql (the shipped database's schema), DROP (the pin), kcd2/voices/ (catalogues)
third_party/    sqlite3 (shared by app and mod); moonshine/ when fetched
setting.txt     the world setting every conversation's prompt opens with
VERSION         the version the next release will follow on from (scripts/release.sh bumps it)
```

Not here: the generators, the probes, the design journal and the benchmarks, which live
in the data repository. Comments in the code that cite `mods/kcd2/README.md`,
`behaviors.md` or `quest-design/` refer to that journal.

## Licence

MIT (`LICENSE`). Third-party software and model weights come under their own licences,
listed in `installer/LICENSES/README.txt`. Nothing of Kingdom Come: Deliverance II is in
this repository; the mod reads the game's own files on the player's machine.
