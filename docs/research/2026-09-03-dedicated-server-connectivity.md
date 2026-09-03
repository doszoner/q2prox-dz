# Dedicated-серверы через q2prox-dz: текущая граница и направление решения

Дата live-проверки: `2026-09-03T12:23:15Z`–`2026-09-03T12:24:10Z`.

Статус: исследование; код и production не изменялись.

Все пути ниже относительны общего рабочего каталога проектов Q2. Live-вывод
намеренно ограничен schema, alias/found и числом ICE servers. ICE/TURN URL,
username, credential, SDP, candidates и PeerId не сохранялись и не выводились.

Исследованные состояния:

- `q2prox-dz`: `947f3f8ff0db2c1e838b802340fff1cd2a780da9`;
- `quake2`: `2ee048f33041033df098226d9408e423cfc1768c`;
- `WebRTC-NET`: `87bd6dca4f469d2a7737daae2042eb3aedda6277`
  (рабочая ветка alias-liveness, не доказательство production deployment);
- `quake2-dedicated` — каталог документов/evidence без собственного `.git`.

## Краткий вердикт

**VERIFIED:** текущие dedicated исправны на уровне публичного preflight: три
строки присутствуют в catalog и все три точных alias дают `found=true`.

**VERIFIED:** q2prox-dz всё равно отбрасывает их до запуска gateway. Его product
layer принимает только browser-hosted schema `prefix=q2`, требует `roomReady`
и `rtcServer`, строит alias как `q2.<owner>`. Dedicated публикует
`prefix=q2ded`, `owner=0`, не публикует `roomReady`/`rtcServer`, а transport
alias совпадает с полным `room`. На одном и том же live snapshot текущий Linux
launcher получил `3` dedicated rows, `3` точных зарегистрированных alias, но
`strict_true=0`, `ready_q2ded=0`, `registered_q2ded=0`.

**VERIFIED:** transport не является корнем этой недоступности. Linux legacy и
Windows WebView transport принимают общий safe alias, а сохранённый реальный
прогон Linux gateway с dedicated прошёл Quake handshake, получил server status,
вошёл в игру и передавал datagrams в обе стороны.

**Рекомендация:** не добавлять новый transport и не выдавать `q2ded` за `q2`.
Сначала утвердить маленький versioned join descriptor, который явно сообщает
q2prox точный transport alias, однозначный RTC selector и join readiness.
Нормализовать оба catalog-профиля в уже существующий room config, затем снять
только лишнее `q2.*`-ограничение Linux managed parser. До утверждения владельцем
catalog/RTC этого контракта implementation verdict — **NO-GO**.

## VERIFIED

### 1. Live catalog и lookup сейчас здоровы, но q2prox их не выбирает

Безопасный срез:

```sh
curl -fsS --compressed --max-time 15 https://cloud.dos.zone/room/get |
  jq -c '{rooms:[.rooms[] | select(.createSettings.prefix=="q2ded") |
    {room,owner,
     roomReady:(if has("roomReady") then .roomReady else "<absent>" end),
     rtcServer:(if has("rtcServer") then .rtcServer else "<absent>" end),
     players,
     createSettings:{prefix:.createSettings.prefix,name:.createSettings.name,
                     map:.createSettings.map,maxPlayers:.createSettings.maxPlayers}}]}'
```

Результат: три строки `srv1[SPB]`, `test[srv]`, `srv2[SPB]`; у каждой
`prefix=q2ded`, `owner=0`, `roomReady=<absent>`, `rtcServer=<absent>`.
Полные route identities:

```text
room.q2ded.ambitious-cepheus.srv1.spb
room.q2ded.ambitious-cepheus.test.srv
room.q2ded.ambitious-cepheus.srv2.spb
```

Точные read-only lookup:

```sh
for alias in \
  room.q2ded.ambitious-cepheus.srv1.spb \
  room.q2ded.ambitious-cepheus.test.srv \
  room.q2ded.ambitious-cepheus.srv2.spb
do
  curl -fsS --max-time 15 "https://net.dos.zone/lookup/$alias" |
    jq -c '{found}'
done
```

Результат: `{"found":true}` три раза. Глобальный `/lookup` также содержал
ровно эти три dedicated alias. Это доказывает видимость route, но не ICE,
DataChannel или gameplay.

Прогон того же snapshot через фактический Linux launcher (extensionless script
загружен тем же `SourceFileLoader`, что его тест):

```text
{'q2ded_rows': 3, 'strict_true': 0, 'ready_q2ded': 0,
 'registered_q2ded': 0, 'exact_aliases_found': 3}
```

Следовательно, прежняя alias-liveness проблема на этом snapshot не является
причиной невидимости в q2prox: даже найденные alias отсекаются schema path.

Дополнительные live-факты:

