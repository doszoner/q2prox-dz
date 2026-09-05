# Windows build VM, 2026-09-05

Назначение: готовая локальная Windows-точка для быстрых headless-проверок
q2prox-dz без перехвата GUI, мыши, клавиатуры и звука.

## VM

```text
name: Windows 11 25H2 + LTSC (x64) 28in1
uuid: 9a57e185-2b98-458b-b58f-c9e23e666ce9
audio: disabled / backend none
guest user: user
```

Пароль гостевого пользователя не записывать в команды, логи, документы или
репозиторий. Для `VBoxManage guestcontrol` использовать временный файл
`--passwordfile`, удаляемый сразу после команды.

## Что подготовлено внутри Windows

- Guest Additions и `VBoxManage guestcontrol`.
- Visual Studio 2022 Build Tools / MSVC x64.
- Windows SDK.
- CMake, Node.js, Python 3, Git.
- WebView2 SDK package:
  `C:\Users\user\q2prox-work\rc14-20260905-104104\q2prox-windows-rc14-input-20260905-104023\Microsoft.Web.WebView2.1.0.4129.50.nupkg`.
- WebRTC-NET q2prox-shared source:
  `C:\Users\user\q2prox-work\rc14-20260905-104104\WebRTC-NET-q2prox-shared`.
- WebRTC-NET q2prox-shared Windows runtime artifacts:
  `C:\Users\user\q2prox-work\rc14-20260905-104104\WebRTC-NET-q2prox-shared-release-windows`.

## Проверенная команда сборки

Запускать через `VBoxManage guestcontrol ... powershell.exe -EncodedCommand`,
без интерактивного окна:

```powershell
& "C:\Users\user\q2prox-work\rc14-20260905-104104\q2prox-dz\windows\product\build-payload.ps1" `
  -OutputDir "C:\Users\user\q2prox-work\rc14-output-YYYYMMDD-HHMMSS" `
  -WebRtcNetSourceDir "C:\Users\user\q2prox-work\rc14-20260905-104104\WebRTC-NET-q2prox-shared" `
  -WebRtcNetReleaseDir "C:\Users\user\q2prox-work\rc14-20260905-104104\WebRTC-NET-q2prox-shared-release-windows" `
  -WebView2Package "C:\Users\user\q2prox-work\rc14-20260905-104104\q2prox-windows-rc14-input-20260905-104023\Microsoft.Web.WebView2.1.0.4129.50.nupkg"
```

Перед новым прогоном каталог `-OutputDir` должен отсутствовать или быть пустым.

## Evidence последнего headless-прогона

```text
date: 2026-09-05
output: C:\Users\user\q2prox-work\rc14-output-20260905-snapshot2

dependency verification: PASS
coordinator build and tests: PASS
windows dynamic coordinator matrix: PASS
gateway build and tests: PASS
gateway Node protocol tests: PASS
catalog contract tests: PASS
install/uninstall contract: PASS
launcher stdin contract: PASS
BREPRO: PASS
release real-payload smoke: PASS
WINDOWS RELEASE PASS
```

Скопированные на host артефакты:

```text
/home/ssolovey/Projects/Q2/DosZone/q2prox-dz-dedicated-compat/dist/q2prox-dz-1.0.0-rc14-windows-x64-portable.zip
SHA-256: bcfd29076621d00e51a434e231baa1e05aa6940d4f9cbfd3320c8eb67dca7a50

/home/ssolovey/Projects/Q2/DosZone/q2prox-dz-dedicated-compat/dist/q2prox-dz-1.0.0-rc14-windows-x64-setup.zip
SHA-256: 2263ac2bcf6f197041f095eb65f034db9e139a7fd5507f592b99498362cf3d2b
```

## Snapshot

Актуальная стартовая точка хранит окружение, а не конкретный смысл текущего
кода. Код в `C:\Users\user\q2prox-work\...\q2prox-dz` нужно заменить новым
checkout или свежей копией перед следующим прогоном.

```text
snapshot: q2prox-build-env-ready-20260905
uuid: 4baf70e7-c6f0-47ee-ac02-9d14130de3a3
purpose: GuestControl + Windows build toolchain + locked local dependencies
current: yes
vm state after preparation: poweroff
```

Использование:

```bash
VBoxManage snapshot 9a57e185-2b98-458b-b58f-c9e23e666ce9 restore q2prox-build-env-ready-20260905
VBoxManage startvm 9a57e185-2b98-458b-b58f-c9e23e666ce9 --type headless
```

Восстановление snapshot отбрасывает текущее состояние VM после snapshot. Перед
restore сохранить нужные логи и артефакты на host.

