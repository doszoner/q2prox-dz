# Общий многокомнатный шлюз q2prox-dz

- **Статус:** реализуется; Linux gateway-layer PASS, Windows runtime gate pending
- **Дата:** 2026-09-03
- **Область:** Windows и Linux
- **Заменяет после принятия:** фиксированную схему `3 q2ded + 2 q2` из
  [ADR 0004](../adr/0004-q2pro-x-custom-source-room-hub.md) и
  [дизайна поддержки dedicated](2026-09-03-dedicated-server-support.md)

**Подтверждённый amendment/status:** в наблюдении 20--40 секунд deployed
`wss://net.js-dos.com:444` не прислал `AliasQueryResult` ни для `=alias`, ни для
полного prefix; обычная scheduling race по этому наблюдению маловероятна. Это
наблюдение, а вывод о неподдержанном deployed API остаётся inference. Native controlled peer не
достиг `READY`; чтение кода показывает ту же query-dependent границу в
Linux/Windows gateway. Dos.Zone server менять не требуется.

**Evidence boundary:** exact Q2PRO-X 40-endpoint source gate остаётся
`PENDING`, пока evidence не записан в durable verification docs; он не
доказывает handshake, map entry или gameplay. Gameplay здесь не объявляется.

## Решение кратко

В одном сеансе q2prox-dz запускаются:

```text
один Q2PRO-X
+ один coordinator
+ один общий gateway process
    + один WebRTC-NET/HumbleNet runtime
    + одно signaling-соединение и один собственный PeerId
    + N удалённых peer connections
    + N локальных UDP endpoints
```

Каждой доступной комнате соответствует отдельный случайный адрес
`127.0.0.1:<port>`, но не отдельный процесс gateway. Q2PRO-X видит эти адреса
через существующий Custom source, сам посылает обычный Quake II `status` и
подключается к выбранному адресу. Gateway маршрутизирует датаграммы по паре
«локальный endpoint — virtual PeerId».

Для каждой цели gateway получает уникальный `routePeerId` через
`humblenet_p2p_virtual_peer_for_alias(alias)`. Предлагаемый read-only API
`humblenet_p2p_resolved_peer_for_virtual_peer(routePeerId)` должен возвращать
`0` для invalid/unresolved/closed либо concrete `1..0x7fffffff`; getter ничего
не создаёт. `concretePeerId` нужен только для duplicate guard и поля `READY
peer=`.

Список является динамическим и не делится на квоты `q2` и `q2ded`. Launcher
сначала целиком проверяет снимок каталога, затем отправляет полный heartbeat;
coordinator применяет разницу. Сбой одной цели не скрывает остальные комнаты.
Молчаливого усечения списка нет.

## Причина изменения

Версия `1.0.0-rc13` обслуживает пять комнат пятью gateway-процессами. На
Windows каждый gateway создаёт своё дерево WebView2. Один измеренный gateway
без Q2PRO-X занимает примерно 139--141 MiB private memory и 347--352 MiB
working set. На Linux четыре idle gateway вместе с coordinator и guardian
занимали около 118 MiB RSS и 6,4--6,8% одного логического ядра.

Увеличение констант до `20 q2 + 20 q2ded` функционально просто, но сохраняет
линейный рост наиболее дорогой части. На Windows это означало бы десятки
WebView2-деревьев. Такой предел нельзя считать долговременным решением.

В текущем WebRTC-NET `62fe52c` поддержаны virtual allocation,
`sendto`/`recvfrom`, read-only resolver и registration getter:

- `humblenet_p2p_virtual_peer_for_alias(alias)` даёт отдельный virtual route
  key на alias;
- `humblenet_p2p_sendto()` и `humblenet_p2p_recvfrom()` работают с virtual
  route PeerId;
- реализация WebRTC хранит несколько `RTCPeerConnection` в одной карте.

Следовательно, переиспользование runtime является использованием штатной
возможности зависимости, а не новым транспортным протоколом.

## Цели

- показывать все доступные совместимые `q2` и `q2ded` комнаты без квот по виду;
- поддерживать как минимум 40 комнат в одном сеансе, не создавая 40 WebView2
  или native gateway процессов;
- сохранить настоящий Quake II `status`, имена, карту, число игроков и ping;
- сохранить переключение комнат внутри штатного меню Q2PRO-X;
- закрывать, восстанавливать и удалять одну комнату независимо от остальных;
- сохранить существующие границы секретов, loopback и владения процессами;
- иметь одинаковый внешний контракт и одинаковые проверки на Windows/Linux.

