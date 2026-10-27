/* qwen_tts_voice_file.h - a resident engine that speaks in any of several cloned voices.
 *
 * The command-line tool binds one .qvoice to one model load, and a load is most of the
 * cost of a line. This API lifts that binding out of main.c: open the model once, open as
 * many voices as there are speakers, and switch between them per line. A voice is the
 * parsed contents of a .qvoice file (speaker x-vector, reference transcript and codec
 * frames, and the source model's text-projection and codec-embedding tables it was made
 * with); applying one is a handful of pointer swaps and small copies, no weights move.
 *
 * Only the in-context-learning form of a voice is used, the same as the tool's --icl-only:
 * any WFULL or WDLT talker weight-deltas in the file are left unread, so the talker's own
 * weights are never touched and voices can be switched freely on one context. The prefix
 * KV cache is not consulted in clone mode, so there is nothing stale to invalidate.
 *
 * Plain C with opaque types, so it can be included from C++. Nothing here exposes the
 * engine's own headers, which do not compile as C++. Thread-safety: one engine serves one
 * caller at a time; serialise speak() externally. */
#ifndef QWEN_TTS_VOICE_FILE_H
#define QWEN_TTS_VOICE_FILE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qwen_voice_engine qwen_voice_engine_t;
typedef struct qwen_voice qwen_voice_t;

/* Load a model directory (the CustomVoice weights). threads <= 0 picks the engine's own
 * default from the core count. bits is the precision of the talker and code-predictor
 * matmul weights: 16 keeps the bf16 as stored, 8 or 4 quantizes them at load (the tool's
 * --int8 / --int4; the tables a voice file overrides are not among them). On failure
 * returns NULL with a message in err. */
qwen_voice_engine_t *qwen_voice_engine_open(const char *model_dir, int threads, int bits,
                                            int silent, char *err, size_t errsz);
void qwen_voice_engine_close(qwen_voice_engine_t *engine);

/* Parse a .qvoice and check it against the open model (x-vector width, hidden sizes). The
 * voice remains valid until closed; closing a voice that is currently applied restores
 * the model's own tables first. */
qwen_voice_t *qwen_voice_open(qwen_voice_engine_t *engine, const char *path, char *err, size_t errsz);
void qwen_voice_close(qwen_voice_t *voice);

const char *qwen_voice_name(const qwen_voice_t *voice);   /* from META, may be "" */
int qwen_voice_ref_frames(const qwen_voice_t *voice);
int qwen_voice_language_id(const qwen_voice_t *voice);    /* 0 when the file did not say */

/* Speak text in the given voice. The samples are mono float at qwen_voice_sample_rate();
 * release them with qwen_voice_free_samples(), not free(). A seed of 0 means time-based.
 * Returns 0 on success, -1 with a message in err otherwise; a line that produced no codec
 * frames is a success with *n_samples == 0. */
int qwen_voice_speak(qwen_voice_engine_t *engine, qwen_voice_t *voice, const char *text,
                     unsigned seed, float **samples, int *n_samples, char *err, size_t errsz);
void qwen_voice_free_samples(float *samples);
int qwen_voice_sample_rate(void);

/* The same, delivered as it is made. The engine decodes codec frames to audio on a thread
 * of its own while the talker is still producing the next ones, so the first chunk (two
 * frames, 160 ms) is ready long before the line is finished and each later chunk follows at
 * about the rate it is spoken. on_audio is called from that decoder thread, not the
 * caller's, with mono float samples at qwen_voice_sample_rate(); the buffer is the
 * engine's and is gone when the callback returns. Returning non-zero from on_audio stops
 * the line. cancelled, if given, is asked on the caller's thread before every talker step
 * and stops the line at the next frame when it returns non-zero: that is how a line is cut
 * off within a frame of being told to, rather than at the next chunk.
 *
 * Returns 0 when the line ran to its end or was stopped by either callback, with
 * *n_samples the count delivered; -1 with a message in err when it could not start. */
typedef int (*qwen_voice_audio_cb)(const float *samples, int n_samples, void *user);
typedef int (*qwen_voice_cancel_cb)(void *user);
int qwen_voice_speak_stream(qwen_voice_engine_t *engine, qwen_voice_t *voice, const char *text,
                            unsigned seed, qwen_voice_audio_cb on_audio,
                            qwen_voice_cancel_cb cancelled, void *user, int *n_samples,
                            char *err, size_t errsz);

#ifdef __cplusplus
}
#endif

#endif
