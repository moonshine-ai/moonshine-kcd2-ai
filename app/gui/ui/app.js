// Moonshine's window: draws the state it is given and makes the call a button names.
//
// It decides nothing. Every sentence on the home screen, and which button goes with it,
// comes from the app (app/gui/bridge.cpp, home_view); what is here is the mapping from
// that state to elements, the settings form, and the log view's reading of a file a
// piece at a time.
//
// The app is reached through one function, moonshineCall(method, args), which the native
// window binds and which answers with a promise; the app in turn calls
// window.moonshine.push(state) whenever the state changes. Opened in a browser there is
// no such function, and mock.js stands in for the app (see the top of that file).

(function () {
  'use strict';

  const $ = (id) => document.getElementById(id);

  function call(method, args) {
    const through = window.moonshineCall || window.moonshineMock;
    if (!through) return Promise.reject(new Error('no app to call'));
    return through(method, args || {});
  }

  let state = null;
  let view = 'home';

  // --- moving between the four screens ---------------------------------------------------

  function show(name) {
    view = name;
    for (const section of document.querySelectorAll('.view')) section.hidden = section.id !== 'view-' + name;
    for (const tab of document.querySelectorAll('.tab')) {
      if (tab.dataset.view === name) tab.setAttribute('aria-current', 'page');
      else tab.removeAttribute('aria-current');
    }
    if (name === 'logs') startLog(); else stopLog();
    if (name === 'settings' && state && !dirty()) fillSettings(state);
  }

  document.addEventListener('click', (event) => {
    const target = event.target.closest('[data-view], [data-call]');
    if (!target) return;
    if (target.dataset.view) show(target.dataset.view);
    else call(target.dataset.call, target.dataset.what ? { what: target.dataset.what } : {});
  });

  // --- the question box ------------------------------------------------------------------

  function ask(question) {
    const box = $('confirm');
    $('confirm-title').textContent = question.title;
    $('confirm-text').textContent = question.text || '';
    $('confirm-yes').textContent = question.yes || 'OK';
    $('confirm-no').textContent = question.no || 'Cancel';
    $('confirm-option').hidden = !question.option;
    $('confirm-option-text').textContent = question.option || '';
    $('confirm-check').checked = false;
    return new Promise((resolve) => {
      box.addEventListener('close', () => {
        resolve({ yes: box.returnValue === 'yes', checked: $('confirm-check').checked });
      }, { once: true });
      box.returnValue = 'no';
      box.showModal();
    });
  }

  // --- home --------------------------------------------------------------------------------

  function setNotice(element, text, kind) {
    element.hidden = !text;
    element.dataset.kind = kind || '';
    element.querySelector('span').textContent = text || '';
  }

  function pip(element, on, label) {
    element.dataset.on = on;
    element.querySelector('span').textContent = label;
  }

  function renderHome(s) {
    const home = s.home;
    $('lamp').dataset.tone = home.tone;
    $('home-title').textContent = home.title;
    $('home-detail').textContent = home.detail;

    const task = s.task;
    const downloading = task.running && task.total > 0;
    $('home-progress').hidden = !(downloading && (task.kind === 'update' || task.kind === 'repair'));
    $('home-progress').firstElementChild.style.width = downloading ? (100 * task.done / task.total).toFixed(1) + '%' : '0';

    const button = $('home-button');
    button.hidden = !home.button;
    button.textContent = home.button;
    button.dataset.action = home.action;
    // Play is the invitation, whether it starts everything or only the game again; Stop
    // and Cancel are there when wanted and do not shout.
    button.classList.toggle('plain', home.action !== 'play' && home.action !== 'startGame');

    const fix = $('home-fix');
    fix.hidden = !home.fix_label;
    fix.textContent = home.fix_label;
    fix.dataset.fix = home.fix;
    // When it is the only thing to do, it is the main button.
    fix.classList.toggle('primary', !home.button);
    fix.classList.toggle('quiet', !!home.button);

    const ended = !task.running && (task.error || task.result);
    const about = ended && (task.kind === 'update' || task.kind === 'repair' || task.kind === 'uninstall');
    // A bug report can be sent from here too (the button in the corner), so how it is
    // going and how it went are said here as well as on the About screen.
    const report = task.kind === 'report';
    const reporting = report && task.running;
    let said = about || (report && ended) ? (task.error || task.result) : '';
    if (reporting) said = task.title + (task.detail ? ': ' + task.detail : '\u2026');
    setNotice($('home-notice'), said, task.error ? 'error' : '');
    $('home-notice-cancel').hidden = !reporting;
    $('home-notice-show').hidden = !(report && ended && task.path);
    $('home-notice-dismiss').hidden = reporting;
    setNotice($('about-notice'), about ? (task.error || task.result) : '', task.error ? 'error' : '');
    const collected = task.kind === 'collect';
    setNotice($('logs-notice'), collected ? (task.running ? 'Collecting the logs…' : (task.error || task.result)) : '',
              task.error ? 'error' : '');
    $('logs-show').hidden = !(collected && task.path);
    $('log-collect').disabled = task.running;

    // The three things a player might wonder about, as dots.
    const engine = s.engine;
    const up = engine.state === 'running' && (engine.phase === 'ready' || engine.phase === 'talking');
    if (!s.settings.mic) pip($('pip-mic'), 'no', 'Microphone off');
    else if (up && !engine.ear) pip($('pip-mic'), 'warn', 'Microphone not available');
    else pip($('pip-mic'), up ? 'yes' : 'no', 'Microphone');
    if (up && !engine.voice) pip($('pip-voice'), 'warn', 'Voices not available');
    else pip($('pip-voice'), up ? 'yes' : 'no', 'Voices');
    pip($('pip-model'), up ? 'yes' : 'no', s.settings.api ? 'Model: ' + (s.settings.api_model || 'custom API') : 'Model on this PC');
    $('pip-model').title = engine.model || '';

    const available = s.update.state === 'available';
    $('update-link').hidden = !available;
    $('update-link').textContent = available ? 'Update to ' + s.update.latest : '';
    $('about-badge').hidden = !available;
  }

  $('home-button').addEventListener('click', () => {
    const action = $('home-button').dataset.action;
    if (action) call(action, {});
  });

  $('home-fix').addEventListener('click', async () => {
    const fix = $('home-fix').dataset.fix;
    if (fix === 'repair') call('repair', {});
    else if (fix === 'reinstall') call('applyUpdate', { reinstall: true });
    else if (fix === 'settings') show('settings');
    else if (fix === 'logs') show('logs');
    else if (fix === 'stop') call('stop', {});
    else if (fix === 'folder') {
      const picked = await call('chooseFolder', {});
      if (picked && picked.path) call('saveSettings', { root: picked.path });
    }
  });

  // --- logs --------------------------------------------------------------------------------

  let logWhich = 'app';
  let logNext = -1;
  let logTimer = null;
  let logBusy = false;
  const kLogKept = 400000;

  async function readLog() {
    if (logBusy) return;
    logBusy = true;
    try {
      const which = logWhich;
      const piece = await call('readLog', { which: which, from: logNext });
      if (which !== logWhich || view !== 'logs') return;
      const log = $('log');
      const atEnd = log.scrollTop + log.clientHeight >= log.scrollHeight - 24;
      if (piece.reset) log.textContent = '';
      if (piece.text) {
        log.append(piece.text);
        // An evening's log is megabytes; what is kept on screen is its end.
        if (log.textContent.length > kLogKept * 1.5) {
          const text = log.textContent;
          log.textContent = text.slice(text.indexOf('\n', text.length - kLogKept) + 1);
        }
        log.normalize();
      }
      logNext = piece.next;
      if (atEnd || piece.reset) log.scrollTop = log.scrollHeight;
    } catch (error) {
      // The next tick tries again.
    } finally {
      logBusy = false;
    }
  }

  function startLog() {
    stopLog();
    logNext = -1;
    $('log').textContent = '';
    readLog();
    logTimer = setInterval(readLog, 800);
  }

  function stopLog() {
    if (logTimer) clearInterval(logTimer);
    logTimer = null;
  }

  for (const button of document.querySelectorAll('[data-log]')) {
    button.addEventListener('click', () => {
      logWhich = button.dataset.log;
      for (const other of document.querySelectorAll('[data-log]')) {
        other.setAttribute('aria-selected', other === button ? 'true' : 'false');
      }
      startLog();
    });
  }

  $('log-copy').addEventListener('click', async () => {
    const text = $('log').textContent;
    let done = false;
    try {
      await navigator.clipboard.writeText(text);
      done = true;
    } catch (error) {
      // A page that was handed over as a string is not always allowed the clipboard this
      // way; selecting and copying is.
      const range = document.createRange();
      range.selectNodeContents($('log'));
      const selection = window.getSelection();
      selection.removeAllRanges();
      selection.addRange(range);
      done = document.execCommand('copy');
      selection.removeAllRanges();
    }
    $('log-copy').textContent = done ? 'Copied' : 'Could not copy';
    setTimeout(() => { $('log-copy').textContent = 'Copy'; }, 1500);
  });

  $('log-collect').addEventListener('click', () => call('collectLogs', {}));
  $('logs-show').addEventListener('click', () => call('open', { what: 'task' }));

  // --- settings ----------------------------------------------------------------------------

  // What is in the form, in the shape the app keeps settings in.
  function formSettings() {
    return {
      root: $('set-root').value.trim(),
      launch_game: $('set-launch').checked,
      autostart: $('set-autostart').checked,
      mic: $('set-mic').checked,
      api: $('set-api').checked,
      api_url: $('set-api-url').value.trim(),
      api_model: $('set-api-model').value.trim(),
      api_extra: $('set-api-extra').value.trim(),
      api_guessing: $('set-api-guessing').checked,
      args: $('set-args').value.trim(),
    };
  }

  let removeKey = false;

  function dirty() {
    if (!state) return false;
    if ($('set-api-key').value !== '' || removeKey) return true;
    const form = formSettings();
    return Object.keys(form).some((key) => form[key] !== state.settings[key]);
  }

  function fillSettings(s) {
    const set = s.settings;
    $('set-root').value = set.root;
    $('set-launch').checked = set.launch_game;
    $('set-autostart').checked = set.autostart;
    $('set-mic').checked = set.mic;
    $('set-local').checked = !set.api;
    $('set-api').checked = set.api;
    $('set-api-url').value = set.api_url;
    $('set-api-model').value = set.api_model;
    $('set-api-extra').value = set.api_extra;
    $('set-api-guessing').checked = set.api_guessing;
    $('set-args').value = set.args;
    $('set-api-key').value = '';
    removeKey = false;
    refreshSettings();
  }

  // The parts of the form that follow from the rest of it.
  function refreshSettings() {
    if (!state) return;
    $('api-panel').hidden = !$('set-api').checked;
    const found = state.check.game_root;
    $('root-found').textContent = $('set-root').value.trim() ? ''
      : found ? 'Found at ' + found : 'Not found yet. Choose the folder the game is installed in.';
    const kept = state.has_key && !removeKey;
    $('set-api-key').placeholder = removeKey ? 'Removed when you save' : kept ? 'A key is saved. Type to replace it.' : '';
    $('key-remove').hidden = !kept;
    const changed = dirty();
    $('save').disabled = !changed;
    $('revert').hidden = !changed;
    if (changed) setResult($('save-result'), '', '');
  }

  function setResult(element, text, kind) {
    element.textContent = text;
    element.dataset.kind = kind;
  }

  $('settings').addEventListener('input', refreshSettings);
  $('settings').addEventListener('change', refreshSettings);

  $('key-remove').addEventListener('click', () => {
    removeKey = true;
    $('set-api-key').value = '';
    refreshSettings();
  });

  $('revert').addEventListener('click', () => {
    fillSettings(state);
    setResult($('save-result'), '', '');
    setResult($('api-result'), '', '');
  });

  $('choose-folder').addEventListener('click', async () => {
    const picked = await call('chooseFolder', {});
    if (picked && picked.path) {
      $('set-root').value = picked.path;
      refreshSettings();
    }
  });

  // The key as the app should be told it: what was typed, an empty one to remove it, or
  // nothing at all to leave what is kept alone.
  function keyArgument() {
    const typed = $('set-api-key').value.trim();
    if (typed) return { key: typed };
    return removeKey ? { key: '' } : {};
  }

  $('settings').addEventListener('submit', async (event) => {
    event.preventDefault();
    if (!dirty()) return;
    $('save').disabled = true;
    const answer = await call('saveSettings', Object.assign(formSettings(), keyArgument()));
    if (answer.ok) {
      $('set-api-key').value = '';
      removeKey = false;
      // The state that follows carries the saved settings; until it arrives the form is
      // what was saved.
      if (state) Object.assign(state.settings, formSettings());
      refreshSettings();
      setResult($('save-result'), answer.restarted ? 'Saved. Moonshine is starting again with the new settings.' : 'Saved.', 'ok');
    } else {
      setResult($('save-result'), answer.error || 'The settings were not saved.', 'error');
      $('save').disabled = false;
    }
  });

  $('api-test').addEventListener('click', async () => {
    const button = $('api-test');
    button.disabled = true;
    setResult($('api-result'), 'Asking it a question…', '');
    let answer;
    try {
      answer = await call('testApi', Object.assign(formSettings(), keyArgument()));
    } catch (error) {
      answer = { ok: false, error: 'The test could not be run.' };
    }
    button.disabled = false;
    if (answer.ok) {
      let text = 'Connected. ' + (answer.model || 'The model') + ' answered in ' + Number(answer.seconds || 0).toFixed(1) + ' s.';
      if (answer.reasons) text += ' It reasons before it answers, which makes replies slower; the extra request JSON can turn that down if the service allows.';
      setResult($('api-result'), text, 'ok');
    } else {
      setResult($('api-result'), answer.error || 'It did not answer.', 'error');
    }
  });

  // --- about -------------------------------------------------------------------------------

  function renderAbout(s) {
    $('about-version').textContent = s.version;
    const update = s.update;
    const lines = {
      unknown: '',
      checking: 'Looking for a newer version…',
      current: 'This is the newest version.',
      available: 'Version ' + update.latest + ' is available.',
      failed: update.error,
    };
    $('update-line').textContent = lines[update.state] || '';
    $('update-line').hidden = !$('update-line').textContent;
    const button = $('update-button');
    button.textContent = update.state === 'available' ? 'Update to ' + update.latest : 'Check for updates';
    button.dataset.call = update.state === 'available' ? 'applyUpdate' : 'checkUpdate';
    button.disabled = s.task.running || update.state === 'checking';
    $('repair-button').disabled = s.task.running;
    $('uninstall-button').disabled = s.task.running;

    const task = s.task;
    const working = task.running && (task.kind === 'update' || task.kind === 'repair');
    $('about-task').hidden = !working;
    $('task-title').textContent = task.title;
    $('task-detail').textContent = task.detail;
    $('task-bar').style.width = task.total > 0 ? (100 * task.done / task.total).toFixed(1) + '%' : '0';

    const list = $('checklist');
    const wanted = s.check.items.filter((item) => item.name);
    // Rebuilt only when it differs: it is selectable text, and a selection should survive
    // a state that changed nothing here.
    const key = JSON.stringify(wanted) + s.check.state + s.check.error;
    if (list.dataset.key !== key) {
      list.dataset.key = key;
      list.textContent = '';
      if (s.check.state === 'failed') {
        const row = document.createElement('li');
        row.dataset.state = 'missing';
        row.append(document.createElement('i'), Object.assign(document.createElement('b'), { textContent: 'app' }),
                   Object.assign(document.createElement('span'), { textContent: s.check.error }));
        list.append(row);
      }
      // The app's own names for two of its lines are a programmer's.
      const plainer = { 'cwd': 'installed in', '%USER%': 'saved games' };
      for (const item of wanted) {
        const row = document.createElement('li');
        row.dataset.state = item.state;
        row.append(document.createElement('i'),
                   Object.assign(document.createElement('b'), { textContent: plainer[item.name] || item.name }),
                   Object.assign(document.createElement('span'), { textContent: item.text }));
        list.append(row);
      }
    }
  }

  $('repair-button').addEventListener('click', () => call('repair', {}));

  // --- a bug report ------------------------------------------------------------------------

  // What was written stays in the box until a report with it has arrived: one that did not
  // go can be sent again without typing it again.
  let reportWritten = false;

  function renderReport(s) {
    const task = s.task;
    const mine = task.kind === 'report';
    $('report-button').disabled = task.running;
    $('home-report').disabled = task.running;
    $('report-task').hidden = !(mine && task.running);
    $('report-task-title').textContent = task.title;
    $('report-task-detail').textContent = task.detail;
    $('report-task-bar').style.width = task.total > 0 ? (100 * task.done / task.total).toFixed(1) + '%' : '0';
    const ended = mine && !task.running && (task.error || task.result);
    setNotice($('report-notice'), ended ? (task.error || task.result) : '', task.error ? 'error' : '');
    // The zip, when it is still here: it only is when it could not be sent.
    $('report-show').hidden = !(ended && task.path);
    if (ended && !task.error && !task.path && reportWritten && task.result.indexOf('Sent') === 0) {
      $('report-description').value = '';
      reportWritten = false;
    }
  }

  // The form, from either of its two buttons: the corner of the home screen and About.
  function openReport() {
    if (!state) return;
    const report = state.report || {};
    $('report-name').value = report.name || '';
    $('report-email').value = report.email || '';
    $('report-name').maxLength = report.name_limit || 200;
    $('report-email').maxLength = report.email_limit || 254;
    $('report-description').maxLength = report.description_limit || 4000;
    setResult($('report-result'), '', '');
    $('report-send').disabled = false;
    $('report').showModal();
    $('report-description').focus();
  }

  $('report-button').addEventListener('click', openReport);
  $('home-report').addEventListener('click', openReport);
  $('report-cancel').addEventListener('click', () => $('report').close());
  $('report-show').addEventListener('click', () => call('open', { what: 'task' }));
  $('home-notice-show').addEventListener('click', () => call('open', { what: 'task' }));

  $('report-form').addEventListener('submit', async (event) => {
    event.preventDefault();
    $('report-send').disabled = true;
    let answer;
    try {
      answer = await call('sendReport', {
        name: $('report-name').value.trim(),
        email: $('report-email').value.trim(),
        description: $('report-description').value.trim(),
      });
    } catch (error) {
      answer = { ok: false, error: 'The report could not be started.' };
    }
    if (answer.ok) {
      reportWritten = true;
      $('report').close();
    } else {
      setResult($('report-result'), answer.error || 'The report could not be started.', 'error');
      $('report-send').disabled = false;
    }
  });

  $('uninstall-button').addEventListener('click', async () => {
    const answer = await ask({
      title: 'Uninstall Moonshine?',
      text: 'This removes Moonshine, its models and its voices from this PC. The game and your saves are not touched.',
      option: 'Also remove the Moonshine mod from the game\u2019s mods folder',
      yes: 'Uninstall',
    });
    if (!answer.yes) return;
    const result = await call('uninstall', { remove_mod: answer.checked });
    if (!result.ok) setNotice($('about-notice'), result.error, 'error');
  });

  // --- the state, and the app's way in -------------------------------------------------------

  function render(s) {
    const first = state === null;
    const touched = !first && dirty();
    state = s;
    renderHome(s);
    renderAbout(s);
    renderReport(s);
    $('key-kept').textContent = s.windows
      ? 'Kept in Windows Credential Manager, not in Moonshine\u2019s files or logs.'
      : 'Kept in a file only you can read (this is a development build).';
    // A form nobody has touched follows the settings (the folder picker on the home
    // screen changes them, and so does a save); one being edited is left alone.
    if (first || (!touched && dirty())) fillSettings(s);
    refreshSettings();
  }

  window.moonshine = {
    push: render,
    // The window's close button, while the characters are listening.
    confirmClose: async function () {
      if ($('confirm').open) return;
      const answer = await ask({
        title: 'Close Moonshine?',
        text: 'The characters stop answering when Moonshine is closed. The game keeps running.',
        yes: 'Close',
        no: 'Keep it open',
      });
      if (answer.yes) call('quit', {});
    },
    show: show,
  };

  // Selecting is for text that is worth copying; a right-click anywhere else is nothing.
  document.addEventListener('contextmenu', (event) => {
    if (!event.target.closest('input, textarea, .log, .plain, .checklist, .notice, .result')) event.preventDefault();
  });

  call('getState', {}).then(render, () => {
    $('home-title').textContent = 'Moonshine could not start';
    $('home-detail').textContent = 'The window opened but the app behind it did not answer.';
    $('lamp').dataset.tone = 'problem';
  });
})();
