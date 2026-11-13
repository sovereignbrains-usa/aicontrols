English · [Русский](README.ru.md)

# AIControls

[![release 0.1.116](https://img.shields.io/badge/release-0.1.116-4c92ff)](https://github.com/sovereignbrains/aicontrols/releases)
[![MIT](https://img.shields.io/github/license/sovereignbrains/aicontrols)](LICENSE)
[![build](https://github.com/sovereignbrains/aicontrols/actions/workflows/build.yml/badge.svg)](https://github.com/sovereignbrains/aicontrols/actions/workflows/build.yml)

Native Windows chat: [Claude Code](https://docs.claude.com/en/docs/claude-code) and Grok Build in one window.
Tools ask before they act, secrets stay encrypted, and the window has a terminal, a browser, and GitHub repositories.
C++23, Win32, Direct2D and DirectWrite. No Electron, one exe.

> Not an official Anthropic or xAI product. “Claude” is a trademark of Anthropic.
> The client starts the `claude.exe` and `grok.exe` you already installed and talks to them.

Current build: **0.1.116**.

## What it does

**Chat**
- Both agents in one list. Switching the model continues the same conversation: the other side receives the thread.
- Streamed replies, markdown: headings, lists, quotes, links, tables, code.
- Attachments: files and pictures (`+`, `Ctrl+U`, `Ctrl+V`, drag and drop). Pictures are scaled down to 1568 px. Voice is `Ctrl+M`.
- You can send a message while a reply is still coming. `Esc` stops it.
- Select text with the mouse, `Ctrl+C`. Model and effort, including Auto. A limit ring; the reset time is Moscow time.
- Permissions are separate for Claude and for Grok. The buttons are «Спрашивать», «Файлы без вопросов», Auto, «План», «Без вопросов».
- Haiku names the chat from the first question and answer (2–6 words, up to 48 characters). A name you type wins.

**Tools.** The client’s own MCP server: `run_command` (PowerShell),
`read_file`, `write_file`, `edit_file`, `list_directory`, `screenshot`, `browser`, `list_chats`, `read_chat`,
`drive_search`, `drive_read`. A command or a file tool can take `on`: `user@host[:port]` runs the same action on that server over ssh
(a key, then the password from the secret `ssh user@host`). Local reads do not ask, except in «Спрашивать».
A write, a command, a screenshot, the browser, and any `on` wait for «Разрешить» when the mode says so. «Без вопросов» does not ask.
A card for a file that was written has «Открыть» and «В папке». The title stays «Готово».

**Grok** uses its own tools to search the web and to make pictures and video (Imagine). Its `read_file` and `grep` do not ask
and show up as a line in the feed. One helper: `aicontrols:hand`, the same tools, no shell of its own.

**Secrets** are name → value in one file, encrypted with DPAPI and bound to the Windows account.
The model sees names only. Values are filled into the command (`env_secrets` or `{{secret:NAME}}`)
and cut out of the output. Edit them in the sidebar.

**Window**
- On the left, «Чаты | Проекты»: search, how old a chat is, a model dot, a draft, delete into the trash with «Вернуть».
- A project is the folder its chats work in: «Общее», any folder, or a GitHub repository.
  A clone lands in `%USERPROFILE%\projects`. The token is the secret `GITHUB`. Claude’s memory is shared by every project.
- Terminal and browser are the left column, opened from the header next to the sidebar. Terminal: PowerShell, ConPTY, tabs.
  Browser: WebView2, its own profile, not yours. Search is Startpage. It does not open local addresses.
  Under the address bar, bookmarks for the hosted chats (ChatGPT, Claude, Gemini, Grok, Qwen, DeepSeek, and others). A star in the field adds the page,
  right-click reorders or deletes, middle-click opens a new tab. The browser profile keeps the signed-in accounts.
- The bug at the bottom of the sidebar is the failure journal: red lines from chats, browser and title errors, and crashes Windows recorded.
  A red dot means something is unseen. «Разобрать в чате» puts the journal into the message.
- Scrollbars drag, and the middle button scrolls like a browser. The window draws its own title bar.
- It updates itself. Check a downloaded installer against the `.sha256` beside it.
  After an update the same chat opens.
- The `ask` tool asks another model through its free API (Gemini, Groq, OpenRouter, Mistral, Cerebras, DeepSeek, Qwen):
  shorten a long text, a draft, a translation. Only the answer comes back to the chat. The key is a secret: `GEMINI_API_KEY`, `GROQ_API_KEY`,
  `OPENROUTER_API_KEY`, `MISTRAL_API_KEY`, `CEREBRAS_API_KEY`, `DEEPSEEK_API_KEY`, or `DASHSCOPE_API_KEY`. Each call asks.
- CLI traffic goes out through a bridge on 127.0.0.1: direct, otherwise sing-box at `127.0.0.1:2080`, otherwise `ssh -D`.
  The bridge refuses local addresses.

## What you need

- Windows 10 1809 or later, or Windows 11 (x64).
- [Claude Code](https://docs.claude.com/en/docs/claude-code/setup): `claude.exe` in `%USERPROFILE%\.local\bin` or on `PATH`, and/or
  Grok Build: `grok.exe` in `%USERPROFILE%\.grok\bin` or on `PATH`.
- For GitHub repositories, `git` on `PATH` and a token in the secret `GITHUB`.
- For the browser tab, the WebView2 runtime (Windows 11 already has it).

Installers are on the [releases](https://github.com/sovereignbrains/aicontrols/releases) page. Building from source is in [docs/build.md](docs/build.md).

## Where your files are

Settings: `%APPDATA%\AIControls\settings.json`. Secrets: `%APPDATA%\AIControls\secrets.bin`. Which parts belong to one chat: `%APPDATA%\AIControls\chats.json`.
The failure journal: `%LOCALAPPDATA%\AIControls\journal.jsonl` (and `journal.html` beside it). Bookmarks: `%LOCALAPPDATA%\AIControls\bookmarks.json`, site icons in `favicons\`.
The chats themselves stay with the CLI. Claude: `%USERPROFILE%\.claude\projects`. Grok: `%USERPROFILE%\.grok\sessions`.

## License

The code is [MIT](LICENSE). The fonts in `assets/fonts` (Unbounded, Geist, JetBrains Mono) are under the SIL Open Font License. See [THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

To report a vulnerability, see [SECURITY.md](SECURITY.md).
