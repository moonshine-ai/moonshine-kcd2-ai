// moonshine_app -- a conversation with one NPC, with or without the game.
//
// Not the shipping front end, which will be a webview beside the game. This is the
// same pipeline underneath it, in two modes.
//
// Without --game, the conversation is entirely on the terminal, so the model side
// can be exercised with neither the game nor a microphone involved: the world text
// is prefilled at start-up, the character sheet is prefilled as though the player
// had walked up, and the reply is handed over a sentence at a time in the order the
// voice would say it. It reports prefill time for every turn, because that is the
// number that decides whether any of this is playable, and the one that can
// regress silently.
//
// With --game, the same pipeline is driven by the game: conversations open and
// close when the mod says they do, and each reply is written where the mod will
// read it. The player's words still come from stdin, because the microphone is not
// wired up yet, and that is the only difference from the finished thing.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "console.h"
#include "embedder.h"
#include "phrase_cache.h"
#include "gamelog.h"
#include "inspect.h"
#include "knowledge.h"
#include "lipsync.h"
#include "llamacpp.h"
#include "ollama.h"
#include "personadb.h"
#include "serve.h"
#include "memory.h"
#include "prefill.h"
#include "menu.h"
#include "prompt.h"
#include "quest.h"
#include "sentence.h"
#include "session.h"
#include "turnfile.h"
#include "voice.h"
#include "listen.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace {

using namespace moonshine;
using Clock = std::chrono::steady_clock;

// Windows power-throttles a process that has no window in front, and on a hybrid part
// that means the efficiency cores and the low clock. Every model here runs on the CPU
// while the game owns the screen, so that is exactly this process. Measured on an
// i9-13900HX: a 3.5s line of speech took 7.3s throttled and 4.2s not. Priority is left
// alone; the game beside it is not competed with any harder than before.
void keep_full_speed() {
#ifdef _WIN32
    PROCESS_POWER_THROTTLING_STATE state{};
    state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
    state.StateMask = 0;  // controlled, and off
    SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state, sizeof state);
#endif
}

// With the game, the GPU voice keeps its submissions short (src/clone_llama.cpp,
// load_or_fail): the game has the GPU flat out, and a submission of ours that cannot be
// interleaved with its frames for two seconds is a Windows GPU reset that ends the game,
// the desktop and this process together (three on 12 Oct 2026, each during a line). The
// engine's knobs are its environment, as with QWEN_SEED, so these are defaults set here
// and left alone when the environment already says otherwise.
//
// Only the code2wav window is capped. Measured on the Windows laptop with the GPU to itself, a 16.6s line
// (12 Oct 2026): the cap costs nothing (RTF 0.25 either way), while turning ggml's CUDA
// graphs off as well, so that the kernels go one at a time, more than doubles the time per
// line (RTF 0.25 to 0.54 idle; under the game that is past real time for most lines). The
// window is the largest single graph here, so capping it is most of the margin at none of
// the cost; QWEN_GGUF_CUDA_GRAPHS=0 in the environment buys the rest if it proves needed.
void set_env_default(const char* name, const char* value) {
    const char* set = std::getenv(name);
    if (set && *set) return;
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

void share_the_gpu() {
    set_env_default("QWEN_GGUF_WINDOW", "24");
}

// The CPU voice's threads: half the hardware threads, the same count the LLM takes
// (llamacpp.cpp, threads_for), in place of the engine's own default of eight. Measured on
// the Windows laptop (32 threads; 15 Oct 2026), a 14s line: 8 threads RTF 0.78 idle and 1.48 beside the
// game, 16 threads 0.72 and 1.18; 24 no better than 16, and all 32 falls apart (12.4) once
// the game wants a core, since the pool spin-waits. QWEN_THREADS set by hand is kept.
void voice_threads() {
    const unsigned all = std::thread::hardware_concurrency();
    if (all == 0) return;
    set_env_default("QWEN_THREADS", std::to_string((all + 1) / 2).c_str());
}

// The words' places in the audio are only read off the voice model (about 3% on the RTF,
// and flash attention off for the talker) when the mouth is shaped from them, which it is
// not by default: the game's own clips are the mouth, cut at the pauses heard in the audio
// (voice.cpp, lip_hooks). Either variable set by hand is left as set.
void words_only_for_shapes() {
    const char* shapes = std::getenv("MOONSHINE_MOUTH_SHAPES");
    if (shapes && *shapes && std::strcmp(shapes, "0") != 0) return;
    set_env_default("QWEN_GGUF_WORDS", "0");
}

// A copy of everything said on the terminal, in a file, with the time of day in front of
// each line (--log). On the Windows laptop the app runs in a window of its own that nothing keeps
// (the launcher), and what the voice reported about a line -- when its first audio
// came, whether the engine ran behind the speaker -- was the evidence missing from every
// question about a conversation that went badly (12 Oct 2026). One file shared by cout and
// cerr, so the two read in the order they happened.
struct LogFile {
    std::FILE* file = nullptr;
    std::mutex lock;
    bool at_line_start = true;

    void put(const char* text, std::streamsize count) {
        std::lock_guard<std::mutex> held(lock);
        for (std::streamsize i = 0; i < count; ++i) {
            if (at_line_start) stamp();
            std::fputc(text[i], file);
            at_line_start = text[i] == '\n';
        }
    }

    void stamp() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t seconds = std::chrono::system_clock::to_time_t(now);
        const int millis = static_cast<int>(
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() % 1000);
        std::tm local{};
#ifdef _WIN32
        localtime_s(&local, &seconds);
#else
        localtime_r(&seconds, &local);
#endif
        char text[32];
        std::strftime(text, sizeof text, "%H:%M:%S", &local);
        std::fprintf(file, "%s.%03d ", text, millis);
    }
};

class LogTee : public std::streambuf {
public:
    LogTee(std::streambuf* console, LogFile& log) : console_(console), log_(log) {}

protected:
    int overflow(int ch) override {
        if (ch == traits_type::eof()) return 0;
        const char c = static_cast<char>(ch);
        console_->sputc(c);
        log_.put(&c, 1);
        return ch;
    }
    std::streamsize xsputn(const char* text, std::streamsize count) override {
        console_->sputn(text, count);
        log_.put(text, count);
        return count;
    }
    int sync() override {
        console_->pubsync();
        std::fflush(log_.file);
        return 0;
    }

private:
    std::streambuf* console_;
    LogFile& log_;
};

// printf's formatting, through cout, so the tee above sees it.
template <typename... Args>
void printed(const char* format, Args... args) {
    if constexpr (sizeof...(args) == 0) {
        std::cout << format << std::flush;
    } else {
        char text[2048];
        std::snprintf(text, sizeof text, format, args...);
        std::cout << text << std::flush;
    }
}

// Stands in for what the persona layer will read out of npcs.sqlite. Short on
// purpose: measured, every token of character sheet costs about 15ms of the
// opening line on a 4B, and this is the text worth keeping lean.
constexpr std::string_view kDefaultSheet =
    "You are Cross-eyed Adam, a beggar in Kuttenberg.\n"
    "\n"
    "You sit on Havirska street, near the Italian Court. Your father Vaclav was a\n"
    "miner until his lungs gave out. You resent your brother Jost, who took the\n"
    "family's room above the cooper's. Your sister Marta brings you bread when her\n"
    "husband is not looking. You fear the circator, who has moved you on twice.\n"
    "\n"
    "You speak in sentences of 30 words or less. Never break character, and only\n"
    "output what you say, never inner thoughts or unspoken descriptions.";

struct Args {
    std::string world_path = "setting.txt";
    // Who every soul in the game is. The generator writes it; the app only reads it.
    std::string personas_path = "data/kcd2/npcs.sqlite";
    std::string sheet_path;
    std::string host = "http://127.0.0.1:11434";
    Options options;
    // Which model runtime answers.
    //
    // llama.cpp because that is what ships: it is in this process, so there is nothing to
    // install beside the app and nothing that can take the KV cache away between
    // conversations. Ollama stays reachable with --ollama because it is what every latency
    // figure in benchmarks/ was measured against, and a claim that this is faster is worth
    // nothing unless the old one can still be run on the same machine an hour later.
    bool ollama = false;
    // The weights, when llama.cpp is answering. Empty means look for the blob Ollama already
    // downloaded for options.model, which both development machines have.
    std::string gguf;
    // The embedding model for the knowledge base (embedder.h), and whether to use one at all.
    // Empty means look in the models folder for the file the conversion writes.
    std::string embed_gguf;
    bool knowledge = true;
    // Kokoro, through Moonshine Voice. On unless --no-voice, and only when this binary
    // was linked against the library. The default is the imported Newcastle man.
    bool voice_off = false;
    // Set by --voice, as against the default, so a build without the library stays
    // quiet until somebody asked it to speak.
    bool voice_chosen = false;
    std::string voice = std::string(kDefaultVoice);
    std::filesystem::path voices_path = "data/kcd2/voices";
    std::filesystem::path tts_root;
    // The microphone, for the menu's say row (listen.h). On unless --no-mic, with the
    // Moonshine speech model named by --stt-model, or tiny-streaming-en from the cache.
    bool mic = true;
    std::filesystem::path stt_model;
    // Listen for this many seconds and print what was heard, then exit (--hear).
    int hear_seconds = 0;
    // Say this one line and exit, which is how a voice is checked without a model.
    std::string speak;
    // With --speak, write a wav here instead of playing the line.
    std::filesystem::path speak_out;
    // With --speak, the voice-bank abbreviation of the clone to use.
    std::string actor;
    // Lines already spoken (phrase_cache.h). Empty is the phrases/ directory beside
    // the voices, when it is there; --no-phrase-cache leaves every line live.
    std::filesystem::path phrase_cache;
    bool phrase_cache_off = false;
    // A file of lines to make and keep, then exit. Each line is an actor, a tab, and
    // the words. --phrase-cache names where they go; the default is voices/phrases.
    std::filesystem::path bake_phrases;
    // A batch of lines to write and exit. Each line is voice, tab, wav path, tab, text.
    std::filesystem::path render;
    bool list_voices = false;
    // With --soul: a line of the player's to score against what that soul knows, printing
    // the ranking and the direction the session would add. The way to ask "what would they
    // say to this" without the game.
    std::string ask;
    // ...at this standing: stranger (open), vouched (friendly) or known (confided).
    std::string standing = "stranger";
    std::size_t least_chars = 45;
    // A turn whose prefix is being reused prefills in well under a second. Slower
    // than this means it is not, and which turn it happens on says why. Only used as a
    // fallback now: when the start-up warm measured a cold rate, the judgement is made
    // in tokens against that instead, which needs no per-machine number.
    double warn_prefill_seconds = 2.0;
    // How long to leave the world prefix unasked-for before asking again, while no
    // conversation is going on. Short enough that a loading screen covers a repair,
    // long enough that it is not a busy loop against the backend.
    //
    // Negative means nobody asked, and what that resolves to depends on who is answering.
    // Ollama can lose the prefix without saying so -- measured, `ollama stop` threw away 647
    // of 848 tokens -- and the heartbeat exists to notice. In-process there is nobody to lose
    // it to, so the beat is off: it would be the app defending the cache against itself.
    std::chrono::seconds rewarm_after{-1};
    // Follow the game rather than talking on the terminal alone.
    bool game = false;
    // Offer the model over HTTP for the Character Playground instead of following the game.
    //
    // Instead, not as well: there is one llama_context holding one sequence, so a playground
    // turn arriving mid-conversation would evict the conversation's tail. 0 is off.
    int serve_port = 0;
    // Report what discovery found and stop. Worth having on its own because the
    // first thing to establish about a machine that is not working is whether the
    // game was found at all, and this asks nothing of the model.
    bool where = false;
    // Answer without a model, so the game side can be tested on its own.
    bool canned = false;
    // Follow every line the NPC says with four replies for the player to pick from,
    // drawn by the mod over the game's own dialogue list. Off, the player types.
    bool menu = true;
    // While the player reads the menu, generate the replies to the rows that have no
    // written answer, so the one they pick is said at once (Session::speculate); and while
    // they look at a soul from talking distance, that soul's opening line, if it would
    // otherwise be the model's to write when they press (Session::look). Costs nothing
    // they would notice: the model is otherwise idle, and a pick, a typed line or a look
    // at somebody else stops the guess in progress within a token.
    bool speculate = true;
    // One soul to inspect. The question "why did that NPC say that" starts with what
    // the model was told about them, and this is how to read it without playing.
    std::string soul;
    // The raw state the mod would have read, so the playground and the terminal exercise
    // the same classification the game does rather than a second copy of it.
    Approach approach;
    std::string doing;
    // Where the soul's quest stands, for inspection and the playground, which keeps its own
    // score rather than reading a save. --have goes into approach.have.
    QuestFacts facts;
    // Which of the soul's favours --quest-state and --have speak of, when they are party to
    // several: that one is put first, where the facts go. Empty is the first as dealt.
    std::string quest_id;
    // For a scene of a story: how the scene before it ended (`truth`, `lied` ...), which the
    // save would carry; it is then done, and this scene open or void. Empty leaves the one
    // before untouched, which locks this scene.
    std::string before_how;
    // A file of the mod's own `BRIDGE state` and `BRIDGE memory` lines, standing in for the
    // dump a save would make before a begin, so a sheet with a memory can be inspected
    // without a game. Empty means a first meeting.
    std::string memory_path;
    // Whether the pre-written opening line is said at all.
    //
    // ON. It was off from the day the greetings were written until September 2026, to hear what
    // a model says in their place, latency included, before committing to the lines; the
    // comparison was made (a generated opening cost 15.15s cold in the first real conversation
    // and never came down to the written line's nothing), and the default followed it.
    // --generated-greetings puts the model back on the opening line for a session; the old
    // --written-greetings is still accepted and asks for what is now the default. Said on the
    // status line at start-up either way.
    bool say_written = true;
    // Emit that inspection as JSON instead of prose, for the playground, which stands in
    // for the game while the voice is being worked on.
    bool json = false;
    // Empty means find it. Overridable because a second Steam library that is only
    // listed in libraryfolders.vdf will not be found.
    std::string root;
    std::string user;
    std::string modid;
    // A copy of the terminal in a file, each line stamped with the time (LogFile). Empty
    // is none.
    std::filesystem::path log;
};

