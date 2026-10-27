#!/usr/bin/env python3
"""Play the game's part against a real moonshine_app, over the real files.

The unit tests cover what a conversation should do; this covers whether the
running program does it. It is the only check that the two halves of the bridge
agree about turn numbers, since one half is counted inside the app and the other
inside the mod's Lua, and nothing in-process can catch them disagreeing.

What it does is what the mod does, and only what the mod does: it prints its
markers to kcd.log, and it plays the next turn when, and only when, that turn's
file appears. It walks forward exactly one turn at a time, so a turn written
early or twice shows up as a wrong number here rather than as a line said to the
wrong person in the game.

Runs either against a real model or against the app's placeholder turns. Nothing it
checks is about the words, so --canned is the one to reach for:

    python3 scripts/fake_game.py --canned          nothing installed, a few seconds
    python3 scripts/fake_game.py --canned --pick   ...answering off the reply menu
    python3 scripts/fake_game.py --ollama           a real model, from a running Ollama

The app follows every line with the rows of a reply menu, as @opt turns, and streams a
reply as @more turns; neither is a line, and both are taken in as the mod takes them
(see play_spoken). --pick answers each line by choosing a row, as the game's list would,
and requires the rows to be there. --verbose prints what the app said even when the run
passed, which is where its "repeated its last line" notes are. Every row is written
(the seeds, the director's, the soul's own topics and remarks); the model writes none, so a
soul with nothing written has an empty menu after the opening round and the player types.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

# What the app prints once it has swept, prefilled and started following. Waited
# for rather than slept on, because prefill is the better part of ten seconds cold
# and a fixed sleep would either be flaky or slow.
READY = "following the log"

# Where the mod is willing to look, in the order it tries. The app writes all of
# them; finding the first is enough to play a turn.
TURN_DIRS = [
    ("user", "moonshine"),
    ("root", "moonshine"),
    ("root", "Data/moonshine"),
]


OPT = re.compile(rb"^@opt (\d+) (\S+) (\d+):")
COUNTED = re.compile(rb"^(\d+):")


class Turn:
    """A turn file read as the mod's Directives() reads it: the directives off the front,
    then the words. `more` is a reply still being streamed; `rows` the reply menu's rows;
    `commands` everything else (@quest, @said, @memory), kept whole for the eye."""

    def __init__(self, words: str):
        self.more = False
        self.rows: list[tuple[int, str, str]] = []
        # "@rows <n>": the list is whole at n rows (0 for none); "@end": the conversation
        # closes once this turn is read. Both since 4 Oct 2026.
        self.counted: int | None = None
        self.end = False
        self.commands: list[str] = []
        data = words.encode("utf-8")
        while data.startswith(b"@"):
            head, _, rest = data.partition(b" ")
            if head == b"@more":
                self.more = True
                data = rest.lstrip(b" ")
            elif head == b"@rows":
                count, _, rest = rest.partition(b" ")
                if not count.isdigit():
                    break
                self.counted = int(count)
                data = rest.lstrip(b" ")
            elif head == b"@end":
                self.end = True
                self.commands.append("@end")
                data = rest.lstrip(b" ")
            elif head == b"@beat":
                # The speaker's next turn follows this one (a line in two beats, 7 Oct 2026).
                self.commands.append("@beat")
                data = rest.lstrip(b" ")
            elif head == b"@voiced":
                # The app speaks this line; the mod holds for the voice (12 Oct 2026).
                self.commands.append("@voiced")
                data = rest.lstrip(b" ")
            elif head == b"@opt":
                found = OPT.match(data)
                if not found:
                    break
                length = int(found.group(3))
                text = data[found.end():found.end() + length]
                self.rows.append((int(found.group(1)), found.group(2).decode(),
                                  text.decode("utf-8", errors="replace")))
                data = data[found.end() + length:].lstrip(b" ")
            elif head == b"@quest":
                fields = rest.split(b" ", 2)
                if len(fields) < 2:
                    break
                self.commands.append(b" ".join([head] + fields[:2]).decode())
                data = fields[2].lstrip(b" ") if len(fields) > 2 else b""
            elif head == b"@said":
                data, text = take_counted(rest)
                self.commands.append(f"@said {text}")
            elif head == b"@memory":
                # "@memory <soul> compact <rev> <n> <bytes>:<text>" or "@memory <soul> stat <k> <v>"
                fields = rest.split(b" ", 4)
                if len(fields) < 4:
                    break
                if fields[1] == b"compact":
                    data, text = take_counted(fields[4] if len(fields) > 4 else b"")
                    self.commands.append(b" ".join([head] + fields[:4]).decode() + f" {text}")
                else:
                    self.commands.append(b" ".join([head] + fields[:4]).decode())
                    data = fields[4].lstrip(b" ") if len(fields) > 4 else b""
            else:
                break
        self.words = data.decode("utf-8", errors="replace")

    @property
    def silent(self) -> bool:
        """A file that is not a line: a streaming update, or rows (or the count of none)
        and nothing else."""
        return self.more or ((bool(self.rows) or self.counted is not None) and not self.words)


def take_counted(data: bytes) -> tuple[bytes, str]:
    """A `<bytes>:<text>` field, counted in bytes as the app writes them and the mod's Lua
    reads them, so a curly apostrophe does not cut the text three bytes short."""
    found = COUNTED.match(data)
    if not found:
        return b"", ""
    length = int(found.group(1))
    text = data[found.end():found.end() + length]
    return data[found.end() + length:].lstrip(b" "), text.decode("utf-8", errors="replace")


class FakeGame:
    def __init__(self, base: pathlib.Path):
        self.root = base / "game"
        self.user = base / "user"
        self.root.mkdir(parents=True)
        self.user.mkdir(parents=True)
        self.log_path = self.root / "kcd.log"
        self.turn = 0
        self.written_at = 0.0
        # Turns that were files but not lines: streaming updates and menu rows.
        self.silent = 0
        # The rows last offered, as (row, intent, words).
        self.menu: list[tuple[int, str, str]] = []
        # Directives seen on turns, each once, for the transcript.
        self.commands: list[str] = []
        # How often each soul has been talked to, as the mod keeps it in the save: it
        # survives a relaunch, and is dumped before every begin line, counting the
        # conversation that is opening.
        self.met: dict[str, int] = {}
        self.launch()

    def launch(self) -> None:
        """Truncate the log, as the engine does on every start."""
        self.log = self.log_path.open("w", buffering=1)
        self.turn = 0
        self.silent = 0
        self.menu = []
        self.commands = []

    def say(self, line: str) -> None:
        self.log.write(line + "\n")
        self.log.flush()

    def scene(self, blood: float, hour: float | None, rain: float | None) -> str:
        world = "".join(f" {name}={value}" for name, value in (("hour", hour), ("rain", rain))
                        if value is not None)
        return (f" rch=0.5 bld={blood} drt=0.02 dru=0 hp=100 drawn=false "
                f"asleep=false chatting=false{world}")

    def look(self, npc: str, blood: float = 0.0, doing: str = "none",
             hour: float = 16.7823, rain: float = 0.24) -> None:
        """The player has the soul in their sights from talking distance and has not pressed.

        What the mod logs once per look: the record dumped as the begin's would be -- met
        counting the conversation that has not started, nothing stored -- and then a look line
        with the begin's own readings, so the app can have the opening line ready by the time
        they press. A look that is followed by a begin with the same readings is the case worth
        exercising; the app matches the two by the sheet it builds from each.
        """
        self.say(f"<12:04:28> BRIDGE state npc={npc} met={self.met.get(npc, 0) + 1} first=nil "
                 "last=nil rev=0 entries=0")
        self.say(f"<12:04:29> BRIDGE look npc={npc} doing={doing}{self.scene(blood, hour, rain)}")

    def begin(self, npc: str, blood: float = 0.0, doing: str = "none",
              hour: float = 16.7823, rain: float = 0.24) -> None:
        """One BRIDGE begin, shaped exactly as the emitted Lua writes it.

        The mod reports raw readings and no verdict: which state gets remarked on is
        app/src/approach.cpp's decision. So this sends numbers, and blood is the one the
        harness varies because it is the cheapest state to reach a written greeting with.

        hour and rain are the world rather than either party, and the defaults are a drizzly
        late afternoon so the ordinary run exercises both. Passing None for either leaves the
        field off the line entirely, which is what a mod too old to read it does -- and what
        the app has to keep working against, since the mod can only be deployed while the game
        is shut and so is routinely a lap behind.
        """
        # The save's record of the soul, dumped ahead of the begin line as the mod does it;
        # nothing remembered yet but the count, which is what the reunion greeting reads.
        self.met[npc] = self.met.get(npc, 0) + 1
        self.say(f"<12:04:30> BRIDGE state npc={npc} met={self.met[npc]} first=nil last=nil "
                 "rev=0 entries=0")
        self.say(f"<12:04:31> BRIDGE begin npc={npc} doing={doing}{self.scene(blood, hour, rain)}")

    def left(self, why: str = "player walked away") -> None:
        self.say(f"<12:05:02> BRIDGE left, {why}, 3.4m away")

    def typed(self, words: str) -> None:
        """The player's words off the mod's own keyboard, as the mod logs them."""
        self.say(f"<12:04:40> [moonshine_names] BRIDGE said {words}")

    def picked(self, row: int) -> str:
        """The player chose a row of the menu: the mod logs the pick, then the words."""
        number, intent, text = self.menu[row - 1]
        self.say(f"<12:04:41> [moonshine_names] BRIDGE picked {number} {intent} -- {text}")
        self.typed(text)
        return text

    def turn_file(self, turn: int) -> pathlib.Path | None:
        for which, sub in TURN_DIRS:
            path = (self.root if which == "root" else self.user) / sub / f"turn_{turn}.txt"
            if path.exists():
                return path
        return None

    def play(self, timeout: float) -> str | None:
        """Wait for the next turn's file, then play it, exactly as the pump does.

        Deliberately as credulous as the mod: it says whatever is in the file, with
        no idea which run wrote it. Telling a fresh turn from a stale one is the
        app's job, and `written_since` is how the test checks it was done.
        """
        deadline = time.monotonic() + timeout
        want = self.turn + 1
        while time.monotonic() < deadline:
            path = self.turn_file(want)
            if path is not None:
                try:
                    words = path.read_text(encoding="utf-8", errors="replace").strip()
                except PermissionError:
                    # Windows: the app's rename over the path is in flight, and the file is
                    # not to be had this instant. The mod meets the same and reads next tick.
                    time.sleep(0.02)
                    continue
                self.written_at = path.stat().st_mtime
                self.turn = want
                self.say(f"<12:04:35> BRIDGE played turn={want} -- {words}")
                return words
            time.sleep(0.05)
        return None

    def play_spoken(self, timeout: float) -> str | None:
        """Play turns until one carries words, as the mod's pump does with what it reads.

        Two kinds of turn carry none. A reply being streamed arrives as @more turns, each
        the text so far, and only the last, unflagged, is the line. And the rows of the reply
        menu come as @opt turns, one file per row accepted, each carrying the whole list;
        those are kept in `menu` for `picked` and are not lines either. Both are counted in
        `silent` so the turn arithmetic can leave them out.
        """
        deadline = time.monotonic() + timeout
        while True:
            words = self.play(timeout=max(0.0, deadline - time.monotonic()))
            if words is None:
                return None
            turn = self.take(words)
            if turn.silent:
                continue
            return turn.words

    def settle(self, quiet: float) -> str | None:
        """Play whatever silent turns keep arriving, until none has for `quiet` seconds.

        For the rows after a line: they are written in groups as the app decides them,
        and there is no marker for the last, so the end of them is a pause. Returns
        the words of a turn that had some, which is a line nobody asked for.
        """
        while True:
            words = self.play(timeout=quiet)
            if words is None:
                return None
            turn = self.take(words)
            if turn.silent:
                continue
            return turn.words

    def take(self, words: str) -> Turn:
        """Reads a turn as the mod does, keeping its rows and counting it if it is silent."""
        turn = Turn(words)
        if turn.rows:
            # Each file carries the whole list so far; only the row it added is news.
            same_start = turn.rows[:len(self.menu)] == self.menu
            for number, intent, text in turn.rows[len(self.menu):] if same_start else turn.rows:
                print(f"  row {number}   [{intent}] {text}", flush=True)
            self.menu = turn.rows
        elif turn.counted == 0:
            print("  rows    (none: the way out alone)", flush=True)
            self.menu = []
        for command in turn.commands:
            if command not in self.commands:
                print(f"  {command}", flush=True)
                self.commands.append(command)
        if turn.silent:
            self.silent += 1
        return turn

    def written_since(self, when: float) -> bool:
        """Whether the turn just played was written after `when`.

        The check that catches a stale file being spoken, and it holds however the
        words happen to come out: comparing the text would pass by luck, since two
        generations of the same prompt differ anyway.
        """
        return self.written_at > when


class Pump:
    """Reads the app's output as it arrives, so the test can wait for a line."""

    def __init__(self, stream):
        self.lines: list[str] = []
        self._lock = threading.Lock()
        self._thread = threading.Thread(target=self._read, args=(stream,), daemon=True)
        self._thread.start()

    def _read(self, stream) -> None:
        for line in stream:
            with self._lock:
                self.lines.append(line.rstrip("\n"))

    def wait_for(self, needle: str, timeout: float) -> bool:
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self._lock:
                if any(needle in line for line in self.lines):
                    return True
            time.sleep(0.05)
        return False

    def capture(self, pattern: str, timeout: float, since: int = 0) -> str | None:
        """The first group of the first line matching pattern, once one arrives; only lines
        from index `since` on (see mark()), for a line the app prints once a conversation."""
        expression = re.compile(pattern)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self._lock:
                for line in self.lines[since:]:
                    found = expression.search(line)
                    if found:
                        return found.group(1)
            time.sleep(0.05)
        return None

    def mark(self) -> int:
        """How many lines have arrived, to pass to capture() as `since`."""
        with self._lock:
            return len(self.lines)

    def text(self) -> str:
        with self._lock:
            return "\n".join(self.lines)


