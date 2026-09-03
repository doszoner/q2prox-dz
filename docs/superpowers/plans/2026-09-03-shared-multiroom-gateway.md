# Shared Multiroom Gateway Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use
> `superpowers:subagent-driven-development` (recommended) or
> `superpowers:executing-plans` to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** заменить фиксированные `3 q2ded + 2 q2` отдельные gateways одним
WebRTC-NET runtime, который публикует динамический набор комнат во встроенной
вкладке `Custom` Q2PRO-X на Windows и Linux.

**Architecture:** launcher каждые 15 секунд отправляет полностью проверенный
heartbeat комнат и общий ICE snapshot. Coordinator владеет Q2PRO-X, Custom
source и одним shared transport; transport держит отдельный loopback UDP socket
и remote PeerId на комнату в одном HumbleNet context. Сбой комнаты изолирован,
общий crash получает одну попытку восстановления, секреты не сохраняются.

**Tech Stack:** C++20/CMake, Win32/WebView2, JavaScript ES modules,
WebRTC-NET/HumbleNet, Python 3, PowerShell 7, CTest/Node unittest.

**Design:**
[2026-09-03-shared-multiroom-gateway.md](../../design/2026-09-03-shared-multiroom-gateway.md)

---

## Ограничения выполнения

- Не увеличивать старые slot constants как «временное решение».
- Не менять Q2PRO-X, Quake protocol, Dos.Zone и серверный `status`.
- Не добавлять зависимости, новый сервис, транзакционный snapshot protocol,
  LRU, idle eviction или второй пользовательский режим.
- Не хранить ICE/TURN, alias, room JSON, SDP или candidates на диске, в argv,
  environment либо журналах.
- Не использовать текущий грязный sibling checkout WebRTC-NET для release.
- Worktree создавать рядом с репой, не в `~/.config/superpowers/worktrees`.
- Все commits: `doszoner <doszoner@users.noreply.github.com>`; не push.
- После первого непрошедшего admission gate остановить зависимые задачи,
  сохранить evidence и исправлять только доказанный узкий дефект.

## Task 0: Зафиксировать чистую исходную точку

**Files:** create `docs/verification/1.0.0-rc14.md`.

- [ ] **Step 1: проверить q2prox-dz checkout.**

```bash
git status --short --branch
git config user.name
git config user.email
git rev-parse HEAD
```

Expected: только заранее утверждённые design/plan изменения; identity ровно
`doszoner` / `doszoner@users.noreply.github.com`.

- [ ] **Step 2: прочитать locked WebRTC-NET commit без использования состояния
  соседнего checkout.**

```bash
python3 - <<'PY'
import json
print(json.load(open('deps.lock.json'))['webrtcNet']['commit'])
PY
git -C ../WebRTC-NET status --short --branch
```

Expected: lock печатает immutable commit; любой dirty state соседней репы
зафиксирован, но не очищается и не используется.

- [ ] **Step 3: создать отдельный WebRTC-NET worktree от locked commit.**

```bash
git -C ../WebRTC-NET worktree add -b feat/q2prox-shared-runtime \
  ../WebRTC-NET-q2prox-shared \
  "$(python3 -c 'import json; print(json.load(open("deps.lock.json"))["webrtcNet"]["commit"])')"
git -C ../WebRTC-NET-q2prox-shared submodule update --init --recursive
```

Expected: clean feature worktree от exact locked commit. Если worktree уже
существует, проверить его HEAD/clean state вместо повторного создания.

- [ ] **Step 4: записать baseline и команды в новый verification doc.**

Документ содержит q2prox HEAD, WebRTC-NET HEAD/submodule commits, supported
Q2PRO-X SHA-256, OS и явный список ещё не пройденных live gates; никаких room
JSON или credential values.

- [ ] **Step 5: подготовить exact Emscripten 5.0.2 вне репозиториев.**

```bash
export Q2DZ_EMSDK=/home/ssolovey/Projects/Q2/DosZone/noncommit/q2prox-shared/emsdk
if [[ ! -x "$Q2DZ_EMSDK/emsdk" ]]; then
  git clone https://github.com/emscripten-core/emsdk.git "$Q2DZ_EMSDK"
fi
"$Q2DZ_EMSDK/emsdk" install 5.0.2
"$Q2DZ_EMSDK/emsdk" activate 5.0.2
source "$Q2DZ_EMSDK/emsdk_env.sh"
emcc --version
```

