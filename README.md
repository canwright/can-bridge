# CAN 万能桥 · Universal CAN Bridge

让**别的牌子**的 CAN 盒子，在只认某个牌子的上位机软件里直接用。

> **一句话**：把周立功 / PEAK / 创芯 的盒子，伪装成另一家的设备，
> 让 ZCANPRO、CANape 这类软件以为插的是它认识的那块卡。

---

> **English**: a DLL shim that lets a ZLG / PEAK / Chuangxin / GCAN CAN interface
> be used by host software which only accepts one specific brand
> (ZCANPRO, CANape, CANoe, vendor test tools). Open the
> [maintenance notes](MAINTENANCE.md) for the engineering constraints.
>
> **中文**：把周立功 / PEAK / 创芯 / 广成 的 CAN 盒子，转接给只认某一个牌子的
> 上位机软件使用（ZCANPRO、CANape、CANoe、厂家测试工具）。

---

## 它解决什么问题

```
现实：                       想要的：
  手里有 创芯/周立功/PCAN       同一块盒子，ZCANPRO / CANape 都能用
  想用的软件只认 周立功/Vector
  ────────────────────────►   不用为了软件再买一块硬件
```

桥做的是 **DLL 层的翻译**：

```
上位机软件（以为在跟原厂设备说话）
        │  调用 VCI_* 或 ZCAN_* 或 xlCan* 接口
        ▼
    本项目的门面 DLL
        │  统一转成中立模型 CtFrame / CtChannelCfg
        ▼
    厂商适配层（pcan / vci / gcan / kvaser / tosun / vector）
        │
        ▼
    你真正插着的那块盒子
```

---

## 已验证的能力（真机实测，2026-11）

| 盒子 | 打开 | 设波特率 | 发送 | 接收 |
| --- | --- | --- | --- | --- |
| **周立功 USBCAN-2E-U**（型号 21） | ✅ | ✅ `VCI_SetReference` | ✅ 返回 1 | ✅ **4,650 帧** |
| **创芯 CANalyst**（型号 4） | ✅ | ✅ Timing 字节 | ✅ 返回 1 | ✅ **2,500 帧** |
| **PEAK PCAN-USB** | ✅ | ✅ | ✅ | ⏳ 需第二节点 |
| **Vector VN1630A** | ✅ | ✅ | ✅ | ✅ **150 秒 2087 帧 × 双通道，0 错误** |
| 同星 / 广成 / Kvaser | — | — | — | 代码已写，**无硬件未验证** |

**卸载后原厂软件立即恢复可用** —— 这是本项目的一条硬性底线。

真周立功盒子插着时，桥会**优先把调用转给原厂 DLL**
（`kerneldlls\USBCAN.dll` 或安装时备份的 `.zlg.bak`），
行为和装桥之前完全一致 —— 见 `docs/10`。

---

## 一个盒子，装成三种样子

同一份硬件，按上位机要求的接口换门面：

| 上位机认的接口 | 门面 DLL | 状态 |
| --- | --- | --- |
| 周立功老 VCI（`VCI_*`，14 个导出） | `USBCAN.dll` | ✅ 已验证（ZCANPRO 收发） |
| 周立功新 VCI（`ZCAN_*`，61 个导出） | `zlgcan.dll` | ✅ 已验证（开设备/初始化/收发） |
| PEAK（`CAN_*`，16 个函数） | `PCANBasic.dll` | ✅ 已验证（`CAN_Write` → `CAN_Read`） |
| Vector（`xl*`，约 575 个导出） | 不需要 | ✅ 本来就是 Vector 硬件 |

**出差只带一块盒子，不管对方软件认哪家。**

| 一块盒子 + 三种门面 | 结果 |
| --- | --- |
| VN1630A + `USBCAN.dll` | ZCANPRO 正常收发 |
| VN1630A + `zlgcan.dll` | 用周立功新 API 的软件正常 |
| VN1630A + `PCANBasic.dll` | 用 PEAK API 的软件正常 |
| VN1630A 原生 | CANape / CANoe 正常 |

