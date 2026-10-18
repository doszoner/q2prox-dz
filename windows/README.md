# Windows

Windows-поставка состоит из трёх небольших частей:

- `gateway/` — WebView2 UDP↔WebRTC gateway;
- `coordinator/` — lifecycle, readiness и content preflight;
- `product/` — catalog launcher, portable/setup packaging и пользовательские README.

## Текущий режим

`q2dz-launch.ps1` получает и проверяет каталог Dos.Zone, публикует все
совместимые комнаты в пределах `256` targets и передаёт полный heartbeat
координатору через `stdin`.
Координатор запускает один shared gateway и один неизменённый Q2PRO-X со
стабильным локальным HTTP-источником для штатной вкладки `Custom`. Отдельного
окна выбора комнаты нет.

Launcher опрашивает каталог примерно раз в 15 секунд. Пользователь нажимает
обычную кнопку `Refresh` в `Custom`, чтобы обновить список. Новая комната
появляется после очередного успешного опроса; закрытая исчезает после двух
опросов, в которых её уже нет. Q2PRO-X перезапускать не нужно.
Заполненная активная комната сохраняется.
Ошибка каталога сохраняет текущие комнаты. Ручное редактирование CFG,
Favorites или аргументов запуска не требуется.

При закрытии одного endpoint удаляется только его target; Q2PRO-X и остальные
endpoints продолжают работать. Выход Q2PRO-X или жёсткое завершение владельца
останавливает всю сессию. Отдельный внутренний очиститель ждёт завершения
coordinator и удаляет его session directory даже при жёстком завершении.

Граница компонентов:

- PowerShell: опрос, проверка каталога и полный heartbeat совместимых комнат;
- C++ coordinator: владение Q2PRO-X/gateway и локальный HTTP-источник;
- gateway: общий WebView2 transport runtime с отдельным loopback endpoint на
  target;
- Q2PRO-X: штатные `Custom` и `Refresh`.

## Local development

Единственная release-chain инструкция находится в
[`docs/RELEASE.md`](../docs/RELEASE.md). Для локальной component-сборки нужны
Visual Studio 2022 / Build Tools 2022 с MSVC v143 x64/x86 и Windows SDK,
CMake ≥ 3.24, Python 3, Node.js, WebView2 SDK и locked WebRTC-NET JS/WASM release.
Release-скрипт явно выбирает генератор `Visual Studio 17 2022`.
Подготовка новой Windows-машины, получение зависимостей и команда выпуска rc18 —
в [Windows handoff](../docs/verification/2026-10-18-nearest-turn-windows-handoff.md#подготовка-новой-windows-машины).
Пример отдельной component-сборки:

```powershell
cmake -S .\windows\gateway -B .\windows\gateway\build -A x64 `
  -DWEBVIEW2_SDK_DIR=C:\deps\webview2-sdk-1.0.4129.50 `
  -DWEBRTC_NET_RELEASE_DIR=C:\deps\webrtc-release
cmake --build .\windows\gateway\build --config Release
ctest --test-dir .\windows\gateway\build -C Release --output-on-failure
```

The locked external inputs are WebView2 SDK NuGet package `1.0.4129.50` and the
public WebRTC-NET repository `https://github.com/doszoner/WebRTC-NET.git` at
immutable commit `62fe52cd20bae55baf03339afee60c895f1aee90`; no tag is
required. The WebRTC-NET release directory must contain the matching
`webrtcnet.mjs` and `webrtcnet.wasm` files.

Coordinator:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\windows\coordinator\tools\build-release.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\windows\coordinator\tools\run-lifecycle-matrix.ps1
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\windows\coordinator\tools\check-reproducible.ps1 `
  -WebView2SdkDir C:\deps\webview2-sdk `
  -WebRtcNetReleaseDir C:\deps\webrtc-release
```

Packaging and publication gates are documented only in
[`docs/RELEASE.md`](../docs/RELEASE.md).
