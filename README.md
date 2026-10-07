# Communication Framework

这是一个独立于具体产品项目的长期通讯框架学习工程。

目标不是复制以前项目中的代码，而是从零实现一套能够迁移到 Linux、RTOS 和嵌入式屏端的通讯框架，并理解每个模块背后的设计取舍。

## 设计目标

- 使用纯 C99 实现可移植核心。
- 核心层不依赖 LVGL、pthread 或具体 RTOS。
- 支持 UART/TCP 等连续字节流输入。
- 正确处理拆包、粘包、垃圾字节和 CRC 错误。
- 使用结构化消息跨任务传递业务事件。
- 支持请求序号、应答匹配、超时和重试。
- 明确缓冲区及消息的内存所有权。
- 能在电脑上运行自动测试，再移植到设备。

## 目标数据链路

```text
应用层
  -> 消息管理层（请求、应答、超时、重试）
  -> 消息队列
  -> 协议编码器
  -> 传输层（UART/TCP/模拟传输）

传输层
  -> RingBuffer
  -> 增量协议解析器
  -> 消息队列
  -> 消息管理层
  -> 应用层回调
```

## 目录结构

```text
communication_framework/
├── include/       对外头文件
├── src/           与操作系统无关的核心实现
├── tests/         电脑端单元测试和异常流测试
├── examples/      最小收发示例
├── ports/
│   ├── linux/     pthread、条件变量及Linux传输适配
│   └── rtos/      RTOS队列、线程、时间等适配接口
└── docs/          学习笔记、协议和设计决策
```

## 学习方式

每次只完成一个可测试的小模块：先理解职责和边界，再实现代码，最后用正常与异常用例验证。不要在前一个模块未验证时继续叠加功能。

详细顺序见 [docs/roadmap.md](docs/roadmap.md)。

## 运行字节流级集成测试

在 Linux 仓库根目录执行：

```sh
mkdir -p build
gcc -std=c99 -Wall -Wextra -Werror -pedantic \
    -Iinclude -Iexamples/appliance_manager -Iports/linux \
    src/*.c examples/appliance_manager/*.c ports/linux/*.c \
    tests/test_appliance_stream_integration.c -pthread \
    -o build/test_appliance_stream_integration
./build/test_appliance_stream_integration
```

该测试串联状态查询、TX 队列、编码器、模拟对端、RingBuffer、Parser、RX 队列、
Message Manager 和业务模型通知，覆盖拆包、粘包、错误恢复、重复回复及超时重试。
使用真实 pthread 队列，但在单线程内按确定顺序推进，并显式传入模拟时间。
测试没有接入实际串口、网络或 LVGL，不能替代任务并发测试和电源板联调。

## 运行 Linux 多任务示例

```sh
make -C examples/linux_runtime
./build/linux_runtime
./build/linux_runtime retry
./build/linux_runtime rx-full
```

主线程串行管理请求和业务模型，收发线程通过 pthread 帧队列交接数据，模拟
设备通过本地流式 socket 返回响应。示例使用单调时钟驱动重试，并处理队列满、
断连和线程停止回收。更多场景、线程职责及移植边界见
[Linux 运行示例说明](examples/linux_runtime/README.md)。
