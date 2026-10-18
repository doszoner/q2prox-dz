# Dos.Zone gateway для существующего Q2PRO-X: design

> Исторический документ; не инструкция к текущей версии. Перенесён 2026-10-18.
> Отметки задач и результаты относятся к исходной дате; проверки сейчас не повторялись.
> Пути прототипов и локальных evidence могут отсутствовать. Актуальное состояние — в индексе docs/README.md.

- Статус: **In review, revision 2**
- Дата: 2026-08-22
- Нормативность: документ задаёт продуктовый MVP; transport evidence находится
  в `TECHNICAL-REPORT.md`, `EXECUTION-CHECKLIST.md` и `EVIDENCE-SUMMARY.md`
- Related:
  - `docs/reviews/native-quake2-client-doszone-design-review-20260821.md`
  - `docs/reviews/native-quake2-client-doszone-design-review-response-20260822.md`
  - `docs/adr/0001-windows-webrtc-transport-backend.md`

## 1. Решение

Первый поставляемый вариант — per-user add-on для уже установленного Q2PRO-X.
Add-on показывает собственный компактный список активных комнат Dos.Zone. После
выбора одной комнаты он подготавливает одну UDP↔WebRTC-NET-сессию и запускает
существующий `Q2PRO-X.exe` с:

```text
+connect 127.0.0.1:<random-session-port>
```

Игра и игровые данные повторно не распространяются. Add-on не меняет
`Q2PRO-X.exe`, PAK, пользовательские конфиги, обычный ярлык и штатные источники
серверов.

Интеграция списка внутрь Q2PRO-X не входит в Phase 1. Отдельный source/tab
`Dos.Zone` предпочтителен как Phase 2, если появится поддерживаемый source или
extension contract Q2PRO-X. `Custom`/`servers.lst` и synthetic status допустимы
только как поздний compatibility adapter после отдельного runtime proof.

Dos.Zone считается внешней системой. MVP использует наблюдаемые read-only
контракты и не требует изменений от maintainers Dos.Zone.

## 2. Основания и границы уверенности

На Linux x86-64 подтверждены Gates A–D: live signaling, alias, fresh ICE/TURN,
DataChannel, byte-identical UDP bridge и вход неизменённого Q2PRO-X 1.4 в карту
Dos.Zone по protocol 35. Доказаны размеры datagrams 1, 32, 512, 1390 и 1400
bytes и двустороннее игровое действие.

Это доказывает транспортную архитектуру одной сессии, но не доказывает:

- Windows transport backend и installer;
- launcher room selector и process supervision;
- clean Q2PRO-X profile;
- совместимость content/mod/map конкретной комнаты;
- reconnect, update, signing и crash policy;
- создание или hosting комнаты.

Публичная поставка запрещена до Platform Gate P и Windows Gates W1–W6.

## 3. Цель, пользователи и границы

### Цель MVP

- Пользователь существующего Q2PRO-X видит готовые публичные Q2-комнаты
  Dos.Zone в launcher и входит в выбранную игру без ручной консольной команды.
- Joining не открывает portal/browser UI и не показывает детали WebRTC.
- Отказ add-on не влияет на обычный запуск и серверные sources Q2PRO-X.

### Пользователи и внешние владельцы

- Пользователь владеет установленным Q2PRO-X, его assets и config.
- Add-on владеет только своим каталогом, launcher, gateway, кэшем и журналами.
- Dos.Zone и его read-only endpoints принадлежат внешним maintainers.
- Delivery owner владеет signing, update manifest, SBOM и release evidence.

### Не входит в MVP

- создание, hosting и администрирование комнаты без официального Dos.Zone flow;
- DOM scraping, browser-cookie reuse и undocumented write API;
- изменение Dos.Zone или обязательная работа его maintainers;
- `Custom` source, `servers.lst` и synthetic server rows;
- модификация или замена `Q2PRO-X.exe`;
- публичный shared UDP gateway;
- произвольные Quake II clients, macOS и несколько одновременных сессий;
- бесшовный reconnect после перезапуска browser host;
- поставка карт, модов, PAK или иных игровых assets.

