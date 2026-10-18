# Spec Delta

## Purpose

Обновление каталога комнат лаунчером q2prox-dz: как одна плохая строка каталога или алиас влияет на остальные серверы и уже живые цели, на обеих ОС по одному контракту.

## ADDED Requirements

### Requirement: Row identity is extracted before validity

Лаунчер SHALL определять идентичность каждой q2/q2ded-строки `room/get` независимо от её валидности: строка идентифицируема, если `room` — допустимый токен (`^[A-Za-z0-9._-]{1,255}$`); иначе строка SHALL отвергаться с `reason=identity` и MUST NOT связываться ни с какой целью. Конфликты идентичности (одинаковый `room` или одинаковый производный алиас) SHALL искаться среди всех идентифицируемых строк до фильтра валидности.

#### Scenario: Unidentifiable row

- **WHEN** строка содержит `room` с пробелом рядом с валидной строкой
- **THEN** валидная строка становится целью, неидентифицируемая даёт `CATALOG_ROW … index=<n> reason=identity`, цели не затрагивает.

#### Scenario: Valid and invalid rows share a room id, either order

- **WHEN** в каталоге валидная строка и строка с `players=17, maxPlayers=16` имеют один `room`, в любом порядке
- **THEN** ни одна из них не становится новой целью; обе дают `reason=duplicate`; живая цель с этим `room`, если есть, удерживается.

### Requirement: Refresh rejects rows, not the whole catalog

Лаунчер SHALL при обновлении каталога отвергать отдельные строки `room/get`, не прошедшие проверку строки или попавшие в конфликт идентичности, и продолжать обновление с остальными валидными строками. Отказ всего обновления SHALL сохраняться для (а) сбоев получения и разбора: HTTP не-2xx, таймаут, недопустимый JSON, ошибка распаковки, не-UTF-8 или превышение размера тела, ключи, различающиеся регистром; (б) дефектов тела и глобального плана: отсутствие или не-массив `rooms`, отсутствие или не-массив `aliases`, невалидный `rtc/servers`, ICE-агрегат и его лимиты, превышение ёмкости целей, сериализация конфига, сбой запуска проверки контента. При отказе всего обновления цели, их счётчики и конфиги MUST NOT меняться и heartbeat MUST NOT отправляться. Поведение SHALL быть одинаковым на Windows и Linux.

#### Scenario: One invalid row beside a healthy one

- **WHEN** каталог содержит валидную q2ded-строку и строку с `players=17, maxPlayers=16`
- **THEN** обновление завершается `result=ok`, валидная строка становится целью
- **AND** невалидная строка не становится целью и даёт `CATALOG_ROW result=rejected kind=room room=<id> field=players reason=range`.

#### Scenario: Acquisition failure keeps state

- **WHEN** `room/get` отвечает HTTP 503, или тело не разбирается как JSON
- **THEN** обновление завершается `result=error`, цели, `misses` и `invalid` прежние, heartbeat не отправлен.

#### Scenario: Body-level defect still fails the refresh

- **WHEN** `rooms` отсутствует или не является массивом, или `rtc/servers` невалиден, или число целей превышает ёмкость
- **THEN** обновление завершается `result=error`, прежние цели сохраняются, heartbeat не отправлен.

#### Scenario: Foreign games only

- **WHEN** каталог непустой, но не содержит ни одной q2/q2ded-строки
- **THEN** обновление считается пустым каталогом: отсутствующие цели получают miss и после двух misses DROP.

### Requirement: Live target survives an invalid row for two refreshes

Для живой цели, чья строка присутствует в каталоге, но отвергнута (невалидна или в конфликте), лаунчер SHALL сохранять цель с последним принятым конфигом, отправлять PUT с этим конфигом, не принимать отвергнутый конфиг и не увеличивать `misses`; вместо этого SHALL увеличиваться счётчик `invalid`. На втором подряд успешном обновлении с отвергнутой строкой (`invalid` уже 1) цель SHALL получить DROP. Валидная строка SHALL обнулять `misses` и `invalid` и принимать новый конфиг; отсутствие строки SHALL обнулять `invalid` и увеличивать `misses`. Неуспешное обновление MUST NOT менять счётчики. Поле `invalid` SHALL переживать все копии карты целей (`Copy-Q2DzTargetMap`, копия в цикле Hub, `dataclasses.replace`).

#### Scenario: Transient bad row does not drop a connected server

- **WHEN** живая цель на одном успешном обновлении получает строку с `players=17, maxPlayers=16`, а на следующем — валидную
- **THEN** DROP нет, в первом heartbeat PUT с прежним конфигом, во втором — с новым; `invalid` = 1, затем 0.

#### Scenario: Persistently bad row expires