def run(args: argparse.Namespace) -> int:
    base = pathlib.Path(tempfile.mkdtemp(prefix="moonshine-fake-"))
    faults: list[str] = []
    transcript: list[dict[str, object]] = []
    try:
        game = FakeGame(base)
        # A turn file from a run that crashed. The app must sweep it: left there, the
        # mod walks forward while the next turn is readable and races through words
        # meant for somebody else.
        stale = game.user / "moonshine"
        stale.mkdir(parents=True, exist_ok=True)
        (stale / "turn_1.txt").write_text("words from a run that died\n")

        command = [
            str(args.app), "--game",
            "--root", str(game.root),
            "--user", str(game.user),
            "--world", str(args.world),
        ]
        # Everything this checks is about turn numbers and files, none of which needs
        # a model to have an opinion. Run canned, it needs nothing installed and
        # finishes in a second or two.
        if args.canned:
            command.append("--canned")
        elif args.ollama:
            command += ["--ollama", "--host", args.host, "--model", args.model]
        else:
            # The shipping backend, and so the default here too. --model still matters: with
            # no --gguf it is how the app finds the weights Ollama already downloaded.
            command += ["--model", args.model]
            if args.gguf:
                command += ["--gguf", str(args.gguf)]
        # Off in the app by default, so this is what asks for the other path. The assertions
        # below follow the same flag, which is the point: both halves of the switch get checked.
        if not args.written_greetings:
            command.append("--generated-greetings")
        # Every second rather than every two minutes, so the idle re-warm fires repeatedly
        # during a run that lasts seconds. Asked for explicitly because in-process it is off by
        # default -- there is nobody to lose the cache to -- and it is still worth exercising:
        # the re-warm takes the same lock as the game's own events and holds it across a call to
        # the model, so a run that still counts its turns correctly is a run it did not
        # interfere with.
        command += ["--rewarm", "1"]

        app = subprocess.Popen(
            command,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )

        pump = Pump(app.stdout)

        # The launcher starts before the game does, so the test waits for it the way a
        # player would. Prefill is most of this wait.
        print(f"waiting up to {args.warmup}s for the app to prefill", flush=True)
        if not pump.wait_for(READY, timeout=args.warmup):
            print("the app never started following the log\n", file=sys.stderr)
            print(pump.text(), file=sys.stderr)
            app.kill()
            return 1

        if game.turn_file(1) is not None:
            faults.append("the stale turn from the dead run was not swept")

        def player(said: str) -> None:
            app.stdin.write(said + "\n")
            app.stdin.flush()

        # The NPC speaks first: a player who pressed the button should not have to
        # open the conversation themselves.
        game.begin(args.soul, args.blood, args.doing)

        # The app reports who it decided the soul id belongs to. Checking it here is
        # what makes this a test of the persona lookup and not just of the file
        # protocol: a lookup that silently falls back to a stranger writes turns just
        # as happily as one that works.
        who = pump.capture(r"-- talking to (.+?) \(", timeout=args.timeout) or "?"
        if who in ("?", "Stranger"):
            faults.append(f"{args.soul} was not looked up; the app said {who!r}")

        # The animation name has to survive the log line, the field parse, the database
        # lookup and the sheet, and every one of those is a place it can be dropped in
        # silence. A stale mod once cost a whole play session exactly this way: the look
        # was never reported, the app served the everyday greeting, and nothing anywhere
        # said the field was missing rather than empty.
        clause = clause_for(args.personas, args.doing)
        if clause:
            # The status line carries the chosen state after the clause when it is not
            # "plain", so the clause stops at the comma rather than running to the end.
            said = pump.capture(r"-- talking to .+?\(.+?\), caught ([^,]+)",
                                timeout=args.timeout)
            if said != clause:
                faults.append(f"doing={args.doing} should have been caught {clause!r}, "
                              f"but the app said {said!r}")

        # The world has the same chain to survive -- log line, field parse, session, prompt --
        # and the same way of failing in silence, since an hour nobody reads is simply not
        # mentioned and reads exactly like a dry day nobody remarked on. begin() sent a drizzly
        # late afternoon, so both halves have to come back.
        for wanted in ("late afternoon", "drizzly"):
            if not pump.capture(rf"-- talking to .+?\b({re.escape(wanted)})\b",
                                timeout=args.timeout):
                faults.append(f"the world sent on the begin line did not reach the app: "
                              f"nothing said {wanted!r}")

        greeting = game.play_spoken(timeout=args.timeout)
        if greeting is None:
            faults.append("no greeting was written when the conversation opened")
        else:
            transcript.append({"turn": 1, "npc": greeting})
            print(f"  turn 1  {who}: {greeting}", flush=True)
            # The greeting the generator wrote for them, said as it stands. Checked here
            # because the win is latency and latency is invisible to a unit test: generating
            # this line instead cost 15.15s of the player standing in silence.
            # The user directory is made fresh for every run, so the app's book of
            # acquaintances starts empty and this is necessarily a first meeting.
            wrote = written_greeting(args.personas, args.soul, met=0)
            if not wrote:
                pass
            elif args.written_greetings and greeting != wrote:
                faults.append(f"the greeting written for a stranger was not said; got "
                              f"{greeting!r}, not {wrote!r}")
            elif not args.written_greetings and greeting == wrote:
                # The other half of the switch, and worth asserting rather than assuming: with
                # written greetings off the opening line has to come from the model, and a soul
                # who has lines on disk is the only case where saying one anyway would be
                # invisible -- the words would be good, and the 15s would be gone with them.
                faults.append(f"written greetings are off, but the opening line was the written "
                              f"one: {greeting!r}")

        # The rows for the line just said follow it, one turn each, and the next line is
        # not due until the player has spoken; so a pause with nothing new is the menu
        # complete, and where a pick can be made from it.
        stray = game.settle(args.settle)
        if stray is not None:
            faults.append(f"a line arrived that nobody had spoken for: {stray!r}")
        if args.pick and len(game.menu) != 4:
            faults.append(f"the opening line was followed by {len(game.menu)} rows, not 4")

        for i, said in enumerate(args.say, start=2):
            if args.pick and game.menu:
                row = (i - 2) % len(game.menu) + 1
                said = game.picked(row)
                print(f"  player: [row {row}] {said}", flush=True)
            else:
                print(f"  player: {said}", flush=True)
                player(said)
            # The rows on offer were for the line before; the reply brings its own.
            game.menu = []
            words = game.play_spoken(timeout=args.timeout)
            if words is None:
                faults.append(f"nothing was written for turn {i} after the player spoke")
                break
            transcript.append({"turn": i, "player": said, "npc": words})
            print(f"  turn {i}  {who}: {words}", flush=True)
            stray = game.settle(args.settle)
            if stray is not None:
                faults.append(f"a line arrived that nobody had spoken for: {stray!r}")
            if args.pick and not game.menu:
                faults.append(f"no rows followed the line after the player's turn {i}")

        # Every turn got its own file, in order and without a gap. The engine caches
        # what it reads by path, so a repeated number is a line that cannot be said.
        if game.turn - game.silent != len(transcript):
            faults.append(f"played {game.turn} turns, {game.silent} of them rows or streaming, "
                          f"but collected {len(transcript)} lines")

        # Leaving and returning: the count carries on, because the file names have to
        # stay unique for as long as the game is open.
        game.left()
        time.sleep(0.5)
        # The player looks at them before pressing, as the mod reports from the frame their
        # name comes up. The app plans the opening line on it: generated ahead when nothing is
        # written for the moment, or the sheet read in when a line is. Either way the look has
        # to reach the app and be seen as this soul, which is what the status proves; and a
        # guess made has to be the line said on the press that follows with the same readings.
        game.look(args.soul)
        looked = pump.capture(r"-- (looked at .+)", timeout=args.timeout)
        guessed_ahead = False
        pressed_early = False
        if looked is None:
            faults.append("the look at the soul before the second conversation never reached the app")
        elif args.press_after is not None:
            # The player quicker than the model: the press comes while the guess is still
            # being made, and what it must find is the prefix the guess built, not a cold
            # sheet. Checked below, off the timing line's cached count.
            time.sleep(args.press_after)
            guessed_ahead = pump.wait_for("opening line ahead, ", timeout=0.0)
            pressed_early = not guessed_ahead and "opening line is guessed ahead" in looked
        elif "opening line is guessed ahead" in looked:
            guessed_ahead = pump.wait_for("opening line ahead, ", timeout=args.timeout)
            if not guessed_ahead:
                faults.append("the app planned to guess the opening line on the look and never did")
        elif "sheet is read in ahead" in looked:
            if not pump.wait_for("'s sheet is read in", timeout=args.timeout):
                faults.append("the app set out to read the sheet in on the look and never did")
        else:
            faults.append(f"the look was seen but not planned for: {looked!r}")
        before, silent_before = game.turn, game.silent
        pressed_at = pump.mark()
        game.begin(args.soul)
        again = game.play_spoken(timeout=args.timeout)
        if again is None:
            faults.append("the second conversation never opened")
        elif guessed_ahead and not pump.wait_for(
                "opening line was guessed while the player looked at them, and is said at once",
                timeout=args.timeout):
            faults.append("the opening line guessed on the look was not the one said on the press")
        elif pressed_early:
            if not pump.wait_for("still being guessed when the player pressed", timeout=args.timeout):
                faults.append("the press came during the guess, and the app did not say so")
            world_tokens = pump.capture(r"world:\s+(\d+) tok", timeout=1.0)
            cached = pump.capture(r"\[(\d+) of those \d+ tokens were already cached", timeout=args.timeout,
                                  since=pressed_at)
            if world_tokens is not None and cached is not None and int(cached) < int(world_tokens) + 32:
                faults.append(f"the press during the guess found only {cached} tokens cached against a "
                              f"world of {world_tokens}: the guess's prefill was lost")
        elif game.turn - (game.silent - silent_before) != before + 1:
            # Streaming updates and rows in between are turns too, but not lines.
            faults.append(f"turn {game.turn} followed turn {before}, which is not next")
        else:
            print(f"  turn {game.turn}  {who}: {again}", flush=True)
        # Its rows, before the relaunch below sweeps them; otherwise they are files from the
        # last run and the check that the sweep worked has something real to find.
        game.settle(args.settle)

        # A relaunch. The mod counts from one again and looks for turn 1 within a
        # beat, so anything still on disk from this session is a line it will say to
        # whoever the player talks to next.
        restarted_at = time.time()
        game.launch()
        # Same state as the first conversation, so the only thing that has changed between
        # the two openings is that they have met.
        game.begin(args.soul, args.blood, args.doing)
        fresh = game.play_spoken(timeout=args.timeout)
        if fresh is None:
            faults.append("nothing was written after the game restarted")
        elif game.turn - game.silent != 1:
            faults.append(f"after a restart the first line was turn {game.turn}, "
                          f"with {game.silent} silent turns before it, not turn 1")
        elif not game.written_since(restarted_at):
            faults.append("the first turn after the restart was a file from the last run")
        else:
            print(f"  restarted, turn 1  {who}: {fresh}", flush=True)
            # Second conversation with the same soul, so they now know the player by sight and
            # must say so. This is the whole met path proven end to end: the app wrote the soul
            # into its book when the first conversation opened, read it back here, and served
            # the other of the two lines written for them.
            knew = written_greeting(args.personas, args.soul, met=1)
            if knew and args.written_greetings and fresh != knew:
                first = written_greeting(args.personas, args.soul, met=0)
                faults.append(
                    "the second conversation with the same soul was greeted as a first: got "
                    f"{fresh!r}, which is "
                    + ("the line written for a stranger"
                       if fresh == first else f"not the line written for a reunion, {knew!r}"))

        app.stdin.write("/quit\n")
        app.stdin.flush()
        try:
            app.wait(timeout=10)
        except subprocess.TimeoutExpired:
            app.kill()
            faults.append("the app did not exit when asked")
        output = pump.text()
    finally:
        shutil.rmtree(base, ignore_errors=True)

    if args.json:
        print(json.dumps({"faults": faults, "transcript": transcript}, indent=2))

    print()
    if faults:
        print("FAILED")
        for fault in faults:
            print(f"  - {fault}")
        print("\n--- what the app said ---")
        print(output)
        return 1
    if args.verbose:
        print("--- what the app said ---")
        print(output)
    print(f"OK: {len(transcript)} turns, all in step with the game")
    return 0


