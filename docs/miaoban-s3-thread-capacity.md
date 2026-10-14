# 喵伴 S3-N16R8 + H2 Thread BR 合入与容量测试

日期：2026-10-14。目标：ESP32-S3-WROOM-1-N16R8（16 MiB Flash / 8 MiB PSRAM）与 ESP32-H2-MINI-1U UART RCP。

## 结论

**本次合入版应用及资源镜像均通过编译和分区大小检查，能够装入16 MiB Flash，并保留双 OTA。** 本次应用镜像为 **2.852 MiB**，每个 OTA 槽还剩 **1.085 MiB**。

内部 RAM 已通过静态链接容量检查；**8 MiB PSRAM 的运行时峰值、音频与 Thread 并发稳定性尚未实测，不能据此声称整机全部功能已通过硬件测试。** 此次基于本地小智 ESP-VoCat 工程，不包含官方 Brookesia 多应用整机固件。

## 1. 实际对照结果

两版均启用 AFE、WakeNet、设备端 AEC、中文和表情资源，均为 S3、16 MiB Flash、八线80 MHz PSRAM 配置。两版使用相同组件版本，差异主要是 Thread BR、相关 IPv6/mDNS/DTLS 配置及独立 Thread NVS 分区。

| 项目 | 无 Thread 对照版 | 合入 Thread BR |
| --- | ---: | ---: |
| 应用 `.bin` | 2,554,912 bytes（2.437 MiB） | 2,991,024 bytes（2.852 MiB） |
| 每个 OTA 槽容量 | 3.938 MiB | 3.938 MiB |
| 每槽剩余 | 1.501 MiB | 1.085 MiB |
| 表情/字体/语音模型资源镜像 | 2,968,883 bytes（2.831 MiB） | 2,968,883 bytes（2.831 MiB） |
| assets 分区容量 | 8 MiB | 8 MiB |
| 内部 DIRAM 静态占用 | 151,278 bytes（147.73 KiB） | 186,242 bytes（181.88 KiB） |
| DIRAM 链接区域尚未静态占用 | 190,482 bytes（186.02 KiB） | 155,518 bytes（151.87 KiB） |

Thread 合入使本次应用增加 **436,112 bytes（0.416 MiB）**，内部 DIRAM 静态占用增加 **34,964 bytes**。DIRAM 报告区域总量为 341,760 bytes，是当前链接配置区域；其“未静态占用”不是启动后的可用堆，不能直接当成运行期余量。S3 的 DIRAM 数据也不能套用先前经典 WROOM 的 IRAM 百分比。

H2 官方 RCP 独立构建镜像为 **263,632 bytes（约257.45 KiB）**，使用默认2 MiB Flash构建配置。它烧录在H2上，不计入上述S3应用；本版没有把RCP升级镜像嵌入S3，也没有实现RCP自动更新。

## 2. PSRAM 评估边界

- S3-N16R8 的物理 PSRAM 是8 MiB；编译器不会测量实物容量或运行时可用堆。启动日志会核对实际检测值。
- 启用 `OPENTHREAD_PLATFORM_MALLOC_CAP_SPIRAM` 和平台消息池，消息缓冲数量1024。按当前SDK默认32位指针及128 bytes缓冲块，消息池本体约128 KiB、索引约4 KiB，合计约132 KiB，另有分配器开销。
- 132 KiB只代表该消息池，未包括协议栈实例、网络队列、任务栈、mDNS、TLS或语音/显示资源。SDK分配器在PSRAM不足时还可能回退到内部RAM，因此必须同时观察内部堆。
- 新增监督任务栈6144 bytes、OpenThread任务栈8192 bytes；这些默认占用内部内存，还有TCB和队列等开销。屏幕及音频DMA也不能单凭PSRAM余量判断。
- 360×360 RGB565单帧253.125 KiB、双帧506.25 KiB；资源镜像存于Flash，不意味着启动时整份复制到PSRAM。

据此可以确认Flash可容纳本次配置；PSRAM具有实现基础，但完整运行余量仍待实测。不能把本结果扩大到所有可选模型、全部多媒体功能、完整Matter Controller或官方Brookesia多应用同时运行。

## 3. 合入内容

独立构建名称：`esp-vocat-s3-n16r8-thread`。原 `esp-vocat` 保持关闭新增BR功能；另设 `esp-vocat-s3-n16r8` 作为开启同样设备端AEC的对照。