## Не входит

- изменение Dos.Zone, Q2PRO-X, Quake II protocol или серверов;
- синтез `status`, multiplexing нескольких комнат через один UDP endpoint;
- общий кроссплатформенный runtime или новая сторонняя зависимость;
- создание соединения только после клика по строке сервера: Q2PRO-X не даёт
  add-on такого события;
- idle eviction, приоритеты комнат, LRU и автоматическое масштабирование;
- поддержка бесконечного числа комнат вне физических ресурсов машины.

## Внешнее поведение

Пользовательский сценарий не меняется:

1. Пользователь запускает q2prox-dz.
2. Открывается Q2PRO-X.
3. В `Join Online -> Custom` после `Refresh` видны все готовые комнаты.
4. Пользователь подключается, отключается и выбирает другую комнату без
   перезапуска игры.
5. Исчезновение одной комнаты удаляет только её строку. Q2PRO-X и остальные
   комнаты продолжают работать.

Отдельного выбора комнат, ручной настройки `cl_serverbrowser_custom_source` и
разделения на обычные/dedicated вкладки нет.

## Границы компонентов

### Launcher

Сохраняет текущие обязанности:

- загрузка и строгая проверка `/room/get`, `/lookup` и `/rtc/servers`;
- преобразование строк в нормализованные `JoinTarget`;
- проверка карты;
- опрос каждые 15 секунд;
- отправка желаемого множества комнат coordinator.

Launcher больше не назначает слот и не отбрасывает комнаты по квоте вида.
Состояние — словарь по точному `roomId`.

### Coordinator

Сохраняет владение Q2PRO-X, Custom source и всей сессией. Вместо дочернего
gateway на комнату он запускает один общий gateway, передаёт ему изменения
набора целей и публикует только endpoints со статусом `READY`.

Windows продолжает использовать один корневой Job Object. Linux продолжает
использовать process group и guardian. Падение общего gateway не оставляет
Q2PRO-X без владельца: coordinator один раз перезапускает gateway, очищает
Custom source и ждёт следующий полный heartbeat launcher не более 15 секунд.
Coordinator не хранит секретный снимок для replay. Повторный общий сбой
отключает Dos.Zone transport до следующего запуска q2prox-dz, но не закрывает
Q2PRO-X. Бесконечного цикла нет.

Coordinator запускает Q2PRO-X сразу после готовности локального HTTP source, а
не после готовности всех комнат. Пустой каталог поэтому означает пустую вкладку
`Custom`, а не задержку или отказ запуска игры. Endpoints добавляются в уже
работающий source по мере индивидуального `READY`.

### Shared gateway

Gateway скрывает за одной границей:

- WebRTC-NET initialization и signaling;
- сопоставление `targetId -> alias -> routePeerId (virtual) -> concretePeerId`;
- отдельный loopback UDP socket и захваченный Q2 source для каждой комнаты;
- OOB readiness и bounded room-close probes для каждой комнаты;
- маршрутизацию UDP -> virtual routePeerId и virtual routePeerId -> UDP socket;
- независимое удаление и повторное создание цели.

Это остаётся одним существующим gateway executable на платформу. Новый
отдельный сервис и общая библиотека не создаются.

Production gateway не зависит от `AliasQuery` и соответствующей mailbox/query
orchestration. `routePeerId` создаётся существующим virtual-peer helper, а
concrete разрешается предлагаемым read-only getter-ом.
`humblenet_p2p_disconnect(PeerId)`
немедленно удаляет все map entries этого connection и закрывает его; это
делает `DROP -> PUT` рабочим с первой попытки. Ни сервер, ни protocol v3 для
этого не меняются.

Тестовый controlled peer использует только предлагаемый узкий
`humblenet_p2p_is_alias_registered(alias)`: `true` допустим лишь после
`AliasLookup`-confirmation к текущему own PeerId, когда нет `pending` или
`replay`. HTTP `/lookup` controlled peer не вызывает.

### Custom source

Текущий HTTP source уже принимает список endpoints. Он публикует все готовые
`127.0.0.1:<port>` в стабильном порядке по `roomId`. Endpoint удерживается за
комнатой до её удаления или общего restart gateway, чтобы `status -> connect`
не сменил назначение порта.

## Модель данных

