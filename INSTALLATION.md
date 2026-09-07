# Установка q2prox-dz

Add-on подключает уже установленный Q2PRO-X к публичным комнатам Quake II на
Dos.Zone. Игра, карты и игровые данные поставляются отдельно.

Общий пользовательский порядок:

```text
Install or unpack q2prox-dz.
Run Q2PRO-X - Dos.Zone.
Open Join online, then Custom.
Use the ordinary Refresh button in Custom to see newly created or closed rooms.
Do not edit Q2PRO-X CFG, Favorites or command-line arguments.
```

Launcher опрашивает каталог примерно раз в 15 секунд и публикует все
совместимые комнаты, которые проходят проверку каталога, content check и
текущий предел `256` targets. Новый endpoint появляется после очередного
успешного опроса, закрытый — после двух успешных опросов, в которых его уже
нет. Заполненная активная комната сохраняется. Если каталог временно
недоступен, текущие комнаты сохраняются.

Запускатель не открывает меню Q2PRO-X принудительно: клиент использует свою
обычную настройку запуска. Если после запуска видна консоль, нажмите `Esc`,
откройте `Join online` → `Custom` и нажмите `Refresh`.

## Если Q2PRO-X ещё не установлен

Скачайте последнюю поддерживаемую версию с официального сайта
<https://q2pro-x.com/>, распакуйте её рядом с принадлежащими вам данными Quake II
и один раз запустите клиент. При обновлении Q2PRO-X используйте тот же
официальный источник. Add-on не скачивает и не обновляет игру.

## Windows setup

1. Скачайте `q2prox-dz-1.0.0-rc15-windows-x64-setup.zip` и полностью
   распакуйте его.
2. Закройте уже запущенный Q2PRO-X и запустите `Install.cmd`.
3. `Install.cmd` сам откроет первый запуск. Укажите в нём установленный
   `Q2PRO-X.exe`.
4. Откройте `Join online` → `Custom`, нажмите `Refresh` и выберите комнату.

Права администратора не нужны. После первого запуска используйте ярлык
**Q2PRO-X - Dos.Zone** из меню «Пуск». Не запускайте этот ярлык повторно, пока
первый запуск из `Install.cmd` ещё работает. Повторный запуск нового
`Install.cmd` обновляет add-on. Для удаления используйте ярлык **Uninstall
Q2PRO-X - Dos.Zone**.

## Windows portable

1. Скачайте `q2prox-dz-1.0.0-rc15-windows-x64-portable.zip` и распакуйте его в
   отдельную папку.
2. Закройте уже запущенный Q2PRO-X и запустите `Run.cmd`.
3. При первом запуске укажите `Q2PRO-X.exe`.
4. Откройте `Join online` → `Custom`, нажмите `Refresh` и выберите комнату.

Для обновления распакуйте новый архив в новую папку и запустите новый
`Run.cmd`. Для удаления удалите portable-папку. Сохранённый путь к Q2PRO-X
находится в `%LOCALAPPDATA%\Q2PRO-X Dos.Zone\state\q2-path.txt`; его можно
удалить отдельно.

Если Windows сообщает об отсутствии WebView2 Runtime, установите официальный
Microsoft Evergreen Runtime x64:
<https://developer.microsoft.com/microsoft-edge/webview2/consumer/>.

## Linux x86-64

Нужны Wine, Python 3 и библиотеки, перечисленные в
[linux/README.md](linux/README.md).

```bash
tar -xzf q2prox-dz-1.0.0-rc15-linux-x86_64.tar.gz
cd q2prox-dz-1.0.0-rc15
./install.sh
```

Запустите **Q2PRO-X — Dos.Zone** из меню приложений или командой
`q2dz-launch`. Откройте `Join online` → `Custom`, нажмите `Refresh` и выберите
комнату.
Повторный `./install.sh` обновляет add-on. Удаление:

```bash
"${XDG_DATA_HOME:-$HOME/.local/share}/q2dz/uninstall.sh"
```

## Если Q2PRO-X обновлён или перенесён

Если путь к EXE не изменился, ничего делать не нужно. Если изменился:

- Windows: удалите `%LOCALAPPDATA%\Q2PRO-X Dos.Zone\state\q2-path.txt` и снова
  запустите launcher;
- Linux: выполните `q2dz-launch --select-q2`.

## Типичные проблемы

- **Комнат нет:** сейчас нет готовых публичных комнат; нажмите `Refresh` позже.
- **Комната появилась или закрылась:** дождитесь очередного опроса каталога и
  нажмите `Refresh`; перезапуск Q2PRO-X не нужен.
- **Q2PRO-X уже запущен:** закройте его перед запуском add-on.
- **Map ... is missing:** установите нужную карту/мод из доверенного источника
  в выбранную игровую установку либо выберите другую комнату.
- **Unknown publisher:** Windows RC пока не подписан. Для закрытого alpha-теста
  используйте GitHub Release либо архив от доверенного распространителя с
  проверенным SHA-256; для широкого публичного распространения требуется
  Authenticode.

Комнату создают в официальном интерфейсе Dos.Zone. После создания дождитесь
очередного опроса каталога и нажмите `Refresh` в `Custom`.

## Необязательная проверка архива

Рядом с каждым release-архивом публикуется файл `.sha256`. Проверка особенно
полезна при передаче архива не через GitHub:

```powershell
Get-FileHash .\q2prox-dz-*-windows-x64-*.zip -Algorithm SHA256
```

```bash
sha256sum -c q2prox-dz-*-linux-x86_64.tar.gz.sha256
```
