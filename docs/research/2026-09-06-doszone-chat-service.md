# Dos.Zone lobby chat: текущий read/write-контракт для Q2PRO-X

## Текущий статус проверки: Task 1 bounded / Task 2 unknown, 6 сентября

Bounded audit client seam завершён; его exact source/build boundary зафиксирован
в [исследовании клиента](../research/2026-09-06-doszone-chat-client.md#bounded-client-seam-audit-exact-package-and-documented-callbacks-2026-09-06).
Browser UI send/read подтверждены, но собственная программная/native отправка
остаётся **UNKNOWN**, а не невозможной. Исходники Q2PRO-X недоступны; новый
официальный API заранее не требуется как blanket-условие. SECURITY по-прежнему
запрещает private-write reverse engineering, replay browser heartbeat и
использование чужой identity.

## Дополнение после разрешённой проверки 6 сентября

Ниже сохранено исходное исследование без отправок. Позднее пользователь
разрешил свою комнату или `test[srv]`. В отдельной сессии HeadlessChrome 151
на Linux через штатное лобби выполнены: имя `Client1`, Join в `test[srv]`,
**Send message to chat**, одна нейтральная проверочная строка, **Send**.

- Интерфейс: MP `1.0.88 (q2ded)/v21`; bundle `index-DNVg2lYe.js`.
- Подтверждение: `2026-09-05T23:02:39.776952Z` (6 сентября локально).
- Независимый GET `/room/get` нашёл ровно одно совпадение собственного текста
  с автором `Client1` в точном room ID строки `test[srv]`, тип `q2ded`.
- От начала проверочного опроса до совпадения — **2,34 с**; это не замер
  полного времени доставки и не обещание задержки.
- **PASS только браузерный ввод → публичный поток чата**. Q2PRO-X не
  запускался. Вход в игровую карту не доказан: при загрузке iframe показывал
  `Server is not responding`; причину игровой ошибки этот опыт не определяет.
- Нажат штатный Exit, проверены возврат в лобби и отсутствие iframe,
  закрыт собственный браузер. Сервер не изменялся и не останавливался.
- Чужие cookies/секреты не копировались, прямой POST и воспроизведение
  внутреннего heartbeat не выполнялись.

Прежнее «мы умеем отправлять» верно для браузерного участника, не для
независимого нативного отправителя. Разрешение данного опыта уже получено.
Оно не создаёт интерфейс Q2PRO-X и не доказывает отправку без браузера.

## Supplemental: sibling-кандидаты direct browser iframe

Прочитаны исходники `~/Projects/Q2/q2server/archive/experiments/doszone_interaction_check/direct_chat_check.js` и `~/Projects/Q2/q2server/archive/experiments/doszone_interaction_check/chat_check.js`.
`direct_chat_check.js` создаёт страницу Chromium с `quake2.html`, кликает canvas,
нажимает `T`, вводит текст и `Enter`; `chat_check.js` проходит lobby UI, Join
и тот же ввод в iframe. Это прямой browser iframe path, не standalone native
Q2PRO-X sender и не доказательство native identity. Новых отправок по этим
кандидатам не выполнялось; source review только исключил их как native sender.

Официальные страницы [player API](https://js-dos.com/player-api.html) и
[networking](https://js-dos.com/networking.html) в текущей ревизии описывают
DosProps/NetInstance emulation и WebRTC, но не показывают lobby-chat
registration. Это ограниченное наблюдение, не доказательство отсутствия API
вообще.

## Исходное исследование

Замечание редакции 2 дизайна: ниже сохранены выводы первоначального аудита.
Требование обязательно получить новый официальный API было чрезмерным.
Программную отправку и способ собственной сессии ещё предстоит установить;
это не доказательство невозможности и не отмена ограничений на чужие сессии
и неподдерживаемые обходы. Проверенные browser send/read остаются фактами.

Дата проверки: `2026-09-06` (локальное время; публичный snapshot `/room/get`
имел timestamp `2026-09-05T21:52:00Z`).

Статус: **исследование; production-код не изменялся; комнаты не создавались и
не присоединялись; сообщения не отправлялись**.

Цель — проверить текущую публичную реализацию Dos.Zone и границу между
браузерным лобби-участником и нативным Q2PRO-X. Секреты, cookie, ICE/TURN,
SDP, peer IDs, точные room IDs и raw JSON в этот документ не заносились.

## Краткий вердикт

По текущим источникам нельзя исчерпывающе установить отсутствие native API.
Подтверждено только наличие browser send; поддерживаемая отправка от native
Q2PRO-X не подтверждена:

- **VERIFIED:** браузерный участник комнаты отправляет сообщения; текущий
  parent bundle также принимает `mp.room.sendToChat` от iframe и добавляет
  сообщение в очередь собственного/известного браузерного участника.
- **NOT VERIFIED / UNKNOWN:** отдельного поддерживаемого API, который позволил бы
  независимому Q2PRO-X стать участником комнаты и отправить сообщение от своей
  identity, в текущих публичных материалах не найдено. Это не доказывает, что
  такого закрытого или ещё не опубликованного API не существует. Нельзя
  заменить этот пробел подделкой browser secret, скрытой браузерной игрой или
  произвольным POST.
- **VERIFIED (исторический sibling):** старый `doszone_chat_bridge.cs` умеет
  только читать публичный snapshot и показывать сообщения через локальный CFG;
  упомянутый в руководстве `doszone_chat_sender.exe` исходником не поставлен.

Следовательно, доказан только изолированный read-only reader. Companion — это
  fallback proposal вне текущего in-game UX, а не готовая реализация active-room
  mapping в текущем hub и не выбранное решение. Для безопасного полного native
read/write HUD нужны подтверждённый client seam и собственная participant/write
capability; способ их получить ещё исследуется. URL, request body и способ
авторизации для C2 здесь намеренно не изобретаются.

## Первичные текущие источники

Проверены без authenticated mutation: lobby
`https://dos.zone/mp/?lobby=q2`; bundle
`https://dos.zone/mp/assets/index-DNVg2lYe.js` (fetched 2026-09-06,
SHA-256 `4c62141c878f4d8623a5232f032fe95787cb1721adea99423e4fc0c942bfe4fd`,
`Last-Modified: Fri, 04 Sep 2026 12:32:21 GMT`, `641421` bytes); public GET
`https://cloud.dos.zone/room/get` и `https://net.dos.zone/lookup`; iframe
`https://cdn.dos.zone/q2/client-v21/quake2.html` (SHA-256
`359e70520b6134ad40fcc77f075f338ca1431c4b3a528ed4117e15f8d985cc2e`,
`16268` bytes); local counterparts `quake2/emscripten/quake2.html` and
`quake2-dev-modes/emscripten/quake2.html`. GET output был ограничен schema и
количествами.

## Что реально поддерживает текущий browser path

### Чтение и короткоживущий список сообщений

В текущем bundle `lookup()` получает `/room/get`, группирует
`connected_peers` по точному `room`, затем для текущей комнаты объединяет:

- собственные локальные сообщения моложе `10 s`;
- `sentMessages` остальных peer-ов из той же комнаты;
- сортировку по `createdAt` и отображение максимум пяти записей.

Это не история и не гарантированная доставка. Snapshot содержал 35 room rows,
из них 3 `q2ded`; в них были `players`/`onlineUsers`, но не было
`connected_peers`. Это наблюдение данного snapshot, а не доказательство, что
`players` означает chat participants. Target также трактует `players` как
browser lobby count, а не полный native count: `docs/ARCHITECTURE.md:53-75`.

### Browser write path

Текущий bundle формирует локальную запись `{message, createdAt, fromId,
fromName, peerId}`. В heartbeat `updatePeer(room)` он отправляет только свежие
сообщения (до пяти, моложе десяти секунд) вместе с browser identity и
`mySecret` через `POST https://cloud.dos.zone/room/update`.

Это наблюдение сделано по статическому текущему bundle; сам POST не вызывался.
Вызов является внутренним browser participant heartbeat, а не опубликованным
native send endpoint. Client-side ограничения, видимые в bundle (`minInterval`
около 2 s, длина до 120, burst/cooldown и повтор сообщения), нельзя считать
server-side moderation или authorization contract.

### `sendToChat` — существующий, но ограниченный iframe seam

Parent message handler в текущем bundle принимает:

```text
mp.room.sendToChat { author, text }
```

и вызывает внутренний `sendMessage`. Его разрешение автора ограничено:

- отсутствующий/собственный author — собственный `myPeerId` и имя;
- numeric author — уже известный `clients[peerId]` текущей комнаты;
- string author — собственное имя или точное совпадение имени известного
  browser peer; неизвестное имя отклоняется.

Это control seam для embedded browser integrations, но не native identity,
room registration, permission или delivery ack. Native процесс не получает
`myPeerId`/`mySecret` и не должен выдавать себя за известного peer.

## iframe hooks и граница с игрой

Текущий Q2 iframe объявляет `mp.room.ready` с chat keys `T` и `P`; parent может
прислать `mp.room.mountChatHandlers`/`unmountChatHandlers`, `mp.room.player` и
`mp.room.netConfig`. `mp.room.player` передаёт имя и token игровой оболочке,
но это не регистрация Dos.Zone chat participant:
`q2prox-dz-dedicated-compat/docs/verification/2026-08-24-native-client-lobby-presence.md:68-73`.

После mount iframe перехватывает `KeyT` и `KeyP` и отправляет parent только
`mp.room.chat` или `mp.room.chatPrivate`. Текущий iframe **не отправляет**
`mp.room.sendToChat`; этот event принимается parent как внешний control message.
Локальные подтверждающие строки:

- `quake2/emscripten/quake2.html:217-235,274-305`;
- `quake2-dev-modes/emscripten/quake2.html:233-251,290-321`.

Remote iframe совпадает по этим hooks с локальным source shape: seam позволяет
браузерной оболочке открыть parent input, но не даёт Q2PRO-X HUD канала доставки.

## Room IDs, transport aliases и lifecycle

Нельзя подменять room ID игровым alias:

- для browser-hosted Q2 bundle использует transport `q2.<owner>`;
- для dedicated row при `owner == 0` current Q2 adapter использует exact
  `room.q2ded.*` value as transport address;
- `q2prox-dz-dedicated-compat` закрепляет ту же границу: `_room_alias()`
  возвращает полный `room` для `q2ded` и `q2.<owner>` для `q2`:
  `linux/tools/q2dz-launch:295-395`.

Канал игры и lobby chat — разные lifecycles. Current browser loop:

1. примерно раз в секунду обновляет own room/shadow room через heartbeat;
2. затем делает public lookup;
3. если собственная room отсутствует в ответе не менее 30 s, очищает room;
4. при смене/закрытии комнаты очищает local messages и identity.

Это видно в текущем bundle возле `updatePeer`, `lookup` и loop
(`setTimeout(..., 1e3)`). В target q2prox catalog refresh приблизительно 15 s
(`docs/ARCHITECTURE.md:53-57`), что само по себе не может быть heartbeat для
  десятисекундного chat window. Existing gateway lifecycle подтверждает transport
готовность/закрытие, но не lobby presence:
`windows/coordinator/MANAGED-GATEWAY-PROTOCOL.md:42-71`.

Target room config уже сохраняет exact `roomId` и alias только в bounded
in-memory config (`linux/tools/q2dz-launch:595-621`). В managed gateway
протоколе player/statistics tokens не являются частью config:
`windows/coordinator/MANAGED-GATEWAY-PROTOCOL.md:17-40`.

## Identity, moderation и неизвестные места

**Известно:** browser participant identity принадлежит parent state; `sendToChat`
может ссылаться только на себя или известный parent-side peer. В bundle также
есть local `chatMuted`/censorship UI state и client-side length/rate checks.

**Не доказано:**

- существует ли поддерживаемый native participant registration endpoint или
  protocol, и как он выдаёт short-lived room-scoped capability;
- как native identity связывается с exact room/generation/alias и dedicated
  session, какие server-side moderation/rate/expiry/disconnect правила действуют;
- является ли `q2ded.createSettings.onlineUsers` gameplay count, lobby presence
  count или только status metadata, и где должен появляться native participant;
- является ли `mp.room.sendToChat` документированным public API. В текущем
  bundle он существует, но этого недостаточно для external client contract;
- есть ли у выпускаемого Q2PRO-X безопасный native seam для HUD, input,
  асинхронного HTTPS и подтверждения отправки.

В частности, `mp.room.ready`, `mp.room.player`, transport `TRANSPORT_READY`,
Quake OOB `ping/ack` и наличие клиента в игровой таблице не являются
доказательством lobby chat authorization.

## Проверка sibling-кода и текущие ограничения target

В `q2prox-dz-dedicated-compat` нет реализации `sentMessages`, `mp.room.*`,
`/room/update` или sender API. Его security boundary запрещает portal DOM/cookie
automation, private write-API reverse engineering и изменение Q2PRO-X executable:
`linux/docs/SECURITY.md:10-22`.

Старый sibling example `q2prox-dz/docs/examples/doszone_chat_listener/` полезен
только для проверки того, что «native read/display невозможен» — слишком широкое
утверждение:

- `doszone_chat_bridge.cs:123-151` делает public GET `/room/get`;
- `doszone_chat_bridge.cs:280-359` читает `connected_peers[].sentMessages[]`;
- `doszone_chat_bridge.cs:370-383,442-460` пишет локальный `dz_messages.cfg`
  и триггерит F1.

Но sample не реализует lobby write. Руководство заявляет «отправку» и ссылается
на `doszone_chat_sender.exe`
(`q2prox-dz/docs/examples/doszone_chat_listener/Руководство по настройке.txt:13-24,40-48`),
однако sender source отсутствует. Это не production-safe client и не разрешение
на повторение browser heartbeat.

## Минимальный разумный write path (не реализован)

До изменения production-кода владелец Dos.Zone должен опубликовать C2 contract
с минимумом следующих свойств:

1. выдача собственного short-lived capability для **точного** room ID и
   проверенной room generation/alias, с отображаемым именем и понятной
   identity binding;
2. presence heartbeat/close и server expiry при падении клиента;
3. отправка сообщения с явным success/failure (и, если обещается, idempotency
   key), server-side moderation, rate/length limits и owner policy;
4. отдельное описание `q2` и `q2ded`, включая lifetime dedicated room;
5. запрет повторного использования browser `mySecret`, чужого peer ID и
   скрытого browser participant.

Конкретный URL, метод, JSON и credential name должны прийти от внешнего
владельца службы. После этого q2prox может добавить маленький adapter поверх
уже проверенного `(roomId, alias, target)` mapping, не меняя UDP/WebRTC gateway.
Если существующего C1 client seam нет, для HUD-варианта потребуется отдельный
upstream release; `q2prox-dz` не должен подменять установленный Q2PRO-X.

Практический admission gate:

- owner предоставляет versioned docs и sandbox/test room, где явно разрешён
  один контролируемый authorized send;
- контрактный тест подтверждает exact room binding, identity, ack, expiry и
  revoke без записи секретов/текстов в logs;
- только после этого рассматриваются adapter и native HUD.

До прохождения этого gate допустим лишь отдельный read-only reader или
изолированный companion proposal: polling public `/room/get`, exact room
matching, ограниченная память сообщений и явное «отправка недоступна». Это не
означает, что текущий hub уже выдаёт active-room mapping. Никаких replay или
probe `/room/update` в рамках этой работы не выполнялось.

## Итоговые границы решения

| Вопрос | Current result |
| --- | --- |
| Есть ли browser send? | **Да**, через browser participant heartbeat; внутренний, не native API. |
| Есть ли iframe control seam? | **Да**, `sendToChat` принимается parent; Q2 iframe сам его не emit-ит. |
| Может ли независимый Q2PRO-X отправлять от себя сейчас? | **UNKNOWN / NOT PROVEN**, supported identity/write contract не найден; это не доказательство невозможности. |
| Можно ли считать gateway transport готовым chat participant? | **Нет**; transport readiness и gameplay join не равны lobby presence. |
| Что доказано без внешнего владельца? | Изолированный read-only reader; companion остаётся fallback proposal. |
| Что нужно от внешнего владельца? | C2 native participant/write contract; для HUD также C1 client seam. |