void usage() {
    std::cout <<
        "moonshine_app -- hold a conversation with one NPC\n"
        "\n"
        "  --game          follow kcd.log and answer the mod, instead of the terminal\n"
        "  --where         say where the game was found and whether every file the\n"
        "                  app is about to open is there, and stop; exit 1 if not\n"
        "  --canned        answer with placeholder turns, for testing without a model\n"
        "  --no-menu       do not offer replies to pick from after each line; the player\n"
        "                  types instead (the menu needs a mod built with it)\n"
        "  --no-guessing   do not work out the replies to the rows on offer while the\n"
        "                  player reads them, nor the opening line of a soul they are\n"
        "                  looking at; every pick and every unwritten greeting waits\n"
        "                  on the model\n"
        "  --root DIR      game directory holding kcd.log (default: found)\n"
        "  --user DIR      what the engine calls %USER%, holding profiles and saves\n"
        "  --modid ID      mod id, must match the generator's (default moonshine_names)\n"
        "  --world FILE    the world setting shared by every NPC (default setting.txt)\n"
        "  --personas DB   npcs.sqlite, who each soul is (default data/kcd2/npcs.sqlite)\n"
        "  --soul ID       print one soul's character sheet and stop\n"
        "  --charisma N    how well dressed the player is, 0..1 (rch)\n"
        "  --blood N       blood on the player, 0..1 (bld)\n"
        "  --dirt N        dirt on the player, 0..1 (drt)\n"
        "  --drink N       how drunk the player is, 0..1 (dru)\n"
        "  --health N      player health, on the game's own scale where 30 is hurt\n"
        "  --armed         the player has a weapon drawn\n"
        "  --met           they have spoken to this soul before\n"
        "  --generated-greetings  generate the opening line even where one was written,\n"
        "                  to hear what a model says in its place (and wait for it);\n"
        "                  written lines are said by default (--written-greetings)\n"
        "  --asleep        the soul is asleep\n"
        "  --chatting      the soul is already talking to somebody else\n"
        "  --hour N        world hour, 0..24 decimal, so 18.5 is half six in the evening\n"
        "  --rain N        rain intensity, 0 dry to 1 heavy; the game calls 0.6 heavy rain\n"
        "  --doing ANIM    the animation they were caught playing, as the engine spells\n"
        "                  it: LumberJackSawVAR, SittingIdle, Bartender_TakeAndTapStein\n"
        "  --quest-state S where this soul's quest stands: none, accepted, handed or done,\n"
        "                  several joined by commas (accepted,handed); unlearned for a pray\n"
        "                  the player has not the perk to do; how:<how> for the way it went\n"
        "  --have N        how many of the quest's item the player is carrying\n"
        "  --quest ID      which of the soul's favours --quest-state and --have speak of,\n"
        "                  when they are party to several (default: the first as dealt)\n"
        "  --before HOW    for a scene of a story, how the scene before it ended (truth,\n"
        "                  lied, quiet ...); it is then done, and this one open or void\n"
        "  --memory FILE   the mod's BRIDGE state/memory lines for this soul, as a save\n"
        "                  would dump them, so the sheet shows what they remember\n"
        "  --json          with --soul, emit the prompt and greeting as JSON\n"
        "  --ask TEXT      with --soul, score the player's line against what the soul\n"
        "                  knows and print the ranking and the direction (needs the\n"
        "                  embedding model)\n"
        "  --standing S    with --ask, the player's standing: stranger, vouched or known\n"
        "  --sheet FILE    the character sheet for the terminal, without --game\n"
        "  --serve PORT    serve the model to the Character Playground instead of following\n"
        "                  the game, so the playground uses the shipping cache\n"
        "  --model NAME    model to ask (default gemma4:12b)\n"
        "  --gguf PATH     weights for the in-process llama.cpp backend; default is\n"
        "                  whichever blob Ollama already holds for --model\n"
        "  --gpu           run the model on the GPU, for an experiment. Without it the app\n"
        "                  opens no GPU device at all: the model, the embedder and the\n"
        "                  voice are on the CPU, and the game has the GPU to itself. The\n"
        "                  voice has its own switch, MOONSHINE_TTS_BACKEND=gpu\n"
        "  --embed-gguf PATH  the EmbeddingGemma weights for the knowledge base; default\n"
        "                  models/embeddinggemma-300m-q8_0.gguf (then -q4_k_s, -f16),\n"
        "                  looked for from here\n"
        "  --no-knowledge  do not score typed lines against what the soul knows; the\n"
        "                  model answers from the sheet alone\n"
        "  --ollama        ask a running Ollama server instead of loading the model here\n"
        "  --host URL      where Ollama is (default http://127.0.0.1:11434)\n"
        "  --buffer N      characters to hold before speaking (default 45)\n"
        "  --voice ID      Kokoro voice to speak replies in (default bm_p286, the\n"
        "                  imported Newcastle man). Imported ids are in\n"
        "                  data/kcd2/voices (--voices); stock ids such as bm_george\n"
        "                  and bf_emma work too\n"
        "  --voices        list the imported Kokoro voices and stop\n"
        "  --no-voice      do not speak; replies stay on the screen\n"
        "  --tts-root DIR  Moonshine Voice assets, the directory that holds kokoro/\n"
        "                  and en_gb/ (default: the moonshine_voice cache, then a\n"
        "                  sibling moonshine checkout)\n"
        "  --no-mic        do not listen; the menu's \"(Say something else)\" row is\n"
        "                  typed into only\n"
        "  --stt-model M   the Moonshine speech model to listen with: a model name in\n"
        "                  the moonshine_voice cache (default tiny-streaming-en) or a\n"
        "                  directory holding one\n"
        "  --hear N        listen on the microphone for N seconds, printing the words\n"
        "                  as they are heard, and exit; the ear's --speak\n"
        "  --speak TEXT    say this and exit. The voice named by --actor when it is set,\n"
        "                  otherwise the Kokoro voice from --voice\n"
        "  --actor NAME    with --speak or the terminal, which voice: one of the Phonon\n"
        "                  catalogue's (toby, maeve, ...; models/phonon-31m-en/voices)\n"
        "  --out FILE      with --speak, write a wav instead of playing it\n"
        "  --render FILE   write a batch of wavs and exit. Each line is a voice id,\n"
        "                  a tab, a wav path, a tab, and the words\n"
        "  --phrase-cache DIR  lines already spoken, played instead of made (default:\n"
        "                  voices/phrases, when that directory is there)\n"
        "  --no-phrase-cache  make every line live, even one that was kept\n"
        "  --bake-phrases FILE  make the lines in FILE and keep them in the phrase\n"
        "                  cache, then exit. Each line is an actor, a tab, and the words\n"
        "  --rewarm N      seconds between asking for the world prefix again while idle,\n"
        "                  0 to never (default 120)\n"
        "  --log FILE      keep a copy of everything printed here in FILE, each line\n"
        "                  stamped with the time of day\n"
        "  --keep-thinking do not send think=false, for a model that has no switch\n";
}

std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + path);
    std::ostringstream text;
    text << in.rdbuf();
    std::string out = text.str();
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

double seconds_since(Clock::time_point from) {
    return std::chrono::duration<double>(Clock::now() - from).count();
}

