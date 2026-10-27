/* qwen_tts_voice_file.c - see the header. The parser follows main.c's --load-voice path
 * for .qvoice versions 1-3 exactly, except that what main.c wrote straight into the
 * context is kept in a qwen_voice_t so several can be applied in turn. */
#include "qwen_tts_voice_file.h"
#include "qwen_tts.h"
#include "qwen_tts_kernels.h"

#include <math.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct qwen_voice {
    qwen_voice_engine_t *engine;
    int enc_dim;
    float *embedding;        /* enc_dim */
    char *ref_text;          /* may be NULL */
    int n_ref_frames;
    int *ref_codes;          /* n_ref_frames * 16, or NULL */
    int language_id;         /* 0 when absent */
    char name[64];
    float ref_seconds;
    /* Source-model tables (WOVR), NULL when the file has none. */
    uint16_t *fc1; float *fc1_b;
    uint16_t *fc2; float *fc2_b;
    uint16_t *codec_full;    /* the model's codec table with the file's rows merged in */
    /* tts_pad/bos/eos (TPAD, or recomputed from WOVR), NULL when neither was present. */
    float *pad, *bos, *eos;
};

struct qwen_voice_engine {
    qwen_tts_ctx_t *ctx;
    int silent;
    int enc_dim;
    /* The model's own tables, to put back between voices and before unloading. */
    uint16_t *fc1; float *fc1_b;
    uint16_t *fc2; float *fc2_b;
    uint16_t *codec;
    float *pad, *bos, *eos;  /* copies of the model's, hidden_size each */
    int language_id;
    qwen_voice_t *current;
};

static void set_err(char *err, size_t errsz, const char *fmt, ...) {
    if (!err || !errsz) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, errsz, fmt, ap);
    va_end(ap);
}

static float *copy_floats(const float *src, int n) {
    if (!src) return NULL;
    float *d = (float *)malloc((size_t)n * sizeof(float));
    if (d) memcpy(d, src, (size_t)n * sizeof(float));
    return d;
}

/* ---- engine -------------------------------------------------------------------------- */

qwen_voice_engine_t *qwen_voice_engine_open(const char *model_dir, int threads, int bits,
                                            int silent, char *err, size_t errsz) {
    if (threads > 0) {
        qwen_set_threads(threads);
    } else {
        int n = qwen_get_num_cpus();
        qwen_set_threads(n < 8 ? n : 8);
    }
    if (!silent) qwen_caps_report(stderr);
    qwen_tts_ctx_t *ctx = qwen_tts_load_ex(model_dir, silent, bits == 8, bits == 4);
    if (!ctx) {
        set_err(err, errsz, "could not load the model in %s", model_dir);
        return NULL;
    }
    qwen_voice_engine_t *e = (qwen_voice_engine_t *)calloc(1, sizeof *e);
    if (!e) {
        qwen_tts_unload(ctx);
        set_err(err, errsz, "out of memory");
        return NULL;
    }
    e->ctx = ctx;
    e->silent = silent;
    e->enc_dim = ctx->speaker_enc.enc_dim > 0 ? ctx->speaker_enc.enc_dim : ctx->config.hidden_size;
    e->fc1 = ctx->text_proj_fc1_bf16;
    e->fc1_b = ctx->text_proj_fc1_bias;
    e->fc2 = ctx->text_proj_fc2_bf16;
    e->fc2_b = ctx->text_proj_fc2_bias;
    e->codec = ctx->codec_embedding_bf16;
    const int h = ctx->config.hidden_size;
    e->pad = copy_floats(ctx->cached_tts_pad_embed, h);
    e->bos = copy_floats(ctx->cached_tts_bos_embed, h);
    e->eos = copy_floats(ctx->cached_tts_eos_embed, h);
    e->language_id = ctx->language_id;
    /* One buffer the context owns and frees; each voice is copied into it. */
    ctx->speaker_embedding = (float *)calloc((size_t)e->enc_dim, sizeof(float));
    if (!ctx->speaker_embedding) {
        qwen_tts_unload(ctx);
        free(e->pad); free(e->bos); free(e->eos); free(e);
        set_err(err, errsz, "out of memory");
        return NULL;
    }
    return e;
}