Expected: `emcc` reports 5.0.2; SDK lives only under `noncommit/`.

- [ ] **Step 6: commit.**

```bash
git add docs/verification/1.0.0-rc14.md
git commit -m "test: record shared gateway baseline"
```

## Task 1: Исправить lifecycle cleanup в WebRTC-NET WASM

**Files:** in the isolated WebRTC-NET worktree, modify
`src/humblenet/src/libwebrtc_asmjs.cpp`, `webrtcnet/src/main.cpp`,
`tests/CMakeLists.txt`; create `tests/test_asmjs_cleanup.cpp` and
`tests/test_asmjs_cleanup.mjs`.

- [ ] **Step 1: написать RED state test.**

Emscripten test target вызывает настоящие
`libwebrtc_create_connection_extended`, `libwebrtc_create_channel`,
`libwebrtc_close_connection` и `libwebrtc_close_channel`. Node driver даёт
минимальный fake `RTCPeerConnection`/DataChannel и проверяет:

```text
connection id назначается монотонным счётчиком, не Map.size
channel id назначается монотонным счётчиком, не Map.size
counter overflow завершает создание fail-closed до переиспользования id
connections.delete(connection.id)
channels.delete(channel._id)
remote channel close также удаляет channel._id
```

Сценарий оставляет B живым, удаляет A, создаёт C и доказывает разные id и
сохранность B; отдельно вызывает remote channel close callback. Живой
500-cycle browser churn остаётся admission gate Task 7.

- [ ] **Step 2: запустить только новый тест и получить RED.**

```bash
cmake -S ../WebRTC-NET-q2prox-shared \
  -B ../WebRTC-NET-q2prox-shared/build-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ -DUSE_BORINGSSL=False \
  -DHUMBLENET_EXTERNAL_WEBRTC=ON
cmake --build ../WebRTC-NET-q2prox-shared/build-native --target bin_flatc
export Q2DZ_EMSDK=/home/ssolovey/Projects/Q2/DosZone/noncommit/q2prox-shared/emsdk
source "$Q2DZ_EMSDK/emsdk_env.sh"
emcmake cmake -S ../WebRTC-NET-q2prox-shared \
  -B ../WebRTC-NET-q2prox-shared/build-wasm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build ../WebRTC-NET-q2prox-shared/build-wasm \
  --target asmjs-lifecycle-test
node ../WebRTC-NET-q2prox-shared/tests/test_asmjs_cleanup.mjs \
  ../WebRTC-NET-q2prox-shared/build-wasm/asmjs-lifecycle-test.mjs
```

Expected: FAIL — старый код использует `Map.size + 1`, оставляет remote-closed
channel и удаляет local channel id из неверной map.

- [ ] **Step 3: минимально исправить map cleanup.**

Требуемая семантика:

```javascript
connection.id = ++libwebrtc.nextConnectionId;
channel._id = ++libwebrtc.nextChannelId;
libwebrtc.connections.delete(connection.id);
libwebrtc.channels.delete(channel._id);
```

Счётчики инициализируются один раз и не уменьшаются. Local и remote close
удаляют правильную map entry. Перед удалением сохранить callback id, обнулить
обработчики, закрыть объект и не обращаться к удалённой записи повторно. Не
переписывать backend и не менять signaling.

В `webrtcnet/src/main.cpp` добавить один production export:

```cpp
extern "C" bool EMSCRIPTEN_KEEPALIVE setIceServers(const char* json) {
    return json && humblenet_set_iceservers(json);
}
```

Negative test передаёт null/invalid/empty JSON; valid test обновляет runtime
после `connectTo()`. Export не логирует строку.

- [ ] **Step 4: GREEN, существующие tests и exact WASM build.**

