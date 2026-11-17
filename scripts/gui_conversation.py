#!/usr/bin/env python3
"""One conversation through Moonshine's window: the window, the engine it starts, and a
fake game on the other side.

The unit tests drive the window's logic with engines that are not there, and
scripts/fake_game.py drives a real engine with no window. This is the two together, which
is what a player has: the real window (app/gui) is opened and told, by a script run in
its own page, to press Play; the engine it starts is then talked to through the game's
files exactly as fake_game.py's FakeGame does it; and what the window showed while that
happened is read back. It passes when

  - the window got from "Ready to play" to the characters listening,
  - the engine it started greeted, and answered a typed line, through the turn files,
  - the window said "Talking with <somebody>" while it did, and went back afterwards,
  - Stop stopped the engine and the window closed with exit 0.

    python3 scripts/gui_conversation.py --canned       no model needed, a few seconds
    python3 scripts/gui_conversation.py                the model in models/llm

--home names a folder laid out as an installation (moonshine_app, models, data, setting.txt
beside each other); its moonshine.cfg is rewritten to point at the fake game, so it should
not be one somebody plays from. Without it a throwaway one is made from this checkout out
of symbolic links, which works on the Mac and Linux; on Windows pass --home.

The engine is started with --no-voice: a test that speaks aloud on somebody's desk is a
surprise.
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from fake_game import FakeGame  # noqa: E402

# Run by the window in its page (MOONSHINE_UI_SCRIPT). It presses Play, reports each change
# of the home screen, waits for a conversation to come and go, presses Stop and closes.
PAGE_SCRIPT = r"""
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const el = (id) => document.getElementById(id);
const say = (what, more) => moonshineCall('trace', Object.assign({ what: what }, more || {}));
const home = () => ({ tone: el('lamp').dataset.tone, title: el('home-title').textContent, detail: el('home-detail').textContent,
                      button: el('home-button').hidden ? '' : el('home-button').textContent });
