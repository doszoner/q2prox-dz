# Dedicated Server Support Implementation Plan

> **Execution:** `superpowers:subagent-driven-development`, one task at a time,
> tests first, no push.

**Goal:** показать до трёх `q2ded`-серверов и до двух обычных комнат во
встроенной вкладке `Custom` Q2PRO-X на Linux и Windows.

**Architecture:** сохранить один существующий gateway на комнату. Python и
PowerShell нормализуют две формы каталога в прежний room config. Координаторы
получают пять независимых слотов `3 q2ded + 2 q2`; только dedicated получает
две ограниченные попытки восстановления после отказа.

**Baseline:** `947f3f8ff0db2c1e838b802340fff1cd2a780da9`.

**Execution status (2026-09-03):** Tasks 0–3 и кодовая часть Task 6 выполнены.
Linux build/package/reproducibility gates из Task 4 прошли; live gameplay и
switching точного финального архива остаются открыты. Exact Windows build,
lifecycle, package и live gates Task 5 не выполнялись. Источник фактов —
`docs/verification/1.0.0-rc13.md`; незакрытые checkbox ниже остаются
обязательными, а не считаются выполненными по плану.

## Ограничения

- Не менять transport gateway, WebRTC-NET, Q2PRO-X, Quake protocol и Dos.Zone.
- Не добавлять зависимости, общий runtime или второе окно выбора.
- Не выводить ICE/TURN values, SDP, candidates или PeerId.
- Не трогать `docs/verification/2026-08-29-lobby-and-game-player-sources.md`.
- Все commits от `doszoner <doszoner@users.noreply.github.com>`; не push.
- Worktree размещать рядом с репой, не в `~/.config/superpowers/worktrees`.

## Task 0: Compatibility preflight

**Create:** `docs/verification/2026-09-03-q2ded-compatibility-preflight.md`

- [ ] Зафиксировать checkout, HEAD, identity и dirty state.
- [ ] Одним безопасным снимком получить только публичную форму q2ded rows,
  exact `found` и число RTC rows/entries без credential values.
- [ ] Проверить: есть q2ded; `room.q2ded.*`; exact lookup; RTC schema;
  flatten+stable dedupe даёт `1..16`; compact gateway JSON `<=65535`.
- [ ] Обязательно выполнить один Linux canary через существующий gateway с
  exact dedicated alias и полным текущим агрегированным RTC payload: OOB
  `ping/ack` и настоящий `status`.
- [ ] Записать дату проверки, UTC публикации, checkout/commit/lock identities и
  обезличенный PASS/FAIL. Точные UTC intervals обязательны для финальных live
  release gates; отсутствие отдельного интервала у одноразового preflight
  фиксируется как ограничение, но не требует повторной передачи credentials.
  Invalid RTC или `found=false` — stop gate, не повод ослаблять validation.

## Task 1: Backward-compatible coordinator capacity

Сначала расширить принимающую сторону: старые launchers продолжат отправлять
два слота, а промежуточный commit останется рабочим.

### Task 1A: Linux coordinator

**Files:** `linux/src/json.cpp`, `linux/src/session_main.cpp`,
`linux/tests/unit_tests.cpp`, `linux/tests/fake_lifecycle.sh`,
`linux/docs/ARCHITECTURE.md`.

- [ ] RED: indices `0..4`, reject `5`, alias `room.q2ded.*`, five fake gateway,
  isolated close, hard-kill cleanup.
- [ ] RED: dedicated initial failure/`ROOM_CLOSED` -> exactly two recovery
  starts on heartbeat -> final tombstone; readiness does not reset budget;
  legacy q2 retains current behavior.
- [ ] Изменить только max-slots/parser bounds и один recovery counter в
  `LiveSlot`; kind определяется по проверенному room-id prefix.
- [ ] Clean portable build, CTest and fake lifecycle GREEN.
- [ ] Commit: `feat(linux): supervise dedicated room gateways`.

### Task 1B: Windows coordinator

**Files:** `windows/coordinator/src/windows/main.cpp`, coordinator tests,
`windows/coordinator/tools/run-lifecycle-matrix.ps1`, coordinator README.

- [ ] Эквивалентные RED tests: `0..4`, five gateway, isolated close, two
  recovery starts, final tombstone и root Job cleanup.
- [ ] Та же минимальная state change; WebView2 gateway не менять.
- [ ] Portable C++ checks на Linux; exact Release build/lifecycle в Windows.
- [ ] Доказать: one close не завершает Q2; Q2 exit/hard kill очищает всё.
- [ ] Commit: `feat(windows): supervise dedicated room gateways`.

## Task 2: Общий catalog contract

**Files:**

- `tests/contracts/doszone-api-cases.json`
- `linux/tools/q2dz-launch`, `linux/tests/test_launcher.py`
- `windows/product/catalog.ps1`, `windows/product/q2dz-launch.ps1`
- `windows/product/tests/catalog.tests.ps1`
- `windows/product/tests/launcher-stdin.tests.ps1`

- [ ] Сначала добавить общие fixtures: valid q2ded; unsafe/wrong-prefix room;
  duplicate alias/room; malformed metadata; invalid RTC; duplicate ICE;
  aggregate `0`, `17` и oversized JSON. Старые q2 cases сохранить.
