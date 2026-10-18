# ADR 0005: внешний VPS gateway для подключения Q2PRO-X к Dos.Zone

> Архивное предложение, не принято как архитектура продукта. Перенесён 2026-10-18.
> Отметки задач и результаты относятся к исходной дате; проверки сейчас не повторялись.
> Пути прототипов и локальных evidence могут отсутствовать. Актуальное состояние — в индексе docs/README.md.

- **Status:** Proposed
- **Date:** 2026-08-23
- **Related:** [Архитектура](../ARCHITECTURE.md),
  [ADR 0001](../adr/0001-windows-webrtc-transport-backend.md),
  [ADR 0003](../adr/0003-linux-managed-session-and-guardian.md),
  [ADR 0004](../adr/0004-q2pro-x-custom-source-room-hub.md)

## Контекст

Текущая поставка устанавливает gateway на компьютер игрока. Q2PRO-X общается
с ним через случайный `127.0.0.1:<port>`, а gateway соединяется с комнатой
Dos.Zone через WebRTC-NET. На Windows для WebRTC используется WebView2, на
Linux — native WebRTC-NET и Wine для Q2PRO-X.

Такая схема уже доказала саму возможность подключения, но требует отдельной
установки, запуска launcher и платформенных поставок. Для небольшой аудитории
Quake II это может быть лишним пользовательским трением.

Предложена альтернатива: разместить gateway на внешнем Linux VPS и показывать
его публичные UDP endpoints во встроенном server browser Q2PRO-X. В идеальном
сценарии пользователь один раз добавляет Dos.Zone как `Custom` source или
получает адреса через `Favorites`, после чего выбирает комнаты как обычные
Quake II серверы. Локальная программа не устанавливается.

Dos.Zone рассматривается как внешняя данность. Решение не должно требовать
изменений portal, room lifecycle, WebRTC-NET protocol или Quake II server со
стороны Dos.Zone maintainers.

## Предварительное направление

Провести ограниченный feasibility spike внешнего Linux VPS gateway. Не считать
VPS выбранным production-решением и не заменять локальные поставки до
прохождения gates этого ADR.

Если spike подтвердит многопользовательскую маршрутизацию, приемлемую задержку
и простой server-browser UX, целевым продуктом может стать гибрид:

- hosted VPS — основной и наиболее простой путь для игрока;
- текущие локальные Windows/Linux packages — fallback, диагностический режим и
  вариант для самостоятельного hosting.

Это направление сохраняет существующий native Linux transport. Оно не
подразумевает перенос Windows WebView2 gateway на сервер.

## Желаемый пользовательский сценарий

1. Пользователь устанавливает или обновляет обычный Q2PRO-X.
2. Один раз добавляет URL `Custom` source либо получает готовую конфигурацию.
3. Открывает стандартный server browser Q2PRO-X.
4. Видит доступные комнаты Dos.Zone с настоящими hostname, map, players и ping.
5. Выбирает комнату и подключается обычной командой Quake II.
6. После `Disconnect` выбирает другую комнату без перезапуска игры.

Без изменений Q2PRO-X это будет существующая вкладка `Custom`, а не буквально
новая вкладка с надписью `Dos.Zone`. Отдельное брендированное имя потребовало бы
участия maintainer Q2PRO-X и не является условием решения.

`Favorites` годятся для первого ручного spike, но хуже как production UX:

- слоты `adr0..adr15` принадлежат пользователю и ограничены шестнадцатью;
- комнаты динамичны;
- stale адреса не удаляются автоматически;
- Dos.Zone смешивается с личным списком серверов.

Для итогового варианта предпочтителен проверенный `Custom` source из ADR 0004.

## Почему текущий gateway нельзя просто перенести на VPS

### Loopback-only boundary

Обе текущие реализации намеренно слушают только `127.0.0.1`. Managed protocol,
endpoint validation и security tests также требуют loopback. Открыть текущий
socket на `0.0.0.0` одной заменой адреса означало бы удалить существующую
границу доверия без rate limiting, клиентской изоляции и защиты от публичного
UDP abuse.

Минимальный spike может оставить gateway loopback-only и поставить перед ним
маленький публичный UDP relay. Production-вариант потребует явной public-edge
границы и отдельных security gates.

### Один захваченный UDP source

Текущий Windows gateway и оба Linux transport слоя запоминают первый UDP source
tuple и отбрасывают datagrams от второго. Это правильная защита локального
single-session flow, но она означает:

