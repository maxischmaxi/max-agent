# max-agent

A minimal terminal AI coding agent in C17. Streams responses, runs tools
(bash, read_file, edit_file, write_file), keeps sessions resumable, and
renders to the standard terminal buffer — your tmux scrollback is the chat
history.

[![ci](https://github.com/maxischmaxi/max-agent/actions/workflows/ci.yml/badge.svg)](https://github.com/maxischmaxi/max-agent/actions/workflows/ci.yml)

## Build

```sh
make                    # debug build -> build/debug/max
make BUILD=release      # optimized  -> build/release/max
make test               # build and run all tests in tests/
```

Debug builds include AddressSanitizer and UndefinedBehaviorSanitizer.
Requires GNU make, gcc or clang, and libcurl.

## Installation

```sh
make install      # builds a release binary and installs it as `max`
```

This compiles an optimized release build and copies it to `/usr/local/bin/max`
— the standard location for locally compiled software on Linux/BSD/macOS.
If that directory is not writable for your user (which is common when
`/usr/local` is owned by root), run it with sudo:

```sh
sudo make install
```

Afterwards you can start the agent from anywhere with:

```sh
max
```

`make install` respects the usual conventions, so you can relocate the
binary if `/usr/local` is not what you want:

```sh
make install PREFIX=~/.local          # -> ~/.local/bin/max
make install DESTDIR=pkgroot         # for packaging/slackware-style staging
```

To remove it again:

```sh
make uninstall      # removes /usr/local/bin/max
sudo make uninstall
```

Both targets use the same `PREFIX`/`DESTDIR`/`BINDIR` variables, so
`make install PREFIX=~/.local && make uninstall PREFIX=~/.local` round-trips
cleanly.

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
max                     # if installed, or:
./build/debug/max       # straight from the repo
./build/debug/max --debug    # write a trace to /tmp/max-agent-<session>.log
```

| Command     | What it does                                |
| ----------- | ------------------------------------------- |
| `/models`   | pick a model from the config                |
| `/resume`   | list and resume past sessions               |
| `/rename`   | name the current session                   |
| `/new`      | start a fresh session (old one is kept)    |
| `/clear`    | same as `/new`                              |
| `/settings` | theme, system prompt, confirm-quit behavior |
| `/system-prompt` | edit the system prompt in your `$EDITOR` — `:wq` applies it, `:q` keeps the old one |
| `/quit`     | exit                                        |

The agent has `bash`, `read_file`, `edit_file` and `write_file` tools and runs them
asynchronously — independent calls execute in parallel (file edits stay sequential).
`ctrl+c` cancels a running turn, kills the tools' process groups, and returns you to
the prompt. When the model's context window overflows, older history is compacted
into an LLM-generated summary instead of being silently dropped, so long-running
tasks keep their context.

With `--debug`, every keypress, frame, thread, tool call and API error is
logged to `/tmp/max-agent-<session-id>.log` — sanitizer reports (use-after-free,
leaks) land in the same file.

## Development

| Command              | What it does                               |
| -------------------- | ------------------------------------------ |
| `make test`          | run all tests (each `tests/*.c` = one binary) |
| `make install`      | build release and install as `max`         |
| `make uninstall`    | remove the installed binary                |
| `make format`        | format sources with clang-format           |
| `make format-check`  | fail if anything is unformatted            |
| `make lint`          | run clang-tidy                             |
| `make clean`         | remove `build/` and `compile_commands.json` |

`tools/termemu.py` is a small pty-based terminal emulator for end-to-end
testing without a real terminal:

```sh
python3 tools/termemu.py "hello" --sleep 2
```