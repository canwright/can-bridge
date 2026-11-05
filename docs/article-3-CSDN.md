# CAN 盒子通用桥：让 CANape / ZCANPRO 用上周立功、PEAK、创芯的盒子（附 USBCAN-2E-U 收不到报文排查全过程）

> 本文记录一个真实项目：把自己手里的国产 CAN 盒子，转接给只认某个品牌的
> 上位机软件使用。文中包含三个把我坑了两天的具体问题、完整的排查方法，
> 以及一份"用两个数字判断 CAN 故障方向"的速查表。
> 即使不用这个工具，排查 CAN 硬件问题也可以直接参考。

---

## 一、问题背景

手上的设备很普通：

| 设备 | 型号 | 价格级别 |
| --- | --- | --- |
| CAN 盒子 1 | 周立功 USBCAN-2E-U（2 通道） | 两千左右 |
| CAN 盒子 2 | 创芯 CANalyst-II（2 通道） | 几百 |
| 调试软件 | ZCANPRO | 免费 |

用 ZCANPRO 一切正常，收发报文都没问题。

但要做 ECU 标定，得用 **CANape**。打开 CANape 的设备列表一看：

```
Vector VN1610
Vector VN1630A
Vector VN1640A
```

**只有 Vector 的硬件。** 插着周立功的盒子，CANape 完全看不见。

买一块 VN1630A？两万多。

于是花了两天，写了个 DLL 转发层：**让 CANape 以为插的是 VN1630，实际把每一帧转发给那块两千块的国产盒子。**

> **本文涉及的源码已开源（Apache-2.0）：https://github.com/canwright/can-bridge**
>
> 下面每段代码在仓库里都有完整实现，文末附逐文件链接。

---

## 二、为什么软件会"只认某个牌子"

这不是软件故意排外，是 Windows 加载 DLL 的机制决定的。

上位机要跟 CAN 盒子通信，必须加载厂家提供的动态库：

| 上位机 | 加载的 DLL | 使用的接口 |
| --- | --- | --- |
| ZCANPRO | `zlgcan.dll` / `USBCAN.dll` | `ZCAN_*` / `VCI_*` |
| CANape / CANoe | `vxlapi.dll` | `xlCanTransmit` / `xlReceive` |
| PCAN-View | `PCANBasic.dll` | `CAN_Initialize` / `CAN_Read` |
| 很多刷写工具 | `ControlCAN.dll` | `VCI_*` |

Windows 的 DLL 搜索顺序：

```
① 程序所在目录
② 系统目录 System32 / SysWOW64
③ PATH 环境变量里的目录
```

所以只要在 **①** 的位置放一个**同名、但内容是我们自己的** DLL，
上位机就会加载它 —— 而它以为拿到的是原厂的。

这就是 **DLL 门面（shim）**。原理极简单，难的是**把这个门面做得足够真**。

---

## 三、整体架构

```
        CANape（以为在跟 VN1630 说话）
                │  调用 xlCanTransmit / xlReceive
                ▼
        假 vxlapi.dll（门面）
                │
                ▼
        统一的中立帧模型 CtFrame
        { id, ext, rtr, dlc, len, data[64], 时间戳, 方向, 通道 }
                │
                ▼
        厂商适配层（周立功 / PEAK / 创芯 / 广成 / Kvaser ...）
                │
                ▼
        真正插着的那个盒子
```

**关键设计：中间一定要有一层厂商中立的模型。**

如果直接拿周立功的结构体当内部表示，那么任何一处字段错位都会全线崩
—— 这正是我下面第一个坑的来源。

---

## 四、踩坑一：一个结构体，16 字节还是 18 字节

### 现象

`VCI_InitCAN` 返回成功，`VCI_StartCAN` 返回成功，但**一帧报文都收不到**。

### 我写错的代码

周立功 VCI 接口里有个初始化结构体 `VCI_INIT_CONFIG`。我凭记忆写成了 18 字节：

```c
typedef struct {
    BYTE  Mode;
    BYTE  InitBytes;     /* ← 这两行是我自己加的，官方没有 */
    DWORD AccCode;
    DWORD AccMask;
    DWORD Reserved;
    BYTE  Filter;
    BYTE  Timing0;
    BYTE  Timing1;
    BYTE  Mode2;         /* ← 也是我编的 */
} VCI_INIT_CONFIG;
```

### 官方定义（ControlCAN.h，共 16 字节）

