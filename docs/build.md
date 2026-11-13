# Сборка

Visual Studio 2022 Build Tools (MSVC, C++23), CMake 3.28+, Ninja и [vcpkg](https://vcpkg.io) в `C:\vcpkg`
(путь задан в `CMakePresets.json`). Зависимости — `wil`, `nlohmann-json`, `webview2` — vcpkg ставит сам.

```bat
tools\build.cmd dev        :: build\dev\aicontrols.exe (Debug)
tools\build.cmd dist       :: build\dist\aicontrols.exe (Release)
tools\dist.cmd 0.1.116     :: + установщик build\installer\AIControls-Setup-0.1.116.exe (Inno Setup 6)
tools\build.cmd analyze    :: то же с MSVC /analyze — перед каждым пушем
ctest --test-dir build\dev :: тесты ядра
```

Версия установщика — аргумент `tools\dist.cmd`, не номер в исходниках. `vcpkg.json` остаётся `0.1.0`.
Ставится на пользователя, без прав администратора, в `%LOCALAPPDATA%\Programs\AIControls`.

Перенести секреты из другой программы, не показывая значений: передайте их переменными окружения
и запустите `aicontrols.exe --import-secrets ИМЯ1,ИМЯ2`. Список имён — `aicontrols.exe --list-secrets`.
Окно с образцами ленты, без записи настроек и без CLI: `aicontrols.exe --gallery`.

## Откуда берётся обновление

Кнопка внизу панели смотрит в `%USERPROFILE%\projects\aicontrols\build\installer` и сравнивает имя
`AIControls-Setup-<версия>.exe` с DisplayVersion установленной копии. Если файл новее, клиент запускает его так:
`/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /RELAUNCH` — и закрывает своё окно. После установки открывается тот же чат.

Хэш этого локального файла клиент перед запуском не считает. `.sha256` кладётся рядом с установщиком опубликованного релиза:
скачанный файл сверяют с ним. Страница релизов — https://github.com/sovereignbrains/aicontrols/releases.

## Проверка на GitHub

`.github/workflows/build.yml` на каждый пуш собирает preset `dev` и гоняет `ctest`.
На раннере стоит Visual Studio Enterprise, не Build Tools, поэтому воркфлоу не вызывает `tools\build.cmd`:
тот зовёт `VsDevCmd.bat` из Build Tools. Пробники (`tests/probes`) в `ctest` не входят.

Карточка для шаринга лежит в `docs/social-preview.png` (1280×640). GitHub подставляет её в превью ссылки только если файл загружен в Settings → Social preview. Из репозитория сам он её не берёт.

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
Журнал сбоев — `%LOCALAPPDATA%\AIControls\journal.jsonl` (и страница `journal.html` рядом).
Сами чаты лежат у CLI: Claude — `%USERPROFILE%\.claude\projects`, Grok — `%USERPROFILE%\.grok\sessions`.