static void emb_cache_reset(qwen_tts_ctx_t *ctx) {
    for (int i = 0; i < ctx->emb_cache.capacity; i++) ctx->emb_cache.keys[i] = -1;
    ctx->emb_cache.count = 0;
}

/* Point the context at the model's own tables again. */
static void restore_model(qwen_voice_engine_t *e) {
    qwen_tts_ctx_t *ctx = e->ctx;
    const int h = ctx->config.hidden_size;
    int tables_changed = ctx->text_proj_fc1_bf16 != e->fc1 || ctx->text_proj_fc2_bf16 != e->fc2 ||
                         ctx->codec_embedding_bf16 != e->codec;
    ctx->text_proj_fc1_bf16 = e->fc1;
    ctx->text_proj_fc1_bias = e->fc1_b;
    ctx->text_proj_fc2_bf16 = e->fc2;
    ctx->text_proj_fc2_bias = e->fc2_b;
    ctx->codec_embedding_bf16 = e->codec;
    if (e->pad && ctx->cached_tts_pad_embed) memcpy(ctx->cached_tts_pad_embed, e->pad, (size_t)h * sizeof(float));
    if (e->bos && ctx->cached_tts_bos_embed) memcpy(ctx->cached_tts_bos_embed, e->bos, (size_t)h * sizeof(float));
    if (e->eos && ctx->cached_tts_eos_embed) memcpy(ctx->cached_tts_eos_embed, e->eos, (size_t)h * sizeof(float));
    free(ctx->ref_text); ctx->ref_text = NULL;
    free(ctx->cached_ref_codes); ctx->cached_ref_codes = NULL;
    ctx->cached_ref_n_frames = 0;
    ctx->voice_clone = 0;
    ctx->xvector_only = 0;
    ctx->language_id = e->language_id;
    if (tables_changed) emb_cache_reset(ctx);
    e->current = NULL;
}

void qwen_voice_engine_close(qwen_voice_engine_t *e) {
    if (!e) return;
    restore_model(e);
    qwen_tts_unload(e->ctx);
    free(e->pad); free(e->bos); free(e->eos);
    free(e);
}

/* ---- voice ---------------------------------------------------------------------------- */

static int read_u32(FILE *f, uint32_t *v) { return fread(v, sizeof *v, 1, f) == 1; }

static int skip_bytes(FILE *f, long long n) {
    while (n > 0) {
        long step = n > 0x40000000LL ? 0x40000000L : (long)n;
        if (fseek(f, step, SEEK_CUR) != 0) return 0;
        n -= step;
    }
    return 1;
}