- у q2ded row есть `updatedAt`, `ownerSeenAt`, `version`, `shadowRoom` и
  `shadowRoomEnterPolicy`; `createSettings` содержит `onlineUsers`, но не join
  descriptor;
- в snapshot не было ни одной live `prefix=q2` строки, поэтому текущую
  production-форму `q2` нельзя заново подтвердить этим запросом;
- `/rtc/servers` вернул семь domain rows по два ICE servers в каждой. В документ
  записаны только domains/count; содержимое ICE не выводилось.

### 2. Фактические различия catalog-контрактов

| Поле/смысл | `q2`, контракт потребителя q2prox | `q2ded`, live producer |
|---|---|---|
| discriminator | `createSettings.prefix == "q2"` | `"q2ded"` |
| room identity | safe `room.q2...` | `room.q2ded...`, одновременно текущий route alias |
| `owner` | seed для `q2.<owner>` | metadata `0` для всех трёх |
| readiness | обязательный boolean `roomReady` | поле отсутствует |
| RTC selection | обязательный hostname `rtcServer` | поле отсутствует |
| capacity | `players` и `createSettings.maxPlayers` | те же поля; дополнительно `createSettings.onlineUsers` |
| transport alias | вычисляется q2prox как `q2.<owner>` | `NET_SERVER_NAME`, совпадающий с `room` |

Положительный `q2` fixture прямо назван `valid-current-live-q2-shape` и содержит
`roomReady`, `rtcServer`, `owner`, `prefix=q2`, map и capacity:
`q2prox-dz/tests/contracts/doszone-api-cases.json:2`. Это текущий
repository-owned contract q2prox; из-за отсутствия live q2 row production drift
остаётся отдельно не проверен.

Dedicated producer пишет `status.json` с `name`, `map`, `maxPlayers`,
`onlineUsers`, `version=108` и константным `owner=0`:
`quake2/server/sv_main.c:520-529`, `quake2/server/sv_main.c:543-584`.
Файл обновляется из server frame раз в секунду:
`quake2/server/sv_main.c:3055-3060`.
Room agent связывает тот же `NET_SERVER_NAME` с `prefix=q2ded`:
`quake2/dedicated/entrypoint.sh:11-23`,
`quake2/dedicated/dedicated_room_agent.toml:3-11`. Entry point передаёт этот
alias q2ded как `net_server_name`: `quake2/dedicated/entrypoint.sh:37-45`.

Использовать dedicated `owner` как route seed нельзя: три live destination
схлопнутся в один синтетический `q2.0`.

### 3. Где именно q2prox отбрасывает dedicated

Нормативный product contract пока сознательно ограничен `prefix=q2`, alias
`q2.<owner>`, `game=3zb2`, `ordered=false`:
`q2prox-dz/docs/ARCHITECTURE.md:6-22`.

Linux product layer:

- `_strict_room` требует `prefix=q2`, `roomReady` и `rtcServer`:
  `q2prox-dz/linux/tools/q2dz-launch:286-320`;
- `ready_rooms` оставляет только strict + `roomReady=true`:
  `q2prox-dz/linux/tools/q2dz-launch:343-351`;
- `registered_rooms` проверяет только `q2.<owner>`:
  `q2prox-dz/linux/tools/q2dz-launch:361-365`;
- `make_room_config` снова вычисляет `q2.<owner>` и выбирает ровно одну RTC row
  по `selected.rtcServer`: `q2prox-dz/linux/tools/q2dz-launch:461-486`;
- refresh выполняет catalog -> lookup -> RTC/config:
  `q2prox-dz/linux/tools/q2dz-launch:665-684`.

Windows product layer повторяет тот же contract:

- row validation: `q2prox-dz/windows/product/catalog.ps1:268-289`;
- alias filter: `q2prox-dz/windows/product/catalog.ps1:321-332`;
- selected room и RTC match:
  `q2prox-dz/windows/product/catalog.ps1:449-486`;
- итоговый config: `q2prox-dz/windows/product/q2dz-launch.ps1:102-127`.

После product layer Linux managed parser имеет второй независимый blocker:
он принимает только alias, начинающийся с `q2.`:
`q2prox-dz/linux/src/json.cpp:312-338`.

Repository check на неизменённом коде:

```text
$ python3 -m unittest q2prox-dz/linux/tests/test_launcher.py
Ran 46 tests in 0.171s
OK
```

То есть поведение не случайная ошибка запроса: оно зафиксировано текущими
валидаторами и тестами.

### 4. Alias lookup и RTC selection — независимые входы

