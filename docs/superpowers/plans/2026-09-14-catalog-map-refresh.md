# Catalog map refresh Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Сохранить игровой endpoint при доступной новой карте, изолировать отсутствующие карты и объяснять ошибки/пустые результаты обновления каталога безопасным журналом.

**Architecture:** Исправляются существующие Python/PowerShell planners и обёртки refresh. Coordinator, transport, протоколы и UI остаются прежними; свежий полный config хранится для recovery, metadata не пересоздаёт live target.

**Tech Stack:** Python stdlib/unittest, PowerShell/.NET, существующий coordinator и UDP fixture для ограниченного интеграционного smoke.

---

Worktree: `/path/to/DosZone/noncommit/q2prox-map-refresh-worktree`, ветка `fix_catalog_map_refresh`. База обоих итоговых ревью: `deee241ac0c4bc2c28646d4e387305fac94a9e72`. Spec: `docs/superpowers/specs/2026-09-14-catalog-map-refresh-design.md`. Работа и коммит уже разрешены пользователем; без установки, push и новой поставки. Issue tracker отсутствует; источником требований служит этот spec, устанавливать инструменты для issue tracker не требуется.

## Task 1: Исправить планирование и добавить журнал refresh на обеих платформах

**Files:**
- Modify: `linux/tools/q2dz-launch`: `reconcile_targets`, `content_ready`, `refresh_targets`, короткий helper stderr-события.
- Modify: `windows/product/catalog.ps1`: `Test-Q2DzRoomConfigChanged`, `Get-Q2DzTargetPlan`, короткий helper stderr-события.
- Modify: `windows/product/q2dz-launch.ps1`: `Get-Q2DzLiveTargets`.
- Modify: `docs/design/2026-09-03-shared-multiroom-gateway.md`: уточнить внутреннее map-only правило, как явно предписано новым spec.
- Test: `linux/tests/test_launcher.py`, `windows/product/tests/catalog.tests.ps1` — дополнить существующие проверки, не создавать тестовую инфраструктуру.

- [x] **1. Добавить падающие регрессии в существующие тесты.**

Python: использовать существующие `room`, `dedicated_room`, `lookup_for`, `rtc_rows`, `catalog`, `FakeProcess`. Ядро map-only теста (в `LauncherTests`):

```python
old = room(map_name="q2dm1")
new = room(map_name="q2dm2")
config = launch.make_room_config(old, lookup_for([old]), rtc_rows())
current = {old["room"]: launch.HubTarget(old["room"], launch._config_text(config), map_name="q2dm1")}
checked = []
plan = launch.reconcile_targets(current, [new],
    lambda row: launch.make_room_config(row, lookup_for([row]), rtc_rows()),
    lambda row: checked.append(row["room"]) or True)
self.assertEqual(plan.drops, ())
self.assertEqual(checked, [old["room"]])
self.assertEqual(plan.targets[old["room"]].map_name, "q2dm2")
self.assertEqual(json.loads(plan.targets[old["room"]].config)["map"], "q2dm2")
```

Повторить этот сценарий для dedicated row. Для отсутствующей карты заменить старый тест `test_map_content_failure_preserves_prior_working_target_and_emits_nothing`: false теперь означает отсутствие, а не ошибку. Проверить отсутствующую новую/существующую комнату вместе с доступным соседом; `drops` содержит только существующую отсутствующую, следующий план с доступными картами возвращает её. Сохранить существующий shared test исключения checker, который требует прежний snapshot и отсутствие записи.

Коды checker проверяются через уже предусмотренный `runner`:

```python
for code, available in ((0, True), (3, False)):
    self.assertEqual(launch.content_ready("/games/Q2PRO-X.exe", "q2dm1",
        runner=lambda *a, **k: subprocess.CompletedProcess(a, code)), available)
for code in (64, 70):
    with self.assertRaises(launch.LaunchError):
        launch.content_ready("/games/Q2PRO-X.exe", "q2dm1",
            runner=lambda *a, **k: subprocess.CompletedProcess(a, code))
with self.assertRaises(launch.LaunchError):
    launch.content_ready("/games/Q2PRO-X.exe", "q2dm1",
        runner=mock.Mock(side_effect=OSError("secret-marker")))
```