**两个上位机同时用一块盒子**：Vector 按通道授权，两个程序用不同
`xl_channel` 和不同 `app_name` 即可，实测两进程各 2087 帧互不干扰。
见 `docs/10`。

---

## 安全性设计（这是重点，不是附注）

这个项目会碰厂家的 DLL，所以安全机制比功能更重要。

### 1. 绝不盲删/盲改厂家文件

安装前先备份，卸载按备份还原；**没有验证过的还原路径就不删除**。

### 2. 优先用 `App.exe.local` —— 一个现有文件都不改

Windows 加载器规则：程序目录下若有 `<程序名>.exe.local` 文件夹，会**优先搜索它**。

```
CANape 安装目录\
├── CANape.exe          ← 不动
├── vxlapi.dll          ← 不动（原厂）
└── CANape.exe.local\   ← 只新增这个文件夹
        vxlapi.dll      ← 我们的
```

**卸载 = 删掉那个文件夹。** 见 `tests/DotLocalTest.c`，这是实测验证过的。

### 3. 一键恢复

`CanRestore.exe` 独立于桥本身，桥坏了也能用：

```
扫 ZCANPRO / CANape / CANoe 目录
找 *.zlg.bak / *.canbridge.bak
还原原文件，并把现场另存为 *.replaced
按 Y 之前不动任何文件
```

### 4. 句柄必须配对释放

**真机踩过的坑**：用错型号关设备 → 驱动里留着"设备被占用" →
连厂家自己的软件都打不开 → 只能重装厂家软件或重启电脑。

规则：打开/关闭用同一个型号、无条件执行、打印返回值。

---

## 快速开始

### 给 ZCANPRO 用

```
1. 点 一键工具.exe → 「① 一键检测并修复」
2. ZCANPRO 里型号选 USBCAN-I / USBCAN-II → 打开设备
```

### 给别的软件用

```
1. 点 一键工具.exe →「一键诊断」先看版本对不对
2. 需要换门面时用「选软件装桥」（★ 不要选 ZCANPRO 自己，会被拒绝）
```

详见 `docs/03-使用说明.txt`。

---

## 构建

需要 **32 位** MSVC（上位机大多是 32 位）。

```bat
cd src
build-unibridge.bat      :: UniVciBridge.dll  —— VCI 门面（核心）
build-oneclick.bat       :: 一键工具.exe
build-canrestore.bat     :: 一键恢复.exe
build-vxlstub.bat        :: VxlApiStub.dll    —— vxlapi 探针
```

**测试**（不需要硬件）：

```bat
build-ctconvtest.bat && out\CtConvTest.exe      :: 50 项帧转换往返测试
build-vxllayout.bat  && out\VxlLayoutTest.exe   :: 8 项 XL 结构布局校验
```

---

## 厂商 DLL 不在本仓库

本仓库**只含源码**。运行时需要各厂家的 DLL（`ControlCAN.dll`、`USBCAN_E.dll`、
`PCANBasic.dll`、`vxlapi.dll` 等），这些有各自的 EULA，**不允许再分发**。

请自行从厂家官网下载，或安装对应驱动后由系统提供。见 `tools/GET_VENDOR_DLLS.txt`。

---

## 文档

| 文件 | 内容 |
| --- | --- |
| `docs/01-验证记录与排查手册.txt` | **最值钱的一份**：9 个真机坑 + 用两个数字判断故障方向 |
| `docs/02-模拟画像配置.txt` | `profile=` / `channels=` 怎么把盒子演成别的型号 |
| `docs/04-VN模拟-怎么用.txt` | 演成 Vector VN1610 / VN1630 的进展与测试方法 |
| `docs/05-开发纪律.txt` | 这个项目踩出来的开发规则（结构体、句柄、验证） |
| `docs/06-Vector真机验证记录.txt` | VN1630A 打通全过程：三个卡了很久的 bug |
| `docs/07-多路并行尝试记录.txt` | 两路并行为什么失败两次、第三次怎么成的 |
| `docs/09-驱动兼容性记录.txt` | 老款 ZLG 在 Win11 起不来的真实原因与排查方法 |
| `docs/10-2026-11-05改动记录.txt` | 一天 14 个提交的完整记录（时间戳/性能/滤波/错误回传） |

