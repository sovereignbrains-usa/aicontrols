# Security

## English

The client runs an agent’s command only after you confirm it. «Без вопросов» turns that confirmation off, and you are the one who turns that mode on. Local file writes can also skip the question in «Файлы без вопросов».

Secrets are stored with DPAPI, bound to your Windows account. The model sees names, not values. A value is filled into the command and cut out of what the command prints.

Report a vulnerability in a private [advisory](https://github.com/sovereignbrains/aicontrols/security/advisories/new). Private vulnerability reporting is turned on under Settings → Security. If that page is closed, open an issue and add the label `security`. Do not paste secret values.

## Русский

Клиент исполняет команду агента после вашего подтверждения. Режим «Без вопросов» это подтверждение выключает, и включаете его вы. Локальную запись файла может пропустить и «Файлы без вопросов».

Секреты лежат в DPAPI и привязаны к учётной записи Windows. Модель видит имена, не значения. Значение подставляется в команду и вырезается из вывода.

Репорт — приватный [advisory](https://github.com/sovereignbrains/aicontrols/security/advisories/new). Приватные advisory включаются в Settings → Security. Если страница закрыта — issue с меткой `security`. Значения секретов не вставляйте.
