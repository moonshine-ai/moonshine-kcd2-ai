# /// script
# requires-python = ">=3.11"
# dependencies = [
#   "torch>=2.4.0",
#   "transformers>=4.51.0",
#   "peft>=0.14.0",
#   "trl>=0.15.0",
#   "datasets>=3.2.0",
#   "accelerate>=1.2.0",
#   "huggingface_hub>=0.28.0",
#   "scikit-learn>=1.5.0",
#   "gliner2[local]",
#   "trackio",
# ]
# ///
"""Cloud GPU evaluation & LoRA SFT bake-off for KCD2 NPC Command Understanding.

Runs on Hugging Face Jobs (`hf jobs uv run`) and uploads raw test-split predictions
plus merged LoRA weights to `petewarden/kcd2-command-models` so local evaluation can
link every prediction against `data/kcd2/npcs.sqlite` and quantize GGUFs for CPU benchmarking.

Modes (`--arm`, or `ARM` env var):
  - `B`: Zero-shot & LoRA-finetuned `unsloth/functiongemma-270m-it`
  - `CD`: Zero-shot `fastino/gliner2.5-small-v1`, fine-tuned DeBERTa/GLiNER2.5 classifier+span,
          zero-shot `MadeAgents/Hammer2.1-0.5b`, zero-shot `Qwen/Qwen3-1.7B`,
          zero-shot & LoRA-finetuned `Qwen/Qwen3-0.6B`
"""
from __future__ import annotations

import argparse
import json
import os
import re
import time
from pathlib import Path
from typing import Any

import torch
from datasets import Dataset, load_dataset
from huggingface_hub import HfApi
from peft import LoraConfig, PeftModel
from transformers import AutoModelForCausalLM, AutoProcessor, AutoTokenizer
from trl import SFTConfig, SFTTrainer

DATASET_REPO = os.environ.get("DATASET_REPO", "petewarden/kcd2-npc-command-bench")


def _make_sft_config(**kwargs: Any) -> SFTConfig:
    import inspect

    sig = inspect.signature(SFTConfig.__init__)
    valid = set(sig.parameters.keys())
    filtered = {k: v for k, v in kwargs.items() if k in valid}
    return SFTConfig(**filtered)
OUTPUT_REPO = os.environ.get("OUTPUT_REPO", "petewarden/kcd2-command-models")