```c
typedef struct _INIT_CONFIG {
    DWORD AccCode;
    DWORD AccMask;
    DWORD Reserved;
    UCHAR Filter;
    UCHAR Timing0;
    UCHAR Timing1;
    UCHAR Mode;
} VCI_INIT_CONFIG, *PVCI_INIT_CONFIG;
```

### 后果：不是崩溃，是**全部字段错位 2 字节**

日志里打出来的值是这样：

```
InitCAN 原始字节: 00 00 00 00 FF FF FF FF 00 00 00 00 00 00 1C 00
InitCAN 配置:     Filter=28  Timing0=0x00  Timing1=0x88  Mode2=121
```

按 16 字节布局对照：

| 我读到的 | 实际是 |
| --- | --- |
| `Filter=28` | 真实 `Timing1 = 0x1C`（正好是 500k 的时序） |
| `Timing1=0x88` | 越过结构体末尾的**栈垃圾** |
| `Mode2=121` | 同样是栈垃圾 |

**我当时盯着这些数字分析很久，还据此得出"官方布局肯定不一样"的结论，
把排查方向带偏了两次。**

### 解决办法：加编译期断言

```c
typedef char CHECK_INIT_CONFIG[(sizeof(VCI_INIT_CONFIG) == 16) ? 1 : -1];
typedef char CHECK_BOARD_INFO  [(sizeof(VCI_BOARD_INFO)   == 80) ? 1 : -1];
typedef char CHECK_CAN_OBJ     [(sizeof(VCI_CAN_OBJ)      == 24) ? 1 : -1];
```

一行断言能抓出的问题，比我两天的日志分析还多。

> **顺便一个坑中坑**：`VCI_BOARD_INFO` 官方是 80 字节，
> 但如果文件开头有个全局 `#pragma pack(push,1)`，它会被压成 **79 字节**
> —— 字段偏移一样，只是总长度差 1。这个偏差存在很久都没人发现，
> 是断言当场抓出来的。

---

## 五、踩坑二：36MHz 和 16MHz（USBCAN-2E-U 收不到报文的真正原因）

这个坑最典型，也是**「USBCAN-2E-U 收不到报文」这个搜索词的正解**。

### 现象

- 能打开设备
- 能设置波特率（返回 1）
- 能启动通道（返回 1）
- **一帧报文都收不到**
- **错误帧刷了 74458 条**
- REC 从 0 一路涨到 130，TEC 一直是 0

```
错误码: 0x10 总线错误 / 0x04 消极错误 / 0x02 错误报警
错误类型: 填充错误、位错误、支配(控制)位错误
错误属性: 接收错误
```

### 关键判断：错误帧的数量就是诊断信息

| 接收帧数 | 错误帧数 | 说明 |
| --- | --- | --- |
| 0 | **0** | 没接总线、CAN_H/CAN_L 接反、或插错通道 |
| 0 | **上万** | **总线上有信号，但波特率或型号不对** |
| 持续增长 | 0 | 正常 |

**万级错误帧说明：总线是活的，是这个节点解错了。**
所以问题一定在波特率上，而不是接线。

### 原因：同一套接口，不同的控制器时钟

| 型号 | CAN 控制器时钟 | 波特率怎么设 |
| --- | --- | --- |
| 经典 USBCAN-II | 16 MHz | 靠 `InitCAN` 里的 `Timing0/Timing1` |
| **USBCAN-2E-U** | **36 MHz** | **靠 `VCI_SetReference` 下发** |

官方手册里写得很清楚：

> USBCAN-E/2E-U 接口卡的 CAN 控制器使用 **36MHz 时钟**，
> 用户自定义波特率时要根据该时钟频率进行计算。

以及更关键的一句：

> 波特率必须通过 `VCI_SetReference` 设置，
> `VCI_InitCAN` 里的 `Timing0/Timing1` **被硬化忽略**。

也就是说 —— **对 2E-U 来说，时序字节根本没人看。**

### 正确调用顺序

```c
/* 1. 打开设备（2E-U 的型号是 21，不是 20） */
VCI_OpenDevice(21, 0, 0);

/* 2. ★ 必须先设波特率（这一句是关键） */
DWORD baud = 0x060007;                    /* 500K */
VCI_SetReference(21, 0, 0, 0, &baud);

/* 3. 再初始化 */
VCI_INIT_CONFIG cfg = {0};
cfg.AccMask = 0xFFFFFFFF;
cfg.Filter  = 0;
VCI_InitCAN(21, 0, 0, &cfg);

/* 4. 启动 */
VCI_StartCAN(21, 0, 0);
```