```bash
cmake -S ../WebRTC-NET-q2prox-shared \
  -B ../WebRTC-NET-q2prox-shared/build-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ -DUSE_BORINGSSL=False \
  -DHUMBLENET_EXTERNAL_WEBRTC=ON
cmake --build ../WebRTC-NET-q2prox-shared/build-native \
  --target humblenet_external_webrtc_provider_install
cmake -S ../WebRTC-NET-q2prox-shared \
  -B ../WebRTC-NET-q2prox-shared/build-native -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ -DUSE_BORINGSSL=False \
  -DHUMBLENET_EXTERNAL_WEBRTC=ON
cmake --build ../WebRTC-NET-q2prox-shared/build-native \
  --target websockets-build webrtc_shared humblenet bin_flatc

export Q2DZ_EMSDK=/home/ssolovey/Projects/Q2/DosZone/noncommit/q2prox-shared/emsdk
source "$Q2DZ_EMSDK/emsdk_env.sh"
emcmake cmake -S ../WebRTC-NET-q2prox-shared \
  -B ../WebRTC-NET-q2prox-shared/build-wasm -G Ninja \
  -DCMAKE_BUILD_TYPE=Release
cmake --build ../WebRTC-NET-q2prox-shared/build-wasm --target webrtcnet
cmake --build ../WebRTC-NET-q2prox-shared/build-wasm \
  --target asmjs-lifecycle-test
node ../WebRTC-NET-q2prox-shared/tests/test_asmjs_cleanup.mjs \
  ../WebRTC-NET-q2prox-shared/build-wasm/asmjs-lifecycle-test.mjs

install -d ../WebRTC-NET-q2prox-shared/release-linux \
  ../WebRTC-NET-q2prox-shared/release-windows
install -m 0644 ../WebRTC-NET-q2prox-shared/build-native/libhumblenet.so \
  ../WebRTC-NET-q2prox-shared/build-native/libwebrtc.so \
  ../WebRTC-NET-q2prox-shared/build-wasm/webrtcnet.mjs \
  ../WebRTC-NET-q2prox-shared/build-wasm/webrtcnet.wasm \
  ../WebRTC-NET-q2prox-shared/src/humblenet/include/humblenet.h \
  ../WebRTC-NET-q2prox-shared/src/humblenet/include/humblenet_p2p.h \
  ../WebRTC-NET-q2prox-shared/release-linux/
install -m 0644 ../WebRTC-NET-q2prox-shared/build-wasm/webrtcnet.mjs \
  ../WebRTC-NET-q2prox-shared/build-wasm/webrtcnet.wasm \
  ../WebRTC-NET-q2prox-shared/release-windows/
```

Expected: actual state test PASS, существующие tests PASS, сборка exact WASM
release завершается без предупреждений/ошибок. 500-cycle live churn остаётся
в Task 7.

- [ ] **Step 5: commit в WebRTC-NET worktree.**

```bash
git -C ../WebRTC-NET-q2prox-shared add \
  src/humblenet/src/libwebrtc_asmjs.cpp webrtcnet/src/main.cpp \
  tests/CMakeLists.txt tests/test_asmjs_cleanup.cpp \
  tests/test_asmjs_cleanup.mjs
git -C ../WebRTC-NET-q2prox-shared commit \
  -m "fix(wasm): release closed WebRTC entries"
```

Сохранить новый commit и SHA-256 `webrtcnet.mjs`/`webrtcnet.wasm`. До появления
этого immutable commit Task 4 и release tasks заблокированы.

- [ ] **Step 6: проверить публичную достижимость commit, не выполняя push.**

```bash
Q2DZ_WEBRTC_COMMIT="$(git -C ../WebRTC-NET-q2prox-shared rev-parse HEAD)"
git ls-remote git@github.com:doszoner/WebRTC-NET.git | \
  awk -v sha="$Q2DZ_WEBRTC_COMMIT" '$1 == sha { found=1 } END { exit !found }'
```

Expected для public release: вывод содержит exact SHA. До отдельного
разрешённого владельцем push команда закономерно может вернуть пустой вывод;
это блокирует packaging, но не локальную реализацию и тесты.

## Task 2: Ввести bounded protocol v2

**Files:** create `tests/contracts/hub-protocol-cases.json`; modify
`linux/src/protocol.hpp`, `linux/src/protocol.cpp`,
`linux/tests/unit_tests.cpp`,
`windows/coordinator/include/q2dz/child_protocol.hpp`,
`windows/coordinator/src/child_protocol.cpp`,
`windows/coordinator/tests/core_tests.cpp`.

- [ ] **Step 1: добавить общие RED fixtures.**

`tests/contracts/hub-protocol-cases.json` содержит positive/negative cases для:

