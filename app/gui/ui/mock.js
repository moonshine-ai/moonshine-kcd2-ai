// A stand-in for the app, for working on the interface in a browser.
//
//   python3 -m http.server -d app/gui/ui 8377      then open http://localhost:8377/?scene=ready
//
// Not shipped: the line that loads this file is marked data-dev in index.html and is taken
// out when the page is put into the program (app/gui/page.cpp). When the native window
// has bound moonshineCall this file does nothing.
//
// ?scene= names one of the states below; the box in the corner switches between them, and
// Play walks through a start the way the real one goes. ?tab=logs (settings, about) opens
// on that screen, and ?plain leaves the corner box out, for a picture of the page.

(function () {
  'use strict';
  if (window.moonshineCall) return;

  const items = [
    { name: 'game root', text: 'C:\\Program Files (x86)\\Steam\\steamapps\\common\\KingdomComeDeliverance2', state: 'ok' },
    { name: 'log', text: 'kcd.log (not there yet)', state: 'ok' },
    { name: 'world', text: 'setting.txt (0 MB)', state: 'ok' },
    { name: 'personas', text: 'data\\kcd2\\npcs.sqlite (328 MB)', state: 'ok' },
    { name: 'model', text: 'models\\llm\\Qwen3-4B-Instruct-2507-Q4_K_M.gguf (2381 MB)', state: 'ok' },
    { name: 'embedder', text: 'models\\embeddinggemma-300m-q8_0.gguf (333 MB)', state: 'ok' },
    { name: 'voice', text: 'models\\phonon, Phonon through Moonshine Voice on the CPU', state: 'ok' },
    { name: 'voices', text: 'models\\phonon: 37 voice(s), 8 imported', state: 'ok' },
    { name: 'ear', text: 'models\\stt\\tiny-streaming-en', state: 'ok' },
    { name: 'mod', text: 'mods\\moonshine_names (0.9.102.28)', state: 'ok' },
  ];

  function base() {
    return {
      version: '0.9.102.28',
      folder: 'C:\\Users\\henry\\AppData\\Local\\Moonshine',
      windows: true,
      home: { tone: 'idle', title: 'Ready to play', detail: 'Starts Kingdom Come: Deliverance 2 with the people of Bohemia listening.',
              button: 'Play', action: 'play', fix_label: '', fix: '' },
      engine: { state: 'idle', message: '', restarts: 0, phase: '', talking_to: '', model: '', remote_model: false,
                voice: false, ear: false, game_log: false, error: '' },
      check: { state: 'done', ready: true, game_root: items[0].text, error: '', items: JSON.parse(JSON.stringify(items)) },
      settings: { root: '', args: '', mic: true, launch_game: true, autostart: true, api: false, api_url: '',
                  api_model: '', api_extra: '', api_guessing: false },
      has_key: false,
      task: { kind: '', running: false, title: '', detail: '', done: 0, total: 0, error: '', result: '', path: '' },
      update: { state: 'current', latest: '0.9.102.28', error: '' },
      report: { name: '', email: '', name_limit: 200, email_limit: 254, description_limit: 4000 },
    };
  }

  function running(phase, home, more) {
    const s = base();
    s.engine = Object.assign(s.engine, { state: 'running', phase: phase, model: 'Qwen3-4B-Instruct-2507-Q4_K_M.gguf (llama.cpp)',
                                         voice: true, ear: true, game_log: true }, more || {});
    s.home = Object.assign({ tone: 'live', title: '', detail: '', button: 'Stop', action: 'stop', fix_label: '', fix: '' }, home);
    return s;
  }

  const scenes = {
    ready: base,
    checking: () => Object.assign(base(), { home: { tone: 'busy', title: 'Checking the installation', detail: '', button: '', action: '', fix_label: '', fix: '' },
                                            check: { state: 'checking', ready: false, game_root: '', error: '', items: [] } }),
    loading: () => running('loading-model', { tone: 'busy', title: 'Starting', detail: 'Loading the language model' }, { voice: false, ear: false }),
    warming: () => running('warming', { tone: 'busy', title: 'Starting', detail: 'Getting the voices ready' }),
    waiting: () => running('ready', { title: 'Waiting for the game', detail: 'Moonshine is ready and joins in when the game is up.' }, { game_log: false }),
    listening: () => running('ready', { title: 'Listening', detail: 'Walk up to someone in the game and talk to them.' }),
    talking: () => running('talking', { title: 'Talking with Innkeeper Betty' }, { talking_to: 'Innkeeper Betty' }),
    // The game was quit and Moonshine left running: Play starts the game again.
    gone: () => running('ready', { title: 'Waiting for the game',
                                   detail: 'Kingdom Come: Deliverance 2 is not running. Press Play to start it; Moonshine is ready and joins in.',
                                   button: 'Play', action: 'startGame', fix_label: 'Stop', fix: 'stop' }, { game: 'down' }),
    gamestarting: () => running('ready', { tone: 'busy', title: 'Starting the game',
                                           detail: 'Moonshine is ready and joins in when Kingdom Come: Deliverance 2 is up.' }, { game: 'starting' }),
    nogame: () => {
      const s = base();
      s.check.ready = false; s.check.game_root = '';
      s.check.items[0] = { name: 'game root', text: 'not found under Steam\u2019s library folders', state: 'missing' };
      s.home = { tone: 'problem', title: 'Kingdom Come: Deliverance 2 was not found', detail: 'Choose the folder the game is installed in.',
                 button: '', action: '', fix_label: 'Choose game folder', fix: 'folder' };
      return s;
    },
    missing: () => {
      const s = base();
      s.check.ready = false;
      s.check.items[4] = { name: 'model', text: 'models\\llm has no .gguf', state: 'missing' };
      s.home = { tone: 'problem', title: 'Some of Moonshine\u2019s files are missing', detail: 'Missing: model.',
                 button: '', action: '', fix_label: 'Repair files', fix: 'repair' };
      return s;
    },
    failed: () => {
      const s = base();
      s.engine.state = 'failed'; s.engine.message = 'Moonshine keeps stopping as soon as it starts.';
      s.home = { tone: 'problem', title: 'Moonshine could not start', detail: s.engine.message, button: 'Try again', action: 'play',
                 fix_label: 'View logs', fix: 'logs' };
      return s;
    },
    restarting: () => {
      const s = running('', { tone: 'problem', title: 'Starting again',
                              detail: 'Moonshine stopped unexpectedly and is starting again. Anything that was cut off will need saying again.',
                              fix_label: 'View logs', fix: 'logs' });
      s.engine.state = 'restarting'; s.engine.restarts = 1;
      return s;
    },
    update: () => Object.assign(base(), { update: { state: 'available', latest: '0.9.104', error: '' } }),
    repairing: () => {
      const s = base();
      s.task = { kind: 'repair', running: true, title: 'Repairing files', detail: 'Downloading: 812 of 2,381 MB, 11.4 MB/s',
                 done: 812, total: 2381, error: '', result: '', path: '' };
      s.home = { tone: 'busy', title: s.task.title, detail: s.task.detail, button: 'Cancel', action: 'cancelTask', fix_label: '', fix: '' };
      return s;
    },
    repaired: () => {
      const s = base();
      s.task = { kind: 'repair', running: false, title: 'Repairing files', detail: '', done: 0, total: 0, error: '',
                 result: '1 file was downloaded again. Everything is now in place.', path: '' };
      return s;
    },
    reporting: () => {
      const s = base();
      s.task = { kind: 'report', running: true, title: 'Sending the report', detail: '1.3 of 3.4 MB',
                 done: 13, total: 34, error: '', result: '', path: '' };
      return s;
    },
    reported: () => {
      const s = base();
      s.task = { kind: 'report', running: false, title: 'Sending the report', detail: '', done: 0, total: 0, error: '',
                 result: 'Sent, thank you. Reference AB12CD34.', path: '' };
      return s;
    },
    unreported: () => {
      const s = base();
      s.task = { kind: 'report', running: false, title: 'Sending the report', detail: '', done: 0, total: 0,
                 error: 'The report could not be sent (could not reach the server). The logs are saved on your desktop as Moonshine-logs-20261116-201455.zip. Try again later, or send that file.',
                 result: '', path: 'C:\\Users\\henry\\Desktop\\Moonshine-logs-20261116-201455.zip' };
      return s;
    },
    api: () => {
      const s = running('ready', { title: 'Listening', detail: 'Walk up to someone in the game and talk to them.' },
                        { remote_model: true, model: 'llama-3.3-70b at api.groq.com (API)' });
      s.settings = Object.assign(s.settings, { api: true, api_url: 'https://api.groq.com/openai/v1', api_model: 'llama-3.3-70b-versatile' });
      s.has_key = true;
      return s;
    },
    apidown: () => {
      const s = scenes.api();
      s.engine.error = 'The API did not accept the key. Check it in Settings.';
      s.home = Object.assign(s.home, { tone: 'problem', detail: s.engine.error, fix_label: 'Open Settings', fix: 'settings' });
      return s;
    },
  };

  let current = (new URLSearchParams(location.search).get('scene')) || 'ready';
  let state = (scenes[current] || scenes.ready)();
  let timers = [];

  function push() { if (window.moonshine) window.moonshine.push(JSON.parse(JSON.stringify(state))); }

  function go(name, keep) {
    for (const timer of timers) clearTimeout(timer);
    timers = [];
    const settings = state.settings, hasKey = state.has_key;
    state = (scenes[name] || scenes.ready)();
    if (keep) { state.settings = settings; state.has_key = hasKey; }
    push();
  }

  const logLines = [
    'log:        logs\\moonshine_app.log',
    'model: Qwen3-4B-Instruct-2507-Q4_K_M.gguf, 4096 ctx, 8 threads',
    'world: prefilled 1123 tokens in 3.41s',
    'voice: Phonon, 37 voices',
    'ear:   tiny-streaming-en, default microphone',
    'following the log. Type what the player says here, or in the game once their line is up; /help for the commands.',
    '== Innkeeper Betty (soul 4c1f...) ==',
    '> God be with you. Have you a bed for the night?',
    'Betty: Aye, if you\u2019ve the coin for it. Two groschen, and that buys you a bowl of pottage besides.',
    '[first audio 0.71s, 1338 tok in, 1197 cached, 41 tok out in 1.92s]',
  ];
  let logAt = 0;

  window.moonshineMock = function (method, args) {
    const ok = { ok: true };
    const later = (value, after) => new Promise((resolve) => setTimeout(() => resolve(value), after || 30));
    switch (method) {
      case 'getState': return later(JSON.parse(JSON.stringify(state)));
      case 'play':
        go('checking', true);
        timers.push(setTimeout(() => go('loading', true), 500), setTimeout(() => go('warming', true), 2600),
                    setTimeout(() => go('waiting', true), 4200), setTimeout(() => go('listening', true), 6500),
                    setTimeout(() => go('talking', true), 10000));
        return later(ok);
      case 'stop': go('ready', true); return later(ok);
      case 'startGame':
        go('gamestarting', true);
        timers.push(setTimeout(() => go('listening', true), 3000));
        return later(ok);
      case 'readLog': {
        if (args.from < 0) logAt = 0;
        const line = logLines[logAt % logLines.length];
        logAt += 1;
        const text = args.which === 'launcher' ? '2026-11-16 20:14:0' + (logAt % 10) + '  asking Steam to start Kingdom Come: Deliverance 2\n' : line + '\n';
        return later({ text: text, next: (args.from < 0 ? 0 : args.from) + text.length, reset: args.from < 0, file: 'logs/moonshine_app.log' });
      }
      case 'saveSettings': {
        const next = Object.assign({}, state.settings);
        for (const key of Object.keys(next)) if (key in args) next[key] = args[key];
        if (next.api && !next.api_url) return later({ ok: false, error: 'Enter the API\u2019s address.' });
        state.settings = next;
        if ('key' in args) state.has_key = args.key !== '';
        push();
        return later({ ok: true, restarted: state.engine.state === 'running' });
      }
      case 'testApi':
        return later(args.api_url.indexOf('bad') >= 0 ? { ok: false, error: 'The API did not accept the key. Check it in Settings.' }
                                                      : { ok: true, model: args.api_model, reply: 'ready', seconds: 0.62, reasons: false }, 1200);
      case 'chooseFolder': return later({ ok: true, path: 'D:\\SteamLibrary\\steamapps\\common\\KingdomComeDeliverance2' }, 300);
      case 'repair': go('repairing', true); timers.push(setTimeout(() => go('repaired', true), 4000)); return later(ok);
      case 'cancelTask': go('ready', true); return later(ok);
      case 'dismissTask': state.task = base().task; push(); return later(ok);
      case 'checkUpdate':
        state.update = { state: 'checking', latest: '', error: '' }; push();
        timers.push(setTimeout(() => { state.update = { state: 'available', latest: '0.9.104', error: '' }; push(); }, 1200));
        return later(ok);
      case 'applyUpdate':
        state.task = { kind: 'update', running: true, title: 'Updating Moonshine', detail: 'Downloading Moonshine 0.9.104: 31 of 96 MB, 9.8 MB/s',
                       done: 31, total: 96, error: '', result: '', path: '' };
        state.home = { tone: 'busy', title: state.task.title, detail: state.task.detail, button: 'Cancel', action: 'cancelTask', fix_label: '', fix: '' };
        push();
        return later(ok);
      case 'collectLogs':
        state.task = { kind: 'collect', running: true, title: 'Collecting logs', detail: '', done: 0, total: 0, error: '', result: '', path: '' };
        push();
        timers.push(setTimeout(() => {
          state.task = Object.assign(state.task, { running: false, path: 'C:\\Users\\henry\\Desktop\\Moonshine-logs-20261116-201455.zip',
            result: 'Saved to your desktop as Moonshine-logs-20261116-201455.zip. Send that file with a sentence or two about what you were doing.' });
          push();
        }, 1500));
        return later(ok);
      case 'sendReport': {
        if (state.task.running) return later({ ok: false, error: 'Moonshine is in the middle of something else. Send the report when that has finished.' });
        state.report = Object.assign(state.report, { name: args.name, email: args.email });
        state.task = { kind: 'report', running: true, title: 'Collecting logs', detail: '', done: 0, total: 0, error: '', result: '', path: '' };
        push();
        const total = 3.4 * 1024 * 1024;
        for (let step = 1; step <= 5; step += 1) {
          timers.push(setTimeout(() => {
            state.task = Object.assign(state.task, { title: 'Sending the report', done: total * step / 5, total: total,
                                                     detail: (3.4 * step / 5).toFixed(1) + ' of 3.4 MB' });
            push();
          }, 1200 + step * 350));
        }
        timers.push(setTimeout(() => {
          // A description with "fail" in it is a report that does not arrive.
          const failed = (args.description || '').indexOf('fail') >= 0;
          state.task = Object.assign(state.task, { running: false, detail: '' }, failed
            ? { error: 'The report could not be sent (could not reach the server). The logs are saved on your desktop as Moonshine-logs-20261116-201455.zip. Try again later, or send that file.',
                path: 'C:\\Users\\henry\\Desktop\\Moonshine-logs-20261116-201455.zip' }
            : { result: 'Sent, thank you. Reference AB12CD34.' });
          push();
        }, 3400));
        return later(ok);
      }
      case 'uninstall': return later({ ok: false, error: 'This is the mock: nothing was uninstalled.' });
      default: return later(ok);
    }
  };

  // The corner box.
  window.addEventListener('DOMContentLoaded', () => {
    const asked = new URLSearchParams(location.search);
    if (asked.get('tab')) setTimeout(() => window.moonshine && window.moonshine.show(asked.get('tab')), 0);
    if (asked.has('plain')) return;
    const box = document.createElement('select');
    box.style.cssText = 'position:fixed;right:8px;bottom:44px;z-index:9;opacity:.6;font:11px system-ui';
    for (const name of Object.keys(scenes)) box.add(new Option(name, name, false, name === current));
    box.addEventListener('change', () => { current = box.value; go(current); });
    document.body.append(box);
  });
})();
