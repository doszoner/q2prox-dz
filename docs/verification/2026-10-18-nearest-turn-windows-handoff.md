# Передача Windows release 1.0.0-rc18 и проверки nearest TURN

## Начать здесь: Windows из того же release source

Linux `1.0.0-rc18` собран и прошёл package/build gates; exact archive gameplay
не запускался, статус candidate. Человек завершает
только Windows portable/setup и общую комплектацию выпуска. Windows VM на
этой машине отсутствует; Windows здесь не собираем и не проверяем.

1. В существующем GitHub Draft rc18 взять **полный source SHA из release notes
   и соответствующего release tag**. Получить чистый checkout именно этого SHA;
   проверить `git rev-parse HEAD` и VERSION `1.0.0-rc18`. Если tag/notes ещё нет
   или SHA расходятся, получить их у владельца draft до сборки. Новый номер
   не выбирать, VERSION не менять, Linux не пересобирать. Итоги и SHA Linux — в
   [ведомости rc18](1.0.0-rc18.md). Release source SHA:
   `ec4a2f6e66e75090c6c379239246be54bbed995e`; tag/notes должны подтверждать его.
2. Подготовить Windows dependencies строго по [deps.lock.json](../../deps.lock.json):
   WebView2 NuGet `1.0.4129.50` и WebRTC-NET commit
   `62fe52cd20bae55baf03339afee60c895f1aee90` с locked submodules и release files.
   Выполнить canonical `windows/product/build-payload.ps1` из
   [RELEASE.md](../RELEASE.md#windows), включая встроенные dependency, CTest/Node,
   `/Brepro` и fresh-extract real-PE smoke gates. Локальные пути и `/tmp`
   предыдущего исполнителя не нужны; зависимости подготовить на своей машине.
3. Выполнить Windows PowerShell **5.1** suites и live задачи **1.3, 2.4, 3.4**
   по checklist ниже. Проверить именно новые portable/setup после распаковки;
   source launcher проверяется дополнительно. Записать Windows/PowerShell/toolchain,
   source SHA, exit codes и фактические результаты; незакрытые задачи оставить открытыми.
4. Получить `q2prox-dz-1.0.0-rc18-windows-x64-portable.zip` и
   `q2prox-dz-1.0.0-rc18-windows-x64-setup.zip` с соседними `.sha256`.
   Проверить VERSION, внутренний SHA256SUMS.txt, licenses/allowlists и fresh-extract
   smoke, записать реальные SHA-256 в ведомость и notes. Старые rc17 не подмешивать.
5. Загрузить Windows ZIP и sidecars в **существующий Draft rc18**, сохранив
   Linux tar.gz и его SHA-256. Только после готовности обеих Windows поставок
   собрать `q2prox-dz-alpha-1.0.0-rc18-<YYYYMMDD>.zip` из всех трёх поставок
   **этого source commit**, sidecars, инструкций и changelog; добавить SHA-256
   общего архива и обновить draft notes/ведомость. Linux-only alpha не создавать.

Unsigned Windows candidates допустимы только в закрытой alpha-группе с явным
предупреждением. До широкого публичного выпуска нужен Authenticode gate и
повторная проверка exact release archives. Публикация/latest здесь не разрешены;
критерии выпуска определяет [RELEASE.md](../RELEASE.md).

Linux nearest TURN ранее проверен из исходников; это **не gameplay-приёмка
нового Linux архива**. Windows PowerShell 5.1/live ожидаются. Серверные TURN
allocations не измерены, защита общей квоты не реализована.

## Продолжение: Windows checklist nearest TURN

Состояние на 2026-10-18: задачи **1.3, 2.4 и 3.4** пока открыты. Реализация
находится в коммите `2cf4173`, результаты Linux отражены в `4b67b6e`;
они входят в release source commit rc18. Для продолжения нужен checkout
именно этого commit с этим документом. Локальные файлы предыдущего исполнителя не требуются.

Контекст: [proposal](../../openspec/changes/q2prox-nearest-turn/proposal.md),
[design](../../openspec/changes/q2prox-nearest-turn/design.md),
[контракт](../../openspec/changes/q2prox-nearest-turn/specs/q2prox-ice-selection/spec.md),
[задачи](../../openspec/changes/q2prox-nearest-turn/tasks.md),
[ограничения TURN](../TURN-RESOURCE-POLICY.md).

## Что сделано и где граница проверки

Лаунчеры выбирают основной RTC-сервер по медиане TCP-connect к порту 3478
и добавляют только TURN/TURNS URL резервного. Измерения ограничены по времени,
кэш хранит domain, credentials берутся из свежего ответа. Обновление credentials
не должно пересоздавать активный target. Транспорт шлюза не менялся.
Сокращение endpoints не ограничивает общую квоту TURN; connection caps
и recovery группы D в эту задачу не входят.

- Python 3.14.7: 53 теста, один существующий skip из-за отсутствия собранного
  native gateway. PowerShell Core 7.6.5 на Linux: 88 общих cases, проверки
  deadline/cache/refresh/heartbeat и launcher stdin прошли.
- Обычный Hub и Linux source launcher: Custom, ручной Refresh, вход в собственную
  комнату, q2dm1 → q2dm2 без смены PID/start ticks клиента, штатный игровой Quit.
  В журнале — `q2-normal-exit` и `coordinator_exit rc=0`. Комната, процессы
  проверки и временные файлы убраны. Проверка шла на отдельном Xvfb.
- Windows PowerShell 5.1 и Windows live **не запускались**. Core на Linux
  не подтверждает Windows. Принудительной live-ротации credentials не было;
  соответствующий контракт проверен компонентно. Серверные allocations
  не измерены: доступа к операторским метрикам нет. Gameplay принимает человек.

## 1. Компонентные проверки: задачи 1.3 и 2.4

Из корня checkout в PowerShell выполнить один раз оба suite именно под
Windows PowerShell 5.1. Они не запускают игру. Записать `git rev-parse HEAD`,
версию Windows, версию PowerShell и код завершения каждого запуска:

```powershell
git rev-parse HEAD
& "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -Command '$PSVersionTable.PSVersion.ToString()'
& "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -File .\windows\product\tests\catalog.tests.ps1
"catalog exit code: $LASTEXITCODE"
& "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -File .\windows\product\tests\launcher-stdin.tests.ps1
"launcher-stdin exit code: $LASTEXITCODE"
```

Ожидается версия 5.1 и два кода 0. Общие fixtures проверяют порядок/состав ICE,
selection/cache/refresh/heartbeat. Deadline включает DNS, очередь и запуск
worker; зависший DNS и полный stdin pipe не должны удерживать launcher.
Завершённые samples сохраняются, worker завершается без накопления процессов.
В stdin suite также проверяются UTF-8, EOF и exit codes.

## 2. Запуск изменённого исходника: задача 3.4

Нужны Windows 10/11 x64, Windows PowerShell 5.1, WebView2 Runtime,
установленный Q2PRO-X с игровыми данными и установленное дополнение Dos.Zone.
[Установка](../../INSTALLATION.md) и [состав поставки](../RELEASE.md).
Сначала установить новый setup rc18 из release source SHA выше; проверить
`VERSION` установленной копии `1.0.0-rc18`. Старые native-компоненты rc17
для этой передачи не использовать.
В активной версии дополнения должны быть `q2dz-session.exe`,
`q2dz-content-check.exe`, `gateway/q2-doszone-webview2-gateway.exe` и его
комплектные `WebView2Loader.dll`/`assets`. Их совместимость проверяется на месте.

Обычный установленный ярлык запускает установленную копию launcher, а не
изменённый исходник. Для проверки checkout есть внутренний способ: dot-source
[launcher](../../windows/product/q2dz-launch.ps1), затем задать `$app` на каталог
установленных native-компонентов и вызвать `Start-Q2DzLauncher`. Это не публичный
CLI; способ сверен с исходниками, но ещё не выполнен на Windows.

Закрыть свою игру и подготовить отдельную writable-копию каталога Q2PRO-X
с игровыми данными: конфиги и игровые логи пишутся в выбранное дерево игры.
Запустить следующую последовательность из корня checkout в отдельной
интерактивной консоли Windows PowerShell 5.1. В диалоге выбрать `Q2PRO-X.exe`
из подготовленной копии. Временный `LOCALAPPDATA` изолирует состояние дополнения,
но сам по себе не изолирует файлы игры.

```powershell
$sourceApp = (Resolve-Path .\windows\product).Path
$originalLocalAppData = $env:LOCALAPPDATA
$releaseRoot = Join-Path $originalLocalAppData 'Q2PRO-X Dos.Zone'
$pointer = Get-Content -LiteralPath (Join-Path $releaseRoot 'current.json') -Raw | ConvertFrom-Json
$installedApp = Join-Path $releaseRoot ([string]$pointer.directory -replace '/', '\')
$testStateRoot = Join-Path $env:TEMP ('q2prox-nearest-turn-' + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Path $testStateRoot | Out-Null
try {
    $env:LOCALAPPDATA = $testStateRoot
    . (Join-Path $sourceApp 'q2dz-launch.ps1')
    $app = $installedApp
    $launcherExitCode = Start-Q2DzLauncher
    "source launcher exit code: $launcherExitCode"
} finally {
    $env:LOCALAPPDATA = $originalLocalAppData
}
"test state directory: $testStateRoot"
```

Путь к native-компонентам определяется до подмены `LOCALAPPDATA`. Launcher
подгружает `catalog.ps1` из checkout; `VERSION` и native-компоненты берёт
из `$installedApp`. Поэтому версия в SESSION log сама по себе не подтверждает
проверку исходников: отдельно сохранить HEAD и версию установленного дополнения.
У launcher нет флагов `-Refresh`, `-Join`, `-Map` или `-Exit`.

## 3. Живой сценарий и завершение

Использовать обычный multiplayer Hub с origin `https://test.js-dos.com:443`
и собственную тестовую комнату. При локальной проверке Hub сохранять этот
origin и брать штатный клиент; продуктовая правка здесь — launcher.
Если Cloud Create/Join отказал, записать ошибку, не подменять Cloud заглушкой.

1. В Q2PRO-X открыть `Join online → Custom`, проверить список и ручной `Refresh`.
   Launcher обновляет каталог каждые 15 секунд.
2. Войти в свою комнату; подтвердить игрока со стороны хозяина.
3. Сменить карту штатным способом владельца комнаты, например `gamemap q2dm2`.
   Убедиться, что новая карта появилась в native-клиенте и процесс клиента
   сохранился. Чужие комнаты не менять.
4. Проверить обновление ICE без DROP/нового active target ID: обычные heartbeat
   PUT и новая ICE generation допустимы. Если реальной ротации credentials
   не было, так и записать; компонентный результат не называть live-ротацией.
5. Выйти через игровой Quit. Проверить завершение своих launcher, coordinator,
   gateway/WebView2 и worker. Закрыть тестовую комнату через Exit в Hub
   и убедиться, что она исчезла. Чужие процессы не завершать.
6. Сохранить безопасную сводку, затем удалить созданные этой проверкой
   временное состояние и копию игры. Не удалять установленный продукт.

В Linux-проверке Hub перехватывал клавишу P: помогла штатная настройка
Chat hotkeys → No. Это подсказка для повторения, не требование менять настройки
всех пользователей. Фоновые проверки не должны захватывать рабочий стол,
клавиатуру или звук человека; при ручной проверке человек управляет сам.

## Что передать обратно

В сводке указать HEAD, Windows/PowerShell и версию native-компонентов, два
exit codes, результаты шагов живого сценария и очистки. Если что-то не достигнуто,
указать шаг и причину; оставить соответствующий пункт 1.3/2.4/3.4 открытым
в [tasks.md](../../openspec/changes/q2prox-nearest-turn/tasks.md).
Достаточно безопасных итоговых строк: не публиковать raw ICE, SDP, candidates,
credentials, cookies и полные join URL. Сырые evidence держать вне Git.

После проверки обновить статусы в OpenSpec и краткую сводку в документации.
Не архивировать change до закрытия Windows-пунктов. Повторять завершённые Linux-проверки исходников и пересобирать Linux не требуется.
Windows release archives проходят собственные gates по RELEASE.md. Загрузить их
в существующий Draft rc18; публичная/latest публикация в эту передачу не входит.
