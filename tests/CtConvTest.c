/*
 * CtConvTest.c —— 转换往返测试（不需要任何硬件）
 *
 * 目的：证明 VCI_CAN_OBJ → CtFrame → VCI_CAN_OBJ 不丢字段。
 *
 * 为什么值得单独写：
 *   这种转换"看着对、实际丢字段"的情况最难查 ——
 *   真机上表现为"收到的报文数据少一个字节""扩展帧变成标准帧"
 *   或者"时间戳全是 0"，而代码怎么看都没错。
 *
 * 用法：直接运行，全部通过返回 0。
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include "CtModel.h"

/* 周立功 24 字节帧结构（和 ControlCAN.h 一致） */
#pragma pack(push, 1)
typedef struct {
    DWORD ID;
    DWORD TimeStamp;
    BYTE  TimeFlag;
    BYTE  SendType;
    BYTE  RemoteFlag;
    BYTE  ExternFlag;
    BYTE  DataLen;
    BYTE  Data[8];
    BYTE  Reserved[3];
} VCI_CAN_OBJ;
#pragma pack(pop)

/* BYTE 在上面的结构里用到，先给个别名（windows.h 已定义 BYTE） */
#include "CtConvert.h"

static int g_pass = 0, g_fail = 0;

static void Check(int cond, const char *what)
{
    if (cond) { g_pass++; }
    else { g_fail++; printf("  [FAIL] %s\n", what); }
}

/* 往返一次：VCI -> Ct -> VCI，比较关键字段 */
static void RoundTrip(const VCI_CAN_OBJ *in, const char *tag)
{
    CtFrame f;
    VCI_CAN_OBJ out;
    char buf[160];

    CtFromVciObj(in, &f, 0);
    CtToVciObj(&f, &out);

    sprintf(buf, "%s: ID(0x%X vs 0x%X)", tag, in->ID & 0x1FFFFFFF, out.ID & 0x1FFFFFFF);
    Check((in->ID & 0x1FFFFFFF) == (out.ID & 0x1FFFFFFF), buf);

    sprintf(buf, "%s: ExternFlag(%u vs %u)", tag, in->ExternFlag, out.ExternFlag);
    Check(in->ExternFlag == out.ExternFlag, buf);

    sprintf(buf, "%s: RemoteFlag(%u vs %u)", tag, in->RemoteFlag, out.RemoteFlag);
    Check(in->RemoteFlag == out.RemoteFlag, buf);

    sprintf(buf, "%s: DataLen(%u vs %u)", tag, in->DataLen, out.DataLen);
    Check(in->DataLen == out.DataLen, buf);

    sprintf(buf, "%s: Data", tag);
    Check(memcmp(in->Data, out.Data, in->DataLen > 8 ? 8 : in->DataLen) == 0, buf);

    sprintf(buf, "%s: TimeStamp(%lu vs %lu)", tag,
            (unsigned long)in->TimeStamp, (unsigned long)out.TimeStamp);
    Check(in->TimeStamp == out.TimeStamp, buf);

    sprintf(buf, "%s: TimeFlag(%u vs %u)", tag, in->TimeFlag, out.TimeFlag);
    Check(in->TimeFlag == out.TimeFlag, buf);
}

int main(void)
{
    VCI_CAN_OBJ a;

    printf("=== CtFrame 转换往返测试 ===\n\n");

    /* 1. 标准帧，8 字节，带时间戳 */
    ZeroMemory(&a, sizeof(a));
    a.ID = 0x123; a.ExternFlag = 0; a.DataLen = 8; a.TimeFlag = 1; a.TimeStamp = 12345;
    memcpy(a.Data, "\x01\x02\x03\x04\x05\x06\x07\x08", 8);
    RoundTrip(&a, "[1] 标准帧8字节");

    /* 2. 扩展帧，8 字节 */
    ZeroMemory(&a, sizeof(a));
    a.ID = 0x18FF6003; a.ExternFlag = 1; a.DataLen = 8; a.TimeFlag = 1; a.TimeStamp = 99;
    memcpy(a.Data, "\xAA\xBB\xCC\xDD\xEE\xFF\x11\x22", 8);
    RoundTrip(&a, "[2] 扩展帧8字节");

    /* 3. 标准帧，1 字节（最容易丢数据的情况） */
    ZeroMemory(&a, sizeof(a));
    a.ID = 0x7FF; a.DataLen = 1; a.TimeFlag = 1; a.TimeStamp = 1;
    a.Data[0] = 0x5A;
    RoundTrip(&a, "[3] 标准帧1字节");

    /* 4. 远程帧，无数据 */
    ZeroMemory(&a, sizeof(a));
    a.ID = 0x456; a.RemoteFlag = 1; a.DataLen = 0; a.TimeFlag = 1; a.TimeStamp = 7;
    RoundTrip(&a, "[4] 远程帧0字节");

    /* 5. 无时间戳（TimeFlag=0） */
    ZeroMemory(&a, sizeof(a));
    a.ID = 0x100; a.DataLen = 2; a.TimeFlag = 0; a.TimeStamp = 0;
    a.Data[0] = 0x11; a.Data[1] = 0x22;
    RoundTrip(&a, "[5] 无时间戳");

    /* 6. 扩展帧 + 远程（组合） */
    ZeroMemory(&a, sizeof(a));
    a.ID = 0x1FFFFFFF; a.ExternFlag = 1; a.RemoteFlag = 1;
    a.DataLen = 0; a.TimeFlag = 1; a.TimeStamp = 777;
    RoundTrip(&a, "[6] 扩展远程帧");

    /* 7. CtFrame 的字段语义检查 */
    {
        CtFrame f;
        ZeroMemory(&a, sizeof(a));
        a.ID = 0x18FF6003; a.ExternFlag = 1; a.DataLen = 8; a.TimeFlag = 1; a.TimeStamp = 500;
        CtFromVciObj(&a, &f, 3);
        Check(f.ext == 1,                "[7] ext 标志");
        Check(f.id == 0x18FF6003,        "[7] id 保留 29 位");
        Check(f.len == 8 && f.dlc == 8,  "[7] len/dlc");
        Check(f.bus == 3,                "[7] bus 通道号");
        Check(f.bus_kind == CT_BUS_CAN,  "[7] bus_kind");
        Check(f.dir == CT_DIR_RX,        "[7] 方向默认 Rx");
        Check(f.ts_ns == 500ULL * 1000000ULL, "[7] 毫秒 → 纳秒");
    }

    /* 8. 标准帧的 ID 必须被截到 11 位（防止厂家给脏值） */
    {
        CtFrame f;
        ZeroMemory(&a, sizeof(a));
        a.ID = 0xFFFFFFFF; a.ExternFlag = 0; a.DataLen = 1;
        CtFromVciObj(&a, &f, 0);
        Check(f.id == 0x7FF, "[8] 标准帧 ID 截到 11 位");
    }

    printf("\n=== 结果：通过 %d 项，失败 %d 项 ===\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