Peer-server хранит `alias -> PeerId` отдельно от session identity:
`WebRTC-NET/src/peer-server/catalog.cpp:28-49`,
`WebRTC-NET/src/peer-server/catalog.cpp:86-121`. Exact HTTP lookup возвращает
только `found`, а глобальный — список alias:
`WebRTC-NET/src/peer-server/peer-server.cpp:121-145`.
Внутренний exact AliasQuery использует префикс `=`, тогда как обычный запрос
является prefix search: `WebRTC-NET/src/peer-server/p2p_connection.cpp:326-372`.

Browser client также получает alias из URL, отдельно ждёт exact lookup и затем
подключается к `<alias>.humblenet`:
`quake2/emscripten/game.js:368-390`, `quake2/emscripten/game.js:445-471`,
`quake2/emscripten/game.js:564-568`. Его ICE config приходит отдельным
`mp.room.netConfig` до загрузки runtime:
`quake2/emscripten/quake2.html:274-305`.

Dedicated получает RTC независимо: entrypoint запускает room agent с `-rtc` и
ждёт появление `rtc.json` до запуска q2ded
(`quake2/dedicated/entrypoint.sh:37-38`,
`quake2/dedicated/entrypoint.sh:61-70`), а q2ded загружает изменения через
`humblenet_set_iceservers`:
`quake2/qcommon/humblenet_common.c:170-180`.

Из live q2ded row q2prox может узнать точный alias, но не sanctioned RTC row.
Выводить RTC из `.spb`, брать первый domain или считать, что обеим сторонам
обязательно нужен один domain, — не опубликованный контракт.

### 5. Transport уже общий для browser-hosted и dedicated server

Linux legacy gateway принимает любой safe alias длиной до 255, а не только
`q2.*`: `q2prox-dz/linux/transport/probe_options.cpp:72-78`. Он разрешает
переданный alias и пересылает цельные UDP datagrams reliable/channel `0`:
`q2prox-dz/linux/transport/q2_doszone_gateway.cpp:79-128`,
`q2prox-dz/linux/transport/q2_doszone_gateway.cpp:140-159`.

Windows transport start message также принимает общий safe alias и выполняет
exact query: `q2prox-dz/windows/gateway/assets/transport_protocol.mjs:60-89`,
`q2prox-dz/windows/gateway/assets/transport.html:48-70`,
`q2prox-dz/windows/gateway/assets/transport.html:120-166`.

q2ded использует тот же socket profile: receive/send channel `0`, reliable send,
а `<alias>.humblenet` превращается в virtual peer:
`quake2/humblenet/src/humblenet/include/humblenet_socket.h:50-89`,
`quake2/humblenet/src/humblenet/include/humblenet_socket.h:124-160`.
WebRTC-NET сохраняет datagram boundaries и разрешает channel `0`:
`WebRTC-NET/src/humblenet/src/humblenet_datagram.cpp:39-49`,
`WebRTC-NET/src/humblenet/src/humblenet_datagram.cpp:77-124`.

Сохранённый practical proof для dedicated:

- gateway поднял exact `room.q2ded...` alias, открыл channel и завершил с
  `udp_to_webrtc=1`, `webrtc_to_udp=1`:
  `noncommit/q2prox-dz-gates/20260831-dedicated-gateway/gateway-sanitized.log:1-12`;
- реальный client прошёл challenge/connection, protocol 35 и вошёл в игру:
  `noncommit/q2prox-dz-gates/20260831-dedicated-gateway/q2pro-stdout.log:76-87`;
- gateway за весь client run передал `2383` datagrams к WebRTC и `385` обратно:
  `noncommit/q2prox-dz-gates/20260831-dedicated-gateway/gateway-q2pro-sanitized.log:1-12`;
- server status пришёл от dedicated и содержал его map/game/capacity:
  `noncommit/q2prox-dz-gates/20260831-dedicated-gateway/status-response.txt:1-3`.

Это доказывает существующий UDP<->HumbleNet path до Quake handshake/map-entry,
но не является свежим 2026-09-03 release test и не доказывает Windows path.

### 6. Отдельный предел: hub имеет только два transport slots

Даже после schema fix текущий product не сможет одновременно показать все три
live dedicated servers: Linux константно ограничен двумя slots
(`q2prox-dz/linux/tools/q2dz-launch:30-37`), а архитектура явно обещает от нуля
до двух независимых gateway (`q2prox-dz/docs/ARCHITECTURE.md:42-56`).

Это не причина текущего нуля, но критерий полноты: patch, после которого видны
только первые два имени, не решает требование «все dedicated доступны».

## INFERENCE

1. Корень текущего нуля — несовместимость control-plane schema, потому что
   точные alias найдены, но те же строки дают `strict_true=0` до transport start.
2. `room` является transport alias для текущего dedicated producer: один
   `NET_SERVER_NAME` передаётся q2ded и room agent, live `room` совпадает с
   lookup alias. Но consumer не должен превращать это наблюдение в вечную
   эвристику без versioned contract.