## Cleanup 2026-09-05

Удалены только явно мёртвые VirtualBox-объекты:

```text
deleted VM: Windows.11.28000.21.13.MR.Bumerino
deleted folder: /home/ssolovey/VirtualBox VMs/Windows 11 25H2 _ LTSC (x64) 28in1 Clone
closed stale medium: /home/ssolovey/VirtualBox VMs/Windows/Windows_1.vdi
```

После очистки зарегистрирована только рабочая VM:

```text
Windows 11 25H2 + LTSC (x64) 28in1
9a57e185-2b98-458b-b58f-c9e23e666ce9
```

## Snapshot smoke 2026-09-05

Проверено после cleanup:

```text
VBoxManage list runningvms: empty before start
VBoxManage startvm 9a57e185-2b98-458b-b58f-c9e23e666ce9 --type headless: PASS
Guest Additions visible: 7.2.16
GuestControl cmd.exe /c ver: PASS
Windows version: 10.0.26200.9168
audio: none
```

`acpipowerbutton` не выключил гостя за ограниченное окно ожидания, поэтому
конкретно этот unattended workflow не должен полагаться на ACPI shutdown как на
единственный cleanup. VM была выключена командой `VBoxManage controlvm ...
poweroff`; итоговое состояние — `poweroff`.

## Runtime preflight 2026-09-05

Проверено через headless VM и GuestControl, без host keyboard/mouse injection.

```text
GuestControl cmd.exe /c ver: PASS
Windows version: 10.0.26200.9168
WebView2 Runtime: present, 152.0.4191.62
C: free space: 31.93 GiB
interactive user session: none (`query user`: No User exists)
Q2PRO-X.exe under C:\Users\user: not found
saved q2 path:
  C:\Users\user\AppData\Local\Temp\q2dz-rc11-clean-979c25a91cb9462a918f361091f8c869\q2\Q2PRO-X.exe
  stale / not a usable rc14 live runtime path
```

Host-side available inputs:

```text
/home/ssolovey/Downloads/q2pro-x-1.5.rar
/home/ssolovey/Downloads/q2pro-x-1/
/home/ssolovey/Games/Quake2-PlayGround-2026/
/home/ssolovey/Downloads/Telegram Desktop/Quake2 [Sergh's Edition, 2026].rar
```

Следствие: этот snapshot остаётся хорошей build/headless-test стартовой точкой,
но Windows Q2PRO-X/WebView2 gameplay gate на нём ещё не готов к запуску. Для
live runtime нужен отдельный подготовительный шаг: положить в VM актуальный
Q2PRO-X/game-data tree, сбросить stale `q2-path.txt`, войти в пользовательскую
GUI-сессию или подготовить отдельный runtime snapshot с интерактивной сессией.

Guest `shutdown.exe /s /t 0` через GuestControl в этом прогоне показал help и не
выключил VM; финальный cleanup снова выполнен `VBoxManage controlvm ... poweroff`.

## Runtime snapshot 2026-09-05

Подготовлен отдельный snapshot для Windows live gameplay gate.

```text
snapshot: q2prox-runtime-ready-20260905
uuid: f00c01fd-dbb8-4215-ad33-53d3b1fe9ab0
parent/current previous point: q2prox-build-env-ready-20260905
vm state after snapshot: poweroff
```

В VM добавлен runtime tree:

```text
C:\Users\user\q2-runtime-20260905\Quake2-PlayGround-2026\Q2PRO-X.exe
SHA-256: 831f42dc96e3399a8a8737943d6fde752e11f5e5f48ffc5957537c4e66cbf71d
```

`q2-path.txt` установлен на этот путь:

```text
C:\Users\user\AppData\Local\Q2PRO-X Dos.Zone\state\q2-path.txt
```

Runtime ZIP был временно собран на host из
`/home/ssolovey/Games/Quake2-PlayGround-2026/`, без `logs`, `screenshots` и
crash/log files, передан в VM, проверен по SHA-256 и удалён из VM и `/tmp`.

Граница доказанного: это только подготовка runtime-окружения. Live gate всё ещё
требует интерактивной пользовательской GUI-сессии в Windows и не считается
пройденным.

Использование:

```bash
VBoxManage snapshot 9a57e185-2b98-458b-b58f-c9e23e666ce9 restore q2prox-runtime-ready-20260905
VBoxManage startvm 9a57e185-2b98-458b-b58f-c9e23e666ce9 --type headless
```

## RC14 live-ready snapshot 2026-09-05

Подготовлен следующий snapshot: runtime tree плюс exact rc14 Windows
поставки, уже разложенные для ручного live gate.

