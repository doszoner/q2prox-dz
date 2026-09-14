# Map refresh: диагностика, исправление и проверки

Дата: 2026-09-14. Исходная база: `deee241ac0c4bc2c28646d4e387305fac94a9e72`.
Рабочая ветка: `fix_catalog_map_refresh`.

## Что установлено по инциденту

Серверные логи за 13 сентября сохранены локально в `DosZone/noncommit/q2prox-disconnect-20260914/`. SHA256 исходного архива `c7bb262748a72f8a48227aaf1f6c427e21313887f35dc4532892972fe78b2b91`; скопированный архив побайтно совпадает. Сопоставлены 32065 строк двух дедиков и локальный journal Q2PRO-X. В публичный Git raw логи, SDP и credentials не включаются.

Второй эпизод не был немедленным отказом загрузки карты: Q2 успешно прошёл смены в 17:58:15 и 17:59:46 UTC, вошёл на q2dm1 в 17:59:48.081. Лишь в 18:01:44.310 launcher напечатал `CONTENT_READY q2dm1`; reset пользователя на сервере — 18:01:44.322, таймаут Q2 — 18:01:47.284. В Эр-Рияде это 21:01:44 и 21:01:47. Аналогичная цепочка есть в первом эпизоде 20:33:20/23 по Эр-Рияду.

На исходном коде map-only update воспроизводит `DROP → PUT` и уничтожение старого loopback UDP endpoint. Новый WebRTC-канал может открыться, пока Q2 продолжает безрезультатный handshake по старому адресу. Реальный исторический HUB command не записан; причинная привязка к обоим эпизодам имеет высокую уверенность по корреляции и воспроизведению, а не по захваченному DROP.

Дополнительный просмотр journal 17:33:40–18:02:00 UTC не обнаружил `CONTENT_MISSING` или `CONTENT_ERROR`. Конкретная причина задержки около 118 секунд до обработки map update остаётся неизвестной: нет ответов HTTP и ошибок refresh. В launcher нет специального таймера 118/120 секунд; есть последовательные fetch до 15 секунд каждый, пауза 15 секунд между попытками и синхронный checker. Target readiness происходит после планирования и не объясняет предшествующую задержку CONTENT_READY.

Для пропадания списка локализованы три механизма:

1. Map-only DROP снимает опубликованный endpoint до готовности замены. Несколько одновременных обновлений могут временно опустошить Custom.
2. Linux первоначально прерывал весь plan из-за одной отсутствующей карты. Сценарий «две комнаты, у одной нет карты» воспроизведён на исходном коде: общий `map content check failed`, рабочий сосед тоже не публикуется. Windows rc16 этот случай уже изолирует.
3. Оба launcher пересекают готовый каталог с точными aliases из `/lookup`. Допустимый частичный lookup оставляет часть комнат без ошибки; отдельного фильтра собственных комнат нет. Это ближе всего к описанию «видны только свои», но без снимка lookup не является доказанной причиной того эпизода.

Пустой источник до READY и обновление через штатный Refresh — часть ADR 0004. Другие наблюдаемые варианты — ошибка HTTP/валидации на старте или общий gateway recovery — отличать по новым refresh-событиям и существующим событиям coordinator. Не объявлять все причины пустого списка устранёнными.

## Дизайн и pre-mortem

[Spec](../superpowers/specs/2026-09-14-catalog-map-refresh-design.md) и [план](../superpowers/plans/2026-09-14-catalog-map-refresh.md) созданы до исходных правок. Design pre-mortem: REVISE; уточнены независимый map check и точные коды checker 0/3/error с нормализацией OSError. Повторная сверка исправленного дизайна: PASS. Дополнительно найденная противоречащая строка shared-runtime design включена в явное исправление политики map-only; wire-контракты и ADR 0004 сохранены.

Отдельный plan pre-mortem: **PASS** — проверены семантика повторного PUT, коды checker, явная правка противоречия в shared design и stderr-only вывод PowerShell. Production-код до этого verdict не менялся.

## Проверки и ревью

Исходный fix-коммит: `6f3fc10` (`fix map refresh disconnects and catalog diagnostics`). Изменены только три launcher-файла, два существующих test-файла и строка shared-runtime design. Реализацию выполнил отдельный worker; последующие ревью независимы.

До изменений: Python — 22 tests PASS; PowerShell — PASS, 40 общих контрактных случаев. Новые регрессии сначала упали: Python 8 failures/4 errors, PowerShell map-only q2 ожидал 0 drops и получил 1.