PowerShell: рядом с существующими `$mapPlan`/`$icePlan` добавить доступную смену карты на основании `$mapCurrent` и `$mapMakeConfig`; `Assert-Equal 0 $availablePlan.Drops.Count`, проверить свежие MapName и config.map и однократный callback. Сохранить текущие missing/sibling/recovery/error tests и transport-change/DROP-before-PUT checks.

Для диагностики захватить Python stderr через `contextlib.redirect_stderr(io.StringIO())`, PowerShell `[Console]::SetError([IO.StringWriter]::new())` с обязательным восстановлением в finally. Реальным refresh-функциям передать существующие fixtures через FetchJson/runner. Проверить успешный непустой и пустой каталог, ready row без alias, отказ fetch на lookup и отказ checker; проверять stage/result/counts/UTC/duration, отсутствие `secret-marker` и отсутствие вывода диагностики в returned plan/stdout. Исключение от fetch должно остаться исключением.

Неполный lookup — успешный ответ с меньшим registered count, не новый вид ошибки. Существующую точную проверку регистрации не обходить и устаревшие aliases не кэшировать. Readiness происходит после planner и не является объяснением задержки CONTENT_READY.

- [x] **2. Запустить регрессии и увидеть ожидаемое падение.**

```sh
python3 linux/tests/test_launcher.py
pwsh -NoLogo -NoProfile -File windows/product/tests/catalog.tests.ps1
```

Ожидание до fix: map-only тест обнаруживает DROP, Linux missing-map сценарий прерывает обе комнаты; новый журнал отсутствует. Исходная база уже проверена: Python 22 теста PASS, PowerShell 40 shared cases PASS.

- [x] **3. Внести минимальную реализацию.**

В Linux перед вычислением `changed` исключить два поля из отдельных декодированных копий; полный `config_text` не менять:

```python
for config in (previous_config, fresh_config):
    config.pop("iceServers", None)
    config.pop("map", None)
changed = fresh_config != previous_config
map_changed = bool(target.map_name) and target.map_name != map_name
if map_changed and not map_available(row):
    catalog_event(f"CATALOG_MAP result=missing room={room_id} map={map_name}")
    drops.add(room_id)
    continue
```

Цикл новых комнат пропускает уже обработанные current IDs; false пишет missing и продолжает, вместо общего исключения:

```python
if room_id in current or room_id in updated:
    continue
if not map_available(row):
    catalog_event(f"CATALOG_MAP result=missing room={room_id} map={row['createSettings']['map']}")
    continue
```

`content_ready` сохраняет существующие аргументы checker, оборачивает runner в try/except OSError и различает результат:

```python
except OSError as exc:
    raise LaunchError("content checker execution failed") from exc
if result.returncode == 0:
    return True
if result.returncode == 3:
    return False
raise LaunchError("content checker execution failed")
```

На Windows существующий цикл удаления `iceServers` расширить на `map`:

```powershell
foreach ($name in @('iceServers', 'map')) {
  if ($value.PSObject.Properties.Name -contains $name) {
    [void]$value.PSObject.Properties.Remove($name)
  }
}
```

Независимый `$mapChanged` check оставить. Цикл новых rows пропускает `$current.ContainsKey($roomId)` вместе с `$updated.ContainsKey($roomId)`, чтобы false не вызывал второй checker в том же snapshot. В двух missing-ветках добавить `Write-Q2DzCatalogEvent "CATALOG_MAP result=missing room=$roomId map=$map"` (в новой ветке map берётся из текущего row).

Два небольших platform-native helper, без framework и dependency:

```python
def catalog_event(message):
    stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    print(f"{stamp} {message}", file=sys.stderr, flush=True)
```

```powershell
function Write-Q2DzCatalogEvent {
  param([string]$Message)
  [Console]::Error.WriteLine(('{0} {1}' -f [DateTime]::UtcNow.ToString('o'), $Message))
}
```

В refresh сохранить существующие fetch, validation и make-config; обернуть тело в try/finally с начальными `result=error`, `stage=catalog`, ready/registered/targets/drops counts. Перед lookup, RTC и planner ставить соответствующий stage. При успешном plan поставить result=ok и записать его counts. В finally выдать ровно одно refresh-событие через helper:

```text
<UTC> CATALOG_REFRESH result=ok stage=plan elapsed_ms=17 previous=3 ready=4 registered=3 targets=3 drops=0
<UTC> CATALOG_REFRESH result=error stage=lookup elapsed_ms=15002 previous=3 ready=4 registered=0 targets=3 drops=0
```

