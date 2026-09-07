# Q2PRO-X: native Dos.Zone chat/HUD feasibility

Дата исследования: 2026-09-06. Сначала изучены файлы; затем проведена локальная
проверка Q2PRO-X под Wine в отдельной копии и профиле. Итоги ниже заменяют
прежний общий вывод о невозможности расширения интерфейса. Производственная
установка, игровой сервер и транспорт не изменялись.

Продолжение: [опыт локального обмена через файлы](2026-09-06-q2prox-local-text-exchange.md).
В нём проверены открытие редактора, ввод и вызов экспорта; передача текста
внешнему помощнику пока не доказана из-за ошибки подготовки переменной.

## Практическая проверка установленного клиента

**Подтверждено:** файл `q2pro-x.menu2` позволяет добавить собственное меню,
его `field` меняет значение cvar, а штатный `draw` показывает это значение
непосредственно поверх карты. Это работает без изменения EXE. Кириллица
`Client1: Привет; ё` из фиксированной UTF-8 конфигурации отображается правильно.

Среда: Linux, Wine 11.15, отдельный Wine prefix и `homedir`, Q2PRO-X 1.5 с
SHA-256 `831f42dc96e3399a8a8737943d6fde752e11f5e5f48ffc5957537c4e66cbf71d`.
Использована локальная карта `base1`, звук отключён. К публичному игровому
серверу этот экземпляр не подключался; виртуальные машины не запускались.

| Проверка | Результат | Свидетельство |
| --- | --- | --- |
| Собственное меню из `q2pro-x.menu2`, поле `q2dz_probe_text` | PASS | `03-probe.png` |
| `draw q2dz_probe_text 30 160 white` поверх карты | PASS | `07-local-map.png` |
| Редактирование поля → выход из меню → новый текст на карте | PASS | `09-updated-world.png` |
| UTF-8 строка с кириллицей в штатном `draw` | PASS для этой строки | `14-unicode-render.png` |
| Вставка кириллицы в поле меню через буфер обмена | Не установлено: поле осталось пустым | `10-unicode-field.png`, `11-unicode-paste.png` |
| Внешнее обновление cvar без `exec` и имитации клавиш | Не доказано | Проверенного интерфейса пока нет |
| Отправка введённого текста внешнему помощнику | Не доказано | Поле меняет cvar, но это не механизм отправки |

Каталог опыта и снимков:
`/home/ssolovey/Projects/Q2/DosZone/noncommit/q2-chat-native-UiBrmm/`.
Конфигурация: `game/baseq2/configs/q2dz-probe.cfg`; меню:
`game/baseq2/q2pro-x/q2pro-x.menu2`; фиксированная строка:
`game/baseq2/q2dz-unicode-probe.cfg`; журнал:
`home/baseq2/logs/console.log`. В журнале сохранён вывод штатной `cmdlist`.
Он сам по себе не доказывает наличие или отсутствие внешнего интерфейса.

Воспроизведение в этой копии: выполнить `exec configs/q2dz-probe.cfg`, изменить
поле Text, выбрать Show text in game. Для кириллицы выполнить
`exec q2dz-unicode-probe.cfg`. Эти команды загружают **фиксированные тестовые
данные**, а не сообщения лобби. Исполняемый CFG с внешними сообщениями не
предлагается как продуктовый канал.

Погрешности стенда: `xdotool` потребовал явного X11 keyboard focus; при наборе
знаков заменял `/`, `:` и `;`. Поэтому точность пунктуации в опыте с полем не
подтверждена. Первоначально отсутствовала карта `q2dm1`; опыт продолжен на
имеющейся `base1`, подготовительная конфигурация исправлена. Это не отказы чата.

После опыта клиент завершён командой `quit`, отдельный Wine остановлен,
исходная раскладка возвращена. SHA-256 исходных EXE, `q2pro-x.cfg`,
`q2pro-x.local.cfg`, `q2pro-x.bindings.cfg` и `q2pro-x.menu2` совпали с замерами
до запуска. Пользовательская установка не изменена.

**Вывод для дизайна:** расширение интерфейса файлами клиента возможно.
Рисование строки и редактирование cvar больше не являются неизвестными.
Но ещё нужны безопасный двусторонний обмен с помощником, привязка к текущей
комнате и отправка от собственного участника Dos.Zone. Эти возможности нельзя
вывести из одного работающего `draw`. Изменять Quake-сервер ради этого запрещено.

## Первоначальный анализ файлов (историческая часть)

Обозначения: **[EXACT]** — установленный Q2PRO-X или его локальные документы;
**[UPSTREAM]** — публичный Q2PRO; **[PROJECT]** — текущий q2prox-dz;
**[PROPOSAL]** — возможное будущее изменение, не существующая возможность.

## Короткий вывод

**NO-GO для package-only native feed.** В текущей поставке нет доказанного
безопасного канала, через который внешний Dos.Zone watcher мог бы передать
произвольный текст в нативный HUD Q2PRO-X без key simulation и без записи/exec
конфигурации.

Что реально подтверждено:

- **[EXACT]** `Q2PRO-X.exe` — установленный Q2PRO-X 1.5: PE32+ x86-64,
  SHA-256 `831f42dc96e3399a8a8737943d6fde752e11f5e5f48ffc5957537c4e66cbf71d`.
  Manifest фиксирует runtime source commit/tag, но исходников Q2PRO-X в
  установочном дереве нет: `/home/ssolovey/Games/Quake2-PlayGround-2026/MANIFEST.txt:1-14`.