class ModHost:
    """The emitted Lua hosted headless (mod/harness/host.lua) in place of the lines
    this file writes itself: the real mod logs the markers, reads the turn files and keeps
    the store, and this drives the player's side over its stdin. The fake clock moves only
    on tick(), so a run is as fast as the app answers."""

    def __init__(self, mod: pathlib.Path, root: pathlib.Path, user: pathlib.Path, echo: bool = False):
        here = pathlib.Path(__file__).resolve().parent.parent
        command = ["luajit", str(here / "mod/harness/host.lua"), "--mod", str(mod),
                   "--root", str(root), "--user", str(user)]
        if echo:
            command.append("--echo")
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=None if echo else subprocess.DEVNULL, text=True, bufsize=1)
        ready = self.proc.stdout.readline().strip()
        if not ready.startswith("ready "):
            raise RuntimeError(f"the host did not come up: {ready!r}")
        self.log_path = pathlib.Path(ready.split(" ", 1)[1])
        self.ports: list[str] = []

    def ask(self, command: str) -> str:
        self.proc.stdin.write(command + "\n")
        self.proc.stdin.flush()
        line = self.proc.stdout.readline()
        if not line:
            raise RuntimeError(f"the host died on {command!r}")
        line = line.rstrip("\n")
        if line.startswith("err "):
            raise RuntimeError(f"{command!r}: {line[4:]}")
        return line[3:] if line.startswith("ok ") else ""

    def fields(self, command: str) -> dict[str, str]:
        """key=value pairs; a word with no '=' belongs to the value before it (a name)."""
        out: dict[str, str] = {}
        last = None
        for part in self.ask(command).split(" "):
            key, sep, value = part.partition("=")
            if sep:
                out[key] = value
                last = key
            elif last is not None:
                out[last] += " " + part
        return out

    def tick(self, ms: int = 400) -> dict[str, str]:
        got = self.fields(f"tick {ms}")
        self.ports = [p for p in got.get("ports", "").split(",") if p]
        return got

    def rows(self) -> list[tuple[int, str, str]]:
        out = []
        for item in self.ask("rows").split("\x1f"):
            if item:
                row, intent, text = item.split("\t", 2)
                out.append((int(row), intent, text))
        return out

    def until(self, want, timeout: float, step: int = 400, pause: float = 0.03) -> dict[str, str] | None:
        """Tick the clock until want(status) holds or `timeout` real seconds pass. The pause
        between ticks is what gives the app its turn at the files."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            got = self.tick(step)
            if want(got):
                return got
            time.sleep(pause)
        return None

    def until_menu(self, timeout: float) -> dict[str, str] | None:
        return self.until(lambda s: s.get("menu") == "true" and int(s.get("drawn", "0")) > 0, timeout)

    def until_closed(self, timeout: float) -> dict[str, str] | None:
        return self.until(lambda s: s.get("active") == "false", timeout)

    def log_text(self) -> str:
        try:
            return self.log_path.read_text(encoding="utf-8", errors="replace")
        except FileNotFoundError:
            return ""

    def quit(self) -> None:
        try:
            self.proc.stdin.write("quit\n")
            self.proc.stdin.flush()
            self.proc.wait(timeout=5)
        except (BrokenPipeError, OSError, subprocess.TimeoutExpired):
            self.proc.kill()


def run_mod(args: argparse.Namespace) -> int:
    """Play a whole errand: the real app and the real emitted Lua, headless.

    look, begin, the help row, the offer, the accepting row, @quest accept, the store,
    the close; the holder's own menu; save, quit, relaunch, load; begin again with
    have=1, @quest complete, the thing home, the reward, done. Judged from the kcd.log
    trail the way a quest pass on the Windows laptop is judged."""
    base = pathlib.Path(tempfile.mkdtemp(prefix="moonshine-mod-"))
    root, user = base / "game", base / "user"
    root.mkdir(parents=True)
    user.mkdir(parents=True)
    faults: list[str] = []
    steps: list[tuple[str, float]] = []
    started = time.monotonic()
    host: ModHost | None = None
    app: subprocess.Popen | None = None

    def step(name: str) -> None:
        steps.append((name, time.monotonic() - started))
        print(f"  {time.monotonic() - started:6.1f}s  {name}", flush=True)

    def fault(text: str) -> None:
        faults.append(text)
        print(f"  FAULT: {text}", flush=True)

    try:
        host = ModHost(args.mod, root, user, echo=args.echo)
        quest = host.fields(f"quest {args.quest}")
        qid, giver, holder, item = quest["id"], quest["giver"], quest["target"], quest["item"]
        kind = quest["kind"]
        if kind != "retrieve":
            print(f"--mod plays a retrieve; {quest['name']} is a {kind}", file=sys.stderr)
            return 2
        if args.how == "pay" and int(quest["pay"]) == 0:
            # The giver pays money, so the holder's menu has no pay row and the quest no
            # complete_paid beat (README, *The holder's menu*).
            print(f"{quest['name']}: the giver pays, so the holder has no pay row; pick another --how "
                  "or a quest whose pay is above 0", file=sys.stderr)
            return 2
        host.ask(f"soul {giver} 1 0 0")
        host.ask(f"soul {holder} 2 0 0")
        host.ask(f"give {holder} {item} 1")
        step(f"host up, {quest['name']} ({qid}): {giver} wants {item[:8]}… from {holder}")

        command = [str(args.app), "--game", "--root", str(root), "--user", str(user),
                   "--world", str(args.world), "--canned"]
        app = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, text=True, bufsize=1)
        pump = Pump(app.stdout)
        if not pump.wait_for(READY, timeout=args.warmup):
            print("the app never started following the log\n", file=sys.stderr)
            print(pump.text(), file=sys.stderr)
            return 1
        step("app following the log")

        # The giver's prompt reads as the favour's own.
        hint = host.ask(f"look {giver}")
        if hint != "favour":
            fault(f"the giver's prompt read {hint!r}, not the favour hint")

        # Conversation 1.
        host.ask(f"begin {giver}")
        if not host.until_menu(args.timeout):
            fault("no menu was drawn after the greeting")
            raise StopIteration
        rows = host.rows()
        step("menu 1: " + "; ".join(f"[{i}] {t}" for i, _, t in rows))
        help_row = next((r for r, intent, _ in rows if intent == "help"), None)
        if help_row is None:
            fault("no help row on the opening menu")
            raise StopIteration
        host.ask(f"pick {help_row}")
        if not host.until_menu(args.timeout):
            fault("no menu after the help row")
            raise StopIteration
        rows = host.rows()
        step("menu 2: " + "; ".join(f"[{i}] {intent}: {t}" for i, intent, t in rows))
        accept_row = next((r for r, intent, _ in rows if intent == "accept"), None)
        if accept_row is None:
            fault("no accept row after the offer")
            raise StopIteration
        host.ask(f"pick {accept_row}")
        closed = host.until_closed(args.timeout)
        if closed is None:
            fault("the conversation did not close after the accepting row")
            raise StopIteration
        state = host.fields(f"state {qid}")
        if state.get("accepted") != "true":
            fault(f"not accepted in the store after the pick: {state}")
        if f"{qid}:accept" not in host.ports:
            fault(f"the accept port never fired; ports {host.ports}")
        log = host.log_text()
        for needle in (f"BRIDGE quest {qid} accept --", "BRIDGE quest armed code=", "BRIDGE quest beat code=",
                       "BRIDGE picked 0 end -- (the app closed the conversation)"):
            if needle not in log:
                fault(f"missing from the log: {needle!r}")
        step("accepted and closed by the app")

        # The holder's Talk.
        if host.ask(f"look {holder}") != "talk":
            fault("the holder did not offer the game's own Talk once the errand was open")
        host.ask(f"holder {qid} {args.how}")
        if host.ask(f"count player {item}") != "1":
            fault("the thing did not reach the player's pocket")
        step(f"the holder gave in ({args.how})")

        # Save, quit, relaunch, load.
        save = base / "save.lua"
        host.ask(f"save {save}")
        host.quit()
        host = ModHost(args.mod, root, user, echo=args.echo)
        host.ask(f"soul {giver} 1 0 0")
        host.ask(f"soul {holder} 2 0 0")
        host.ask(f"give player {item} 1")
        host.ask(f"load {save}")
        state = host.fields(f"state {qid}")
        if state.get("handed") != "true":
            fault(f"the save did not bring the handover back: {state}")
        if "BRIDGE state loaded via OnLoad version=1" not in host.log_text():
            fault("the load did not log the store coming back")
        step("saved, relaunched, loaded")

        # Conversation 2: have=1 at the door.
        purse_before = int(host.ask("count player " + MONEY))
        mark = len(host.log_text())
        host.ask(f"begin {giver}")
        done = host.until(lambda s: host.fields(f"state {qid}").get("done") == "true", args.timeout)
        log = host.log_text()[mark:]
        begin = next((l for l in log.splitlines() if "BRIDGE begin npc=" in l), "")
        # have= is the soul's first favour's; every favour rides in holding=<id>:<n>.
        if f"{qid}:1" not in begin.split(" holding=")[-1].split(" ")[0] and " have=1" not in begin:
            fault(f"the second begin line did not say the player holds the thing: {begin!r}")
        if done is None:
            fault("the errand was not completed on the second visit")
            raise StopIteration
        purse_after = int(host.ask("count player " + MONEY))
        reward = int(quest["reward"])
        if purse_after - purse_before != reward * 10:
            fault(f"the purse moved {purse_after - purse_before} deci-groschen, not {reward * 10}")
        if host.ask(f"count {giver} {item}") != "1" or host.ask(f"count player {item}") != "0":
            fault("the thing did not go home")
        how = {"persuade": "persuaded", "threaten": "threatened", "pay": "paid"}.get(args.how, args.how)
        host.until(lambda s: any(p.startswith(f"{qid}:complete") for p in host.ports), args.timeout)
        if f"{qid}:complete_{how}" not in host.ports:
            fault(f"the complete_{how} port never fired; ports {host.ports}")
        step(f"completed as complete_{how}, {reward} groschen paid")
        if host.until_menu(args.timeout):
            host.ask("leave")
            host.until_closed(args.timeout)
        if host.ask(f"offerable {qid}") != "false open":
            fault("the errand is offerable again after done")
        step("left; the errand is not offered again")
    except StopIteration:
        pass
    except Exception as e:  # noqa: BLE001 - the report is the point
        fault(f"raised {type(e).__name__}: {e}")
    finally:
        output = ""
        if app is not None:
            try:
                app.stdin.write("/quit\n")
                app.stdin.flush()
                app.wait(timeout=10)
            except (BrokenPipeError, OSError, subprocess.TimeoutExpired):
                app.kill()
            output = pump.text() if "pump" in locals() else ""
        log = host.log_text() if host is not None else ""
        if host is not None:
            host.quit()
        if args.keep:
            print(f"  kept {base}")
        else:
            shutil.rmtree(base, ignore_errors=True)

    print()
    if faults:
        print("FAIL")
        for f in faults:
            print(f"  - {f}")
        print("\n--- kcd.log (tail) ---")
        print("\n".join(log.splitlines()[-40:]))
        print("\n--- what the app said ---")
        print(output)
        return 1
    if args.verbose:
        print("--- what the app said ---")
        print(output)
    print(f"PASS: {args.quest} played to done headless in {time.monotonic() - started:.1f}s")
    return 0


# The money class the harness's engine uses (mod/harness/engine.lua, Engine.moneyClass).
MONEY = "5ba5b5c1-money-0000-0000-000000000000"


def clause_for(personas: str, anim: str) -> str | None:
    """The words the generator wrote for one animation name, if it is catchable at all.

    Read from the database for the same reason `written_greeting` is: comparing the app
    against itself would pass whatever it did. Returns None when there is nothing to
    compare -- no animation given, an animation nobody can be caught at like dying in a
    quest, or a database built before the table existed.
    """
    import sqlite3

    if not anim or anim == "none" or not pathlib.Path(personas).exists():
        return None
    with sqlite3.connect(f"file:{personas}?mode=ro", uri=True) as db:
        try:
            row = db.execute(
                "SELECT clause FROM anim_activity WHERE fragment = ? AND issues IS NULL",
                (anim,),
            ).fetchone()
        except sqlite3.OperationalError:
            return None
    return (row[0] or None) if row else None


def written_greeting(personas: str, soul: str, met: int = 0) -> str | None:
    """The line the generator wrote for this soul to say to a stranger (met=0) or to somebody
    they have spoken to before (met=1), if any.

    Read straight from the database rather than taken from the app, so that the check is
    against what was written and not against what the app decided it had written. Returns
    None when nothing was written, and then they are greeted by the model instead and there
    is nothing to compare.
    """
    import sqlite3

    if not pathlib.Path(personas).exists():
        return None
    with sqlite3.connect(f"file:{personas}?mode=ro", uri=True) as db:
        try:
            row = db.execute(
                "SELECT words FROM npc_default_greeting WHERE soul_name = ? AND met = ?",
                (soul, met),
            ).fetchone()
        except sqlite3.OperationalError:
            return None
    return row[0] if row else None


def main() -> int:
    here = pathlib.Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app", type=pathlib.Path, default=here / "app/build/moonshine_app")
    parser.add_argument("--world", type=pathlib.Path, default=here / "setting.txt")
    parser.add_argument("--host", default="http://127.0.0.1:11434",
                        help="where Ollama is")
    parser.add_argument("--model", default="qwen3:4b-instruct")
    parser.add_argument("--ollama", action="store_true",
                        help="ask a running Ollama server rather than loading the model in "
                             "the app, which is the backend that ships")
    parser.add_argument("--gguf", type=pathlib.Path,
                        help="weights for the in-process backend; default is whichever blob "
                             "Ollama already holds for --model")
    parser.add_argument("--soul", default="kkut_additive_man_47",
                        help="the soul id the mod would log for whoever the player met")
    parser.add_argument("--doing", default="LumberJackSawVAR",
                        help="the animation the soul was caught playing, as the engine "
                             "spells it (default %(default)s)")
    parser.add_argument("--blood", type=float, default=0.5,
                        help="how bloody the player is, 0..1. Above the game's low band of "
                             "0.3 the app picks the 'bloody' greeting, which is what this "
                             "checks is said (default %(default)s)")
    parser.add_argument("--personas", default="data/kcd2/npcs.sqlite",
                        help="the database the app reads, checked against what it says")
    parser.add_argument("--say", nargs="*", default=[
        "Good day to you. What is your name?",
        "Who is the circator you speak of?",
    ])
    parser.add_argument("--written-greetings", dest="written_greetings", action="store_true",
                        default=True,
                        help="say the pre-written opening line, which is the app's default; the "
                             "opening line is asserted to be one of the written ones.")
    parser.add_argument("--generated-greetings", dest="written_greetings", action="store_false",
                        help="run the app with --generated-greetings, so the opening line is "
                             "generated even where one was written, and assert that it was.")
    parser.add_argument("--canned", action="store_true",
                        help="placeholder turns instead of a model; needs nothing installed")
    parser.add_argument("--warmup", type=float, default=90.0,
                        help="how long to allow for prefill and the opening line")
    parser.add_argument("--timeout", type=float, default=60.0)
    parser.add_argument("--pick", action="store_true",
                        help="answer each line by picking a row of the menu the app offers, "
                             "as the game's list would, instead of typing --say; and require "
                             "that the rows are there to pick")
    parser.add_argument("--press-after", type=float, default=None, metavar="SECONDS",
                        help="press this long after the look instead of waiting for the guess, "
                             "and require that the press finds the prefix the guess had built "
                             "(the player quicker than the model)")
    parser.add_argument("--settle", type=float, default=4.0,
                        help="seconds without a new turn file that mean the menu is complete")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--verbose", action="store_true",
                        help="print what the app said even when the run passed")
    parser.add_argument("--mod", type=pathlib.Path, metavar="LUA",
                        help="host the emitted moonshine_names.lua headless (mod/harness/) "
                             "instead of writing the markers here, and play a whole errand against "
                             "the real app, canned: scripts/mod_harness.sh --keep leaves one under "
                             "/tmp/mn/modpak/Scripts/mods/moonshine_names.lua")
    parser.add_argument("--quest", default="Charity Begins at Home",
                        help="with --mod, the errand to play, by id or name (default %(default)s)")
    parser.add_argument("--how", default="persuade", choices=["persuade", "threaten", "pay"],
                        help="with --mod, how the holder's own menu ends (default %(default)s)")
    parser.add_argument("--echo", action="store_true",
                        help="with --mod, print the hosted mod's log as it goes")
    parser.add_argument("--keep", action="store_true",
                        help="with --mod, leave the game and user trees and the save behind")
    args = parser.parse_args()
    if args.canned or args.mod:
        args.warmup = min(args.warmup, 20.0)
        args.settle = min(args.settle, 1.0)

    if not args.app.exists():
        print(f"no app at {args.app}; build it first", file=sys.stderr)
        return 2
    if args.mod:
        if not args.mod.exists():
            print(f"no emitted Lua at {args.mod}; scripts/mod_harness.sh --keep writes one", file=sys.stderr)
            return 2
        if shutil.which("luajit") is None:
            print("luajit is not installed (brew install luajit)", file=sys.stderr)
            return 2
        return run_mod(args)
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
