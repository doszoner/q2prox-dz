# Spec Delta

> Условное предложение группы D; не реализовано. Нельзя читать SHALL ниже как
> описание текущего поведения. Решение о реализации требует подтверждения причины
> по диагностике сессии; историческая отметка задачи 4.1 сама по себе этого не доказывает.

## Purpose

Восстановление целей координатора q2prox-dz после сбоя транспорта так, чтобы цель, исчерпавшая бюджет повторов, не оставалась тупиком до перезапуска. **Условно:** вводится группой D только при решении `confirmed` в `D-decision.md` (design D15); без него эта delta-спека при архивации не промотируется.

## ADDED Requirements

### Requirement: READY replenishes the recovery budget

Координатор на обеих ОС SHALL сбрасывать счётчик восстановления цели в ноль при событии готовности транспорта (любой READY, включая мгновенный). Константы лимитов по ОС SHALL остаться прежними (Windows q2 2 / q2ded 3 по сбоям, Linux q2 1 / q2ded 2 по стартам восстановления). Обычный путь восстановления (сбой → terminal → повтор на следующем PUT при оставшемся бюджете) SHALL остаться без изменений.

#### Scenario: Failure right after READY

- **WHEN** цель с `recovery=<limit-1>` становится READY и сразу получает сбой
- **THEN** она проходит terminal с `recovery=1 retry=immediate` и пересоздаётся на следующем PUT, а не остаётся с `retry=none`.

#### Scenario: Fast flapping is unbounded by design

- **WHEN** цель повторяет READY→сбой→READY много раз
- **THEN** каждый цикл записывается событием `TARGET` с `recovery=`, и восстановление продолжается.

### Requirement: Exhausted target retries after a time-based cooldown

Кулдаун SHALL применяться только к цели с исчерпанным бюджетом: первый PUT по ней, который иначе был бы проигнорирован, SHALL установить `retry_not_before = now + cooldown` (монотонное время, продуктовый дефолт 60 с, тестовая опция `--retry-cooldown-ms` рядом с `--ready-timeout-ms`) и записать `TARGET … retry=cooldown`; последующие PUT до срока SHALL игнорироваться без продления; первый PUT при `now >= retry_not_before` SHALL пересоздать цель с нулевым счётчиком, новым идентификатором, снятым старым маршрутом, свежим сроком готовности и сброшенными флагами (`retry=recreate`). Кулдаун MUST NOT считаться по числу PUT и MUST NOT задерживать повторы при оставшемся бюджете.

#### Scenario: PUT within cooldown

- **WHEN** PUT по цели с исчерпанным бюджетом приходит раньше срока
- **THEN** он игнорируется, срок не продлевается, `TARGET` не пишется повторно.

#### Scenario: PUT at the boundary

- **WHEN** PUT приходит ровно при `now == retry_not_before` (матрица с `--retry-cooldown-ms 1000`)
- **THEN** цель пересоздана с новым идентификатором, `retry=recreate`, и попадает в pending.

#### Scenario: Default cooldown is 60 s

- **WHEN** опция `--retry-cooldown-ms` не задана
- **THEN** разбор опций даёт 60000.

#### Scenario: Catalog errors pause retries

- **WHEN** обновление каталога падает и heartbeat не приходит
- **THEN** PUT нет, срок не наступает «сам»: пересоздание ждёт следующего успешного heartbeat.

#### Scenario: Closed room leaves the catalog

- **WHEN** цель получила `ROOM_CLOSED`, а комната исчезла из каталога
- **THEN** PUT после первого miss игнорируется (кулдаун может быть установлен), второй miss даёт DROP, пересоздания нет.

#### Scenario: Disabled transport is not retried

- **WHEN** транспорт отключён (`transport_disabled`)
- **THEN** кулдаун и пересоздание не применяются.

### Requirement: Stale events and repeated terminal are inert

События с идентификатором цели, уже заменённым пересозданием или `recover_gateway`, SHALL игнорироваться. Повторный terminal по той же цели MUST NOT менять её состояние или признак повторяемости.

#### Scenario: Late READY for an old id

- **WHEN** READY со старым идентификатором приходит после пересоздания
- **THEN** состояние новой цели не меняется.

#### Scenario: Second terminal for a tombstone

- **WHEN** terminal приходит повторно для уже закрытой цели
- **THEN** состояние и признак повторяемости не меняются.

### Requirement: Documentation follows the policy

`windows/coordinator/README.md`, `linux/docs/ARCHITECTURE.md` и ADR 0004 (раздел политики восстановления) SHALL описывать сброс бюджета при READY и кулдаун; исторические отчёты не переписываются.

#### Scenario: Docs match code

- **WHEN** группа D выполнена
- **THEN** фразы «successful readiness does not reset this budget» в этих файлах заменены на новую политику.