После исправления:

| Проверка | Результат |
| --- | --- |
| `python3 linux/tests/test_launcher.py` | 26 tests PASS |
| `pwsh -NoLogo -NoProfile -File windows/product/tests/catalog.tests.ps1` | PASS, 40 shared cases и дополнительные регрессии planner/диагностики |
| Реальный Python planner + установленный coordinator + UDP fixture, `reproduce.py --assert-preserved` | PASS, без диагностической подмены Python-модуля |
| Endpoint при `q2dm8 → q2dm1` | `127.0.0.1:45613 → 127.0.0.1:45613`, DROP отсутствует, оба echo отвечают |
| `git diff --check` | PASS |

Endpoint smoke независимо запущен root на исправленном source; JSON сохранён в локальном `fixed-result.json`. Длительность самой интеграционной проверки — менее секунды; длительных игровых/нагрузочных матриц не запускали. Регрессии проверяют пропуск/удаление отсутствующей карты, возвращение комнаты, различие checker missing/error, transport identity и отсутствие утечки внедрённого секретного маркера в diagnostics.

Первое ревью `code-review` выполнено двумя независимыми параллельными reviewer по диапазону `deee241a...6f3fc10`. Issue tracker не требуется: spec передан явно. Spec-ось одновременно является SDD spec gate для единственного блока реализации.

## Standards

**PASS — 0 findings.** Нарушений документированных стандартов нет: сохранены границы launcher/coordinator, изоляция snapshots, строгая валидация и stdout/control protocols. Новые stderr diagnostics содержат проверенные идентификаторы и агрегированные counts. Существенных baseline smells нет: использованы существующие planners, platform libraries и тестовая инфраструктура; helpers имеют несколько вызывающих мест. Зависимости и speculative abstractions не добавлены. Правило исключать тесты из Git относится к другому репозиторию `quake2/`.

## Spec

**PASS — 0 findings.** Оба planner исключают map и ICE из transport comparison, независимо проверяют изменённую карту один раз, сохраняют свежий config и изолируют отсутствующие карты. Linux execution errors сохраняют предыдущий snapshot. UTC/монотонная длительность/stage/counts и fixed diagnostic text реализованы, registration filtering сохранён. Shared-runtime правило исправлено явно; coordinator/schema/release/install изменений нет. Не обнаружено недостающих требований, scope creep или неверной реализации.

Итог первого ревью: Standards 0, Spec 0; критических замечаний нет ни в одной оси.

## Второе последовательное ревью

`superpowers:requesting-code-review`, отдельный независимый reviewer после завершения обеих осей первого ревью: **Ready: Yes**. Critical: 0, Important: 0, Minor: 0. Проверены caller/control flow, сохранение свежего recovery config, snapshot isolation, однократный checker, stderr/success-pipeline безопасность, релевантные тесты и evidence. Это SDD quality gate и финальное ревью единственного блока реализации. Повторных прогонов уже прошедших тестов reviewer не запускал.

Оба pre-mortem, исправление дизайна, реализация, регрессии и два последовательных ревью завершены. Код принят для commit/source gate; релиз и живой gameplay этим не объявляются.

## Границы поставки

Изменения предназначены для исходников launcher на Linux/Windows. Работа не включает установку, новую release version, публикацию, удалённый дедик или изменения пользовательских CFG. Проверки coordinator с fixture gateway не являются живой WebRTC/Windows gameplay проверкой. Старые сохранённые endpoint/credentials не мигрируются: персистентного состояния здесь нет. Откат — revert fix-коммита.

Журнал пишется в stderr и автоматически на диск не сохраняется. После установки сборки с исправлением portable Windows-запуск из `cmd.exe` можно записать без сырых transport payload:

```bat
Run-Portable.cmd 2>&1 | findstr /C:"CATALOG_REFRESH" /C:"CATALOG_MAP" > catalog-refresh.log
```

Для существующего Linux desktop launch с journal identifier `q2dz-launch`:

```sh
journalctl --user -t q2dz-launch --since today --utc --no-pager | rg 'CATALOG_(REFRESH|MAP)' > catalog-refresh.log
```

`result=error stage=lookup` отличает отказ lookup от `result=ok ready=8 registered=2` (валидное частичное пересечение). `result=ok ... targets=0` — успешный пустой план; counts сами по себе не доказывают READY или отображение в UI. Повторное чтение локального источника выполняется штатной кнопкой Refresh Q2.
