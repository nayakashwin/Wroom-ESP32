#!/usr/bin/env python3
"""llm_agent.py — run a local (or any OpenAI-compatible) LLM as an autonomous
tool-using agent. Replaces opencode for scripted LLM jobs.

stdlib only (urllib). Example:

  python3 llm_agent.py \
      --cwd ~/esp-wroom-32/blink300 \
      --prompt ~/esp-wroom-32/blink300/PROMPT.md \
      --model maternion/mimo-v2.6:9b-thinking

Tools given to the model: bash, write_file, read_file, list_files.
"""
import argparse
import json
import os
import pathlib
import sys
import time
import urllib.error
import urllib.request

DEFAULT_BASE = "http://127.0.0.1:11435/v1"
DEFAULT_MODEL = "maternion/mimo-v2.6:9b-thinking"
INSTRUCTIONS = pathlib.Path(__file__).with_name("esp32-build-flash.md")
MAX_TOOL_OUTPUT = 20000
REQ_TIMEOUT = 1800  # seconds; thinking models can be slow

SYSTEM_PROMPT = """\
You are an autonomous software agent on a Linux machine (user: dietpi).
You have these tools and ONLY these tools: bash, write_file, read_file, list_files.
You cannot ask the user questions — decide and act. Work end-to-end until the
task is fully done and verified, then reply with a short final report containing
the evidence (command outputs proving success).

Environment facts:
- PlatformIO is at $HOME/.platformio/penv/bin; your bash tool already has it on PATH.
- Flashing/serial needs the dialout group: wrap such commands in: sg dialout -c '...'
- Serial device is /dev/ttyUSB0 at 115200 baud.
- Keep changes inside the project directory (given below) unless a build/flash
  step genuinely requires more.

Machine rules for building and flashing ESP32 firmware follow.

<shared_instructions>
{instructions}
</shared_instructions>
"""

TOOLS = [
    {"type": "function", "function": {
        "name": "bash",
        "description": "Run a shell command in the project directory and return stdout+stderr.",
        "parameters": {"type": "object", "properties": {
            "command": {"type": "string", "description": "shell command to execute"}},
            "required": ["command"]}}},
    {"type": "function", "function": {
        "name": "write_file",
        "description": "Create or overwrite a file with the given content.",
        "parameters": {"type": "object", "properties": {
            "path": {"type": "string"},
            "content": {"type": "string"}},
            "required": ["path", "content"]}}},
    {"type": "function", "function": {
        "name": "read_file",
        "description": "Read a file and return its contents.",
        "parameters": {"type": "object", "properties": {
            "path": {"type": "string"}},
            "required": ["path"]}}},
    {"type": "function", "function": {
        "name": "list_files",
        "description": "List files in a directory (default: project root).",
        "parameters": {"type": "object", "properties": {
            "path": {"type": "string"}}}}},
]


def log(line: str) -> None:
    print(line, flush=True)


def api(base: str, key: str, payload: dict) -> dict:
    req = urllib.request.Request(
        f"{base}/chat/completions",
        data=json.dumps(payload).encode(),
        headers={"Content-Type": "application/json", "Authorization": f"Bearer {key}"},
    )
    with urllib.request.urlopen(req, timeout=REQ_TIMEOUT) as r:
        return json.loads(r.read())


def run_bash(command: str, cwd: str, timeout: int = 600) -> str:
    import subprocess
    env = dict(os.environ)
    env["PATH"] = f"{os.path.expanduser('~/.platformio/penv/bin')}:{env.get('PATH', '')}"
    try:
        p = subprocess.run(command, shell=True, cwd=cwd, env=env,
                           capture_output=True, text=True, timeout=timeout)
        out = p.stdout + p.stderr
        if p.returncode:
            out += f"\n[exit code {p.returncode}]"
    except subprocess.TimeoutExpired:
        out = f"[command timed out after {timeout}s]"
    return out or "(no output)"


