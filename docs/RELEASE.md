# Сборка и выпуск

Это единственная нормативная release-chain инструкция проекта. Команды ниже
описывают обязательный путь, но сами по себе не подтверждают PASS.
[`1.0.0-rc6`](verification/1.0.0-rc6.md) и
[`1.0.0-rc7`](verification/1.0.0-rc7.md) сохраняют исторические результаты и
последующие field findings. `1.0.0-rc8` сохраняет предыдущий проверенный
rollback. Текущий Linux candidate `1.0.0-rc18` и ожидаемые Windows
portable/setup отслеживаются в [ведомости rc18](verification/1.0.0-rc18.md).
Все три поставки должны использовать один release source commit и VERSION;
Windows продолжает человек по [handoff](verification/2026-10-18-nearest-turn-windows-handoff.md).
Исторические результаты [rc17](verification/1.0.0-rc17.md) не подтверждают rc18.
Общий alpha rc18 собирается только после готовности Windows.

Пользователю следует публиковать только canonical архивы и соседние
`.sha256`:

- `q2prox-dz-<version>-windows-x64-portable.zip`;
- `q2prox-dz-<version>-windows-x64-setup.zip`;
- `q2prox-dz-<version>-linux-x86_64.tar.gz`.

Общий alpha-архив имеет установленное имя
`q2prox-dz-alpha-<version>-<YYYYMMDD>.zip`, где дата обозначает день комплектации.
Например, сохранённый общий rc17 называется `q2prox-dz-alpha-1.0.0-rc17-20260914.zip`.
Он объединяет три платформенные поставки и документацию; VERSION платформ
не меняется из-за перепаковки общего архива. Для rc17 сохранена раскладка
`dist/rc17/`, см. [ведомость](verification/1.0.0-rc17.md).

Устаревший `tools/build-test-bundle.py` удалён: он создавал другое имя
`test-bundle`, искал входные файлы в прежних каталогах и добавлял документы
старых проверок. Исторические упоминания в отчётах не служат инструкцией по выпуску.
Удаление скрипта не означает сборку или публикацию нового архива. Платформенные команды ниже
создают свои результаты; их перенос в общий каталог не означает новую сборку.

## Публичная граница

Package allowlists исключают `input/`, reviewer prompts, work plans, caches,
raw VM evidence, логи, build directories и credentials. Tracked public tree
также исключает временные материалы; утверждённые implementation plans остаются
в `docs/superpowers/plans/`. Локальные untracked/ignored материалы не входят в
пользовательские архивы; отдельный staging framework для этого не нужен.

## Windows

Для rc18 сначала пройти [подготовку новой Windows-машины](verification/2026-10-18-nearest-turn-windows-handoff.md#подготовка-новой-windows-машины).
Там указаны компоненты Visual Studio 2022, получение точных JS/WASM-файлов
из Draft и проверка зависимостей. Один clone WebRTC-NET не даёт готовых JS/WASM.
Архив `windows-build-inputs.zip` — вспомогательные зависимости сборки,
он не заменяет portable/setup и не входит в общий пользовательский alpha.

Из корня checkout:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\windows\product\build-payload.ps1 `
  -WebView2Package C:\deps\Microsoft.Web.WebView2.1.0.4129.50.nupkg `
  -WebRtcNetSourceDir C:\deps\WebRTC-NET `
  -WebRtcNetReleaseDir C:\deps\webrtc-release `
  -OutputDir .\dist\windows-1.0.0-rc18
```

Команда проверяет `deps.lock.json`, извлекает SDK из locked NuGet, собирает
coordinator и gateway с нуля, запускает их CTest/Node gates, проверяет
`/Brepro`, создаёт только allowlisted payload и вызывает
fresh-extract real-PE smoke для portable и setup. В output остаются только
два canonical ZIP и два соседних `.sha256`. Внутренний
`package-windows.ps1` не является пользовательским release API. Windows
prerequisites и локальная component-сборка описаны в
[windows/README.md](../windows/README.md).

Выходной каталог должен отсутствовать или быть пустым. После неудачной сборки
использовать новый каталог; временный каталог диагностики сохраняет сам скрипт.
Команды загрузки Windows-файлов и комплектации общего архива приведены
[в конце handoff](verification/2026-10-18-nearest-turn-windows-handoff.md#комплектация-и-загрузка-в-draft).

## Linux

Из `linux/`:

```bash
export Q2DZ_WEBRTC_NET_RELEASE_DIR=/path/to/pinned/webrtc-release
export Q2DZ_WEBRTC_NET_SOURCE_DIR=/path/to/WebRTC-NET
tools/run-sanitizers.sh
tools/check-reproducible.sh
tests/package_smoke.sh
```

`tests/package_smoke.sh` вызывает `build-package.sh`, который выполняет полный
`build-release.sh`; отдельные повторы этих сборок не требуются. Установить
`TMPDIR` на каталог evidence вне checkout; smoke сохраняет там готовый архив.

`Q2DZ_WEBRTC_NET_RELEASE_DIR` supplies the matching headers and shared
libraries. `Q2DZ_WEBRTC_NET_SOURCE_DIR` supplies only the upstream license and
notice files copied by `tools/build-package.sh`. The authoritative WebRTC-NET
source is the public repository
`https://github.com/doszoner/WebRTC-NET.git` at immutable commit
`62fe52cd20bae55baf03339afee60c895f1aee90`; a tag is not required provenance.

Точный toolchain и runtime prerequisites описаны в
[linux/README.md](../linux/README.md); Windows dependency pins описаны в
[windows/README.md](../windows/README.md).

## Перед публикацией

Оставшиеся rc15 gates ведутся в
[remaining release gates](verification/2026-09-05-remaining-release-gates.md).
Интерактивные проверки выполняются по
[exact Q2PRO-X 40-source runbook](verification/2026-09-05-exact-q2pro-x-40-source-runbook.md)
и [live gameplay gates runbook](verification/2026-09-05-live-gameplay-gates-runbook.md).

- тесты и package smoke должны пройти из чисто распакованного архива;
- Q2PRO-X должен войти в реальную комнату; движение/стрельба должны быть видны
  владельцу;
- закрытие комнаты и аварийное завершение не должны оставить orphan-процессы;
- в hub-режиме закрытие одной комнаты не должно завершить Q2PRO-X или другой
  gateway; после этого должен пройти вход во вторую комнату;
- одновременно должны быть проверены обычные `q2` и dedicated `q2ded` строки;
  полный список не должен молча усекаться, а превышение `256` targets должно
  завершаться явной ошибкой;
- archive/args/env/logs не должны содержать room credentials;
- должны быть опубликованы SHA-256 и все third-party licenses/notices.

Unsigned Windows candidates допустимы для проверки корректности в
закрытой alpha-группе с явным предупреждением. До широкого публичного выпуска
необходимо выполнить Authenticode gate и заново проверить exact release archives.

Пока эти условия не выполнены для конкретного архива, он остаётся
кандидатом для проверки и не считается готовой пользовательской поставкой.
