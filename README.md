# max-agent

A minimal terminal AI coding agent in C17. Streams responses, runs tools
(bash, read_file, write_file), keeps sessions resumable, and renders to the
standard terminal buffer — your tmux scrollback is the chat history.

[![ci](https://github.com/maxischmaxi/max-agent/actions/workflows/ci.yml/badge.svg)](https://github.com/maxischmaxi/max-agent/actions/workflows/ci.yml)

## Build

```sh
make                    # debug build -> build/debug/max-agent
make BUILD=release      # optimized  -> build/release/max-agent
make test               # build and run all tests in tests/
```

Debug builds include AddressSanitizer and UndefinedBehaviorSanitizer.
Requires GNU make, gcc or clang, and libcurl.

## Configuration

The config lives at `~/.config/.maxagent/config.json`. The full schema is in
[`config.schema.json`](config.schema.json) — add it as `$schema` for editor
completion and validation:

```json
{
  "$schema": "https://raw.githubusercontent.com/maxischmaxi/max-agent/main/config.schema.json",
  "providers": [
    {
      "api": "openai-completions",
      "apiKey": "ollama",
      "baseUrl": "http://localhost:11434/v1",
      "models": [
        { "id": "qwen3-coder:30b", "contextWindow": 262144, "reasoning": false }
      ]
    }
  ],
  "settings": {
    "activeModel": "qwen3-coder:30b"
  }
}
```

Any OpenAI-compatible endpoint works (Ollama, LM Studio, OpenRouter, ...).
Settings changed inside the app (`/models`, `/settings`) are persisted back
to this file.

## Usage

```sh
./build/debug/max-agent            # start the agent
./build/debug/max-agent --debug    # write a trace to /tmp/max-agent-<session>.log
```

| Command     | What it does                                |
| ----------- | ------------------------------------------- |
| `/models`   | pick a model from the config                |
| `/resume`   | list and resume past sessions               |
| `/rename`   | name the current session                    |
| `/new`      | start a fresh session (old one is kept)    |
| `/clear`    | same as `/new`                              |
| `/settings` | theme, system prompt, confirm-quit behavior |
| `/quit`     | exit                                        |

The agent has `bash`, `read_file` and `write_file` tools and runs them
asynchronously — `ctrl+c` cancels a running turn, kills the tool's process
group, and returns you to the prompt.

With `--debug`, every keypress, frame, thread, tool call and API error is
logged to `/tmp/max-agent-<session-id>.log` — sanitizer reports (use-after-free,
leaks) land in the same file.

## Development

| Command              | What it does                               |
| -------------------- | ------------------------------------------ |
| `make test`          | run all tests (each `tests/*.c` = one binary) |
| `make format`        | format sources with clang-format           |
| `make format-check`  | fail if anything is unformatted            |
| `make lint`          | run clang-tidy                             |
| `make clean`         | remove `build/` and `compile_commands.json` |

`tools/termemu.py` is a small pty-based terminal emulator for end-to-end
testing without a real terminal:

```sh
python3 tools/termemu.py "hello" --sleep 2
```