- [ ] Для positive fixtures записать expected exact alias, ordered deduplicated
  `iceServers` и slot indices `0..2/3..4`; обе платформы совпадают. Размер
  проверяет один boundary fixture: compact JSON `<=65535`.
- [ ] Получить ожидаемый RED на Linux и Windows.
- [ ] Минимально выделить нормализацию строки в существующих файлах; без нового
  модуля или класса.
- [ ] Для q2 сохранить прежний путь. Для q2ded: `alias=room`, все fresh RTC
  rows, stable exact dedupe, пределы `16/65535`, локальные `3zb2/false`.
- [ ] Exact lookup фильтрует оба вида до content check/process spawn.
- [ ] Planner использует `0..2 q2ded`, `3..4 q2`, без cross-kind eviction.
- [ ] У каждого пула собственный bounded candidate/content-check budget.
- [ ] Удержанный q2ded на каждом успешном poll получает свежий RTC config; при
  RTC fetch/validation failure команды не отправляются и live slots остаются.
- [ ] Slot хранит map. Смена map повторяет content check: доступная обновляет
  сохранённый JSON без restart, отсутствующая очищает только slot; следующий
  recovery использует новую map. Остальные metadata не перезапускают gateway.
- [ ] GREEN:

```bash
python3 -m unittest linux/tests/test_launcher.py
pwsh -NoProfile -File windows/product/tests/catalog.tests.ps1
pwsh -NoProfile -File windows/product/tests/launcher-stdin.tests.ps1
```

- [ ] Commit: `feat: accept dedicated server catalog entries`.

## Task 3: Документация и версия

**Files:** ADR 0004, `README.md`, `INSTALLATION.md`, `docs/ARCHITECTURE.md`,
`docs/RELEASE.md`, `CHANGELOG.md`, `VERSION`, verification rc13,
`linux/README.md`, `windows/README.md`, `windows/product/README.md`,
`windows/product/README.txt`, `windows/product/README-PORTABLE.txt`.

- [ ] Принять поправку ADR: cap `3+2`, совместимый q2ded contract, bounded
  recovery, future join-descriptor seam.
- [ ] Описать только встроенный `Custom`, автоматическую настройку source,
  `Refresh`, переключение и recovery limit.
- [ ] Удалить обещания upstream `join v1`.
- [ ] Версия `1.0.0-rc13`; один раздел в общем `CHANGELOG.md`, без отдельных
  changelog-файлов на каждый прогон.
- [ ] Package-content tests проверяют, что Linux и обе Windows поставки содержат
  обновлённые пользовательские README.
- [ ] Commit: `docs: document dedicated server support`.

## Task 4: Linux release gate

- [ ] Штатная locked build/package chain; fresh install/update/uninstall.
- [ ] Installed live package: dedicated в `Custom`, настоящий status, map entry,
  действие, Disconnect, другой server, isolated close без выхода Q2PRO-X.
- [ ] В штатном состоянии Q2PRO-X открыт на `Custom`; после прогрева измерить
  60 секунд пять idle gateway, исключив процесс игры: median CPU `<=75%`
  одного logical CPU, aggregate RSS `<=1 GiB`, без устойчивого роста
  threads/handles, paging pressure, orphan/session dirs/credentials.
- [ ] Записать только факты в `docs/verification/1.0.0-rc13.md` и commit
  `test(linux): verify dedicated server release`.

## Task 5: Windows release gate

- [ ] Exact locked source-to-ZIP build; portable и setup из fresh extraction на
  чистой Windows VM.
- [ ] Live: dedicated в `Custom`; status/map/action; переключение с обычной
  комнатой; isolated close; Q2 остаётся открыт.
- [ ] В штатном состоянии Q2PRO-X открыт на `Custom`; после прогрева измерить
  60 секунд пять idle WebView2 trees, исключив процесс игры: median CPU
  `<=75%` одного logical CPU, private bytes `<=1 GiB`, без
  устойчивого роста handles/threads, paging pressure, orphan/session data.
- [ ] Записать SHA-256, canonical artifacts и факты; commit
  `test(windows): verify dedicated server release`.

## Task 6: Финализация

- [ ] Полный portable test set, dependency verifier, package allowlist, fresh
  extraction smoke, `git diff --check`.
- [ ] `git status`: без build trees, credentials, raw room JSON, `.pyc`, VM
  files и защищённого пользовательского отчёта.
- [ ] Последовательно: `ponytail:ponytail-review` -> fixes; `code-review`
  Standards+Spec -> fixes; `superpowers:requesting-code-review` -> fixes;
  `superpowers:verification-before-completion`.
- [ ] После каждой правки повторить затронутый gate, затем полный набор.
- [ ] Сообщить commits, tests, live evidence, artifacts и ограничения. Не push.

## Definition of Done

- Три здоровых `q2ded` и две `q2` одновременно доступны в `Custom` на обеих
  платформах.
- Настоящий status/connect идёт через прежний gateway-per-room path.
- Q2PRO-X переживает закрытие одного сервера и позволяет выбрать другой.
- Recovery ограничен двумя попытками и не оставляет процессы.
- Legacy q2, security boundaries и release chain не регрессировали.
- Linux и Windows пакеты проверены из fresh extraction; документация и
  changelog соответствуют фактическому результату.
