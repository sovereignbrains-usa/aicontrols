# AIControls

Нативный клиент чата под Windows: [Claude Code](https://docs.claude.com/en/docs/claude-code) и Grok Build в одном окне.
Инструменты с подтверждением, секреты, терминал, браузер и репозитории GitHub.
C++23, Win32, Direct2D и DirectWrite — без Electron, один exe.

> Не официальный продукт Anthropic или xAI. «Claude» — товарный знак Anthropic.
> Клиент запускает установленные у вас `claude.exe` и `grok.exe` и разговаривает с ними.

Текущая сборка — **0.1.110**.

## Что умеет

**Чат**
- Оба агента в одном списке. Смена модели продолжает тот же разговор: вторая сторона получает переписку.
- Ответы потоком, markdown: заголовки, списки, цитаты, ссылки, таблицы, код.
- Вложения: файлы и снимки (`+`, `Ctrl+U`, `Ctrl+V`, перетаскивание); картинки ужимаются до 1568 px. Голос — `Ctrl+M`.
- Сообщение можно отправить, пока идёт ответ. `Esc` — остановить.
- Выделение текста мышью, `Ctrl+C`. Модель и effort, в том числе Auto. Кольцо лимитов; время сброса — по Москве.
- Права отдельно у Claude и у Grok: Спрашивать, Файлы без вопросов, Auto, План, Без вопросов.
- Название чата придумывает Haiku по первому вопросу и ответу (2–6 слов, до 48 знаков); переименование важнее.

**Инструменты** — свой MCP-сервер внутри клиента: `run_command` (PowerShell),
`read_file`, `write_file`, `edit_file`, `list_directory`, `screenshot`, `browser`, `list_chats`, `read_chat`,
`drive_search`, `drive_read`. У команды и файловых есть `on`: `user@host[:port]` — то же действие на сервере через ssh
(ключ, затем пароль из секрета `ssh user@host`). Чтение локально без вопроса, кроме режима «Спрашивать»;
запись, команда, снимок, браузер и любой `on` ждут «Разрешить», если режим этого требует. «Без вопросов» не спрашивает.
На карточке записанного файла внизу — «Открыть» и «В папке», статус «Готово» остаётся в заголовке.

**Grok** своими инструментами ищет в сети и делает картинки и видео (Imagine). Его `read_file` и `grep` не спрашивают
и пишутся строкой в ленту. Помощник один: `aicontrols:hand` — те же инструменты, без своего шелла.

**Секреты** — «имя → значение» в файле, зашифрованном DPAPI (привязан к учётной записи Windows).
Модель видит только имена: значения подставляются в команду (`env_secrets` или `{{secret:ИМЯ}}`)
и вырезаются из вывода. Правка — в сайдбаре.

**Окно**
- Слева вкладки «Чаты | Проекты»: поиск, возраст чата, точка модели, черновик, удаление в корзину с «Вернуть».
- Проект — папка, в которой работают его чаты: «Общее», любая папка или репозиторий GitHub.
  Клон — в `%USERPROFILE%\projects`, токен из секрета `GITHUB`. Память Claude у всех проектов одна.
- Терминал и браузер — левая колонка, кнопки в шапке рядом с панелью. Терминал: PowerShell, ConPTY, вкладки.
  Браузер: WebView2, свой профиль, не профиль пользователя, поиск — Startpage. Локальные адреса он не открывает.
  Под адресом — закладки облачных ИИ (ChatGPT, Claude, Gemini, Grok, Qwen, DeepSeek…); звёздочка в поле добавляет страницу,
  правая кнопка — порядок и удаление, средняя — в новой вкладке. Входы в аккаунты хранит профиль браузера.
- Жучок внизу панели — журнал сбоев: красные строки чатов, ошибки браузера и названий, падения клиента из отчётов Windows.
  Красная точка — есть непросмотренное; «Разобрать в чате» кладёт журнал в сообщение.
- Полосы прокрутки тянутся мышью, средняя кнопка прокручивает, как в браузере. Своя шапка вместо заголовка окна.
- Обновление само: более новый установщик в `build\installer` даёт кнопку внизу панели.
  После установки открывается тот же чат.
- Инструмент `ask`: вопрос другой модели через её бесплатный API (Gemini, Groq, OpenRouter, Mistral, Cerebras, DeepSeek, Qwen) —
  длинный текст сжать, черновик, перевод; в чат возвращается только ответ. Ключ — секрет `GEMINI_API_KEY`, `GROQ_API_KEY`,
  `OPENROUTER_API_KEY`, `MISTRAL_API_KEY`, `CEREBRAS_API_KEY`, `DEEPSEEK_API_KEY` или `DASHSCOPE_API_KEY`. Каждый вызов спрашивает.
- Трафик CLI идёт через мост на 127.0.0.1: напрямую, иначе sing-box `127.0.0.1:2080`, иначе `ssh -D`.
  Локальные адреса мост не пускает.

## Что нужно

- Windows 10 1809+ или Windows 11 (x64).
- [Claude Code](https://docs.claude.com/en/docs/claude-code/setup) — `claude.exe` в `%USERPROFILE%\.local\bin` или в `PATH` — и/или
  Grok Build — `grok.exe` в `%USERPROFILE%\.grok\bin` или в `PATH`.
- Для репозиториев GitHub — `git` в `PATH` и токен в секрете `GITHUB`.
- Для вкладки браузера — среда WebView2 (на Windows 11 она уже есть).

## Сборка

Visual Studio 2022 Build Tools (MSVC, C++23), CMake 3.28+, Ninja и [vcpkg](https://vcpkg.io) в `C:\vcpkg`
(путь задан в `CMakePresets.json`). Зависимости — `wil`, `nlohmann-json`, `webview2` — vcpkg ставит сам.

```bat
tools\build.cmd dev        :: build\dev\aicontrols.exe (Debug)
tools\build.cmd dist       :: build\dist\aicontrols.exe (Release)
tools\dist.cmd 0.1.110     :: + установщик build\installer\AIControls-Setup-0.1.110.exe (Inno Setup 6)
tools\build.cmd analyze    :: то же с MSVC /analyze — перед каждым пушем
ctest --test-dir build\dev :: тесты ядра
```

Версия установщика — аргумент `tools\dist.cmd`, не номер в исходниках. Ставится на пользователя, без прав администратора,
в `%LOCALAPPDATA%\Programs\AIControls`. Готовые установщики — в [releases](https://github.com/sovereignbrains/aicontrols/releases).

Перенести секреты из другой программы, не показывая значений: передайте их переменными окружения
и запустите `aicontrols.exe --import-secrets ИМЯ1,ИМЯ2`. Список имён — `aicontrols.exe --list-secrets`.
Окно с образцами ленты, без записи настроек и без CLI: `aicontrols.exe --gallery`.

## Устройство

```
src/app    окно: лента, ввод, сайдбар, терминал, браузер (Direct2D/DirectWrite, RichEdit, WebView2)
src/core   всё без интерфейса:
           claude_process   claude.exe, stream-json          grok_process   grok.exe, ACP → те же события
           tools            инструменты и MCP                 secrets        DPAPI
           markdown         разбор в текст и стили           highlight      подсветка кода
           store            чаты, настройки                 chat_export    чат в Markdown
           journal          журнал сбоев                   title_writer   название чата
           bookmarks        закладки браузера             ask            вопрос другой модели по API
           vt_screen, pty   терминал                      browser        вкладка страницы
           bridge           выход CLI в сеть                 github, gdrive репозитории и Диск
           usage, voice     лимиты, голос                   app_update     установщик новее стоящего
tests      тесты ядра; tests/probes — пробники, запускаются вручную
docs       протокол claude.exe: docs/protocol.md
```

Настройки — `%APPDATA%\AIControls\settings.json`, секреты — `%APPDATA%\AIControls\secrets.bin`,
связка частей одного чата — `%APPDATA%\AIControls\chats.json`.
Журнал сбоев — `%LOCALAPPDATA%\AIControls\journal.jsonl` (и страница `journal.html` рядом),
закладки — `%LOCALAPPDATA%\AIControls\bookmarks.json` и значки сайтов в `favicons\`.
Сами чаты лежат у CLI: Claude — `%USERPROFILE%\.claude\projects`, Grok — `%USERPROFILE%\.grok\sessions`.

## Лицензия

Код — [MIT](LICENSE). Шрифты в `assets/fonts` (Unbounded, Geist, JetBrains Mono) — SIL Open Font
License, тексты лицензий лежат рядом с ними.
