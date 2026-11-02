# Протокол claude.exe

AIControls разговаривает с нативным CLI Claude Code (`%USERPROFILE%\.local\bin\claude.exe`) напрямую,
без Node и без ACP: один процесс на чат, JSON по строке в stdin и stdout. Это тот же канал, которым
пользуются официальные Agent SDK. Проверено на CLI 2.1.287–2.1.302 пробой `tools/probes/probe-protocol.ps1`.
Grok — отдельно, в конце.

## Запуск

```
claude.exe --output-format stream-json --verbose --input-format stream-json
           --permission-prompt-tool stdio --tools ""
           --mcp-config {"mcpServers":{"aic":{"type":"sdk","name":"aic"}}}
           [--include-partial-messages] [--model …] [--effort …] [--resume <session id>]
```

- `--tools ""` — встроенные Bash/Read/Edit выключены: у модели только наши инструменты.
- `HTTPS_PROXY` / `HTTP_PROXY` — адрес моста (`BridgeUrl`): случайный порт на 127.0.0.1. На каждое соединение
  мост идёт напрямую, иначе в sing-box на `127.0.0.1:2080`, иначе в `ssh -D`. Локальные адреса отказывает сразу.
- `CLAUDE_CODE_SDK_READS_SESSION_STATE=1` — CLI сообщает `session_state_changed` (running / idle):
  только так видно, что ход идёт, когда сообщение отправлено посреди хода.
- Процесс живёт в job object: закрылся клиент — умер CLI и всё, что он запустил.

## Обмен

1. Мы → `{"type":"control_request","request_id":"aic_init","request":{"subtype":"initialize","hooks":null}}`.
2. CLI → `control_request` / `mcp_message` для сервера `aic`: `initialize`, `notifications/initialized`,
   `tools/list`. Мы отвечаем `control_response` с `{"mcp_response":{…JSON-RPC…}}`.
   Ответ на наш `initialize` (модели, аккаунт) CLI шлёт после этого рукопожатия; до него сообщения
   пользователя держатся в очереди.
3. Мы → `{"type":"user","session_id":"default","parent_tool_use_id":null,"message":{"role":"user","content":…}}`;
   `content` — строка или блоки (text, image, document).
4. CLI → `system/init` (session_id), `stream_event` (дельты текста), `assistant`, `rate_limit_event`
   (`unifiedWindows.five_hour` и `seven_day`: `utilization` 0…1 и `resetsAt` в секундах Unix).
5. Вызов инструмента: CLI → `can_use_tool` (`tool_name` вида `mcp__aic__run_command`, `input`,
   `tool_use_id`). Мы → `{"behavior":"allow","updatedInput":…}` или `{"behavior":"deny","message":…}`.
   После allow CLI → `mcp_message` `tools/call`, мы → результат `{content:[{type:text,…}], isError}`.
   CLI повторяет результат сообщением `user` с блоком `tool_result` и тем же `tool_use_id` —
   по нему карточка в ленте показывает вывод.
6. Конец хода — `{"type":"result",…}`. Закрыть stdin — CLI выходит.

Свои `control_request`: `interrupt` (Esc), `set_model`, `set_permission_mode`.

## Инструменты (`src/core/tools.cpp`)

| Имя | Подтверждение | Что делает |
|---|---|---|
| `run_command` | да | PowerShell без окна; stdin пустой; таймаут до 900 с; секреты через `env_secrets` или `{{secret:ИМЯ}}` |
| `write_file` | да | UTF-8, недостающие папки создаются |
| `edit_file` | да | точная замена; в файле с CRLF правка с LF приводится к CRLF |
| `read_file` | нет | до 2000 строк или 60 КБ за раз; дальше — `offset`. UTF-16 с BOM читается. С сервера — не больше 400 КБ |
| `list_directory` | нет | до 500 записей |
| `screenshot` | да | окно по части заголовка или весь экран, картинкой |
| `browser` | да | вкладка в окне клиента: `open`, `text`, `screenshot`, `click`, `type`, `close` |
| `list_chats` | нет | чаты Claude и Grok |
| `read_chat` | нет | один чат как Markdown, до 2000 строк или 60 КБ |
| `drive_search` | нет | поиск по Google Drive, только чтение |
| `drive_read` | нет | Docs, первый лист Sheets, Slides и текстовые файлы |

Колонка «подтверждение» — режим «Спрашивать». «Файлы без вопросов» пропускает локальные `write_file` и `edit_file`.
«Без вопросов» не спрашивает. «План» не выполняет изменений. Auto пропускает безобидное и спрашивает опасное.
`on` спрашивает всегда, кроме «Без вопросов»: на сервере команда идёт в bash или sh, пути — абсолютные или от `~/`.

Значения секретов вырезаются из любого вывода (`***`). Ошибки PowerShell приходят текстом,
а не CLIXML: команда выполняется в блоке с `2>&1`, синтаксические ошибки CLIXML разбираются.

## Аккаунт

`claude auth status` (JSON: `loggedIn`, `email`, `subscriptionType`), `claude auth login --claudeai`
(CLI сам открывает браузер), `claude auth logout`. Учётные данные общие для всех запусков `claude`
на компьютере (`%USERPROFILE%\.claude`).

## История чатов

`%USERPROFILE%\.claude\projects\<папка, где [^A-Za-z0-9] → ->\<session id>.jsonl`. Название — последняя
запись `custom-title` или `ai-title`, иначе первая строка первого сообщения пользователя.
Продолжение — `--resume <id>`.

Grok хранит чаты иначе: `%USERPROFILE%\.grok\sessions\<папка, %-encoded>\<id>\chat_history.jsonl`.
Какие сессии составляют один чат, клиент помнит сам в `%APPDATA%\AIControls\chats.json`.

## Grok

`grok.exe` говорит ACP (`grok agent stdio`, JSON-RPC по строке). `GrokProcess` переводит это в те же события,
что окно уже понимает от Claude: текст, запрос разрешения, результат хода. Наши инструменты доходят до него MCP-сервером на замыкании.
Его собственные, которые профиль пропускает: `web_search`, `web_fetch`, `image_gen`, `image_edit`, `image_to_video`,
`reference_to_video`. `read_file` и `grep` выполняются у него сами и в ленте становятся строкой, не карточкой.
Помощник один, `aicontrols:hand`: те же инструменты, веб и Imagine, без своего шелла.
