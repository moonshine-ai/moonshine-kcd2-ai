# Moonshine AI for Kingdom Come: Deliverance II

This mod gives every unnamed villager, beggar, miner and varlet in Kuttenberg and Trosky a name and a *Talk* option. Talk to them and they answer in character, in their own voice, about their own work and neighborhood, and sometimes they have something for you to do.

Originally I wrote this for myself as "fan fiction" after I'd completed the game and wanted to explore more of the world's stories. It uses AI heavily, but it's intended as a tribute to the amazing work by the whole Warhorse team, not a template for replacing human creativity. The code is open source under an MIT license, available at https://github.com/moonshine-ai/moonshine-kcd.

Two pieces, built here:

- **The mod** (`mod/`): a deterministic pak writer, `kcd2modgen`, that turns the database
  of souls into a KCD2 mod -- names, dialogue graphs, quests, and one Lua file. No DLL and
  no injector; uninstalling is deleting one folder.
- **The app** (`app/`): `moonshine_app`, a C++ program that runs beside the game, follows
  its log, and talks to the mod through files. It runs the model (llama.cpp), the voices
  (Phonon, through its Rust runtime), the ear (Moonshine Voice), and the knowledge base
  each soul answers from.

And the installer (`installer/`) that packs the app, the models and the data for testers.
`TESTERS.md` is what a tester is sent; `INSTALL.md` is the step-by-step installation
guide for players.

The *data*, the names, biographies, relationships, knowledge, quests and routines of
some 5,800 souls, is not made here. It is generated in a separate
repository and arrives as a **drop**: a folder of TSV and JSONL tables, one float32 matrix
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

It bumps `VERSION` (and INSTALL.md's link), copies the tracked files over, runs
`scripts\build_windows.bat release` and `installer\Stage.ps1` there, uploads with
`installer\publish.py`, and commits the bump as `Release <version>`, tagged. The pieces
run on their own too. With the release build, the drop, the models and a mod folder
under `release\mod\moonshine_names` in place on the build machine:

```bat
powershell -ExecutionPolicy Bypass -File installer\Stage.ps1 -Version 0.9.123 -DownloadBase https://download.moonshine.ai/kcd2
```

stages the package under `release\stage\`, runs its own preflight from inside it, and
builds `release\out\<version>\Moonshine-<version>-setup.exe` with Inno Setup 6.5+
(installed through winget if missing). The installer is small, about 100 MB: the models,
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
the hosting too); `installer\Verify-Install.ps1` does the same from a tester's link.

## Installing the mod

The whole of the build output belongs in `<game>\mods\moonshine_names\`, and nothing else
has to be touched. The installer carries a copy and puts it there by default; the Steam
Workshop item is the alternative, for testers and for anyone who wants Steam to update
the mod on its own (not both at once: two mods, one id). By hand: copy the folder. If
you have other mods and a `mod_order.txt`, note that the file is a whitelist and must name
`moonshine_names` or the item is skipped.

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