改完之后：**一帧变 4650 帧。**

### 官方波特率参考码（手册第 36 页，别用公式推）

```
0x060003  1000K      0x060004   800K      0x060007   500K
0x1C0008   250K      0x1C0011   125K      0x160023   100K
0x1C002C    50K      0x1600B3    20K      0x1C00E0    10K
0x1C01C1     5K
```

我一开始想按位域反推，算出来 **2666667 bps** —— 明显是位域假设错了。

> **结论：这种表只能抄，不能推。**

---

## 六、踩坑三：关设备用错型号，会把驱动卡死

这个坑最恶劣，因为它**会让厂家自己的软件也用不了**。

### 现象

用完我写的工具之后：

- 原厂 ZCANPRO 打不开设备
- 我自己的工具也打不开
- 拔插盒子没用
- **但重装 ZCANPRO 就好使，而且不需要重启电脑**

### 破案关键

**「不用重启」这四个字。**

```
只重装软件就恢复  →  问题在【文件层 / 用户态】
必须重启电脑才恢复 →  问题在【内核驱动】
```

这一下把排查范围缩小了一大半。

### 真正的原因

代码里有个全局变量记着"打开的型号"，初值是 3。
探测阶段用型号 **21** 打开了 2E-U，但关设备时用的是这个初值：

```c
/* 错的 */
g_vci.CloseDevice(g_devType, 0);    /* CloseDevice(3, 0) —— 型号不对 */
```

型号错，驱动返回失败 —— **设备根本没关掉**。

驱动里于是留着一条"设备已被占用"的记录，而占用它的进程已经退出了。
之后谁也打不开。

### 正确写法

```c
/* ★ 用实际打开成功的型号，且无条件执行 */
DWORD closeType = g_vciRealType ? g_vciRealType : g_devType;
DWORD r = g_vci.CloseDevice(closeType, 0);
LogF("关闭设备 CloseDevice(%lu) -> %lu", closeType, r);
g_vciOpened = 0;
```

**三个要点，缺一不可：**

1. 用**实际打开成功的型号**，不是"默认值"
2. **无条件执行**，不能包在 `if (backend == XXX)` 里
   （探测阶段打开了但没选上后端时，也要关）
3. **打印返回值** —— 否则你永远不知道它有没有关成功

### 顺着这条线又发现第二个问题

继续查卸载逻辑，发现：

```c
/* 错的：只要文件大小和我的 DLL 一样，就当成"安装时新建的"，直接删 */
if (sTarget == sBridge) DeleteFileW(target);
```

问题是：装了桥之后，`USBCAN.dll` 本来就是桥的副本，**大小当然一样**。
所以一旦备份丢了（重装上位机会清掉 `.bak`），一卸载就把原厂文件删了。

删掉之后上位机没有设备插件 —— 表现就是**"卸载之后反而更坏"**。

正确做法：

```c
/* 1. 备份前先判断：要备份的这个文件是不是桥自己 */
if (IsSelfDll(target)) { /* 不备份，否则等于把桥当原厂存起来 */ }

/* 2. 卸载时先找 kerneldlls 里的原厂副本还原 */
if (FileExists(kerneldlls_copy)) CopyFile(kerneldlls_copy, target);

/* 3. 都没有才删，并明确警告 */
```

---

## 七、怎么做到"不弄坏厂家的软件"

这一点比功能重要。因为使用者可能同时装着 CANoe、CANape、ZCANPRO，
不能因为加了个工具就让它们坏掉。

### 保险一：优先用 `App.exe.local`，一个现有文件都不改

Windows 有个机制：**如果程序目录下存在 `<程序名>.exe.local` 文件夹，
加载器会优先搜索这个文件夹。**

```
CANape 安装目录\
├── CANape.exe          ← 不动
├── vxlapi.dll          ← 不动（原厂的还在）
└── CANape.exe.local\   ← 只新增这一个文件夹
        vxlapi.dll      ← 我们的
```

**卸载 = 删掉那个文件夹。**

实测（Win11）验证有效：

```
目录：
    Test.exe
    probe.dll                （标记 APPDIR）
    Test.exe.local\
        probe.dll            （标记 LOCALDIR）

运行 LoadLibrary("probe.dll") 得到：LOCALDIR
实际加载路径：...\Test.exe.Local\probe.dll
```

