# Changelog

Заметные пользовательские изменения перечисляются здесь. Технические команды,
хеши и результаты gates хранятся отдельно в `docs/verification/`.

Формат следует [Keep a Changelog](https://keepachangelog.com/ru/1.1.0/),
версии проекта — [Semantic Versioning](https://semver.org/lang/ru/).

## [Unreleased]

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

[Unreleased]: https://github.com/doszoner/q2prox-dz/compare/v1.0.0-rc8...HEAD
[1.0.0-rc8]: https://github.com/doszoner/q2prox-dz/releases/tag/v1.0.0-rc8
