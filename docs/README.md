# Документация q2prox-dz

Текущий Linux release candidate — `1.0.0-rc18`; Windows portable/setup
ожидаются из того же source commit. Результаты и ограничения фиксируются в
[ведомости rc18](verification/1.0.0-rc18.md). Исходники, package gates и игровая
приёмка отражают разные уровни проверки.

## Текущий продукт

- [Установка](../INSTALLATION.md), [архитектура](ARCHITECTURE.md),
  [сборка и состав поставок](RELEASE.md), [изменения](../CHANGELOG.md).
- [ADR 0001: Windows backend](adr/0001-windows-webrtc-transport-backend.md),
  [0002: жизненный цикл](adr/0002-launcher-owned-session-lifecycle.md),
  [0003: Linux guardian](adr/0003-linux-managed-session-and-guardian.md),
  [0004: общий gateway и Custom](adr/0004-q2pro-x-custom-source-room-hub.md).
- [Диагностика сессий](LOGGING.md): реализована после архивов rc17.
- [Проверка архивов rc17](verification/1.0.0-rc17.md): исторические результаты
  с точными границами, не новая проверка текущего HEAD.

## Необходимые доработки и предложения

**TURN требует ограничения расхода шлюзом.** Ограничение каталога в 256 targets
не ограничивает общую квоту сервера, а выбранные ICE endpoints предлагаются каждому
соединению. [Разбор источников расхода, квот и критериев проверки](TURN-RESOURCE-POLICY.md).

| Материал | Состояние |
| --- | --- |
| [OpenSpec: nearest TURN](../openspec/changes/q2prox-nearest-turn/proposal.md) | Реализовано в исходниках; Python/PowerShell Core и Linux-сценарий через обычный Hub проверены; WinPS 5.1 и Windows-сценарий ожидаются. Сокращение endpoints, не защита общей квоты |
| [OpenSpec: исчезновение комнат](../openspec/changes/fix-q2prox-rooms-vanish/proposal.md) | Logging и tolerant rows в коде; recovery группы D остаётся условным предложением |
| [Дизайн чата](design/2026-09-06-doszone-chat.md), [план](superpowers/plans/2026-09-06-doszone-chat.md), [ревью](reviews/2026-09-06-doszone-chat.md) | Исследование/план, не поставленная функция чата; room snapshot не равен чату |
| [Внешний VPS gateway](archive/2026-08-23-hosted-vps-gateway-proposal.md) | Архивное предложение, не принятая архитектура |

[Handoff: Windows rc18 из того же source commit и checklist nearest TURN](verification/2026-10-18-nearest-turn-windows-handoff.md).

[Как читать OpenSpec и проверять формат](../openspec/README.md).
Спецификации в changes могут включать невыполненные требования.

## Планы и история решений

| Дизайн / план | Как использовать |
| --- | --- |
| [Dedicated](design/2026-09-03-dedicated-server-support.md), [план](superpowers/plans/2026-09-03-dedicated-server-support.md) | История добавления q2ded; текущий контракт в ADR 0004 |
| [Общий gateway](design/2026-09-03-shared-multiroom-gateway.md), [план](superpowers/plans/2026-09-03-shared-multiroom-gateway.md) | Реализованное направление; прежнее исключение lazy/idle eviction не отменяет новой проблемы TURN |
| [Пустой Custom Windows](superpowers/plans/2026-09-08-windows-empty-custom-fix.md) | История rc16, результаты в verification |
| [Смена карты: дизайн](superpowers/specs/2026-09-14-catalog-map-refresh-design.md), [план](superpowers/plans/2026-09-14-catalog-map-refresh.md) | Исправление `6f3fc10`, архивы rc17 |
| [Архив и происхождение](archive/README.md) | Первоначальный gateway, WebView2 spike, dual package и review remediation; старые задачи не переносить автоматически в backlog |

В архивных планах сохранены исходные отметки выполнения. Это история автора,
а не повторная проверка; локальные evidence могут быть недоступны читателю.
Нормативное описание текущего поведения имеет приоритет над старым планом.

## Исследования и диагностика

- [Каталог и исчезновение комнат](research/2026-10-01-rooms-vanish.md).
- [Обрывы после смены карты](verification/2026-09-14-disconnect-map-refresh-diagnosis.md).
- [Autoprobe и задержка входа](verification/2026-09-13-client-startup-delay-investigation.md).
- [Разные профили CFG](verification/2026-09-11-client-config-profile-diagnosis.md).
- [Участники лобби и игроки](verification/2026-08-29-lobby-and-game-player-sources.md).
- [Связность dedicated](research/2026-09-03-dedicated-server-connectivity.md).
- [Сервис чата](research/2026-09-06-doszone-chat-service.md),
  [клиент чата](research/2026-09-06-doszone-chat-client.md),
  [локальный обмен](research/2026-09-06-q2prox-local-text-exchange.md).

Публичные диагностические сводки отредактированы: без пользовательских логов,
переписки, личных путей и TURN credentials. Сторонние чатовые бинарники,
примеры без установленного происхождения и полные приватные evidence сохранены
вне репозитория. Удаление локальной ссылки не означает удаление исходного материала.
