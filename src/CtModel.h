/*
 * CtModel.h —— CAN 工具链的中立模型（Chameleon Transport Model）
 *
 * ==========================================================================
 *   这一层要解决的问题
 * ==========================================================================
 *
 *   现在的桥里，内核直接是「周立功形状」：
 *     · 帧用 VCI_CAN_OBJ（24 字节，带 SendType/RemoteFlag/ExternFlag 一堆标志位）
 *     · 通道配置用 VCI_INIT_CONFIG（Timing0/Timing1 是芯片寄存器值）
 *
 *   结果：
 *     · 一次结构体布局记错（16 vs 18 字节）就要查两天
 *     · 一次波特率编码抄错（16MHz vs 36MHz）就一帧收不到，且不报错
 *     · 想加 CAN FD / LIN / Vector，就得在每个 switch 里到处打补丁
 *
 *   这个模型是厂商中立的：物理量就是物理量，能力就是能力，
 *   冒充谁由 Profile 决定，跟底层是哪家的盒子无关。
 *
 *
 * ==========================================================================
 *   三个层次
 * ==========================================================================
 *
 *   CtFrame / CtChannelCfg   —— 数据与配置（厂商无关）
 *   CtCapabilities           —— 底层【真实】有什么能力
 *   CtProfile                —— 对外【假装】是什么设备
 *
 *   桥的工作 = 把 Capabilities 翻译成 Profile 要求的样子：
 *
 *       物理设备(创芯 2ch, 无FD)  ──►  Capabilities
 *                                        │
 *                                  模拟/降级/裁剪
 *                                        │
 *       ZCANPRO 看到的「USBCAN-2E-U」◄── Profile
 *
 *
 * ==========================================================================
 *   为后期留的三件事（现在就定死接口，以后不用重写）
 * ==========================================================================
 *
 *   1) CAN FD 与 LIN
 *      CtFrame 直接支持 64 字节载荷、BRS/ESI 标志；
 *      bus_kind 区分 CAN / LIN。VN1630A 的 2 CAN + 2 LIN 能用一个数组表达。
 *
 *   2) 冒充任意设备
 *      CtProfile 描述「对外演成什么」：型号码、通道数、支持 FD 与否、
 *      序列号、硬件类型字符串。换一个 Profile 就换一个身份，
 *      底层不需要改一行。
 *
 *   3) 能力不足时的兜底
 *      CtCapabilities 里明确写「没有硬件滤波」「1ms 时间戳」这类事实，
 *      上层据此决定：软件滤波 / 时间戳插值 / 直接拒绝 FD 帧。
 *      不会出现「能打开但数据不对」这种最难查的状态。
 *
 * ==========================================================================
 *   设计约束
 * ==========================================================================
 *
 *   · 只用定长整数和指针，不用 STL/C++，要能在 __stdcall DLL 里编译
 *   · 不依赖 windows.h（时间单位用 ns，由调用方提供）
 *   · 所有字段都可零初始化 = 合法状态
 *   · 不改动任何现有行为：本文件是纯新增，尚未接进收发路径
 *
 * 版本：2026-11-05 第一版
 */
#ifndef CT_MODEL_H
#define CT_MODEL_H