Логический `JoinTarget` сохраняет действующие поля:

```text
roomId       точный устойчивый идентификатор строки каталога
kind         q2 | q2ded
alias        точный HumbleNet alias
map          безопасное имя карты
game         3zb2
ordered      false
```

`iceServers` передаётся отдельным общим снимком и не дублируется в каждой
комнате внутри gateway. Внутри coordinator каждой активной версии `roomId`
назначается монотонный
64-битный `targetId`. Он используется только для корреляции управляющих событий
и не является сетевым адресом, индексом массива или секретом.

Состояние цели:

```text
ABSENT -> RESOLVING -> PROBING -> READY
                    \-> FAILED
READY -> CLOSED | FAILED | REMOVED
```

Стабильные переходы: allocation failure — `FAILED stage=resolve
code=peer_bind_failed`; unresolved до deadline — `FAILED stage=resolve
code=alias_timeout`; duplicate concrete — `FAILED stage=resolve
code=duplicate_remote_peer`; потеря уже установленного transport — `CLOSED
reason=transport_closed`.

Только `READY` попадает в Custom source. Повторная попытка создаёт новый
`targetId`; позднее событие старой версии игнорируется.

## Управляющий протокол

Фиксированные строки launcher -> coordinator вида `0 <roomId> <json>`
заменяются версионированными ограниченными строками:

```text
Q2DZ/ROOMS/2 ICE generation=<uint64> json=<compact-one-line-json>
Q2DZ/ROOMS/2 PUT room=<safe-room-id> json=<compact-one-line-json>
Q2DZ/ROOMS/2 DROP room=<safe-room-id>
```

Coordinator -> gateway использует ту же простую модель, но заменяет roomId на
несекретный instance id:

```text
Q2DZ/HUB/2 ICE generation=<uint64> json=<compact-one-line-json>
Q2DZ/HUB/2 PUT id=<uint64> json=<compact-one-line-json>
Q2DZ/HUB/2 DROP id=<uint64>
Q2DZ/HUB/2 STOP
```

Gateway отвечает:

```text
Q2DZ/HUB/2 READY id=<uint64> udp=127.0.0.1:<high-port> peer=<concrete-peer-id>
Q2DZ/HUB/2 CLOSED id=<uint64> reason=<stable-code>
Q2DZ/HUB/2 FAILED id=<uint64> stage=<stable-stage> code=<stable-code>
Q2DZ/HUB/2 HEARTBEAT state=ok targets=<count> ready=<count>
```

Существующие parsers уже умеют bounded line framing, а текущий launcher уже
формирует компактный JSON без переводов строк. Новый кодек не нужен. Parser
отклоняет NUL, CR/LF, превышение длины и неверную JSON-схему. `ICE` должен
предшествовать `PUT` нового peer. Максимальная
длина строки, очереди и JSON остаётся ограниченной; credential-shaped поля
никогда не возвращаются в события.

Версии 1 и 2 нельзя смешивать. Несовпадение coordinator/gateway оставляет
Custom source пустым и Q2PRO-X работающим, но transport не запускает.

`READY peer` — только concrete PeerId для duplicate/readiness evidence; для
маршрутизации он не используется.

## Маршрутизация датаграмм

Для каждой цели gateway хранит:

```text
targetId
alias
routePeerId       unique virtual PeerId used by transport maps
concretePeerId    0 until resolved, then 1..0x7fffffff
udpSocket
capturedClientAddress | none
probeState
counters
```

Правила:

1. При создании цели gateway получает `routePeerId` через
   `humblenet_p2p_virtual_peer_for_alias(alias)`; invalid virtual id означает
   `FAILED code=peer_bind_failed`.
2. Первый OOB probe отправляется через `routePeerId`, чем инициируется
   AliasLookup; gateway делает pump и затем читает proposed
   `humblenet_p2p_resolved_peer_for_virtual_peer(routePeerId)`.
3. `0` означает invalid/unresolved/closed и оставляет цель unresolved до
   deadline; после deadline цель получает `FAILED code=alias_timeout`.
4. Если ACK пришёл раньше concrete id, ACK удерживается до resolver result;
   concrete reserve и проверка уникальности выполняются атомарно. До этого
   `READY` запрещён.
5. Один concrete PeerId может принадлежать только одной активной цели; при
   duplicate новая цель получает `FAILED code=duplicate_remote_peer`, а
   survivor не меняется.