**限制**：只对**按名字加载**（`LoadLibrary("vxlapi.dll")`）生效。
如果软件是按完整路径加载的，`.local` 不起作用。

### 保险二：必须改文件时，备份要可用

- 备份文件名固定后缀（如 `.canbridge.bak`），卸载工具据此还原
- **没有验证过还原路径，绝不删除厂家文件**

### 保险三：独立的"一键恢复"工具

它不依赖主程序 —— 主程序坏了也能用：

```
· 自动扫 ZCANPRO / CANape / CANoe 目录
· 找 *.zlg.bak / *.canbridge.bak
· 还原原文件，并把现场另存为 .replaced（误删也能找回）
· 按 Y 之前不动任何文件
```

---

## 八、验证结果（只写真机验证过的）

| 盒子 | 打开 | 设置波特率 | 发送 | 接收 |
| --- | --- | --- | --- | --- |
| **周立功 USBCAN-2E-U** | ✓ | ✓ `SetReference` | ✓ | ✓ **4650 帧** |
| **创芯 CANalyst-II** | ✓ | ✓ Timing 字节 | ✓ | ✓ **2500 帧** |
| **PEAK PCAN-USB** | ✓ | ✓ | ✓ | 需要第二个节点 |

**卸载之后，原厂软件立即恢复可用** —— 这是硬性底线，也是花最多时间保证的一条。

### 未验证 / 未完成的，也写清楚

- 同星、广成、Kvaser：后端代码写了，**没有硬件，未验证**
- CAN FD / LIN：数据结构支持了（64 字节载荷），**通路还没接**
- Vector 自家盒子：默认关闭（它的权限模型不同，会选错后端）

> 我最想避免的是"看起来能用，实际数据是错的"。
> 那种问题比"打不开"难查一百倍。

---

## 九、一份通用的 CAN 故障速查表

不管用不用这个工具，这张表都能省时间：

| 接收帧数 | 错误帧数 | 结论 |
| --- | --- | --- |
| 0 | **0** | 没接总线 / **CAN_H、CAN_L 接反** / 插错通道 / 句柄泄漏 |
| 0 | **上万** | 有信号，但**波特率或型号不对** |
| 持续增长 | 0 | 正常 |
| 少量 | 偶发 | 多半是**终端电阻缺失**（总线末端反射） |

### 补充几条硬件常识

- **CAN_H / CAN_L 接反**的表现是「0 帧 + 0 错误帧」
  —— 因为接收端永远看不到 dominant 位，连帧起始都识别不到。
  **这是最难看出来的一种故障。**
- **一个 CAN 设备同时只能被一个进程打开**。测试前先把
  ZCANPRO / CANoe / CANape 全部关掉，或者拔插一次盒子。
- 周立功 USBCAN-E/2E-U **内部没有 120Ω 终端电阻**，
  需要自己接在接线端子的 `R+` / `R-` 之间。

---

## 十、源码结构与下载地址

**仓库地址：https://github.com/canwright/can-bridge**

> 仓库里**不含任何厂商 DLL**（`ControlCAN.dll`、`USBCAN_E.dll`、
> `PCANBasic.dll`、`vxlapi.dll` 等各有自己的 EULA，不允许再分发）。
> 需要时请自行从厂家官网下载，`tools/GET_VENDOR_DLLS.txt` 里写了各家地址。

### 核心源码

