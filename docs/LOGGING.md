# Журнал сессии q2prox-dz

Журнал нужен, чтобы по одному файлу отнести пропажу списка серверов к
обновлению каталога, транспорту или координатору — без консоли у тестера и
без доступа к его машине у разработчика.

## Файл и формат

| | Windows | Linux |
|---|---|---|
| Путь | `%LOCALAPPDATA%\Q2PRO-X Dos.Zone\state\logs\q2dz.log` | `${XDG_STATE_HOME:-~/.local/state}/q2prox-dz/logs/q2dz.log` |
| Права | каталог и файл только для владельца | каталог `0700`, файл `0600` |
| Кодировка | UTF-8 без BOM | UTF-8 без BOM |

Файл открывается лаунчером до первого обновления каталога и до старта
координатора. Пишет файл только главный цикл лаунчера (D9): stderr
координатора читается отдельным читателем в очередь, а лаунчер дренирует её
во время ожидания, обновлений и остановки, включая финальный дренаж после
выхода ребёнка. Координатор и шлюз файла не открывают — их строки доходят
через stderr лаунчера.

Ротация по размеру: `q2dz.log` до 5 МБ, при переполнении переезжает в
`q2dz.1.log`, старый `q2dz.1.log` затирается. Держатся ровно два файла
(10 МБ суммарно). При каденции heartbeat 15 с и типичных строках ~100–150 Б
горизонт для одних LAUNCHER/COORDINATOR-строк — порядка 100–200 часов на
файл; флуд-строки `CHILD` сокращают его пропорционально.

На практике горизонт определяет шлюз: Windows-шлюз пишет `GATEWAY stats`
примерно раз в секунду, строка ~415 Б, это ≈5 МБ / 415 Б ≈ 12600 строк
≈ 3.5 часа на файл (≈7 часов на оба файла) — именно этот поток, а не
heartbeat, выедает ротацию быстрее всего. Linux-шлюз такой построчный
`stats`-поток в stderr не эмитирует (см. `linux/src/managed_gateway_main.cpp`),
поэтому горизонт на Linux ближе к общей оценке 100–200 часов на файл.

Если журнал нельзя открыть или записать, события тихо уходят в stderr:
обновление каталога и сессия не прерываются.

Строка события: `TS COMPONENT EVENT key=value …`, где `TS` — метка UTC
ISO 8601. Значения — `^[A-Za-z0-9._:-]{1,64}$`, кроме `room=`, которое
соответствует грамматике идентификатора комнаты `^[A-Za-z0-9._-]{1,255}$` и
не усекается (это ключ корреляции), и полезной нагрузки `CHILD line=`, которая
экранируется (управляющие символы → пробелы) и обрезается до 512 символов.

## Таблица событий

Allow-list. Новое событие без строки в этой таблице не проходит ревью.

| COMPONENT | EVENT | Поля | Когда | Что значит |
|---|---|---|---|---|
| LAUNCHER | `SESSION start` | `build= rc= os=` | старт лаунчера | началась сессия лаунчера; `rc` — номер RC сборки |
| LAUNCHER | `SESSION stop` | `reason= rc=` | конец лаунчера | чем и как завершилась сессия лаунчера |
| LAUNCHER | `CATALOG_REFRESH` | `result=ok\|error stage=catalog\|lookup\|servers\|plan\|config\|content error=none\|http\|timeout\|json\|schema\|rtc\|capacity\|config\|content_check\|other http=<n> elapsed_ms=<n> previous= ready= registered= targets= drops= puts=` | каждое обновление каталога | судьба опроса `room/get`, `/lookup`, `rtc/servers` и плана целей; при ошибке — стадия, категория и HTTP-статус без текста исключения; `error=none` — обновление успешно |
| LAUNCHER | `CATALOG_ROW` | `result=rejected kind=room\|alias room=<safe id>\|index=<n> field=<имя поля> reason=identity\|range\|type\|missing\|duplicate\|format` | каждая отвергнутая строка или алиас | какая именно строка каталога или запись lookup отклонена; само значение не пишется |
| LAUNCHER | `CATALOG_MAP` | `result=missing room= map=` | как сейчас | у строки каталога нет карты или карта не найдена у тестера |
| LAUNCHER | `HEARTBEAT` | `generation= puts= drops=` | каждый записанный heartbeat | лаунчер отдал координатору свежий набор целей |
| LAUNCHER | `CHILD` | `line=<…>` или `suppressed=1` | строка stderr ребёнка вне известных событий | произвольная строка координатора/шлюза, экранированная; `suppressed=1` — строка содержала секрет и вырезана целиком |
| COORDINATOR | `SESSION start` | `build= session= instance= gateway_restarts=` | старт координатора | координатор поднялся с этой сборкой и id сессии |
| COORDINATOR | `SESSION stop` | `reason= rc=` | конец координатора | чем и с каким кодом завершился координатор |
| COORDINATOR | `TARGET` | `room= id= state=pending\|ready\|terminal reason=<код> recovery=<n> retry=none\|immediate\|cooldown\|recreate published_ready=<n>` | каждый переход цели | PUT принят (`pending`), endpoint появился (`ready`), цель завершилась (`terminal`); `retry=none` означает «повторять не будем» |
| COORDINATOR | `GATEWAY` | `event=start\|stop\|fatal\|restart reason=<код> restarts=<n>` | жизненный цикл шлюза | общий транспорт поднялся, остановлен, упал фатально или перезапущен |
| GATEWAY | `GATEWAY stats\|error\|stop` | числовые поля шлюза (`windows/gateway/src/main.cpp`, `linux/src/managed_gateway_main.cpp`) | каденс/ошибка/остановка шлюза | счётчики и причина остановки самого шлюза |

