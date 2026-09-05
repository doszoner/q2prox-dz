# Сборка и выпуск

Это единственная нормативная release-chain инструкция проекта. Команды ниже
описывают обязательный путь, но сами по себе не заявляют PASS.
[`1.0.0-rc6`](verification/1.0.0-rc6.md) и
[`1.0.0-rc7`](verification/1.0.0-rc7.md) сохраняют исторические результаты и
последующие field findings. `1.0.0-rc8` сохраняет предыдущий проверенный
rollback. Текущий `1.0.0-rc14` получает PASS только из
[`1.0.0-rc14`](verification/1.0.0-rc14.md), где должны быть зафиксированы
точные результаты компонентных, live и package gates.

Пользователю следует публиковать только три canonical архива и соседние
`.sha256`:

- `q2prox-dz-<version>-windows-x64-portable.zip`;
- `q2prox-dz-<version>-windows-x64-setup.zip`;
- `q2prox-dz-<version>-linux-x86_64.tar.gz`.

## Публичная граница

Package allowlists исключают `input/`, reviewer prompts, work plans, caches,
raw VM evidence, логи, build directories и credentials. Tracked public tree
также исключает временные материалы; утверждённые implementation plans остаются
в `docs/superpowers/plans/`. Локальные untracked/ignored материалы не входят в
пользовательские архивы; отдельный staging framework для этого не нужен.

## Windows

Из корня checkout:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\windows\product\build-payload.ps1 `
  -WebView2Package C:\deps\Microsoft.Web.WebView2.1.0.4129.50.nupkg `
  -WebRtcNetSourceDir C:\deps\WebRTC-NET `
  -WebRtcNetReleaseDir C:\deps\webrtc-release `
  -OutputDir .\dist\windows-1.0.0-rc14
```

Команда проверяет `deps.lock.json`, извлекает SDK из locked NuGet, собирает
coordinator и gateway с нуля, запускает их CTest/Node gates, проверяет
`/Brepro`, создаёт только allowlisted payload и вызывает
fresh-extract real-PE smoke для portable и setup. В output остаются только
два canonical ZIP и два соседних `.sha256`. Внутренний
`package-windows.ps1` не является пользовательским release API. Windows
prerequisites и локальная component-сборка описаны в
[windows/README.md](../windows/README.md).

## Linux

Из `linux/`:

```bash
export Q2DZ_WEBRTC_NET_RELEASE_DIR=/path/to/pinned/webrtc-release
export Q2DZ_WEBRTC_NET_SOURCE_DIR=/path/to/WebRTC-NET
tools/build-release.sh
tools/run-sanitizers.sh
tools/check-reproducible.sh
tools/build-package.sh
tests/package_smoke.sh
```

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

Unsigned Windows candidates допустимы для корректностного тестирования в
закрытой alpha-группе с явным предупреждением. До широкого публичного выпуска
необходимо выполнить Authenticode gate и заново проверить exact release archives.

Пока эти условия не выполнены для конкретного rc14-архива, он остаётся
кандидатом для проверки и не считается готовой пользовательской поставкой.
