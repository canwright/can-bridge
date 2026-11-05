/*
 * CtConvert.h —— VCI_CAN_OBJ  ⇄  CtFrame  的唯一一对转换
 *
 * 用法：调用方需要先定义好
 *          VCI_CAN_OBJ   （周立功 24 字节帧结构）
 *          CtFrame / CtU8.. （见 CtModel.h）
 * 然后再 #include 本文件。
 *
 * ★ 为什么单独抽出来
 *
 *   1) 全后端共用这一对，不许各自再写一遍。
 *      真机上因为"各家自己转"踩过的坑：24 字节结构体字段错位、
 *      RemoteFlag/ExternFlag 混用、时间戳单位不一致。
 *
 *   2) 能被单元测试单独引用 —— native\CtConvTest.c 就是用它做往返验证。
 *      转换逻辑最怕"看着对、实际丢字段"，光靠真机试是试不出来的。
 *
 * ★ 注意单位：VCI_CAN_OBJ.TimeStamp 是【毫秒】，CtFrame.ts_ns 是【纳秒】。
 *   单位不同是这类 bug 的常见来源，所以换算只在这里做一次。
 */
#ifndef CT_CONVERT_H
#define CT_CONVERT_H

/* VCI_CAN_OBJ → CtFrame
   bus: 通道号，写进 CtFrame.bus */
static void CtFromVciObj(const VCI_CAN_OBJ *o, CtFrame *f, int bus)
{
    if (!f) return;
    memset(f, 0, sizeof(*f));
    if (!o) return;

    f->ext  = o->ExternFlag ? 1 : 0;
    f->id   = o->ID & (f->ext ? 0x1FFFFFFFu : 0x000007FFu);
    f->kind = o->RemoteFlag ? CT_FRAME_REMOTE : CT_FRAME_DATA;
    f->dlc  = CT_LEN_TO_DLC(o->DataLen);
    f->len  = f->dlc;
    f->dir  = CT_DIR_RX;
    f->bus_kind = CT_BUS_CAN;
    f->bus  = (CtU8)(bus < 0 ? 0 : bus);
    f->ts_ns = o->TimeFlag ? ((CtU64)o->TimeStamp * 1000000ULL) : 0ULL;
    if (f->len) memcpy(f->data, o->Data, (size_t)f->len);
}

/* CtFrame → VCI_CAN_OBJ */
static void CtToVciObj(const CtFrame *f, VCI_CAN_OBJ *o)
{
    if (!o) return;
    memset(o, 0, sizeof(*o));
    if (!f) return;

    o->ID         = f->id & (f->ext ? 0x1FFFFFFFu : 0x000007FFu);
    o->ExternFlag = f->ext ? 1 : 0;
    o->RemoteFlag = (f->kind == CT_FRAME_REMOTE) ? 1 : 0;
    o->DataLen    = (BYTE)(f->len > 8 ? 8 : f->len);
    o->TimeStamp  = (DWORD)(f->ts_ns / 1000000ULL);
    o->TimeFlag   = f->ts_ns ? 1 : 0;
    o->SendType   = 0;
    if (o->DataLen) memcpy(o->Data, f->data, o->DataLen);
}

#endif /* CT_CONVERT_H */
