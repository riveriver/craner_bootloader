# 固件确认机制说明

## 1. 目的

本文说明当前 Bootloader 中“固件确认”的实现方式，重点描述业务代码如何配合 OTA 元数据完成：

- 新固件写入后的状态标记
- 启动阶段的确认计数
- 确认失败后的自动回退
- 业务侧应如何理解“确认成功”

## 2. 设计概述

当前工程采用的是“双分区 + 元数据 + 启动试运行次数”的确认模型。

核心思想如下：

1. OTA 下载完成后，Bootloader 不会立即把新固件标记为最终有效。
2. 新固件会先被写入为 `PENDING` 状态。
3. 每次上电启动时，Bootloader 会检查该 Pending 状态，并增加一次 `boot_count`。
4. 如果 Pending 固件连续启动次数未超过阈值，则继续允许启动。
5. 如果超过确认次数仍未被业务系统认可，则自动回退到另一个可启动分区。

当前确认阈值定义在 [application/ota_flash_service.c](../application/ota_flash_service.c) 中：

- `OTA_FLASH_CONFIRM_MAX_ATTEMPTS = 3`

## 3. 相关元数据

确认逻辑主要依赖 `ota_flash_meta_t`：

- `active_slot`：当前要启动的分区
- `ota_request`：是否存在 OTA 请求
- `target_slot`：OTA 目标分区
- `boot_count`：Pending 固件已启动次数
- `image[i].state`：镜像状态，`EMPTY / VALID / PENDING`

对应定义在 [application/ota_flash_service.h](../application/ota_flash_service.h) 中。

## 4. OTA 完成后的状态写入

当 YMODEM 下载和 Flash 写入都成功后，业务流程会进入文件结束处理：

- 代码位置： [application/ota_manage_service.c](../application/ota_manage_service.c)
- 关键调用：`ota_flash_mark_pending()`

该函数会把新镜像写成 Pending 状态，并同步更新元数据：

- `image[slot].address`：目标地址
- `image[slot].size`：镜像大小
- `image[slot].crc32`：镜像 CRC
- `image[slot].state = OTA_FLASH_IMAGE_PENDING`
- `active_slot = slot`
- `ota_request = OTA_FLASH_OTA_REQUEST_NONE`
- `target_slot = OTA_FLASH_SLOT_NONE`
- `boot_count = 0`

这一步的含义是：

> 新固件已经写入完成，但仍处于“待确认”状态。

## 5. 上电启动时的确认流程

上电后，`ota_flash_service_init()` 会读取元数据，并调用 `ota_flash_handle_pending_boot()` 处理 Pending 状态。

### 5.1 不是 Pending

如果当前 `active_slot` 对应镜像不是 `PENDING`，则直接认为元数据正常，继续启动。

### 5.2 是 Pending

如果当前镜像状态是 `PENDING`：

1. 检查 `boot_count`
2. 如果 `boot_count < OTA_FLASH_CONFIRM_MAX_ATTEMPTS`：
   - `boot_count++`
   - 写回元数据
   - 继续尝试从该分区启动
3. 如果 `boot_count >= OTA_FLASH_CONFIRM_MAX_ATTEMPTS`：
   - 触发回退
   - 切换到另一个可启动分区
   - 清空 Pending 镜像状态
   - 将 `boot_count` 复位为 0

相关逻辑在 [application/ota_flash_service.c](../application/ota_flash_service.c) 的 `ota_flash_handle_pending_boot()`、`ota_flash_rollback_pending_slot()` 中。

## 6. 业务代码中的“确认”含义

当前工程里，业务侧并没有一个单独的“确认成功”函数去主动把 Pending 改成 Valid。

所以现在的确认语义更接近于：

- **系统启动成功并持续运行，未触发回退**，就视为该固件已经通过确认
- **如果启动后仍然异常重启，超过确认次数就自动回退**

也就是说，当前实现是一个“试运行确认”模型，而不是“应用显式 ACK 确认”模型。

## 7. 业务流程如何理解确认

从业务视角看，固件确认可理解为以下顺序：

1. 新版本通过 OTA 下载并写入目标分区
2. Bootloader 写入 Pending 元数据
3. 设备重启并尝试启动新固件
4. 若新固件能正常运行，系统不会进入回退分支
5. 若新固件连续失败，Bootloader 会在达到确认阈值后自动回退

因此，业务代码应将“确认成功”理解为：

> 新固件在多个启动周期内稳定运行，且未触发 Pending 回退。

## 8. 异常回退逻辑

除了 Pending 确认回退外，业务流程中还有一种更早的回退：

- 当 OTA 写入过程检测到镜像非法、CRC 不一致或 Flash 写失败时
- `ota_manage_service.c` 会调用 `ota_manage_recover_boot_slot()`
- 该函数会把 `active_slot` 切回另一个可启动分区，并清理失败分区信息

这属于“写入阶段回退”，不是确认阶段回退。

## 9. 当前实现的结论

当前固件确认机制可以总结为一句话：

> 新固件写入后先标记为 Pending，系统在启动时根据 `boot_count` 进行试运行确认；若超过确认次数仍未稳定，则自动回退到旧固件。

## 10. 若需要更严格的业务确认

如果后续希望实现“应用启动成功后显式确认”，建议新增一个业务接口，例如：

- `ota_flash_confirm_current_slot()`
- 或 `ota_flash_confirm_slot(slot)`

其职责可以是：

- 将当前分区镜像状态从 `PENDING` 改为 `VALID`
- 清零 `boot_count`
- 清空 `ota_request` 和 `target_slot`
- 写回元数据

这样就能把“试运行确认”升级为“显式确认”。

## 11. 相关文件

- [application/ota_flash_service.c](../application/ota_flash_service.c)
- [application/ota_flash_service.h](../application/ota_flash_service.h)
- [application/ota_manage_service.c](../application/ota_manage_service.c)
- [application/ota_manage_service.h](../application/ota_manage_service.h)