```text
один неизменённый gateway process
  = один native UDP source
  = одна WebRTC peer session
```

Поэтому один публичный `VPS:port` на комнату не сможет обслужить нескольких
игроков через один существующий gateway process. Не доказано, что несколько
Quake II клиентов можно безопасно multiplex-ить через один WebRTC-NET peer,
даже если различать их по Quake `qport`.

Самый консервативный вариант — отдельный native gateway на каждую пару
`player session + room`, а внешний UDP frontend только демультиплексирует
клиентов и пересылает их через разные loopback sockets.

### Server browser создаёт соединение раньше, чем пользователь нажмёт Join

Q2PRO-X отправляет connectionless `status` запросы всем показанным endpoints.
Если первый UDP packet должен сначала запустить WebRTC gateway, получить fresh
room configuration, завершить signaling и дождаться OOB readiness, ответ может
не уложиться в timeout server browser.

Отсюда три возможных варианта, ни один пока не выбран:

1. **Lazy session:** первый `status` создаёт player-room gateway, исходный
   packet буферизуется, Q2PRO-X получает ответ после readiness. Это самый
   короткий код, если exact browser retries дают достаточно времени.
2. **Pre-allocation:** персональный Custom source создаёт готовые endpoints до
   возврата списка. Это предсказуемее, но требует способа устойчиво различать
   пользователей и создаёт WebRTC peers для комнат, в которые они могут не
   войти.
3. **Shared status cache + per-player connect gateway:** один room probe хранит
   свежий raw status response, а отдельный gateway создаётся на
   `getchallenge/connect`. Это экономит peers, но добавляет Quake-aware routing,
   cache freshness и новый correctness surface.

Сначала проверяется lazy session. Остальные варианты нужны только если точные
тайминги Q2PRO-X его опровергнут.

### `status` и `connect` должны попасть в одну player session

Frontend естественно различает игроков по публичному UDP source tuple:

```text
(source IP, source UDP port, room endpoint, room generation)
```

Но пока не доказано, что exact Q2PRO-X использует один и тот же UDP socket для:

```text
status -> getchallenge -> connect -> gameplay
```

Если source port меняется, простое tuple mapping потеряет подготовленный
gateway. Ослаблять isolation до «принимать любого клиента с этого IP» нельзя:
несколько игроков могут находиться за одним NAT. Нужен packet trace exact
Q2PRO-X до реализации routing logic.

### Комната и credentials имеют поколение и срок жизни

Room alias, owner, readiness и RTC/ICE данные должны повторно проверяться через
те же bounded catalog contracts, что и в локальном launcher. Gateway нельзя
повторно использовать после закрытия комнаты, смены generation или истечения
fresh credentials. Секретные transport данные остаются только в памяти одной
gateway session.

## Минимальная предполагаемая схема

```text
Q2PRO-X clients
      |
      | public Quake II UDP
      v
Linux VPS public UDP frontend
      |-- room catalog / Custom source
      |-- source-tuple session table
      |-- rate limits and idle expiry
      |
      +-- client A, room X -> loopback socket -> native gateway A-X -> WebRTC
      +-- client B, room X -> loopback socket -> native gateway B-X -> WebRTC
      +-- client A, room Y -> loopback socket -> native gateway A-Y -> WebRTC
                                                               |
                                                               v
                                                        Dos.Zone rooms
```

Public frontend не реализует WebRTC. Existing native Linux gateway продолжает
отвечать только за одну WebRTC peer session. Frontend отвечает только за UDP
demultiplexing, lifecycle, quotas и room-to-port registry.

Это гипотеза минимальной архитектуры, а не разрешение немедленно писать новый
daemon. Сначала должны пройти packet/timing gates ниже.

## Основные сомнения и риски

### Correctness

- неизвестен exact `status -> connect` source tuple Q2PRO-X;
- WebRTC startup может быть медленнее browser timeout;
- неизвестно, влияет ли подготовленный WebRTC peer без игрового `connect` на
  room peer count, player slots или ресурсы browser owner;
- закрытие и повторное создание комнаты с тем же alias не должно направлять
  клиента в старую session;
- несколько игроков за одним NAT должны оставаться независимыми;
- UDP NAT rebinding не должен случайно передавать session другому клиенту.

### Производительность

Hosted path добавляет hop:

```text
player -> VPS -> WebRTC direct/TURN -> Dos.Zone owner
```

Задержка зависит от географии VPS, маршрута к owner и использования TURN.
Локальный FPS не показывает network latency или packet cadence. Нужны
сравнительные RTT, loss и gameplay observations для local и hosted paths.