Args parse(int argc, char** argv, bool* stop) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) throw std::runtime_error(arg + " needs a value");
            return argv[++i];
        };
        if (arg == "-h" || arg == "--help") { usage(); *stop = true; return args; }
        else if (arg == "--game") args.game = true;
        else if (arg == "--where") args.where = true;
        else if (arg == "--canned") args.canned = true;
        else if (arg == "--log") args.log = value();
        else if (arg == "--no-menu") args.menu = false;
        else if (arg == "--no-guessing") args.speculate = false;
        else if (arg == "--root") args.root = value();
        else if (arg == "--user") args.user = value();
        else if (arg == "--modid") args.modid = value();
        else if (arg == "--world") args.world_path = value();
        else if (arg == "--personas") args.personas_path = value();
        else if (arg == "--soul") args.soul = value();
        else if (arg == "--charisma") args.approach.charisma = std::stod(value());
        else if (arg == "--blood") args.approach.blood = std::stod(value());
        else if (arg == "--dirt") args.approach.dirt = std::stod(value());
        else if (arg == "--drink") args.approach.drink = std::stod(value());
        else if (arg == "--health") args.approach.health = std::stod(value());
        else if (arg == "--armed") args.approach.armed = true;
        else if (arg == "--met") args.approach.met = true;
        else if (arg == "--written-greetings") args.say_written = true;
        else if (arg == "--generated-greetings") args.say_written = false;
        else if (arg == "--asleep") args.approach.asleep = true;
        else if (arg == "--chatting") args.approach.chatting = true;
        else if (arg == "--hour") args.approach.hour = std::stod(value());
        else if (arg == "--rain") args.approach.rain = std::stod(value());
        else if (arg == "--doing") args.doing = value();
        else if (arg == "--quest-state") {
            // One word or several joined by commas, since the facts are separate: a holder
            // who handed the thing over and a giver who was said yes to are two different
            // things that can both be true. Cumulative, the way the mod folds its
            // commands: done implies accepted.
            std::string rest = value();
            args.facts = QuestFacts{};
            while (!rest.empty()) {
                const std::size_t comma = rest.find(',');
                const std::string state = rest.substr(0, comma);
                rest = comma == std::string::npos ? std::string() : rest.substr(comma + 1);
                if (state == "none") continue;
                if (state == "accepted") args.facts.accepted = true;
                else if (state == "handed") args.facts.handed = true;
                else if (state == "done") args.facts.done = args.facts.accepted = true;
                // The dispute's flags, as the save keeps them.
                else if (state == "broker") args.facts.broker = true;
                else if (state == "apologised") args.facts.apologised = args.facts.broker = true;
                else if (state == "refused") args.facts.refused = args.facts.broker = true;
                // A travel's: Henry has been within sight of the one looked at.
                else if (state == "seen") args.facts.seen = args.facts.accepted = true;
                // A beat's, a pray's and a mind's: the man down, the prayer said, the man
                // dead, one visit made (checked, once per visit).
                else if (state == "beaten") args.facts.beaten = args.facts.accepted = true;
                else if (state == "prayed") args.facts.prayed = args.facts.accepted = true;
                // A leave's and a return's: the thing put down, the cry heard after; a
                // return's grave robbed, its curse on.
                else if (state == "left") args.facts.left = args.facts.accepted = true;
                else if (state == "heard") args.facts.heard = args.facts.left = args.facts.accepted = true;
                else if (state == "looted") args.facts.looted = args.facts.accepted = true;
                else if (state == "cursed") args.facts.cursed = args.facts.accepted = true;
                // An ambush's: the false way written down, the men at the place met.
                else if (state == "directed") args.facts.directed = args.facts.accepted = true;
                else if (state == "ambushed") args.facts.ambushed = args.facts.directed = args.facts.accepted = true;
                else if (state == "dead") args.facts.dead = args.facts.accepted = true;
                else if (state == "checked") { ++args.facts.checks; args.facts.accepted = true; }
                // An ask's whose word is carried on: the giver told and paid, Henry sent on.
                else if (state == "onward") args.facts.onward = args.facts.handed = args.facts.accepted = true;
                // A find's keepsake in Henry's pocket.
                else if (state == "keepsake") args.facts.keepsake = args.facts.accepted = true;
                // An unyielding holder's: Henry refused to his face; the giver has named the
                // one who keeps the house; she has let him in; she has stolen him the key.
                else if (state == "rebuffed") args.facts.rebuffed = args.facts.accepted = true;
                else if (state == "named") args.facts.named = args.facts.rebuffed = args.facts.accepted = true;
                else if (state == "letin") args.facts.letin = args.facts.named = args.facts.rebuffed = args.facts.accepted = true;
                else if (state == "keyed") args.facts.keyed = args.facts.named = args.facts.rebuffed = args.facts.accepted = true;
                // A holder caught in the night and denounced at the bedside: nothing came back.
                else if (state == "denounced") { args.facts.denounced = args.facts.seen = args.facts.accepted = true; args.facts.how = "denounced"; }
                // Given up to the giver's face: done, as `abandoned`.
                else if (state == "abandoned") { args.facts.done = args.facts.accepted = true; args.facts.how = "abandoned"; }
                // A pray's giver met by a player the game will not let pray at a shrine.
                else if (state == "unlearned") args.facts.cannot_pray = true;
                // How it went, as the store's how (`how:lied`, `how:pinned`): what a giver
                // answers to once the thing is handed or the errand done.
                else if (state.rfind("how:", 0) == 0) args.facts.how = state.substr(4);
                // A dictated letter written, with its parts' outcome digits (`wrote:023`).
                else if (state.rfind("wrote:", 0) == 0) { args.facts.wrote = state.substr(6); args.facts.accepted = true; }
                else {
                    throw std::runtime_error(
                        "--quest-state takes none, accepted, handed, done, broker, apologised, "
                        "refused, seen, beaten, prayed, left, heard, looted, cursed, directed, "
                        "ambushed, dead, checked, onward, keepsake, rebuffed, named, letin, keyed, denounced, abandoned, "
                        "unlearned, how:<how> or wrote:<digits>, joined by commas");
                }
            }
        }
        else if (arg == "--have") args.approach.have = std::stoi(value());
        else if (arg == "--quest") args.quest_id = value();
        else if (arg == "--before") args.before_how = value();
        else if (arg == "--memory") args.memory_path = value();
        else if (arg == "--json") args.json = true;
        else if (arg == "--sheet") args.sheet_path = value();
        else if (arg == "--model") args.options.model = value();
        else if (arg == "--gpu") args.options.cpu_only = false;
        else if (arg == "--serve") args.serve_port = std::stoi(value());
        else if (arg == "--gguf") args.gguf = value();
        else if (arg == "--embed-gguf") args.embed_gguf = value();
        else if (arg == "--no-knowledge") args.knowledge = false;
        else if (arg == "--ask") args.ask = value();
        else if (arg == "--standing") {
            args.standing = value();
            if (args.standing != "stranger" && args.standing != "vouched" && args.standing != "known") {
                throw std::runtime_error("--standing takes stranger, vouched or known");
            }
        }
        else if (arg == "--ollama") args.ollama = true;
        else if (arg == "--host") args.host = value();
        else if (arg == "--buffer") args.least_chars = std::stoul(value());
        else if (arg == "--voice") {
            args.voice = value();
            args.voice_chosen = true;
        }
        else if (arg == "--voices") args.list_voices = true;
        else if (arg == "--no-voice") args.voice_off = true;
        else if (arg == "--tts-root") args.tts_root = value();
        else if (arg == "--no-mic") args.mic = false;
        else if (arg == "--stt-model") args.stt_model = value();
        else if (arg == "--hear") args.hear_seconds = std::stoi(value());
        else if (arg == "--speak") args.speak = value();
        else if (arg == "--actor") args.actor = value();
        else if (arg == "--out") args.speak_out = value();
        else if (arg == "--render") args.render = value();
        else if (arg == "--phrase-cache") args.phrase_cache = value();
        else if (arg == "--no-phrase-cache") args.phrase_cache_off = true;
        else if (arg == "--bake-phrases") args.bake_phrases = value();
        else if (arg == "--rewarm") args.rewarm_after = std::chrono::seconds(std::stoi(value()));
        else if (arg == "--keep-thinking") args.options.disable_thinking = false;
        else throw std::runtime_error("unknown argument " + arg);
    }
    // Resolved here rather than in the field, because the answer depends on --ollama, which
    // may be read after --rewarm on the command line.
    if (args.rewarm_after.count() < 0) {
        args.rewarm_after = args.ollama ? std::chrono::seconds{120} : std::chrono::seconds{0};
    }
    return args;
}

// A model's worth of plumbing without a model.
//
// For testing the game side on a machine with nothing loaded: whether the mod finds
// the file, says the words, holds the screen and reports the turn are all questions
// about the game, and waiting on an LLM to ask them only makes them slower to ask.
// This is what the old standalone moonshine_bridge did, and having it here is what
// lets there be one implementation of the protocol rather than two that drift.
class CannedLlm : public Llm {
public:
    Timing chat(const std::vector<Message>& messages, const Options& options,
                const OnDelta& on_delta) override {
        // A prefill is not a turn. It asks for a single token, and counting those
        // would have the first thing the player hears announce itself as turn 3.
        if (options.reply_tokens <= 1) return {};
        ++turns_;
        // Long enough to be worth reading, and two sentences so the gating between
        // them is exercised rather than bypassed.
        const std::string reply =
            "This is canned turn " + std::to_string(turns_) +
            ", standing in for something a model would have said. "
            "If you can read this, the mod found the file and said the words.";
        // In pieces, because a reply that arrives whole would not exercise the part
        // of this that hands sentences over as they finish.
        for (std::size_t at = 0; at < reply.size(); at += 16) {
            on_delta(std::string_view(reply).substr(at, 16));
        }
        return {};
    }

    std::string describe() const override { return "canned replies, no model"; }

private:
    int turns_ = 0;
};

// What the soul knows, and the embedder that finds it: the base, the model and the director
// together, or nothing and the reason said once. None of the three is fatal to miss -- the
// conversation goes on from the sheet, as it did before there was a knowledge base -- but
// each is worth one line, because a soul answering every question from the sheet alone is
// otherwise indistinguishable from one whose base was never found.
struct Knowing {
    std::unique_ptr<KnowledgeBase> base;
    std::unique_ptr<Embedder> embedder;
    std::unique_ptr<KnowledgeDirector> director;
};

Knowing open_knowledge(const Args& args) {
    Knowing out;
    if (!args.knowledge) {
        std::cout << "knowledge:  off (--no-knowledge), so the model answers from the sheet alone\n";
        return out;
    }
    out.base = std::make_unique<KnowledgeBase>(args.personas_path);
    if (!out.base->ok()) {
        std::cout << "knowledge:  cannot read " << args.personas_path << " (" << out.base->trouble()
                  << ")\n";
        out.base.reset();
        return out;
    }
    if (!out.base->has_tables()) {
        std::cout << "knowledge:  " << args.personas_path
                  << " has no knowledge tables (run build_kcd2_knowledge.py), so every soul"
                     " answers from the sheet alone\n";
        out.base.reset();
        return out;
    }
#ifdef MOONSHINE_HAS_LLAMA
    std::filesystem::path gguf = args.embed_gguf;
    if (gguf.empty()) gguf = LlamaEmbedder::find_weights();
    if (gguf.empty()) {
        std::cout << "knowledge:  no embedding model found; pass --embed-gguf PATH or convert"
                     " EmbeddingGemma into models/ (knowledge-design plan section 7.2)\n";
        out.base.reset();
        return out;
    }
    try {
        out.embedder = std::make_unique<LlamaEmbedder>(gguf);
    } catch (const std::exception& why) {
        std::cout << "knowledge:  " << why.what() << "\n";
        out.base.reset();
        return out;
    }
    out.director = std::make_unique<KnowledgeDirector>(*out.embedder, *out.base);
    std::cout << "knowledge:  " << out.base->souls_with_items() << " souls with items, found by "
              << out.embedder->describe() << "\n";
#else
    std::cout << "knowledge:  built without the llama.cpp backend, so there is no embedder and"
                 " the model answers from the sheet alone\n";
    out.base.reset();
#endif
    return out;
}

// Following the game. Two threads, because one of them is always blocked: the
// poller spends whole seconds inside the model generating a reply, and stdin is
// standing in for a microphone that will have a thread of its own anyway. One lock
// around the session, so a player typing while a greeting is still being generated
// waits for it rather than interleaving with it.
//
// Where the kept lines are. Empty when the cache is off, or (unless `creating`) the
// default directory is not there yet -- a checkout with nothing baked stays live.
std::filesystem::path phrase_cache_dir(const Args& args, bool creating) {
    if (args.phrase_cache_off && !creating) return {};
    if (!args.phrase_cache.empty()) return args.phrase_cache;
    const std::filesystem::path dir = args.voices_path / "phrases";
    std::error_code error;
    if (creating || std::filesystem::is_directory(dir, error)) return dir;
    return {};
}

void attach_phrase_cache(Speaker& speaker, const Args& args) {
    if (args.phrase_cache_off) return;
    const std::filesystem::path dir = phrase_cache_dir(args, false);
    // An empty answer is the default directory not being there. Said, because a checkout
    // that was baked on another machine and not sent plays every known line live and the
    // only sign is the wait (22 Oct 2026: the phrases directory never left the Mac).
    if (dir.empty()) {
        const std::filesystem::path where =
            args.phrase_cache.empty() ? args.voices_path / "phrases" : args.phrase_cache;
        std::cout << "voice: no phrase cache in " << where.string()
                  << "; known lines will be spoken live\n"
                  << std::flush;
        return;
    }
    speaker.set_phrase_cache(dir);
    std::cout << "voice: " << speaker.cached_lines() << " cached lines in " << dir.string() << "\n"
              << std::flush;
}

// The voice, or nothing and the reason already printed. A missing library or a missing
// asset tree leaves the conversation as it was: words on the screen, no sound. --speak
// is the one caller that treats that as a failure, because the line was the whole point.
std::unique_ptr<Speaker> open_speaker(const Args& args, bool required) {
    if (args.voice_off) return nullptr;
    if (!tts_built()) {
        // A build with neither Kokoro nor a way to play the clones stays silent.
        if (required || args.voice_chosen || !args.tts_root.empty()) {
            std::cerr << "voice: this build has no voice\n";
        }
        return nullptr;
    }
    const std::filesystem::path root = find_tts_root(args.tts_root);
#ifndef MOONSHINE_HAS_TTS
    // The clones are spoken by the engine on the machine that holds the weights.
    // Kokoro is the fallback, and this build does not have it.
    (void)required;
    try {
        const std::string voice = args.voice.empty() ? std::string(kDefaultVoice) : args.voice;
        auto speaker = std::make_unique<Speaker>(voice, args.voices_path, root);
        std::cout << "voice: " << speaker->voice()
                  << " (cloned voices; Kokoro is not in this build)\n"
                  << std::flush;
        attach_phrase_cache(*speaker, args);
        return speaker;
    } catch (const std::exception& why) {
        std::cerr << "voice: " << why.what() << "\n";
        return nullptr;
    }
#else
    if (root.empty()) {
        std::cerr << "voice: no Moonshine Voice assets found. Pass --tts-root, or set\n"
                     "  MOONSHINE_TTS_ROOT to the directory that holds kokoro/ and en_gb/.\n";
        return nullptr;
    }
    try {
        auto speaker = std::make_unique<Speaker>(args.voice, args.voices_path, root);
        std::string about;
        for (const ImportedVoice& voice : imported_voices(args.voices_path)) {
            if (voice.id == speaker->voice()) about = voice.about;
        }
        std::cout << "voice: " << speaker->voice();
        if (!about.empty()) std::cout << " (" << about << ")";
        std::cout << "\n" << std::flush;
        attach_phrase_cache(*speaker, args);
        return speaker;
    } catch (const std::exception& why) {
        std::cerr << "voice: " << why.what() << "\n";
        return nullptr;
    }
#endif
}

