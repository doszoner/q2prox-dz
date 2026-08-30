# Changelog

Заметные пользовательские изменения перечисляются здесь. Технические команды,
хеши и результаты gates хранятся отдельно в `docs/verification/`.

Формат следует [Keep a Changelog](https://keepachangelog.com/ru/1.1.0/),
версии проекта — [Semantic Versioning](https://semver.org/lang/ru/).

## [Unreleased]

## [1.0.0-rc10] - 2026-08-30

### Added

- Живое обновление списка комнат Dos.Zone во вкладке `Custom`: launcher
  опрашивает каталог примерно раз в 15 секунд, а Q2PRO-X получает текущие
  строки после обычного `Refresh`.
- Запуск с пустым списком `Custom`: отсутствие комнат или временная ошибка
  каталога не требуют отдельного окна и не завершают Q2PRO-X.

### Changed

- Поддерживаются два независимых слота комнат. Уже выбранная активная полная
  комната сохраняется; третья комната её не вытесняет.
- Для перехода между комнатами достаточно штатного `Disconnect` и выбора другой
  строки в `Custom`; ручное редактирование CFG, Favorites и аргументов запуска
  не требуется.

### Fixed

- Windows coordinator больше не завершает локальный HTTP-источник `Custom` с
  `WSAEINVAL`: запрашиваемые флаги `WSAPoll` приведены к контракту Winsock.

## [1.0.0-rc9] - 2026-08-29

### Added

- Выбор комнат перенесён во встроенный браузер Q2PRO-X: до двух комнат Dos.Zone
  доступны в штатной вкладке `Custom`, отдельного окна выбора нет, а игру не
  нужно перезапускать для перехода между комнатами.

### Removed

- Удалён прежний single-room путь текущего launcher/coordinator; архив rc8
  остаётся проверенным откатом.

### Changed

- Число из каталога не выдаётся за число игроков; реальные игроки, их имена,
  карта и ping берутся встроенным браузером из обычного Quake II status.
- Поддерживаемый проверяемый профиль обновлён до Q2PRO-X 1.5 by ly от
  2026-08-25.
- Закрытие или падение одной комнаты в hub-режиме завершает только её gateway;
  Q2PRO-X и остальные комнаты остаются доступны.

## [1.0.0-rc8] - 2026-08-24

### Fixed

- Windows gateway больше не переводит WebView2 transport в фоновый режим:
  устранены обновления игрового мира примерно раз в секунду без постоянных
  Chromium-флагов.
- Windows и Linux запускают Q2PRO-X в правильном порядке:
  `+set game 3zb2 +connect 127.0.0.1:<port>`.
- Исправлены Windows lifecycle, child-event framing, room-close и catalog
  validation defects, найденные при ревью.
- Исправлены Linux internal-port race, source installer и сохранение
  санитизированных lifecycle diagnostics.

### Changed

- Windows portable/setup и Linux package собираются из закреплённых зависимостей
  канонической release-chain с allowlist, fresh-extract smoke и SHA-256.
- Windows setup использует versioned staging, безопасное обновление и удаляет
  только принадлежащие add-on файлы.
- Добавлены bounded transport cadence и datagram counters без содержимого
  пакетов и credential values.

[Unreleased]: https://github.com/doszoner/q2prox-dz/compare/v1.0.0-rc10...HEAD
[1.0.0-rc10]: https://github.com/doszoner/q2prox-dz/compare/v1.0.0-rc9...v1.0.0-rc10
[1.0.0-rc9]: https://github.com/doszoner/q2prox-dz/compare/v1.0.0-rc8...v1.0.0-rc9
[1.0.0-rc8]: https://github.com/doszoner/q2prox-dz/releases/tag/v1.0.0-rc8