static float bf16_to_f32(uint16_t b) {
    uint32_t bits = (uint32_t)b << 16;
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

/* tts_pad/bos/eos through the source model's text projection, as main.c does when the
 * file carries WOVR but no TPAD: embed(token) -> fc1 -> SiLU -> fc2. */
static int recompute_special_embeds(qwen_voice_t *v, const qwen_tts_ctx_t *ctx) {
    const int h = ctx->config.hidden_size, th = ctx->config.text_hidden_size;
    const int ids[3] = {151671, 151672, 151673};
    float *outs[3];
    v->pad = (float *)malloc((size_t)h * sizeof(float));
    v->bos = (float *)malloc((size_t)h * sizeof(float));
    v->eos = (float *)malloc((size_t)h * sizeof(float));
    float *tmp1 = (float *)malloc((size_t)th * sizeof(float));
    float *tmp2 = (float *)malloc((size_t)th * sizeof(float));
    if (!v->pad || !v->bos || !v->eos || !tmp1 || !tmp2) {
        free(tmp1); free(tmp2);
        return -1;
    }
    outs[0] = v->pad; outs[1] = v->bos; outs[2] = v->eos;
    for (int t = 0; t < 3; t++) {
        const uint16_t *emb = ctx->tok_embeddings_bf16 + (int64_t)ids[t] * th;
        for (int j = 0; j < th; j++) tmp1[j] = bf16_to_f32(emb[j]);
        for (int i = 0; i < th; i++) {
            float sum = v->fc1_b[i];
            const uint16_t *row = v->fc1 + (size_t)i * th;
            for (int j = 0; j < th; j++) sum += bf16_to_f32(row[j]) * tmp1[j];
            tmp2[i] = sum / (1.0f + expf(-sum));
        }
        for (int i = 0; i < h; i++) {
            float sum = v->fc2_b[i];
            const uint16_t *row = v->fc2 + (size_t)i * th;
            for (int j = 0; j < th; j++) sum += bf16_to_f32(row[j]) * tmp2[j];
            outs[t][i] = sum;
        }
    }
    free(tmp1); free(tmp2);
    return 0;
}

static void voice_free_contents(qwen_voice_t *v) {
    free(v->embedding);
    free(v->ref_text);
    free(v->ref_codes);
    free(v->fc1); free(v->fc1_b); free(v->fc2); free(v->fc2_b); free(v->codec_full);
    free(v->pad); free(v->bos); free(v->eos);
}

qwen_voice_t *qwen_voice_open(qwen_voice_engine_t *e, const char *path, char *err, size_t errsz) {
    if (!e || !path) {
        set_err(err, errsz, "no engine or path");
        return NULL;
    }
    qwen_tts_ctx_t *ctx = e->ctx;
    const int h = ctx->config.hidden_size, th = ctx->config.text_hidden_size;
    FILE *f = fopen(path, "rb");
    if (!f) {
        set_err(err, errsz, "cannot open %s", path);
        return NULL;
    }
    qwen_voice_t *v = (qwen_voice_t *)calloc(1, sizeof *v);
    if (!v) {
        fclose(f);
        set_err(err, errsz, "out of memory");
        return NULL;
    }
    v->engine = e;
    v->enc_dim = e->enc_dim;

    char magic[4];
    uint32_t version = 0;
    if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "QVCE", 4) != 0) {
        set_err(err, errsz, "%s is not a .qvoice file", path);
        goto fail;
    }
    if (!read_u32(f, &version) || version < 1 || version > 3) {
        set_err(err, errsz, "%s: unsupported .qvoice version %u", path, version);
        goto fail;
    }
    int file_enc_dim = e->enc_dim;
    if (version >= 2) {
        uint32_t d;
        if (!read_u32(f, &d)) {
            set_err(err, errsz, "%s: truncated before enc_dim", path);
            goto fail;
        }
        file_enc_dim = (int)d;
    }
    if (file_enc_dim != e->enc_dim) {
        set_err(err, errsz, "%s has enc_dim %d but the model expects %d", path, file_enc_dim, e->enc_dim);
        goto fail;
    }
    v->embedding = (float *)malloc((size_t)e->enc_dim * sizeof(float));
    if (!v->embedding || fread(v->embedding, sizeof(float), (size_t)e->enc_dim, f) != (size_t)e->enc_dim) {
        set_err(err, errsz, "%s: truncated speaker embedding", path);
        goto fail;
    }
    uint32_t ref_text_len;
    if (!read_u32(f, &ref_text_len)) {
        set_err(err, errsz, "%s: truncated before ref text", path);
        goto fail;
    }
    if (ref_text_len > 0) {
        if (ref_text_len > (1u << 20)) {
            set_err(err, errsz, "%s: ref text length %u is not plausible", path, ref_text_len);
            goto fail;
        }
        v->ref_text = (char *)malloc(ref_text_len + 1);
        if (!v->ref_text || fread(v->ref_text, 1, ref_text_len, f) != ref_text_len) {
            set_err(err, errsz, "%s: truncated ref text", path);
            goto fail;
        }
        v->ref_text[ref_text_len] = '\0';
    }
    uint32_t n_ref_frames;
    if (!read_u32(f, &n_ref_frames)) {
        set_err(err, errsz, "%s: truncated before ref frames", path);
        goto fail;
    }
    if (n_ref_frames > 0) {
        if (n_ref_frames > 1000000u) {
            set_err(err, errsz, "%s: ref frame count %u is not plausible", path, n_ref_frames);
            goto fail;
        }
        size_t n_codes = (size_t)n_ref_frames * 16;
        v->ref_codes = (int *)malloc(n_codes * sizeof(int));
        if (!v->ref_codes || fread(v->ref_codes, sizeof(int), n_codes, f) != n_codes) {
            set_err(err, errsz, "%s: truncated ref codes", path);
            goto fail;
        }
        v->n_ref_frames = (int)n_ref_frames;
    }

    if (version >= 3) {
        char tag[4];
        /* META */
        if (fread(tag, 1, 4, f) == 4 && memcmp(tag, "META", 4) == 0) {
            uint32_t lang_id = 0, model_size = 0, meta_enc_dim = 0, flags = 0;
            char lang[16] = {0};
            float ref_dur = 0;
            if (!read_u32(f, &lang_id) || fread(lang, 1, 16, f) != 16 || !read_u32(f, &model_size) ||
                !read_u32(f, &meta_enc_dim) || fread(&ref_dur, sizeof ref_dur, 1, f) != 1 ||
                fread(v->name, 1, 64, f) != 64 || !read_u32(f, &flags)) {
                set_err(err, errsz, "%s: truncated META", path);
                goto fail;
            }
            v->name[63] = '\0';
            v->language_id = (int)lang_id;
            v->ref_seconds = ref_dur;
            if (model_size > 0 && model_size != (uint32_t)h && !e->silent)
                fprintf(stderr, "qwen voice: %s was made with hidden=%u, model has %d; cross-size\n",
                        path, model_size, h);
        } else {
            fseek(f, -4, SEEK_CUR);
        }
        /* TPAD */
        if (fread(tag, 1, 4, f) == 4 && memcmp(tag, "TPAD", 4) == 0) {
            uint32_t tpad_h;
            if (!read_u32(f, &tpad_h)) {
                set_err(err, errsz, "%s: truncated TPAD", path);
                goto fail;
            }
            if ((int)tpad_h == h && ctx->cached_tts_pad_embed) {
                v->pad = (float *)malloc((size_t)h * sizeof(float));
                v->bos = (float *)malloc((size_t)h * sizeof(float));
                v->eos = (float *)malloc((size_t)h * sizeof(float));
                if (!v->pad || !v->bos || !v->eos ||
                    fread(v->pad, sizeof(float), (size_t)h, f) != (size_t)h ||
                    fread(v->bos, sizeof(float), (size_t)h, f) != (size_t)h ||
                    fread(v->eos, sizeof(float), (size_t)h, f) != (size_t)h) {
                    set_err(err, errsz, "%s: truncated TPAD body", path);
                    goto fail;
                }
            } else if (!skip_bytes(f, 3LL * tpad_h * (long long)sizeof(float))) {
                set_err(err, errsz, "%s: truncated TPAD body", path);
                goto fail;
            }
        } else {
            fseek(f, -4, SEEK_CUR);
        }
        /* WOVR */
        if (fread(tag, 1, 4, f) == 4 && memcmp(tag, "WOVR", 4) == 0) {
            uint32_t wh, wth, wcv;
            if (!read_u32(f, &wh) || !read_u32(f, &wth) || !read_u32(f, &wcv)) {
                set_err(err, errsz, "%s: truncated WOVR", path);
                goto fail;
            }
            long long body = (long long)wth * wth * 2 + (long long)wth * 4 + (long long)wh * wth * 2 +
                             (long long)wh * 4 + (long long)wcv * wh * 2;
            if ((int)wh == h && (int)wth == th) {
                const int cv = (int)wcv;
                uint16_t *ce = (uint16_t *)malloc((size_t)cv * h * sizeof(uint16_t));
                v->fc1 = (uint16_t *)malloc((size_t)th * th * sizeof(uint16_t));
                v->fc1_b = (float *)malloc((size_t)th * sizeof(float));
                v->fc2 = (uint16_t *)malloc((size_t)h * th * sizeof(uint16_t));
                v->fc2_b = (float *)malloc((size_t)h * sizeof(float));
                if (!ce || !v->fc1 || !v->fc1_b || !v->fc2 || !v->fc2_b) {
                    free(ce);
                    set_err(err, errsz, "out of memory for %s", path);
                    goto fail;
                }
                if (fread(v->fc1, sizeof(uint16_t), (size_t)th * th, f) != (size_t)th * th ||
                    fread(v->fc1_b, sizeof(float), (size_t)th, f) != (size_t)th ||
                    fread(v->fc2, sizeof(uint16_t), (size_t)h * th, f) != (size_t)h * th ||
                    fread(v->fc2_b, sizeof(float), (size_t)h, f) != (size_t)h ||
                    fread(ce, sizeof(uint16_t), (size_t)cv * h, f) != (size_t)cv * h) {
                    free(ce);
                    set_err(err, errsz, "%s: truncated WOVR body", path);
                    goto fail;
                }
                /* The file's codec rows over the model's full table. */
                const int full = ctx->config.codec_vocab_size;
                v->codec_full = (uint16_t *)malloc((size_t)full * h * sizeof(uint16_t));
                if (!v->codec_full) {
                    free(ce);
                    set_err(err, errsz, "out of memory for %s", path);
                    goto fail;
                }
                memcpy(v->codec_full, e->codec, (size_t)full * h * sizeof(uint16_t));
                const int rows = cv < full ? cv : full;
                memcpy(v->codec_full, ce, (size_t)rows * h * sizeof(uint16_t));
                free(ce);
                if (!v->pad && ctx->cached_tts_pad_embed && recompute_special_embeds(v, ctx) != 0) {
                    set_err(err, errsz, "out of memory for %s", path);
                    goto fail;
                }
            } else if (!skip_bytes(f, body)) {
                set_err(err, errsz, "%s: truncated WOVR body", path);
                goto fail;
            }
        }
        /* WFULL / WDLT follow: talker weight deltas, deliberately unread (--icl-only). */
    }
    fclose(f);
    return v;