```text
snapshot: q2prox-rc14-live-ready-20260905
uuid: 8110ebf4-1666-427c-82a9-458ee95d297b
parent/current previous point: q2prox-runtime-ready-20260905
vm state after snapshot: poweroff
```

В VM находятся:

```text
C:\Users\user\q2dz-release\rc14\input\
  q2prox-dz-1.0.0-rc14-windows-x64-portable.zip
  q2prox-dz-1.0.0-rc14-windows-x64-portable.zip.sha256
  q2prox-dz-1.0.0-rc14-windows-x64-setup.zip
  q2prox-dz-1.0.0-rc14-windows-x64-setup.zip.sha256

C:\Users\user\q2dz-release\rc14\portable\Run.cmd
C:\Users\user\q2dz-release\rc14\setup\Install.cmd
C:\Users\user\Desktop\Run Q2PROX-DZ rc14 portable.cmd
```

Проверенные SHA-256 внутри VM:

```text
portable: bcfd29076621d00e51a434e231baa1e05aa6940d4f9cbfd3320c8eb67dca7a50
setup:    2263ac2bcf6f197041f095eb65f034db9e139a7fd5507f592b99498362cf3d2b
```

При подготовке обнаружена особенность `VBoxManage guestcontrol copyto`:
guest destination path должен передаваться в форме `C:/Users/.../file`, иначе
backslash path может не попасть в ожидаемый каталог. Это VM workflow issue, не
изменение продукта.

Использование для следующего Windows live gate:

```bash
VBoxManage snapshot 9a57e185-2b98-458b-b58f-c9e23e666ce9 restore q2prox-rc14-live-ready-20260905
VBoxManage startvm 9a57e185-2b98-458b-b58f-c9e23e666ce9 --type gui
```

После входа в Windows запустить с рабочего стола:

```text
Run Q2PROX-DZ rc14 portable.cmd
```

Граница доказанного: поставка разложена и проверена по SHA-256, но gameplay
live gate ещё не выполнен.

## RC14 setup VM gate 2026-09-05

Проверено через GuestControl, без запуска `Install.cmd`, потому что
`Install.cmd` после установки открывает пользовательский ярлык. Для headless
проверки выполнялись только `install.ps1`, повторный `install.ps1` и
`uninstall.ps1`.

```text
source setup: C:\Users\user\q2dz-release\rc14\setup
first install: PASS
second install/update: PASS
uninstall: PASS
version1: 1.0.0-rc14
version2: 1.0.0-rc14
version directory:
  1.0.0-rc14-296a1fb65fdd9ccf23f02875831c3ce128cd894be4e3e9a51912936f51fa61b4
q2-path preserved:
  C:\Users\user\q2-runtime-20260905\Quake2-PlayGround-2026\Q2PRO-X.exe
product processes after gate: 0
```

После gate VM была выключена и восстановлена к snapshot
`q2prox-rc14-live-ready-20260905`, чтобы оставить чистую стартовую точку для
ручного gameplay live gate.

## Старые snapshot

После cleanup 2026-09-05 у этой VM остались только точки, которые имеют
понятное назначение:

```text
q2prox-dz-clean-runtime-20260830
guest-additions-ready-20260905
q2prox-build-env-ready-20260905
q2prox-runtime-ready-20260905
q2prox-rc14-live-ready-20260905
```

`q2prox-build-env-ready-20260905` — штатная точка для быстрых headless-сборок.
`q2prox-dz-clean-runtime-20260830` и `guest-additions-ready-20260905` оставлены
как минимальный откат к runtime/GuestControl-состоянию, если понадобится чистая
проверка без build tools. `q2prox-runtime-ready-20260905` — ближайшая стартовая
точка для Windows live gameplay gate. `q2prox-rc14-live-ready-20260905` уже
содержит разложенные rc14 portable/setup. Это не отработанные мёртвые образы.

Удалены как отработанные старые rollback-точки:

```text
clean-windows-25h2-pre-product-20260822
clean-installed-20260822
```

Проверка после удаления:

```text
registered VMs: Windows 11 25H2 + LTSC (x64) 28in1 only
running VMs: none
current snapshot: q2prox-build-env-ready-20260905
remaining snapshots: q2prox-dz-clean-runtime-20260830,
  guest-additions-ready-20260905,
  q2prox-build-env-ready-20260905
```

После подготовки runtime snapshot текущая точка изменилась:

```text
current snapshot: q2prox-runtime-ready-20260905
running VMs: none
```

После подготовки rc14 live-ready snapshot текущая точка изменилась:

```text
current snapshot: q2prox-rc14-live-ready-20260905
running VMs: none
```
