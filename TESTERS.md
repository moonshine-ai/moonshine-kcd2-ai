# Moonshine AI for Kingdom Come: Deliverance 2 tester notes

This mod gives every unnamed villager, beggar, miner and varlet in Kuttenberg and Trosky a name and a *Talk* option. Talk to them and they answer in character, in their own voice, about their own work and neighborhood, and sometimes they have something for you to do.

Thank you for trying this. It is an early build: it will do weird things, and I very much appreciate all the bug reports you can share. 

The AI-generated speech also sounds terrible, especially for a game that's known for its voice acting. I am in the process of hiring professional actors for the dialog that's known ahead of time, and licensing their voices for better clones for freeform speech.

Originally I wrote this for myself as "fan fiction" after I'd completed the game and wanted to explore more of the world's stories. It uses AI heavily, but it's intended as a tribute to the amazing work by the whole Warhorse team, not a template for replacing human creativity. The code is open source under an MIT license, available at https://github.com/moonshine-ai/moonshine-kcd.

Two pieces go on your computer:

1. **The mod**, from the Steam Workshop. Small. Names, dialogue graphs, quests, Lua scripts.
2. **The Moonshine app**, from an installer I send you a link to. About 5.5 GB, nearly all
   of it model weights. It runs beside the game and talks to it through files, and provides speech to text and text to speech.

Both are needed. The mod without the app renames people but the Talk option gives them
nothing to say; the app without the mod has nobody to talk to.

## You need

- Windows 10 or 11, 64-bit.
- Kingdom Come: Deliverance 2 on **Steam**, version 1.5.6 (the current one). GOG and Epic
  builds will not do for this round.
- A reasonably recent PC: **32 GB of RAM is recommended**; 16 GB works but the game and
  the model will fight over it and the first answer each session comes slower. The app
  uses the CPU, not the graphics card, so the game keeps the GPU to itself.
- **10 GB free** on the drive your user folder is on (`C:\Users\<you>\AppData\Local`).
- A microphone, if you want to speak to them rather than type. Optional.

## Install, in order

### 1. The mod (Steam Workshop)

Subscribe here: **WORKSHOP_URL** (the item is unlisted; the link is the only way in).

Steam downloads it the next time it checks, usually at once. Start the game once and
look at the main menu's *Mods* entry: *Moonshine NPC Names* should be listed and ticked.

If you have other mods and you made a `mod_order.txt` in the game's `mods\` folder, note
that file is a **whitelist**: a mod not named in it does not load, Workshop or not. Add
a line `moonshine_names` to it, or delete the file if you were not using it for anything.

### 2. The app (installer)

You will get a link to `Moonshine-<version>-setup.exe`, about 100 MB. Run it. The
weights, the database and the voices (about 5.5 GB) are downloaded by the installer while
it runs, with one progress bar over the lot; on a slow line that is the long part. If
the connection drops it offers **Retry**; if you cancel, running it again later picks up
the files it does not have yet.

- Windows will show a blue **"Windows protected your PC"** box, because the installer is
  not signed with a certificate it knows. Click **More info**, then **Run anyway**.
- The installer asks for no administrator rights and installs under
  `C:\Users\<you>\AppData\Local\Moonshine`. The one thing that may ask for permission is
  the Microsoft Visual C++ runtime, if your PC does not already have it.
- **Untick** the option **"Install the mod into the game's mods folder"**. It is ticked
  by default for people who do not use the Workshop; with the Workshop item subscribed
  as well you would have the mod twice.
- At the end, let it **Check the installation**. A window lists everything the app
  found and ends in `ready`. If it ends in `NOT READY`, something is marked `MISSING` a
  few lines up; see *If it does not work*.

## Playing

Start **Moonshine** from the desktop shortcut or the Start Menu (not the game directly).
A console window opens, checks the installation, asks Steam to start the game, and then
loads the model, which takes ten to thirty seconds. Leave that window open while you
play; closing it stops the app, and the game carries on without voices.

In the game, load a save in Kuttenberg or anywhere around Trosky and walk up to anyone
who used to be called just *Beggar* or *Villager* or *Miner*. They have a name now, and
the interaction prompt offers **Talk**. Press it.

- They say something; the words are on screen and spoken aloud.
- A menu follows with a few things Henry might say, then **(Say something else)** and
  **(End dialog)**.
- **(Say something else)** opens a line to type into. If you have a microphone, just
  talk: what the app hears appears in the line as you speak. Read it, fix it if you
  like, and press **Enter** (or the pad's A) to send it. **Escape** (or B) clears the line;
  Escape on an empty line brings the menu back.
- **F9** opens the same line at any time during a conversation.
- Tap the skip button to cut a line short; **(End dialog)** or holding it leaves.

The first line of a session takes a few seconds; after that answers come in two to five
seconds on a modern CPU. Some people will offer you a job. Do it, or do not; they
remember either way, and they remember what you said to them last time.

Things that are known and not worth reporting: a voice that does not quite match the
face; an answer that repeats a phrase; a long pause before the first line after a load;
the lips moving roughly rather than exactly.

## If it does not work

- **The app window says `NOT READY`.** Something is missing from the install, or the
  game was not found. If the game is in a Steam library the app did not look in, make
  a file `moonshine.cfg` in the Moonshine folder with one line:
  `root = D:\SteamLibrary\steamapps\common\KingdomComeDeliverance2` (your path). Then
  run *Check the Moonshine installation* from the Start Menu. Otherwise a download did
  not finish: run *Download or repair Moonshine files* from the Start Menu, which fetches
  only what is missing or damaged.
- **A new build is out.** *Update Moonshine* in the Start Menu downloads the new
  installer and runs it; files that did not change between builds are kept, so it is
  far smaller than the first install.
- **Nobody has a name.** The mod is not loading. Check the *Mods* menu in the game, and
  `mod_order.txt` as above.
- **Names but no Talk option, or Talk and then silence.** The app is not running, or
  cannot see the game. Is the Moonshine window open and past "following the log"?
- **The app window keeps restarting.** It gives up after five quick crashes and says so.
  Send me the logs.
- **The voice is far behind the text, or the game stutters while they speak.** Say so,
  and say what CPU you have; it is in the logs too.

### Sending logs

Start Menu, *Collect Moonshine logs* (or run `Collect-Logs.ps1` in the Moonshine
folder). It writes `Moonshine-logs-<date>.zip` to your desktop: the app's logs, the
game's `kcd.log`, what the app found of the installation and a few lines about your
PC. No saves, nothing personal beyond what you typed into conversations. Send me that
zip and a sentence about what you were doing when it went wrong.

## Removing it

*Uninstall Moonshine* in the Start Menu, or Settings > Apps. It removes the Moonshine
folder and its logs; nothing in the game's folder was changed. Unsubscribe from the
Workshop item to take the mod out. Saves made with the mod active load without it; the
game drops the names and the people go back to being *Beggar*.

## What is in it

A list of the third-party software and model weights, and their licences, is in
`LICENSES\README.txt` in the Moonshine folder. The language model is Qwen3, the voices
are Phonon's British English catalogue, the ear is Moonshine Voice; all run locally.