fail:
    fclose(f);
    voice_free_contents(v);
    free(v);
    return NULL;
}

void qwen_voice_close(qwen_voice_t *v) {
    if (!v) return;
    if (v->engine && v->engine->current == v) restore_model(v->engine);
    voice_free_contents(v);
    free(v);
}

const char *qwen_voice_name(const qwen_voice_t *v) { return v ? v->name : ""; }
int qwen_voice_ref_frames(const qwen_voice_t *v) { return v ? v->n_ref_frames : 0; }
int qwen_voice_language_id(const qwen_voice_t *v) { return v ? v->language_id : 0; }

/* ---- speaking ------------------------------------------------------------------------- */

static int apply_voice(qwen_voice_engine_t *e, qwen_voice_t *v) {
    qwen_tts_ctx_t *ctx = e->ctx;
    const int h = ctx->config.hidden_size;
    if (e->current == v) return 0;

    memcpy(ctx->speaker_embedding, v->embedding, (size_t)e->enc_dim * sizeof(float));
    free(ctx->ref_text);
    ctx->ref_text = v->ref_text ? strdup(v->ref_text) : NULL;
    free(ctx->cached_ref_codes);
    ctx->cached_ref_codes = NULL;
    ctx->cached_ref_n_frames = 0;
    if (v->n_ref_frames > 0) {
        size_t n_codes = (size_t)v->n_ref_frames * 16;
        ctx->cached_ref_codes = (int *)malloc(n_codes * sizeof(int));
        if (!ctx->cached_ref_codes) return -1;
        memcpy(ctx->cached_ref_codes, v->ref_codes, n_codes * sizeof(int));
        ctx->cached_ref_n_frames = v->n_ref_frames;
    }
    ctx->voice_clone = 1;
    ctx->xvector_only = v->n_ref_frames > 0 ? 0 : 1;
    ctx->graft_mode = 0;
    ctx->language_id = v->language_id > 0 ? v->language_id : e->language_id;

    const float *pad = v->pad ? v->pad : e->pad;
    const float *bos = v->bos ? v->bos : e->bos;
    const float *eos = v->eos ? v->eos : e->eos;
    if (pad && ctx->cached_tts_pad_embed) memcpy(ctx->cached_tts_pad_embed, pad, (size_t)h * sizeof(float));
    if (bos && ctx->cached_tts_bos_embed) memcpy(ctx->cached_tts_bos_embed, bos, (size_t)h * sizeof(float));
    if (eos && ctx->cached_tts_eos_embed) memcpy(ctx->cached_tts_eos_embed, eos, (size_t)h * sizeof(float));

    uint16_t *fc1 = v->fc1 ? v->fc1 : e->fc1;
    uint16_t *fc2 = v->fc2 ? v->fc2 : e->fc2;
    uint16_t *codec = v->codec_full ? v->codec_full : e->codec;
    int tables_changed = ctx->text_proj_fc1_bf16 != fc1 || ctx->text_proj_fc2_bf16 != fc2 ||
                         ctx->codec_embedding_bf16 != codec;
    ctx->text_proj_fc1_bf16 = fc1;
    ctx->text_proj_fc1_bias = v->fc1 ? v->fc1_b : e->fc1_b;
    ctx->text_proj_fc2_bf16 = fc2;
    ctx->text_proj_fc2_bias = v->fc2 ? v->fc2_b : e->fc2_b;
    ctx->codec_embedding_bf16 = codec;
    if (tables_changed) emb_cache_reset(ctx);

    e->current = v;
    return 0;
}

