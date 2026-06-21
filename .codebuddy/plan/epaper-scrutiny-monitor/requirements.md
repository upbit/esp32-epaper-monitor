# 需求文档

## 引言

本项目将现有基于 Arduino + GxEPD2 的 2.13 英寸墨水屏 Demo 迁移并重构为基于 **ESP-IDF (PlatformIO espidf framework, platform-espressif32 ~3.50201.0)** 的低功耗墨水屏状态监控终端。设备使用 ESP32-WROOM-32，定期从内网 [Scrutiny v0.9.2](https://github.com/AnalogJ/scrutiny) 服务获取 NAS 硬盘 SMART 状态，并以单盘轮播的方式在墨水屏上展示重点健康指标（SMART 通过/失败、温度、寿命/通电时间、坏扇区等）。设备同时开启 HTTP 服务，允许用户通过浏览器获得所有硬盘的完整可视化状态视图。

主要设计目标：

- 使用 **ESP-IDF 原生 SPI/HTTP/WiFi/HTTP-Server 组件**，替代 Arduino 框架，便于后续显示位图与扩展三色墨水屏（黑/白/红）；
- 采用 **自建轻量驱动层（SSD167x/SSD168x 命令族）+ Adafruit_GFX（ESP-IDF 移植版）作为 UI/渲染层** 的分层设计：驱动层基于 `spi_master`/`gpio` 直接驱动控制器，UI 层复用成熟的 Adafruit_GFX 图形/文字/位图能力。当前面板 GDEH0213B72(SSD1675A) 的初始化与刷新波形直接移植自已验证的 GxEPD2 SSD1675A 时序，确保刷新效果一致；SSD1675A/SSD1680/SSD1683 命令高度兼容，未来三色 2.13"(GDEY0213Z98/SSD1680) 与 4.2"(GDEY042Z98/SSD1683) 仅需新增子类（分辨率/LUT/红色平面），业务层不变；
- 在 2.13"（约 250×122 像素）有限空间内做到信息密度合适、可读、必要时图形化（矩形进度条、SMART OK/WARN/FAIL 文字标记）；
- WiFi/HTTP 拉取失败、Scrutiny 服务异常、硬盘列表为空等情况都需要有明确的屏幕兜底显示；
- 因墨水屏寿命受限于刷新次数，刷新策略需在"可读性"和"屏幕寿命"之间取得平衡（单盘展示 15s、每轮播完全部硬盘一遍后检查 `FETCH_INTERVAL_SEC`，满足才触发下一次拉取，避免高频拉取浪费资源）。
- 每完成一轮所有硬盘的轮播后，先显示一次 WiFi 信息页（SSID/IP/固件版本/上次 fetch 相对时间），方便用户随时确认设备地址。

### 硬件与接线（已验证）

```
墨水屏驱动板        ESP32 DevKit
GND      --------> GND
3V3      --------> 3V3
SCK      --------> GPIO18  (VSPI SCK)
SDA(DIN) --------> GPIO23  (MOSI)
RST      --------> GPIO25
DC       --------> GPIO26
CS       --------> GPIO27
BUSY     --------> GPIO33
```

屏幕型号：2.13" V2，控制器 SSD1675A，对应 GxEPD2 的 `GxEPD2_213_B72 / GDEH0213B72`（黑白 250×122，支持局部刷新）。本项目自建驱动子类暂命名为 `Ssd1675a_213_BW`，其初始化序列、LUT 与刷新流程移植自 GxEPD2 的 `GxEPD2_213_B72` 实现（GPL-3.0，需在源码头注明出处）。

### 数据源（Scrutiny v0.9.2，字段已固化、不做容错）

> 重要：Scrutiny 自 v0.9.0 起，将设备唯一标识从 `wwn` 迁移为 **Scrutiny UUID**。详情接口的设备标识参数不再是 WWN，应从 `/api/summary` 动态取得当前设备的标识字符串后再使用。本项目对该标识仅作为**不透明字符串 `scrutiny_id`** 看待，不做语义解析。

- Base URL（可配置）：`SCRUTINY_API_BASE = http://192.168.66.2:8085/api`
- Summary 端点：`GET ${SCRUTINY_API_BASE}/summary`
- 详情端点（本期不调用，仅做未来扩展记录）：`GET ${SCRUTINY_API_BASE}/device/<scrutiny_id>/details`
- 返回结构（关键路径，固定使用）：
  - `data.summary`：对象，**key 为 Scrutiny UUID（`scrutiny_id`，不透明字符串）**，value 为单盘信息；
  - 每盘对象：
    - `device.device_name`（如 `sda`）、`device.model_name`、`device.serial_number`；
    - `smart.temp`（摄氏度，整数）；
    - `smart.power_on_hours`（小时，整数）；
    - `smart.device_status`（位掩码：0=passed，其它=有失败/警告）；
    - `smart.attrs."5".value`（Reallocated_Sector_Ct）；
    - `smart.attrs."197".value`（Current_Pending_Sector）；
    - `smart.attrs."198".value`（Offline_Uncorrectable）；
- 任一上述三类属性 > 0 即视为有坏扇区风险，叠加 `device_status` 进行整体状态判定：
  - `device_status == 0` 且 三类属性均为 0 → `OK`
  - 三类属性任一 > 0 且 `device_status == 0` → `WARN`
  - `device_status != 0` → `FAIL`

---

## 需求

### 需求 1：ESP-IDF 框架迁移与项目骨架

**用户故事：** 作为开发者，我希望将项目从 Arduino 框架迁移到纯 ESP-IDF 框架（PlatformIO `framework = espidf`，`platform = espressif32 @ ~6.7.0`，其捆绑 `framework-espidf ~3.50201.0`，即 **ESP-IDF v5.2.1**），以便获得更原生的外设与系统组件控制能力、方便扩展 HTTP server 等功能，并方便后续扩展到三色墨水屏。

> 版本说明：`~3.50201.0` 是 PlatformIO `framework-espidf` 工具包版本号，对应 **ESP-IDF v5.2.1**（并非 v4.x）。在 `platformio.ini` 中通常通过选定能捆绑该 IDF 版本的 `platform = espressif32`（如 `~6.7.0`）来锁定；如需精确钉死，也可显式声明 `platform_packages = framework-espidf @ ~3.50201.0`。

#### 验收标准

1. WHEN 用户执行 `pio run` THEN 系统 SHALL 以 `framework = espidf`、`board = nodemcu-32s`、ESP-IDF v5.2.1（`platform = espressif32@~6.7.0`，必要时配合 `platform_packages = framework-espidf@~3.50201.0`）完成编译，且不再依赖 `arduino` 框架与 `zinggjm/GxEPD2`。
2. WHEN 项目编译成功 THEN 系统 SHALL 提供基于 `app_main()` 的 ESP-IDF 入口（`src/main.cpp`，因 UI/驱动层为 C++），并通过 FreeRTOS task 组织 WiFi、HTTP 拉取、显示轮播、HTTP server 四类工作。
3. WHEN 项目存在原 Arduino 代码 THEN 系统 SHALL 移除或归档（不再参与编译）旧的 Arduino 实现，避免符号冲突。
4. WHEN 组织显示驱动与 UI 库 THEN 系统 SHALL：①将自建 EPD 驱动与渲染封装放在工程内（`lib/` 或 `components/`，随仓库版本管理、不走外部 registry）；②仅通过 `lib_deps` 引入 Adafruit_GFX 的 ESP-IDF 可用移植（例如 `lovyan03/...` 或经裁剪的 `Adafruit GFX` 纯 C++ 版本，最终以可在 `framework=espidf` 下编译为准），并锁定具体版本使构建可复现；③若所选 GFX 库无法在纯 IDF 下直接编译，则将其精简源码内置到工程 `lib/EpdGfx/`。
5. WHEN ESP-IDF 项目初始化 THEN 系统 SHALL 通过 `sdkconfig.defaults` 或 `build_flags` 启用：WiFi、`esp_http_client`、`esp_http_server`、SPI master、`cJSON`、NVS，且默认日志等级为 `INFO`。

### 需求 2：墨水屏驱动选型与初始化（自建 SSD167x/168x 驱动 + Adafruit_GFX）

**用户故事：** 作为开发者，我希望用一个自建的轻量驱动层（覆盖 SSD167x/SSD168x 命令族）驱动当前 SSD1675A (2.13" V2)，并复用成熟 GFX 库做 UI，使代码可控、UI 易开发，并为未来三色屏（SSD1680/SSD1683）以最小改动切换。

#### 验收标准

1. WHEN 进入实现阶段 THEN 系统 SHALL 采用 **分层设计**：`EpdBase`（SPI/GPIO/复位/BUSY/通用 SSD167x-168x 命令时序）→ 面板子类（`Ssd1675a_213_BW` 等，仅承载分辨率/LUT/RAM 寻址/平面数）→ `EpdGfx : Adafruit_GFX`（framebuffer + `drawPixel` + 刷新提交）。当前面板 `Ssd1675a_213_BW` 的初始化/LUT/刷新波形 SHALL 移植自已验证的 GxEPD2 `GxEPD2_213_B72`，并在源码头注明出处与 GPL-3.0 许可。
2. WHEN 驱动初始化 THEN 系统 SHALL 使用 ESP-IDF `spi_master` 在 VSPI（`SPI3_HOST`）上以 ≤ 4 MHz 时钟驱动屏幕，引脚：DC=GPIO26、RST=GPIO25、CS=GPIO27、BUSY=GPIO33、SCK=GPIO18、MOSI=GPIO23（GPIO19 MISO 不使用）。
3. WHEN 屏幕初始化完成 THEN 系统 SHALL 通过项目内 `display.hpp` 提供薄封装接口：`display_init()`、`display_show_boot(ssid, ip, version)`、`display_show_disk(const DiskView&)`、`display_show_message(title, body)`、`display_hibernate()`，屏蔽驱动细节，便于上层调用与未来替换。
4. WHEN 一次单盘渲染完成且后续 ≥ 5 秒无更新 THEN 系统 SHALL 调用驱动的 `sleep()` / `powerOff()`（进入控制器 Deep Sleep）让控制器进入低功耗。
5. IF BUSY 引脚在单次刷新中超过 30 秒仍未拉低 THEN 系统 SHALL 记录 ERROR 日志、累计失败计数；连续 3 次失败 SHALL 触发屏幕硬复位并重新初始化。
6. WHEN 未来扩展三色屏（黑/白/红）THEN `display.hpp` SHALL 通过编译期宏（如 `EPD_PANEL` / `EPD_THREE_COLOR`）切换到对应面板子类（如 `Ssd1680_213_BWR`、`Ssd1683_420_BWR`）；`EpdGfx` 在三色模式下维护黑/红双 framebuffer 平面，`drawPixel` 按颜色写入对应平面；业务层 `DiskView` → 渲染逻辑保持不变（本期不交付三色实现，仅预留接口与平面抽象）。

### 需求 3：WiFi 连接与首屏自检

**用户故事：** 作为使用者，我希望设备上电后自动连接到内置 SSID/密码的 WiFi，并在墨水屏上显示 SSID 与 IP 地址，以便确认设备已上线、便于通过 HTTP 访问；同时轮播过程中也能周期性看到 WiFi 信息页。

#### 验收标准

1. WHEN 设备上电启动 THEN 系统 SHALL 通过 ESP-IDF `esp_wifi` API 以 STA 模式使用编译期注入的 SSID/Password（`build_flags` 中 `-DWIFI_SSID=\"...\"` `-DWIFI_PASS=\"...\"`，或从 `secrets.ini` 注入）发起连接。
2. WHEN WiFi 连接成功并获得 IP THEN 系统 SHALL 在 5 秒内通过墨水屏（全刷一次）显示"启动自检页"：标题 `E-Paper Monitor`、`SSID: <ssid>`、`IP: <x.x.x.x>`、固件版本/构建时间。该自检页样式与轮播中插入的 WiFi 信息页一致（共用同一渲染函数）。
3. IF WiFi 在启动后 60 秒内仍未成功获取 IP THEN 系统 SHALL 在屏幕上显示 `WiFi: connecting...` 与目标 SSID，并继续后台无限重试。
4. WHEN WiFi 断开 THEN 系统 SHALL 通过事件回调自动重连；当再次获得 IP 时，在下一次全刷时机更新显示。
5. WHEN 启动流程完成 THEN 启动顺序 SHALL 严格为：①WiFi 连接 → ②显示启动自检页（至少 5 秒）→ ③首次 fetch（同步阻塞，最多 10 秒）→ ④进入硬盘轮播；若首次 fetch 失败，进入"无数据"兜底页并交由 `fetch_task` 后台继续重试。

### 需求 4：Scrutiny API 周期性拉取与解析

**用户故事：** 作为使用者，我希望设备连上 WiFi 后定期从 Scrutiny 拉取硬盘汇总数据，以便屏幕展示最新状态。

#### 验收标准

1. WHEN 触发条件满足（"完成一轮所有硬盘的轮播后" AND "距上次成功 fetch 已超过 `FETCH_INTERVAL_SEC` 秒"）AND WiFi 已连接 THEN 系统 SHALL 使用 `esp_http_client` GET `${SCRUTINY_API_BASE}/summary`，超时 `HTTP_FETCH_TIMEOUT_MS`（默认 10000ms）。
2. WHEN HTTP 响应码 200 AND Body 为合法 JSON THEN 系统 SHALL 使用 `cJSON` 按"引言-数据源"中固化的字段路径解析每块硬盘，构造 `Disk` 结构（`scrutiny_id`、`device_name`、`model_name`、`serial_number`、`temp`、`power_on_hours`、`device_status`、`realloc`、`pending`、`uncorrectable`、`overall_status`）。其中 `scrutiny_id` 取自 `data.summary` 的 key（**不透明字符串**，不做语义假设）。
3. WHEN 解析完成 THEN 系统 SHALL 在 mutex 保护下用新列表整体替换 `g_disks`（最多 `MAX_DISKS` = 16 块），按 `device_name` 字母序稳定排序，并更新 `g_last_fetch_ts`、`g_carousel_index = 0`。
4. IF HTTP 请求失败（连接错误、非 200、超时、JSON 解析失败）THEN 系统 SHALL 记录 ERROR 日志、保留上一次成功数据继续轮播；连续失败 ≥ 5 次时，屏幕角落显示 `STALE`，HTML 页 header 显示红色提示。
5. WHEN HTTP Body 较大（>16KB）THEN 系统 SHALL 使用 `esp_http_client` 流式读取（`esp_http_client_read` 分块到动态扩容 buffer），避免一次性大块 `malloc`。
6. IF Scrutiny 返回的 `data.summary` 为空对象 THEN 系统 SHALL 将 `g_disks` 清空，屏幕展示 `No disks reported` 兜底页，并继续保持轮询节奏。

### 需求 5：单盘信息轮播显示

**用户故事：** 作为使用者，我希望墨水屏每 15 秒切换一块硬盘并展示其重点 SMART 信息，每轮播完一遍所有硬盘后插入一次 WiFi 信息页方便我确认设备地址；同时只有当距上次拉取已过去足够长时间时，才去 Scrutiny 拉取最新数据，以节省资源。

#### 验收标准

1. WHEN `g_disks` 非空 THEN 系统 SHALL 按索引顺序每 `DISPLAY_PER_DISK_SECONDS`（默认 15）秒切换一块硬盘并在墨水屏上展示。
2. WHEN 单盘渲染 THEN 系统 SHALL 优先使用驱动的局部刷新（partial update）；当出现下述任一情况时改为一次全刷以消除残影：①设备启动后第一次绘制；②累计局刷计数 ≥ `PARTIAL_REFRESH_BUDGET`（默认 20）；③上一帧与当前帧布局发生变化（如硬盘数变更，或在硬盘页与 WiFi 信息页之间切换）。
3. WHEN 渲染单盘页 THEN 系统 SHALL 至少展示：
   - 顶部状态条：`<idx>/<total>` 序号、整体健康标记（`OK` / `WARN` / `FAIL` 文字标签）、相对时间 `Tm ago`（最近一次 fetch 至今分钟数）；
   - 设备标识：`device_name`（如 `sda`）+ 型号缩写（裁切至屏宽）；
   - 温度：数字 + `°C`，附 0–60°C 映射的水平矩形进度条；
   - 寿命：`power_on_hours` + 等效"≈ X d"或"≈ X y"；
   - 坏扇区：`Realloc=<n> Pend=<n> Uncorr=<n>`，任一 > 0 时整体状态升级为 `WARN`，配合 `device_status != 0` 升级为 `FAIL`。
4. WHEN 信息无法在 250×122 内完整放下 THEN 系统 SHALL 优先省略型号缩写与"等效天数"，但 `device_name`、整体状态、温度三项必须保留。
5. WHEN 完成一轮所有硬盘的轮播（`g_carousel_index` 从 0 走到 `N-1`）THEN 系统 SHALL 在切换到下一轮之前插入一帧 WiFi 信息页（停留 `DISPLAY_PER_DISK_SECONDS` 秒），内容与启动自检页一致：`SSID`、`IP`、固件版本、`Last fetch: Xm ago`、`Disks: N`。
6. WHEN 完成一轮所有硬盘的轮播（并展示完 WiFi 信息页） AND 距上次成功 fetch 已超过 `FETCH_INTERVAL_SEC`（默认 600 秒）AND WiFi 已连接 THEN 系统 SHALL 通过 `xTaskNotify` 或事件触发 `fetch_task` 拉取一次最新数据；拉取完成（无论成功/失败）后从第 1 块硬盘重新开始轮播。若条件不满足则直接进入下一轮。
7. WHEN 单盘渲染发生在"数据已陈旧（距上次成功 fetch > `FETCH_INTERVAL_SEC` 的 2 倍）"状态 THEN 系统 SHALL 在屏幕角落显示 `STALE` 标记。
8. WHEN HTTP server 有访问 THEN 系统 SHALL 不影响轮播节奏（HTTP 与 display 任务独立调度）。

### 需求 6：HTTP Server 可视化全量视图

**用户故事：** 作为使用者，我希望通过浏览器访问设备 IP，看到所有硬盘的完整、可视化的健康状态。

#### 验收标准

1. WHEN 设备成功获取 IP THEN 系统 SHALL 启动 ESP-IDF `esp_http_server`，监听 `HTTP_SERVER_PORT`（默认 80）。
2. WHEN 用户访问 `GET /` THEN 系统 SHALL 返回 `Content-Type: text/html; charset=utf-8` 的自包含 HTML 页面（CSS 内联，无外链、无 JS 依赖），包含：
   - 顶部：标题 `E-Paper Monitor`、设备信息（SSID、IP、固件版本、上次数据更新相对时间）；
   - 卡片列表，每张卡对应一块硬盘：`device_name`、`model_name`、`serial_number`、整体状态徽章（绿 `OK` / 黄 `WARN` / 红 `FAIL`）、温度（数字 + CSS 渐变温度条）、`power_on_hours` 与等效天/年、`Realloc/Pending/Uncorrectable` 三项；
   - 页面默认深色背景、12 列响应式 grid。
3. WHEN 用户访问 `GET /api/disks` THEN 系统 SHALL 返回当前缓存的硬盘数据（JSON），结构为 `{"last_fetch_ts":..,"uptime_s":..,"disks":[...]}`，供二次集成。
4. WHEN 用户访问 `GET /healthz` THEN 系统 SHALL 返回 JSON `{"status":"ok","last_fetch_ts":..,"disks":N,"uptime_s":..,"consec_fetch_fail":..}`，HTTP 状态码 200。
5. IF 缓存为空（尚未成功 fetch）THEN `GET /` SHALL 返回友好的"数据尚未就绪"页面，展示下一次预计拉取时间与当前失败计数。
6. WHEN HTTP 处理函数访问 `g_disks` THEN 系统 SHALL 通过 mutex 加锁；单次响应在 ESP32 上 RAM 占用 ≤ 32KB（chunked send）。

### 需求 7：任务划分、并发与异常处理

**用户故事：** 作为开发者，我希望各类工作以独立 FreeRTOS 任务运行，互不阻塞，并具备基本的异常恢复能力。

#### 验收标准

1. WHEN 系统启动完成 THEN 系统 SHALL 至少创建以下 FreeRTOS 任务：(a) `fetch_task` 周期拉取（栈 6KB，等待事件 + 超时唤醒）、(b) `display_task` 负责轮播与渲染（栈 6KB）、(c) `esp_http_server` 内部任务；WiFi 使用事件回调，不单独建任务。
2. WHEN 多个任务访问 `g_disks` THEN 系统 SHALL 使用 `SemaphoreHandle_t` 互斥锁保护读写，避免脏读。
3. WHEN 任意任务发生不可恢复异常 THEN 系统 SHALL：HTTP 连续失败 ≥ 5 次维持旧数据 + STALE；屏幕初始化失败 30 秒内重新初始化；屏幕连续失败 ≥ 10 次调用 `esp_restart()` 兜底。
4. WHEN 设备运行 THEN 系统 SHALL 每 60 秒一次打印 `uxTaskGetStackHighWaterMark` 与 `esp_get_free_heap_size`，便于调优。
5. WHEN 进入轮播间隙 THEN `display_task` SHALL 使用 `vTaskDelay` 让出 CPU。

### 需求 8：可配置项与构建参数

**用户故事：** 作为开发者，我希望关键参数集中可配。

#### 验收标准

1. WHEN 项目构建 THEN 系统 SHALL 通过 `include/config.h` 提供以下默认常量，并允许 `build_flags` 或 `secrets.ini` 覆盖：
   - `WIFI_SSID`、`WIFI_PASS`
   - `SCRUTINY_API_BASE`（默认 `"http://192.168.66.2:8085/api"`，不含尾部 `/`，业务侧拼接 `/summary` 等子路径）
   - `DISPLAY_PER_DISK_SECONDS`（默认 15）
   - `FETCH_INTERVAL_SEC`（默认 600，每轮播完一遍硬盘后才检查是否到达该间隔）
   - `HTTP_FETCH_TIMEOUT_MS`（默认 10000）
   - `MAX_DISKS`（默认 16）
   - `PARTIAL_REFRESH_BUDGET`（默认 20）
   - `HTTP_SERVER_PORT`（默认 80）
2. WHEN 用户希望避免将 WiFi 凭据提交到仓库 THEN `platformio.ini` SHALL 通过 `extra_configs = secrets.ini` 加载 `secrets.ini`，且 `.gitignore` 默认忽略 `secrets.ini`；仓库中提供 `secrets.example.ini` 作模板。
3. WHEN 任意配置变更 THEN 仅需重新编译，无需修改业务源码。

### 需求 9：日志与可观测性

**用户故事：** 作为开发者，我希望通过串口日志清晰看到设备的运行轨迹。

#### 验收标准

1. WHEN 关键事件发生（WiFi 状态变化、HTTP 拉取开始/结束/失败、屏幕全刷/局刷、轮播切换、HTTP 请求到达）THEN 系统 SHALL 通过 `ESP_LOGI/W/E` 输出结构化日志，含 tag（如 `WIFI`/`FETCH`/`DISP`/`HTTPD`）与关键字段。
2. WHEN 串口连接 THEN 系统 SHALL 在启动头部打印固件版本、构建时间、配置摘要（SSID、`SCRUTINY_API_BASE`、节奏参数）。
3. IF 日志频率过高 THEN 系统 SHALL 通过等级控制（`DEBUG` 默认关闭）避免刷屏。

### 需求 10：低功耗考虑（软目标）

**用户故事：** 作为使用者，我希望设备在长期通电运行下保持较低功耗与较长墨水屏寿命。

#### 验收标准

1. WHEN 屏幕完成一次刷新 THEN 系统 SHALL 调用驱动 `sleep()` 让控制器进入低功耗。
2. WHEN 当前为两次刷新之间的等待期 THEN `display_task` SHALL 使用 `vTaskDelay` 让出 CPU，且不持续读取 BUSY/SPI。
3. WHEN 在一轮 10 轮的展示周期内 THEN 系统 SHALL 尽量采用局刷，仅在需求 5.2 列出的情形下触发全刷，以保护墨水屏寿命。
4. 该需求为软目标：若与功能冲突，以功能为准。

---

## 边界与开放问题

1. **驱动选型已确定**：采用 **自建 SSD167x/168x 薄驱动层 + Adafruit_GFX(ESP-IDF 移植) UI 层** 的分层方案（不采用 CalEPD：其无 GDEH0213B72/SSD1675A 类、且 IDF5 支持仅在功能分支）。当前 B72 波形移植自 GxEPD2（GPL-3.0），需保留出处声明。本期仅交付黑白 2.13"，三色为预留接口。
2. **JSON Schema 已固化**：基于 Scrutiny v0.9.2，按"数据源"小节给出的字段路径解析，不做容错；若实际部署版本不同，由开发者升级 Scrutiny 或修改解析代码。
3. **设备标识**：Scrutiny 自 v0.9.0 起从 WWN 迁移为 Scrutiny UUID；本项目将 `data.summary` 的 key 视为**不透明字符串 `scrutiny_id`**，不解析其格式，未来用于详情接口拼接。
4. **三色屏扩展**：本期不交付，驱动层通过编译期宏预留切换点（需求 2.6）。
5. **OTA / Web 配网**：本期不交付，后续作为新需求接入。
6. **绝对时间**：本期使用 `esp_timer` 的相对秒数计算"距上次更新分钟数"；不引入 SNTP，HTTP 页与屏幕角落均以相对时间显示。