## 4. Пользовательский контракт и поставка

Пользователь получает подписанный release artifact, а не Git checkout. Per-user
installer без elevation:

1. Находит `Q2PRO-X.exe` либо один раз просит указать путь.
2. Устанавливает add-on в собственный каталог, например
   `%LOCALAPPDATA%\Q2PRO-X\DosZone\`.
3. Создаёт отдельный ярлык `Q2PRO-X — Dos.Zone`.
4. Не заменяет executable, assets и config игры.
5. Имеет uninstall, удаляющий только add-on-owned файлы и ярлык.

Ярлык открывает небольшой room selector с полями:

- name;
- map;
- players/maxPlayers;
- RTC region/domain;
- freshness/availability.

До установления реального соединения launcher не показывает `Ping`: loopback
RTT не является latency комнаты. Launcher не создаёт фиктивные player names.

Действие `Join` подготавливает выбранную room и автоматически запускает exact
Q2PRO-X. Действие `Создать игру на Dos.Zone` может открыть официальный create
flow во внешнем browser и после возврата обновить каталог; add-on не
автоматизирует portal и не обещает определить созданную комнату без
поддерживаемого contract.

Если Q2PRO-X уже запущен, Phase 1 просит закрыть его и не пытается внедрять
команды в чужой процесс.

## 5. Компоненты и ответственность

### Launcher UI

- показывает каталог и per-room ошибки;
- принимает `Refresh`, `Join` и `Создать игру на Dos.Zone`;
- запускает и сворачивает Q2PRO-X;
- не содержит WebRTC или Quake II protocol logic.

### Catalog adapter

- читает и валидирует наблюдаемые Dos.Zone endpoints;
- преобразует external schema в ограниченную внутреннюю room model;
- изолирует mapping room id → alias;
- не хранит ICE credentials в catalog cache.

### Session coordinator

- владеет одной выбранной room и state machine;
- выполняет fresh alias/RTC validation только при `Join`;
- запускает transport, UDP bridge и Q2PRO-X в определённом порядке;
- отменяет и очищает сессию при ошибке или выходе Q2.

### WebRTC transport backend

- устанавливает WSS signaling, alias lookup, ICE/STUN/TURN и DataChannel;
- реализует единый внутренний datagram contract;
- выбирается Platform ADR и не подменяется silent fallback-ом.

### UDP bridge

- bind только на случайный high port `127.0.0.1` для активной сессии;
- сохраняет границы и payload Quake II datagrams;
- обслуживает один разрешённый Q2 UDP source и отклоняет второй.

### Process/update supervisor

- владеет процессами launcher/gateway/Q2PRO-X и их завершением;
- на Windows использует Job Object или эквивалентную parent-death supervision;
- владеет signed update, rollback и cleanup add-on state.

## 6. Наблюдаемые внешние контракты

Endpoints не считаются гарантированным публичным API Dos.Zone. Доступ к ним
разрешён только через versioned catalog adapter с allow-list scheme/host,
запретом неожиданных redirects, bounded timeout/retry и лимитами на body size,
JSON depth, room count и string lengths.

### Каталог комнат

`GET https://cloud.dos.zone/room/get` возвращает `rooms` и `timestamp`.
Отображаемая row должна иметь валидные `room`, `roomReady`, `rtcServer`,
`players` и `createSettings` с `prefix`, `name`, `map`, `maxPlayers`.

Каждая room парсится независимо. Невалидная row пропускается с aggregate
diagnostic и не выключает весь каталог. Last-known catalog разрешён только для
отображения с явной отметкой `устарело`; он никогда не разрешает соединение без
fresh проверки существования room.

### Alias

Наблюдаемое соответствие `room.q2.<segment>.<number>` → `q2.<number>` находится
в одном versioned mapping adapter. При появлении явного alias field он имеет
приоритет над вычислением.

`GET https://net.dos.zone/lookup` проверяется только для выбранной комнаты при
`Join`. Ошибка помечает эту room, а не скрывает остальные строки.

### ICE servers