```text
Q2DZ/ROOMS/2 ICE generation=7 json=[...]
Q2DZ/ROOMS/2 PUT room=room.q2ded.server json={...}
Q2DZ/ROOMS/2 DROP room=room.q2ded.server
Q2DZ/HUB/2 ICE generation=7 json=[...]
Q2DZ/HUB/2 PUT id=42 json={...}
Q2DZ/HUB/2 DROP id=42
Q2DZ/HUB/2 READY id=42 udp=127.0.0.1:27910 peer=17
```

Negative cases: v1/v2 mix, zero/overflow id, unsafe room, duplicate key, NUL,
CR/LF, missing ICE before PUT, non-loopback endpoint, credential-shaped event,
line >256 KiB и queue overflow.

- [ ] **Step 2: получить RED на Linux и portable Windows core.**

```bash
cmake -S linux -B /tmp/q2dz-linux-core -G Ninja
cmake --build /tmp/q2dz-linux-core
ctest --test-dir /tmp/q2dz-linux-core --output-on-failure -R unit
cmake -S windows/coordinator -B /tmp/q2dz-win-core -DQ2DZ_BUILD_TESTS=ON
cmake --build /tmp/q2dz-win-core
ctest --test-dir /tmp/q2dz-win-core --output-on-failure
```

Expected: v2 fixtures fail before parser implementation.

- [ ] **Step 3: реализовать только parser/formatter и типы.**

Минимальные типы:

```cpp
enum class HubCommandType { ice, put, drop, stop };
struct HubCommand {
    HubCommandType type;
    std::uint64_t id_or_generation;
    std::string room_id;
    std::vector<unsigned char> json;
};
```

Парсер находит фиксированный prefix и ` json=`, не пишет общий JSON parser
заново, использует существующие UTF-8/safe-token/secure-clear helpers. Queue
остаётся bounded, но принимает один полный heartbeat до 256 целей.

- [ ] **Step 4: GREEN и security scan.**

Expected: одинаковые fixtures PASS на обеих платформах; события с ICE/alias/
token/SDP/candidate отклоняются.

- [ ] **Step 5: commit.**

```bash
git add tests/contracts linux/src/protocol.* linux/tests/unit_tests.cpp \
  windows/coordinator/include/q2dz/child_protocol.hpp \
  windows/coordinator/src/child_protocol.cpp windows/coordinator/tests/core_tests.cpp
git commit -m "feat: define shared gateway protocol"
```

## Task 3: Сделать один native Linux transport runtime

**Files:** modify `linux/transport/q2_doszone_gateway.cpp`,
`linux/tests/unit_tests.cpp`, `linux/CMakeLists.txt`,
`linux/tools/run-sanitizers.sh`; create
`linux/transport/multiroom_state.hpp` and
`linux/transport/multiroom_state.cpp`; later remove
`linux/src/managed_gateway_main.cpp` and `linux/config/legacy-gateway.json` only
after the direct path is GREEN.

- [ ] **Step 1: RED unit tests для target routing.**

Покрыть динамическую map, уникальные endpoints, duplicate remote PeerId,
unknown peer drop, отдельный captured source, independent DROP и stable
datagram counters. Тестируемую чистую логику держать в существующем transport
target; новый interface/factory не создавать.

- [ ] **Step 2: заменить single-room loop на динамический.**

В `linux/transport/q2_doszone_gateway.cpp`:

- один `humblenet_init/p2p_init`;
- `poll()` над control fd и UDP sockets;
- `unordered_map<targetId, Target>` и reverse `remotePeerId -> targetId`;
- один `humblenet_p2p_wait()` на loop;
- exact alias query, reject `duplicate_remote_peer`;
- per-target OOB probe/close state;
- `humblenet_set_iceservers()` только на runtime thread перед новыми peers.

- [ ] **Step 3: перенести проверенные защиты в direct gateway.**

`q2dz-webrtc-gateway` напрямую читает persistent HUB/2 stdin, пишет bounded
events в stdout и применяет существующие `disable_core_dumps_and_ptrace`,
owner/process-group cleanup, secure clear и safe diagnostics. После GREEN
удалить managed adapter target/config из CMake/package. Второй IPC, legacy
child и memfd forwarding не создавать.

- [ ] **Step 4: GREEN.**

```bash
Q2DZ_WEBRTC_NET_RELEASE_DIR=../WebRTC-NET-q2prox-shared/release-linux \
  cmake -S linux -B /tmp/q2dz-linux-shared -G Ninja \
  -DQ2DZ_REQUIRE_WEBRTC_GATEWAY=ON
cmake --build /tmp/q2dz-linux-shared
ctest --test-dir /tmp/q2dz-linux-shared --output-on-failure
```