**现场排查工具**：`诊断工具\自检.exe` —— 一把跑完 9 项，逐项 ✓ / ★，
不需要懂原理就能判断是桥的问题、盒子的问题、还是配置的问题。

---

## 已知限制（诚实列表）

- **只支持 CAN 2.0**。CAN FD / LIN 已有数据结构（`CtFrame` 支持 64 字节载荷），
  但**尚未接通**任何厂商的 FD 路径。这是目前最大的缺口。
- **同星 / 广成 / Kvaser 未在真机验证** —— 代码写了，没有硬件。
- **周立功老款 USBCAN-II** 在 Win11 25H2 起不来。原因查实了：
  2017 版驱动的 PE 头 `MajorSubsystemVersion` 是 1.10，而内核要求 ≥ 6.0，
  加载直接返回 `0xC000007B`。**关内存完整性没用**，重装同一版驱动也没用。
  见 `docs/09`。新款 USBCAN-E-U 的 2020 版驱动正常。
- **同一条通道跨进程共享**（两个程序抢同一路 CAN）只有一个实验性原型：
  客户端能接入、能收到广播帧，但通过共享路径发出的帧在测试接线下拿不到
  ACK，根因未定位。默认关闭，要 ini 写 `share=1` 才启用。
  **可靠的做法是按通道分离**，不是共享同一路。
- **不能改为上层请求的型号**。`VCI_BOARD_INFO.DeviceType` 必须与请求一致，
  否则 ZCANPRO 会断言崩溃（`channelNumber_count`）。

---

## 这个项目的立场

本项目为 **互操作性** 目的设计：

> 让你**已合法拥有**的硬件，在你**已获得许可**的软件里使用。

**它不提供、也不支持任何绕过软件许可或硬件绑定的功能。**
不包含破解、不校验绕过、不伪造许可。

如果你用它来规避软件授权，那不是本项目的用途，也不被支持。

---

## 许可

[Apache License 2.0](LICENSE)

---

## 贡献

欢迎补后端 —— 每个厂商就是一个自包含适配器：

```c
probe() → 填 CtCapabilities（如实上报能力）
open() / close()
init(通道配置) / start() / stop()
send(CtFrame*) / recv(CtFrame*, n, wait_ms)
```

**最有价值的贡献**：拿你真机上的盒子跑一遍，把"能不能收发"的结论提上来，
顺便补一下 `docs/01` 的诊断表。

提交前请先跑：

```bat
tests\CtConvTest.exe       :: 必须 50/50
tests\VxlLayoutTest.exe    :: 必须 8/8
```

---

## 本项目的搜索关键词

> 这一段是为了让需要它的人搜得到。如果你是在搜索框里找到这里的，说明生效了。

**中文场景**
周立功 USBCAN-2E-U 用 CANape · 创芯 CANalyst 在 ZCANPRO 里用 ·
CANape 没有 Vector 硬件 · PCAN 盒子伪装成 Vector VN1630 ·
ZCANPRO 换别的牌子盒子 · USBCAN.dll 替换 · ControlCAN.dll 转发 ·
CAN 盒 通用 驱动 · 虚拟 CAN 通道 · 多个软件同时用一块 CAN 盒 ·
CAN_H CAN_L 接反 · USBCAN-2E-U 收不到报文 · 错误帧 74458

**English**
use ZLG USBCAN from CANape · use Chuangxin CANalyst with ZCANPRO ·
CANape without Vector hardware · fake vxlapi.dll · vxlapi shim ·
VCI to PCAN bridge · CAN interface interoperability ·
DLL proxy for CAN driver · CAN bus multi-application sharing ·
CAN frame error passive · PEAK PCAN-USB alternative

**技术名词**
VCI_OpenDevice · VCI_InitCAN · VCI_SetReference · VCI_BOARD_INFO ·
VCI_INIT_CONFIG · ZCAN_OpenDevice · XLchannelConfig · xlOpenPort ·
xlGetDriverConfig · App.exe.local · DLL redirection · CAN 2.0A/B ·
J1939 · CCP · UDS