Для 10–20 потенциальных пользователей native gateway process per active
player-room может быть приемлем, но это требуется измерить. Server-browser
refresh не должен создавать неограниченное число WebRTC peers и процессов.

### Availability and operations

- VPS становится общей точкой отказа;
- кто-то должен оплачивать, обновлять и наблюдать сервис;
- нужны автоматический restart, bounded cleanup и rollback;
- room catalog или signaling outage затрагивает сразу всех hosted users;
- публичный сервис должен иметь понятный status/diagnostic ответ без утечки
  credentials.

Это принципиально отличается от локальной поставки, где нет постоянно
работающей общей инфраструктуры.

### Security and privacy

- публичный UDP endpoint будут сканировать и засорять произвольными packets;
- connectionless Quake status нельзя превращать в amplification vector;
- необходимы per-IP и global quotas, предел active sessions и idle expiry;
- VPS видит IP адреса игроков — retention и отсутствие payload logging должны
  быть задокументированы;
- room/ICE/TURN credentials, SDP, candidates и tokens запрещено сохранять в
  argv, environment, files или обычных logs;
- frontend должен принимать только поддерживаемые `q2` rooms из валидированного
  Dos.Zone catalog, а не произвольные alias/RTC inputs от сети;
- compromise VPS затрагивает всех hosted users, поэтому release/update chain
  сервиса должна быть воспроизводимой и минимальной.

### Зависимость от внешнего проекта

Решение не должно reverse-engineer private portal automation или требовать от
Dos.Zone поддерживать наш продукт. При этом централизованный gateway может
создать больше signaling/TURN нагрузки, чем несколько локальных пользователей.
До production запуска следует показать maintainers измеренный профиль нагрузки
и убедиться, что он приемлем, не перекладывая на них реализацию.

## Альтернативы

### Сохранить только локальные packages

Плюсы:

- уже существующая lifecycle и security boundary;
- нет общей инфраструктуры и публичного UDP attack surface;
- один gateway однозначно принадлежит одному игроку.

Минусы:

- установка и запуск helper на каждом компьютере;
- отдельные Windows/Linux release concerns;
- Windows зависит от WebView2;
- выбор комнат пока происходит вне обычного server browser.

Это действующий fallback и контрольный baseline.

### Полностью hosted VPS gateway

Плюсы:

- лучший UX: игроку не нужен локальный add-on;
- одна Linux implementation обслуживает Windows и Linux Q2PRO-X;
- отсутствует Windows WebView2 transport path;
- обновления gateway выполняются в одном месте;
- штатный server browser становится основной точкой выбора комнат.

Минусы:

- требуется новый многопользовательский public-edge/session layer;
- дополнительная задержка и общая точка отказа;
- постоянная эксплуатация, security и abuse handling;
- текущий gateway нельзя использовать как общий endpoint без orchestration.

### Hosted default + local fallback

Плюсы:

- простой путь для большинства игроков;
- локальная поставка остаётся при outage, плохом маршруте или self-hosting;
- существующая работа не выбрасывается;
- rollout можно проводить на нескольких тестерах без необратимого перехода.

Минусы:

- поддерживаются два deployment mode;
- документация должна ясно объяснять, когда нужен локальный fallback;
- часть release gates остаётся для обеих поставок.

Это предпочтительное направление, только если hosted feasibility gates дадут
PASS.

### Попросить maintainer Q2PRO-X добавить first-class Dos.Zone provider

Это может улучшить название вкладки и catalog discovery, но не решает
UDP-to-WebRTC transport. Рассматривать можно позже; делать maintainer
Q2PRO-X обязательной зависимостью сейчас не требуется.

### Искать существующий штатный Quake II relay/proxy

Перед написанием protocol-aware frontend reviewer должен проверить, существует
ли поддерживаемый Quake II/Q2PRO relay, который уже корректно multiplex-ит
нескольких клиентов, challenge/connect и NAT. Если такой компонент совместим с
WebRTC-NET boundary, он предпочтительнее собственного Quake-aware proxy.

## Feasibility gates до production-кода

1. **Exact client trace:** записать packet trace чистого Q2PRO-X для
   `status -> getchallenge -> connect -> gameplay -> disconnect`; подтвердить
   source ports и retry timings.
2. **One-player VPS spike:** существующий native Linux gateway за минимальным
   UDP relay принимает публичного Windows Q2PRO-X; status, map entry, movement,
   fire и disconnect проходят.