Expected: unit/transport contract tests PASS; sanitizer run has no ASan/UBSan
failure.

```bash
Q2DZ_WEBRTC_NET_RELEASE_DIR=../WebRTC-NET-q2prox-shared/release-linux \
  linux/tools/run-sanitizers.sh
```

До запуска изменить `run-sanitizers.sh` и `linux/CMakeLists.txt`, чтобы
`q2dz-webrtc-gateway` действительно компилировался с ASan/UBSan и product
transport target был обязателен.

- [ ] **Step 5: Linux two-target live admission gate.**

Одновременно одна `q2` и одна `q2ded` цель: OOB ack/status обеих, изолированное
закрытие одной, вторая продолжает status/game traffic. Обновить ICE, сделать
старые test credentials недействительными, добавить новую цель без restart.
Если два alias разрешились в один remote PeerId, новая цель обязана получить
явный `duplicate_remote_peer`.

- [ ] **Step 6: commit.**

```bash
git add linux tests/contracts docs/verification/1.0.0-rc14.md
git commit -m "feat(linux): share one transport across rooms"
```

## Task 4: Сделать один Windows WebView2 runtime

**Files:** modify `windows/gateway/assets/transport.html`,
`windows/gateway/assets/transport_protocol.mjs`,
`windows/gateway/src/main.cpp`, `windows/gateway/src/webview_bridge_protocol.h`,
`windows/gateway/src/webview_bridge_protocol.cpp`,
`windows/gateway/tests/transport_protocol.test.mjs`,
`windows/gateway/tests/webview_bridge_protocol_test.cpp`, `deps.lock.json`.

- [ ] **Step 1: RED Node tests для multi-target page state.**

`transport_protocol.test.mjs` проверяет: exact alias queries, разные remote
PeerId, duplicate reject, target-tagged send/receive, per-target probes,
independent close, unknown peer, bounded pump work и ICE update before connect.

- [ ] **Step 2: RED native bridge tests.**

`webview_bridge_protocol_test.cpp` проверяет targetId во всех UDP/state
messages, 1400/1401 boundary, malformed target, credential-shaped output и
40 interleaved targets without cross-route.

- [ ] **Step 3: переделать `transport.html` на одну map.**

Сохранить один module, один 5 ms timer и один receive buffer. Добавить maps
`targetId -> Target` и `remotePeerId -> targetId`; за tick обрабатывать
ограниченное число входящих пакетов, чтобы одна шумная комната не блокировала
остальные. Не создавать controller/module/timer на комнату.

- [ ] **Step 4: переделать native host на N UDP sockets.**

`windows/gateway/src/main.cpp` сохраняет один STA/WebView2 controller и owner
thread, но socket/client/probe/counters становятся per-target. Control pipe
читается bounded и доставляется на STA thread; WebView callbacks никогда не
исполняются с reader thread.

- [ ] **Step 5: подключить исправленный locked WASM.**

Обновить q2prox `deps.lock.json` exact WebRTC-NET commit и hashes, затем:

```bash
python3 tools/verify-dependencies.py \
  --webrtc-source ../WebRTC-NET-q2prox-shared \
  --linux-release ../WebRTC-NET-q2prox-shared/release-linux
```

На Windows после копирования `release-windows` в `C:\deps\webrtc-release`:

```powershell
py -3 tools\verify-dependencies.py `
  --windows-release C:\deps\webrtc-release `
  --webview2-loader C:\deps\webview2-sdk\build\native\x64\WebView2Loader.dll
```

Expected: commit, submodules, licenses и platform-specific binary hashes PASS;
ни один verifier не видит артефакты другой платформы.

- [ ] **Step 6: exact gateway build/tests и two-target admission gate.**

```powershell
cmake -S windows\gateway -B C:\q2dz-build\gateway `
  -G "Visual Studio 17 2022" -A x64 `
  -DBUILD_TESTING=ON `
  -DWEBVIEW2_SDK_DIR=C:\deps\webview2-sdk `
  -DWEBRTC_NET_RELEASE_DIR=C:\deps\webrtc-release
cmake --build C:\q2dz-build\gateway --config Release
ctest --test-dir C:\q2dz-build\gateway -C Release --output-on-failure
node --test windows\gateway\tests\transport_protocol.test.mjs
```

