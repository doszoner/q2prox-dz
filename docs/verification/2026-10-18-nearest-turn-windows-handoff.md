# Передача Windows release 1.0.0-rc18 и проверки nearest TURN

## Начать здесь: Windows из того же release source

Читать актуальный handoff из `master`: документация дополняется после сборки.
Исходники для сборки брать строго из тега `v1.0.0-rc18`, как показано ниже.
На [странице Releases](https://github.com/doszoner/q2prox-dz/releases) открыть
Draft `v1.0.0-rc18`. Он доступен участнику с правом записи в репозиторий;
публичного выпуска пока нет.

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

## Подготовка новой Windows-машины

Нужны Windows 10/11 x64 и:

- Visual Studio **2022** или Build Tools **2022**: workload «Desktop development
  with C++», MSVC **v143 x64/x86**, Windows 10/11 SDK. Список компонентов —
  [у Microsoft](https://learn.microsoft.com/en-us/visualstudio/install/workload-component-id-vs-build-tools?view=vs-2022).
  Одной установки более новой Visual Studio недостаточно: builder выбирает 2022.
- Git for Windows, CMake **3.24 или новее**, Python 3 и Node.js с `node --test`
  (например, Node 22). Команды `git`, `cmake`, `ctest`, `python`, `node` должны
  быть в PATH. Python должен запускать настоящий интерпретатор, а не Microsoft Store.
- GitHub CLI `gh`, авторизованный для работы с Draft этого репозитория.
  Если у исполнителя нет доступа к Draft, владелец должен выдать доступ или
  передать архив зависимостей и Linux-поставку; публичная ссылка Draft не поможет.
- Для smoke/live: WebView2 **Evergreen Runtime x64** (это не SDK NuGet),
  Q2PRO-X и игровые данные по [INSTALLATION.md](../../INSTALLATION.md).

Далее использовать **Windows PowerShell 5.1**, не PowerShell 7.
После установки инструментов открыть новую консоль. Примеры рассчитаны на
новый каталог; повторные попытки выполнять в новом каталоге, не поверх результатов.

```powershell
$ErrorActionPreference = 'Stop'
$PSVersionTable.PSVersion
git --version
cmake --version
ctest --version
python --version
node --version
gh auth status

$workRoot = Join-Path $env:USERPROFILE 'q2prox-rc18'
New-Item -ItemType Directory $workRoot | Out-Null
Set-Location $workRoot
git clone -c core.autocrlf=false https://github.com/doszoner/q2prox-dz.git source
if ($LASTEXITCODE -ne 0) { throw 'Clone q2prox-dz failed' }
Set-Location source
git switch --detach v1.0.0-rc18
if ($LASTEXITCODE -ne 0) { throw 'Release checkout failed' }
if ((git rev-parse HEAD) -ne 'ec4a2f6e66e75090c6c379239246be54bbed995e') { throw 'Wrong source SHA' }
if ((Get-Content VERSION -Raw).Trim() -ne '1.0.0-rc18') { throw 'Wrong VERSION' }
$sourceRoot = (Get-Location).Path
$deps = Join-Path $workRoot 'deps'
New-Item -ItemType Directory $deps | Out-Null
```

### Получение зависимостей

В Draft приложен `q2prox-dz-1.0.0-rc18-windows-build-inputs.zip` с двумя
готовыми файлами `webrtcnet.mjs` / `webrtcnet.wasm` и лицензиями. Они извлечены
из сохранённого rc17 portable и **побайтово совпадают с rc18 deps.lock.json**.
Это повторное использование неизменившихся зависимостей; Windows executable
rc18 будут собраны заново. SHA-256 архива:
`f0eb53bbf8540f1430c6aafff89f8c271a2c893e15b5c7fc6655ab38e88d6b9e`.

Продолжить в той же консоли из `$sourceRoot`:

```powershell
gh release download v1.0.0-rc18 --repo doszoner/q2prox-dz `
  --pattern 'q2prox-dz-1.0.0-rc18-windows-build-inputs.zip*' --dir $deps
if ($LASTEXITCODE -ne 0) { throw 'Dependency download failed; check Draft access' }
$inputsZip = Join-Path $deps 'q2prox-dz-1.0.0-rc18-windows-build-inputs.zip'
if ((Get-FileHash $inputsZip -Algorithm SHA256).Hash.ToLowerInvariant() -ne 'f0eb53bbf8540f1430c6aafff89f8c271a2c893e15b5c7fc6655ab38e88d6b9e') { throw 'Build-inputs hash mismatch' }
Expand-Archive -LiteralPath $inputsZip -DestinationPath (Join-Path $deps 'inputs')
$webrtcRelease = Join-Path $deps 'inputs\webrtc-release'
$lock = Get-Content .\deps.lock.json -Raw | ConvertFrom-Json
foreach ($entry in $lock.webrtcNet.windows.PSObject.Properties) {
  $actual = (Get-FileHash (Join-Path $webrtcRelease $entry.Name) -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($actual -ne $entry.Value) { throw "Dependency hash mismatch: $($entry.Name)" }
}
$sdkPackage = Join-Path $deps 'Microsoft.Web.WebView2.1.0.4129.50.nupkg'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
Invoke-WebRequest -UseBasicParsing -Uri $lock.webView2.source -OutFile $sdkPackage

$webrtcSource = Join-Path $deps 'WebRTC-NET'
git clone -c core.autocrlf=false $lock.webrtcNet.source $webrtcSource
if ($LASTEXITCODE -ne 0) { throw 'Clone WebRTC-NET failed' }
git -C $webrtcSource switch --detach $lock.webrtcNet.commit
if ($LASTEXITCODE -ne 0) { throw 'WebRTC-NET checkout failed' }
git -C $webrtcSource -c core.autocrlf=false -c 'url.https://github.com/.insteadOf=git@github.com:' submodule update --init
if ($LASTEXITCODE -ne 0) { throw 'WebRTC-NET submodules failed' }
python .\tools\verify-dependencies.py --lock .\deps.lock.json `
  --webrtc-source $webrtcSource --webview2-package $sdkPackage
if ($LASTEXITCODE -ne 0) { throw 'Dependency verification failed; do not change lockfile' }
```

HTTPS-подмена действует только на одну команду submodules: upstream использует
SSH URL. `core.autocrlf=false` сохраняет байты исходных лицензий для verifier.
Команды проверяют NuGet, оба JS/WASM, commit, submodules и лицензии. Полную
проверку с WebView2Loader.dll выполняет builder после распаковки NuGet. При несовпадении
хешей не обновлять lockfile и не подбирать «похожую» версию.

### Сборка Windows

Из `$sourceRoot`, в той же консоли:

```powershell
$windowsOut = Join-Path $workRoot 'windows-output'
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\windows\product\build-payload.ps1 `
  -WebView2Package $sdkPackage -WebRtcNetSourceDir $webrtcSource `
  -WebRtcNetReleaseDir $webrtcRelease -OutputDir $windowsOut
if ($LASTEXITCODE -ne 0) { throw 'Windows release build failed' }
```

Ожидается `WINDOWS RELEASE PASS` и ровно два ZIP с двумя `.sha256`.
Builder уже выполняет component suites; успешные прогоны под 5.1 можно записать
в checklist ниже без повторения. Затем обязательны ручные проверки новых
portable/setup и live-сценарий. Ниже также есть отдельные команды suites для
диагностики. Не загружать артефакты после неуспешного builder.

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

## Комплектация и загрузка в Draft

Выполнять только после успешной сборки и ручных проверок выше. Команды продолжают
ту же PowerShell-консоль (`$workRoot`, `$sourceRoot`, `$windowsOut`). Если консоль
закрыта, задать эти три пути заново по фактическим каталогам. Linux берётся из
Draft без пересборки. В общий архив входят три поставки, их sidecars, инструкция,
changelog и manifest; build inputs, исходники и сырые журналы туда не входят.

```powershell
Set-Location $sourceRoot
$releaseRepo = 'doszoner/q2prox-dz'
$tag = 'v1.0.0-rc18'
$draft = gh release view $tag --repo $releaseRepo --json isDraft,tagName | ConvertFrom-Json
if ($LASTEXITCODE -ne 0 -or -not $draft.isDraft -or $draft.tagName -ne $tag) { throw 'Expected existing rc18 Draft' }
$stage = Join-Path $workRoot 'alpha-stage'
New-Item -ItemType Directory $stage | Out-Null
$platformFiles = @(
  'q2prox-dz-1.0.0-rc18-linux-x86_64.tar.gz',
  'q2prox-dz-1.0.0-rc18-windows-x64-portable.zip',
  'q2prox-dz-1.0.0-rc18-windows-x64-setup.zip'
)
gh release download $tag --repo $releaseRepo --pattern "$($platformFiles[0])*" --dir $stage
if ($LASTEXITCODE -ne 0) { throw 'Linux download failed' }
foreach ($name in $platformFiles[1..2]) {
  Copy-Item -LiteralPath (Join-Path $windowsOut $name) -Destination $stage
  Copy-Item -LiteralPath (Join-Path $windowsOut "$name.sha256") -Destination $stage
}
foreach ($name in $platformFiles) {
  $file = Join-Path $stage $name
  $expected = ((Get-Content -LiteralPath "$file.sha256" -Raw).Trim() -split '\s+')[0]
  $actual = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLowerInvariant()
  if ($expected -notmatch '^[0-9a-fA-F]{64}$' -or $actual -ne $expected) { throw "Hash mismatch: $name" }
  if ($name -eq $platformFiles[0] -and $actual -ne 'e700aa7b121fbb3bef3dedbf3b554c595e8307e54dc09acbb1d8b91a13ed6ee9') { throw 'Wrong Linux candidate' }
}
Copy-Item -LiteralPath .\CHANGELOG.md -Destination $stage
@'
# q2prox-dz 1.0.0-rc18 — closed alpha candidate

Source: ec4a2f6e66e75090c6c379239246be54bbed995e
Contains three platform packages and their SHA-256 sidecars.
Choose Linux tar.gz, Windows portable ZIP, or Windows setup ZIP.
Extract that package and follow its included README.
Quake II, Q2PRO-X and game data are not included.
Windows is unsigned; this candidate is for the closed alpha group.
Linux exact-archive gameplay is still pending. See CHANGELOG.md for changes.
Verification: https://github.com/doszoner/q2prox-dz/blob/master/docs/verification/1.0.0-rc18.md
'@ | Set-Content -LiteralPath (Join-Path $stage 'README-FIRST.md') -Encoding UTF8
$manifest = Get-ChildItem -LiteralPath $stage -File | Sort-Object Name | ForEach-Object {
  '{0}  {1}' -f (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant(), $_.Name
}
$manifest | Set-Content -LiteralPath (Join-Path $stage 'SHA256SUMS.txt') -Encoding ASCII
$alpha = Join-Path $workRoot ('q2prox-dz-alpha-1.0.0-rc18-' + (Get-Date -Format yyyyMMdd) + '.zip')
Compress-Archive -Path (Join-Path $stage '*') -DestinationPath $alpha
$alphaHash = (Get-FileHash -LiteralPath $alpha -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText("$alpha.sha256", "$alphaHash  $([IO.Path]::GetFileName($alpha))`n", [Text.UTF8Encoding]::new($false))
$uploads = @($alpha, "$alpha.sha256")
foreach ($name in $platformFiles[1..2]) {
  $uploads += (Join-Path $stage $name)
  $uploads += (Join-Path $stage "$name.sha256")
}
gh release upload $tag --repo $releaseRepo @uploads
if ($LASTEXITCODE -ne 0) { throw 'Upload incomplete; inspect Draft assets before retry' }
gh release view $tag --repo $releaseRepo --json isDraft,assets
```

Ожидается шесть новых assets: два Windows ZIP, общий alpha и три sidecars.
Linux и архив build inputs должны остаться. Команда не перезаписывает одноимённые
файлы: при частичной загрузке проверить Draft и загрузить только отсутствующие.
Сверить SHA-256 assets с локальными файлами, обновить release notes и ведомость
фактическими Windows-результатами. Сохранить `Draft` и `Pre-release`, кнопку
публичного выпуска не нажимать. Linux gameplay остаётся открытым пунктом.

Изменения отчёта оформлять отдельной веткой от актуального `master`; сборочный
checkout и тег `v1.0.0-rc18` не менять. Этот handoff дополняется независимо от
исходников сборки, поэтому в самом теге его версия может быть старее.