3. **Startup timing:** measured gateway readiness и первый status response
   укладываются в фактические retries Q2PRO-X без synthetic server response.
4. **Two-player isolation:** два игрока одновременно входят в одну комнату
   через один публичный room endpoint, но разные WebRTC gateway sessions;
   packets и lifecycle не смешиваются.
5. **Same-NAT:** два клиента за одним public IP остаются независимыми.
6. **Room switch:** один Q2PRO-X выполняет `Room A -> Disconnect -> Room B` без
   перезапуска процесса.
7. **Room lifecycle:** inactive room close удаляет только её endpoints; active
   close корректно disconnect-ит затронутых игроков; alias reuse не оживляет
   stale session.
8. **Host impact:** измерить WebRTC peers, Quake player slots и browser owner
   resources для status-only и active sessions.
9. **Latency:** сравнить local и VPS path на одинаковых client/room; сохранить
   RTT distribution, loss/drop counters и gameplay observation. Приемлемый
   предел выбирается по измеренному baseline, а не по предположению.
10. **Load cap:** измерить CPU, RAM, file descriptors, UDP sockets и cleanup при
    1, 5 и 10 одновременных player-room sessions; установить hard cap.
11. **Abuse:** malformed/oversized datagrams, scan flood и status flood не
    создают unbounded processes, memory, logs или amplification.
12. **Failure cleanup:** client disappearance, gateway crash, room close,
    service restart и hard kill не оставляют processes, sockets, credentials
    или stale catalog rows.
13. **Custom source:** exact Q2PRO-X показывает только ready public endpoints и
    обновляет список по `Refresh` без изменения Favorites.

Gates 1–4 являются минимальным решением вопроса «архитектура вообще
жизнеспособна». До их PASS не следует строить installer, control panel,
container platform, database, authentication system или autoscaling.

## Критерий решения

Hosted направление получает `GO`, если:

- exact Q2PRO-X routing подтверждён packet trace;
- два одновременных игрока корректно используют один публичный room endpoint;
- startup и room switching работают без изменения Q2PRO-X и Dos.Zone;
- добавленная задержка приемлема относительно локального baseline;
- lifecycle, quotas и credential handling проходят gates;
- сервис остаётся операционно разумным для аудитории порядка 10–20 человек.

Направление получает `NO-GO` или возвращается к локальному baseline, если для
него требуется изменить Quake II/HumbleNet protocol, Dos.Zone service или
Q2PRO-X binary; если shared endpoint нельзя надёжно разделить между игроками;
либо если задержка и постоянная эксплуатация перевешивают отсутствие локальной
установки.

## Вопросы ревьюеру

1. Можно ли безопасно multiplex-ить несколько Quake II clients через один
   WebRTC-NET peer с использованием `qport`, или отдельный peer на игрока
   действительно обязателен?
2. Достаточен ли тонкий source-tuple UDP demultiplexer перед неизменёнными
   loopback gateways, или в Quake challenge/connect есть скрытая связь, которая
   требует protocol-aware proxy?
3. Какой самый простой способ пережить startup latency: lazy gateway,
   pre-allocation или cached raw status?
4. Существует ли штатный Q2PRO/Quake II relay, server-browser source или proxy,
   который устраняет необходимость писать эту часть самостоятельно?
5. Не создаёт ли WebRTC peer для одного status probe player slot или заметную
   нагрузку на browser owner?
6. Какие минимальные rate limits и anti-amplification правила необходимы для
   публичного Quake II UDP endpoint?
7. Какие gates из списка избыточны, а какие критические проверки пропущены?
8. При малой аудитории разумнее ли hosted-default + local-fallback, либо
   эксплуатационная сложность всё равно делает локальный helper лучшим
   конечным продуктом?

## Последствия

Положительные при успешном spike:

- Windows и Linux пользователи получают один и тот же нативный Q2PRO-X UX;
- локальный helper перестаёт быть обязательным;
- WebView2-дефекты не влияют на hosted path;
- комнаты выбираются и переключаются стандартным способом.

Отрицательные:

- появляется постоянно работающий сервис и публичная security boundary;
- lifecycle меняется с single-user на multi-user;
- один gateway на комнату недостаточен без доказанного multiplexing;
- потребуется измеренный operational cap и владелец VPS.

Пока ADR имеет статус `Proposed`, никакая из этих выгод не считается
доказанной, а текущие локальные поставки не объявляются устаревшими.