Непосредственно перед каждой новой сессией add-on получает
`GET https://cloud.dos.zone/rtc/servers`, находит exact `room.rtcServer` и
передаёт полный свежий `iceservers` backend-у до P2P initialization.

ICE username/credential живут только в памяти сессии. Прошлый bundle и catalog
cache не являются fallback. Ошибка schema, domain или fresh fetch блокирует
только выбранное соединение.

### Escaping

External strings имеют отдельное ограничение и escaping для UI, log и JSON.
Phase 1 не генерирует Quake status wire. Если он появится в Phase 2, для него
действует отдельная byte-level validation и собственное escaping.

## 7. Session lifecycle

Нормативная state machine:

```text
Idle
  → CatalogReady
  → PreparingRoom
  → TransportReady
  → LaunchingQ2
  → Active
  → Closing
  → Idle

Любое состояние → Failed → Closing
```

### Просмотр каталога

1. Launcher получает catalog и показывает валидные rows без alias/ICE preflight.
2. Refresh отменяет старый request и атомарно заменяет UI model.
3. Ошибка одной row не ломает остальные; общая ошибка endpoint сохраняет только
   маркированный stale display, если он существует.

### Join

1. Пользователь выбирает одну room.
2. Coordinator повторно подтверждает fresh room existence, alias и RTC domain.
3. Получается fresh ICE bundle и запускается выбранный transport backend.
4. Backend достигает transport readiness.
5. При доказанной совместимости gateway отправляет настоящий stateless Q2
   `status` request browser-host и ждёт стандартный `print` response. Это только
   optional readiness check, не synthetic response пользователю.
6. Если warm-up не поддержан, gateway заранее поднимает WSS/PeerId и bounded
   buffer сохраняет первый настоящий `getchallenge` до готовности DataChannel.
7. Gateway bind-ит один случайный loopback port.
8. Launcher запускает `Q2PRO-X.exe +connect 127.0.0.1:<port>`.
9. После первого datagram от supervised Q2 source gateway фиксирует source;
   остальные UDP sources отклоняются.
10. После входа в карту launcher сворачивается, но продолжает supervision.

### Local control и trust boundary

Readiness и команды `prepare`, `ready`, `stop` передаются по private
per-user control channel. Windows implementation использует named pipe с
случайной per-launch capability либо механизмом с эквивалентными свойствами.
Capability не передаётся внешним процессам и не является ICE credential.

Случайный UDP port и порядок supervised launch уменьшают возможность захвата
маршрута локальным процессом, но не считаются доказательством process identity.

### Завершение и отказ

Session завершается по Q2 process exit, explicit stop, WebRTC close или
подтверждённой transport failure. Нормативного 30-second gameplay idle timeout
нет. Допустим только большой защитный orphan timeout, обоснованный и проверенный
отдельно от игрового timeout.

Ошибка показывает стадию: catalog, room gone, RTC config, signaling, ICE,
DataChannel, Q2 handshake или content. После `Closing` не остаётся process,
socket, named pipe или session WebView data.

## 8. Platform/Transport decision

Windows backend ещё не выбран. Нормативный процесс выбора находится в
`docs/adr/0001-windows-webrtc-transport-backend.md`.

Допустимы два кандидата:

1. WebView2 transport host с packaged WebRTC-NET JS/WASM.
2. Поддерживаемый native Windows port современного Chromium WebRTC backend.

Старый Chromium/WebRTC tag, silent microstack fallback, WSL/Wine и ручные DLL
не являются shipping path. До PASS Platform Gate P нельзя начинать installer,
public beta или Phase 2 integration. WebView2 — первый кандидат spike, а не
заранее принятое production решение.

## 9. Content compatibility

Transport success не означает совместимость game content. Browser-host грузит
конкретные baseq2 PAK и `/3zb2/pak6.pak`, запускается с `+game 3zb2` и может
использовать карты, отсутствующие у desktop client.

Add-on не загружает и не распространяет игровые assets без отдельного законного
distribution decision. До соединения он может фильтровать room только по
доказанному content profile; неизвестный content не объявляется совместимым.

Поддерживаемая Q2PRO-X distribution/content profile должна быть зафиксирована
до public beta и проверена W6 на clean installation.

