# Допуск чата Dos.Zone: актуальное состояние Task 1–6

> Этот отчёт фиксирует фактическое состояние редакции 2: исследование собственной
> отправки вместо требования нового API заранее и внутренний Task 4 mapping без
> отдельного внешнего C3. Исторические PASS ниже сохранены; условия редакции 1
> не являются текущим планом.

Дата: 2026-09-06. Checkout: `q2prox-dz-dedicated-compat`.

**Вердикт:** полный двусторонний чат в нативном HUD/input Q2PRO-X пока не
доказан и не завершён; native sender остаётся **UNKNOWN**, а не невозможным.
Quake-сервер, `quake2`, WebRTC-NET/HumbleNet и фоновые службы менять нельзя;
правки Q2PRO-X разрешены. Исходники Q2PRO-X недоступны; bounded client-seam
audit завершён, но реализация клиента пока недоступна.

## Фактическое состояние Task 1–6

| Задача | Состояние | Что подтверждено сейчас |
| --- | --- | --- |
| Task 1 — native input/callback/connection/render research | **DONE (BOUNDED AUDIT)** | [Exact client-seam audit](../research/2026-09-06-doszone-chat-client.md#bounded-client-seam-audit-exact-package-and-documented-callbacks-2026-09-06) завершён; source/build package отсутствует. |
| Task 2 — own-session program send + independent read | **NOT PROVEN** | Browser UI send/read PASS; собственная программная/native отправка не доказана. |
| Task 3 — offline native editor/HUD | **NOT IMPLEMENTED** | Menu/cvar/draw PASS; native editor/HUD patch не начат. |
| Task 4 — Linux/Windows mapping | **IMPLEMENTED — LINUX/WINDOWS PASS** | Реализация затрагивает 9 code/ADR files; Linux CTest 5/5 PASS за 21,8 с и Windows build/matrix/core/process-cleanup PASS. |
| Task 5 — typed lifecycle и A→B | **NOT RUN** | Ждёт client implementation material; mapping сам по себе чат не доказывает. |
| Task 6 — fresh packages/gameplay/release | **NOT RUN** | Полная Windows/Linux-Wine цепочка не выполнялась. |

## Проверенная текущая база

- Подробные факты и первичные ссылки: [исследование клиента](../research/2026-09-06-doszone-chat-client.md) и [исследование службы](../research/2026-09-06-doszone-chat-service.md). Исторический blocker-текст сюда не переносится.
- В отдельной копии и Wine-профиле проверен exact Q2PRO-X 1.5, EXE SHA-256 `831f42dc96e3399a8a8737943d6fde752e11f5e5f48ffc5957537c4e66cbf71d`.
- **PASS (клиентская отрисовка):** `q2pro-x.menu2` создаёт меню; `field` меняет cvar; штатный `draw` показывает значение поверх карты.
- **PASS (эта строка):** кириллица `Client1: Привет; ё` из фиксированной UTF-8 fixture отображается штатным `draw`.
- Кириллический input/paste не доказаны. Меню, поле/cvar и отрисовка доказаны; это не доказывает внешний feed, native send или async HTTPS.
- Исходные EXE и четыре config/menu-файла сверены по SHA-256 до/после; отдельная копия закрыта, пользовательская установка не изменена.
- **PASS (только browser path):** официальным UI отправлена одна нейтральная строка от `Client1` в `test[srv]`; независимый `GET /room/get` нашёл собственное сообщение в точном room ID, `q2ded`.
- Этот PASS означает browser input → public chat feed. Q2PRO-X в service-тесте не запускался; iframe показал `Server is not responding`, вход на карту этим опытом не доказан.
- В исходной проверке не было реализации полного native chat и готового client/service package для него; production-код, сервер и транспорт тогда не менялись. Secrets/чужие peer IDs не извлекались и не переносились в инструменты; heartbeat не воспроизводился; штатный браузер мог использовать свою auth.

## Task 1/3 — клиентский путь Q2PRO-X: **AUDIT DONE / NOT IMPLEMENTED**

Menu, cvar и `draw` уже PASS и не требуют повторно доказывать наличие исходников.
Bounded audit завершён; его [граница client seam и source/build](../research/2026-09-06-doszone-chat-client.md#bounded-client-seam-audit-exact-package-and-documented-callbacks-2026-09-06)
зафиксирована отдельно. Для client patch отсутствует exact source/build package
либо first-party extension ABI; это делает реализацию недоступной сейчас, но не
доказывает невозможность. Это не blanket-требование нового официального API
заранее; текущий `GET /servers` остаётся только catalog/status/connect seam, не
chat/IPC.

## Task 2 — own-session program send: **NOT PROVEN**

Browser UI send не создаёт native identity, а собственная программная/native
отправка и независимое чтение пока не доказаны. [SECURITY](../../linux/docs/SECURITY.md)
сохраняет запрет private-write reverse engineering: не использовать browser
heartbeat, `mySecret`, чужие peer IDs/cookies и не сочинять URL, body или credential.
Это неизвестный путь, не доказательство невозможности.

## Task 4 — внутренний mapping: **IMPLEMENTED — LINUX/WINDOWS PASS**

[Целевая архитектура](../design/2026-09-06-doszone-chat.md) разрешает узкий
read-only `GET /q2dz/rooms` рядом с неизменным `/servers`. Rev2 уже разрешает
этот внутренний Task 4. Реализация затрагивает 9 code/ADR files. Фактический
Linux CTest дал **5/5 PASS за 21,8 с** ([лог](</home/ssolovey/Projects/Q2/DosZone/noncommit/q2dz-chat-mapping-20260906.L9KWY9/linux-ctest.log>)).
Windows MSVC собрался; первый CTest был отклонён из-за неполного source zip без
tests/contracts/hub-protocol-cases.json. Исправленный архив
`coordinator-source-queue-fixed.zip` SHA-256
`e2657192ef5352d4a46c1d09d737d0c60cccff3d4ed6e7d019d2449d384ffd71` дал build
и полный matrix **PASS** ([matrix log](</home/ssolovey/Projects/Q2/DosZone/noncommit/q2dz-chat-mapping-20260906.L9KWY9/windows-matrix-pass.log>)); Windows
26200.9168 / MSVC 19.44.35228. Предыдущая диагностика queue-протокола сохранена
([log](</home/ssolovey/Projects/Q2/DosZone/noncommit/q2dz-chat-mapping-20260906.L9KWY9/windows-matrix-diagnostic.log>)); финальный core test **2/2 PASS**, SHA-256
`4c3a78fee648ca26ec2e4d6055436bd1bd56a0b4a86786ec5d17c4471ff5ca19` и
coordinator hash `D3B6A59540805C095F7AE6DCC0739F0D1782D9A8FFD3C5966F2F034EAC59B1ED`
сверены до/после ([core log](</home/ssolovey/Projects/Q2/DosZone/noncommit/q2dz-chat-mapping-20260906.L9KWY9/windows-core-final.log>)); process cleanup **PASS**.
В Windows очередь была рассчитана на 32 строки (31 обычное событие + резерв).
Массовый READY вызывал переполнение: синтетический `Q2DZ/1FATAL` попадал в HUB/2
и приводил к сбросу списка. Лимит 258 = 256 READY + heartbeat + fatal; байтовый
лимит 64 KiB сохранён. Regression проверяет целый burst и порядок.
В matrix входят source invariants, dynamic/session-change, max-targets,
case-sensitive, protocol/malformed/partial recovery, restart/exit/crash и
hard-kill сценарии; mapping независим от native chat и не доказывает отправку.
Для воспроизведения Windows: source root `repo/windows/coordinator`, общий
`tests/contracts` — рядом в `repo`; `tools/build-release.ps1 -BuildDir
<work>/build-final`, затем `tools/run-lifecycle-matrix.ps1 -BuildDir
<work>/build-final/Release`. DefaultBuildDir не использовать.

## Остаток native client/send

Для native editor/HUD нужен exact client source/build или эквивалентное
documented extension. Собственная native-регистрация/отправка в границах
[SECURITY](../../linux/docs/SECURITY.md) не установлена; browser send PASS этого
не заменяет.

## Оставшиеся задачи

Task 5 (typed lifecycle/A→B) и Task 6 (fresh packages, gameplay, release) не
запускались. Полный Goal двустороннего чата не завершён; этот отчёт не объявляет
PASS и не маскирует неизвестный native sender обходом.