int qwen_voice_speak(qwen_voice_engine_t *e, qwen_voice_t *v, const char *text, unsigned seed,
                     float **samples, int *n_samples, char *err, size_t errsz) {
    if (samples) *samples = NULL;
    if (n_samples) *n_samples = 0;
    if (!e || !v || !text || !samples || !n_samples) {
        set_err(err, errsz, "bad arguments");
        return -1;
    }
    if (v->engine != e) {
        set_err(err, errsz, "voice belongs to a different engine");
        return -1;
    }
    if (apply_voice(e, v) != 0) {
        set_err(err, errsz, "out of memory applying voice");
        return -1;
    }
    e->ctx->seed = seed ? (uint32_t)seed : (uint32_t)time(NULL);
    float *audio = NULL;
    int n = 0;
    if (qwen_tts_generate(e->ctx, text, &audio, &n) != 0) {
        free(audio);
        set_err(err, errsz, "generation failed");
        return -1;
    }
    *samples = audio;
    *n_samples = n;
    return 0;
}

int qwen_voice_speak_stream(qwen_voice_engine_t *e, qwen_voice_t *v, const char *text,
                            unsigned seed, qwen_voice_audio_cb on_audio,
                            qwen_voice_cancel_cb cancelled, void *user, int *n_samples,
                            char *err, size_t errsz) {
    if (n_samples) *n_samples = 0;
    if (!e || !v || !text || !on_audio) {
        set_err(err, errsz, "bad arguments");
        return -1;
    }
    if (v->engine != e) {
        set_err(err, errsz, "voice belongs to a different engine");
        return -1;
    }
    if (apply_voice(e, v) != 0) {
        set_err(err, errsz, "out of memory applying voice");
        return -1;
    }
    e->ctx->seed = seed ? (uint32_t)seed : (uint32_t)time(NULL);
    /* With stream set and a callback in place the decoder thread hands each chunk to the
     * callback instead of appending it, and the generator returns no buffer of its own. The
     * same decoder path either way, so the audio is what qwen_voice_speak would have made. */
    e->ctx->stream = 1;
    e->ctx->audio_cb = on_audio;
    e->ctx->audio_cb_userdata = user;
    e->ctx->cancel_cb = cancelled;
    e->ctx->cancel_cb_userdata = user;
    float *audio = NULL;
    int n = 0;
    const int rc = qwen_tts_generate(e->ctx, text, &audio, &n);
    e->ctx->stream = 0;
    e->ctx->audio_cb = NULL;
    e->ctx->audio_cb_userdata = NULL;
    e->ctx->cancel_cb = NULL;
    e->ctx->cancel_cb_userdata = NULL;
    free(audio); /* NULL on this path; defensive */
    if (rc != 0) {
        set_err(err, errsz, "generation failed");
        return -1;
    }
    if (n_samples) *n_samples = n;
    return 0;
}

void qwen_voice_free_samples(float *samples) { free(samples); }

int qwen_voice_sample_rate(void) { return QWEN_TTS_SAMPLE_RATE; }
