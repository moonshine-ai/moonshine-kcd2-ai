# Moonshine AI for Kingdom Come: Deliverance 2

A mod and a companion app that give every unnamed villager, beggar, miner and varlet in
Kuttenberg and Trosky a name and a **Talk** option. Talk to them and they answer in
character, in their own voice, about their own work and neighbourhood, and sometimes they
have something for you to do. Everything runs on the player's own PC: the language model,
the voices and the ear are local, and the game keeps the GPU to itself.

Two pieces, built here:

- **The mod** (`mod/`): a deterministic pak writer, `kcd2modgen`, that turns the database
  of souls into a KCD2 mod -- names, dialogue graphs, quests, and one Lua file. No DLL and
  no injector; uninstalling is deleting one folder.
- **The app** (`app/`): `moonshine_app`, a C++ program that runs beside the game, follows
  its log, and talks to the mod through files. It runs the model (llama.cpp), the cloned
  voices (Qwen3-TTS), the ear (Moonshine Voice), and the knowledge base each soul answers
  from.

And the installer (`installer/`) that packs the app, the models and the data for testers.
`TESTERS.md` is what a tester is sent.

The *data* -- the names, biographies, relationships, knowledge, quests and routines of
some 5,800 souls, and the voice clones -- is not made here. It is generated in a separate
repository and arrives as a **drop**: a folder of TSV and JSONL tables, one float32 matrix
of knowledge vectors, the voices, and the game-derived assets, under a manifest of hashes.
`data/DROP` pins the drop this checkout builds from; `tools/assemble_db.py` turns it into
the SQLite file both the app and the generator read. This repository assembles and
integrates; it does not generate.

## Building

### What you need

- CMake 3.20+, a C++20 compiler (Apple clang, MSVC 2022, or GCC 12+), git, Python 3.10+.
- About 10 GB of disk for the drop and the models.
- Optional: `lua` and `luajit` for the mod's Lua checks (`brew install lua luajit`).

Clone with the submodule (`app/vendor/llama.cpp` is pinned to an upstream commit; the
patches in `app/vendor/patches/` are applied at configure time):

```sh
git clone --recurse-submodules <this repository>
cd moonshine-kcd2-ai
```

### The data and the models

```sh
scripts/fetch_drop.sh --from <where drops are kept>     # data/kcd2/npcs.sqlite, voices
scripts/fetch_models.sh                                 # models/, about 5.4 GB
scripts/fetch_moonshine.sh                              # third_party/moonshine, built
```

`fetch_drop` takes a directory, a `gs://` prefix (through `gcloud`) or an `https://`
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
`--ollama`); `-DMOONSHINE_TTS=OFF -DMOONSHINE_STT=OFF` build without Moonshine Voice. On
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

With the release build, the drop, the models and a mod folder under `release\mod\moonshine_names`
in place:

```bat
powershell -ExecutionPolicy Bypass -File installer\Stage.ps1 -Version 0.9.123
```

stages the package under `release\stage\`, runs its own preflight from inside it, and
builds `release\out\<version>\Moonshine-<version>-setup.exe` with Inno Setup (installed
through winget if missing). `scripts/release_version.sh` prints the version a build
carries, `0.9.<commit count>` unless `MOONSHINE_VERSION` says otherwise.
`installer\Verify-Clean.ps1` runs the installer as a fresh local account and talks to the
result; `installer\Verify-Install.ps1` does the same from a tester's download links.

## Installing the mod

The whole of the build output belongs in `<game>\mods\moonshine_names\`, and nothing else
has to be touched. Testers get it from the Steam Workshop (the installer also carries a
copy, for an off-by-default task that puts it in `mods\`). By hand: copy the folder. If
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
app/            the companion app: src/, tests/, vendor/ (llama.cpp submodule, qwen3-tts, opus, headers)
mod/            kcd2modgen: src/, vendor/miniz, harness/ (the Lua engine stub and its cases), assets/
installer/      Stage.ps1, moonshine.iss, the launcher (Moonshine.ps1), the verifiers, LICENSES/
scripts/        fetch_*, build_*, release_version.sh, the scheduler and mouth-pak builders, fake_game.py
tools/          assemble_db.py, fetch_drop.py
data/           schema.sql (the shipped database's schema), DROP (the pin), kcd2/voices/ (catalogues)
third_party/    sqlite3 (shared by app and mod); moonshine/ when fetched
setting.txt     the world setting every conversation's prompt opens with
```

Not here: the generators, the probes, the design journal and the benchmarks, which live
in the data repository. Comments in the code that cite `mods/kcd2/README.md`,
`behaviors.md` or `quest-design/` refer to that journal.

## Licence

MIT (`LICENSE`). Third-party software and model weights come under their own licences,
listed in `installer/LICENSES/README.txt`. Nothing of Kingdom Come: Deliverance 2 is in
this repository; the mod reads the game's own files on the player's machine.