- **WHEN** строка живой цели отвергнута на двух успешных обновлениях подряд
- **THEN** на втором цель получает DROP.

#### Scenario: Failed refresh between bad rows

- **WHEN** строка отвергнута, затем обновление завершается ошибкой, затем строка отвергнута снова
- **THEN** DROP происходит на третьем (втором успешном) обновлении; после ошибки состояние не изменилось.

#### Scenario: Invalid then missing then invalid

- **WHEN** строка живой цели отвергнута, на следующем обновлении отсутствует, на следующем снова отвергнута
- **THEN** DROP нет: каждый переход вида сбрасывает другой счётчик (`invalid`=1 → `misses`=1,`invalid`=0 → `invalid`=1,`misses`=0).

#### Scenario: Missing row is unchanged

- **WHEN** строка живой цели отсутствует в каталоге два успешных обновления подряд
- **THEN** цель получает DROP, как и до изменения.

### Requirement: Identity conflicts skip the group and keep the live target

При конфликте идентичности лаунчер SHALL исключать всю группу конфликтующих строк из новых целей и MUST NOT создавать замену; живая цель с таким идентификатором SHALL удерживаться как при отвергнутой строке с `reason=duplicate`.

#### Scenario: Duplicate group beside a healthy neighbour

- **WHEN** две строки делят один `room`, а третья валидна и уникальна
- **THEN** третья становится целью, группа дубликатов — нет, обе строки группы дают `reason=duplicate`.

#### Scenario: Duplicate derived alias

- **WHEN** две строки с разными `room` дают один производный алиас
- **THEN** группа исключена с `reason=duplicate`, соседние строки обрабатываются как обычно.

#### Scenario: Retained targets count toward capacity

- **WHEN** удерживаемые цели и валидные строки вместе превышают ёмкость
- **THEN** новые цели сверх ёмкости не создаются, обновление не падает (ёмкость превышается только числом целей в карте — тогда `result=error`, как сейчас).

### Requirement: Alias lookup is sanitised before use

Лаунчер SHALL исключать из `/lookup` алиасы, не являющиеся допустимым токеном, и схлопывать дубликаты, оставляя алиас зарегистрированным; санированный список SHALL использоваться при отборе (`Select-Q2DzRegisteredRooms`/`registered_rooms`), при сборке конфига (`New-Q2DzRoomConfig`/`make_room_config`) и в обёртках `make_hub_targets`/`make_hub_configs`. Отвергнутый алиас MUST NOT попадать в журнал — только индекс и код причины `format` или `duplicate`.

#### Scenario: Bad alias beside a good one

- **WHEN** `/lookup` содержит алиас с пробелом и алиас живой q2ded-комнаты
- **THEN** комната регистрируется и становится целью; обновление не падает на стадии плана; журнал содержит `kind=alias index=<n> reason=format` и не содержит самого алиаса.

#### Scenario: Duplicate alias keeps registration

- **WHEN** `/lookup` содержит `q2.1` дважды
- **THEN** комната с алиасом `q2.1` считается зарегистрированной, второй экземпляр даёт `reason=duplicate`.

### Requirement: Shared refresh-scenario contract

Контракт `tests/contracts/doszone-api-cases.json` SHALL содержать кейсы вида `refresh-scenario`: `steps[]` с полями `rooms?`, `aliases?`, `rtc?`, `fail?` (`http|timeout|json|rtc|capacity|content_check`) и `expected` на шаг (`result`, `accepted`, `registered`, `targets` с `misses`/`invalid`, `drops`, `puts`, `rows`). Оба раннера SHALL исполнять их через функции обновления (`Get-Q2DzLiveTargets`+`Get-Q2DzTargetPlan`; `refresh_targets`), не через селекторы. Существующие кейсы `invalid-*`, `unsupported-*`, `heartbeat-duplicate-*`, прямые проверки селекторов и строгие валидаторы SHALL остаться без изменений.

#### Scenario: Minimal case set is present and green on both runners

- **WHEN** запускаются `windows/product/tests/catalog.tests.ps1` и `linux/tests/test_launcher.py`
- **THEN** оба исполняют кейсы: плохая строка + соседка; неидентифицируемая строка; invalid→valid; invalid×2; invalid→error→invalid; invalid→missing→invalid; группа дубликатов `room` и производного алиаса + соседка; валидный+невалидный дубликат в обоих порядках, с живой целью и без; `/lookup` с плохим и с дублирующим алиасом; пустой и «только чужие игры» каталог; первый refresh с ошибкой, затем восстановление; `fail=http/json/rtc/capacity/content_check`; ёмкость с удерживаемыми целями
- **AND** все существующие кейсы по-прежнему проходят.
