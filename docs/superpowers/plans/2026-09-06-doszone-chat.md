# Dos.Zone Chat Rev2 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Довести чат текущей комнаты Dos.Zone до проверенного двустороннего
нативного UX внутри Q2PRO-X на Windows и Linux/Wine, сохранив обычный игровой
чат, пользовательские бинды и игровой транспорт.

**Architecture:** Q2PRO-X владеет connection, HUD, штатным editor и input
lifecycle. Coordinator даёт только read-only mapping готового target к точной
комнате; Dos.Zone read/send выполняется найденным разрешённым client-owned
способом. Quake-сервер, `quake2`, WebRTC/HumbleNet и фоновые службы не меняются.

**Tech Stack:** Существующие C++ coordinator, shell/PowerShell tests, exact
Q2PRO-X source/API только при получении материала; новые процессы, IPC,
библиотеки и HTTP-сервисы заранее не назначаются.

---

## База и правила

Редакция 2 принята после последовательных technical-premortem и ponytail-review
Astra high с правками. Это план исследования и доведения, не утверждение
готового клиентского патча: конкретные точки расширения и программная отправка
проверяются в задачах 1–2. [Протокол](../../reviews/2026-09-06-doszone-chat.md).

Целевой документ — [design rev2](../../design/2026-09-06-doszone-chat.md),
входы — [client research](../../research/2026-09-06-doszone-chat-client.md) и
[service research](../../research/2026-09-06-doszone-chat-service.md). У exact
Q2PRO-X сейчас не найден внешний HUD/input API и нет доступного source; это
исследовательский факт, не доказательство невозможности private extension.
`messagemode_doszone` — proposal новой команды, не existing command.

Порядок каждого известного изменения: test/black-box scenario → минимальный
code → повторный test. Для неизвестного client source сначала записать exact
path, symbol/callback, build command и test command; до этого не писать полный
код, fake API или выдуманный adapter. Не применять `SendInput`, `xdotool`,
global hooks, synthetic keys, executable CFG, packet/protocol interception,
browser identity relay или companion вместо native UX.

Старый `docs/superpowers/plans/2026-09-06-doszone-chat-GOAL.txt` относится к
редакции 1 Goal; его формальные C1/C2/C3 и STOP не являются этим планом.
Новый GOAL не создавать. Commit, push, GUI/network/build выполняются не в этом
документарном turn.

## Файлы

| Область | Exact files | Результат |
|---|---|---|
| Client | путь/source/API устанавливается в Task 1 | native hotkey/editor/HUD/connect callbacks, без выдуманного пути |
| Linux | `linux/src/session_main.cpp`, `linux/tests/fake_q2.py`, `linux/tests/fake_lifecycle.sh` | `/q2dz/rooms` из готовых targets, `/servers` без регрессии |
| Windows | `windows/coordinator/src/windows/main.cpp`, `windows/coordinator/tests/windows/fake_q2.cpp`, `windows/coordinator/tools/run-lifecycle-matrix.ps1` | тот же bounded contract/lifecycle |
| Docs/evidence | `docs/adr/0004-q2pro-x-custom-source-room-hub.md`, `README.md`, `INSTALLATION.md`, `CHANGELOG.md`, `docs/RELEASE.md`, `docs/verification/doszone-chat-admission.md` | ADR вместе с implementation; один итоговый report |

## Task 1. Реальные native input/callback/connection/render точки

**Files:** читать exact install, client research, Q2PRO-X material и
`docs/ARCHITECTURE.md`; evidence писать в `docs/verification/doszone-chat-admission.md`.

- [ ] Зафиксировать exact hash/version. Найти фактические key-down/repeat/key-up,
  editor buffer, Enter/Esc, focus/menu, connect/disconnect, HUD/render и
  client-owned HTTP callback; upstream Quake references не выдавать за fork.
- [ ] Для каждой найденной точки записать exact file/symbol, ownership,
  thread/timeout, build/test command. Если material отсутствует, назвать
  конкретный missing source/API и остановить только client patch, не подменяя
  его server/transport обходом.
- [ ] Доказать active room прямым client connection event с переданными
  endpoint/epoch; `status`, `READY`, UDP heuristic, map name и `Refresh` не
  считаются epoch. Совместное соответствие mapping проверяется в Task 5.
  Будущий key: `(hub session, target, exact room, client connection epoch)`.

