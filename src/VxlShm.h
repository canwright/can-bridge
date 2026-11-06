/*
 * VxlShm.h —— 64 位门面 <-> 32 位辅助进程 的共享内存协议
 *
 * 为什么要辅助进程：周立功只提供 32 位 DLL，64 位的 CANape 调不了。
 *   64 位 CANape → vxlapi64.dll（门面）→ 共享内存 → VxlHelper.exe（32位）→ 周立功
 *
 * 帧记录固定 32 字节，收发各一条环形队列，先进先出。
 */
#ifndef VXLSHM_H
#define VXLSHM_H

#define VXL_SHM_NAME   "Local\\VxlFacadeShm_v1"
#define VXL_EVT_RX     "Local\\VxlFacadeEvtRx_v1"   /* 辅助进程 → 门面：有帧了 */
#define VXL_EVT_TX     "Local\\VxlFacadeEvtTx_v1"   /* 门面 → 辅助进程：要发帧 */
#define VXL_SHM_MAGIC  0x5A4C4748L                  /* "ZLGH" */

#define VXL_RING 4096                                /* 每边 4096 帧 */

#pragma pack(push,1)
typedef struct {
    unsigned id;        /* CAN ID（含扩展帧标志位由 flags 表示） */
    unsigned timeStamp; /* 0.1ms 单位，和 VCI 一致 */
    unsigned char dlc;
    unsigned char flags;/* bit0=远程帧 bit1=扩展帧 */
    unsigned char data[8];
    unsigned char pad[14];   /* 补齐到 32 字节 */
} VxlFrame;

typedef struct {
    volatile long magic;
    volatile long ready;      /* 1 = 辅助进程已打开硬件并就绪 */
    volatile long quit;       /* 1 = 门面要求退出 */
    volatile long err;        /* 辅助进程最近的错误码 */
    volatile long rxHead, rxTail;   /* 辅助进程写入，门面读取 */
    volatile long txHead, txTail;   /* 门面写入，辅助进程读取 */
    volatile long rxOverflow;
    VxlFrame rx[VXL_RING];
    VxlFrame tx[VXL_RING];
} VxlShm;
#pragma pack(pop)

#endif
