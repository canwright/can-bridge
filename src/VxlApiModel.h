/*
 * VxlApiModel.h —— Vector XL API 的结构布局（实测校准版）
 *
 * ==========================================================================
 *   这份文件里的每个数字都是【量出来的】，不是猜的
 * ==========================================================================
 *
 *   Vector 手册给了字段顺序，但没给 XL_MAX_LENGTH 的值 ——
 *   而它决定两个 char 数组的长度，进而决定整个 channel[] 数组的步长。
 *   猜错 1 个字节，64 个通道全部错位，门面永远对不上。
 *
 *   所以写了 native\VxlProbe.c 直接问真 vxlapi.dll：
 *   把缓冲区填 0xCC，调 xlGetDriverConfig，然后找通道名字符串的位置。
 *
 *   本机实测（vxlapi 20.30.14，2 个 Virtual Channel）：
 *
 *       channel[0].name          @ 元素内 +0     "Virtual Channel 1"
 *       channel[0].transceiverName @ 元素内 +155 "Virtual CAN"
 *       channel[1].name          @ 元素内 +227
 *       → sizeof(XLchannelConfig) = 227
 *
 *   逐字段核对（都自洽）：
 *       +32   hwType              = 1
 *       +35   transceiverType     = 0x0016
 *       +42   channelMask         = 1              （8 字节，pack(1)）
 *       +50   channelCapabilities = 0xA0000007
 *       +54   channelBusCapabilities = 0x00010001
 *       +58   isOnBus             = 0
 *       +63   busParams.busType   = 1   (XL_BUS_TYPE_CAN)
 *       +67   busParams.can.bitRate = 0x0007A120 = 500000
 *
 *   由此推出：XL_MAX_LENGTH = 31、XLbusParams = 36、XLchannelConfig = 227、
 *             布局是 pack(1)。
 *
 * ==========================================================================
 *   用途
 * ==========================================================================
 *   做「把周立功/PCAN 盒子模拟成 VN1610/VN1630」的假 vxlapi.dll 时，
 *   必须按这份布局填 XLdriverConfig —— 软件（CANape 等）会照这个布局读。
 */

#ifndef VXL_API_MODEL_H
#define VXL_API_MODEL_H

#pragma pack(push, 1)

typedef unsigned int       XLstatus;
typedef unsigned int       XLaccess;
typedef unsigned __int64   XLuint64;
typedef unsigned int       XLportHandle;

#define XL_MAX_LENGTH            31      /* 实测：name[] 占 32 字节 */
#define XL_CONFIG_MAX_CHANNELS   64      /* 手册 page 60 */
#define MAX_MSG_LEN              8       /* 经典 CAN 载荷 */

/* 总线类型（手册里的枚举值；1 = CAN 已被实测确认） */
#define XL_BUS_TYPE_NONE         0x00000000
#define XL_BUS_TYPE_CAN          0x00000001
#define XL_BUS_TYPE_LIN          0x00000002
#define XL_BUS_TYPE_FLEXRAY      0x00000004
#define XL_BUS_TYPE_MOST          0x00000008
#define XL_BUS_TYPE_DAIO         0x00000010

/* 通道能力 / 总线兼容性（CHANNEL_BUS_CAPABILITIES） */
#define XL_BUS_COMPATIBLE_CAN    0x00000001
#define XL_BUS_ACTIVE_CAP_CAN    0x00010000
#define XL_BUS_COMPATIBLE_LIN    0x00000002
#define XL_BUS_ACTIVE_CAP_LIN    0x00020000

/* 通道标志位 */
#define XL_CHANNEL_FLAG_TIME_SYNC_RUNNING   0x0001
#define XL_CHANNEL_FLAG_NO_HWSYNC_SUPPORT   0x0002
#define XL_CHANNEL_FLAG_CANFD_ISO_SUPPORT   0x0010


/* -------------------------------------------------------------------------
   错误码（源：XL Driver Library Manual v25.20 第 504~509 页的错误码表）
   ★ 这些是手册里逐条列出的数值，不是猜的。
   ------------------------------------------------------------------------- */
