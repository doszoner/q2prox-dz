# Windows empty Custom: исправление и поставка rc16

Пользователь 2026-09-08 разрешил исправления подтверждённых причин, тесты,
коммит и новую поставку; Ponytail ultra. Работа в существующей
`feat/dedicated-compat`, без нового worktree, push и изменений production.

## Минимальный дизайн

1. Стабильность комнаты: как на Linux, не учитывать `iceServers` при решении
   DROP/PUT, но продолжать отдельное обновление ICE. Не удалять ICE-контракт.
2. Карты: недоступная карта исключает только эту цель, включая случай смены
   карты существующей цели (DROP), не мешая остальным. Ошибка запуска checker
   остаётся ошибкой, глобальную валидацию и duplicate checks не ослаблять.
3. JS/C++: формировать UDP JSON в ожидаемом порядке полей; регрессия должна
   проверять реального отправителя и decoder, не только каждый отдельно.
4. Terminal events: согласовать глобальный HUB/2 FATAL между gateway и
   coordinator; сохранить stage/code и существующее bounded recovery.
   Не игнорировать неизвестные события и не писать секретные payload.

## Исполнение

- [x] Сначала падающие проверки в существующих PowerShell/Node/C++ тестах.
- [x] Минимальные изменения `windows/product/catalog.ps1`, gateway assets,
  coordinator event parsing/handling и необходимых protocol docs; без рефакторинга.
- [x] Локальные PowerShell/Node/C++/Python проверки проходят.
- [x] Независимые проверки соответствия и качества: SPEC PASS / QUALITY PASS
  для source-commit gate, не для Windows-поставки.
- [x] VERSION rc16, CHANGELOG, evidence; Linux остаётся rc15.
- [ ] Windows build-payload.ps1 в существующей
  VM с закреплёнными зависимостями, lifecycle, Brepro, package smoke.
- [ ] Проверка новой portable-поставки на Windows: несколько целей, одна
  недоступная карта, смена ICE; реальный WebView2/Quake II обмен если среда позволяет.
  Не приравнивать fake-peer/READY к gameplay.
- [x] Коммит от текущей repo identity; без push.
- [ ] Portable/setup ZIP + SHA256; источник поставки однозначен.
  Старые поставки не перезаписывать.
- [x] Открытые live-gates перечислены явно в rc16 verification.

Windows build/package и проверка готовых архивов заблокированы авторизацией
GuestControl. VM возвращена в исходное состояние `saved`; требуется доступ
от пользователя. Это не PASS поставки.

Основание: `docs/verification/2026-09-08-windows11-empty-custom-review.md`.
Исполнение по subagent-driven-development: один implementer, затем review;
root параллельно готовит Windows-окружение и поставку.