Expected: one gateway EXE, one WebView2 tree, q2+q2ded simultaneous OOB/status,
one close isolated, ICE rotation permits a new connection. Полный
`build-payload.ps1` запускается только в Task 8 после перевода coordinators и
launchers на protocol v2.

- [ ] **Step 7: commit.**

```bash
git add windows/gateway deps.lock.json docs/verification/1.0.0-rc14.md
git commit -m "feat(windows): share one WebView2 transport"
```

## Task 5: Перевести coordinators на dynamic targets

**Files:** modify `linux/src/session_main.cpp`,
`linux/tests/fake_lifecycle.sh`, `windows/coordinator/src/windows/main.cpp`,
`windows/coordinator/tests/windows/fake_gateway.cpp`,
`windows/coordinator/tools/run-lifecycle-matrix.ps1`.

- [ ] **Step 1: расширить fake gateways и получить RED.**

Linux/Windows matrix: shuffled READY, late stale event after DROP+PUT, target
timeout, one close, one fatal, global crash/restart, second global crash,
Q2 exit и owner hard-kill. Проверить 0/1/2 targets; capacity и 40-room behavior
принадлежат launcher/resource gates.

- [ ] **Step 2: заменить arrays/slots на maps.**

Удалить `kHubMaxRooms`, `HUB_DEDICATED_SLOTS`, slot digit parser и room-indexed
directories. Coordinator хранит только несекретное runtime state:

```cpp
struct LiveTarget {
    std::string room_id;
    std::uint64_t target_id;
    Endpoint endpoint;
    State state;
    unsigned recovery_count;
};
```

Room config очищается сразу после передачи. Stale event с прежним targetId
игнорируется. Custom source сортирует READY endpoints по roomId.

- [ ] **Step 3: отвязать Q2 startup от room readiness.**

После готовности HTTP source запустить Q2 с прежними `game`, `cl_timeout` и
`cl_serverbrowser_custom_source`. Пустой каталог и transport failure не должны
закрывать Q2. Только Q2 exit, owner termination или coordinator internal
failure завершают всю process tree.

- [ ] **Step 4: реализовать ровно один global restart.**

После gateway crash: source empty, restart count +1, новый gateway ждёт
следующий launcher heartbeat. Coordinator не replay-ит payload. После второго
crash source остаётся пуст, Q2 работает, новые transport commands отвергаются
до следующего запуска q2prox.

- [ ] **Step 5: GREEN на Linux и Windows.**

Expected: fake lifecycle matrices PASS; exactly one shared transport child,
no room-sized process fanout, no orphans.

- [ ] **Step 6: commit.**

```bash
git add linux/src/session_main.cpp linux/tests/fake_lifecycle.sh \
  windows/coordinator/src/windows/main.cpp \
  windows/coordinator/tests/windows/fake_gateway.cpp \
  windows/coordinator/tools/run-lifecycle-matrix.ps1
git commit -m "feat: supervise dynamic room targets"
```

## Task 6: Перевести launchers с slots на full heartbeat

**Files:** modify `tests/contracts/doszone-api-cases.json`,
`linux/tools/q2dz-launch`, `linux/tests/test_launcher.py`,
`windows/product/catalog.ps1`, `windows/product/q2dz-launch.ps1`,
`windows/product/tests/catalog.tests.ps1`,
`windows/product/tests/launcher-stdin.tests.ps1`.

- [ ] **Step 1: общие RED catalog cases.**

Добавить cases: mixed 20 q2 + 20 q2ded, stable order, all 40 retained, one
removed after two successful polls, map change/content failure, one full room,
invalid global RTC, duplicate room/alias, exactly 256 и 257 compatible rows.

- [ ] **Step 2: заменить slot planner на словарь roomId.**

Python/PowerShell выполняют тот же алгоритм:

```text
validate whole response
normalize all compatible registered non-full new rooms
content-check each new/changed map
reject whole new snapshot when compatible count > 256
emit ICE once
emit idempotent PUT for every desired target
emit DROP after two successful misses
```

При сетевой/schema ошибке не отправлять ничего и сохранять текущий state.

- [ ] **Step 3: сохранить свежесть без хранения секретов.**