## Фильтр секретов

По ADR 0001 («Verification contract», п. 6 — отсутствие credentials в logs)
и ADR 0004 («Security invariants») в журнал и в stderr лаунчера не попадают
учётные данные TURN, полный JSON ответов и конфиги, содержимое control-pipe,
SDP, тела сообщений чата, отвергнутые алиасы и тексты исключений.

Строка stderr ребёнка, целиком соответствующая грамматике события из таблицы
выше (`COMPONENT EVENT key=value …`, каждое значение по грамматике раздела
«Файл и формат»), проходит без изменений, но от подстрочного фильтра
освобождено только поле `room=`, прошедшее собственную проверку формата —
иначе легальный `room=room.q2ded.nice` был бы вырезан. Любое другое поле
события, содержащее подстроку фильтра (например, `reason=ice_configuration_failed`
из-за `ice`), всё равно проходит через фильтр ниже и вырезает строку целиком.
Коды `reason=`/`stage=`/`error=`, приходящие от транспорта или шлюза,
координатор отображает в собственные безопасные коды до записи в stderr,
поэтому подстрок фильтра в них не остаётся.

Любая другая строка stderr ребёнка, содержащая без учёта регистра одну из подстрок
`credential`, `password`, `secret`, `token`, `authorization`, `ice`,
`candidate`, `sdp`, `turn:`, `turns:`, `username`, `fingerprint`,
`local-description`, `remote-description`, `session-description`,
`peer id`, `peer_id`, `peerid`, `alias=`, `alias "`, `alias lookup`,
`aliaserror`, или начинающаяся с SDP-поля `<буква>=`, заменяется на
`LAUNCHER CHILD suppressed=1`. Список совпадает с
`linux/src/common.cpp:184-200` (`contains_sensitive_key` / `sanitize_log_line`).

## Типовые случаи

- **Пустой список при старте.** Ищите последний `CATALOG_REFRESH`:
  `result=error stage=catalog error=http http=503` — облако ответило не-2xx;
  `error=timeout` — сеть; `result=error stage=catalog error=schema` вместе с
  `CATALOG_ROW result=rejected … field=<поле> reason=<код>` — в ответе была
  хотя бы одна битая строка: она названа по `CATALOG_ROW`, но всё обновление
  всё равно отказано (как в rc17); `result=ok targets=0` — каталог сам пустой.
  Отказ всего обновления `error=schema` остаётся также для тела без
  `rooms`/`aliases` или где
  они не массивы и для ключей, различающихся регистром.
- **Комната есть, а в списке её нет.** Ищите `TARGET room=<id>`.
  `state=pending` без `ready` — транспорт не поднял endpoint;
  `state=terminal reason=<код> retry=none` — цель умерла и повтор не
  запланирован; `reason=ready_timeout` — шлюз не ответил за ready-таймаут.
- **Падение транспорта.** `GATEWAY event=fatal reason=<код>` — шлюз умер;
  `event=restart` — поднялась замена; `event=fatal reason=transport_disabled`
  — после второго падения транспорт отключён до конца сессии.
- **Протокол управления.** `CHILD line=…` с `HUB_PROTOCOL_ERROR` — лаунчер и
  координатор разошлись по `Q2DZ/ROOMS/2`.
- **Чат DosZone.** `logs/chat.log` сегодня не существует; чат никак не
  логируется. Формат появится вместе с реализацией чата.

## Проверка множества событий

`noncommit/q2prox-rooms-fix-plan/events-vs-table.sh` сравнивает имена
событий, которые реально пишут лаунчеры, координаторы и шлюзы, с колонкой
EVENT этой таблицы; множества должны совпадать.
