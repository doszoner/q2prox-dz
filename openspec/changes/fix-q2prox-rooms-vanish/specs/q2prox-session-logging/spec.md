# Spec Delta

## Purpose

Журнал сессии q2prox-dz, по которому тестер без консоли и разработчик без доступа к машине могут отнести пропажу списка серверов к обновлению каталога, транспорту или координатору.

## ADDED Requirements

### Requirement: Single session log file owned by the launcher

В пределах сессии лаунчер SHALL быть единственным процессом и единственным потоком, пишущим и ротирующим файл `logs/q2dz.log` в каталоге состояния пользователя (Windows `%LOCALAPPDATA%\Q2PRO-X Dos.Zone\state\`, Linux `${XDG_STATE_HOME:-~/.local/state}/q2prox-dz/` с правами 0600). Журнал SHALL быть открыт до первого обновления каталога и до старта координатора. Stderr координатора SHALL читаться асинхронно в очередь и записываться в файл главным циклом лаунчера на всех фазах сессии (старт, обновление, ожидание, остановка), с финальным дренажем до освобождения процесса. Ротация SHALL быть по размеру: два файла по 5 МБ. Сбой записи MUST NOT прерывать обновление каталога или сессию; при ошибке диска вывод SHALL тихо вернуться в stderr. Координатор и шлюз MUST NOT открывать файлы журнала. Параллельный второй лаунчер того же пользователя не поддерживается: Windows-лаунчер завершается, если экземпляр уже запущен; на Linux файл открывается в режиме append, и одновременная ротация двумя процессами — известное ограничение, не гарантия.

#### Scenario: Tester finds the log without extra steps

- **WHEN** тестер запускает Q2prox обычным способом и воспроизводит пустой список
- **THEN** в каталоге состояния есть `logs/q2dz.log` с событиями лаунчера и координатора за эпизод, без `Run-Log.cmd`.

#### Scenario: Full stderr pipe does not stall the coordinator

- **WHEN** координатор пишет в stderr больше, чем вмещает буфер трубы, во время ожидания опроса
- **THEN** координатор не блокируется, строки появляются в файле в порядке записи.

#### Scenario: Coordinator stderr reaches the file through the launcher

- **WHEN** лаунчер запускает фейкового ребёнка, пишущего в stderr заданные строки, и завершает сессию
- **THEN** все строки есть в файле, включая написанные непосредственно перед выходом ребёнка.

#### Scenario: Disk failure

- **WHEN** файл журнала недоступен для записи
- **THEN** обновление каталога и heartbeat продолжаются, события уходят в stderr.

### Requirement: One line per event in a single format

Каждое событие SHALL занимать одну строку вида `TS COMPONENT EVENT key=value …` с меткой времени UTC ISO 8601. Каждое значение SHALL соответствовать `^[A-Za-z0-9._:-]{1,64}$`, кроме `room=`, которое SHALL соответствовать грамматике идентификатора комнаты `^[A-Za-z0-9._-]{1,255}$` и MUST NOT усекаться (это ключ корреляции), и `CHILD line=` (экранированная строка ≤ 512 символов). Набор событий и полей (allow-list):

| COMPONENT | EVENT | поля | когда |
|---|---|---|---|
| LAUNCHER | `SESSION start` | `build= rc= os=` | старт лаунчера |
| LAUNCHER | `SESSION stop` | `reason= rc=` | любой выход лаунчера после `SESSION start` (общий путь выхода) |
| LAUNCHER | `CATALOG_REFRESH` | `result=ok\|error stage=catalog\|lookup\|servers\|plan\|config\|content error=none\|http\|timeout\|json\|schema\|rtc\|capacity\|config\|content_check\|other http=<n> elapsed_ms=<n> previous= ready= registered= targets= drops= puts=` | каждое обновление |
| LAUNCHER | `CATALOG_ROW` | `result=rejected kind=room\|alias room=<safe id>\|index=<n> field=<имя поля> reason=identity\|range\|type\|missing\|duplicate\|format` | каждая отвергнутая строка/алиас |
| LAUNCHER | `CATALOG_MAP` | как сейчас | как сейчас |
| LAUNCHER | `HEARTBEAT` | `generation= puts= drops=` | каждый записанный heartbeat |
| LAUNCHER | `CHILD` | `line=<…>` или `suppressed=1` | строка stderr ребёнка вне известных событий |
| COORDINATOR | `SESSION start` | `build= session= instance= gateway_restarts=` | старт координатора |
| COORDINATOR | `SESSION stop` | `reason= rc=` | конец координатора |
| COORDINATOR | `TARGET` | `room= id= state=pending\|ready\|terminal reason=<код> recovery=<n> retry=none\|immediate\|cooldown\|recreate published_ready=<n>` | каждый переход |
| COORDINATOR | `GATEWAY` | `event=start\|stop\|fatal\|restart reason=<код> restarts=<n>` | жизненный цикл шлюза |
| GATEWAY | `GATEWAY stats\|error\|stop` | существующие числовые поля `windows/gateway/src/main.cpp:1082-1103,1142,1199-1209` | как сейчас |

Коды `reason=`/`stage=`/`error=` MUST NOT содержать подстроки из фильтра секретов (ниже); коды, приходящие от транспорта или шлюза (например содержащие `ice`), координатор SHALL отображать в собственные безопасные коды до записи в stderr. Лаунчер SHALL добавить в `CATALOG_REFRESH` `error=` и `http=` (не-2xx больше не отбрасывается без статуса) и писать `CATALOG_ROW` для каждой отвергнутой строки или алиаса — в группе B как классификацию при сохранённом поведении rc17, в группе C как фактическую судьбу строки.

#### Scenario: Empty list is attributable

- **WHEN** обновление каталога падает из-за HTTP 503
- **THEN** в журнале есть `CATALOG_REFRESH result=error stage=catalog error=http http=503`.

#### Scenario: Stuck room is attributable

- **WHEN** цель переходит в terminal и повтор не планируется
- **THEN** в журнале есть `TARGET room=<id> id=<n> state=terminal reason=<код> recovery=<n> retry=none`.

#### Scenario: Rejected row is attributable before the behaviour change

- **WHEN** сборка группы B получает каталог с одной невалидной строкой
- **THEN** журнал содержит `CATALOG_ROW … reason=range` и `CATALOG_REFRESH result=error stage=catalog error=schema` (поведение rc17 сохранено).

### Requirement: Secrets excluded by construction

Новые события SHALL писать только поля из таблицы выше. Строка stderr ребёнка SHALL записываться без изменений только если она целиком соответствует событию из таблицы: известные `COMPONENT` и `EVENT`, только разрешённые для этого события ключи, каждое значение по грамматике выше, и ни одно поле, кроме `room=` с валидным идентификатором, не содержит подстроки фильтра секретов (ниже). Любая другая строка, похожая на событие, проходит через подстрочный фильтр как обычная строка ребёнка. Дренаж остальных строк SHALL заменять на `CHILD suppressed=1` любую строку, содержащую (без учёта регистра) одну из подстрок `credential, password, secret, token, authorization, ice, candidate, sdp, turn:, turns:, username, fingerprint, local-description, remote-description, session-description, peer id, peer_id, peerid, alias=, alias ", alias lookup, aliaserror`, или начинающуюся с SDP-поля `<буква>=`. В журнал MUST NOT попадать: учётные данные TURN, полный JSON ответов или конфиг, содержимое control-pipe, SDP, тела сообщений чата, отвергнутые алиасы, не прошедшие проверку значения `room`, текст исключений.

#### Scenario: Rejected alias is not leaked

- **WHEN** `/lookup` содержит алиас `not an alias`
- **THEN** журнал содержит `CATALOG_ROW result=rejected kind=alias index=<n> reason=format` и не содержит строки `not an alias`.

#### Scenario: Canaries in child stderr never reach disk

- **WHEN** фейковый ребёнок пишет в stderr строки `credential=CANARY1`, `a=candidate:CANARY2`, `turn:CANARY3@host`
- **THEN** ни одна из подстрок `CANARY` не встречается в `q2dz.log`, ротированном файле и stderr лаунчера; на их месте `CHILD suppressed=1`.

#### Scenario: Legitimate coordinator events pass the filter

- **WHEN** координатор пишет `TARGET room=room.q2ded.1 id=7 state=terminal reason=transport_failed recovery=3 retry=none published_ready=2`
- **THEN** строка попадает в файл без изменений.

#### Scenario: Room id containing a filter substring passes

- **WHEN** координатор пишет `TARGET room=room.q2ded.nice id=1 state=ready reason=none recovery=0 retry=none published_ready=1`
- **THEN** строка попадает в файл без изменений, а строка `a=candidate:… nice` вне грамматики событий заменяется на `CHILD suppressed=1`.

#### Scenario: Event-shaped line with a secret field is suppressed
- **WHEN** в stderr ребёнка приходят строки `COORDINATOR TARGET credential=CANARY`, `GATEWAY error password=CANARY` и полная строка `TARGET` с `reason=credentialCANARY`
- **THEN** каждая заменяется на `CHILD suppressed=1`; `CANARY` в файле не встречается.

#### Scenario: Existing no-exception-text tests still pass

- **WHEN** обновление падает с исключением
- **THEN** текст исключения в журнале отсутствует; существующие проверки `catalog.tests.ps1:490-524` на это проходят.

### Requirement: Logging is documented as a table

`docs/LOGGING.md` в q2prox-dz SHALL описывать формат строки, путь и права файла на каждой ОС, ротацию и её горизонт в часах при каденции 15 с, таблицу событий из этой спеки с колонкой «что значит», фильтр секретов со ссылкой на ADR 0001 §6 и ADR 0004 «Security invariants», и раздел чтения типовых случаев (пустой список, застрявшая комната, падение транспорта, чат). Новое событие без строки в таблице MUST NOT проходить ревью.

#### Scenario: Every emitted event is in the table

- **WHEN** сравниваются имена событий в коде лаунчера, координатора и шлюза с таблицей `LOGGING.md`
- **THEN** множества совпадают.