- 保留现有S3喵伴外设引脚。UART2使用新增GPIO8/TX → H2 GPIO23/RX，GPIO21/RX ← H2 GPIO24/TX，460800、8N1、无硬件流控、3.3 V、共地。
- 复用小智Wi-Fi STA，先建立IPv6链路本地地址，再启动OpenThread与BR。配网销毁STA前通过生命周期钩子停止BR/Thread，重连后复用持久Dataset恢复；这些恢复路径尚未上板验证。
- 新增64 KiB `ot_nvs`，使用原分区空隙，保留两个`0x3f0000` OTA槽和8 MiB assets位置。
- 新建随机Thread Dataset，避免采用统一的示例网络密钥；本版尚无Dataset导入/导出UI，加入既有家庭Thread网络的产品流程仍待实现。
- 加入每30秒内存/角色日志；BR变体保持Wi-Fi性能模式，持续转发的待机功耗需另测。
- Windows/Ninja改用参数文件，解决原版及合入版都遇到的命令行长度限制；工作区中文路径通过任务专属英文构建目录规避，共享SDK未修改。

H2使用同一SDK提交的官方 `examples/openthread/ot_rcp`。必须有有效RCP、正确EN/BOOT绑带和外接天线；当前无EN/BOOT自动控制、自动更新或PTA接线。没有连接H2时，底层RCP初始化可能触发断言/重启，不应把合入版直接烧录到未改造的原装喵伴。

## 4. 已完成检查与未测项

已完成：两种S3配置的完整构建、应用/资源分区检查、链接容量分析、H2 RCP构建、改动文件差异检查及新代码格式检查。现有宿主测试按模块/环境复测累计111项通过；MCP C++测试组因WSL缺少cJSON/mbedTLS开发头文件跳过。此前Windows宿主测试遇到编码/旧编译器/临时目录问题，WSL的CI测试也遇到每次启动清空临时Python命令适配的问题；调整运行环境后相应测试通过，没有修改测试源码掩盖失败。

未完成：S3/H2烧录、PSRAM初始化实测、Spinel握手、Thread节点入网与跨网转发、音频/屏幕并发、Wi-Fi重连、OTA及长时间压力测试。本地没有本次S3+H2目标整机的串口日志。

本仓库上游在IDF >=6时已关闭旧的机身电容触摸组件；本次保留该现状，没有通过裁剪更多功能来获得容量。SD、完整底座操作和声源定位等是否实现，应按当前小智工程实际代码判断。本结果不等于原厂整机全部功能验收。

上板后至少采集 `MiaobanThread` 的 `internal_free/internal_min/internal_largest`、`psram_free/psram_min/psram_largest`，并记录全部相关任务栈余量。覆盖待机、录音、播放/打断、动画、Thread流量，以及TLS连接、Wi-Fi重连和OTA峰值；是否合格按实际最大连续申请、丢帧及网络丢包确定。

## 5. 复现与证据

ESP-IDF：本机v6.1，Git提交 `fff9895c82d744c7237be8847347bdd1b07c6643`；GNU Xtensa工具链15.2.0。

```sh
python scripts/build.py espressif/esp-vocat --name esp-vocat-s3-n16r8 --language zh-CN
python scripts/build.py espressif/esp-vocat --name esp-vocat-s3-n16r8-thread --language zh-CN
```

具体接线和生命周期说明见 [板型说明](../main/boards/espressif/esp-vocat/THREAD_BR.md)。机器可读数据见 [容量证据](./evidence/miaoban-s3-thread-capacity-20261014.json)。本地构建镜像、map、ELF、配置和日志保存在工作区上级 `outputs/20261014-s3-thread/`；这些大文件未提交Git。

合入镜像 SHA-256：`170e2c6eba361beb0c4cbe32c8e16b5c8cca5558eccc881c83ebda3438bf6145`。

资料参考：[官方BR SDK](https://github.com/espressif/esp-thread-br)、[同一SDK提交的ot_br](https://github.com/espressif/esp-idf/tree/fff9895c82d744c7237be8847347bdd1b07c6643/examples/openthread/ot_br)、[官方喵伴V1.2](https://docs.espressif.com/projects/esp-dev-kits/zh_CN/latest/esp32s3/esp-vocat/user_guide_v1.2.html)。