#ifdef __cplusplus
extern "C" {
#endif

/* --------------------------------------------------------------------------
   基本类型
   -------------------------------------------------------------------------- */
#ifndef CT_U8_DEFINED
typedef unsigned char      CtU8;
typedef unsigned short     CtU16;
typedef unsigned int       CtU32;
typedef unsigned __int64   CtU64;
#define CT_U8_DEFINED
#endif

/* --------------------------------------------------------------------------
   1. 通道种类
   -------------------------------------------------------------------------- */
typedef enum {
    CT_BUS_CAN = 0,        /* 经典 CAN 2.0A/B */
    CT_BUS_LIN = 1         /* LIN（VN1630A 的 CH3/CH4 就是 LIN） */
} CtBusKind;

/* --------------------------------------------------------------------------
   2. 帧种类
   -------------------------------------------------------------------------- */
typedef enum {
    CT_FRAME_DATA    = 0,  /* 数据帧 */
    CT_FRAME_REMOTE  = 1,  /* 远程帧（RTR） */
    CT_FRAME_ERROR   = 2,  /* 错误帧（部分厂家能给） */
    CT_FRAME_OVERLOAD= 3   /* 过载帧 */
} CtFrameKind;

/* --------------------------------------------------------------------------
   3. 方向
   -------------------------------------------------------------------------- */
typedef enum {
    CT_DIR_RX = 0,
    CT_DIR_TX = 1,
    CT_DIR_TX_ECHO = 2     /* 自己发出去、又被硬件回读回来的帧
                              （PCAN 的 echo 帧、Vector 的自收）
                              单独标记，避免上层把它当成对端报文 */
} CtFrameDirection;

/* --------------------------------------------------------------------------
   4. CtFrame —— 一帧数据（厂商中立的最终形态）

   ★ 为什么 payload 直接给 64：
     经典 CAN 最多 8 字节，但 CAN FD 到 64。VN1630A/FD 盒子都会用到。
     固定 64 字节（+ 头部）约 96 字节，按 1000 fps 也就 96 KB/s，
     完全可以直接在环形缓冲里拷贝，不需要动态分配。

   ★ dlc 和 len 分开：
     经典 CAN：dlc = len，0..8
     CAN FD  ：dlc 是 0..15 的编码（0..8,12,16,20,24,32,48,64），
               len 是实际字节数。两者不能混，混了 FD 数据必错。
   -------------------------------------------------------------------------- */
typedef struct {
    CtU32   id;            /* 帧 ID，最多 29 位；不含任何标志位（标准/扩展由 ext 决定） */
    CtU8    kind;          /* CtFrameKind */
    CtU8    fd;            /* 0 = 经典 CAN，1 = CAN FD */
    CtU8    brs;           /* FD: 1 = 数据段用高速率 */
    CtU8    esi;           /* FD: 1 = 发送方处于错误被动 */
    CtU8    dlc;           /* 0..15（经典 0..8） */
    CtU8    len;           /* 实际数据字节数：经典 0..8，FD 0..64 */
    CtU8    ext;           /* 1 = 29 位扩展帧，0 = 11 位标准帧 */
    CtU8    dir;           /* CtFrameDirection */
    CtU8    bus_kind;      /* CtBusKind */
    CtU8    bus;           /* 该 bus_kind 下的通道号，从 0 开始 */
    CtU8    reserved[3];   /* 对齐用，保持全零 */
    CtU64   ts_ns;         /* 时间戳，纳秒，单调递增；0 = 未知 */
    CtU8    data[64];      /* 载荷；len 之后的字节保持全零 */
} CtFrame;

/* --------------------------------------------------------------------------
   5. CtChannelCfg —— 通道配置（只用物理量）

   ★ 关键：这里【没有】Timing0/Timing1，也没有 0x060007。
     那些是厂商编码，由各自的适配层从 bitrate 算出来。
     这样"36MHz 控制器要用另一套时序"这种事，
     只会影响那一个厂商适配层，不会污染内核。
   -------------------------------------------------------------------------- */
typedef struct {
    CtU32   bitrate;        /* 仲裁段速率，bps，如 500000 */
    CtU32   data_bitrate;   /* FD 数据段速率，bps；0 = 不用 FD */
    CtU16   sample_point;   /* 采样点，千分比：875 = 87.5%；0 = 用默认 */
    CtU8    fd;             /* 1 = 启用 CAN FD */
    CtU8    listen_only;    /* 1 = 只听（不发 ACK）；模拟"监听不干扰"时用 */
    CtU8    loopback;       /* 1 = 自测模式（自己发自己收） */
    CtU8    filter_enable;  /* 1 = 启用下面的 ID 滤波 */
    CtU32   filter_id;      /* 验收码 */
    CtU32   filter_mask;    /* 屏蔽码：1 = 该位无关（与周立功语义一致） */

    /* LIN 专用（bus_kind == CT_BUS_LIN 时有效） */
    CtU32   lin_baud;       /* LIN 速率，通常 19200 / 9600 */
    CtU8    lin_master;     /* 1 = 主节点 */
} CtChannelCfg;

/* --------------------------------------------------------------------------
   6. CtCapabilities —— 底层【真实】能力

   探测阶段要如实填写。上层据此决定降级策略，而不是"先打开再说"。
   今天那些"能打开但收不到 / 能打开但崩"的坑，根子就是没有这一层。
   -------------------------------------------------------------------------- */
typedef struct {
    CtU32   can_channels;   /* 真实 CAN 通道数 */
    CtU32   lin_channels;   /* 真实 LIN 通道数 */

    CtU8    can_fd;         /* 支持 CAN FD */
    CtU8    fd_no_brs;      /* FD 但不支持速率切换（只能同速） */
    CtU8    hw_filter;      /* 有硬件 ID 滤波 */
    CtU8    hw_timestamp;   /* 有硬件时间戳 */

    CtU8    ts_res_ns;      /* 时间戳分辨率（纳秒）：1000 = 1us，1000000 = 1ms */
    CtU8    ts_wraps;       /* 计时器会回绕（PCAN 的 32 位 ms 就会） */

    CtU8    supports_listen_only;
    CtU8    supports_loopback;
    CtU8    supports_echo;  /* 能回读自己发出的帧 */
    CtU8    supports_error_frames;

    CtU32   max_bitrate;    /* 支持的仲裁段最高速率 */
    CtU32   max_data_bitrate;

    /* 通道独占：PCAN/Vector 是独占的，创芯/周立功可多进程？
       填 1 表示"同一时刻只能一个进程用"，用于决定退出时是否必须显式关闭 */
    CtU8    exclusive;
    CtU8    reserved[3];
} CtCapabilities;

/* --------------------------------------------------------------------------
   7. CtProfile —— 对外【假装】成什么设备

   ★ 这就是"把任何设备模拟成想要的设备"的开关。

     例：把一个创芯 2 通道盒子，演成周立功 USBCAN-2E-U：
         profile.type_code    = 21
         profile.name         = L"USBCAN-2E-U"
         profile.can_channels = 2
         profile.fd           = 0

     底层 CtCapabilities 说 has_fd=0，而 Profile 要求 fd=1 时，
     上层就必须明确拒绝（或只在 FD 帧全是 8 字节以内时降级），
     不能默默返回成功 —— 那会变成"能打开但数据是错的"。

     ★ 序列号/硬件类型字符串也要能演，因为有些上位机会按字符串判断型号。
   -------------------------------------------------------------------------- */
typedef struct {
    CtU32   type_code;          /* 厂商侧的型号编码，如周立功 3/4/20/21 */
    const wchar_t *name;        /* 显示名，如 L"USBCAN-II" */
    CtU32   can_channels;       /* 对外声称的 CAN 通道数 */
    CtU32   lin_channels;       /* 对外声称的 LIN 通道数 */
    CtU8    fd;                 /* 对外声称支持 CAN FD */
    CtU8    hw_filter;          /* 对外声称有硬件滤波（没有就用软件筛） */
    CtU8    echo;               /* 对外声称能回读自发帧 */
    CtU8    reserved;
    const char *serial;         /* 对外声称的序列号（NULL = 自动生成） */
    const char *hw_type;        /* 对外声称的硬件类型串（NULL = 用 name） */
} CtProfile;


/* --------------------------------------------------------------------------
   7b. 预置画像：把 1~2 口的盒子演成 Vector 设备

   ★ 用途：以后做「假 vxlapi.dll」时，用这些画像决定对外报成什么硬件。

     真实 vxlapi.dll 靠 xlGetDriverConfig 返回的通道表让软件枚举硬件，
     所以"演成 VN1630A"= 在那张表里造出：
         Channel 1 (CAN, 名字 VN1630A Channel 1)
         Channel 2 (CAN)
         Channel 3 (LIN)
         Channel 4 (LIN)

   ★ 注意：这些只是【描述数据】。现在桥里没有任何代码用它，
     加进来是为了让"模拟成任意设备"这件事有统一的表达，
     而不是等做 vxlapi 门面时再临时拍一套结构。
   -------------------------------------------------------------------------- */

/* VN1610：2 路 CAN，无 LIN，无 FD（老款）；VN1610 的 FD 版本另有型号 */
static const CtProfile kCtProfileVN1610 = {
    /* type_code    */ 0,               /* Vector 侧没有周立功那种型号编码 */
    /* name         */ L"VN1610",
    /* can_channels */ 2,
    /* lin_channels */ 0,
    /* fd           */ 0,
    /* hw_filter    */ 0,
    /* echo         */ 0,
    0,
    /* serial       */ "VN1610-BRIDGE",
    /* hw_type      */ "Vector VN1610"
};

/* VN1630A：2 路 CAN + 2 路 LIN（真机 CH1/3、CH2/4 共用 DB9） */
static const CtProfile kCtProfileVN1630A = {
    0, L"VN1630A",
    2, 2,
    0,     /* 经典 VN1630A 不带 CAN FD */
    0, 0, 0,
    "VN1630A-BRIDGE",
    "Vector VN1630A"
};

/* VN1640A：4 路 CAN（有些场景要 4 路） */
static const CtProfile kCtProfileVN1640A = {
    0, L"VN1640A",
    4, 0,
    0, 0, 0, 0,
    "VN1640A-BRIDGE",
    "Vector VN1640A"
};

/* 周立功侧常用的几个（已有 ini 配置，这里给出结构体版本，
   以后 UI 直接引用，不用再手写字符串） */
static const CtProfile kCtProfileUsbcanI = {
    3, L"USBCAN-I", 1, 0, 0, 1, 0, 0, "USBCAN-I-BRIDGE", "USBCAN-I"
};
static const CtProfile kCtProfileUsbcanII = {
    4, L"USBCAN-II", 2, 0, 0, 1, 0, 0, "USBCAN-II-BRIDGE", "USBCAN-II"
};
static const CtProfile kCtProfileUsbcan2EU = {
    21, L"USBCAN-2E-U", 2, 0, 0, 1, 0, 0, "USBCAN-2E-U-BRIDGE", "USBCAN-2E-U"
};

/* --------------------------------------------------------------------------
   8. 统一状态码

   各家 DLL 的返回值五花八门（1/0、0/负数、PCAN_ERROR_*）。
   适配层统一翻成这个，上层判断才不会写错。
   -------------------------------------------------------------------------- */
typedef enum {
    CT_OK             = 0,
    CT_ERR_NOT_FOUND  = 1,   /* 找不到设备 */
    CT_ERR_BUSY       = 2,   /* 被别的进程占用 */
    CT_ERR_PERMISSION = 3,   /* 权限不足（Vector 的 perm=0 就是这个） */
    CT_ERR_PARAM      = 4,   /* 参数不合法 */
    CT_ERR_UNSUPPORT  = 5,   /* 硬件不支持该功能 */
    CT_ERR_NOT_OPEN   = 6,   /* 没打开就调用 */
    CT_ERR_TX_FULL    = 7,   /* 发送缓冲满 */
    CT_ERR_BUS_ERROR  = 8,   /* 总线错误 / 没 ACK */
    CT_ERR_TIMEOUT    = 9,
    CT_ERR_INTERNAL   = 10
} CtStatus;

/* --------------------------------------------------------------------------
   9. 后端接口（每个厂商适配层实现这一组）

   ★ 注意这里没有 Timing、没有 SetReference、没有 Baudrate 常量。
     全部从 CtChannelCfg.bitrate 出发，厂商的编码差异全部烂在
     open/init 里面，不影响任何上层逻辑。
   -------------------------------------------------------------------------- */
typedef struct CtBackend CtBackend;
struct CtBackend {
    const char *name;                  /* "pcan" / "vci" / "vector" ... */

    CtStatus (*probe)  (const wchar_t *host_dir, CtCapabilities *cap);
    CtStatus (*open)   (const wchar_t *host_dir, CtU32 device_index);
    CtStatus (*close)  (void);
    CtStatus (*init)   (CtU32 bus_kind, CtU32 channel, const CtChannelCfg *cfg);
    CtStatus (*start)  (CtU32 bus_kind, CtU32 channel);
    CtStatus (*stop)   (CtU32 bus_kind, CtU32 channel);
    CtStatus (*send)   (CtU32 bus_kind, CtU32 channel, const CtFrame *f);
    CtU32    (*recv)   (CtU32 bus_kind, CtU32 channel, CtFrame *out, CtU32 max, CtU32 wait_ms);
    CtU32    (*pending)(CtU32 bus_kind, CtU32 channel);
};

/* --------------------------------------------------------------------------
   10. 便捷判断（内联，避免到处写错位运算）
   -------------------------------------------------------------------------- */
#define CT_ID_IS_EXT(id)        (((id) & 0x80000000u) != 0)
#define CT_ID_STD_MASK(id)      ((id) & 0x000007FFu)
#define CT_ID_EXT_MASK(id)      ((id) & 0x1FFFFFFFu)

/* 经典 CAN 的 dlc 就是长度 */
#define CT_DLC_TO_LEN(dlc)      ((dlc) > 8 ? 8 : (dlc))
#define CT_LEN_TO_DLC(len)      ((len) > 8 ? 8 : (len))

/* CAN FD：DLC 编码 → 实际字节数 */
static CtU8 CtFdDlcToLen(CtU8 dlc)
{
    static const CtU8 t[16] = { 0,1,2,3,4,5,6,7,8,12,16,20,24,32,48,64 };
    return t[dlc & 0x0Fu];
}

/* CAN FD：实际字节数 → DLC 编码（向上取整到最近的合法档） */
static CtU8 CtFdLenToDlc(CtU8 len)
{
    if (len <= 8)  return len;
    if (len <= 12) return 9;
    if (len <= 16) return 10;
    if (len <= 20) return 11;
    if (len <= 24) return 12;
    if (len <= 32) return 13;
    if (len <= 48) return 14;
    return 15;
}

/* --------------------------------------------------------------------------
   11. 能力是否够演这个 Profile

   ★ 这是"模拟成任意设备"的核心检查。
     返回 CT_OK 才能真的对外声称；否则上层要么降级、要么明确拒绝。
     绝不允许"先答应下来，实际做不到" —— 那是最难查的一类问题。
   -------------------------------------------------------------------------- */
static CtStatus CtCanEmulate(const CtCapabilities *cap, const CtProfile *pf)
{
    if (!cap || !pf) return CT_ERR_PARAM;

    /* 通道数：可以做减法（多的不暴露），不能做加法 */
    if (pf->can_channels > cap->can_channels) return CT_ERR_UNSUPPORT;
    if (pf->lin_channels > cap->lin_channels) return CT_ERR_UNSUPPORT;

    /* FD：底层没有就不可能演出来 */
    if (pf->fd && !cap->can_fd) return CT_ERR_UNSUPPORT;

    /* 硬件滤波可以没有 —— 上层用软件筛兜底 */
    /* echo 可以没有 —— 上层把自发的帧记一份即可 */

    return CT_OK;
}

#ifdef __cplusplus
}
#endif

#endif /* CT_MODEL_H */