`previous` — входной target count; при ошибке targets отражает сохранённое входное количество, drops=0. Linux timer `time.monotonic()`, Windows `[Diagnostics.Stopwatch]::StartNew()`/`ElapsedMilliseconds`. Ready rows вычислить после catalog validation до lookup, registered — после lookup, fresh ICE — в rtc stage. Исключения продолжать передавать существующим startup/poll catch; не печатать их текст. Helper не должен попадать в PowerShell success pipeline. На каждый refresh один summary; missing-события только при проверке конкретной комнаты.

В shared runtime design заменить противоречащую строку следующими пунктами (остальные lifecycle правила сохранить):

```text
- изменившиеся alias, game, ordered или общий transport contract ->
  DROP старой версии и PUT новой;
- изменение map -> повторный content check; доступная карта обновляет
  сохранённый config без DROP, отсутствующая снимает только эту цель;
```

Добавить ссылку на `../superpowers/specs/2026-09-14-catalog-map-refresh-design.md` как явное уточнение map-only политики от 14 сентября.

- [x] **4. Запустить два затронутых набора и один endpoint smoke.**

```sh
python3 linux/tests/test_launcher.py
pwsh -NoLogo -NoProfile -File windows/product/tests/catalog.tests.ps1
Q2DZ_DIAGNOSTIC_REPO=/path/to/DosZone/noncommit/q2prox-map-refresh-worktree python3 /path/to/DosZone/noncommit/q2prox-disconnect-20260914/reproduce.py --assert-preserved
git diff --check
```

Ожидание: оба набора PASS; smoke `PRESERVED`, old_endpoint == new_endpoint, оба отвечают. Smoke использует настоящий planner+установленный coordinator, fixture gateway и заглушку клиента; не обозначать результат как живой WebRTC/gameplay. Не запускать VM, длительные нагрузочные матрицы и unrelated suites. Повторять только затронутые проверки после реальных исправлений.

- [x] **5. Self-review и рабочий коммит.**

Проверить diff по spec; в индекс включить только шесть перечисленных ниже файлов. Тестовая политика `quake2/` не распространяется на отдельный `q2prox-dz`; здесь тесты уже tracked и регрессии сохраняются с кодом. Raw incident logs и локальные fixture outputs не коммитить.

```sh
git add linux/tools/q2dz-launch linux/tests/test_launcher.py windows/product/catalog.ps1 windows/product/q2dz-launch.ps1 windows/product/tests/catalog.tests.ps1 docs/design/2026-09-03-shared-multiroom-gateway.md
git diff --cached --name-only
git commit -m "fix map refresh disconnects and catalog diagnostics"
```

## Последовательность ревью и завершения

- [x] SDD: отдельный implementer на единственный Task 1; spec gate выполняет независимая Spec-ось первого ревью, quality gate — второй reviewer ниже. Не дублировать те же ревью ещё одним кругом; исправить подтверждённые замечания перед следующим этапом.
- [x] `code-review`: два независимых параллельных reviewer по Standards и Spec, фиксированная база выше, команда `git diff deee241ac0c4bc2c28646d4e387305fac94a9e72...HEAD`. Spec указан явно; ненужной установки issue tracker нет.
- [x] После PASS Spec-оси и завершения `code-review` — отдельный `requesting-code-review` по тому же диапазону, включая свежие исправления. Это quality gate SDD и итоговое ревью всего единственного блока реализации.
- [x] Сохранить результаты pre-mortem, ревью и checks в `docs/verification/2026-09-14-catalog-map-refresh-fix.md`; отметить ограничения исторической диагностики и живого тестирования. Закоммитить дизайн, план и итоговую проверку. Сохранить ветку/worktree; не merge/push/install.

Self-review плана: критерии spec покрыты Task 1 и завершающими ревью; используемые helper/поля определены здесь, существующие fixtures названы точно. Отдельных подсистем и новых зависимостей нет.

Plan technical-premortem: **PASS**. Независимо проверены действующий heartbeat без DROP (coordinator сохраняет endpoint), контракт кодов checker, явное уточнение shared-design map-политики и отсутствие PowerShell success-pipeline вывода у stderr helper. Ограничения: fixture не доказывает живой gameplay, неполный lookup остаётся диагностической гипотезой.