Launcher строит fresh union ICE на каждом успешном poll. Coordinator получает
полный heartbeat; Python/PowerShell не пишут payload во временные файлы и
обнуляют/освобождают byte buffers после записи.

- [ ] **Step 4: GREEN.**

```bash
python3 -m unittest linux/tests/test_launcher.py -v
pwsh -NoProfile -File windows/product/tests/catalog.tests.ps1
pwsh -NoProfile -File windows/product/tests/launcher-stdin.tests.ps1
```

Expected: платформы выдают одинаковый logical heartbeat, 40/40 представлены,
257 даёт `catalog_capacity_exceeded`, хвост никогда не скрывается.

- [ ] **Step 5: commit.**

```bash
git add tests/contracts/doszone-api-cases.json linux/tools/q2dz-launch \
  linux/tests/test_launcher.py windows/product/catalog.ps1 \
  windows/product/q2dz-launch.ps1 windows/product/tests
git commit -m "feat: publish all compatible rooms"
```

## Task 7: Exact Q2PRO-X и ресурсные gates

Все шаги используют один retained 40-target controlled harness. Не создавать
отдельные генераторы для Q2PRO-X, Linux и Windows.

**Files:** create `tests/tools/controlled-room-set.py` and
`tests/tools/q2dz-controlled-peer.cpp`; register the peer only as a test target
in `linux/CMakeLists.txt`; modify `docs/verification/1.0.0-rc14.md` after the
harness is committed.

Harness имеет два режима с общей конфигурацией и teardown:

- `source`: 40 локальных UDP responders для exact Q2PRO-X browser/status;
- `webrtc`: на отдельном Linux test host запускает 40 лёгких peer processes с
  уникальными нейтральными alias `room.q2test.server01..40`, каждый отвечает
  только на OOB ping/status.

Все процессы принадлежат одной process group и завершаются в `finally`; test
host не входит в измерения client CPU/RSS. ICE поступает через stdin и не
пишется в evidence.

- [ ] **Step 1: 40-endpoint deterministic source test.**

Запустить exact supported Q2PRO-X с чистым профилем и HTTP source из 40
контролируемых loopback endpoints. Packet capture доказывает `status` ко всем
40, доступность последних строк после Refresh и один source tuple для
`status -> connect`.

- [ ] **Step 2: multiroom routing test.**

Две реальные комнаты одновременно отвечают настоящим `status`; выполнить
`connect A -> disconnect -> refresh -> connect B`. Ни один пакет A не приходит
на endpoint B; имена/map/player count получены от настоящих серверов.

В том же запуске разрешить все поддерживаемые live aliases и проверить
уникальность concrete PeerId. Любой duplicate блокирует release: скрывать одну
комнату или считать virtual PeerId достаточным запрещено.

- [ ] **Step 3: 30-minute resource/churn test Linux.**

На отдельном test host запустить `webrtc` mode, на client — 40 controlled
targets, repeated add/drop, active/inactive close. Зафиксировать RSS, CPU,
fd/thread count каждую минуту. PASS: один runtime, без устойчивого роста после
прогрева, add-on <1 GiB RSS и <75% одного logical CPU. Teardown подтверждает
отсутствие controlled peer processes на test host.

- [ ] **Step 4: тот же gate Windows.**

Зафиксировать process tree, private bytes, working set, handles, threads,
`pump_gap_max_ms`. PASS: один gateway/одно WebView2 tree, add-on <1 GiB private
bytes, <75% одного logical CPU, без секундных pump gaps и без роста после
прогрева.

- [ ] **Step 5: записать evidence без секретов и commit.**

```bash
git add docs/verification/1.0.0-rc14.md
git commit -m "test: verify shared gateway capacity"
```

## Task 8: Документация, release и живое принятие

**Files:** modify `VERSION`, `CHANGELOG.md`, `README.md`, `INSTALLATION.md`,
`docs/ARCHITECTURE.md`, `docs/RELEASE.md`,
`docs/adr/0004-q2pro-x-custom-source-room-hub.md`, `linux/README.md`,
`linux/docs/ARCHITECTURE.md`, `windows/README.md`,
`windows/coordinator/README.md`, `windows/product/README.md`,
`windows/product/README.txt`, `windows/product/README-PORTABLE.txt` and the
existing Linux/Windows package allowlist tests.

- [ ] **Step 1: принять архитектуру в документах.**

