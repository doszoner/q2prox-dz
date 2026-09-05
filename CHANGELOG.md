# Changelog

Заметные пользовательские изменения перечисляются здесь. Технические команды,
хеши и результаты gates хранятся отдельно в `docs/verification/`.

Формат следует [Keep a Changelog](https://keepachangelog.com/ru/1.1.0/),
версии проекта — [Semantic Versioning](https://semver.org/lang/ru/).

`1.0.0-rc6` — первая сохранённая версия проекта. Более ранних выпусков и
пользовательских артефактов в истории проекта нет.

## [Unreleased]

## [1.0.0-rc14] - 2026-09-05

### Added

- Общий многокомнатный gateway теперь рассчитан на все совместимые комнаты
  Dos.Zone вместо фиксированных `3 q2ded + 2 q2` слотов.
- Linux live gateway-layer проверка подтвердила одновременные `q2` и `q2ded`,
  обычный Quake II `status`, удаление одной цели без поломки второй и
  добавление новой dedicated цели после обновления ICE без перезапуска gateway.

### Changed

- Shared gateway больше не зависит от production `AliasQuery`: транспортная
  маршрутизация использует virtual PeerId, а resolved concrete PeerId нужен
  только для `READY` и защиты от duplicate peer.
- Обновлён закреплённый WebRTC-NET commit и SHA-256 runtime artifacts.

### Fixed

- Исправлено сохранение pending datagram state в WebRTC-NET, когда flush не
  смог передать пакет сразу; такие пакеты теперь остаются pending до следующей
  возможности отправки.
- Windows coordinator теперь принимает CRLF-строки управления и terminal events
  от PowerShell/child pipes без ложного protocol failure.
- Windows lifecycle matrix больше не использует устаревшее поле
  `credential` в тестовом `ROOMS/2 PUT` и устойчивее ждёт bounded recovery
  после terminal events.
- Windows release smoke проверяет текущий heartbeat-контракт launcher
  (`ICE`/`DROP`/`PUT`) вместо удалённых старых helper-имён.

## [1.0.0-rc13] - 2026-09-03

### Added

- Dedicated-серверы Dos.Zone с каталогом `q2ded` теперь доступны рядом с
  обычными комнатами во встроенной вкладке `Custom` Q2PRO-X.
- Одновременно поддерживаются до трёх dedicated-серверов и до двух обычных
  комнат; закрытие одного endpoint не завершает Q2PRO-X.

### Changed

- Dedicated-сервер получает не более двух автоматических попыток
  восстановления за один запуск. После этого достаточно перезапустить
  Q2PRO-X — Dos.Zone.
- Для dedicated-серверов используются точный зарегистрированный alias и свежая
  объединённая конфигурация Dos.Zone; игровые и секретные данные по-прежнему не
  сохраняются в CFG, аргументах или постоянных файлах.

## [1.0.0-rc12] - 2026-08-31

### Fixed

- Windows launcher снова заполняет вкладку `Custom`: результат чтения
  каталогов Dos.Zone больше не получает лишний уровень массива при прохождении
  через блок PowerShell. Добавлена проверка как корневого объекта каталога, так
  и корневого массива RTC через фактический путь загрузки.

## [1.0.0-rc11] - 2026-08-30

### Fixed

- Q2PRO-X больше не получает команду принудительного открытия пустого
  браузера серверов. Он использует обычную настройку стартового меню; комнаты
  Dos.Zone доступны через штатный `Join online` → `Custom`.
- Ожидание после закрытия комнаты ограничено 20 секундами вместо стандартных
  120 секунд. Значение действует только в запущенном add-on процессе Q2PRO-X.

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

## [1.0.0-rc7] - 2026-08-24

### Fixed

- Windows и Linux теперь выбирают модификацию `3zb2` до подключения к серверу:
  `+set game 3zb2 +connect 127.0.0.1:<port>`. Это устранило ошибку разбора
  серверных данных при подключении из `baseq2`.

### Added

- Добавлены ограниченные обезличенные счётчики задержек Windows transport для
  воспроизведения проблемы «90 FPS, но одно обновление мира в секунду» без
  записи содержимого пакетов или данных комнаты.
- Полный Windows lifecycle matrix включён в каноническую release-chain до
  упаковки portable и setup.

### Changed

- Добавлена проверяемая привязка исходников и пакетов к закреплённому commit
  WebRTC-NET и точным SHA-256 зависимостей.
- Исправлены названия пользовательских архивов, путь сохранённого Q2PRO-X и
  граница обязательной цифровой подписи в документации поставки.

## [1.0.0-rc6] - 2026-08-23

### Added

- Первая пользовательская поставка шлюза для подключения установленного
  Q2PRO-X к существующей комнате Quake II на Dos.Zone.
- Windows portable и setup-поставки на основе WebView2, а также Linux x86-64
  package с нативным WebRTC-NET gateway и запуском Q2PRO-X через Wine.
- Launcher выбирает комнату, проверяет наличие нужной карты и запускает
  неизменённый Q2PRO-X через локальный UDP endpoint.
- Coordinator владеет Q2PRO-X и gateway как одной сессией, ограничивает
  readiness, завершает дочерние процессы и очищает временные данные.
- Fresh room/ICE данные передаются только внутри сессии и не сохраняются в
  аргументах запуска, CFG или пользовательских файлах.

### Changed

- Add-on устанавливается отдельно от Quake II и не заменяет EXE, CFG, карты или
  PAK-файлы пользователя.

[Unreleased]: https://github.com/doszoner/q2prox-dz/compare/v1.0.0-rc14...HEAD
[1.0.0-rc14]: https://github.com/doszoner/q2prox-dz/compare/v1.0.0-rc13...v1.0.0-rc14
[1.0.0-rc13]: https://github.com/doszoner/q2prox-dz/compare/v1.0.0-rc12...v1.0.0-rc13
[1.0.0-rc12]: https://github.com/doszoner/q2prox-dz/compare/v1.0.0-rc11...v1.0.0-rc12
[1.0.0-rc11]: https://github.com/doszoner/q2prox-dz/compare/v1.0.0-rc10...v1.0.0-rc11
[1.0.0-rc10]: https://github.com/doszoner/q2prox-dz/compare/v1.0.0-rc9...v1.0.0-rc10
[1.0.0-rc9]: https://github.com/doszoner/q2prox-dz/compare/v1.0.0-rc8...v1.0.0-rc9
[1.0.0-rc8]: https://github.com/doszoner/q2prox-dz/compare/v1.0.0-rc7...v1.0.0-rc8
[1.0.0-rc7]: https://github.com/doszoner/q2prox-dz/compare/v1.0.0-rc6...v1.0.0-rc7
[1.0.0-rc6]: https://github.com/doszoner/q2prox-dz/releases/tag/v1.0.0-rc6
