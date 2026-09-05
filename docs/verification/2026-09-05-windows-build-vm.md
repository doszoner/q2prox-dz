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

## Старые snapshot

Найденные старые точки этой VM:

```text
clean-installed-20260822
clean-windows-25h2-pre-product-20260822
q2prox-dz-clean-runtime-20260830
guest-additions-ready-20260905
```

Они оставлены как цепочка отката основной VM. Удалять их только отдельным
шагом после проверки, что `q2prox-build-env-ready-20260905` стартует и больше
не нужен откат к чистому runtime/доинструментальному состоянию.
