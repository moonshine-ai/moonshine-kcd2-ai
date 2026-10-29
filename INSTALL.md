# Installing Moonshine for Kingdom Come: Deliverance II

Moonshine gives every unnamed villager, beggar, miner and varlet in Kuttenberg and
Trosky a name and a *Talk* option. They answer in character, in their own voice, and
some of them have work for you. Everything runs on your own PC; nothing you say leaves it.

Two pieces go on your computer, and one installer puts both in place:

1. **The mod**, a small folder in the game's `mods` folder. It gives people their names
   and the *Talk* option.
2. **The Moonshine app**, about 5.5 GB, most of it the language model and the
   characters' knowledge, which the installer downloads while it runs. It runs beside the game in its
   own window.

The mod without the app renames people but they have nothing to say. The app without the
mod has nobody to talk to.

Each step below ends with what you should see. If you see something else, go to
*If something goes wrong* at the end.

## Before you start

- **Windows 10 or 11**, 64-bit.
- **Kingdom Come: Deliverance II on Steam**, updated to the current version (1.5.6). The
  GOG and Epic versions are not supported.
- **16 GB of RAM** at least; **32 GB is recommended**. With 16 GB the game and the model
  share memory and the first answer each session comes slower.
- **10 GB free** on your Windows drive (the app installs under your user folder, not the
  game's).
- **An internet connection** for the install: about 5.5 GB is downloaded. On a slow line
  this is the longest part, and the installer can be left to it.
- **A microphone**, if you want to speak to people rather than type. Optional.
- A Steam account that owns the game, signed in on this PC.

Start the game once on its own before installing anything, so Steam has finished
updating it and the game has written its settings. Then quit it; the installer wants the
game closed.

## Step 1: Download the installer

Download [download.moonshine.ai/kcd2/0.9.98/Moonshine-0.9.98-setup.exe](https://download.moonshine.ai/kcd2/0.9.98/Moonshine-0.9.98-setup.exe); it is around
40 MB. It is the only file you need: the models and the rest come down while it runs.

**You should see** `Moonshine-<version>-setup.exe` in your Downloads folder.

## Step 2: Run the installer

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
    about 5.5 GB in all. The status line names the file it is on and the bar covers the
    whole download; on a 100 Mbit/s line expect about ten minutes, on a slower one
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

## Step 3: Play

Always start the game **through the Moonshine app**, not from Steam, because the mod needs the companion app running to handle the local AI model generation.

1. Double-click **Moonshine** on the desktop (or search for the Moonshine app after pressing the Windows key).
2. A console window opens. It checks the installation, asks Steam to start the game, and
   loads the model. Leave this window open while you play; closing it stops the voices,
   and the game carries on without them.
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
- **(Say something else)** opens a line to type into. With a microphone, just speak:
  what the app hears appears in the line. Read it, fix it if you like, and press
  **Enter** (or **A** on a controller) to send. **Escape** (or **B**) clears the line;
  Escape on an empty line brings the menu back.
- **F9** opens the same line at any time during a conversation.
- Tap the skip button to cut a line short. **(End dialog)**, or holding skip, leaves.

Some people offer work. Take it or not; they remember either way, and they remember what
you said to them last time.

To stop: type `/quit` in the Moonshine window, or close it. The game is not affected.

## If something goes wrong

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
The mod is not loading. Check the game's **Mods** menu (Step 3), and `mod_order.txt` if
you have one. If *Moonshine NPC Names* is not listed at all, the installer did not copy
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
on *Microphone access* and *Let desktop apps access your microphone*. You can always type
instead.

**The Moonshine window says the app keeps stopping.**
It restarts the app automatically and gives up after five quick failures. Collect the
logs (below) and send them.

**The voice lags far behind the text, or the game stutters while people speak.**
The model is running on your CPU, and some are slower than others. Collect the logs,
which record your CPU, and send them.

### Collecting logs

Start Menu > Moonshine > **Collect Moonshine logs**. It writes
`Moonshine-logs-<date>.zip` to your desktop: the app's logs, the game's `kcd.log`, what
the app found of the installation and a few lines about your PC. No saves, and nothing
personal beyond what you typed into conversations. Send that zip with a sentence about
what you were doing when it went wrong.

## The Steam Workshop instead

The mod is also on the Steam Workshop: **WORKSHOP_URL**. Subscribing there means Steam
updates the mod on its own. If you prefer that:

1. Subscribe to the item.
2. In the installer, **untick** *Install the mod into the game's mods folder*.

Do not do both: two copies of the mod load it twice. The mod on the Workshop and the app
you installed must be of the same version; if an update to one makes the other
misbehave, update both.

## Updating

Start Menu > Moonshine > **Update Moonshine** looks for a newer version. If there is
one, it downloads the new installer and starts it; the installer keeps every model and
data file that has not changed, so an update is usually much smaller than the first
install. If you already have the newest version, it checks the files instead and fetches
any that are missing.

## Removing Moonshine

- **The app**: Start Menu > Moonshine > **Uninstall Moonshine**, or Settings > Apps >
  Installed apps > Moonshine > Uninstall. It removes the Moonshine folder and its logs.
- **The mod**: delete the `moonshine_names` folder from the game's `mods` folder (in
  Steam, right-click the game > Manage > Browse local files). The uninstaller leaves it
  alone on purpose, so that removing the app does not change what loads in the game. If
  you used the Workshop, unsubscribe from the item instead.

Saves made with the mod load fine without it; the names simply go back to *Beggar* and
*Villager*.

## What is in it

The language model is Qwen3, the voices are Phonon's British English catalogue, the ear
is Moonshine Voice; all run locally. The list of third-party software and model weights and their licences is in
`LICENSES\README.txt` in the Moonshine folder. The code is open source under the MIT
licence at https://github.com/moonshine-ai/moonshine-kcd.