- **[UPSTREAM]** `draw` рисует текущее значение зарегистрированного cvar или
  macro; это не literal-text API. Открытый исходник хранит указатель `cvar` или
  `macro` и каждый кадр рисует его текущее значение:
  [Q2PRO `screen.c`, draw objects](https://github.com/q2pro/q2pro/blob/master/src/client/screen.c#L658-L770),
  [рендеринг объектов и chat HUD](https://github.com/q2pro/q2pro/blob/master/src/client/screen.c#L836-L950).
- **[UPSTREAM/EXACT]** `scr_chathud` — нативный HUD последних уже принятых
  Quake chat lines. `scr_chathud_time` только убирает/затухает такие строки;
  он не читает HTTP/файл и не создаёт входящие сообщения.
- **[EXACT]** Встроенные команды — `exec`, `wait`, `say`, `say_team`,
  `messagemode`, `messagemode2`. В exact binary/config/docs не найдено
  `con_messageMode`; это не следует подменять похожим названием.
- **[PROJECT]** Единственный существующий q2prox control seam — read-only
  server-browser source `GET /servers`; он не является command/IPC endpoint и
  не доказывает active gameplay room.

Изолированное чтение уже исследовано; внешний read-only companion — возможный
запасной вариант, но не выполненная интеграция с активной комнатой и не
выбранный пользователем интерфейс. Native HUD parity требует подтверждённого
клиентского интерфейса либо изменения
самого клиентского source/build владельцем Q2PRO-X; это отдельный контракт, а
не функция текущего portable package.

## Идентичность exact установки

Проверенный файл и рядом установленный game module:

```text
/home/ssolovey/Games/Quake2-PlayGround-2026/Q2PRO-X.exe
/home/ssolovey/Games/Quake2-PlayGround-2026/baseq2/q2pro-x/gamex86_64.dll
```

Manifest: accepted/runtime source commit `28b0f5338d138dbf3cb70c789fad85787a9af63a`,
tag `build/q2prox-1.5/20260825-101843-28b0f5338d13`, EXE hash above и Game DLL
SHA-256 `64015c99f3121fe61a62bee65ca772192af406c94dd7f4babb82a27b1238d57c`
([`MANIFEST.txt:1-14`](/home/ssolovey/Games/Quake2-PlayGround-2026/MANIFEST.txt:1)).
Рядом также лежит отдельный 32-bit `/home/ssolovey/Games/Quake2-PlayGround-2026/q2pro.exe`;
его нельзя молча считать тем же exact client.

Установочный пакет содержит DOCX и runtime binaries, но не `.c/.h/.cpp` source.
Поэтому публичный upstream `q2pro` — доказательство базовой семантики, а не
доказательство отсутствия частного Q2PRO-X изменения. Exact claims ниже
подкреплены локальным binary string/catalog, exact config и официальными
Q2PRO-X docs; private implementation details остаются unknown.

## Native draw/HUD

| Facility | Что доказано | Граница для Dos.Zone |
|---|---|---|
| `draw <name> <x> <y> [color]` | **[UPSTREAM]** `name` — cvar или macro, а не literal; `undraw` удаляет объект. См. `q2pro_manual.txt:1424-1447` и исходник выше. | Нужен уже существующий cvar/macro и способ безопасно изменить его значение. Внешнего такого command channel нет. |
| `scr_chathud` | **[EXACT]** config включает зарегистрированные `scr_chathud`, `_lines`, `_time`, `_x`, `_y`: `q2pro-x.cfg:678-682`. **[UPSTREAM]** chat ring принимает строки через `SCR_AddToChatHUD`, ограничивает каждую 150 bytes и рисует до 32 строк: `screen.c:872-950`. | Это presentation path уже принятого Quake chat, не listener/HTTP input. `scr_chathud_time` — срок видимости в секундах (`q2pro_manual.txt:490-512`). |
| `scr_draw2d`/screen loop | **[UPSTREAM]** `scr_draw2d` gates 2D; `SCR_DrawObjects` и `SCR_DrawChatHUD` вызываются внутри каждого screen frame: `screen.c:2105-2142`. | Перерисовка есть, но источника новых Dos.Zone строк нет. |
| Q2PRO-X custom HUD | **[EXACT]** binary catalog описывает `cl_opentdm_team_hud_show_teamchat` как “last already-visible say_team”, normal `PRINT_CHAT`, без command injection/suppression (разбор строки из exact EXE). | Это подтверждает display-only boundary exact fork, но binary strings не заменяют source proof и не добавляют lobby feed. |

Публичный manual формулирует draw contract прямо: `draw` добавляет “console
variable or macro identified by name” (`/home/ssolovey/Games/Quake2-PlayGround-2026/q2pro_manual.txt:1424-1441`).
Примеры — только `draw cl_fps` и `draw com_time`
(`q2pro_manual.txt:1443-1448`). В exact Q2PRO-X binary найдены `draw` и `undraw`,
но не найдено документированного literal-text варианта. Поэтому literal draw
для exact fork — **не доказан и не должен приниматься как контракт**.

## Input, command buffer, timers и reload

### Message input

- `say <text>` и `say_team <text>` отправляют Quake text chat; `messagemode` и
  `messagemode2` открывают однострочный prompt. Exact command catalog в
  `Q2PRO-X.exe` содержит их описания; обычные бинды подтверждены
  [`q2pro-x.bindings.cfg:11-26`](/home/ssolovey/Games/Quake2-PlayGround-2026/baseq2/q2pro-x/q2pro-x.bindings.cfg:11).
- `con_auto_chat` лишь решает, как консольные строки без `/` или `\` вести во
  время игры: regular command/forward as chat/forward as team chat
  ([`q2pro_manual.txt:401-406`](/home/ssolovey/Games/Quake2-PlayGround-2026/q2pro_manual.txt:401)).
  Это не external input API.
- Поиск exact config, docs и binary string catalog не нашёл `con_messageMode`
  (встречаются именно `messagemode` и `messagemode2`). Нельзя строить план на
  несуществующем/неподтверждённом имени.

### Command sequencing and events

- `exec` читает `.cfg` и выполняет каждую строку как console command; `wait`
  задерживает остаток уже попавшего в command buffer на client frames. Это exact
  binary catalog и публичная semantics, но не timer callback: пользовательская
  запись `exec`/alias/key injection всё равно остаётся запрещённым seam.
- Public Q2PRO `cl_beginmapcmd`, `cl_changemapcmd`, `cl_disconnectcmd` и
  `trigger` — события map/disconnect/non-chat server message, не периодический
  watcher. `trigger` отдельно исключает chat messages
  ([`q2pro_manual.txt:1369-1385`](/home/ssolovey/Games/Quake2-PlayGround-2026/q2pro_manual.txt:1369)).
- `cl_timer` — macro “time since level load”, не внешний scheduler
  ([`q2pro_manual.txt:1184-1208`](/home/ssolovey/Games/Quake2-PlayGround-2026/q2pro_manual.txt:1184)).
- `vid_restart`, `fs_restart`, `r_reload` перезапускают video/filesystem/media;
  это не reload feed и не re-read arbitrary external text
  ([`q2pro_manual.txt:1500-1514`](/home/ssolovey/Games/Quake2-PlayGround-2026/q2pro_manual.txt:1500)).
- Q2PRO-X live item timers (`it_start`, `it_cancel`, `it_info`) — предметные
  respawn HUD timers, а не универсальная строка/таймерная очередь; это прямо
  ограничено официальным [Live Item Timers Guide](https://q2pro-x.com/q2pro-x/docs-preview/ru/Q2PRO-X_1.5_Live_Item_Timers_Guide_RU.html).

## HTTP, IPC and native lifecycle

### Existing project seam

Exact profile has:

```text
set cl_serverbrowser_custom_source "http://127.0.0.1:34079/servers"
```

([`q2pro-x.cfg:365-378`](/home/ssolovey/Games/Quake2-PlayGround-2026/baseq2/q2pro-x/q2pro-x.cfg:365)).
The project architecture documents that Q2PRO-X reads server name/map/player
count/ping through `Custom` and ordinary `Refresh`, while the coordinator does
not send menu-control commands ([`ARCHITECTURE.md:42-63`](/home/ssolovey/Projects/Q2/DosZone/q2prox-dz-dedicated-compat/docs/ARCHITECTURE.md:42)).
This is a catalog/status/connect path only; existing verification explicitly
limits its claim to browser/status/connect, not live two-room gameplay
([`2026-08-29-q2pro-x-custom-source.md:18-36`](/home/ssolovey/Projects/Q2/DosZone/q2prox-dz-dedicated-compat/docs/verification/2026-08-29-q2pro-x-custom-source.md:18)).

### Client-owned HTTP/IPC features

The exact binary documents or contains strings for several **outbound/client-
owned** flows:

- HTTP downloads and the Custom server-browser source;
- Q2PRO-X voice control plane `POST /voice/prepare`, followed by LiveKit;
- a `CreatePipe` helper protocol for the private `q2pro-x-bridge32.exe` game32
  helper (the binary labels it “game32 bridge”, HELLO/READY and version
  mismatch). This is an internal 32-bit game-module bridge, not a supported
  external text/HUD API.

The official [Live Item Timers Guide](https://q2pro-x.com/q2pro-x/docs-preview/ru/Q2PRO-X_1.5_Live_Item_Timers_Guide_RU.html)
also documents an internal asynchronous authorization request: connection to
the game continues without waiting for the HTTP response, and automatic timer
activation remains off until a valid response arrives. This proves that the
client has an internal async HTTP implementation; it does **not** expose that
implementation as a caller API, callback, or inbound feed endpoint. The exact
binary's `cl_http_*` catalog likewise describes downloads/diagnostics, not
foreign-process command delivery.

Official [Voice Chat Guide](https://q2pro-x.com/q2pro-x/docs-preview/ru/Q2PRO-X_1.5_Voice_Chat_Guide_RU.html)
states that voice is a separate channel, does not replace ordinary Quake text
chat (lines 59-66), and uses `vc_server` to prepare a LiveKit room token
(lines 277-287 and 390-395). It must not be repurposed as a Dos.Zone lobby
message bridge: it has different room identity, auth and media semantics.

No documented generic inbound localhost HTTP endpoint, named-pipe command
protocol, stdin command channel, or library/plugin ABI for `set`/`draw`/chat was
found in the installed docs or exact command catalog. The absence is a package
research result, not a claim about undisclosed private source. Do not add a
second IPC channel to q2prox-dz on this evidence.

## UTF-8 and rendering

Exact profile currently has `cl_console_utf8_ru "-1"`
([`q2pro-x.cfg:84-86`](/home/ssolovey/Games/Quake2-PlayGround-2026/baseq2/q2pro-x/q2pro-x.cfg:84)).
Official [Controls, Mouse & Zoom Guide](https://q2pro-x.com/q2pro-x/docs-preview/ru/Q2PRO-X_1.5_Controls_Mouse_Zoom_Guide_RU.html)
describes this as tri-state **display localization**: ASCII stays on stock
`conchars`, while selected UTF-8 Cyrillic/CP1251 codepoints use the auxiliary
Q2PRO-X console atlas. Official [Network Play Guide](https://q2pro-x.com/q2pro-x/docs-preview/ru/Q2PRO-X_1.5_Network_Play_Guide_RU.html)
explicitly says it affects display/UI, not the Russian input switch.

Exact binary's HUD catalog further separates trusted local chrome from
untrusted/raw paths: server-supplied layout, scoreboard/player names,
configstring-derived chat, last-message chat, voted strings and centerprints
remain on the legacy byte-exact path. Therefore “UTF-8 support exists” does not
prove that arbitrary Dos.Zone text will render safely in native chat HUD. A
future client extension needs explicit UTF-8 validation, length limits,
control-character filtering and a deliberate renderer path.

## Existing prototype: not an acceptable implementation

The old example is useful only as a negative control. It:

- disables TLS certificate validation (`ServerCertificateValidationCallback`
  always returns true) at
  [`doszone_chat_bridge.cs:95-110`](/home/ssolovey/Projects/Q2/DosZone/q2prox-dz/docs/examples/doszone_chat_listener/doszone_chat_bridge.cs:95);
- posts `WM_KEYDOWN/WM_KEYUP` and falls back to `SendInput` to simulate F1 at
  [`doszone_chat_bridge.cs:184-230`](/home/ssolovey/Projects/Q2/DosZone/q2prox-dz/docs/examples/doszone_chat_listener/doszone_chat_bridge.cs:184);
- writes generated `set dz_msgN "..."` lines to multiple `dz_messages.cfg`
  files at [`doszone_chat_bridge.cs:425-464`](/home/ssolovey/Projects/Q2/DosZone/q2prox-dz/docs/examples/doszone_chat_listener/doszone_chat_bridge.cs:425);
- instructs the user to bind `F1` to `exec dz_messages.cfg`
  ([Руководство по настройке.txt](</home/ssolovey/Projects/Q2/DosZone/q2prox-dz/docs/examples/doszone_chat_listener/Руководство по настройке.txt:15>)).

Escaping quotes/semicolons in [`doszone_chat_bridge.cs:362-367`](/home/ssolovey/Projects/Q2/DosZone/q2prox-dz/docs/examples/doszone_chat_listener/doszone_chat_bridge.cs:362)
does not turn an untrusted external string into a supported config transport.
This prototype must not be revived or used as evidence of native capability.

## Strongest feasible design boundary

### Запасной вариант: отдельный просмотр сообщений, не выбранный дизайн

Keep the existing coordinator/source contract: q2prox-dz may publish the
read-only `Custom` server catalog and a companion may read the already available
Dos.Zone room snapshot, correlated by exact room ID. It may show lobby chat in
an external panel/overlay. It must not claim that `connected_peers` proves a
native player or active Quake room; current project docs say those counts are
lobby-only ([`ARCHITECTURE.md:70-75`](/home/ssolovey/Projects/Q2/DosZone/q2prox-dz-dedicated-compat/docs/ARCHITECTURE.md:70)).

No Q2PRO-X `set`, `draw`, `exec`, key, UDP print injection, transparent proxy,
or voice-room write is needed for this read-only package.

### Future native parity: source-level extension (PROPOSAL, not committed)

If native HUD parity is a product requirement, the smallest credible extension
is maintainer-owned Q2PRO-X source/build work:

1. keep coordinator mapping read-only: endpoint -> exact room ID -> target
   generation;
2. use client-owned bounded HTTP reads and supported authorization for sending,
   not a new inbound command/IPC service;
3. deliver validated lines into the existing native chat/HUD lifecycle (the
   public upstream seam is `SCR_AddToChatHUD`, then `scr_chathud` handles draw
   and expiry), or add a separate explicitly trusted overlay;
4. reuse normal client connect/disconnect and input lifecycle, with no simulated
   keys, arbitrary cfg writes, packet injection or transport inspection.

This cannot be implemented or promised against the installed package alone.
The Q2PRO-X owner must provide source/build authority and approve the endpoint,
auth, moderation, UTF-8 and target-lifecycle contract. It also must not silently
replace users' existing EXE.

## Unknowns and stop gates

- `MANIFEST.txt` records a source commit, but the exact Q2PRO-X source and build
  instructions are not in the installed tree. Upstream Q2PRO source is public;
  exact fork changes are not reproducible from that alone.
- No runtime test was performed by instruction, so no claim is made about
  hidden/private commands, arbitrary cvar creation, real-world Unicode glyph
  coverage, or the exact behavior of a future binary.
- Current `Custom` source evidence proves catalog/status/connect behavior only;
  it does not expose active gameplay events or an exact native-room binding.
- Q2PRO-X voice/LiveKit and the internal game32 helper pipe are not approved
  Dos.Zone text/HUD interfaces.
- Until a supported client-owned feed exists, native Dos.Zone lobby chat inside
  `scr_chathud` is **not available package-only**.

## Primary sources and local evidence

- **Exact install:** [`MANIFEST.txt:1-14`](/home/ssolovey/Games/Quake2-PlayGround-2026/MANIFEST.txt:1),
  [`q2pro-x.cfg:84-86,365-378,678-712`](/home/ssolovey/Games/Quake2-PlayGround-2026/baseq2/q2pro-x/q2pro-x.cfg:84),
  [`q2pro-x.bindings.cfg:11-26`](/home/ssolovey/Games/Quake2-PlayGround-2026/baseq2/q2pro-x/q2pro-x.bindings.cfg:11).
- **Installed upstream manual:** [`q2pro_manual.txt:401-415,490-512,1184-1208,1369-1385,1424-1448,1500-1514`](/home/ssolovey/Games/Quake2-PlayGround-2026/q2pro_manual.txt:401).
  It is shipped beside the fork and should be treated as upstream-baseline
  documentation, not private Q2PRO-X source proof.
- **Public upstream source/repository:** [q2pro/q2pro](https://github.com/q2pro/q2pro),
  especially [client `screen.c` draw/chat/frame code](https://github.com/q2pro/q2pro/blob/master/src/client/screen.c#L658-L950).
- **Official Q2PRO-X:** [site](https://q2pro-x.com/), [interface/console docs](https://q2pro-x.com/q2pro-x/docs-preview/ru/Q2PRO-X_1.5_Interface_Menu_Console_Guide_RU.html),
  [UTF-8/input docs](https://q2pro-x.com/q2pro-x/docs-preview/ru/Q2PRO-X_1.5_Controls_Mouse_Zoom_Guide_RU.html),
  [network docs](https://q2pro-x.com/q2pro-x/docs-preview/ru/Q2PRO-X_1.5_Network_Play_Guide_RU.html),
  [voice docs](https://q2pro-x.com/q2pro-x/docs-preview/ru/Q2PRO-X_1.5_Voice_Chat_Guide_RU.html),
  [live-item-timer docs](https://q2pro-x.com/q2pro-x/docs-preview/ru/Q2PRO-X_1.5_Live_Item_Timers_Guide_RU.html).
- **Current q2prox boundary:** [`ARCHITECTURE.md:42-85`](/home/ssolovey/Projects/Q2/DosZone/q2prox-dz-dedicated-compat/docs/ARCHITECTURE.md:42),
  [`2026-08-29-q2pro-x-custom-source.md:5-36`](/home/ssolovey/Projects/Q2/DosZone/q2prox-dz-dedicated-compat/docs/verification/2026-08-29-q2pro-x-custom-source.md:5).
- **Disqualified prototype:** [`doszone_chat_bridge.cs:95-110,184-230,362-367,425-464`](/home/ssolovey/Projects/Q2/DosZone/q2prox-dz/docs/examples/doszone_chat_listener/doszone_chat_bridge.cs:95),
  [Руководство по настройке.txt](</home/ssolovey/Projects/Q2/DosZone/q2prox-dz/docs/examples/doszone_chat_listener/Руководство по настройке.txt:15>).

## Addendum: standard game-chat route (2026-09-06)

**Уточнение после исследования:** пользователь запретил менять Quake-сервер.
Серверные варианты ниже изучены только по исходникам и исключены из решения.
Указанное ниже отсутствие внешних отправок относится к работе исследователя;
основной агент отдельно проверил разрешённую браузерную отправку на `test[srv]`.
Это не доказывает возможность двустороннего чата внутри неизменённого клиента.

### Новые ограничения пользователя

- Подтверждено: у установленного Q2PRO-X нет внешнего HUD/input API, а
  доступных исходников Q2PRO-X нет.
- Допустимы только собственные room/test-server проверки, если они нужны; этот
  addendum не разворачивает сервис и не изменяет соседние checkout-ы.
- Не допускаются protocol interception, packet injection, внешняя отправка и
  выдача native-клиента за browser participant.

### Что уже есть в стандартном Quake II

В доступном `quake2/` путь обычного игрового чата уже замкнут:

```text
T -> messagemode -> Enter -> say/say_team
  -> server ClientCommand -> game Cmd_Say_f
  -> PRINT_CHAT в svc_print -> client CL_ParsePrint
  -> SCR_AddChatMessage -> scr_chathud
```

`default.cfg` связывает `T` с `messagemode`; `Key_Message` превращает Enter в
`say` или `say_team` (`quake2/configs/default.cfg:91`,
`quake2/client/keys.c:760-793`). Клиент регистрирует эти команды как
пересылаемые серверу (`quake2/client/cl_main.c:3784-3799`); сервер передаёт
строковую команду game DLL (`quake2/server/sv_user.c:2463-2471`), где
`ClientCommand` вызывает `Cmd_Say_f` (`quake2/game/g_cmds.c:787-870,908-930`).
Game handler добавляет игровое имя, применяет team/flood/length rules и
доставляет сообщение клиентам через `PRINT_CHAT`.

На сервере `cprintf` сериализует `svc_print`/level/text, а
`SV_BroadcastPrintf` делает тот же broadcast (`quake2/server/sv_game.c:121-178,868-873`,
`quake2/server/sv_send.c:84-153`). Клиент принимает `svc_print`, для
`PRINT_CHAT` вызывает `SCR_AddChatMessage`, а включённый `scr_chathud` рисует
накопленные строки (`quake2/client/cl_parse.c:1188-1219,1353-1357`,
`quake2/client/cl_scrn.c:474-632,1753-1756`). `q2ded` собирается из того же
game/server source и `USE_HUMBLENET`; транспорт не нуждается в изменении для
этого игрового маршрута (`quake2/CMakeLists.txt:150-175,301-320`).

Это доказывает штатный **game chat**, но не Dos.Zone lobby chat. В текущем
parent/iframe контракте Q2 HTML перехватывает `T`/`P` и отправляет только
`mp.room.chat`/`mp.room.chatPrivate`; он не принимает входящий текст и не
эмитит `mp.room.sendToChat` (`quake2/emscripten/quake2.html:217-235,274-305`).
Parent `sendToChat` добавляет сообщение в browser heartbeat только для своего
или уже известного browser peer; native Q2PRO-X не получает `myPeerId`/`mySecret`
(`browser-chat/TECHNICAL-REPORT.md:100-135`,
`q2prox-dz-dedicated-compat/docs/research/2026-09-06-doszone-chat-service.md:72-121`).

### Feasible only conditionally

1. **Наиболее native для приёма в Q2PRO-X HUD:** dedicated/game-side bridge
   читает публичный lobby feed и подаёт валидированные строки в существующий
   server broadcast (`gi.bprintf(PRINT_CHAT, ...)` или адресный `gi.cprintf`),
   после чего Q2PRO-X получает обычный `svc_print` и рисует его своим HUD. Это
   потребует изменения и сборки q2ded/game-side кода, bounded Dos.Zone
   read/room mapping и явного имени bridge. Такой bridge не существует в
   `q2prox-dz-dedicated-compat` и не даёт права native-клиенту писать в lobby.

2. **Browser relay:** parent может передать текст в специально изменённый
   browser Quake client, который отправит обычный `say`; q2ded затем разошлёт
   его по `PRINT_CHAT`. Это потребует изменений parent bundle и browser
   Quake HTML/client build. Relay должен иметь отдельную явную bridge identity;
   он не может писать от имени native Q2PRO-X или текущего browser participant.

3. **Обратное направление native `say` -> Dos.Zone lobby** всё ещё NO-GO:
   game handler может увидеть `say`, но текущий `/room/update` — внутренний
   browser heartbeat с `mySecret`, а `mp.room.sendToChat` не является native
   participant API. Нужен официальный room-scoped native identity/write
   contract и только после него — dedicated/client integration. Одного
   q2ded, gateway или стандартного `PRINT_CHAT` недостаточно.

Итог: стандартный game-side маршрут — самый короткий путь к нативному HUD
приёма, но только как условное изменение browser/dedicated components. В
текущей package-only поставке без Q2PRO-X source/API Dos.Zone lobby chat в
`scr_chathud` не доступен; companion остаётся единственным read-only fallback.

## Native message-mode UX: reuse boundary (2026-09-06)

**Конкретный вывод.** Уточнённый UX (горячая клавиша → штатная строка →
Enter отправляет и возвращает игру, Esc отменяет) совпадает с формой Quake
`key_message`. Штатный режим является выбранной основой доработки;
установленная игра подтверждает наличие обычного `messagemode`, но
установленные menu/config не доказывают отправку в Dos.Zone lobby и не дают
внешнему процессу callback с текстом.

### Что реально настраивается в exact install

Exact install root для следующих menu/config ссылок:
`/home/ssolovey/Games/Quake2-PlayGround-2026/baseq2/q2pro-x/`.

- `q2pro-x.menu2:1178-1185` открывает `g_controls`, а
  `q2pro-x.menu2:1203-1231` — `g_keys`; строка
  `q2pro-x.menu2:1229` — штатный bind `messagemode` с описанием «open the
  chat prompt». Это точное место выбора горячей клавиши в меню.
- Текущий результат сохранён в
  `q2pro-x.bindings.cfg:1-3,13,26`: файл generated, `-` вызывает
  `messagemode`, `=` — `messagemode2`. В `menu2` есть строка public
  `messagemode`, но отдельной строки для `messagemode2` не обнаружено.
- Установленный
  `docs/ru/Q2PRO-X_1.5_Controls_Mouse_Zoom_Guide_RU.docx`, §§17–18,
  подтверждает safe Russian input для консоли/чата и таблицу
  `g_keys → Chat → messagemode`; это документация поведения/настройки, не
  исходник Q2PRO-X.
- `q2pro-x.menu2:6113-6117` упоминает Enter/Escape для
  `demo_item_timers_strip_edit`; это отдельный редактор расположения, не
  доказательство событий message-mode. `q2pro_manual.txt:298-305` описывает
  focus только для звука (`s_auto_focus`), не для chat prompt.

### Primary Quake reference for the event lifecycle

- В открытом sibling Quake II `quake2/client/console.c:241-260` и
  `:267-286` команды `messagemode`/`messagemode2` переводят клиент в
  `cls.key_dest = key_message`; регистрация видна в `:405-409`.
- `quake2/client/keys.c:760-793` обрабатывает Enter и keypad Enter:
  формирует `say` либо `say_team`, очищает буфер и ставит `key_game`.
  `quake2/client/keys.c:827-834` обрабатывает Esc: очищает буфер и отменяет
  режим без отправки.
- `quake2/client/keys.c:1222-1254` показывает вход key-down/key-up и
  autorepeat; `:1297-1323` маршрутизирует Esc только на key-down,
  `:1352-1374` для key-up оставляет лишь release-события `+...`, а
  `:1411-1423` передаёт обычный key-down в `Key_Message`. Значит, штатный
  `messagemode` не требует синтетического key-up от внешнего helper.
- В том же открытом reference focus loss (`quake2/win32/in_win.c:1346-1355`)
  меняет app-active и деактивирует мышь; смены `key_dest` или отмены chat там
  нет. Это отсутствие обработчика в sibling source, не контракт exact EXE.

### Boundary: proven / unknown / proposed

- **PROVEN:** menu/config позволяют выбрать hotkey для существующего
  `messagemode`; standard Quake lifecycle имеет local text editing, Enter/Esc
  и возврат в игру. Это отправляет обычный game `say`/`say_team`.
- **UNKNOWN for closed-source Q2PRO-X:** совпадают ли внутренние
  key-down/repeat/key-up/focus-loss детали один-в-один, и существует ли
  callback/канал, который принимает набранный текст в local helper. Меню,
  shipped docs и exact binary catalog этого не подтверждают.
- **PROPOSED, not existing:** native command/transport, который оставляет
  тот же `key_message` UX, но передаёт подтверждённую строку в Dos.Zone
  room-scoped writer. `menu/bind` сами не определяют новую команду отправки;
  доступный способ подключения обработчика в Q2PRO-X ещё нужно установить.
  Серверный обход не рассматривается.

Следовательно, reuse штатного режима — обоснованный UX-контур для будущего
source-level extension; OS hooks, `SendInput` и внешняя подмена клавиатуры не
нужны и не принимаются. В package-only exact install доказан только game
chat, не native lobby send.

## Bounded client-seam audit: exact package and documented callbacks (2026-09-06)

Это отдельный свежий read-only проход по установленному дереву и shipped
первичным материалам. `Q2PRO-X.exe`, DLL и bridge не запускались; GUI, сеть,
установка и соседние checkout-ы не изменялись. Целью было проверить именно
доступный клиентский seam, а не повторить старый общий no-go.

### Exact identity and source/build boundary

Повторная проверка exact installation дала:

| Artifact | Evidence | Result |
| --- | --- | --- |
| `/home/ssolovey/Games/Quake2-PlayGround-2026/Q2PRO-X.exe` | `exiftool`, `file`, `sha256sum`; [`MANIFEST.txt:1-14`](/home/ssolovey/Games/Quake2-PlayGround-2026/MANIFEST.txt:1) | Product/File version `1.5 by ly, 2026-08-25`; PE32+ x86-64 GUI; SHA-256 `831f42dc96e3399a8a8737943d6fde752e11f5e5f48ffc5957537c4e66cbf71d` |
| `/home/ssolovey/Games/Quake2-PlayGround-2026/baseq2/q2pro-x/gamex86_64.dll` | same commands; [`MANIFEST.txt:8-10`](/home/ssolovey/Games/Quake2-PlayGround-2026/MANIFEST.txt:8) | Product/File version `1.5 by ly, 2026-08-25`; PE32+ x86-64 DLL; SHA-256 `64015c99f3121fe61a62bee65ca772192af406c94dd7f4babb82a27b1238d57c` |
| `baseq2/q2pro-x/utils/q2pro-x-bridge32.exe` | same commands; [`MANIFEST.txt:10`](/home/ssolovey/Games/Quake2-PlayGround-2026/MANIFEST.txt:10) | PE32 i386 helper; SHA-256 `1d408338c54d8a33d6268e3a0dc331d8e8c3ffa31bb097fc31f11d176b4f0f22` |
| Build identity | [`MANIFEST.txt:5-7`](/home/ssolovey/Games/Quake2-PlayGround-2026/MANIFEST.txt:5) | runtime source commit `28b0f5338d138dbf3cb70c789fad85787a9af63a`, tag `build/q2prox-1.5/20260825-101843-28b0f5338d13` |

The package contains runtime binaries, configs, menus, `.modhelp` and DOCX;
it does not contain the Q2PRO-X client source or a build recipe. The comment in
[`q2pro-x.menu2:1-24`](/home/ssolovey/Games/Quake2-PlayGround-2026/baseq2/q2pro-x/q2pro-x.menu2:1)
mentions `src/client/ui/script.c`, but that path is not distributed source.
PE inspection is also a useful hard boundary: `Q2PRO-X.exe` and
`q2pro-x-bridge32.exe` have no export table; `gamex86_64.dll` exports only
`GetGameAPI` and `GetGameAPIEx` (`objdump -p`). Those exports are the game-DLL
loader ABI, not a client plugin/IPC ABI.

Свежий bounded GET официальной страницы [q2pro-x.com](https://q2pro-x.com/) от 2026-09-06 вернул HTTP 200 (`Date: Sun, 06 Sep 2026 12:35:49 GMT`). В HTML у кнопки «Исходники» обнаружены атрибуты `data-qpx-release-title="Исходники Q2PRO-X"` и `data-qpx-release-message="Исходники Q2PRO-X на данный момент ещё не опубликованы, но такая возможность вскоре появится. Проверьте страницу позже."`. Это подтверждает только текущий статус публичной публикации исходников на официальном сайте, а не универсальную невозможность расширения или отсутствие будущего owner-approved source/build/extension ABI.

The internal-symbol observations below are reproducible without starting the
client with `strings -a -n 4 /home/ssolovey/Games/Quake2-PlayGround-2026/Q2PRO-X.exe`;
they are binary evidence only, not a promise that the strings are callable
symbols.

### What the installed DSL actually exposes

The exact guided menu is a declarative DSL. Its own header says that rows must
be defined in the file or be code-built menus, and explicitly says not to add
`inject ... endinject` blocks ([`q2pro-x.menu2:1-24`](/home/ssolovey/Games/Quake2-PlayGround-2026/baseq2/q2pro-x/q2pro-x.menu2:1)). A focused read of the shipped rows found these facilities:

| Candidate | Narrow falsifiable hypothesis | Exact evidence | Typed non-command data? | Client-owned callback? |
| --- | --- | --- | --- | --- |
| Menu `field` | A field-change hook would let a helper observe a typed value without a command. | `q2pro-x.menu2:944-945,1032,5879`; the `field` target is a cvar. | **Yes, as local cvar text.** | **No.** Commit is cvar/menu state; no external observer or event payload. |
| `messagemode` row/bind | Enter could expose the prompt buffer to a room writer while preserving native UX. | `q2pro-x.menu2:1229`; [`q2pro-x.bindings.cfg:13,26`](/home/ssolovey/Games/Quake2-PlayGround-2026/baseq2/q2pro-x/q2pro-x.bindings.cfg:13). | **Yes, in the stock prompt.** | **No documented callback.** It remains normal Quake game chat. |
| `q2pro.modhelp` `prompt` | An inline prompt could target a non-command sink instead of dispatching console text. | [`q2pro.modhelp:1-20`](/home/ssolovey/Games/Quake2-PlayGround-2026/baseq2/q2pro-x/q2pro.modhelp:1) defines inline input, then `exec`/`prompt`; rows `:61,67,73` use it. | **Yes, but as an argument to a configured console command.** | **No.** The described result is command execution on click/Enter. |
| `q2prox_scripts` | A new bind row could call an external callback or carry a message payload. | `q2pro-x.menu2:6393-6411` contains fixed binds (`followip`, `aadump`, `aashot`, `ready`, `vote`) and `macrolist`. | **No new payload channel.** | **No.** It binds existing commands/macros. |
| `draw` / `scr_chathud` | A literal-text draw/append object could render a Dos.Zone line without server chat. | Exact cvars [`q2pro-x.cfg:676-685`](/home/ssolovey/Games/Quake2-PlayGround-2026/baseq2/q2pro-x/q2pro-x.cfg:676); upstream manual `draw` contract remains cvar/macro-based. | **No literal external text contract.** | **No.** Render-only presentation of existing client/server state. |

The strongest falsifiable interpretation is therefore: menu text editing is
real, but every documented destination is a cvar or a console command. None of
these rows accepts a foreign callback function, a room-scoped writer, or a
non-command message object.

### Input, editor, focus and render hooks

The shipped Controls guide documents `g_keys → Chat → messagemode` as the
message prompt. The exact binary's string catalog additionally contains
`ENTER: send   ESC: cancel`, `IF_KeyEvent`, `IF_CharEvent`, `IF_Draw`,
`edit_text`, and `key-down`; the same catalog contains focused-field behavior
(`keys typed into the overlay reach the field`, arrows consumed by a focused
field) and several HUD/editor contracts (`Enter/Escape` leaves an edit mode).
These are useful clues about internal ownership, not an external API:

- key-down and character events are **internal** symbols/strings; no exported
  function, script callback, or documented event subscription was found;
- repeat/key-up semantics for the exact closed-source message prompt are not
  specified by the shipped docs; `bind` and `+command` behavior cannot deliver
  the typed buffer to a helper;
- The shipped docs and binary strings describe Enter/Esc and focused-field
  behavior for built-in prompt/editor UI, but exact runtime dispatch,
  key-up/repeat and focus-loss semantics were not tested in this pass; no
  focus-loss callback or editor-buffer callback was found in inspected
  materials;
- `IF_Draw`/HUD editor strings show an internal render loop, while `draw` and
  `scr_chathud` remain presentation paths. They do not create a new incoming
  Dos.Zone line source.

This yields **partial evidence and a source-patch hypothesis**: native UX may
be reusable at those internal seams. It does not establish that reuse or make
a package-only client patch buildable.

### Connection epoch and endpoint

There is useful internal observability, but not an attachable lifecycle seam.
The exact string catalog contains `connection generation`, `new connection
generation %u`, and `identity: gen=%u endpoint=%s protocol=%d mod=%s state=%d`.
The installed menu exposes `cl_ping_adapt_debug`/`ping_adapt_info` and Network
God diagnostics (`q2pro-x.menu2:1887-1890,1893-1900`); the shipped Network Play
guide says the debug log includes connection-generation resets. These are
client-owned diagnostics and state, not a callback delivering `(endpoint,
epoch)` to another process.

`strings -a -n 4 Q2PRO-X.exe` contains the text “Automatically join the voice
room ... Triggers on `CL_Connected` ... Leaves room automatically on
disconnect.” This is binary text describing private voice behavior; it does
not independently prove a callable client event or supported external
callback. The Custom server-browser source
(`cl_serverbrowser_custom_source`, [`q2pro-x.cfg:369-386`](/home/ssolovey/Games/Quake2-PlayGround-2026/baseq2/q2pro-x/q2pro-x.cfg:369)) is pre-connect catalog input only; it cannot satisfy the required direct connected-client endpoint+epoch event.

**Narrow hypothesis:** if the owner supplies Q2PRO-X source, the existing
`CL_Connected`/disconnect state plus connection generation could be the
correct place to bind endpoint identity and invalidate stale async results.
The installed package does not expose the ownership, thread, cancellation or
callback signature needed to implement or test that hypothesis.

### Positive async HTTP workflow, and its ceiling

The strongest actual documented client-owned async workflow is translation,
not chat transport. The shipped Network Play guide §11 says:

- `/en <text>` asynchronously sends the current prompt to the active provider
  and replaces the prompt; the player still presses Enter;
- `cl_translate_custom_url` is one of the provider URL settings and
  `cl_translate_timeout_ms` is supported;
- only one translation request is in flight; timeout/failure leaves the prompt
  unchanged.

The exact profile has `cl_translate_custom_url ""`, provider `q2prox`, URL
`https://q2pro-x.com/api/translate`, and timeout `3000` ms
([`q2pro-x.cfg:441-449`](/home/ssolovey/Games/Quake2-PlayGround-2026/baseq2/q2pro-x/q2pro-x.cfg:441)). This records the configured provider URL and timeout, while the shipped guide documents the prompt-replacement workflow. Request execution, worker/thread ownership, cancellation and safety were not runtime-tested in this pass. It does **not** establish a generic HTTP client API: the inspected materials do not show a caller-supplied callback, and `/en` is documented as a translation workflow that still requires manual Enter. Whether a compatible custom endpoint could support a room-scoped feed or write is an untested hypothesis, not demonstrated evidence.

Live item timers provide a second internal example: the exact menu says online
authorization is asynchronous and never delays connection
(`q2pro-x.menu2:244-252`), and the guide says the decision is repeated for
each new connection. That workflow is authorization-specific; no callback API
for scripts or helpers is documented in the inspected materials.

| HTTP candidate | Narrow falsifiable hypothesis | Ownership/timeout/build boundary |
| --- | --- | --- |
| `/en` translation | Pointing `cl_translate_custom_url` at a compatible room endpoint could turn the documented prompt workflow into a feed/write seam. **Current status:** inspected docs define translation/prompt replacement; custom-endpoint runtime and callback scope were not tested. | Configured provider URL and `3000` ms cvar; one request in flight is documented; worker/thread, cancellation and callback ABI are not documented; no source/build package. |
| Live-item authorization | Its per-connection async result could carry arbitrary chat data. **Current status:** inspected materials describe signed timer authorization that gates automatic timers; reuse for chat was not tested. | Connection proceeds while authorization is pending per docs; no external sink, thread or callback ABI is exposed in inspected materials. |
| Custom server source | The existing HTTP source could deliver room messages to the HUD. **Current status:** inspected docs/menu scope it to server-browser catalog rows/status/connect; room-message semantics and runtime callback were not tested. | Endpoint is configured in a cvar; no gameplay endpoint/epoch or HUD callback is documented; thread/timeout contract was not found. |

### Required material, verdict and residual risk

| Requirement | Package result |
| --- | --- |
| Exact version/hash | **DONE** — exact Q2PRO-X 1.5 metadata, manifest identity and hashes above. |
| Typed editor with Enter/Esc/focus | **DONE_WITH_CONCERNS** — shipped docs/strings describe built-in behavior; exact runtime key-up/repeat/focus-loss and callback contract were not tested and are not public in inspected materials. |
| Native HUD/render append | **DONE_WITH_CONCERNS** — draw/chat HUD and `IF_Draw` strings are present; no external line-ingest API was found (strings are not callable-ABI proof). |
| Direct connection endpoint+epoch | **NEEDS_CONTEXT** — exact strings/diagnostic docs mention `CL_Connected`/generation; no supported callback/ownership/thread contract was found, and runtime was not tested. |
| Client-owned async HTTP | **DONE_WITH_CONCERNS** — shipped docs describe translation/timer async workflows; runtime/thread/cancellation and generic API remain untested, with no caller callback in inspected materials. |
| Build/test exact client patch | **NEEDS_CONTEXT** — Q2PRO-X source/build/test material is missing. |

Overall result: **DONE_WITH_CONCERNS** for this bounded audit. A source-level
route that reuses native prompt/editor/render and async HTTP machinery remains
plausible, but is a hypothesis rather than demonstrated integration; package-
only implementation remains **NEEDS_CONTEXT**.
This is not a universal impossibility claim. The next required artifact is
the Q2PRO-X owner-approved source/build or a first-party extension ABI that
specifies: key-down/repeat/up and Enter/Esc/focus ownership; direct
connect/disconnect endpoint plus monotonic epoch; render append entry point;
async HTTP callback context, cancellation and timeout; and a build/test
procedure. No server, WebRTC/HumbleNet, service, synthetic-key, CFG-data,
DLL-injection or replacement-fork work is authorized by this finding.

Changed files in this pass: only this subsection in
`docs/research/2026-09-06-doszone-chat-client.md`.

### Closed `/en` room-delivery hypothesis

The shipped `/en` path is narrowly `input → HTTP translation provider →
current-prompt replacement → explicit user Enter through the ordinary
game-chat recipient`. Its documented workflow provides no room/connection-
epoch binding, HUD/render callback, or separate Dos.Zone recipient. Manual
Enter is not itself a defect—the intended UX is explicit—but this path does
not establish client-owned delivery.

The inspected contract also does not define an opaque typed-data result
boundary: it documents translated text placed into the prompt, not a typed
callback or a command/data separation guarantee. Runtime execution or safety
of arbitrary provider output was not tested and is not claimed here. Close
this as a translation-prompt hypothesis, not a native Dos.Zone feed/write
seam.
