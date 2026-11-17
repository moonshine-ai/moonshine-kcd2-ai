// A walk through the real Moonshine window, by a script instead of a hand.
//
//   MOONSHINE_UI_SCRIPT=scripts/gui-walk.js MOONSHINE_HOME=<installation> app/build/Moonshine 2> walk.txt
//   (on Windows, where a program with a window has no stderr: set MOONSHINE_UI_TRACE=walk.txt)
//
// The window runs this in its page once the page has loaded (app/gui/main_gui.cpp). It
// presses what a player presses and reports what the page then shows, a line of JSON
// each, through moonshineCall('trace', ...), which the window prints to stderr. It checks
// nothing itself: the lines are read by whoever ran it. What it is for is the part the
// unit tests cannot reach -- that the page, the bridge and the real engine agree.
//
// The walk: wait for the check, press Play, wait until the engine is up, look at the
// logs and the About screen, press Stop, wait for it to have stopped, close the window.

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const text = (id) => document.getElementById(id).textContent;
const shown = (id) => !document.getElementById(id).hidden;
const say = (what, more) => moonshineCall('trace', Object.assign({ what: what }, more || {}));
const home = () => ({
  tone: document.getElementById('lamp').dataset.tone,
  title: text('home-title'),
  detail: text('home-detail'),
  button: shown('home-button') ? text('home-button') : '',
  fix: shown('home-fix') ? text('home-fix') : '',
  pips: [...document.querySelectorAll('.pip')].map((pip) => pip.dataset.on + ':' + pip.textContent.trim()),
});

async function until(what, reached, seconds) {
  let last = '';
  for (let waited = 0; waited < seconds * 4; waited += 1) {
    const now = home();
    if (JSON.stringify(now) !== last) {
      last = JSON.stringify(now);
      say(what, now);
    }
    if (reached(now)) return true;
    await sleep(250);
  }
  say(what + ': gave up waiting', home());
  return false;
}

(async () => {
  try {
    await until('opening', (h) => h.title !== 'Checking the installation', 60);
    if (text('home-button') !== 'Play') {
      say('not ready, so nothing to play');
    } else {
      document.getElementById('home-button').click();
      const up = await until('starting', (h) => h.tone === 'live' || h.tone === 'problem', 300);

      moonshine.show('logs');
      await sleep(2500);
      const log = text('log');
      say('logs', { characters: log.length, end: log.slice(-400) });
      document.querySelector('[data-log="launcher"]').click();
      await sleep(1500);
      say('launcher log', { end: text('log').slice(-400) });

      moonshine.show('about');
      await sleep(300);
      say('about', {
        version: text('about-version'),
        update: text('update-line'),
        items: [...document.querySelectorAll('#checklist li')].map((row) => row.dataset.state + ' ' + row.querySelector('b').textContent),
      });

      moonshine.show('home');
      await sleep(300);
      if (up && text('home-button') === 'Stop') {
        document.getElementById('home-button').click();
        await until('stopping', (h) => h.button === 'Play', 60);
      }
    }
  } catch (error) {
    say('the walk failed', { error: String(error && error.stack || error) });
  }
  await sleep(500);
  moonshineCall('quit', {});
})();