ADR 0004 ссылается на новый дизайн и удаляет нормативные `3+2`/per-room
process claims. README/ARCHITECTURE объясняют один runtime, динамический Custom
source, Refresh, empty-source behavior, явный capacity failure и rollback.

- [ ] **Step 2: bump следующей RC и CHANGELOG.**

Один общий `CHANGELOG.md`, новый раздел содержит: все совместимые комнаты,
shared runtime, lower Windows memory/process count, isolated close, WebRTC-NET
WASM cleanup и отсутствие ручной настройки Q2PRO-X. Не создавать changelog на
каждый прогон.

- [ ] **Step 3: обновить package allowlists и smoke.**

Windows portable/setup и Linux archive содержат только exact binaries,
runtime, licenses, VERSION и пользовательские README. Старые per-room adapter
артефакты удаляются из payload только если Task 3 доказал, что они больше не
нужны; новый Linux payload содержит direct shared gateway без adapter.

- [ ] **Step 4: full Linux release chain и live gate.**

Fresh archive/install: все текущие q2/q2ded видны, status/map/action,
disconnect/switch, active/inactive close, gateway crash/recovery, Q2 остаётся
открытым, нет orphan/session dirs/credentials.

- [ ] **Step 5: full Windows release chain и live gate.**

Fresh portable ZIP и setup на чистой Windows: тот же сценарий, одно WebView2
tree, normal exit/uninstall, no credentials, no 1 Hz regression.

```powershell
pwsh .\windows\product\build-payload.ps1 `
  -WebView2Package C:\deps\microsoft.web.webview2.1.0.4129.50.nupkg `
  -WebRtcNetSourceDir C:\deps\WebRTC-NET-q2prox-shared `
  -WebRtcNetReleaseDir C:\deps\webrtc-release `
  -OutputDir C:\q2dz-release\rc14
```

После установки rc14 запустить сохранённый rc13 `setup/Install.cmd`, проверить,
что `current.json` указывает на целую rc13, затем снова запустить rc14
`setup/Install.cmd`. PASS: оба payload целые, downgrade и return не создают
mixed-version tree, `q2-path.txt` сохраняется.

- [ ] **Step 6: полная автоматическая проверка.**

```bash
git diff --check
python3 tools/verify-dependencies.py \
  --webrtc-source ../WebRTC-NET-q2prox-shared \
  --linux-release ../WebRTC-NET-q2prox-shared/release-linux
python3 -m unittest linux/tests/test_launcher.py -v
pwsh -NoProfile -File windows/product/tests/catalog.tests.ps1
pwsh -NoProfile -File windows/product/tests/launcher-stdin.tests.ps1
```

Windows dependency verifier из Task 4 повторить на VM. Также выполнить штатные
Linux build/package/reproducibility и Windows exact build/lifecycle/package/
reproducibility scripts из `docs/RELEASE.md`.

- [ ] **Step 7: финальные reviews и исправления.**

Последовательно: `ponytail:ponytail-review`, один correctness/spec review через
`superpowers:requesting-code-review`, затем
`superpowers:verification-before-completion`. После каждой правки повторить
затронутый gate и полный smoke.

- [ ] **Step 8: финальные commits, без push.**

```bash
git add VERSION CHANGELOG.md README.md INSTALLATION.md docs linux windows \
  deps.lock.json tests
git commit -m "docs: document shared multiroom release"
git status --short --branch
```

Expected: clean q2prox worktree; новый WebRTC-NET commit отдельно указан в
`deps.lock.json`; release artifacts имеют SHA-256 и соответствуют evidence.

## Definition of Done

- Нет `3+2`, `kHubMaxRooms=5`, slot digit protocol и per-room WebView2 trees.
- Все совместимые локально обеспеченные контентом комнаты представлены без
  молчаливого усечения; 40 targets — доказанный supported сценарий.
- Один WebRTC-NET runtime обслуживает все targets; duplicate remote PeerId
  отклоняется явно.
- Q2PRO-X запускается и остаётся работающим при пустом каталоге, закрытии
  комнаты и исчерпанном global restart.
- ICE обновляется до новых peer connections; credentials не сохраняются.
- Windows WASM выдерживает 500 churn cycles без роста maps/handles/memory.
- Linux и Windows live acceptance пройдены из свежих поставок.
- Dependency lock, документация, changelog, package contents и verification
  report соответствуют exact release.