6. UDP-пакет читается из socket конкретной цели.
7. Первый loopback source захватывается для этой цели; второй отклоняется.
8. Пакет отправляется через `humblenet_p2p_sendto(..., routePeerId, ...)`.
9. `humblenet_p2p_recvfrom()` возвращает virtual route PeerId отправителя.
10. Reverse map `routePeerId -> targetId` находит ровно одну цель и отправляет
    пакет в её UDP socket на захваченный Q2 source.
11. Неизвестный или уже удалённый virtual PeerId отбрасывается и учитывается
    счётчиком.

`routePeerId` отвечает только за routing. `concretePeerId` не создаёт
соединение, а служит duplicate guard и значением `READY peer=`; duplicate
всегда fail-closed.

## ICE и signaling

В одном HumbleNet context ICE configuration является общей. Launcher строит
один проверенный список как устойчивое объединение всех записей
`/rtc/servers`, удаляя точные дубликаты, и отправляет его одной командой `ICE`
до набора `PUT` очередного полного heartbeat.

Gateway вызывает существующий `humblenet_set_iceservers()` на своём runtime
thread. WebRTC-NET применяет список к общему context: созданные соединения не
перезапускаются, а последующие получают свежие credentials. Отдельная политика
ротации не вводится. Native Linux вызывает API напрямую; WASM wrapper получает
минимальный экспорт `setIceServers(json)`, потому что текущий `connectTo()`
вызывает setter только при первом запуске. Изменившаяся конфигурация должна
сначала целиком пройти launcher validation.

Перед выпуском обязательна живая проверка, что один такой список одновременно
работает для `q2` и `q2ded`. Если это неверно, дизайн возвращается на пересмотр:
разделять runtime по RTC domain без доказанной необходимости нельзя.

## Динамический список и предел ресурсов

В продукте нет отдельных лимитов `q2`/`q2ded`, сортировки «первых N» или
молчаливого скрытия хвоста. Нормальный supported gate — 40 одновременных целей.

Удалённый каталог остаётся входом на границе доверия, поэтому реализация не
может бесконтрольно открывать sockets и peer connections. Применяется один
высокий аварийный предел **256 целей на снимок**. Это не квота интерфейса и не
резервирование по виду комнаты. При превышении launcher:

- не выбирает первые 256;
- не меняет уже опубликованный снимок;
- показывает явную ошибку `catalog_capacity_exceeded`;
- не запускает дополнительные процессы или соединения.

Число 256 значительно выше проверяемого сценария `20 + 20`, укладывается в
существующие ограничения ответа каталога и оставляет конечную верхнюю границу
для файловых дескрипторов и памяти. Менять его можно только после измерения, а
не через пользовательскую настройку.

Если конкретный socket или peer не может быть создан из-за исчерпания ресурсов,
эта цель получает `FAILED`, endpoint не публикуется, а остальные продолжают
работать. Ошибка видна в безопасной диагностике.

## Обновление набора комнат

После каждого полностью успешного опроса launcher отправляет `ICE` и полный
идемпотентный heartbeat всех желаемых целей. Это позволяет восстановиться после
смерти gateway, не сохраняя credentials в coordinator. Coordinator вычисляет
разницу по `roomId`:

- новая цель -> `PUT`;
- исчезнувшая два опроса подряд -> `DROP`;
- неизменная работающая цель -> payload очищается без restart;
- изменившиеся alias, map, game, ordered или общий transport contract ->
  `DROP` старой версии и `PUT` новой;
- изменение только отображаемых полей -> transport не трогается.

Невалидный общий ответ не считается пустым каталогом и не закрывает текущие
цели. Полная комната не добавляется впервые, но уже готовая цель не удаляется
из-за краткого изменения `players`.

## Readiness, закрытие и восстановление

Каждая цель имеет собственный `ManagedReadinessProbe`. `READY` выдаётся только
после виртуального allocation, первого OOB `ping/ack` через `routePeerId`,
resolver result и атомарного unique concrete reserve. ACK до concrete result
удерживается; resolver сам ничего не создаёт.

`humblenet_p2p_disconnect(routePeerId)` немедленно закрывает connection и
удаляет все его map entries;
поэтому `DROP -> PUT` не ждёт старого callback/query результата и работает с
первой попытки.

