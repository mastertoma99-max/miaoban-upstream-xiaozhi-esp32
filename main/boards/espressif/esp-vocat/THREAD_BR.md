# S3-N16R8 + H2 Thread BR 集成实验

本变体在当前小智 ESP-VoCat 板型中增加 OpenThread 主机和边界路由功能，H2 运行官方 UART RCP 固件。它不是官方 Brookesia 多应用整机固件，也不是 Matter Controller。

## 构建

使用 ESP-IDF v6.1，通过项目构建入口选择独立固件名称，避免与原版混淆：

```sh
# 相同 AFE/AEC、唤醒和表情配置的无 Thread 对照
python scripts/build.py espressif/esp-vocat --name esp-vocat-s3-n16r8 --language zh-CN

# 加入 Thread BR
python scripts/build.py espressif/esp-vocat --name esp-vocat-s3-n16r8-thread --language zh-CN
```

两个变体均保留原 ESP-VoCat 外设引脚、16 MB Flash、八线 PSRAM 配置和设备端 AEC。N16R8 的实际 8 MB PSRAM 容量在启动时检测，不由链接器测得；不能把编译成功当作 PSRAM 峰值测试通过。原 `esp-vocat` 变体继续关闭本新增功能。

## H2 接线

| S3 | H2-MINI-1U | 说明 |
| --- | --- | --- |
| GPIO8 / UART2 TX | GPIO23 / UART0 RX，模组焊盘30 | 460800、8N1、3.3 V，无硬件流控 |
| GPIO21 / UART2 RX | GPIO24 / UART0 TX，模组焊盘31 | 专用于 Spinel，不能混入自定义串口数据 |
| GND | GND | 共地 |

GPIO8/21 是本实验新增的 RCP 信号，不是官方原板已布好的 H2 接口。接板前仍应复核实际 PCB 修订和测试点。H2 必须有独立供电、去耦、正确启动绑带及适配的外接天线。当前没有自动控制 H2 EN/BOOT，也没有实现 RCP 自动更新及 PTA 线。

H2 使用同一 ESP-IDF 提交的 `examples/openthread/ot_rcp`，target 为 `esp32h2`，UART RCP 为默认配置。不要向未连接有效 RCP 的原装喵伴直接烧录合入版：底层 SDK 的 RCP 初始化失败可能触发断言或重启。

## 集成行为

- 原 Wi-Fi 管理器继续负责配网/重连；连接事件启动新增后台任务，不创建第二套 Wi-Fi 驱动或默认事件循环。
- 使用现有 STA 作为 BR backbone，启动独立 OpenThread netif 和处理任务。普通断线重连由 SDK 的接口事件处理；切换配网、销毁 STA 前先停止 BR/Thread。若2秒内未释放接口，保留 STA 并提示重试，避免失效引用。再次连接后复用 Dataset 恢复服务；该恢复流程仍需实物验证。
- `partitions/v2/16m_thread.csv` 在原布局空隙加入 `ot_nvs` 64 KiB，保持两个 `0x3f0000` 应用槽和 8 MiB assets 分区的位置。
- 初次启动生成随机 Thread Dataset 并持久保存，后续复用；不会打印网络密钥。当前没有 Dataset 导入/导出 UI，因此尚不提供加入既有家庭 Thread 网络的产品操作流程。
- 启用 OpenThread PSRAM 分配和平台消息池（1024 buffers）。保留主要 BR 能力，不附加 Web 管理界面、测试 CLI 或完整 Matter 控制器。
- BR 变体将 Wi-Fi 保持在性能模式，避免语音界面空闲时进入省电策略影响持续转发；因此待机功耗需要重新测量。
- `MiaobanThread` 日志每30秒记录内部堆、PSRAM 的空闲量/历史低点/最大连续块，以及监督任务栈余量和 Thread 角色。

## 测试边界

本次容量对照保留本仓库现有的语音与显示实现，不能代表原厂全部应用。当前上游 ESP-VoCat 代码在 IDF >=6 时已经关闭旧电容触摸组件；本变体没有通过关闭额外功能来换取容量，也没有补齐该上游缺口。SD 卡挂载、完整底座功能和声源定位等以当前工程实际实现为准，不能仅凭硬件有该器件就认定已验证。

静态构建应分别比较应用镜像、资源镜像、IRAM/DRAM 和外部静态数据。最终整机验收还需连接 S3/H2，测试语音收发、播放中唤醒、表情刷新和 Thread 转发并发，并叠加 Wi-Fi 重连、Thread 重入及 OTA 峰值。运行时的任务栈、内部最大连续块和 PSRAM 历史低点必须来自真实日志。

具体构建结果见项目 `docs/miaoban-s3-thread-capacity.md`。