int run_with_game(const Args& args, Llm& llm, const std::string& world, Speaker* speaker) {
    GamePaths paths = discover();
    if (!args.root.empty()) paths.root = args.root;
    if (!args.user.empty()) paths.user = args.user;
    if (!args.modid.empty()) paths.modid = args.modid;

    if (paths.root.empty()) {
        std::cerr << "moonshine_app: cannot find the game. Pass --root.\n";
        return 1;
    }
    std::cout << "game at " << paths.root << "\n";
    std::cout << "writing turns under " << paths.user << "\n";

    TurnWriter writer(paths);
    // Before anything else, because a turn file from a run that crashed lets the mod
    // race forward through words meant for somebody else.
    writer.sweep();
    // The mouth: as a line's audio starts and ends, a cue file beside the turns naming
    // the game's own facial clips to play (lipsync.h; the mod's LipPoll reads it). From
    // the voice's thread, which is why it goes to the writer directly and not through
    // the session, whose lock the model holds while it generates.
    if (speaker) {
        if (speaker->facial_clips() > 0) {
            LipSink sink;
            sink.write = [&writer](const std::string& file, const std::string& body) {
                writer.lip(file, body);
            };
            sink.forget = [&writer](const std::vector<std::string>& files) { writer.forget_lips(files); };
            speaker->set_lips(std::move(sink));
            std::cout << "mouth: " << speaker->facial_clips()
                      << " of the game's facial clips to borrow while a line is spoken\n";
        } else {
            std::cout << "mouth: no facials.txt beside the voices, so the mod's own jaw beats\n";
        }
    }
    // The ear: the microphone and Moonshine's streaming transcriber, opened while the
    // menu's say row is live and closed as it stops (listen.h). Without it the row is
    // still there, typed into.
    std::unique_ptr<Ear> ear;
    if (args.mic) {
        if (!listen_built()) {
            if (!args.stt_model.empty()) std::cerr << "ear: this build cannot hear\n";
        } else {
            const std::filesystem::path model = find_stt_model(args.stt_model);
            if (model.empty()) {
                std::cerr << "ear: no Moonshine speech model found (" << kDefaultSttModel
                          << " in the moonshine_voice cache); pass --stt-model, or --no-mic\n";
            } else {
                try {
                    ear = std::make_unique<Ear>(model);
                    std::cout << "ear: " << model.parent_path().filename().string()
                              << " listens while the say row is up\n";
                } catch (const std::exception& why) {
                    std::cerr << "ear: " << why.what() << "\n";
                }
            }
        }
    }
    // And the heartbeat from the first moment, before the long prefill: a player who
    // walks up to somebody while the model warms is talking to an app that is running.
    auto unix_now = [] {
        return static_cast<long long>(std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    };
    writer.beat(unix_now());

    // Opened before prefilling, not after. Prefill is the better part of ten seconds
    // and a tailer starts reading at the end of the file, so opening it afterwards
    // silently drops everything the game said while warming up -- including a player
    // who walked up to somebody and got no greeting for their trouble. Opening it
    // first fixes the position; the poller below then catches up.
    LogTailer tailer(log_path(paths));
    if (!tailer.ok()) {
        // Not fatal. Asking a player to start a launcher only once the game is
        // already running is a worse experience than waiting for the log to appear.
        std::cout << "waiting for " << log_path(paths) << " to appear\n";
    }

    // Opened before the prefill, which takes about ten seconds: a player whose database
    // is missing should be told so at once rather than after the wait.
    PersonaDb personas(args.personas_path);
    if (personas.ok()) {
        std::cout << "personas: " << personas.named_souls() << " named souls in "
                  << personas.path() << "\n";
    } else {
        // Not fatal, but worth saying loudly: without this every soul in the game is a
        // stranger, which is most of the point of the mod gone.
        std::cout << "personas: cannot read " << personas.path() << " (" << personas.trouble()
                  << ")\n          everyone will be talked to as a stranger\n";
    }

    Prefiller prefiller(llm, args.options);
    std::cout << "prefilling the world\n" << std::flush;
    // Only the world, because which soul to warm is not knowable yet: the mod reports a
    // conversation starting, not a player walking near someone. Until it reports the
    // approach, every first line pays for that NPC's sheet.
    //
    // Followed by a second ask that measures what prefilling costs here. See Warmth: the
    // rate cannot be read off the world prefill itself, because that prefill is a cache
    // hit whenever the backend still holds the prefix from an earlier run, and this is
    // what turns "the world was prefilled" into "every conversation starts this many
    // tokens in, saving this long, on this machine, today".
    const Warmth warmth = prefiller.prove_world(world);
    const double rate = warmth.prefill_rate();
    if (rate > 0.0) {
        printed("world:      %d tok cached in %.2fs; this machine prefills fresh text at"
                    " %.0f tok/s, so that is %.1fs off every conversation\n",
                    warmth.warm.prompt_tokens, warmth.warm.prefill_seconds, rate,
                    warmth.warm.prompt_tokens / rate);
    } else {
        // The canned model, or a backend that reports no timings. Nothing to judge against,
        // so nothing below will claim what was reused.
        printed("world:      %d tok, and the backend reported no timings, so nothing"
                    " below will say what was reused\n", warmth.warm.prompt_tokens);
    }
    if (args.rewarm_after.count() > 0) {
        printed("cache:      the backend's, so it is asked for again every %llds in case it"
                    " was dropped\n",
                    static_cast<long long>(args.rewarm_after.count()));
    } else {
        printed("cache:      ours, in this process, so nothing can drop it between"
                    " conversations\n");
    }
    std::fflush(stdout);

    // Who has been met and where every quest stands live in the player's saved game now, kept
    // by the mod and dumped into the log before each conversation. The two files an earlier
    // version kept beside the saves are removed on sight: a book that says a soul was met
    // when the save being played has never seen them is worse than no book, and a player
    // who loads an older save should find the people in it as they were.
    for (const char* stale : {"moonshine_met.txt", "moonshine_quests.txt"}) {
        const std::filesystem::path old =
            paths.user.empty() ? std::filesystem::path(stale) : paths.user / stale;
        std::error_code ec;
        if (std::filesystem::remove(old, ec)) {
            std::cout << "removed:    " << old << " -- the save keeps this now\n";
        }
    }
    std::cout << "memory:     in the saved game, dumped by the mod before each conversation\n";

    // Said out loud, because every opening line in the session that follows will be slow and the
    // reason should not have to be guessed at from the timings.
    std::cout << "greetings:  "
              << (args.say_written
                      ? "written where there are any, so the opening line is instant"
                      : "GENERATED, even where a line was written -- expect a long first pause "
                        "(--generated-greetings was given; leave it off to say the written line)")
              << "\n";

    // Loaded after the world, which is the long wait, and before the session that uses it.
    Knowing knowing = open_knowledge(args);

    Session session(llm, writer, prefiller, args.options);
    session.set_world(world);
    session.set_menu(args.menu);
    session.set_speculation(args.speculate);
    if (knowing.director) session.set_knowledge(knowing.director.get());
    session.set_persona_source([&](std::string_view soul, const Approach& approach,
                                  std::string_view anim, const SoulRecord& record) {
        // What the save knows and the begin line does not: whether they have met, where the
        // quest stands, and what was said before. The mod counts this conversation before it
        // dumps, so a first meeting arrives as met=1.
        Approach known = approach;
        known.met = record.met > 1;
        // The facts of every quest the soul is party to are on the standing (the dump's
        // quests= field); the older quest= fields are the first quest's and win for it. The
        // item counts are already on the approach, off the begin line.
        QuestFacts facts;
        if (const std::optional<Soul> found = personas.find(soul);
            found && found->quest.exists() && found->quest.id == record.quest) {
            facts = record.facts;
        }
        const Persona persona = personas.persona(soul, known, anim, args.say_written, facts,
                                                 memory_block(record, kPlayer),
                                                 standing_of(record));
        return std::optional<Persona>(persona);
    });

    Listener listener;
    listener.on_say = [&session](std::string_view piece) {
        // Spoken when the mod shows the line, not as it is written: the caption and
        // the voice then belong to the same moment, and a streamed reply is one
        // reading rather than a queue of pieces.
        std::cout << session.npc() << "> " << piece << "\n" << std::flush;
    };
    listener.on_status = [](std::string_view text) {
        std::cout << "-- " << text << "\n" << std::flush;
    };
    // The greeting, made while the player is still deciding to press, or at the latest as
    // it is written: the mod's voice marker then finds the audio ready instead of starting
    // the synthesis (voice.h, Speaker::prepare). The same clone the marker will name.
    listener.on_prepare = [speaker](std::string_view text, std::string_view actor) {
        if (speaker) speaker->prepare(text, clone_for("npc", actor));
    };
    listener.on_drop_prepared = [speaker] {
        if (speaker) speaker->forget_ahead();
    };
    listener.on_option = [](const MenuOption& option) {
        std::cout << "   " << option.row << ". [" << option.intent << "] " << option.text
                  << "\n" << std::flush;
    };
    // The say row going live and stopping: the microphone with it. A microphone that will
    // not open is reported and the row is left to the keyboard.
    listener.on_listen = [&ear](bool on) {
        if (!ear) return;
        try {
            if (on) {
                ear->start();
            } else {
                ear->stop();
            }
        } catch (const std::exception& why) {
            std::cerr << "ear: " << why.what() << "\n";
        }
    };
    listener.on_timing = [&args, rate](const Timing& timing) {
        std::string aside;
        if (timing.held_seconds > 0.05) {
            aside = ", stood aside " + std::to_string(timing.held_seconds).substr(0, 4) +
                    "s while the voice spoke";
        }
        printed("    [prefill %d tok in %.2fs, %d tok out in %.2fs%s%s]\n",
                    timing.prompt_tokens, timing.prefill_seconds, timing.reply_tokens,
                    timing.decode_seconds, aside.c_str(),
                    timing.guessed ? ", paid ahead of the press: guessed" : "");
        // The thing that regresses without any other symptom: the same words, at ten
        // times the cost, because the cached prefix stopped being reused. Said in tokens
        // against the rate measured at start-up, rather than against a wall-clock limit,
        // because the limit has to be a different number on every machine and the
        // measurement does not.
        const int cached = tokens_already_cached(timing, rate);
        if (rate > 0.0 && cached > 0) {
            printed("    [%d of those %d tokens were already cached, saving %.2fs]\n",
                        cached, timing.prompt_tokens, cached / rate);
        } else if (rate > 0.0 && timing.prompt_tokens > 0) {
            printed("    [warning: none of those %d tokens was reused -- that is the"
                        " whole prompt walked again]\n", timing.prompt_tokens);
        } else if (timing.prefill_seconds > args.warn_prefill_seconds) {
            // No rate to judge against, so the old wall-clock limit is all there is.
            std::cout << "    [warning: that prefill is too slow to be a cache hit]\n";
        }
        std::fflush(stdout);
    };
    session.set_listener(std::move(listener));
    // With a voice, every line says so (@voiced), and the mod holds the camera -- on Henry
    // for his line, on the other party for the closing one -- until the line has been heard
    // to the end (Session::spoken, below) rather than for the reading time it guesses from
    // the words.
    session.set_voiced(speaker != nullptr);
    // The game may have been talking before this opened: the mod's count of turns runs
    // across its whole run, and the tailer reads only what comes after now.
    const RunCounts counts = run_counts(log_path(paths));
    session.resume(counts.played);
    session.resume_hooks(counts.hooked);
    // The slash commands (console.h), from the keyboard below or from the game's own
    // console through the log.
    Console console(personas);
    // Which deal this database is, for the check against the mod's qset= on every begin
    // and look line; the mod fingerprints already warned about, so each is said once.
    const std::string quest_set = personas.quest_set();
    std::set<std::string> mismatched_sets;

    std::mutex lock;
    bool quit = false;
    // Raised by the keyboard loop below while it waits for the lock, so a guess in
    // progress lets go of it.
    std::atomic<bool> typing{false};
    // Which part of the loop below held it, when one holds it for over a second. A marker
    // in the log is answered only when the loop comes round to it, so a stall anywhere in
    // here is a pause the player hears -- and nothing else says where it was (12 Oct 2026:
    // a first line the mod put up 0.8 s after the press was spoken 6 s later, and the
    // voice, the mod and the log's flush had each been cleared in turn).
    using Held = std::chrono::steady_clock::time_point;
    const auto held_by = [](const Held& since, const char* what) {
        const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
        if (took >= 1.0) {
            printed("-- the loop was held %.1fs by %s\n", took, what);
        }
    };
    std::thread poller([&] {
        auto warmed_at = std::chrono::steady_clock::now();
        while (true) {
            std::vector<Event> events;
            {
                const Held since = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> held(lock);
                if (quit) return;
                events = tailer.poll();
                held_by(since, "reading the log");
            }
            for (const Event& event : events) {
                const Held since = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> held(lock);
                if (quit) return;
                struct Timed {
                    const Held& since;
                    const Event& event;
                    decltype(held_by)& held_by;
                    ~Timed() {
                        held_by(since, event.marker == Marker::Begin    ? "the begin line"
                                       : event.marker == Marker::Look   ? "a look"
                                       : event.marker == Marker::Said   ? "the player's line"
                                       : event.marker == Marker::Picked ? "the player's pick"
                                       : event.marker == Marker::Played ? "a played line"
                                       : event.marker == Marker::Voice  ? "a voice line"
                                                                        : "a line of the log");
                    }
                } timed{since, event, held_by};
                // A canned app is a pass's stand-in for the model, and the pass is over when
                // the game is. Left running, it answered the player's next game by hand with
                // "This is canned turn ..." as the first thing anybody said (notes.md, 5 Oct
                // 2026). An app with a model stays: the player relaunches the game under it.
                if (event.marker == Marker::Quit && args.canned) {
                    std::cout << "-- the game quit; a canned app has no business outliving it,"
                                 " stopping\n" << std::flush;
                    quit = true;
                    writer.hush(unix_now());
                    // Straight out rather than back through main, which may be blocked on a
                    // keyboard nobody is at.
                    std::_Exit(0);
                }
                // "/quests" or "/goto 21" typed at the game's console (~): the same command
                // as from the keyboard below, with the answer sent back as a hook that
                // writes it into the game's log for the console to show.
                if (event.marker == Marker::ConsoleLine) {
                    std::cout << "-- the game's console asked: " << event.said << "\n";
                    const ConsoleOutcome outcome = console.run(event.said);
                    std::cout << outcome.text << "\n" << std::flush;
                    session.hook(Console::game_lua(outcome),
                                 outcome.label.empty() ? "the answer to " + event.said : outcome.label);
                    continue;
                }
                // The mod says which deal it was built from on every begin and look line
                // (qset=); this app's database has its own. Different, and an errand accepted
                // here is one the game's graph has never heard of -- nothing in the journal,
                // no notification, no marker (notes.md, 5 Oct 2026: Anna's cat, Berchta,
                // Christine's letter, all played with a mod sent before the deal that
                // renamed them). Said once here and once on the game's screen, per mod seen.
                if ((event.marker == Marker::Begin || event.marker == Marker::Look) &&
                    !event.approach.quest_set.empty() && !quest_set.empty() &&
                    event.approach.quest_set != quest_set &&
                    mismatched_sets.insert(event.approach.quest_set).second) {
                    std::cout << "-- WARNING: the game's mod was built from a different quest set"
                                 " (mod qset=" << event.approach.quest_set << ", this database "
                              << quest_set << "): errands offered here will not reach the"
                                 " journal until the mod is sent again (scripts/build_mod.sh, or"
                                 " kcd2modgen over this database).\n" << std::flush;
                    session.hook(
                        "if Game ~= nil and Game.SendInfoText ~= nil then\n"
                        "\tGame.SendInfoText(\"[Moonshine] The game's mod and the conversation app"
                        " were built from different quest data. Errands accepted now will not reach"
                        " the journal; send the mod again (scripts/build_mod.sh).\", true, nil, 10)\n"
                        "end\n",
                        "the quest-set mismatch warning");
                }
                // A new approach, a reply, or the end of the talk: whatever was still
                // being said belongs to the line before this one.
                if (speaker && (event.marker == Marker::Begin || event.marker == Marker::Said ||
                                event.marker == Marker::Picked || event.marker == Marker::Left ||
                                event.marker == Marker::Restarted)) {
                    speaker->hush();
                }
                // The mod prints this as the line goes on screen. Henry's recording for
                // him; a male or female token is that stand-in; npc is whoever is open.
                if (speaker && event.marker == Marker::Voice) {
                    // And a mouth to go with it: Henry's own actor's clips for him, the
                    // soul's actor's for them, named for the mod by the line's key, which
                    // the mod computes from the same marker (lipsync.h, lip_key).
                    LipTag lips;
                    lips.who = event.intent;
                    // Outside a session the marker names the speaker, and the mouth is that
                    // soul's actor's; the session's face is for a line said in one. Before
                    // (23 Oct 2026) a holder's Talk borrowed the last session's face, and
                    // Father Ernest's reading moved in Christine's actor's clips, which
                    // his head could not play.
                    if (event.intent == "henry") {
                        lips.face = std::string(kHenryFace);
                    } else if (!event.soul.empty()) {
                        if (const std::optional<Soul> who = personas.find(event.soul)) {
                            lips.face = actor_abbr(who->voice_id);
                        }
                    } else {
                        lips.face = session.face();
                    }
                    lips.key = lip_key(event.intent, event.said);
                    speaker->say(event.said, clone_for(event.intent, session.actor()), std::move(lips));
                }
                session.handle(event);
            }
            // Lines the voice is done with since the last look. Henry's line and the closing
            // line of a conversation go back to the mod as a hook (Moonshine.Spoken), which
            // is what it waits for before letting the camera go. Collected here, under the
            // lock, rather than from the voice's own thread, which has no business in the
            // session.
            if (speaker) {
                const Held since = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> held(lock);
                if (quit) return;
                for (const std::string& line : speaker->take_spoken()) session.spoken(line);
                held_by(since, "the lines the voice had finished");
            }
            // What the ear has heard so far, offered to the session on every pass; it sends
            // the words on to the slot when they have changed and the mod has had time to
            // show the last (Session::heard). Under the lock, like the voice above.
            if (ear && ear->listening()) {
                const Held since = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> held(lock);
                if (quit) return;
                session.heard(ear->text());
                held_by(since, "the words heard so far");
            }

            // With the menu up and nothing to do, work out the replies the player might
            // pick next. Under the lock like everything else that uses the model, and
            // stopped within a token by anything that wants it: a line in the log, a
            // line typed here, or being told to quit. With more to guess, straight back
            // round without the sleep, so the log is looked at between guesses.
            //
            // Not while a line is being spoken. The clone engine runs on the same cores as
            // the model and the game, and it is only a little ahead of real time with the
            // game alone (RTF 1.5 on the Windows laptop at int8); the model guessing ahead at the same time
            // is what turns a spoken line into one with silences in it. The guess waits
            // until the line is out, which is still before the player has picked.
            {
                const Held since = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> held(lock);
                if (quit) return;
                const auto speaking = [&] { return speaker && speaker->speaking(); };
                if (session.has_speculation() && !speaking()) {
                    const bool more = session.speculate([&] {
                        return quit || typing.load() || tailer.pending() || speaking();
                    });
                    held_by(since, "a guess at the next reply");
                    if (more && !speaking()) continue;
                }
            }

            // Ask for the world prefix again now and then, so that losing it costs a
            // second of nobody's time instead of the next conversation's opening line.
            //
            // The start-up warm is not enough on its own. Ollama evicts a model to free
            // memory, and the game asks for 7.24 GB the moment it loads a save, which is
            // the leading suspect for the 15.15s opening that started all this. Nothing
            // else talks to this backend, so the prefix cannot be lost to a competing
            // request -- but it can be lost to the model going away underneath it, and
            // there is no notification when that happens.
            //
            // Under the lock, so it cannot overlap a conversation: no event can be
            // handled while it runs. That does mean a player walking up during a repair
            // waits for the rest of it, which is not the cost it appears to be -- the
            // repair is the same prefill their opening line was about to pay for, and
            // having paid it here they get it for nothing. When the prefix is intact this
            // whole branch is a hundredth of a second.
            if (args.rewarm_after.count() > 0 &&
                std::chrono::steady_clock::now() - warmed_at >= args.rewarm_after) {
                const Held since = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> held(lock);
                if (quit) return;
                struct Timed {
                    const Held& since;
                    decltype(held_by)& held_by;
                    ~Timed() { held_by(since, "checking the world prefix"); }
                } timed{since, held_by};
                if (!session.talking()) {
                    // Any sheet cached on top of the world prefix is dropped by asking for
                    // the prefix alone, so the record of what is warm has to be dropped
                    // with it or it will claim a soul is ready who is not.
                    prefiller.forget();
                    const Timing again = prefiller.warm_world(world);
                    const int cached = tokens_already_cached(again, rate);
                    // Silent when it was already there, which is almost always. A line
                    // every two minutes saying nothing happened would bury the one saying
                    // something did.
                    //
                    // Most of it rather than all of it, because an intact prefix still
                    // takes a measurable moment to hand back -- 0.03s, which at this
                    // machine's rate is a handful of tokens -- and reporting that as a loss
                    // made the message fire on every beat. A real loss is not marginal:
                    // the backend either still has the sequence or has replaced it, so
                    // what arrives here is either a few tokens' worth or nearly all of it.
                    if (rate > 0.0 && cached * 2 < again.prompt_tokens) {
                        printed("-- the world prefix had been lost (%d of %d tokens gone);"
                                    " prefilled it again in %.2fs, which is that much off the"
                                    " next conversation\n",
                                    again.prompt_tokens - cached, again.prompt_tokens,
                                    again.prefill_seconds);
                        std::fflush(stdout);
                    }
                    warmed_at = std::chrono::steady_clock::now();
                }
            }
            // The heartbeat the mod reads to know anybody is listening (turnfile.h): a new
            // file as the clock crosses into the next bucket, nothing otherwise.
            {
                const Held since = std::chrono::steady_clock::now();
                std::lock_guard<std::mutex> held(lock);
                if (quit) return;
                writer.beat(unix_now());
                held_by(since, "the heartbeat");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
        }
    });

    // Two keyboards answer here: this one, and the game's own, which the mod turns into
    // text and prints as a BRIDGE said line that the poller above answers.
    std::cout << "following the log. Type what the player says here, or in the game once"
                 " their line is up; /help for the commands.\n"
              << std::flush;
    bool asked_to_stop = false;
    std::string said;
    while (std::getline(std::cin, said)) {
        if (said == "/quit") { asked_to_stop = true; break; }
        if (said.empty()) continue;
        typing = true;
        std::lock_guard<std::mutex> held(lock);
        typing = false;
        // A slash is for the app, not for whoever Henry is talking to (console.h).
        if (Console::is_command(said)) {
            const ConsoleOutcome outcome = console.run(said);
            std::cout << outcome.text << "\n" << std::flush;
            if (outcome.lua) session.hook(*outcome.lua, outcome.label);
            continue;
        }
        if (!session.talking()) {
            std::cout << "-- nobody is listening; walk up to someone and talk to them\n";
            continue;
        }
        if (speaker) speaker->hush();
        session.say(said);
    }

    // Running out of input is not being told to stop. With no console attached --
    // which is how this runs once it is launched alongside the game rather than by
    // hand -- stdin is at end of file from the start, and treating that as a quit
    // would have the app exit before the player reached anybody. The opening line of
    // every conversation needs no typing anyway, so there is still a use in
    // following the log with nobody at the keyboard.
    if (!asked_to_stop) {
        std::cout << "no more typed input; still following the log. Stop me to quit.\n"
                  << std::flush;
        poller.join();
        return 0;
    }

    {
        std::lock_guard<std::mutex> held(lock);
        quit = true;
        writer.hush(unix_now());
    }
    poller.join();
    return 0;
}

// The model the flags asked for, or nothing and a reason why.
//
// Chosen once here rather than at each use, because nothing downstream -- the prefiller, the
// session, the terminal loop -- wants to know which of the three it got.
std::unique_ptr<Llm> open_model(const Args& args) {
    if (args.canned) return std::make_unique<CannedLlm>();

    if (args.ollama) {
        auto ollama = std::make_unique<OllamaLlm>(args.host);
        if (!ollama->reachable()) {
            std::cerr << "moonshine_app: nothing answering at " << args.host << "\n";
            return nullptr;
        }
        return ollama;
    }

#ifdef MOONSHINE_HAS_LLAMA
    std::filesystem::path gguf = args.gguf;
    if (gguf.empty()) {
        // Ollama stores models as GGUF, so a machine that has pulled this model already has a
        // file this backend can read. Shipping means downloading one on first run instead,
        // which belongs with the installer; until then there is nothing to fetch.
        gguf = LlamaLlm::find_ollama_blob(args.options.model);
        if (gguf.empty()) {
            std::cerr << "moonshine_app: no weights found for " << args.options.model
                      << ".\n  Either --gguf PATH, or `ollama pull " << args.options.model
                      << "` and this will read the blob it stores.\n";
            return nullptr;
        }
    }
    // Seconds and gigabytes, once, so it is worth saying that it is happening.
    std::cout << "loading " << gguf.filename().string() << " ..." << std::flush;
    try {
        auto llama = std::make_unique<LlamaLlm>(gguf, args.options);
        std::cout << " done\n";
        return llama;
    } catch (const std::exception& why) {
        std::cout << "\n";
        std::cerr << "moonshine_app: " << why.what() << "\n";
        return nullptr;
    }
#else
    std::cerr << "moonshine_app: built without the llama.cpp backend. Configure with "
                 "-DMOONSHINE_LLAMA=ON after `git submodule update --init`, or run with "
                 "--ollama or --canned.\n";
    return nullptr;
#endif
}

// --where: where the game is, and whether everything the app is about to open is there.
//
// The start-up check the installer's launcher runs before the game, so that a missing
// file is one line naming it rather than a conversation that opens on silence. Each
// input is found the way the app itself finds it -- the same functions, the same
// defaults, the same working directory -- so what this says is what --game would do.
// Required means the app cannot do its job without it: the game, the world text, the
// database, the model's weights (unless --canned), and, where this build speaks, the
// clones' model and at least one voice. The rest is said but not failed on: the app
// runs without the knowledge base, the ear or the mouth's lexicon, and says so itself.
//
// Exit 0 when everything required is there, 1 otherwise.
int preflight(const Args& args) {
    namespace fs = std::filesystem;
    std::error_code bad;
    bool ok = true;
    // Every line is "name:  path (verdict)", the name padded to one column.
    auto say = [](std::string_view name, const std::string& text) {
        std::cout << std::left << std::setw(11) << std::string(name) + ":" << text << "\n";
    };
    auto missing = [&](std::string_view name, const std::string& text) {
        say(name, text + " -- MISSING");
        ok = false;
    };
    auto megabytes = [&](const fs::path& path) {
        const auto size = fs::file_size(path, bad);
        if (bad) return std::string();
        return " (" + std::to_string(size / 1000000) + " MB)";
    };
    auto count_in = [&](const fs::path& dir, std::string_view suffix) {
        std::size_t n = 0;
        if (!fs::is_directory(dir, bad)) return n;
        for (const fs::directory_entry& entry : fs::directory_iterator(dir, bad)) {
            if (entry.path().extension() == suffix) ++n;
        }
        return n;
    };

    GamePaths paths = discover();
    if (!args.root.empty()) paths.root = args.root;
    if (!args.user.empty()) paths.user = args.user;
    if (!args.modid.empty()) paths.modid = args.modid;

    say("cwd", fs::current_path(bad).string());
    if (paths.root.empty()) {
        missing("game root", "not found under Steam's library folders; pass --root");
    } else {
        say("game root", paths.root.string());
    }
    say("log", log_path(paths).string() + " (" +
                   (fs::exists(log_path(paths), bad) ? "present" : "not there yet") + ")");
    say("%USER%", (paths.user.empty() ? std::string("NOT FOUND") : paths.user.string()) + " (" +
                      (fs::is_directory(paths.user, bad) ? "present" : "not there yet") + ")");
    std::cout << "turns will be written to:\n";
    for (const fs::path& path : turn_paths(paths, 1)) {
        std::cout << "  " << path << "\n";
    }

    // The words: the world text every prompt opens with, and who everybody is.
    if (fs::is_regular_file(args.world_path, bad)) {
        say("world", args.world_path + megabytes(args.world_path));
    } else {
        missing("world", args.world_path);
    }
    if (fs::is_regular_file(args.personas_path, bad)) {
        say("personas", args.personas_path + megabytes(args.personas_path));
    } else {
        missing("personas", args.personas_path);
    }

    // The model, as open_model would find it.
    if (args.canned) {
        say("model", "canned placeholder turns; no weights needed");
    } else if (args.ollama) {
        say("model", "Ollama at " + args.host + ", model " + args.options.model + " (not probed)");
    } else {
#ifdef MOONSHINE_HAS_LLAMA
        if (!args.gguf.empty()) {
            if (fs::is_regular_file(args.gguf, bad)) {
                say("model", args.gguf + megabytes(args.gguf));
            } else {
                missing("model", args.gguf);
            }
        } else {
            const fs::path blob = LlamaLlm::find_ollama_blob(args.options.model);
            if (blob.empty()) {
                missing("model", "no --gguf, and Ollama holds no blob for " + args.options.model);
            } else {
                say("model", blob.string() + megabytes(blob) + ", Ollama's blob for " +
                                 args.options.model);
            }
        }
#else
        missing("model", "this build has no llama.cpp backend; --ollama or --canned");
#endif
    }

    // The knowledge base's embedder: optional, unless asked for by path.
    {
        fs::path gguf = args.embed_gguf;
        if (gguf.empty()) gguf = LlamaEmbedder::find_weights();
        if (!gguf.empty() && fs::is_regular_file(gguf, bad)) {
            say("embedder", gguf.string() + megabytes(gguf));
        } else if (!args.embed_gguf.empty()) {
            missing("embedder", args.embed_gguf);
        } else {
            say("embedder", "none found (models/embeddinggemma-*.gguf); souls answer from the sheet alone");
        }
    }

    // The voice: the clones' model and the voices beside it. Speaker opens both.
    if (args.voice_off) {
        say("voice", "off (--no-voice)");
    } else if (!tts_built()) {
        say("voice", "this build has no voice; words stay on the screen");
    } else {
        const bool phonon = clone_engine_name() == "Phonon";
        fs::path model;
        if (clones_built()) {
            model = find_clone_model();
            if (model.empty() && phonon) {
                missing("voice", "no Phonon model (models/phonon-31m-en/model.q8.gguf, or PHONON_MODEL_DIR)");
            } else if (model.empty()) {
                missing("voice", "no Qwen3-TTS model (models/qwen3-tts-0.6b/model.safetensors, or QWEN_MODEL_DIR)");
            } else if (phonon) {
                say("voice", model.string() + ", Phonon on the CPU");
            } else {
                say("voice", model.string() + ", the Qwen3-TTS engine on the " +
                                 [] {
                                     const char* env = std::getenv("MOONSHINE_TTS_BACKEND");
                                     return std::string(env && *env ? env : "cpu");
                                 }());
            }
        } else {
            say("voice", "Kokoro through Moonshine Voice; no cloned voices in this build");
        }
        const fs::path voices = args.voices_path;
        // Phonon's voices live in its model folder; the Qwen3-TTS engine's beside the data.
        const std::size_t clones = phonon ? (model.empty() ? 0 : count_in(model / "voices", ".safetensors"))
                                          : count_in(voices / "clones", ".qvoice");
        if (!fs::is_directory(voices, bad)) {
            missing("voices", voices.string());
        } else if (clones_built() && phonon && clones == 0 && !model.empty()) {
            missing("voices", model.string() + " holds no voices/*.safetensors");
        } else if (clones_built() && !phonon && clones == 0) {
            missing("voices", voices.string() + " holds no clones/*.qvoice");
        } else {
            say("voices", (phonon ? model.string() : voices.string()) + ": " + std::to_string(clones) +
                              " voice(s), " + std::to_string(imported_voices(voices).size()) + " imported");
        }
        auto beside = [&](const char* name, const char* file_name, const char* without) {
            const fs::path file = voices / file_name;
            if (fs::is_regular_file(file, bad)) {
                say(name, file.string() + megabytes(file));
            } else {
                say(name, std::string("no ") + file_name + " beside the voices; " + without);
            }
        };
        beside("facials", "facials.txt", "the mod's own jaw beats stand in for the mouth");
        beside("lexicon", "lexicon.tsv", "the mouth's shapes are read from the words' letters");
        if (args.phrase_cache_off) {
            say("phrases", "off (--no-phrase-cache); every line is spoken live");
        } else {
            const fs::path dir = args.phrase_cache.empty() ? voices / "phrases" : args.phrase_cache;
            const std::size_t lines = count_in(dir, ".opus");
            say("phrases", dir.string() + ": " + std::to_string(lines) + " line(s) spoken ahead" +
                               (lines == 0 ? "; every line is spoken live" : ""));
        }
    }

    // The ear: optional, unless a model was named.
    if (!args.mic) {
        say("ear", "off (--no-mic); the say row is typed into");
    } else if (!listen_built()) {
        say("ear", "this build cannot hear; the say row is typed into");
    } else {
        const fs::path model = find_stt_model(args.stt_model);
        if (!model.empty()) {
            say("ear", model.string());
        } else if (!args.stt_model.empty()) {
            missing("ear", args.stt_model.string());
        } else {
            say("ear", std::string("no ") + std::string(kDefaultSttModel) +
                           " in the moonshine_voice cache; the say row is typed into");
        }
    }

    std::cout << (ok ? "ready" : "NOT READY: something required is missing, see above") << "\n";
    return ok ? 0 : 1;
}

}  // namespace

int main(int argc, char** argv) try {
    bool stop = false;
    const Args args = parse(argc, argv, &stop);
    words_only_for_shapes();
    voice_threads();
    if (stop) return 0;
    keep_full_speed();
    // Before anything is said. Leaked on purpose: cout and cerr are written until the
    // process is gone, from threads main does not join.
    if (!args.log.empty()) {
        std::FILE* file = std::fopen(args.log.string().c_str(), "w");
        if (!file) {
            std::cerr << "moonshine_app: cannot write " << args.log.string() << "\n";
            return 1;
        }
        auto* log = new LogFile{};
        log->file = file;
        std::cout.rdbuf(new LogTee(std::cout.rdbuf(), *log));
        std::cerr.rdbuf(new LogTee(std::cerr.rdbuf(), *log));
        std::cerr << "log:        " << args.log.string() << "\n";
    }

    if (args.list_voices) {
        const std::vector<ImportedVoice> voices = imported_voices(args.voices_path);
        if (voices.empty()) {
            std::cerr << "moonshine_app: no imported voices in " << args.voices_path << "\n";
            return 1;
        }
        for (const ImportedVoice& voice : voices) {
            std::cout << voice.id;
            if (!voice.about.empty()) std::cout << "\t" << voice.about;
            std::cout << "\n";
        }
        return 0;
    }

    // Ahead of the model: the line is the whole program, and loading weights to say it
    // would be most of the wait.
    if (!args.speak.empty()) {
        if (args.voice_off) {
            std::cerr << "moonshine_app: --speak needs the voice; leave off --no-voice\n";
            return 1;
        }
        const std::unique_ptr<Speaker> speaker = open_speaker(args, true);
        if (!speaker) return 1;
        if (!args.speak_out.empty()) {
            speaker->write(args.speak, args.speak_out, args.actor);
            std::cout << args.speak_out.string() << "\n";
        } else {
            speaker->say(args.speak, args.actor);
            speaker->wait();
        }
        return 0;
    }

    // The ear on its own, like --speak for the voice: listen for N seconds and print the
    // words as they change, which is how a microphone and a model are checked without
    // the game.
    if (args.hear_seconds > 0) {
        if (!listen_built()) {
            std::cerr << "moonshine_app: this build cannot hear\n";
            return 1;
        }
        const std::filesystem::path model = find_stt_model(args.stt_model);
        if (model.empty()) {
            std::cerr << "ear: no Moonshine speech model found; pass --stt-model\n";
            return 1;
        }
        try {
            const auto loading = std::chrono::steady_clock::now();
            Ear ear(model);
            std::cout << "ear: " << model.string() << " loaded in "
                      << std::chrono::duration<double>(std::chrono::steady_clock::now() - loading).count()
                      << "s; listening for " << args.hear_seconds << "s\n" << std::flush;
            ear.start();
            const auto started = std::chrono::steady_clock::now();
            std::string last;
            while (std::chrono::steady_clock::now() - started < std::chrono::seconds(args.hear_seconds)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                const std::string now = ear.text();
                if (now == last) continue;
                last = now;
                printed("[%.1fs] %s\n",
                        std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count(),
                        now.c_str());
                std::fflush(stdout);
            }
            ear.stop();
            std::cout << "heard: " << last << "\n";
        } catch (const std::exception& why) {
            std::cerr << "ear: " << why.what() << "\n";
            return 1;
        }
        return 0;
    }

    if (!args.render.empty()) {
        std::ifstream in(args.render);
        if (!in) {
            std::cerr << "moonshine_app: cannot read " << args.render << "\n";
            return 1;
        }
        const std::filesystem::path root = find_tts_root(args.tts_root);
        if (root.empty()) {
            std::cerr << "voice: no Moonshine Voice assets found. Pass --tts-root, or set\n"
                         "  MOONSHINE_TTS_ROOT to the directory that holds kokoro/ and en_gb/.\n";
            return 1;
        }
        std::unique_ptr<Speaker> speaker;
        std::string loaded;
        std::string line;
        int written = 0;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line.front() == '#') continue;
            const std::size_t tab1 = line.find('\t');
            const std::size_t tab2 = tab1 == std::string::npos ? tab1 : line.find('\t', tab1 + 1);
            if (tab1 == std::string::npos || tab2 == std::string::npos || tab2 + 1 >= line.size()) {
                throw std::runtime_error("--render lines are voice, tab, wav path, tab, text");
            }
            const std::string voice = line.substr(0, tab1);
            const std::filesystem::path wav = line.substr(tab1 + 1, tab2 - tab1 - 1);
            const std::string text = line.substr(tab2 + 1);
            if (voice != loaded) {
                speaker = std::make_unique<Speaker>(voice, args.voices_path, root);
                loaded = voice;
                std::cout << "voice: " << speaker->voice() << "\n";
            }
            speaker->write(text, wav);
            std::cout << wav.string() << "\n";
            ++written;
        }
        if (written == 0) {
            std::cerr << "moonshine_app: no lines in " << args.render << "\n";
            return 1;
        }
        return 0;
    }

    // Make the known lines and keep them, so the next conversation plays them. Ahead of
    // the model, like --speak: the line is the whole program.
    if (!args.bake_phrases.empty()) {
        if (args.voice_off) {
            std::cerr << "moonshine_app: --bake-phrases needs the voice; leave off --no-voice\n";
            return 1;
        }
        const std::unique_ptr<Speaker> speaker = open_speaker(args, true);
        if (!speaker) return 1;
        const std::filesystem::path dir = phrase_cache_dir(args, true);
        PhraseCache cache(dir);
        std::ifstream in(args.bake_phrases);
        if (!in) {
            std::cerr << "moonshine_app: cannot read " << args.bake_phrases << "\n";
            return 1;
        }
        std::string line;
        int written = 0;
        int failed = 0;
        double ratio_sum = 0;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line.front() == '#') continue;
            const std::size_t tab = line.find('\t');
            if (tab == std::string::npos || tab + 1 >= line.size()) {
                throw std::runtime_error("--bake-phrases lines are actor, tab, text");
            }
            const std::string actor = line.substr(0, tab);
            const std::string text = line.substr(tab + 1);
            try {
                std::cout << "baking: " << actor << "  " << text.substr(0, 72)
                          << (text.size() > 72 ? "..." : "") << "\n"
                          << std::flush;
                const Speaker::RenderedLine spoken = speaker->render(text, actor);
                const StoredPhrase stored =
                    cache.store(actor, text, spoken.samples.data(), spoken.samples.size(), spoken.rate);
                const double ratio = phrase_compression(stored.bytes, spoken.samples.size(), spoken.rate);
                const double seconds = static_cast<double>(spoken.samples.size()) / spoken.rate;
                std::cout << "  " << std::fixed << std::setprecision(1) << seconds << "s, "
                          << stored.bytes << " bytes, " << std::setprecision(1) << ratio
                          << "x versus 16 kHz PCM\n"
                          << std::flush;
                ratio_sum += ratio;
                ++written;
            } catch (const std::exception& why) {
                std::cerr << "voice: " << why.what() << "\n";
                ++failed;
            }
        }
        if (written == 0) {
            std::cerr << "moonshine_app: no lines baked from " << args.bake_phrases << "\n";
            return 1;
        }
        std::cout << "cached " << written << " lines in " << dir.string() << ", mean "
                  << std::fixed << std::setprecision(1) << (ratio_sum / written)
                  << "x versus 16 kHz PCM\n";
        return failed == 0 ? 0 : 1;
    }

    if (!args.soul.empty()) {
        PersonaDb personas(args.personas_path);
        if (!personas.ok()) {
            std::cerr << "moonshine_app: cannot read " << personas.path() << ": "
                      << personas.trouble() << "\n";
            return 1;
        }
        std::optional<Soul> found = personas.find(args.soul);
        if (!found) {
            if (args.json) std::cout << "{\"found\":false,\"soul\":\"" << args.soul << "\"}\n";
            else std::cout << args.soul << " is not in " << personas.path() << "\n";
            return 1;
        }
        if (!args.quest_id.empty()) {
            // The named favour first, which is where --quest-state and --have are read; the
            // rest keep their order. The sheet is then planned as it would be with the save
            // saying this of that favour and nothing of the others.
            auto it = std::find_if(found->quests.begin(), found->quests.end(),
                                   [&](const Quest& q) { return q.id == args.quest_id; });
            if (it == found->quests.end()) {
                std::cerr << "moonshine_app: " << found->soul << " is not party to quest "
                          << args.quest_id << "\n";
                return 1;
            }
            std::rotate(found->quests.begin(), it, it + 1);
            found->quest = found->quests.front();
        }
        // The memory as a save would have dumped it, read through the same classifier the
        // game's log goes through, so what is inspected is what a conversation would get. The
        // record's --met is implied: a record that says they have met is a reunion.
        std::string memory;
        Approach approach = args.approach;
        QuestFacts facts = args.facts;
        // The standing a save would give: --met stands for a second meeting when there is no
        // record to count from, and the quest state given on the command line is the soul's own.
        Standing standing;
        standing.met = approach.met ? 2 : 1;
        if (!args.memory_path.empty()) {
            const SoulRecord record = record_from_lines(read_file(args.memory_path));
            if (record.soul != found->soul) {
                std::cerr << "moonshine_app: " << args.memory_path << " is a record of \""
                          << record.soul << "\", not " << found->soul << "\n";
                return 1;
            }
            if (record.met > 1) approach.met = true;
            if (found->quest.exists() && found->quest.id == record.quest) facts = record.facts;
            memory = memory_block(record, kPlayer);
            standing = standing_of(record);
        }
        if (found->quest.exists()) {
            const char* stage = facts.done ? "done" : facts.handed ? "handed" : facts.accepted ? "accepted" : nullptr;
            if (stage != nullptr) standing.quests[found->quest.id] = stage;
            if (!facts.how.empty()) standing.hows[found->quest.id] = facts.how;
            if (!args.before_how.empty()) {
                // The scene before this one, as the save would have it: done, and ended the
                // way given (`truth` is the plain ending, which the store records as no how).
                if (found->quest.after_quest_id.empty()) {
                    std::cerr << "moonshine_app: quest " << found->quest.id
                              << " follows nothing; --before has no scene to speak of\n";
                    return 1;
                }
                standing.quests[found->quest.after_quest_id] = "done";
                if (args.before_how != "truth") standing.hows[found->quest.after_quest_id] = args.before_how;
                // And the scenes before that one, the whole way up, and this giver's earlier
                // scenes of the same story that the same gate opened: a save that has the scene
                // before done has them done too, and without them the plan offers the lowest
                // unplayed scene instead of this one (the quests are ordered by scene).
                std::string before = found->quest.after_quest_id;
                for (std::size_t guard = 0; guard < found->quests.size() && !before.empty(); ++guard) {
                    std::string next;
                    for (const Quest& q : found->quests) {
                        if (q.id == before) next = q.after_quest_id;
                    }
                    if (!next.empty()) standing.quests[next] = "done";
                    before = next;
                }
                for (const Quest& q : found->quests) {
                    if (q.id != found->quest.id && q.giver_soul == found->quest.giver_soul && q.chain_order > 0 &&
                        found->quest.chain_order > 0 && q.chain_order < found->quest.chain_order &&
                        !standing.quests.count(q.id)) {
                        standing.quests[q.id] = "done";
                    }
                }
            }
        }
        if (args.json) {
            // Everything the playground needs to put the same words to a model that this
            // app would, and nothing it would have to assemble for itself. A playground
            // that rebuilds the system prompt is a playground that can be wrong about it,
            // and being wrong about it is the one thing it must not be.
            const std::string world = read_file(args.world_path);
            std::cout << persona_json(*found, world, approach, personas.doing(args.doing),
                                      args.options, args.least_chars, args.say_written,
                                      facts, memory, standing);
            return 0;
        }
        const Persona persona = persona_for(*found, approach, personas.doing(args.doing),
                                            args.say_written, facts, memory, standing);
        std::cout << "soul:       " << found->soul << "\n";
        std::cout << "name:       " << (found->name.empty() ? "(never renamed)" : found->name)
                  << "\n";
        std::cout << "occupation: " << found->occupation << " (" << found->social_class << ")\n";
        std::cout << "place:      "
                  << (found->settlement.empty() ? "(none that is a real place)"
                                                : found->settlement)
                  << ", " << found->region << "\n";
        std::cout << "bio:        " << (found->bio.empty() ? "not written yet" : "written")
                  << "\n";
        std::cout << "card:       " << (found->card.empty() ? "not written yet" : "written")
                  << "\n";
        std::cout << "kin:        " << found->kin.size() << " named\n";
        // What the menu's own rows would be answered with, from the database.
        {
            const Scripted& written = persona.scripted;
            std::cout << "trust:      " << trust_word(trust_tier(standing, facts)) << "\n";
            if (!written.offer.empty()) {
                std::cout << "help:       (the offer) " << written.offer << "\n";
                std::cout << "accept row: " << written.accept_row << "\n";
            } else if (!written.help.empty()) {
                std::cout << "help:       " << written.help << "\n";
            }
            if (!written.about.empty()) {
                std::cout << "about:      " << (written.about_told ? "(asked again) " : "")
                          << written.about << "\n";
            }
            // A topic about a person says whom, since the row waits on Henry knowing of them
            // (Session::knows_of); the sheet alone cannot say whether he does.
            for (const Topic& topic : written.topics) {
                std::cout << "ask:        " << topic.key << " (" << topic.label << ") "
                          << (topic.about.empty() ? "" : "[about " + topic.about + ", once heard of] ")
                          << topic.text << "\n";
            }
            for (const Remark& remark : written.remarks) {
                std::cout << "say:        " << remark.key << " \"" << remark.row << "\" -> "
                          << remark.text << "\n";
            }
        }
        // Every favour the soul is party to, and which one this conversation would be about.
        for (const Quest& quest : found->quests) {
            const bool chosen = quest.id == persona.quest_of.id;
            std::cout << (chosen ? "quest:      " : "also:       ") << quest.id << ", "
                      << (quest.is_travel()
                              ? (quest.role == QuestRole::Giver ? "wants a look taken at " + quest.target_name
                                                                : "is looked at for " + quest.giver_name)
                          : quest.is_beat()
                              ? (quest.role == QuestRole::Giver ? "wants amends from " + quest.target_name
                                                                : "must make it right with " + quest.giver_name)
                          : quest.is_pray()
                              ? "wants a prayer said at " + (quest.spot_text.empty() ? std::string("a place") : quest.spot_text)
                          : quest.is_spot()
                              ? "wants " + quest.item_name + (quest.is_return() ? " put back at " : " left at ") +
                                    (quest.spot_text.empty() ? std::string("a place") : quest.spot_text)
                          : quest.is_mind()
                              ? (quest.role == QuestRole::Giver ? "wants " + quest.target_name + " looked in on, " + std::to_string(quest.count) + " visits"
                                                                : "is looked in on for " + quest.giver_name)
                          : quest.is_ask()
                              ? (quest.role == QuestRole::Giver ? "wants the truth out of " + quest.target_name
                                                                : "keeps a truth from " + quest.giver_name)
                          : quest.is_ambush()
                              ? (quest.role == QuestRole::Giver ? "wants the way out of " + quest.target_name
                                                                : "gives a false way for " + quest.giver_name)
                          : quest.is_buy()
                              ? (quest.role == QuestRole::Giver ? "sends for a " + quest.item_name + " from " + quest.target_name
                                                                : "sells a " + quest.item_name + " to " + quest.giver_name)
                              : (quest.role == QuestRole::Giver ? "asks for " : "holds ") + quest.item_name)
                      << " ("
                      << quest.giver_name << " -> " << quest.target_name << ", "
                      << (quest.pays_money() ? std::to_string(quest.reward_groschen) + " groschen"
                                             : quest.reward_text.empty() ? "no money"
                                                                         : quest.reward_text)
                      << (quest.name.empty() ? ", no text written yet" : ", '" + quest.name + "'")
                      << (quest.chain_id.empty() ? "" : ", link " + std::to_string(quest.chain_order) + " of " + quest.chain_id)
                      << (quest.requires_quest_id.empty() ? "" : ", waits on " + quest.requires_quest_id)
                      << (quest.after_quest_id.empty() || !quest.chain_id.empty() ? ""
                          : ", after " + quest.after_quest_id + (quest.after_how.empty() ? "" : " ended " + quest.after_how))
                      << (quest.dispute ? ", a dispute" : "")
                      << ")\n";
        }
        if (persona.quest_of.exists()) {
            const Quest& quest = persona.quest_of;
            if (!persona.quest.question.empty()) {
                std::cout << "listening:  " << persona.quest.question << " -> "
                          << persona.quest.on_yes << "\n";
            }
            if (!persona.quest.apologise_row.empty()) {
                std::cout << "apologise:  " << persona.quest.apologise_row << "\n"
                          << "  answer:   " << persona.quest.apologise_reply << " -> "
                          << persona.quest.apologise_command << "\n";
            }
            for (const DirectorRow& row : persona.quest.rows) {
                std::cout << row.intent << ":" << std::string(row.intent.size() < 11 ? 11 - row.intent.size() : 1, ' ')
                          << row.text << "\n"
                          << "  answer:   " << row.reply << " -> " << row.command << "\n";
            }
            for (const std::string& command : persona.quest.opening_commands) {
                std::cout << "owed:       " << quest_directive(quest, command) << "\n";
            }
            if (!persona.quest.greeting.empty()) {
                std::cout << "opens on:   (written for this visit) " << persona.quest.greeting << "\n";
            } else if (!persona.quest.opening_commands.empty()) {
                std::cout << "opens on:   nothing written for a settling visit, so the model\n";
            }
            if (!quest.greet_owed.empty()) std::cout << "greet_owed: " << quest.greet_owed << "\n";
            if (!quest.greet_settle_unasked.empty()) {
                std::cout << "greet_settle_unasked: " << quest.greet_settle_unasked << "\n";
            }
            if (!quest.greet_rebuffed.empty()) std::cout << "greet_rebuffed: " << quest.greet_rebuffed << "\n";
            if (!quest.named_row.empty()) std::cout << "named_row: " << quest.named_row << "\n";
            if (!quest.named_reply.empty()) std::cout << "named_reply: " << quest.named_reply << "\n";
            if (!quest.give_up_reply.empty()) std::cout << "give_up_reply: " << quest.give_up_reply << "\n";
        }
        std::cout << "\n--- character sheet, " << persona.sheet.size() << " chars ---\n";
        std::cout << persona.sheet << "\n";

        // Both lines, because the question this answers is "what has this character got to
        // say", and whether they recognise the player is half of it.
        if (!found->greeting_first.empty() || !found->greeting_again.empty()) {
            std::cout << "\n--- what they say first"
                      << (args.say_written ? "" : " (NOT SAID: written greetings are off)")
                      << " ---\n";
            std::cout << "  to a stranger:            " << found->greeting_first << "\n";
            std::cout << "  to somebody they have met: " << found->greeting_again << "\n";
        }
        // A line of the player's against what they know: the ranking, and the direction the
        // session would add to it, at the standing asked for. The quest comes first here as
        // it does in the session.
        if (!args.ask.empty()) {
            std::cout << "\n--- \"" << args.ask << "\" ---\n";
            Knowing knowing = open_knowledge(args);
            if (!knowing.director) return 1;
            const Trust tier = args.standing == "known" ? Trust::Confided
                               : args.standing == "vouched" ? Trust::Friendly
                                                            : Trust::Open;
            const auto began = Clock::now();
            const std::size_t known = knowing.director->open(found->soul);
            printed("knows %zu things, read in %.0f ms\n", known,
                        seconds_since(began) * 1000.0);
            float ask_score = 0.0f;
            const auto asked_at = Clock::now();
            const bool offering = knowing.director->is_quest_ask(args.ask, &ask_score);
            printed("offers help: %s (%.2f against the asks, in %.0f ms)\n",
                        offering ? "yes" : "no", static_cast<double>(ask_score),
                        seconds_since(asked_at) * 1000.0);
            const auto ranked_at = Clock::now();
            const std::vector<KbHit> hits = knowing.director->rank(found->soul, args.ask);
            const double ranked_ms = seconds_since(ranked_at) * 1000.0;
            for (std::size_t i = 0; i < hits.size() && i < 8; ++i) {
                printed("  %.3f %-9s %-22s %s\n", static_cast<double>(hits[i].score),
                            hits[i].item->trust.c_str(), hits[i].item->item_id.c_str(),
                            hits[i].via.substr(0, 70).c_str());
            }
            printed("ranked in %.0f ms\n", ranked_ms);
            Advice advice;
            if (offering) {
                const Quest& quest = persona.quest_of;
                const AskState state = !quest.exists() || quest.role != QuestRole::Giver ? AskState::None
                                       : persona.quest.question.empty() ? AskState::Settled
                                                                        : AskState::Fresh;
                advice = quest_ask_direction(quest.id, quest.offer, quest.reward_groschen, state);
            } else {
                // Through the giver's gate, as the session puts it (Session::advise).
                const Quest& quest = persona.quest_of;
                const bool giver = quest.exists() && quest.role == QuestRole::Giver;
                advice = gate_knowledge(knowing.director->advise(found->soul, args.ask, tier, {}),
                                        persona.scripted.kb_gate, giver ? quest.id : std::string(),
                                        giver ? quest.offer : std::string(), false,
                                        persona.scripted.remind);
            }
            std::cout << "knowledge:  " << kb_gate_word(persona.scripted.kb_gate) << "\n";
            std::cout << "direction:  " << advice.note << "\n  " << advice.direction << "\n";
        }
        return 0;
    }

    if (args.where) return preflight(args);

    const std::string world = read_file(args.world_path);
    Persona persona{
        "Cross-eyed Adam",
        args.sheet_path.empty() ? std::string(kDefaultSheet) : read_file(args.sheet_path),
    };
    // --actor picks the cloned voice here as it does for --speak; without it the terminal
    // speaks in the Kokoro voice, where that is built in, and otherwise stays quiet.
    persona.actor = args.actor;

    const std::unique_ptr<Llm> model = open_model(args);
    if (!model) return 1;
    Llm& llm = *model;
    std::cout << llm.describe();
    // llama.cpp names the weights file itself, and the canned backend has no model at all.
    if (args.ollama) std::cout << ", model " << args.options.model;
    std::cout << "\n";
    std::cout << "world " << world.size() << " chars\n";

    // Neither the game nor the terminal: the playground brings its own souls, through the
    // same app, and asks for words over a socket.
    if (args.serve_port > 0) return serve(llm, args.options, world, args.serve_port);

    // After the model, which is the long wait, so a voice that fails to open is a line
    // of its own. The playground never speaks: it has no ear, and the cores stay with
    // the replies it is timing.
    if (args.game) share_the_gpu();
    const std::unique_ptr<Speaker> speaker = open_speaker(args, false);
    // The voice and the model share the cores, and a line half spoken is the one the player
    // is listening to: while one is being made or played the model waits (llm.h).
    if (speaker) {
        Speaker* voice = speaker.get();
        llm.stand_aside_while([voice] { return voice->speaking(); });
        // A line to nobody now, so the engines' first use is paid while the game is still
        // loading and not on the first thing a soul says (voice.h, Speaker::warm). On the
        // voice's own thread; the model's start-up warm below waits for it as it would
        // for a line, which is a few seconds at a moment nobody is listening.
        if (args.game) speaker->warm();
    }

    // Past here the sheet below is what gets talked to, so it is only worth mentioning
    // in the terminal: with the game, every NPC brings their own out of the database.
    if (args.game) return run_with_game(args, llm, world, speaker.get());

    std::cout << "sheet " << persona.sheet.size() << " chars\n";

    Prefiller prefiller(llm, args.options);

    // Start-up. On the machine this was measured on, the first of these is about
    // 6s and the second about 4s, and both would otherwise land on the player's
    // opening line while they stood waiting for an answer.
    auto began = Clock::now();
    const Timing world_warm = prefiller.warm_world(world);
    std::cout << "prefilled the world in " << seconds_since(began) << "s ("
              << world_warm.prompt_tokens << " tokens)\n";

    began = Clock::now();
    const Timing persona_warm = prefiller.warm_persona(world, persona);
    std::cout << "prefilled " << persona.name << " in " << seconds_since(began) << "s ("
              << persona_warm.prompt_tokens << " tokens)\n\n";

    std::vector<Message> history;
    std::string said;
    std::cout << "you> " << std::flush;
    while (std::getline(std::cin, said)) {
        if (said.empty()) { std::cout << "you> " << std::flush; continue; }
        if (said == "/quit") break;
        if (speaker) speaker->hush();

        const std::vector<Message> messages = conversation(world, persona, history, said);
        SentenceBuffer buffer(args.least_chars);

        began = Clock::now();
        double first_audio = -1.0;
        const Timing timing = llm.chat(messages, args.options, [&](std::string_view delta) {
            for (const std::string& chunk : buffer.push(delta)) {
                // Where the voice would start. Printed as it happens so the order
                // and the wait are both visible.
                if (first_audio < 0.0) first_audio = seconds_since(began);
                std::cout << persona.name << "> " << chunk << "\n";
                if (speaker) speaker->say(chunk, persona.actor);
            }
        });
        if (auto last = buffer.flush()) {
            if (first_audio < 0.0) first_audio = seconds_since(began);
            std::cout << persona.name << "> " << *last << "\n";
            if (speaker) speaker->say(*last, persona.actor);
        }

        // Cached tokens said only when the backend knows, which in-process it does. Under
        // Ollama the figure has to be inferred from the elapsed time, and that inference
        // belongs where there is a measured rate to make it against -- the game loop.
        std::string reused;
        if (timing.cached_tokens >= 0) {
            reused = ", " + std::to_string(timing.cached_tokens) + " of them cached";
        }
        const double decode_rate = timing.decode_seconds > 0.0
                                       ? timing.reply_tokens / timing.decode_seconds
                                       : 0.0;
        std::string aside;
        if (timing.held_seconds > 0.05) {
            aside = ", stood aside " + std::to_string(timing.held_seconds).substr(0, 4) +
                    "s while the voice spoke";
        }
        printed("    [first audio %.2fs, whole reply %.2fs, prefill %d tok in %.2fs%s,"
                    " %d tok out at %.0f tok/s%s]\n",
                    first_audio, seconds_since(began), timing.prompt_tokens,
                    timing.prefill_seconds, reused.c_str(), timing.reply_tokens, decode_rate,
                    aside.c_str());
        // Two different failures look the same in the timing and are worth telling
        // apart. On the opening line, a slow prefill means the pre-warm was not
        // reused, which is what a model whose layers are not all full attention
        // does: reusing a prefix the player's words branch away from needs the
        // cache rolled back to the branch point, and a recurrent state cannot be.
        // On any later line it means prefix reuse has stopped working altogether.
        if (timing.prefill_seconds > args.warn_prefill_seconds) {
            std::cout << (history.empty()
                    ? "    [the pre-warm was not reused, so this model pays for the "
                      "character sheet on the opening line]\n"
                    : "    [warning: prefix reuse has stopped; every line is now "
                      "paying for the whole prompt]\n");
        }

        history.push_back({"user", said});
        history.push_back({"assistant", buffer.text()});
        std::cout << "you> " << std::flush;
    }
    return 0;
} catch (const std::exception& e) {
    std::cerr << "moonshine_app: " << e.what() << "\n";
    return 1;
}