Закрытие DataChannel или три ограниченных неудачных OOB probe закрывают только
эту цель. Для `q2ded` сохраняется текущий ограниченный recovery budget; для
`q2` — действующая tombstone/generation логика. Recovery не перезапускает
runtime или соседние цели.

Общий signaling/runtime failure отличается от per-peer failure. Он приводит к
единственному полному restart gateway и смене всех локальных endpoints. Custom
source очищается. Новый процесс получает цели только из следующего полного
heartbeat launcher, не позднее чем через 15 секунд, и публикует их после
readiness. Второй общий failure оставляет Q2PRO-X работающим с пустым Dos.Zone
source и явной безопасной диагностикой.

## Windows

Один `q2dz-gateway.exe` создаёт:

- один WebView2 environment/controller;
- один скрытый, но `IsVisible=TRUE` WebView2 runtime с исправленным timer
  behavior;
- один экземпляр `webrtcnet.mjs`;
- один общий 5 ms pump, который за tick ограниченно опустошает очередь;
- карту target state вместо одиночных `remotePeer`, `buffer` и `probe`.

Не создаются отдельные страницы, controllers или user-data directories на
комнату. Все callbacks и доступ к WebView2 остаются на существующем STA thread.
Native host владеет UDP sockets и передаёт в страницу targetId вместе с
датаграммой; страница возвращает targetId для входящей датаграммы.

До перехода на долгоживущий runtime исправляется очистка закрытых DataChannel и
connection entries в закреплённом WebRTC-NET WASM backend. Поскольку текущие id
вычисляются как `Map.size + 1`, вместе с `Map.delete()` вводятся два монотонных
счётчика id, иначе удаление не последнего элемента переиспользует id живого
объекта. Исправление охватывает local и remote close, получает отдельный
churn-test, новый immutable dependency commit и новые хеши
`webrtcnet.mjs`/`webrtcnet.wasm` в `deps.lock.json`.

## Linux

Один native gateway вызывает `humblenet_init()` и `humblenet_p2p_init()` один
раз, хранит динамическую карту целей и опрашивает sockets через `poll()`. Один
вызов `humblenet_p2p_wait()` обслуживает все peer connections. Существующий
native gateway напрямую говорит HUB/2 через inherited stdin/stdout и применяет
существующие hardening, bounded framing, readiness и redaction helpers. Старый
managed adapter не остаётся в новом payload: его single-config-to-EOF и child
stdin `/dev/null` несовместимы с динамическим протоколом, а второй IPC только
усложнил бы путь.

Новый event framework и сторонний loop не нужны; достаточно `poll()`,
`unordered_map` и существующих protocol helpers.

## Безопасность

- все catalog/lookup/RTC поля проверяются до `PUT`;
- gateway принимает команды только по inherited stdin/pipe текущей сессии;
- ICE, alias и room JSON не передаются в argv, environment или файлы;
- core dumps отключены; динамический secret input идёт только через inherited
  anonymous pipes и очищается из временных буферов; файлов с credentials нет;
- endpoints только loopback high ports;
- UDP source isolation сохраняется отдельно для каждой цели;
- targetId и счётчики безопасны для журналирования, alias и credentials — нет;
- control/event framing, decoded payload, число целей и очереди ограничены;
- Job Object/process group/guardian очищают общий gateway и Q2PRO-X;
- heartbeat содержит только агрегированные числа.

## Сбои и наблюдаемость

Безопасные диагностические счётчики:

```text
targets, resolving, probing, ready, failed
udp_to_webrtc, webrtc_to_udp
oversized_udp_drops, oversized_transport_drops
unknown_peer_drops, second_udp_source_drops
max_udp_to_transport_datagram, max_transport_to_udp_datagram
pump_gap_max_ms
```

Это расширение существующей диагностики, а не новая телеметрическая подсистема.
Per-target журнал использует только `targetId`, стадию и стабильный код. Пакеты,
alias, ICE, SDP, candidates и room JSON не журналируются.

Сбой одной цели не влияет на остальные. Общий gateway crash имеет больший
blast radius, чем прежний процесс на комнату; его компенсируют coordinator
ownership, один bounded restart и перепубликация только готовых endpoints.

## Проверочные ворота

### До полной реализации

1. В одном exact WebRTC-NET context одновременно создать два разных virtual
   `routePeerId`, получить два разных resolved concrete PeerId и провести
   двусторонний OOB `ping/ack` по virtual маршрутам.