3. `updatedAt` и присутствие row показывают свежий heartbeat, однако допустимый
   TTL и join semantics из исследованных публичных схем не следуют. Exact lookup
   должен остаться отдельным fail-closed gate.
4. Новый transport, разбор Quake protocol или изменение q2ded socket adapter не
   нужны для базового подключения: реальный Linux gateway path уже проходил.
5. Alias-liveness исправления WebRTC-NET улучшают отдельный operational contour,
   но не могут сделать q2ded видимым валидатору q2prox.

## UNKNOWN / решения владельца контракта

- Какой компонент является authoritative owner join descriptor: cloud catalog,
  `dedicated_room_agent` или иной registry.
- Должен ли dedicated передавать один `rtcServer`, несколько selectors либо
  клиент должен независимо выбирать RTC row. Локальные исходники room agent
  отсутствуют; по бинарнику/логам нельзя учреждать публичный контракт.
- Какая readiness semantics считается нормативной для q2ded: отдельный boolean,
  bounded heartbeat age, exact lookup или их комбинация.
- Production schema `q2` в момент проверки: live строк не было.
- Разрешено ли менять product limit `2` и достаточно ли ресурсов для трёх и
  более одновременных gateway. Причина лимита в исследованных документах не
  зафиксирована.
- Deployed provenance signaling server и наличие в production текущих
  WebRTC-NET alias-liveness commits.
- Свежий end-to-end результат 2026-09-03 для Linux managed и Windows managed.

## Минимальное долгосрочно корректное направление

Ниже зафиксирована исходная граница, найденная исследованием. Точная схема была
уточнена тремя pre-mortem проходами в
[`docs/design/2026-09-03-dedicated-server-support.md`](../design/2026-09-03-dedicated-server-support.md):
она использует bounded `rtcDomains`, явный `routeGeneration` и отдельное
решение по пяти слотам. Пример `rtcServer/ready` ниже не является принятым
контрактом реализации.

### 1. Сначала — один явный join contract

Минимальный descriptor, которого сейчас не хватает q2prox:

```json
{
  "join": {
    "version": 1,
    "alias": "room.q2ded.example.server",
    "rtcServer": "region.example.invalid",
    "ready": true
  }
}
```

Точные имена полей должен утвердить владелец DOS.Zone API. Не добавлять сюда
Quake protocol, PeerId, ICE credentials или новый transport. `ordered=false`,
signaling URL и datagram limit уже являются product invariants q2prox; делать их
произвольной catalog-конфигурацией без отдельной потребности не нужно.

Если RTC политика не сводится к одному существующему domain key, вместо
угадывания нужен столь же явный versioned RTC selector. Credential по-прежнему
получается только свежим запросом `/rtc/servers` после validation и exact lookup.

### 2. q2prox — нормализовать на существующей границе

- `q2` временно оставить на legacy derivation `q2.<owner>` для совместимости;
- `q2ded` принимать только с утверждённым descriptor; не использовать `owner=0`,
  suffix `.spb` или «первый RTC row»;
- после строгой проверки получить тот же внутренний room config, который уже
  потребляют coordinator/gateway;
- сохранить exact case-sensitive lookup непосредственно перед запуском;
- заменить только Linux managed `q2.*` check на уже применяемую transport-ом
  safe-token grammar; Windows transport менять не требуется.

Это один adapter на control-plane seam, а не fork transport-а.

### 3. Не замаскировать второй продуктовый вопрос

До обещания «все dedicated доступны» нужен отдельный выбор для limit `2`:
увеличить доказанно допустимое число slots либо сделать явный/on-demand выбор.
Просто сортировать общий catalog и молча показывать первые два — некорректно.
Не менять лимит в schema patch без resource/lifecycle gate.

## Проверки, без которых решение не готово

1. Общие Linux/Windows contract fixtures принимают `q2` legacy и `q2ded` v1,
   отклоняют missing/invalid alias, RTC selector и readiness до чтения ICE.
2. Три dedicated строки дают три уникальных destinations; `owner=0` нигде не
   участвует в routing.
3. Stale/missing exact alias fail closed, но один lookup miss не удаляет
   работающий slot вопреки существующей anti-flap политике.
4. Linux managed и Windows managed проходят OOB `ping -> ack` с exact dedicated
   alias и свежим RTC config без записи credentials.
5. Реальный Client1 входит на заявленную map, получает настоящий server status,
   выполняет одно действие; двусторонние counters растут.
6. Stop/restart dedicated удаляет/возвращает route без orphan gateway/process;
   catalog heartbeat и alias readiness проверяются раздельно.
7. Если scope обещает все текущие servers, все три доступны через product UI,
   а не только два первых slot-а.

До утверждения пункта 1 и RTC/readiness semantics: **NO-GO на реализацию**.