**Expected:** обычный menu/bind и `messagemode` остаются proven baseline;
`messagemode_doszone` помечен proposal. `draw`, cvar и browser send не закрывают
native input/render seam.

## Task 2. Own-session program send + independent read

**Files:** service research, `linux/docs/SECURITY.md`, тот же evidence report;
новый service/API не добавлять.

- [ ] Исследовать существующий разрешённый способ получить собственного
  `Client1`, срок, exact room/generation, name, revoke/expiry и send result;
  новый official API не требовать заранее только из-за неудобства.
- [ ] В разрешённой своей комнате выполнить ограниченный black-box: один
  program send нейтральной UTF-8 строки от своего участника и независимый read;
  сверить room/author/result, затем expiry/revoke и generation change, отдельно
  `q2`/`q2ded` если среда разрешает. Не использовать чужие cookies/secrets,
  identity или browser heartbeat.
- [ ] Записать status/code/latency/counters без credentials и переписки. Не
  подбирать private write endpoint и не обходить SECURITY.

**Expected:** воспроизводимый own send + independent read с правильными
author/room и явным отказом старого участника после expiry/revoke/generation.
Если упёрлись, report называет конкретную отсутствующую операцию/material/owner;
URL/body не сочинять и unknown send не ретраить.

## Task 3. Offline-first native editor/HUD

**Files:** только exact Q2PRO-X files из Task 1; client test path/command
фиксируются в report до patch.

- [ ] RED без сети: штатное назначение hotkey, held-key repeat, activation
  symbol suppression, button-up, text editing/paste, Enter/Esc, menu/console,
  Alt-Tab и focus loss. Неназначенное действие ничего не перехватывает.
- [ ] GREEN минимально добавить адресат `DosZone` к найденному message-mode;
  `messagemode_doszone` открывает пустой native editor, символ hotkey не
  вставляется, `T/Y`, `messagemode`/`messagemode2` и existing binds сохраняются.
- [ ] RED повторно: Enter с допустимым непустым text делает одну typed-data
  send, закрывает editor и возвращает game input в том же event cycle до
  network response; пустой/пробельный Enter только закрывает. Esc удаляет
  draft, закрывает chat и не открывает menu. Held movement/attack получает
  штатный release; autorepeat Enter/Esc не попадает в игру.
- [ ] RED/Green UTF-8: до 120 Unicode symbols/480 bytes принимается;
  invalid/control/oversize остаётся в editor с явной ошибкой, без silent trim.
  Проверить paste, кириллицу, wrap и длинный author штатным renderer.
- [ ] Проверить memory-only queue (32 messages, dedup до 256); переписка и
  credentials не попадают в сохраняемые cvars, `console.log`, history, CFG,
  demo или diagnostics, а пользовательские settings сохраняются штатно. Нет
  frame block на network timeout.

**Expected:** каждый scenario даёт наблюдаемый HUD/input result; mock editor,
`draw` и synthetic key не являются PASS. При отсутствии source/test — только
report с нужным exact material, без fake implementation.

## Task 4. Mapping Linux/Windows и ADR 0004

**Files:** `linux/src/session_main.cpp`, `linux/tests/fake_q2.py`,
`linux/tests/fake_lifecycle.sh`; `windows/coordinator/include/q2dz/child_protocol.hpp`,
`windows/coordinator/src/windows/main.cpp`, `windows/coordinator/tests/core_tests.cpp`,
`windows/coordinator/tests/windows/fake_q2.cpp`,
`windows/coordinator/tools/run-lifecycle-matrix.ps1`; ADR обновляется вместе
с этой реализацией, не отдельным внешним task.

- [ ] RED в обоих fake clients: старый `GET /servers` с прежним Host без порта
  сохраняет bytes; Linux RED зафиксирован, отдельный Windows RED не проводился;
  green compatibility проверена. Сохранить baseline.
- [x] GREEN добавить только read-only `GET /q2dz/rooms` на existing listener:
  `version`, `session`, `targets[]` со string `target`, exact loopback
  `endpoint`, case-sensitive `room`, `kind`; только ready targets, один
  immutable snapshot с `/servers`, max 256 targets/256 KiB. Нет POST, credentials,
  CORS, command execution или нового service.
- [x] RED/Green: empty, READY A/B, DROP A, restart/session change, malformed /
  oversize request, wrong Host, any Origin, no READY, no secrets; `/servers`
  byte-compatible, cleanup/Custom не блокируются, q2/q2ded generations не
  смешиваются. ADR фиксирует тот же узкий contract и отсутствие send rights.