## 10. Безопасность, release и observability

- UDP bind разрешён только на loopback.
- HTTP/WSS endpoints имеют allow-list, size limits, cancellation и bounded
  retry с jitter.
- ICE/TURN credentials запрещены в args, env, config, logs, catalog cache и
  разрешённых crash artifacts.
- Full process-memory dumps gateway отключены либо заменены controlled crash
  reporting без памяти, содержащей secrets.
- Logs содержат version, redacted state transitions, room id, alias,
  counters и stop reason; SDP, candidates и credentials запрещены.
- До public beta назначаются Authenticode owner и key custody, подписываются
  binaries и update manifest, публикуются SBOM и third-party notices.
- Update имеет version pin, rollback policy и проверку подписи до установки.
- Если выбран WebView2, transport view запрещает arbitrary navigation,
  DevTools и external windows; per-session user-data directory ограничен и
  очищается как add-on-owned state.

Основные диагностические counters: catalog rows accepted/rejected, selected
room validation stage, signaling/ICE/DataChannel transitions, datagrams/bytes
по направлениям, dropped second-source/oversize packets и cleanup result.

## 11. Rollout и gates

### Phase 0 — transport baseline

Gates A–D и sanitized evidence остаются воспроизводимым Linux regression
reference. Они не заменяются product tests.

### Platform Gate P — выбрать Windows backend

ADR может стать `Accepted` только когда один кандидат на clean Windows x64 с
production flags проходит:

- fresh signaling, ICE и alias;
- binary echo 1/32/512/1390/1400;
- UDP echo с сохранением datagram boundaries;
- Q2PRO-X Gate D: handshake, карта и двустороннее игровое действие;
- secret, licensing и reproducible-build checks выбранного backend.

Если оба кандидата не проходят, локальный Windows release остаётся NO-GO и
требуется новый architecture review.

Accepted evidence 2026-08-22: WebView2 Candidate A прошёл transport и
двустороннее Q2PRO-X action на exact artifact. Launcher-owned Job Object и
managed protocol закрыли Q2/gateway/room/owner lifecycle. Production `/Brepro`,
packaged notices and clean-Windows install/update/uninstall passed. Signing
остаётся W5 delivery blocker, но Candidate A и ADR 0001 приняты.

### Windows product gates

- **W1 Build:** accepted backend и gateway воспроизводимо собираются в
  production configuration без WSL/Wine, старого unpatched Chromium, ручных DLL
  и silent fallback.
- **W2 Transport:** Gates A–C повторены на clean Windows x64 exact artifact.
- **W3 Q2PRO-X:** exact native `Q2PRO-X.exe` входит в активную room; обе стороны
  видят игровое действие.
- **W4 Launcher:** room selector показывает catalog, lazy validation работает,
  один random endpoint запускает Q2 через `+connect`; user config и обычный
  ярлык остаются byte-identical/работоспособны после clean и forced exit.
- **W5 Delivery:** signed install/update/rollback/uninstall проходят на clean
  per-user profile; после exit нет orphan processes, ports и control resources.
- **W6 Content:** поддерживаемый clean content profile входит минимум в одну
  базовую и одну representative non-base room; missing map/mod/PAK даёт
  различимую ошибку или room заранее исключается по доказанному contract.

Public release разрешён только после PASS P и W1–W6 на exact signed artifacts.
Rollback — uninstall add-on или обычный ярлык Q2PRO-X; игра и user data не
требуют восстановления.

## 12. Testing strategy

Минимальный product test set:

1. Catalog/schema tests: malformed/oversized response, limits и fuzz.
2. Room-id→alias mapping и per-room degradation.
3. Fresh ICE ordering, expiration и secret redaction.
4. Backend contract 1/32/512/1390/1400 bytes.
5. UDP second-source, spoof attempt и oversize rejection.
6. Session state-machine cancellation и race tests.
7. Q2 status warm-up fixture либо bounded first-packet buffer fixture.
8. Q2 crash, launcher crash и forced termination cleanup.
9. Exact user config hashes до/после; обязательно, если вернётся `Custom`.
10. Clean Windows Q2PRO-X E2E.
11. Clean supported content profile E2E.
12. Install/update/rollback/uninstall exact signed artifact.