2. Закрыть один remote peer и доказать, что второй продолжает обмен.
3. Повторить 1--2 на native Linux и Windows WebView2/WASM.
4. Под одним объединённым ICE snapshot одновременно подключить одну `q2` и
   одну `q2ded` комнату.
5. Обновить ICE credentials без restart, сделать старые недействительными и
   подключить новую цель.
6. Провести не менее 500 create/connect/drop циклов в одном Windows runtime;
   размеры JS connection/channel maps, handles и память возвращаются к
   устойчивому уровню.
7. Проверить все поддерживаемые live aliases на duplicate concrete PeerId.
   При обнаружении дубля новая цель блокируется fail-closed, survivor остаётся
   READY; virtual PeerId не считается concrete duplicate guard, скрывать одну
   из комнат нельзя.

Любой FAIL блокирует удаление нынешнего per-room режима и возвращает дизайн на
пересмотр. Он не оправдывает простое увеличение числа WebView2 процессов.

### До выпуска

1. Общие protocol/catalog fixtures проходят на Linux и Windows.
2. Fake lifecycle проходит 0, 1, 5 и 40 целей; порядок больше не зависит от
   одной цифры или массива из пяти элементов.
3. При 40 целях создаётся один WebRTC-NET runtime; на Windows — один gateway
   process и ровно одно WebView2 process tree.
4. 40 endpoints уникальны, все представлены в source, отсутствует молчаливое
   усечение; 257-й target даёт явный отказ всего нового снимка.
5. Exact Q2PRO-X 40-endpoint source gate остаётся `PENDING` до записи evidence
   в durable verification docs. После `Refresh` он должен показать уникальные
   endpoints, послать `status` и сохранить один UDP source для
   `status -> connect`; это не gameplay evidence.
6. Две цели одновременно отвечают настоящим `status`; connect/disconnect и
   переключение не смешивают датаграммы.
7. Закрытие активной и неактивной комнаты удаляет только её endpoint.
8. Общий crash приводит ровно к одному restart; следующий heartbeat
   восстанавливает source без хранения credentials в coordinator.
9. Linux live показывает все текущие `q2`/`q2ded`, проходит status, map entry,
   действие, переключение и isolated close.
10. Windows live проходит тот же сценарий из заново распакованного portable ZIP.
11. 30-минутный прогон 40 controlled targets не показывает устойчивого роста
    handles/threads или памяти; add-on без Q2PRO-X остаётся ниже 1 GiB private
    memory/RSS и 75% одного логического CPU после прогрева.
12. Release chain собирает и проверяет exact Linux/Windows archives; protocol
    mismatch и смешанный payload fail closed.

## Внедрение и откат

1. Сначала выполняется двухцелевой transport spike без изменения поставки.
2. Общий gateway реализуется за существующими fake lifecycle tests.
3. Launcher/coordinator переводятся с slots на динамический `roomId -> target`.
4. После полного Linux/Windows PASS фиксированный runtime и константы `3+2`
   удаляются из release path; два пользовательских режима не поддерживаются.
5. Откат продукта — предыдущий rc13 archive. Дисковых данных и CFG migration
   нет.

## Отклонённые варианты

### Просто увеличить число gateway-процессов

Минимальный diff, но линейно умножает WebView2/native runtime, signaling,
память, polling и поверхность cleanup. Пригоден только как аварийная временная
сборка, не как долговременное решение.

### Несколько WebView2 controllers в одном процессе

Убирает часть native процессов, но сохраняет N страниц, WASM runtimes и
signaling contexts. Сложнее одного runtime и почти не устраняет главный расход.

### Создавать gateway только после выбора комнаты

Q2PRO-X не сообщает внешнему add-on о выборе строки до отправки UDP-пакета, а
для отправки ему уже нужен distinct endpoint. Нужен по меньшей мере socket на
каждую опубликованную комнату. Дополнительная lazy state machine сейчас не
окупается.

### Один публичный UDP endpoint для всех комнат

Quake II packets не несут room identity, достаточную для прозрачной обратной
маршрутизации. Это потребовало бы нового протокола или подмены `status` и
нарушило бы текущую прозрачность.

## Критерий принятия

Решение принято для Linux gateway-layer, когда двухцелевой spike подтвердил
общий ICE snapshot для `q2 + q2ded`, isolated drop и добавление новой цели без
restart. До полного релиза остаются Windows runtime gate, package gameplay
acceptance и resource gates. rc13 остаётся рабочим откатом.