Task 4 result: Linux/Windows mapping, core test и process cleanup PASS; детали
зафиксированы в [admission report](../../verification/doszone-chat-admission.md).

Linux baseline после каждого известного изменения (в этом turn не запускать):

```sh
build_dir="$(mktemp -d)"
cmake -S linux -B "$build_dir" -DQ2DZ_REQUIRE_WEBRTC_GATEWAY=OFF
cmake --build "$build_dir" -j2
ctest --test-dir "$build_dir" --output-on-failure
```

Windows coordinator проверять existing `windows/coordinator/tools/build-release.ps1`,
затем `windows/coordinator/tools/run-lifecycle-matrix.ps1`; второй runner не делать.

## Task 5. Typed read/send lifecycle и A→B

**Files:** exact client files из Task 1 плюс mapping files из Task 4; client
test path не подменять fake API.

- [ ] RED: connect A создаёт epoch и чистит old queue/draft; matched target
  запускает read; mapping polling — one in-flight/1 s, deadline 2 s, stale
  после 3 s. `status`, `Refresh`, UDP и map не активируют chat.
- [ ] GREEN: каждый async result несёт `(session,target,room,epoch)`; stale
  результат отбрасывается до HUD/send. A→B во время read/send не показывает A,
  не переносит draft и не отправляет в B. Disconnect, demo, обычный server,
  target/hub death закрывают input/отправку, игра продолжает работать.
- [ ] RED/Green typed data: `[DZ] author: text` проходит UTF-8/type/control/
  size validation и memory renderer; text не становится command, `say`,
  `Cbuf_AddText`, CFG или cvar. Own participant даёт explicit success/refusal/
  unknown; accepted before disconnect не обещает retract.
- [ ] Статусы: `Sending`, `Sent`, `Not sent`, `Delivery unknown`. Unknown send
  не повторяется; после temporary read failure feed возобновляется с backoff
  2/4/8/15 s без resend. Соблюдать bounded body/TLS/redirect; повторное
  открытие во время in-flight лишь показывает busy.

**Black-box matrix/expected:** offline editor; q2 A→B during fetch; q2ded A→B
during send; fail-read → resume feed без resend; expiry/revoke; gateway restart;
menu/scoreboard; Alt-Tab; hard kill. Нужны новый epoch/room, отсутствие старого
HUD/send в B, explicit failure, no orphan и продолжение игры при недоступном
chat service.

## Task 6. Fresh packages, gameplay и release evidence

**Files:** `README.md`, `INSTALLATION.md`, `CHANGELOG.md`, canonical
`docs/RELEASE.md`; package scripts использовать только существующие interfaces.

- [ ] Сохранить exact client/coordinator/gateway versions, fresh package roots
  и baseline logs. Runtime build в этом planning turn не выполнять; release
  commands брать из `docs/RELEASE.md` после готовности кода.
- [ ] На Windows и Linux/Wine пройти без комнаты, затем live `q2` и `q2ded`:
  browser participant → native HUD; `Client1` native send → independent
  browser read с правильными author/room. Проверить движение/стрельбу,
  `say`/`say_team`, menu, Esc, Alt-Tab до и после чата.
- [ ] Повторить A→B during fetch/send, expiry, HTTPS failure, gateway/hub
  restart, normal exit и hard kill. Expected: нет old text/draft/new send в B,
  orphan, frame pause или credentials/text в CFG/log/history/demo.
- [ ] Сравнить frame-time chat/off и request latency; записать versions,
  commands, observations, artifact paths/hashes. Обновлять CHANGELOG только
  доказанным поведением и minimum client version.

## Report и принятие

Один `docs/verification/doszone-chat-admission.md` связывает scenario с exact
file/test/command/observed outcome, проверяет local Markdown links и разделяет
implemented, researched, blocked и not-run. До Q2PRO-X source/API и разрешённого
own-session send report фиксирует конкретный blocker/material/owner, не маскируя
его словом PASS.

Полное принятие требует non-empty tests обеих coordinator implementations,
offline native input/HUD, typed lifecycle/A→B и live browser ↔ Q2PRO-X ↔ browser
для `q2`/`q2ded` на Windows/Linux-Wine. Mock, read-only feed, `draw` и принятый
design отдельно это не доказывают.
