# q2prox-dz

`q2prox-dz` позволяет уже установленному **Q2PRO-X** подключаться к публичным
комнатам Quake II на Dos.Zone. Add-on показывает доступные комнаты, поднимает
локальный UDP↔WebRTC gateway и запускает неизменённый Q2PRO-X.

Он не содержит Quake II, Q2PRO-X, карты или PAK-файлы и не изменяет CFG игры.
Комнаты создаются в обычном интерфейсе Dos.Zone.

## Установка

Скачайте подходящий архив со страницы GitHub Releases:

- Windows x64 setup — обычная установка с ярлыком в меню «Пуск»;
- Windows x64 portable — распаковать и запустить без установки;
- Linux x86-64 — пользовательская установка, Q2PRO-X запускается через Wine.

Полные инструкции: [INSTALLATION.md](INSTALLATION.md).

## Требуемый Q2PRO-X

Поддерживаемый клиентский профиль — Q2PRO-X 1.5 by ly, сборка 2026-08-25, или
точно совместимая с ней сборка. Произвольные Quake II forks и другие игры
Dos.Zone этим add-on не поддерживаются.

Если Q2PRO-X отсутствует или устарел:

1. Скачайте выпуск 1.5 с [официальной страницы Q2PRO-X](https://q2pro-x.com/).
2. Распакуйте/установите файлы Q2PRO-X рядом с уже существующим контентом
   Quake II (`baseq2`, `3zb2` и ваши PAK-файлы).
3. При первом запуске add-on выберите установленный `Q2PRO-X.exe`.

При обновлении заменяйте файлы самого Q2PRO-X, но не PAK-файлы и CFG, если
инструкция конкретного upstream-выпуска прямо не требует обратного.

## Как это работает

```text
Dos.Zone room catalog
        ↓
q2prox-dz launcher → local managed gateway → WebRTC → Dos.Zone room
        ↓                         ↑
Q2PRO-X ───── UDP 127.0.0.1 ─────┘
```

Windows использует WebView2, Linux — нативный WebRTC-NET transport. Launcher
публикует до двух loopback-портов во вкладке `Custom` Q2PRO-X: отдельного окна
выбора комнаты нет. Закрытие одной комнаты не завершает игру, и пользователь
может выбрать другую. Launcher владеет и очищает только созданные им процессы.

- [Архитектура](docs/ARCHITECTURE.md)
- [Сборка и выпуск](docs/RELEASE.md)
- [История изменений](CHANGELOG.md)

## Состояние

Lifecycle, content и datagram contracts покрыты source/unit tests.
[Запись rc6](docs/verification/1.0.0-rc6.md) сохраняет исходные исторические
результаты, а [rc7](docs/verification/1.0.0-rc7.md) — корректирующие offline и
Linux gates. В [rc8](docs/verification/1.0.0-rc8.md) Windows A/B/C локализовал
1 Hz cadence в background scheduling невидимого WebView2; выбран и проверен
нативный fix `IsVisible(TRUE)` без Chromium-флагов. Exact package hashes и
пройденные release gates фиксируются в той же записи rc8. Новый hub-режим и его
ещё не пройденные release gates фиксируются отдельно в
[rc9](docs/verification/1.0.0-rc9.md).

Windows RC artifacts не подписаны Authenticode. Это не мешает
корректностному тестированию в закрытой alpha-группе, но Authenticode обязателен
до широкого публичного распространения и доверия. Для каждого публикуемого
архива обязателен соседний SHA-256.

## Лицензии

- исходный код: [MIT](LICENSE);
- документация: [CC BY 4.0](LICENSE-DOCS);
- примеры конфигурации: [CC0 1.0](LICENSE-CONFIGS);
- сторонние компоненты сохраняют собственные лицензии и notices.

Quake II, Q2PRO-X и Dos.Zone являются отдельными проектами и в эту репу не входят.