def dispatch(name: str, args: dict, cwd: str) -> str:
    try:
        if name == "bash":
            return run_bash(args["command"], cwd)
        if name == "write_file":
            p = pathlib.Path(os.path.expanduser(args["path"]))
            if not p.is_absolute():
                p = pathlib.Path(cwd) / p
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(args["content"])
            return f"wrote {p} ({len(args['content'])} bytes)"
        if name == "read_file":
            p = pathlib.Path(os.path.expanduser(args["path"]))
            if not p.is_absolute():
                p = pathlib.Path(cwd) / p
            return p.read_text()
        if name == "list_files":
            p = pathlib.Path(os.path.expanduser(args.get("path", cwd)))
            if not p.is_absolute():
                p = pathlib.Path(cwd) / p
            return "\n".join(sorted(str(x.relative_to(p)) + ("/" if x.is_dir() else "")
                                    for x in p.iterdir()))
        return f"unknown tool: {name}"
    except Exception as e:  # let the model see and react to the error
        return f"ERROR: {type(e).__name__}: {e}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--model", default=DEFAULT_MODEL)
    ap.add_argument("--base-url", default=DEFAULT_BASE)
    ap.add_argument("--api-key", default=os.environ.get("LLM_API_KEY", "ollama"))
    ap.add_argument("--cwd", default=".", help="project directory (tool workdir)")
    ap.add_argument("--prompt", help="file with the task prompt (else --prompt-text)")
    ap.add_argument("--prompt-text", help="inline task prompt")
    ap.add_argument("--instructions", default=str(INSTRUCTIONS),
                    help="shared instruction file injected into the system prompt")
    ap.add_argument("--max-steps", type=int, default=40)
    ap.add_argument("--log", help="append transcript to this JSONL file")
    args = ap.parse_args()

    cwd = str(pathlib.Path(os.path.expanduser(args.cwd)).resolve())
    if args.prompt:
        task = pathlib.Path(os.path.expanduser(args.prompt)).read_text()
    elif args.prompt_text:
        task = args.prompt_text
    else:
        ap.error("--prompt or --prompt-text required")

    instructions = pathlib.Path(os.path.expanduser(args.instructions)).read_text() \
        if args.instructions else ""
    messages = [
        {"role": "system", "content": SYSTEM_PROMPT.format(instructions=instructions)},
        {"role": "user", "content": f"Project directory: {cwd}\n\nTask:\n{task}"},
    ]

    log(f"== llm_agent model={args.model} cwd={cwd} ==")
    for step in range(1, args.max_steps + 1):
        t0 = time.time()
        try:
            resp = api(args.base_url, args.api_key,
                       {"model": args.model, "messages": messages, "tools": TOOLS,
                        "stream": False, "temperature": 0.2})
        except (urllib.error.URLError, TimeoutError) as e:
            log(f"[step {step}] API error: {e}")
            return 2
        msg = resp["choices"][0]["message"]
        messages.append(msg)
        log(f"[step {step}] ({time.time()-t0:.0f}s) "
            f"finish={resp['choices'][0].get('finish_reason')} "
            f"tokens={resp.get('usage', {}).get('completion_tokens')}")

        think = msg.get("reasoning") or msg.get("thinking") or ""
        if think:
            log(f"  thinking: {think.strip()[:400]}")
        content = (msg.get("content") or "").strip()
        if content:
            log(f"  assistant: {content}")
        calls = msg.get("tool_calls") or []
        if not calls:
            log("== done: model stopped calling tools ==")
            if args.log:
                with open(args.log, "a") as f:
                    f.write(json.dumps({"final": content}) + "\n")
            return 0

        for call in calls:
            fn = call["function"]
            try:
                fn_args = json.loads(fn.get("arguments") or "{}")
            except json.JSONDecodeError:
                fn_args = {}
            log(f"  -> {fn['name']}({json.dumps(fn_args)[:300]})")
            result = dispatch(fn["name"], fn_args, cwd)
            log(f"  <- {result[:600].replace(chr(10), ' | ')}")
            if len(result) > MAX_TOOL_OUTPUT:
                result = result[:MAX_TOOL_OUTPUT] + "\n...[output truncated]"
            messages.append({"role": "tool", "tool_call_id": call.get("id", ""),
                             "name": fn["name"], "content": result})
            if args.log:
                with open(args.log, "a") as f:
                    f.write(json.dumps({"step": step, "tool": fn["name"],
                                        "args": fn_args, "result": result[:2000]}) + "\n")

    log(f"== stopped: max-steps ({args.max_steps}) reached ==")
    return 1


if __name__ == "__main__":
    sys.exit(main())