const talking = (h) => h.title.indexOf('Talking with') === 0 || h.title === 'In conversation';
async function until(what, reached, seconds) {
  let last = '';
  for (let waited = 0; waited < seconds * 10; waited += 1) {
    const now = home();
    if (JSON.stringify(now) !== last) { last = JSON.stringify(now); say(what, now); }
    if (reached(now)) return true;
    await sleep(100);
  }
  say(what + ': gave up waiting', home());
  return false;
}
(async () => {
  try {
    await until('opening', (h) => h.title !== 'Checking the installation', 60);
    if (home().button === 'Play') {
      el('home-button').click();
      if (await until('starting', (h) => h.tone === 'live' || h.tone === 'problem', 600)) {
        if (await until('listening', talking, 300)) await until('talking', (h) => !talking(h), 300);
      }
      if (home().button === 'Stop') {
        el('home-button').click();
        await until('stopping', (h) => h.button === 'Play', 60);
      }
    }
  } catch (error) {
    say('the page script failed', { error: String(error && error.stack || error) });
  }
  await sleep(300);
  moonshineCall('quit', {});
})();
"""


def throwaway_home(checkout: pathlib.Path, engine: pathlib.Path, base: pathlib.Path) -> pathlib.Path:
    home = base / "home"
    home.mkdir()
    for name in ("models", "data", "setting.txt", "VERSION"):
        os.symlink(checkout / name, home / name)
    os.symlink(engine, home / engine.name)
    return home


def main() -> int:
    here = pathlib.Path(__file__).resolve().parent.parent
    exe = ".exe" if os.name == "nt" else ""
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--gui", type=pathlib.Path, default=here / f"app/build/Moonshine{exe}")
    parser.add_argument("--home", type=pathlib.Path, help="an installation to run from; its moonshine.cfg is rewritten")
    parser.add_argument("--canned", action="store_true", help="the engine's placeholder turns, no model")
    parser.add_argument("--soul", default="kkut_additive_man_47")
    parser.add_argument("--say", default="God be with you. Have you a bed for the night?")
    parser.add_argument("--warmup", type=float, default=180.0, help="seconds to allow the engine to come up")
    parser.add_argument("--timeout", type=float, default=90.0, help="seconds to allow each line")
    args = parser.parse_args()

    base = pathlib.Path(tempfile.mkdtemp(prefix="moonshine-gui-"))
    game = FakeGame(base)
    if args.home:
        home = args.home.resolve()
    elif os.name == "nt":
        parser.error("on Windows, pass --home: a folder laid out as an installation")
    else:
        home = throwaway_home(here, (args.gui.parent / "moonshine_app").resolve(), base)

    engine_args = f"--user {game.user} --no-voice" + (" --canned" if args.canned else "")
    (home / "moonshine.cfg").write_text(
        f"root = {game.root}\nargs = {engine_args}\nlaunch_game = off\nmic = off\n", encoding="utf-8")
    script = base / "page.js"
    script.write_text(PAGE_SCRIPT, encoding="utf-8")
    trace = base / "trace.txt"
    status = home / "logs" / "status.json"
    if status.exists():
        status.unlink()

    env = dict(os.environ, MOONSHINE_HOME=str(home), MOONSHINE_UI_SCRIPT=str(script), MOONSHINE_UI_TRACE=str(trace))
    window = subprocess.Popen([str(args.gui)], env=env, stderr=subprocess.DEVNULL)
    faults: list[str] = []

    def phase() -> str:
        try:
            return json.loads(status.read_text(encoding="utf-8")).get("phase", "")
        except (OSError, ValueError):
            return ""

    try:
        print(f"waiting up to {args.warmup:.0f}s for the window to start the engine", flush=True)
        deadline = time.monotonic() + args.warmup
        while phase() != "ready" and time.monotonic() < deadline and window.poll() is None:
            time.sleep(0.2)
        if phase() != "ready":
            faults.append(f"the engine never said it was ready (its status says {phase()!r})")
        else:
            game.begin(args.soul)
            greeting = game.play_spoken(args.timeout)
            print(f"  they: {greeting}", flush=True)
            if not greeting:
                faults.append("no greeting came through the turn files")
            game.typed(args.say)
            print(f"  you:  {args.say}", flush=True)
            reply = game.play_spoken(args.timeout)
            print(f"  they: {reply}", flush=True)
            if not reply:
                faults.append("no reply to the typed line came through the turn files")
            # Long enough for the window, which looks a few times a second, to have seen it.
            time.sleep(2.0)
            game.left()
        try:
            code = window.wait(timeout=120)
        except subprocess.TimeoutExpired:
            faults.append("the window did not close")
            code = None
    finally:
        if window.poll() is None:
            window.kill()

    seen = []
    if trace.exists():
        for line in trace.read_text(encoding="utf-8", errors="replace").splitlines():
            try:
                seen.append(json.loads(line))
            except ValueError:
                pass
    print("what the window showed:")
    for step in seen:
        print(f"  {step.get('what', ''):<10} {step.get('title', '')}" + (f" -- {step['detail']}" if step.get("detail") else "")
              + (f"  [{step['button']}]" if step.get("button") else "") + (f"  {step['error']}" if step.get("error") else ""))
    titles = [step.get("title", "") for step in seen]
    if "Ready to play" not in titles:
        faults.append("the window never said it was ready to play")
    if not any(title in ("Listening", "Waiting for the game") for title in titles):
        faults.append("the window never showed the characters listening")
    if not any(title.startswith("Talking with") or title == "In conversation" for title in titles):
        faults.append("the window never showed the conversation")
    if titles and titles[-1] != "Ready to play":
        faults.append(f"the window ended on {titles[-1]!r}, not back at Ready to play")
    if code not in (0, None):
        faults.append(f"the window exited {code}")
    launcher = home / "logs" / "moonshine.log"
    if launcher.exists():
        last = launcher.read_text(encoding="utf-8", errors="replace").strip().splitlines()[-1:]
        if not last or "app stopped (exit 0)" not in last[0]:
            faults.append(f"the launcher's log does not end with a clean stop: {last}")

    if faults:
        print("\nFAILED:")
        for fault in faults:
            print("  " + fault)
        print(f"\n(kept: {base})")
        return 1
    print("\nok: the window started the engine, a conversation went through it, and Stop stopped it")
    return 0


if __name__ == "__main__":
    sys.exit(main())