TOOL_DEFS = [
    {
        "type": "function",
        "function": {
            "name": "locate_npc",
            "description": (
                "The player asks where a specific person, relative, or tradesperson is right now "
                "or how to find them. Do NOT call for biographical questions ('Where was X born?'), "
                "opinions, or general chat."
            ),
            "parameters": {
                "type": "object",
                "properties": {
                    "target_mention": {
                        "type": "string",
                        "description": "Name, kinship role ('your brother'), trade ('the blacksmith'), or pronoun ('him', 'her').",
                    }
                },
                "required": ["target_mention"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "follow_npc",
            "description": (
                "The player commands the NPC to physically follow someone or follow the player ('Follow me'). "
                "Do NOT call for idioms like 'I do not follow your meaning'."
            ),
            "parameters": {
                "type": "object",
                "properties": {
                    "target_mention": {
                        "type": "string",
                        "description": "Person to follow, or 'me' for the player.",
                    }
                },
                "required": ["target_mention"],
            },
        },
    },
    {
        "type": "function",
        "function": {
            "name": "travel_to",
            "description": (
                "The player asks or commands the NPC to travel/journey together to a town, village, castle, or landmark."
            ),
            "parameters": {
                "type": "object",
                "properties": {
                    "target_mention": {
                        "type": "string",
                        "description": "Destination place name (e.g. 'Trosky Castle', 'Kuttenberg').",
                    }
                },
                "required": ["target_mention"],
            },
        },
    },
]

COMPACT_SYSTEM = (
    "You classify player utterances in a medieval RPG dialogue into an immediate NPC command or NONE.\n"
    "Output EXACTLY ONE line in one of these four formats:\n"
    "NONE\n"
    "LOCATE: <person name, role like 'your brother', trade like 'the blacksmith', or pronoun like 'him'/'her'>\n"
    "FOLLOW: <person name or 'me'>\n"
    "TRAVEL: <settlement, castle, or landmark name>\n\n"
    "Rules:\n"
    "- Output LOCATE only when the player asks where someone currently is or how to find them right now.\n"
    "- Output NONE for past/biographical questions ('Where did X grow up?', 'Where was X born?'), "
    "opinions/gossip ('Can I trust X?'), idioms ('I don't follow you'), and ordinary conversation."
)


def format_dialogue_context(row: dict[str, Any]) -> str:
    parts = [f"NPC Speaker: {row['speaker_name']} ({row['settlement'] or 'Bohemia'})"]
    hist = row.get("history") or row.get("history_json") or []
    if isinstance(hist, str):
        hist = json.loads(hist)
    for m in hist[-4:]:
        who = "Player" if m["role"] == "user" else row["speaker_name"]
        parts.append(f"{who}: {m['content']}")
    parts.append(f"Player: {row['player_utterance']}")
    return "\n".join(parts)


def gold_compact_target(row: dict[str, Any]) -> str:
    act = row["gold_action"]
    tgt = (row.get("gold_surface_target") or "").strip()
    if act == "no_action" or not tgt:
        return "NONE"
    if act == "locate_npc":
        return f"LOCATE: {tgt}"
    if act == "follow_npc":
        return f"FOLLOW: {tgt}"
    if act == "travel_to":
        return f"TRAVEL: {tgt}"
    return "NONE"


def parse_compact_output(raw: str) -> dict[str, str]:
    txt = re.sub(r"<think>.*?</think>", "", raw, flags=re.DOTALL).strip()
    first = ""
    for line in txt.splitlines():
        if line.strip():
            first = line.strip()
            break
    if not first or first.upper().startswith("NONE"):
        return {"action": "no_action", "target_mention": ""}
    m = re.match(r"^(LOCATE|FOLLOW|TRAVEL)\s*:\s*(.+)$", first, flags=re.IGNORECASE)
    if m:
        verb = m.group(1).upper()
        span = m.group(2).strip().strip('"').strip("'")
        amap = {"LOCATE": "locate_npc", "FOLLOW": "follow_npc", "TRAVEL": "travel_to"}
        return {"action": amap[verb], "target_mention": span}
    return {"action": "no_action", "target_mention": ""}


def parse_functiongemma_output(raw: str) -> dict[str, str]:
    # Native format: <start_function_call>call:locate_npc{target_mention:<escape>Margaret<escape>}<end_function_call>
    m = re.search(
        r"call:(locate_npc|follow_npc|travel_to)\{.*?target_mention:(?:<escape>|\"|')?(.*?)(?:<escape>|\"|')?\}",
        raw,
        flags=re.DOTALL,
    )
    if m:
        return {"action": m.group(1), "target_mention": m.group(2).strip()}
    # Also support JSON fallback if emitted
    jm = re.search(r"\{.*?\}", raw, flags=re.DOTALL)
    if jm:
        try:
            obj = json.loads(jm.group(0))
            name = obj.get("name") or obj.get("action") or ""
            args = obj.get("arguments") or obj.get("parameters") or obj
            if name in {"locate_npc", "follow_npc", "travel_to"}:
                return {
                    "action": name,
                    "target_mention": str(args.get("target_mention", "")).strip(),
                }
        except Exception:
            pass
    return {"action": "no_action", "target_mention": ""}


def build_fg_prompt_and_target(tokenizer: Any, row: dict[str, Any]) -> tuple[str, str]:
    dev_msg = (
        "You are a model that can do function calling with the following functions. "
        "Only call a function when the player issues a direct locate, follow, or travel command; "
        "otherwise reply NO_ACTION."
    )
    msgs = [
        {"role": "developer", "content": dev_msg},
        {"role": "user", "content": format_dialogue_context(row)},
    ]
    prompt_str = tokenizer.apply_chat_template(
        msgs, tools=TOOL_DEFS, tokenize=False, add_generation_prompt=True
    )
    act = row["gold_action"]
    tgt = (row.get("gold_surface_target") or "").strip()
    if act in {"locate_npc", "follow_npc", "travel_to"} and tgt:
        target_str = (
            f"<start_function_call>call:{act}{{target_mention:<escape>{tgt}<escape>}}<end_function_call><end_of_turn>\n"
        )
    else:
        target_str = "NO_ACTION<end_of_turn>\n"
    return prompt_str, target_str


@torch.inference_mode()
def eval_causal_lm_batched(
    model: Any,
    tokenizer: Any,
    rows: list[dict[str, Any]],
    prompt_fn: Any,
    parse_fn: Any,
    batch_size: int = 32,
    max_new_tokens: int = 36,
) -> tuple[list[dict[str, Any]], float]:
    model.eval()
    tokenizer.padding_side = "left"
    if tokenizer.pad_token_id is None:
        tokenizer.pad_token_id = tokenizer.eos_token_id

    preds: list[dict[str, Any]] = []
    t0 = time.perf_counter()
    for i in range(0, len(rows), batch_size):
        chunk = rows[i : i + batch_size]
        prompts = [prompt_fn(r) for r in chunk]
        enc = tokenizer(
            prompts, return_tensors="pt", padding=True, truncation=True, max_length=768
        ).to(model.device)
        in_len = enc["input_ids"].shape[1]
        out = model.generate(
            **enc,
            max_new_tokens=max_new_tokens,
            do_sample=False,
            pad_token_id=tokenizer.pad_token_id,
        )
        for j, r in enumerate(chunk):
            gen_ids = out[j][in_len:]
            raw = tokenizer.decode(gen_ids, skip_special_tokens=False)
            # Trim end-of-turn markers cleanly while preserving <start_function_call> if FunctionGemma
            raw_clean = raw.split("<end_of_turn>")[0].split("<|im_end|>")[0].split("<|endoftext|>")[0]
            parsed = parse_fn(raw_clean)
            preds.append(
                {
                    "example_id": r["example_id"],
                    "action": parsed["action"],
                    "target_mention": parsed["target_mention"],
                    "raw": raw_clean.strip()[:160],
                }
            )
    elapsed = time.perf_counter() - t0
    return preds, elapsed


def quick_action_metrics(rows: list[dict[str, Any]], preds: list[dict[str, Any]]) -> dict[str, float]:
    tp = fp = fn = tn = 0
    exact_act = 0
    for r, p in zip(rows, preds):
        g = r["gold_action"]
        pr = p["action"]
        if g == pr:
            exact_act += 1
        if g == "no_action":
            if pr == "no_action":
                tn += 1
            else:
                fp += 1
        else:
            if pr == g:
                tp += 1
            elif pr == "no_action":
                fn += 1
            else:
                fp += 1
    prec = tp / max(tp + fp, 1)
    rec = tp / max(tp + fn, 1)
    f1 = 2 * prec * rec / max(prec + rec, 1e-9)
    fpr = fp / max(tn + fp, 1)
    return {
        "action_acc": round(exact_act / max(len(rows), 1), 4),
        "cmd_f1": round(f1, 4),
        "neg_fpr": round(fpr, 4),
    }


def run_arm_b(train_rows: list[dict], dev_rows: list[dict], test_rows: list[dict], api: HfApi) -> None:
    model_id = "unsloth/functiongemma-270m-it"
    print(f"=== Loading {model_id} for Zero-Shot eval ===")
    tokenizer = AutoTokenizer.from_pretrained(model_id)
    model = AutoModelForCausalLM.from_pretrained(
        model_id, dtype=torch.bfloat16, device_map="cuda"
    )

    def fg_prompt_only(r: dict[str, Any]) -> str:
        p, _ = build_fg_prompt_and_target(tokenizer, r)
        return p

    zs_dev_preds, _ = eval_causal_lm_batched(
        model, tokenizer, dev_rows, fg_prompt_only, parse_functiongemma_output
    )
    zs_test_preds, zs_sec = eval_causal_lm_batched(
        model, tokenizer, test_rows, fg_prompt_only, parse_functiongemma_output
    )
    print("FunctionGemma-270M Zero-Shot test:", quick_action_metrics(test_rows, zs_test_preds))

    # Build SFT dataset
    sft_texts = []
    for r in train_rows:
        p, t = build_fg_prompt_and_target(tokenizer, r)
        sft_texts.append({"text": p + t})
    eval_texts = []
    for r in dev_rows:
        p, t = build_fg_prompt_and_target(tokenizer, r)
        eval_texts.append({"text": p + t})

    train_ds = Dataset.from_list(sft_texts)
    eval_ds = Dataset.from_list(eval_texts)

    print("=== Fine-Tuning FunctionGemma-270M with LoRA SFT ===")
    peft_cfg = LoraConfig(
        r=16,
        lora_alpha=32,
        lora_dropout=0.05,
        bias="none",
        task_type="CAUSAL_LM",
        target_modules=["q_proj", "k_proj", "v_proj", "o_proj", "gate_proj", "up_proj", "down_proj"],
    )
    sft_cfg = _make_sft_config(
        output_dir="/tmp/fg270m_sft",
        num_train_epochs=4,
        per_device_train_batch_size=16,
        gradient_accumulation_steps=2,
        learning_rate=2e-4,
        lr_scheduler_type="cosine",
        warmup_ratio=0.06,
        bf16=True,
        logging_steps=20,
        eval_strategy="epoch",
        save_strategy="no",
        max_length=640,
        dataset_text_field="text",
        report_to="trackio",
        project="kcd2-npc-commands",
        run_name="functiongemma-270m-lora-sft",
    )
    trainer = SFTTrainer(
        model=model,
        processing_class=tokenizer,
        train_dataset=train_ds,
        eval_dataset=eval_ds,
        peft_config=peft_cfg,
        args=sft_cfg,
    )
    t_train = time.perf_counter()
    trainer.train()
    train_sec = time.perf_counter() - t_train

    merged = trainer.model.merge_and_unload()
    ft_dev_preds, _ = eval_causal_lm_batched(
        merged, tokenizer, dev_rows, fg_prompt_only, parse_functiongemma_output
    )
    ft_test_preds, ft_sec = eval_causal_lm_batched(
        merged, tokenizer, test_rows, fg_prompt_only, parse_functiongemma_output
    )
    print("FunctionGemma-270M LoRA-SFT test:", quick_action_metrics(test_rows, ft_test_preds))

    merged_dir = Path("/tmp/fg270m_merged")
    merged.save_pretrained(merged_dir)
    tokenizer.save_pretrained(merged_dir)

    out_json = Path("/tmp/arm_B_results.json")
    out_json.write_text(
        json.dumps(
            {
                "functiongemma_270m_zeroshot": {
                    "model_id": model_id,
                    "gpu_eval_sec": round(zs_sec, 2),
                    "quick_test_metrics": quick_action_metrics(test_rows, zs_test_preds),
                    "dev_predictions": zs_dev_preds,
                    "test_predictions": zs_test_preds,
                },
                "functiongemma_270m_lora_sft": {
                    "model_id": f"{OUTPUT_REPO}/functiongemma-270m-sft",
                    "train_sec": round(train_sec, 2),
                    "gpu_eval_sec": round(ft_sec, 2),
                    "quick_test_metrics": quick_action_metrics(test_rows, ft_test_preds),
                    "dev_predictions": ft_dev_preds,
                    "test_predictions": ft_test_preds,
                },
            },
            indent=2,
        ),
        encoding="utf-8",
    )

    api.upload_file(
        path_or_fileobj=str(out_json),
        path_in_repo="results/arm_B_results.json",
        repo_id=OUTPUT_REPO,
        repo_type="model",
    )
    api.upload_folder(
        folder_path=str(merged_dir),
        path_in_repo="functiongemma-270m-sft",
        repo_id=OUTPUT_REPO,
        repo_type="model",
    )
    print("Uploaded Arm B results and merged weights to", OUTPUT_REPO)


def build_qwen_prompt(tokenizer: Any, row: dict[str, Any], is_qwen3: bool = True) -> str:
    msgs = [
        {"role": "system", "content": COMPACT_SYSTEM},
        {"role": "user", "content": format_dialogue_context(row)},
    ]
    kwargs: dict[str, Any] = {"tokenize": False, "add_generation_prompt": True}
    if is_qwen3:
        kwargs["enable_thinking"] = False
    try:
        return tokenizer.apply_chat_template(msgs, **kwargs)
    except TypeError:
        kwargs.pop("enable_thinking", None)
        return tokenizer.apply_chat_template(msgs, **kwargs)


def eval_gliner25(
    train_rows: list[dict[str, Any]],
    dev_rows: list[dict[str, Any]],
    test_rows: list[dict[str, Any]],
) -> dict[str, Any]:
    from gliner2 import AutoExtractor
    from sklearn.linear_model import LogisticRegression

    print("=== Evaluating Arm D: fastino/gliner2.5-small-v1 (74M) ===")
    ext = AutoExtractor.from_pretrained("fastino/gliner2.5-small-v1", map_location="cuda")
    schema = (
        ext.create_schema()
        .entities(
            {
                "person_target": "Person name, relative ('your brother'), trade ('the blacksmith'), or pronoun ('him', 'her') to find or follow",
                "place_target": "Town, village, castle, mines, or landmark destination to travel to",
            }
        )
        .classification(
            "intent",
            {
                "labels": [
                    "ask_current_whereabouts_of_person",
                    "command_to_follow_person",
                    "command_to_travel_to_place",
                    "normal_conversation_or_past_history_question",
                ]
            },
        )
    )
    label_map = {
        "ask_current_whereabouts_of_person": "locate_npc",
        "command_to_follow_person": "follow_npc",
        "command_to_travel_to_place": "travel_to",
        "normal_conversation_or_past_history_question": "no_action",
    }

    def run_gliner_split(rows: list[dict[str, Any]]) -> tuple[list[dict[str, Any]], float]:
        preds = []
        t0 = time.perf_counter()
        for r in rows:
            utt = r["player_utterance"]
            res = ext.extract(utt, schema, include_confidence=True)
            raw_cls = res.get("intent", "normal_conversation_or_past_history_question")
            if isinstance(raw_cls, dict):
                raw_cls = raw_cls.get("label") or raw_cls.get("value") or "normal_conversation_or_past_history_question"
            action = label_map.get(str(raw_cls), "no_action")
            ents = res.get("entities", {})
            p_spans = ents.get("person_target", [])
            loc_spans = ents.get("place_target", [])
            mention = ""
            if action == "travel_to" and loc_spans:
                mention = loc_spans[0]["text"] if isinstance(loc_spans[0], dict) else str(loc_spans[0])
            elif action in {"locate_npc", "follow_npc"} and p_spans:
                mention = p_spans[0]["text"] if isinstance(p_spans[0], dict) else str(p_spans[0])
            elif action in {"locate_npc", "follow_npc"}:
                # Fallback pronoun check for encoder-only extraction ("Where is he?")
                m_pron = re.search(r"\b(him|her|he|she|me)\b", utt, flags=re.IGNORECASE)
                if m_pron:
                    mention = m_pron.group(1)
            if action != "no_action" and not mention:
                action = "no_action"
            preds.append(
                {
                    "example_id": r["example_id"],
                    "action": action,
                    "target_mention": mention,
                    "raw": json.dumps(res)[:160],
                }
            )
        return preds, time.perf_counter() - t0

    dev_preds, _ = run_gliner_split(dev_rows)
    test_preds, test_sec = run_gliner_split(test_rows)
    print("GLiNER2.5-small Zero-Shot test:", quick_action_metrics(test_rows, test_preds))
    return {
        "model_id": "fastino/gliner2.5-small-v1",
        "gpu_eval_sec": round(test_sec, 2),
        "quick_test_metrics": quick_action_metrics(test_rows, test_preds),
        "dev_predictions": dev_preds,
        "test_predictions": test_preds,
    }


def run_arm_cd(train_rows: list[dict], dev_rows: list[dict], test_rows: list[dict], api: HfApi) -> None:
    results: dict[str, Any] = {}

    # 1. Arm D: GLiNER2.5-small (74M)
    results["gliner25_small_zeroshot"] = eval_gliner25(train_rows, dev_rows, test_rows)

    # 2. Arm C Zero-Shot: Hammer2.1-0.5b & Qwen3-1.7B & Qwen3-0.6B
    for tag, mid, is_q3 in [
        ("hammer2_1_0_5b_zeroshot", "MadeAgents/Hammer2.1-0.5b", False),
        ("qwen3_1_7b_zeroshot", "Qwen/Qwen3-1.7B", True),
        ("qwen3_0_6b_zeroshot", "Qwen/Qwen3-0.6B", True),
    ]:
        print(f"=== Evaluating {tag} ({mid}) ===")
        tok = AutoTokenizer.from_pretrained(mid)
        mdl = AutoModelForCausalLM.from_pretrained(
            mid, dtype=torch.bfloat16, device_map="cuda"
        )
        p_fn = lambda r, _tok=tok, _q3=is_q3: build_qwen_prompt(_tok, r, is_qwen3=_q3)
        d_preds, _ = eval_causal_lm_batched(mdl, tok, dev_rows, p_fn, parse_compact_output)
        t_preds, t_sec = eval_causal_lm_batched(mdl, tok, test_rows, p_fn, parse_compact_output)
        print(f"{tag} test:", quick_action_metrics(test_rows, t_preds))
        results[tag] = {
            "model_id": mid,
            "gpu_eval_sec": round(t_sec, 2),
            "quick_test_metrics": quick_action_metrics(test_rows, t_preds),
            "dev_predictions": d_preds,
            "test_predictions": t_preds,
        }
        if tag != "qwen3_0_6b_zeroshot":
            del mdl
            torch.cuda.empty_cache()
        else:
            # Keep Qwen3-0.6B loaded for LoRA SFT!
            q06_tok, q06_mdl = tok, mdl

    # 3. Fine-Tune Qwen3-0.6B with LoRA SFT on train split
    print("=== Fine-Tuning Qwen/Qwen3-0.6B with LoRA SFT ===")
    sft_train = [
        {"text": build_qwen_prompt(q06_tok, r, True) + gold_compact_target(r) + "<|im_end|>\n"}
        for r in train_rows
    ]
    sft_eval = [
        {"text": build_qwen_prompt(q06_tok, r, True) + gold_compact_target(r) + "<|im_end|>\n"}
        for r in dev_rows
    ]
    peft_cfg = LoraConfig(
        r=16,
        lora_alpha=32,
        lora_dropout=0.05,
        bias="none",
        task_type="CAUSAL_LM",
        target_modules=["q_proj", "k_proj", "v_proj", "o_proj", "gate_proj", "up_proj", "down_proj"],
    )
    sft_cfg = _make_sft_config(
        output_dir="/tmp/qwen3_06b_sft",
        num_train_epochs=4,
        per_device_train_batch_size=16,
        gradient_accumulation_steps=2,
        learning_rate=2e-4,
        lr_scheduler_type="cosine",
        warmup_ratio=0.06,
        bf16=True,
        logging_steps=20,
        eval_strategy="epoch",
        save_strategy="no",
        max_length=640,
        dataset_text_field="text",
        report_to="trackio",
        project="kcd2-npc-commands",
        run_name="qwen3-0.6b-lora-sft",
    )
    trainer = SFTTrainer(
        model=q06_mdl,
        processing_class=q06_tok,
        train_dataset=Dataset.from_list(sft_train),
        eval_dataset=Dataset.from_list(sft_eval),
        peft_config=peft_cfg,
        args=sft_cfg,
    )
    t_tr = time.perf_counter()
    trainer.train()
    tr_sec = time.perf_counter() - t_tr

    merged_q06 = trainer.model.merge_and_unload()
    p_fn_q06 = lambda r: build_qwen_prompt(q06_tok, r, is_qwen3=True)
    ft_dev, _ = eval_causal_lm_batched(merged_q06, q06_tok, dev_rows, p_fn_q06, parse_compact_output)
    ft_test, ft_sec = eval_causal_lm_batched(
        merged_q06, q06_tok, test_rows, p_fn_q06, parse_compact_output
    )
    print("Qwen3-0.6B LoRA-SFT test:", quick_action_metrics(test_rows, ft_test))
    results["qwen3_0_6b_lora_sft"] = {
        "model_id": f"{OUTPUT_REPO}/qwen3-0.6b-sft",
        "train_sec": round(tr_sec, 2),
        "gpu_eval_sec": round(ft_sec, 2),
        "quick_test_metrics": quick_action_metrics(test_rows, ft_test),
        "dev_predictions": ft_dev,
        "test_predictions": ft_test,
    }

    merged_dir = Path("/tmp/qwen3_06b_merged")
    merged_q06.save_pretrained(merged_dir)
    q06_tok.save_pretrained(merged_dir)

    out_json = Path("/tmp/arm_CD_results.json")
    out_json.write_text(json.dumps(results, indent=2), encoding="utf-8")

    api.upload_file(
        path_or_fileobj=str(out_json),
        path_in_repo="results/arm_CD_results.json",
        repo_id=OUTPUT_REPO,
        repo_type="model",
    )
    api.upload_folder(
        folder_path=str(merged_dir),
        path_in_repo="qwen3-0.6b-sft",
        repo_id=OUTPUT_REPO,
        repo_type="model",
    )
    print("Uploaded Arm CD results and merged Qwen3-0.6B weights to", OUTPUT_REPO)


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", default=os.environ.get("ARM", "B"), choices=["B", "CD"])
    args = ap.parse_args()

    token = os.environ.get("HF_TOKEN")
    api = HfApi(token=token)

    ds = load_dataset(DATASET_REPO, token=token)
    train_rows = [dict(x) for x in ds["train"]]
    dev_rows = [dict(x) for x in ds["validation"]]
    test_rows = [dict(x) for x in ds["test"]]
    print(f"Loaded splits: train={len(train_rows)}, dev={len(dev_rows)}, test={len(test_rows)}")

    if args.arm == "B":
        run_arm_b(train_rows, dev_rows, test_rows, api)
    else:
        run_arm_cd(train_rows, dev_rows, test_rows, api)


if __name__ == "__main__":
    main()
