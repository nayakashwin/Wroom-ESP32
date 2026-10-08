#!/usr/bin/env python3
"""llm_chat.py — one-shot chat with a local (or OpenAI-compatible) LLM.
No tools, no loop. stdlib only.

  python3 llm_chat.py "why is the sky blue?"
  python3 llm_chat.py -m ista-27b-iq3xxs --system "Answer in German." hallo
"""
import argparse
import json
import os
import urllib.request

DEFAULT_BASE = "http://127.0.0.1:11435/v1"
DEFAULT_MODEL = "maternion/mimo-v2.6:9b-thinking"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("message", nargs="+")
    ap.add_argument("-m", "--model", default=DEFAULT_MODEL)
    ap.add_argument("--base-url", default=DEFAULT_BASE)
    ap.add_argument("--api-key", default=os.environ.get("LLM_API_KEY", "ollama"))
    ap.add_argument("--system", default="You are a concise, helpful assistant.")
    ap.add_argument("--temperature", type=float, default=0.7)
    args = ap.parse_args()

    messages = [{"role": "system", "content": args.system},
                {"role": "user", "content": " ".join(args.message)}]
    req = urllib.request.Request(
        f"{args.base_url}/chat/completions",
        data=json.dumps({"model": args.model, "messages": messages,
                         "stream": False, "temperature": args.temperature}).encode(),
        headers={"Content-Type": "application/json",
                 "Authorization": f"Bearer {args.api_key}"},
    )
    with urllib.request.urlopen(req, timeout=600) as r:
        resp = json.loads(r.read())
    msg = resp["choices"][0]["message"]
    think = msg.get("reasoning") or msg.get("thinking") or ""
    if think and os.environ.get("SHOW_THINKING"):
        print(think.strip(), "\n---")
    print((msg.get("content") or "").strip())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