Prototype tests 3/3 проверяют только options и fake transport runtime и не
считаются покрытием этих product gates.

## 13. Acceptance criteria Phase 1

- Пользователь с поддерживаемой установкой Q2PRO-X ставит один signed per-user
  add-on без administrator rights и повторной загрузки игры.
- Add-on не изменяет `Q2PRO-X.exe`, game assets и user config.
- Новый ярлык показывает готовые Q2 rooms с name, map, players/maxPlayers,
  region и freshness.
- Выбор room создаёт один случайный loopback endpoint только для активной
  session, а не N sockets для каталога.
- `Join` автоматически запускает exact Q2PRO-X и завершает protocol handshake,
  вход в карту и двустороннее игровое действие.
- Для joining пользователь не открывает browser и не вводит console command.
- До соединения не показывается фиктивный ping или synthetic player identity.
- Room/alias/ICE/WebRTC/Q2/content failures различимы и не оставляют orphan
  process/socket/control resource.
- Второй UDP source и non-loopback bind отклоняются.
- ICE/TURN credentials отсутствуют в args, env, config, logs, cache и
  разрешённых crash artifacts.
- Clean exit, forced termination и uninstall удаляют только add-on-owned state.
- Platform Gate P и W1–W6 проходят на exact signed artifacts.
- Обычный ярлык и обычные server sources Q2PRO-X работают независимо.

## 14. Post-MVP tracks

### Phase 2 — список внутри Q2PRO-X

Предпочтителен first-class source/provider `Dos.Zone`, который получает room
model и команду `Join(room-id)` через local IPC. Он требует поддерживаемого
Q2PRO-X source/extension contract.

Если provider невозможен, `Custom` adapter рассматривается отдельно и обязан
доказать exact file/HTTP source, config persistence, status keys/player count и
latency semantics byte-level/runtime tests. Loopback RTT запрещено выдавать за
network ping; synthetic player identities запрещены.

### Room creation/hosting

Создание native-hosted room требует supported write/ownership/lifecycle API,
multi-peer routing и отдельного threat model. При наличии такого API создаётся
отдельный Hosting ADR. До этого доступно только открытие официального Dos.Zone
create flow без scraping/cookie reuse/private API emulation.

### External gateway

Публичный shared UDP↔WebRTC gateway остаётся отдельной альтернативой поставки с
multi-client routing, abuse protection и постоянной operations ownership. Он не
является fallback внутри текущего MVP.

## 15. Риски и открытые решения

### Blockers до implementation/public beta

1. Platform ADR не принят и Windows backend не прошёл P.
2. Не назначены поддерживаемые Windows versions и clean test environment.
3. Не зафиксирован supported Q2PRO-X distribution/content profile.
4. Не назначены signing/update owner и key custody.
5. Наблюдаемые Dos.Zone endpoints не имеют гарантированного публичного contract;
   adapter обязан оставаться fail-closed на `Join` и tolerant per-room в UI.

### Риски, принятые дизайном

- Reconnect не бесшовный: session закрывается, пользователь повторяет `Join`.
- Room может исчезнуть между catalog и `Join`: fresh validation возвращает
  `room gone`.
- WebView2 и native backend имеют разные packaging/security costs: решение
  принимается evidence, а не размером первоначального glue code.
- Поддержка неизвестного content не обещается.

## Normative lifecycle refinement: ADR 0002

Process ownership, secret transport and readiness are governed by `docs/adr/0002-launcher-owned-session-lifecycle.md` and `session-coordinator/MANAGED-GATEWAY-PROTOCOL.md`.

The coordinator, gateway, session-owned WebView2 descendants and the Q2 instance launched for the join operation form one bounded session. Q2 starts only after a validated loopback endpoint and a real Quake II OOB ping/ack. Room JSON is transferred only through inherited anonymous pipes and never through command lines, environment variables or files. Ordinary Q2 startup remains outside this session. Gameplay inactivity is not a failure signal.
