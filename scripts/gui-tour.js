// The real Moonshine window, shown a screen at a time for pictures to be taken of it.
//
//   MOONSHINE_UI_SCRIPT=scripts/gui-tour.js MOONSHINE_HOME=<installation> app/build/Moonshine --no-play
//
// Like gui-walk.js, run by the window in its own page. It presses nothing that starts or
// changes anything: it waits on each screen for a few seconds, says which it is on
// (moonshineCall('trace', ...)), and closes the window at the end.

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));
const hold = 6000;

(async () => {
  for (let waited = 0; waited < 240 && document.getElementById('home-title').textContent === 'Checking the installation'; waited += 1) {
    await sleep(250);
  }
  for (const screen of ['home', 'settings', 'about', 'logs']) {
    moonshine.show(screen);
    if (screen === 'settings') {
      // The custom API's part of the form, opened but not saved.
      document.getElementById('set-api').click();
    }
    moonshineCall('trace', { screen: screen, at: Date.now() });
    await sleep(hold);
    if (screen === 'settings') document.getElementById('revert').click();
  }
  moonshineCall('quit', {});
})();