#define XL_SUCCESS                          0
#define XL_PENDING                          1
#define XL_ERR_QUEUE_IS_EMPTY               10
#define XL_ERR_QUEUE_IS_FULL                11
#define XL_ERR_TX_NOT_POSSIBLE              12
#define XL_ERR_NO_LICENSE                   14
#define XL_ERR_WRONG_PARAMETER              101
#define XL_ERR_TWICE_REGISTER               110
#define XL_ERR_INVALID_CHAN_INDEX           111
#define XL_ERR_INVALID_ACCESS               112
#define XL_ERR_PORT_IS_OFFLINE              113
#define XL_ERR_CHAN_IS_ONLINE               116
#define XL_ERR_NOT_IMPLEMENTED              117
#define XL_ERR_INVALID_PORT                 118
#define XL_ERR_HW_NOT_READY                 120
#define XL_ERR_CMD_TIMEOUT                  121
#define XL_ERR_CMD_HANDLING                 122
#define XL_ERR_HW_NOT_PRESENT               129
#define XL_ERR_NOTIFY_ALREADY_ACTIVE        131
#define XL_ERR_INVALID_TAG                  132
#define XL_ERR_INVALID_RESERVED_FLD         133
#define XL_ERR_INVALID_SIZE                 134
#define XL_ERR_INSUFFICIENT_BUFFER          135
#define XL_ERR_ERROR_CRC                    136
#define XL_ERR_BAD_EXE_FORMAT               137
#define XL_ERR_NO_SYSTEM_RESOURCES          138
#define XL_ERR_NOT_FOUND                    139
#define XL_ERR_REQ_NOT_ACCEP                141
#define XL_ERR_INVALID_LEVEL                142
#define XL_ERR_NO_DATA_DETECTED             143
#define XL_ERR_INTERNAL_ERROR               144
#define XL_ERR_UNEXP_NET_ERROR              145
#define XL_ERR_INVALID_USER_BUFFER          146
#define XL_ERR_INVALID_PORT_ACCESS_TYPE     147
#define XL_ERR_NO_RESOURCES                 152
#define XL_ERR_WRONG_CHIP_TYPE              153
#define XL_ERR_WRONG_COMMAND                154
#define XL_ERR_INVALID_HANDLE               155
#define XL_ERR_RESERVED_NOT_ZERO            157
#define XL_ERR_INIT_ACCESS_MISSING          158
#define XL_ERR_WRONG_VERSION                160
#define XL_ERR_CANNOT_OPEN_DRIVER           201
#define XL_ERR_WRONG_BUS_TYPE               202
#define XL_ERR_DLL_NOT_FOUND                203
#define XL_ERR_INVALID_CHANNEL_MASK         204
#define XL_ERR_NOT_SUPPORTED                205
#define XL_ERROR                            255

/* CAN FD 专用（第 507 页） */
#define XL_ERR_INVALID_DLC                  0x0201
#define XL_ERR_INVALID_CANID                0x0202
#define XL_ERR_INVALID_FDFLAG_MODE20        0x0203
#define XL_ERR_EDL_RTR                      0x0204
#define XL_ERR_EDL_NOT_SET                  0x0205
#define XL_ERR_UNKNOWN_FLAG                 0x0206


/* CAN 帧 id 里的扩展帧标志
   ★ 手册第 112 页原话：「If the MSB of the id is set, it is an extended
     identifier (see XL_CAN_EXT_MSG_ID)」—— 最高位为 1 即扩展帧，够确定了。 */
#define XL_CAN_EXT_MSG_ID        0x80000000u
#define XL_CAN_RTR_MSG_ID        0x40000000u

/* CAN 帧 flags 字段 */
#define XL_CAN_MSG_FLAG_ERROR_FRAME   0x01
#define XL_CAN_MSG_FLAG_REMOTE_FRAME  0x10

/* XLbusParams 里的联合体固定 32 字节 —— 实测：整个结构 36 字节 */
#define XL_BUS_PARAMS_UNION_SIZE 32

/* -------------------------------------------------------------------------
   XLbusParams（手册 3.4.3，page 63）
   ------------------------------------------------------------------------- */
