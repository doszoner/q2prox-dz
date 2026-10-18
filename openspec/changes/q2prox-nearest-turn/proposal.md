# Proposal

[Handoff: продолжение на Windows](../../../docs/verification/2026-10-18-nearest-turn-windows-handoff.md).

Статус: реализовано в исходниках 2026-10-18; компонентные проверки Python и
PowerShell Core пройдены. Linux-сценарий через обычный Hub завершён: Custom,
Refresh, вход, q2dm1 → q2dm2 без перезапуска клиента и штатный Quit.
Windows-проверки по решению пользователя переданы пользователю Windows и
остаются открытыми; функциональный контракт обеих платформ сохраняется.
[Статусы и Windows checklist](tasks.md).

## Why

До этого изменения лаунчеры Linux и Windows передавали каждой комнате объединение ICE-записей
всех строк RTC-каталога. Общий шлюз поддерживает соединения с доступными
комнатами. Это увеличивает потенциальный расход TURN при discovery.
Число allocations и исчерпание production-квот не измерены.
[Разбор нагрузки и дальнейшие ограничения](../../../docs/TURN-RESOURCE-POLICY.md).

## What Changes

- Выбрать одну RTC-строку по задержке TCP-подключения и добавить TURN URL
  второй строки. Не обещать фиксированного количества URL или аллокаций.
- Кэшировать только выбранные domain в сессии; credentials читать из каждого
  свежего валидного ответа. Повторять выбор, если выбранный domain исчез.
- Предусмотреть ручной выбор `Q2DZ_RTC_SERVER` с безопасной диагностикой.
- Сохранить валидацию полного ответа, размерные лимиты выбранного снимка,
  семантическую дедупликацию и обновление ICE без пересоздания активной комнаты.

Это уменьшение набора endpoints, а не полная защита TURN-квоты. Ограничение
соединений, discovery и повторов — дальнейшая задача с отдельным контрактом
видимости `Custom`. В этой задаче не меняются шлюз, HUB/2 и зависимость WebRTC-NET.

## Capabilities

### New Capabilities
- `q2prox-ice-selection`: выбор ICE, кэш domain, свежие credentials и ручной выбор.

### Modified Capabilities
- Нет. Требования проверки RTC из `q2prox-catalog-refresh` остаются в силе.

## Impact

- `linux/tools/q2dz-launch`: `_ice_snapshot`, `_dedicated_ice_servers`, создание
  сессии и обновление targets; `linux/tests/test_launcher.py`.
- `windows/product/catalog.ps1`, `windows/product/q2dz-launch.ps1`, их тесты.
- `tests/contracts/doszone-api-cases.json`: общие случаи выбора для двух ОС.
- ADR 0004 и документация: описывать новый выбор только после реализации.
