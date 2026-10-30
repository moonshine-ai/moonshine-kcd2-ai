Moonshine for Kingdom Come: Deliverance 2 -- what is in this folder, and whose it is

This is an early tester build and not for redistribution. The pieces below are other
people's work, shipped here under the licences named; the text of each is in this folder.

Program
  moonshine_app.exe       this project
  llama.cpp               MIT                 llama.cpp.txt        https://github.com/ggml-org/llama.cpp
  Moonshine Voice         MIT                 moonshine-voice.txt  https://github.com/moonshine-ai/moonshine
                          (moonshine.dll; the speech-to-text library)
  ONNX Runtime            MIT                 onnxruntime.txt      https://github.com/microsoft/onnxruntime
                          (onnxruntime.dll)
  Phonon runtime (xn-ptts)  MIT or Apache 2.0   xn-ptts.txt          https://github.com/gradium-ai/xn-ptts
                          (ptts_ffi.dll; the text-to-speech runtime, MIT here)
  Opus                    BSD 3-clause        opus.txt             https://opus-codec.org
  cpp-httplib, nlohmann/json, doctest, miniz
                          MIT                 (headers carry their own notices)
  SQLite                  public domain

Model weights
  models\llm\*.gguf       Qwen3 (Alibaba Cloud)              Apache 2.0   Apache-2.0.txt
                          https://huggingface.co/Qwen -- Qwen3-4B-Instruct-2507, unsloth's Q4_K_M GGUF
  models\phonon-31m-en    Phonon 31M English (Gradium)       Apache 2.0   Apache-2.0.txt
                          https://huggingface.co/gradium/phonon-31m-en -- the voice model; and
                          under voices\, twelve speaker embeddings from the Phonon voice
                          catalogue, https://huggingface.co/gradium/phonon-voices (Apache 2.0),
                          each renamed from its catalogue id to a first name
  models\stt\medium-streaming-en
                          Moonshine streaming STT (Moonshine AI)   MIT     moonshine-voice.txt
                          https://download.moonshine.ai (medium-streaming-en)
  models\embeddinggemma-300m-q8_0.gguf
                          EmbeddingGemma (Google)            Gemma Terms of Use
                          https://ai.google.dev/gemma/terms
                          Use of this file is subject to the Gemma Terms of Use and the Gemma
                          Prohibited Use Policy (https://ai.google.dev/gemma/prohibited_use_policy),
                          which you accept by using it. It is here for one purpose: scoring what a
                          player says against what a character knows, on your own PC.

Apache 2.0 notice: the Qwen3 and Phonon weights are redistributed unmodified apart from
format conversion (safetensors to GGUF for the language model) and the renaming of the
voice files; no NOTICE file accompanied the originals.

Voices
  Every voice is one of Phonon's catalogue, British English, picked for each character by
  their gender and age. The game's own actors are not imitated in this build.

Game content
  Nothing of Kingdom Come: Deliverance 2 is in this folder. The mod itself (the names and
  the conversations) is delivered through the Steam Workshop, and reads the game's own files
  on your machine.