typedef struct {
    unsigned int busType;
    union {
        struct {
            unsigned int  bitRate;
            unsigned char sjw;
            unsigned char tseg1;
            unsigned char tseg2;
            unsigned char sam;          /* 1 或 3 */
            unsigned char outputMode;
            unsigned char reserved[7];
            unsigned char canOpMode;
        } can;                          /* 17 字节 */

        struct {
            unsigned int  arbitrationBitRate;
            unsigned char sjwAbr;
            unsigned char tseg1Abr;
            unsigned char tseg2Abr;
            unsigned char samAbr;
            unsigned char outputMode;
            unsigned char sjwDbr;
            unsigned char tseg1Dbr;
            unsigned char tseg2Dbr;
            unsigned int  dataBitRate;
            unsigned char canOpMode;
        } canFD;                        /* 17 字节 */

        struct {
            unsigned char macAddr[6];
            unsigned char connector;
            unsigned char phy;
            unsigned char link;
            unsigned char speed;
            unsigned char clockMode;
            unsigned char bypass;
        } ethernet;                     /* 12 字节 */

        unsigned char raw[XL_BUS_PARAMS_UNION_SIZE];
    } u;
} XlBusParams;

/* -------------------------------------------------------------------------
   XLchannelConfig（手册 3.4.2，page 60）
   ------------------------------------------------------------------------- */
typedef struct {
    char           name[XL_MAX_LENGTH + 1];          /* 0   */
    unsigned char  hwType;                           /* 32  */
    unsigned char  hwIndex;                          /* 33  */
    unsigned char  hwChannel;                        /* 34  */
    unsigned short transceiverType;                  /* 35  */
    unsigned int   transceiverState;                 /* 37  */
    unsigned char  channelIndex;                     /* 41  */
    XLuint64       channelMask;                      /* 42  */
    unsigned int   channelCapabilities;              /* 50  */
    unsigned int   channelBusCapabilities;           /* 54  */
    unsigned char  isOnBus;                          /* 58  */
    unsigned int   connectedBusType;                 /* 59  */
    XlBusParams    busParams;                        /* 63  (36 字节) → 99 */
    unsigned int   driverVersion;                    /* 99  */
    unsigned int   interfaceVersion;                 /* 103 */
    unsigned int   raw_data[10];                     /* 107 → 147 */
    unsigned int   serialNumber;                     /* 147 */
    unsigned int   articleNumber;                    /* 151 */
    char           transceiverName[XL_MAX_LENGTH + 1]; /* 155 */
    unsigned int   specialCabFlags;                  /* 187 */
    unsigned int   dominantTimeout;                  /* 191 */
    unsigned int   reserved[8];                      /* 195 → 227 */
} XlChannelConfig;

/* -------------------------------------------------------------------------
   XLdriverConfig（手册 3.4.1，page 60）
   ------------------------------------------------------------------------- */
typedef struct {
    unsigned int    dllVersion;                      /* 0  */
    unsigned int    channelCount;                    /* 4  */
    unsigned int    reserved[10];                    /* 8  → 48 */
    XlChannelConfig channel[XL_CONFIG_MAX_CHANNELS]; /* 48 */
} XlDriverConfig;

/* -------------------------------------------------------------------------
   CAN 事件结构（手册 5.5.1 XLCANMessage，page 111）
   ------------------------------------------------------------------------- */
typedef struct {
    unsigned long  id;
    unsigned short flags;
    unsigned short dlc;
    XLuint64       res1;
    unsigned char  data[MAX_MSG_LEN];
    XLuint64       res2;
} XlCanMessage;

#pragma pack(pop)

/* =========================================================================
   ★ 编译期断言：布局一旦错了，编译直接失败
   ========================================================================= */
typedef char VXL_ASSERT_MAX_LENGTH[(XL_MAX_LENGTH == 31) ? 1 : -1];
typedef char VXL_ASSERT_BUS_PARAMS[(sizeof(XlBusParams) == 36) ? 1 : -1];
typedef char VXL_ASSERT_CHANNEL   [(sizeof(XlChannelConfig) == 227) ? 1 : -1];
typedef char VXL_ASSERT_CANMSG    [(sizeof(XlCanMessage) == 32) ? 1 : -1];

/* 关键字段偏移也必须对（实测值）*/
typedef char VXL_ASSERT_OFF_HWTYPE  [(offsetof(XlChannelConfig, hwType) == 32) ? 1 : -1];
typedef char VXL_ASSERT_OFF_MASK    [(offsetof(XlChannelConfig, channelMask) == 42) ? 1 : -1];
typedef char VXL_ASSERT_OFF_BUSPAR  [(offsetof(XlChannelConfig, busParams) == 63) ? 1 : -1];
typedef char VXL_ASSERT_OFF_TXNAME  [(offsetof(XlChannelConfig, transceiverName) == 155) ? 1 : -1];

#endif /* VXL_API_MODEL_H */