| 文件 | 作用 |
| --- | --- |
| [src/UniVciBridge.c](https://github.com/canwright/can-bridge/blob/main/src/UniVciBridge.c) | **主程序**：VCI 门面，加载厂商库并转发每一帧（146 KB） |
| [src/CtModel.h](https://github.com/canwright/can-bridge/blob/main/src/CtModel.h) | 厂商中立模型：`CtFrame` / `CtChannelCfg` / `CtCapabilities` / `CtProfile` |
| [src/CtConvert.h](https://github.com/canwright/can-bridge/blob/main/src/CtConvert.h) | `VCI_CAN_OBJ` 与 `CtFrame` 的**唯一一对**转换函数 |
| [src/VxlApiModel.h](https://github.com/canwright/can-bridge/blob/main/src/VxlApiModel.h) | Vector XL 结构布局（实测校准 `sizeof(XLchannelConfig)=227`） |
| [src/VxlApiStub.c](https://github.com/canwright/can-bridge/blob/main/src/VxlApiStub.c) | vxlapi 空壳探针：572 个导出，只记调用不实现功能 |
| [src/ZcanShim.c](https://github.com/canwright/can-bridge/blob/main/src/ZcanShim.c) | 假 `zlgcan.dll`（61 个 `ZCAN_*` 导出） |
| [src/PcanShim.c](https://github.com/canwright/can-bridge/blob/main/src/PcanShim.c) | 假 `PCANBasic.dll`（17 个 `CAN_*` 导出） |
| [src/CanRestore.c](https://github.com/canwright/can-bridge/blob/main/src/CanRestore.c) | **一键恢复**：独立于主程序，主程序坏了也能用 |
| [src/OneClick.c](https://github.com/canwright/can-bridge/blob/main/src/OneClick.c) | 图形化一键工具（检测盒子 / 装驱动 / 装桥 / 诊断） |

### 测试（不需要硬件就能跑）

| 文件 | 作用 |
| --- | --- |
| [tests/CtConvTest.c](https://github.com/canwright/can-bridge/blob/main/tests/CtConvTest.c) | 帧转换往返 **50 项**：标准帧 / 扩展帧 / 1 字节帧 / 远程帧 / 无时间戳 / ID 截断 |
| [tests/VxlLayoutTest.c](https://github.com/canwright/can-bridge/blob/main/tests/VxlLayoutTest.c) | XL 结构布局 **8 项**：拿真 `vxlapi.dll` 的数据回来验偏移 |
| [tests/VxlProbe.c](https://github.com/canwright/can-bridge/blob/main/tests/VxlProbe.c) | 量真 `vxlapi.dll` 的通道结构步长（227 就是靠它量出来的） |
| [tests/DotLocalTest.c](https://github.com/canwright/can-bridge/blob/main/tests/DotLocalTest.c) | 验证 `App.exe.local` 重定向是否还有效 |

### 编译

需要 **32 位** MSVC —— 上位机大多是 32 位，64 位 DLL 它们加载不了。

```bat
cd src
build-unibridge.bat      :: 主程序 UniVciBridge.dll
build-canrestore.bat     :: 一键恢复
build-vxlstub.bat        :: vxlapi 探针

cd ../tests
build-ctconvtest.bat && out/CtConvTest.exe      :: 50 项，必须全过
build-vxllayout.bat  && out/VxlLayoutTest.exe   :: 8 项
```

工具链靠 `src/vcenv.bat` 用 `vswhere` 自动找，**不用手改路径**。

### 文档

| 文件 | 内容 |
| --- | --- |
| [docs/ 目录](https://github.com/canwright/can-bridge/tree/main/docs) | 全部文档（含中文文件名，点进去看） |
| [docs/01-验证记录与排查手册](https://github.com/canwright/can-bridge/tree/main/docs) | **最值钱的一份**：所有坑的完整记录 + 诊断表 |
| [docs/02-模拟画像配置](https://github.com/canwright/can-bridge/tree/main/docs) | 怎么把盒子"演成"别的型号 |
| [docs/04-VN模拟-怎么用](https://github.com/canwright/can-bridge/tree/main/docs) | Vector VN1610 / VN1630 模拟的进展 |
| [MAINTENANCE.md](https://github.com/canwright/can-bridge/blob/main/MAINTENANCE.md) | 维护手册：每次改动要检查什么 |

**其中最值钱的其实不是代码，是那份排查手册** —— 代码会过时，踩过的坑不会。

### 一点说明

这个项目不是为了"绕过什么"，而是为了**互操作性**：
让你**已经合法拥有**的硬件，在你**已经获得许可**的软件里能用。
仓库里没有任何破解、授权绕过或伪造许可的代码，也不接受这类贡献。

## 十一、最后三条经验

1. **结构体、寄存器、波特率码表，一定照官方手册抄，不要凭记忆，不要用公式反推。**
   我在这上面栽的两次，每次都值一天。

2. **打开和关闭必须严格配对：同一个型号、无条件执行、打印返回值。**
   否则会把厂家驱动卡死，而且症状是"厂家软件也不能用了" —— 用户会以为你写了个病毒。

3. **排错先要那个"能分生死"的数据。**
   比如"重装软件就好使，还是必须重启电脑" —— 这一个问题就能把范围砍掉一半。

---

*如果这篇对你有帮助，或者你也在做类似的事，欢迎在评论区交流。
文中所有结论都来自真机实测，未验证的部分我都标注了。*
