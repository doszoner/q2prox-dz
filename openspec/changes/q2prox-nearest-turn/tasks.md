# Tasks

[Handoff: продолжение на Windows](../../../docs/verification/2026-10-18-nearest-turn-windows-handoff.md).

Реализация и компонентные проверки Python/PowerShell Core выполнены 2026-10-18.
Linux-сценарий через обычный Hub завершён в фоне, без управления рабочим столом:
Custom, ручной Refresh, вход, q2dm1 → q2dm2 с тем же PID клиента и штатный Quit.
Windows-проверки по решению пользователя переданы пользователю Windows;
пункты 1.3, 2.4 и 3.4 остаются открытыми. Функциональный контракт двух ОС сохраняется.
Сводка результатов и переносимые инструкции — в handoff по ссылке выше.
Исходные локальные evidence сохранены вне Git; для продолжения они не требуются.

## 1. Общий контракт

- [x] 1.1 Добавить общие fixtures: один/несколько domain, равные RTT, частичные и полные ошибки, deadline/DNS, смешанные STUN/TURN URL, TURN не во второй позиции, дубликаты domain, ручной выбор найден/не найден/невалиден. Проверить ожидаемый порядок и состав ICE на Python и PowerShell Core на Linux. Проверка второй ОС вынесена в 1.3.
- [x] 1.2 Проверить механизм deadline и завершения сетевых задач в Python; уточнить design с учётом ограничений API. Компонентный процесс проверен также в PowerShell Core на Linux. Windows PowerShell 5.1 остаётся в 1.3.
- [ ] 1.3 Пользователь Windows: проверить в Windows PowerShell 5.1 общие fixtures и одинаковые ожидаемые порядок/состав ICE, deadline с зависшим DNS, сохранение успешных samples, завершение worker без накопления задач и ограничение записи в полный stdin pipe. См. Windows checklist; Core на Linux эту задачу не закрывает.

## 2. Лаунчеры

- [x] 2.1 Linux: реализовать ограниченное измерение и выбор, сессионный cache domain, свежие credentials, безопасный override; добавить тесты в существующий launcher suite.
- [x] 2.2 Windows: зеркально реализовать контракт в catalog/launcher, обновить общий runner и тесты для Windows PowerShell 5.1; проверить их на доступном PowerShell Core. Запуск под Windows PowerShell 5.1 вынесен в 2.4.
- [x] 2.3 Проверить исчезновение выбранного domain, смену порядка ответа, лимиты и ошибки полного RTC-ответа; ICE-only refresh не должен пересоздавать активный target: штатные heartbeat PUT сохраняются, DROP отсутствует, coordinator не создаёт новый gateway target/ID.

- [ ] 2.4 Пользователь Windows: выполнить catalog.tests.ps1 и launcher-stdin.tests.ps1 именно в Windows PowerShell 5.1, включая selection/cache/refresh/heartbeat, UTF-8, EOF и exit codes; записать результаты по Windows checklist.

## 3. Документы и проверка

- [x] 3.1 После реализации обновить ADR 0004, README и Unreleased; не называть выбор серверов защитой от общей TURN-квоты.
- [x] 3.2 Агент, Linux: проверить локальный launcher через обычный Hub — список, ручной Refresh в меню, вход, смену карты и игровой Exit. Custom, Refresh, вход в собственную комнату, q2dm1 → q2dm2 с тем же PID и штатный Quit подтверждены в фоне. Runbook/результаты вне Git; без raw ICE/SDP/credentials. Gameplay принимает пользователь.
- [x] 3.3 При наличии операторских метрик сравнить allocations по серверу/квотной области до/после и после закрытия. Если доступа нет, отметить «серверные allocations не измерены». Кандидаты и количество URL не служат заменой.

- [ ] 3.4 Пользователь Windows: проверить локальный launcher через обычный Hub — список, ручной Refresh, вход, смена карты и игровой Exit, свежие credentials без пересоздания активного target, очистку принадлежащих проверке процессов и отсутствие секретов в журнале. Runbook/результаты вне Git; gameplay принимает пользователь.

Компонентные границы: общие fixtures выполняются на Python и PowerShell Core
на Linux. Windows-проверки 1.3/2.4/3.4 переданы пользователю и остаются открытыми.
По 3.3: серверные allocations не измерены; доступа к операторским метрикам нет.
Обновление credentials без пересоздания target подтверждено компонентно;
принудительная live-ротация не выполнялась.

Linux 3.2: source launcher запущен с установленными native-компонентами через
штатные Q2DZ_* overrides. Custom/Refresh и новая управляемая комната наблюдались
в меню. Host scoreboard подтвердил native-вход (один → два игрока).
Штатная команда владельца `gamemap q2dm2` сменила карту; native отрисовал новую
карту на прежнем PID/start ticks. Выбор Quit в игровом меню закрыл клиент;
журнал зафиксировал `q2-normal-exit` и `coordinator_exit rc=0`.
Обычный Hub использовал опубликованный v21, локальный Vite base `/mp/` и штатную
настройку Chat hotkeys → No; native работал на приватном Xvfb. Система и
пользовательский рабочий стол не изменялись. Ранние ошибки тестового запуска
и их исправления описаны в evidence; их частичные результаты не заменяют
последний успешный сценарий. Gameplay принимает пользователь.
Краткая сводка результатов и очистки приведена в handoff; сырые evidence вне Git.


## Windows checklist

Владелец — пользователь Windows. Все пункты ниже пока не выполнены на Windows;
пропуск проверок в этой сессии не означает PASS. Из корня checkout этой ветки
запустить именно Windows PowerShell 5.1, используя штатный путь:

```powershell
& "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -Command '$PSVersionTable.PSVersion.ToString()'
& "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -File .\windows\product\tests\catalog.tests.ps1
& "$env:WINDIR\System32\WindowsPowerShell\v1.0\powershell.exe" -NoProfile -NonInteractive -File .\windows\product\tests\launcher-stdin.tests.ps1
```

Зафиксировать checkout/HEAD, версию `5.1`, exit code каждого запуска и безопасные
итоговые строки тестов вне Git. Здесь проверена работа этих скриптов только в
PowerShell Core на Linux; результат Windows ещё требуется.

1. Для 1.3/2.4: оба скрипта завершаются с кодом 0. Общие cases подтверждают тот
   же ожидаемый порядок/состав ICE; selection/cache/refresh/heartbeat проверки
   проходят. Зависший DNS и не читающий stdin worker не блокируют launcher,
   завершённые samples сохраняются, принадлежащий worker после каждого прогона
   отсутствует. Повторные прогоны не накапливают процессы/задачи.
2. Для 3.4: запустить изменённый Windows launcher с совместимыми native-компонентами
   и использовать обычный Hub с origin `https://test.js-dos.com:443`. Проверить
   список в `Custom`, ручной `Refresh`, вход, смену карты и игровой `Exit`.
   При свежих credentials сохраняются active target ID и соединение; свежий
   ICE generation и обычные heartbeat PUT допустимы, DROP/пересоздание — нет.
3. После выхода проверить завершение принадлежащих проверке launcher, coordinator,
   gateway/WebView2 и worker. Созданную для проверки комнату закрыть через Exit
   и убедиться, что она исчезла. В evidence и журналах не должно быть raw ICE,
   SDP, candidates или credentials; сохранять только безопасные сводки.

Если шаг не достигнут или отказал, записать конкретный шаг/причину и оставить
соответствующую задачу открытой. Игровую приемку выполняет пользователь.
