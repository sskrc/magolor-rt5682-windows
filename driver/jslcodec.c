// SPDX-License-Identifier: GPL-2.0-only
/*
 * jslcodec: Windows KMDF driver for the Realtek RT5682 headset codec (ACPI\10EC5682) on the
 * Jasper Lake Chromebook "Magolor" (Acer R753T).
 *
 * The register sequences (probe setup, calibration, register patch, jack/headset detection,
 * clocking and the DAPM power sequence of the headphone playback path) are ported from the
 * Linux kernel driver sound/soc/codecs/rt5682.c / rt5682-i2c.c and the Intel machine driver
 * sound/soc/intel/boards/sof_rt5682.c:
 *   Copyright 2018 Realtek Semiconductor Corp. (Author: Bard Liao <bardliao@realtek.com>)
 * which are licensed GPL-2.0-only; this file is therefore GPL-2.0-only as well.
 *
 * The codec is an I2S consumer on the DSP's SSP0 (driven by the separate jslaudio driver):
 *   MCLK 24 MHz -> codec PLL1 -> sysclk ~24.576 MHz, DAC/ADC filters clocked through the I2S1 ASRC,
 *   I2S, 2 slots, 24-bit data.
 * The jack is polled every 500 ms (JD1); the headphone path is powered while a jack is inserted.
 */
#include <ntddk.h>
#include <wdf.h>
#include <ntstrsafe.h>
#include <reshub.h>
#include <spb.h>

/* ------------------------------------------------------------------ registers (rt5682.h) */
#define R_RESET            0x0000
#define R_HP_CTRL_1        0x0002
#define R_HP_CTRL_2        0x0003
#define R_I2C_CTRL         0x0008
#define R_CBJ_CTRL_1       0x0010
#define R_CBJ_CTRL_2       0x0011
#define R_CBJ_CTRL_3       0x0012
#define R_CBJ_CTRL_5       0x0014
#define R_CBJ_CTRL_7       0x0016
#define R_CBJ_BST_CTRL     0x000b
#define R_DAC1_DIG_VOL     0x0019
#define R_STO1_ADC_DIG_VOL 0x001c
#define R_STO1_ADC_MIXER   0x0026
#define R_STO1_DAC_MIXER   0x002a
#define R_A_DAC1_MUX       0x002b
#define R_REC_MIXER        0x003c
#define R_CAL_REC          0x0044
#define R_PWR_DIG_1        0x0061
#define R_PWR_DIG_2        0x0062
#define R_PWR_ANLG_1       0x0063
#define R_PWR_ANLG_2       0x0064
#define R_PWR_ANLG_3       0x0065
#define R_CLK_DET          0x006b
#define R_DMIC_CTRL_1      0x006e
#define R_I2S1_SDP         0x0070
#define R_ADDA_CLK_1       0x0073
#define R_TDM_CTRL         0x0079
#define R_TDM_ADDA_CTRL_2  0x007b
#define R_TDM_TCON_CTRL    0x007e
#define R_GLB_CLK          0x0080
#define R_PLL_CTRL_1       0x0081
#define R_PLL_CTRL_2       0x0082
#define R_PLL_TRACK_1      0x0083
#define R_PLL_TRACK_2      0x0084
#define R_PLL_TRACK_3      0x0085
#define R_DEPOP_1          0x008e
#define R_HP_CHARGE_PUMP_1 0x0091
#define R_MICBIAS_2        0x0094
#define R_RC_CLK_CTRL      0x009f
#define R_IRQ_CTRL_2       0x00b7
#define R_IRQ_CTRL_3       0x00b8
#define R_GPIO_CTRL_1      0x00c0
#define R_IL_CMD_1         0x00db
#define R_4BTN_IL_CMD_2    0x00e3
#define R_4BTN_IL_CMD_4    0x00e5
#define R_4BTN_IL_CMD_5    0x00e6
#define R_4BTN_IL_CMD_6    0x00e7
#define R_4BTN_IL_CMD_7    0x00e8
#define R_AJD1_CTRL        0x00f0
#define R_INT_DEVICE_ID    0x00f9
#define R_DEVICE_ID        0x00ff
#define R_DAC_ADC_DIG_VOL1 0x0100
#define R_BIAS_CUR_CTRL_8  0x0111
#define R_CHARGE_PUMP_1    0x0125
#define R_CHOP_DAC         0x013a
#define R_CHOP_ADC         0x013b
#define R_CALIB_ADC_CTRL   0x013c
#define R_TEST_MODE_CTRL_1 0x0145
#define R_PLL2_INTERNAL    0x0156
#define R_HP_IMP_SENS_CTRL_19 0x01c1
#define R_HP_LOGIC_CTRL_2  0x01db
#define R_HP_CALIB_CTRL_1  0x01de
#define R_HP_CALIB_CTRL_2  0x01df
#define R_HP_CALIB_CTRL_3  0x01e0
#define R_HP_CALIB_STA_1   0x01ea
#define R_SAR_IL_CMD_1     0x0210
#define R_SAR_IL_CMD_2     0x0211
#define R_SAR_IL_CMD_3     0x0212
#define R_SAR_IL_CMD_6     0x0215
#define R_SAR_IL_CMD_13    0x021c
#define R_I2C_MODE         0xffff

#define DEVICE_ID_RT5682   0x6530

/* PWR_ANLG_1 */
#define PWR_VREF1 (1u << 15)
#define PWR_FV1   (1u << 14)
#define PWR_VREF2 (1u << 13)
#define PWR_FV2   (1u << 12)
#define PWR_MB    (1u << 9)
#define PWR_BG    (1u << 7)
#define PWR_HA_L  (1u << 1)
#define PWR_HA_R  (1u << 0)
#define LDO1_DVO_MASK (3u << 4)
#define LDO1_DVO_12   (2u << 4)
#define HP_DRIVER_MASK (3u << 2)
#define HP_DRIVER_5X   (3u << 2)
/* PWR_ANLG_2 / _3 */
#define PWR_JDH   (1u << 3)
#define PWR_CBJ   (1u << 9)
#define PWR_PLL   (1u << 6)
#define PWR_LDO2  (1u << 2)
/* PWR_DIG_1 / _2 */
#define PWR_I2S1      (1u << 15)
#define PWR_DAC_L1    (1u << 11)
#define PWR_DAC_R1    (1u << 10)
#define PWR_LDO       (1u << 8)
#define DIG_GATE_CTRL (1u << 0)
#define PWR_DAC_S1F   (1u << 10)
#define PWR_ADC_S1F   (1u << 15)
#define PWR_ADC_L1    (1u << 4)
#define PWR_RM1_L     (1u << 1)
#define SEL_ADCDAT    (1u << 15)
/* HP */
#define HP_C2_DAC_AMP_MUTE (1u << 15)
#define HP_C2_DAC_L_EN     (1u << 14)
#define HP_C2_DAC_R_EN     (1u << 13)
#define HP_LC2_SIG_SOUR2_MASK (1u << 4)
#define CP_SW_SIZE_MASK (7u << 8)
#define CP_SW_SIZE_L    (4u << 8)
#define CP_SW_SIZE_M    (2u << 8)
#define L_MUTE (1u << 15)
#define R_MUTE (1u << 7)
/* clocks */
#define SCLK_SRC_MASK  (7u << 13)
#define SCLK_SRC_PLL1  (1u << 13)
#define PLL1_SRC_MASK  (3u << 8)
#define PLL1_SRC_MCLK  (0u << 8)
#define FILTER_CLK_SEL_MASK (7u << 12)
#define FILTER_CLK_SEL_I2S1_ASRC (1u << 12)
#define FILTER_CLK_DIV_MASK (0xfu << 8)
#define ADC_OSR_MASK (0xfu << 12)
#define DAC_OSR_MASK (0xfu << 0)
#define DAC_STO1_ASRC (1u << 12)
#define DA_ASRC       (1u << 13)
#define AD_ASRC       (1u << 8)
#define ADC_STO1_ASRC (1u << 2)
#define SYS_CLK_DET   (1u << 15)
#define POW_CLK_DET   (1u << 0)
#define PLL_N_SFT 7
#define PLL_M_SFT 12
#define PLL_M_BP  (1u << 11)
#define PLL_RST   (1u << 1)
/* I2S / TDM */
#define I2S1_DL_MASK (7u << 4)
#define I2S1_DL_24   (2u << 4)
#define I2S1_MONO_MASK (1u << 7)
#define I2S_DF_MASK  (7u)
#define I2S1_TX_CHL_MASK (7u << 12)
#define I2S1_RX_CHL_MASK (7u << 8)
#define I2S1_CHL_24  ((2u << 12) | (2u << 8))
#define TDM_EN       (1u << 7)
#define TDM_CH_MASK  ((3u << 12) | (3u << 8))
#define TDM_CL_MASK  (3u << 4)
#define TDM_CL_24    (2u << 4)
#define TDM_FMT_MASK ((1u << 15) | (1u << 14) | (7u << 11) | (1u << 2) | (1u << 1) | (1u << 0))
/* jack / CBJ */
#define JDH_RS_MASK  (1u << 4)
#define OSW_LR       ((1u << 11) | (1u << 10))
#define TRIG_JD_HIGH (1u << 3)
#define JACK_TYPE_MASK 3u
#define FAST_OFF_EN  (1u << 7)
#define PWR_CLK25M_1M ((1u << 9) | (1u << 8))
#define EXT_JD_SRC_MASK   (7u << 4)
#define EXT_JD_SRC_MANUAL (4u << 4)
#define CBJ_IN_BUF_EN (1u << 7)
#define SAR_POW_EN    (1u << 12)
#define SAR_BUTT_DET_EN (1u << 15)
#define SAR_SOUR_MASK 0x3fu
#define POW_IRQ (1u << 15)
#define POW_JDH (1u << 14)
#define POW_ANA (1u << 12)
#define JD1_EN  (1u << 15)
#define JD1_POL_MASK (1u << 13)
#define GP1_PIN_MASK (3u << 14)
#define GP1_PIN_IRQ  (1u << 14)
#define GP45_PIN_MASK ((3u << 8) | (3u << 6))
#define GP4_ADCDAT1_GP5_DACDAT1 ((1u << 8) | (1u << 6))
#define HPA_CP_BIAS_MASK (3u << 2)
#define HPA_CP_BIAS_3UA  (1u << 2)
#define CP_CLK_HP_MASK   (3u << 4)
#define CP_CLK_HP_300KHZ (2u << 4)
#define PM_HP_MASK (3u << 8)
#define PM_HP_HV   (2u << 8)
#define FIFO_CLK_DIV_MASK (7u << 12)
#define FIFO_CLK_DIV_2    (1u << 12)
#define IL_IRQ_EN   (1u << 7)
#define BTN_IL_EN   (1u << 15)
#define BTN_IL_NOR  (1u << 14)

#define MCLK_HZ    24000000u          /* SSP0 MCLK per sof-jsl-rt5682-rt1015.tplg */
#define SYSCLK_HZ  (48000u * 512u)    /* sof_rt5682: pll_out = rate * 512 */
#define BTNDET_DELAY 16

/* ------------------------------------------------------------------ logging */
static HANDLE g_log;
static WCHAR g_regPath[300];

static BOOLEAN RegReadString(PCWSTR value, PWCHAR out, ULONG cbOut)
{
    OBJECT_ATTRIBUTES oa; HANDLE k; UNICODE_STRING kn, vn; ULONG len = 0; BOOLEAN ok = FALSE;
    UCHAR buf[sizeof(KEY_VALUE_PARTIAL_INFORMATION) + 700];
    PKEY_VALUE_PARTIAL_INFORMATION kv = (PKEY_VALUE_PARTIAL_INFORMATION)buf;
    if (!g_regPath[0]) return FALSE;
    RtlInitUnicodeString(&kn, g_regPath);
    InitializeObjectAttributes(&oa, &kn, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwOpenKey(&k, KEY_READ, &oa))) return FALSE;
    RtlInitUnicodeString(&vn, value);
    RtlZeroMemory(buf, sizeof(buf));
    if (NT_SUCCESS(ZwQueryValueKey(k, &vn, KeyValuePartialInformation, kv, sizeof(buf) - 2, &len)) &&
        (kv->Type == REG_SZ || kv->Type == REG_EXPAND_SZ) && kv->DataLength >= 4)
        ok = NT_SUCCESS(RtlStringCbCopyNW(out, cbOut, (PCWSTR)kv->Data, kv->DataLength));
    ZwClose(k);
    return ok;
}

static VOID LogOpen(VOID)
{
    UNICODE_STRING n; OBJECT_ATTRIBUTES oa; IO_STATUS_BLOCK iosb; WCHAR path[300];
    if (g_log) return;
    if (!RegReadString(L"LogPath", path, sizeof(path)))
        RtlStringCbCopyW(path, sizeof(path), L"\\SystemRoot\\Temp\\jslcodec.log");
    RtlInitUnicodeString(&n, path);
    InitializeObjectAttributes(&oa, &n, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    if (!NT_SUCCESS(ZwCreateFile(&g_log, FILE_APPEND_DATA | SYNCHRONIZE, &oa, &iosb, NULL, FILE_ATTRIBUTE_NORMAL,
        FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN_IF,
        FILE_SYNCHRONOUS_IO_NONALERT | FILE_WRITE_THROUGH | FILE_NON_DIRECTORY_FILE, NULL, 0)))
        g_log = NULL;
}

static VOID Log(const char *fmt, ...)
{
    char buf[256]; va_list ap; IO_STATUS_BLOCK iosb; size_t len;
    if (KeGetCurrentIrql() != PASSIVE_LEVEL) return;
    if (!g_log) LogOpen();
    if (!g_log) return;
    va_start(ap, fmt); RtlStringCbVPrintfA(buf, sizeof(buf) - 2, fmt, ap); va_end(ap);
    RtlStringCbLengthA(buf, sizeof(buf) - 2, &len); buf[len++] = '\r'; buf[len++] = '\n';
    ZwWriteFile(g_log, NULL, NULL, NULL, &iosb, buf, (ULONG)len, NULL, NULL);
}

static VOID SleepMs(ULONG ms)
{
    LARGE_INTEGER d; d.QuadPart = -10000LL * ms;
    KeDelayExecutionThread(KernelMode, FALSE, &d);
}

/* ------------------------------------------------------------------ device context */
typedef struct {
    WDFIOTARGET i2c;
    LARGE_INTEGER i2cId;
    BOOLEAN haveI2c;
    BOOLEAN ready;          /* codec initialised in this D0 */
    BOOLEAN veIc;
    ULONG   jackType;       /* 0 = none, 1 = headphone, 2 = headset */
    BOOLEAN hpOn, micOn, commonOn;
    ULONG   i2cErrors;
    WDFTIMER timer;
    WDFWAITLOCK lock;
} CODEC;
WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(CODEC, Ctx)

/* ------------------------------------------------------------------ I2C (SPB): 16-bit register, 16-bit value, big endian */
static NTSTATUS RegWrite(CODEC *c, USHORT reg, USHORT val)
{
    UCHAR b[4] = { (UCHAR)(reg >> 8), (UCHAR)reg, (UCHAR)(val >> 8), (UCHAR)val };
    WDF_MEMORY_DESCRIPTOR md; ULONG_PTR n = 0; NTSTATUS st;
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&md, b, sizeof(b));
    st = WdfIoTargetSendWriteSynchronously(c->i2c, NULL, &md, NULL, NULL, &n);
    if (!NT_SUCCESS(st) && c->i2cErrors++ < 20) Log("i2c write %04X=%04X failed %08X", reg, val, st);
    return st;
}

static NTSTATUS RegRead(CODEC *c, USHORT reg, USHORT *val)
{
    UCHAR a[2] = { (UCHAR)(reg >> 8), (UCHAR)reg }, d[2] = { 0, 0 };
    SPB_TRANSFER_LIST_AND_ENTRIES(2) seq;
    WDF_MEMORY_DESCRIPTOR md; ULONG_PTR n = 0; NTSTATUS st;
    SPB_TRANSFER_LIST_INIT(&seq.List, 2);
    seq.List.Transfers[0] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionToDevice, 0, a, sizeof(a));
    seq.List.Transfers[1] = SPB_TRANSFER_LIST_ENTRY_INIT_SIMPLE(SpbTransferDirectionFromDevice, 0, d, sizeof(d));
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&md, &seq, sizeof(seq));
    st = WdfIoTargetSendIoctlSynchronously(c->i2c, NULL, IOCTL_SPB_EXECUTE_SEQUENCE, &md, NULL, NULL, &n);
    *val = (USHORT)((d[0] << 8) | d[1]);
    if (!NT_SUCCESS(st) && c->i2cErrors++ < 20) Log("i2c read %04X failed %08X", reg, st);
    return st;
}

static USHORT Rd(CODEC *c, USHORT reg) { USHORT v = 0; RegRead(c, reg, &v); return v; }
static VOID Wr(CODEC *c, USHORT reg, USHORT val) { RegWrite(c, reg, val); }
static VOID Upd(CODEC *c, USHORT reg, USHORT mask, USHORT val)
{
    USHORT o = Rd(c, reg), n = (USHORT)((o & ~mask) | (val & mask));
    if (n != o) Wr(c, reg, n);
}

/* ------------------------------------------------------------------ codec sequences (ported from rt5682.c) */
static VOID Reset(CODEC *c)
{
    Wr(c, R_RESET, 0);
    Wr(c, R_I2C_MODE, 1);
}

static VOID Calibrate(CODEC *c)
{
    ULONG count; USHORT v = 0;
    Reset(c);
    Wr(c, R_I2C_CTRL, 0x000f);
    Wr(c, R_PWR_ANLG_1, 0xa2af);
    SleepMs(20);
    Wr(c, R_PWR_ANLG_1, 0xf2af);
    Wr(c, R_MICBIAS_2, 0x0300);
    Wr(c, R_GLB_CLK, 0x8000);
    Wr(c, R_PWR_DIG_1, 0x0100);
    Wr(c, R_HP_IMP_SENS_CTRL_19, 0x3800);
    Wr(c, R_CHOP_DAC, 0x3000);
    Wr(c, c->veIc ? R_CHOP_ADC : R_CALIB_ADC_CTRL, 0x7005);
    Wr(c, R_STO1_ADC_MIXER, 0x686c);
    Wr(c, R_CAL_REC, 0x0d0d);
    Wr(c, R_HP_CALIB_CTRL_2, 0x0321);
    Wr(c, R_HP_LOGIC_CTRL_2, 0x0004);
    Wr(c, R_HP_CALIB_CTRL_1, 0x7c00);
    Wr(c, R_HP_CALIB_CTRL_3, 0x06a1);
    Wr(c, R_A_DAC1_MUX, 0x0311);
    Wr(c, R_HP_CALIB_CTRL_1, 0x7c00);
    Wr(c, R_HP_CALIB_CTRL_1, 0xfc00);
    for (count = 0; count < 60; count++) {
        v = Rd(c, R_HP_CALIB_STA_1);
        if (!(v & 0x8000)) break;
        SleepMs(10);
    }
    Log("HP calibration %s (sta=%04X, %u polls)", count >= 60 ? "FAILED" : "done", v, count);
    Wr(c, R_PWR_ANLG_1, 0x002f);
    Wr(c, R_MICBIAS_2, 0x0080);
    Wr(c, R_GLB_CLK, 0x0000);
    Wr(c, R_PWR_DIG_1, 0x0000);
    Wr(c, R_CHOP_DAC, 0x2000);
    Wr(c, c->veIc ? R_CHOP_ADC : R_CALIB_ADC_CTRL, 0x2005);
    Wr(c, R_STO1_ADC_MIXER, 0xc0c4);
    Wr(c, R_CAL_REC, 0x0c0c);
}

static const USHORT g_patch[][2] = {
    { R_HP_IMP_SENS_CTRL_19, 0x1000 }, { R_DAC_ADC_DIG_VOL1, 0xa020 }, { R_I2C_CTRL, 0x000f },
    { R_PLL2_INTERNAL, 0x8266 },       { R_SAR_IL_CMD_1, 0x22b7 },     { R_SAR_IL_CMD_3, 0x0365 },
    { R_SAR_IL_CMD_6, 0x0110 },        { R_CHARGE_PUMP_1, 0x0210 },    { R_HP_LOGIC_CTRL_2, 0x0007 },
    { R_SAR_IL_CMD_2, 0xac00 },        { 0x0016 /* CBJ_CTRL_7 */, 0x0104 },
};

/* PLL1: Fout = Fin * (N + 2) / ((M + 2) * (K + 2)), M optionally bypassed (RL6231 PLL) */
static VOID PllCode(ULONG fin, ULONG fout, ULONG *n, ULONG *m, ULONG *k, BOOLEAN *mbp)
{
    LONG kk = (LONG)(100000000u / fout) - 2;
    ULONG64 vco, best = (ULONG64)-1;
    ULONG mi, ni;
    if (kk < 0) kk = 0;
    if (kk > 31) kk = 31;
    vco = (ULONG64)fout * (ULONG)(kk + 2);
    *k = (ULONG)kk; *n = 0; *m = 0; *mbp = TRUE;
    for (mi = 0; mi <= 16; mi++) {                 /* mi == 16: M bypass */
        ULONG64 div = (mi == 16) ? 1 : (mi + 2);
        for (ni = 0; ni <= 511; ni++) {
            ULONG64 f = (ULONG64)fin * (ni + 2) / div;
            ULONG64 e = f > vco ? f - vco : vco - f;
            if (e < best) { best = e; *n = ni; *m = (mi == 16) ? 0 : mi; *mbp = (mi == 16); }
        }
    }
}

/* AIF1 = I2S consumer, 2 slots x 24 bit; sysclk = PLL1(MCLK); DAC/ADC filters through the I2S1 ASRC */
static VOID ConfigureClocksAndFormat(CODEC *c)
{
    ULONG n, m, k; BOOLEAN mbp;
    /* rt5682_sel_asrc_clk_src(DA_STEREO1 | AD_STEREO1, I2S1_ASRC) */
    Upd(c, R_PLL_TRACK_2, FILTER_CLK_SEL_MASK, FILTER_CLK_SEL_I2S1_ASRC);
    Upd(c, R_PLL_TRACK_3, FILTER_CLK_SEL_MASK, FILTER_CLK_SEL_I2S1_ASRC);
    /* set_dai_fmt: I2S, NB_NF, codec consumer */
    Upd(c, R_I2S1_SDP, I2S_DF_MASK, 0);
    Upd(c, R_TDM_TCON_CTRL, TDM_FMT_MASK, 0);
    /* set_tdm_slot(0, 0, 2, 24) */
    Upd(c, R_TDM_ADDA_CTRL_2, TDM_EN, 0);
    Upd(c, R_TDM_CTRL, TDM_CH_MASK, 0);
    Upd(c, R_TDM_TCON_CTRL, TDM_CL_MASK, TDM_CL_24);
    Upd(c, R_I2S1_SDP, I2S1_TX_CHL_MASK | I2S1_RX_CHL_MASK, I2S1_CHL_24);
    /* set_pll(PLL1, MCLK, 24 MHz, 24.576 MHz) + set_sysclk(PLL1) */
    PllCode(MCLK_HZ, SYSCLK_HZ, &n, &m, &k, &mbp);
    Upd(c, R_GLB_CLK, PLL1_SRC_MASK, PLL1_SRC_MCLK);
    Wr(c, R_PLL_CTRL_1, (USHORT)((n << PLL_N_SFT) | k));
    Wr(c, R_PLL_CTRL_2, (USHORT)((m << PLL_M_SFT) | (mbp ? PLL_M_BP : 0) | PLL_RST));
    Upd(c, R_GLB_CLK, SCLK_SRC_MASK, SCLK_SRC_PLL1);
    Log("PLL1: N=%u M=%u%s K=%u", n, m, mbp ? "(bypass)" : "", k);
    /* hw_params: 24-bit, stereo */
    Upd(c, R_I2S1_SDP, I2S1_DL_MASK | I2S1_MONO_MASK, I2S1_DL_24);
    /* set_filter_clk for the DAC (ref = 256 * 48 kHz, sysclk 24.576 MHz -> div 2, OSR index 1) */
    Upd(c, R_PLL_TRACK_2, FILTER_CLK_DIV_MASK, 1u << 8);
    Upd(c, R_ADDA_CLK_1, ADC_OSR_MASK | DAC_OSR_MASK, (1u << 12) | (1u << 0));
    /* mixer controls (UCM-style defaults): Stereo1 DAC MIX L <- DAC L1, R <- DAC R1; HPOL/HPOR switches on */
    Upd(c, R_STO1_DAC_MIXER, (1u << 15) | (1u << 5), 0);
    Upd(c, R_HP_CTRL_1, L_MUTE | R_MUTE, 0);
    /* headset mic: RECMIX1L CBJ switch on, CBJ boost 30 dB, Stereo1 ADC L1/R1 source = ADC, L/R = ADC1 L,
       ADC1 switches on, ADC2 (DMIC) switches off  ->  STO1_ADC_MIXER = 0x6060 */
    Upd(c, R_REC_MIXER, 1u << 7, 0);
    Upd(c, R_CBJ_BST_CTRL, 0xfu << 8, 3u << 8);
    Wr(c, R_STO1_ADC_MIXER, 0x6060);
}

static VOID PushButtonIrq(CODEC *c, BOOLEAN enable)
{
    if (enable) {
        Upd(c, R_SAR_IL_CMD_1, SAR_BUTT_DET_EN, SAR_BUTT_DET_EN);
        Upd(c, R_SAR_IL_CMD_13, SAR_SOUR_MASK, 0x3f);
        Wr(c, R_IL_CMD_1, 0x0040);
        Upd(c, R_4BTN_IL_CMD_2, BTN_IL_EN | BTN_IL_NOR, BTN_IL_EN | BTN_IL_NOR);
        Upd(c, R_IRQ_CTRL_3, IL_IRQ_EN, IL_IRQ_EN);
    } else {
        Upd(c, R_IRQ_CTRL_3, IL_IRQ_EN, 0);
        Upd(c, R_SAR_IL_CMD_1, SAR_BUTT_DET_EN, 0);
        Upd(c, R_4BTN_IL_CMD_2, BTN_IL_EN, 0);
        Upd(c, R_4BTN_IL_CMD_2, BTN_IL_NOR, 0);
        Upd(c, R_SAR_IL_CMD_13, SAR_SOUR_MASK, 0);
    }
}

/* rt5682_set_jack_detect, JD1 */
static VOID EnableJackDetect(CODEC *c)
{
    Upd(c, R_CBJ_CTRL_5, 0x0700, 0x0600);
    Upd(c, R_CBJ_CTRL_2, EXT_JD_SRC_MASK, EXT_JD_SRC_MANUAL);
    Wr(c, R_CBJ_CTRL_1, 0xd142);
    Upd(c, R_CBJ_CTRL_3, CBJ_IN_BUF_EN, CBJ_IN_BUF_EN);
    Upd(c, R_SAR_IL_CMD_1, SAR_POW_EN, SAR_POW_EN);
    Upd(c, R_GPIO_CTRL_1, GP1_PIN_MASK, GP1_PIN_IRQ);
    Upd(c, R_RC_CLK_CTRL, POW_IRQ | POW_JDH | POW_ANA, POW_IRQ | POW_JDH | POW_ANA);
    Upd(c, R_PWR_ANLG_2, PWR_JDH, PWR_JDH);
    Upd(c, R_IRQ_CTRL_2, JD1_EN | JD1_POL_MASK, JD1_EN);
    Upd(c, R_4BTN_IL_CMD_4, 0x7f7f, (BTNDET_DELAY << 8) | BTNDET_DELAY);
    Upd(c, R_4BTN_IL_CMD_5, 0x7f7f, (BTNDET_DELAY << 8) | BTNDET_DELAY);
    Upd(c, R_4BTN_IL_CMD_6, 0x7f7f, (BTNDET_DELAY << 8) | BTNDET_DELAY);
    Upd(c, R_4BTN_IL_CMD_7, 0x7f7f, (BTNDET_DELAY << 8) | BTNDET_DELAY);
}

/* rt5682_headset_detect */
static ULONG HeadsetDetect(CODEC *c, BOOLEAN insert)
{
    if (insert) {
        USHORT v; ULONG count = 0, type;
        Upd(c, R_PWR_ANLG_1, PWR_VREF2 | PWR_MB, PWR_VREF2 | PWR_MB);
        Upd(c, R_PWR_ANLG_1, PWR_FV2, 0);
        SleepMs(20);
        Upd(c, R_PWR_ANLG_1, PWR_FV2, PWR_FV2);
        Upd(c, R_PWR_ANLG_3, PWR_CBJ, PWR_CBJ);
        Upd(c, R_HP_CHARGE_PUMP_1, OSW_LR, 0);
        PushButtonIrq(c, FALSE);
        Upd(c, R_CBJ_CTRL_1, TRIG_JD_HIGH, 0);
        SleepMs(60);
        Upd(c, R_CBJ_CTRL_1, TRIG_JD_HIGH, TRIG_JD_HIGH);
        v = Rd(c, R_CBJ_CTRL_2) & JACK_TYPE_MASK;
        while (v == 0 && count < 50) { SleepMs(15); v = Rd(c, R_CBJ_CTRL_2) & JACK_TYPE_MASK; count++; }
        if (v == 1 || v == 2) {
            type = 2;
            Upd(c, R_CBJ_CTRL_1, FAST_OFF_EN, FAST_OFF_EN);
            PushButtonIrq(c, TRUE);
        } else type = 1;
        Upd(c, R_HP_CHARGE_PUMP_1, OSW_LR, OSW_LR);
        Upd(c, R_MICBIAS_2, PWR_CLK25M_1M, PWR_CLK25M_1M);
        Log("jack inserted: CBJ type %u -> %s", v, type == 2 ? "headset" : "headphone");
        return type;
    }
    PushButtonIrq(c, FALSE);
    Upd(c, R_CBJ_CTRL_1, TRIG_JD_HIGH, 0);
    if (!c->commonOn) Upd(c, R_PWR_ANLG_1, PWR_MB | PWR_VREF2, 0);   /* PLL1 / mic paths are off */
    Upd(c, R_PWR_ANLG_3, PWR_CBJ, 0);
    Upd(c, R_MICBIAS_2, PWR_CLK25M_1M, 0);
    Upd(c, R_CBJ_CTRL_1, FAST_OFF_EN, 0);
    Log("jack removed");
    return 0;
}

/* supplies shared by playback and capture: bias, LDO2, PLL1, Vref1, I2S1, CLKDET, ASRC */
static VOID CommonPower(CODEC *c, BOOLEAN on)
{
    if (on == c->commonOn) return;
    if (on) {
        /* bias PREPARE */
        Upd(c, R_PWR_ANLG_1, PWR_BG, PWR_BG);
        Upd(c, R_PWR_DIG_1, DIG_GATE_CTRL | PWR_LDO, DIG_GATE_CTRL | PWR_LDO);
        Upd(c, R_PWR_ANLG_3, PWR_LDO2 | PWR_PLL, PWR_LDO2 | PWR_PLL);
        Upd(c, R_PWR_ANLG_1, PWR_FV1, 0);              /* Vref1 (rt5682_set_verf) */
        Upd(c, R_PWR_ANLG_1, PWR_VREF1, PWR_VREF1);
        SleepMs(20);
        Upd(c, R_PWR_ANLG_1, PWR_FV1, PWR_FV1);
        Upd(c, R_PWR_DIG_1, PWR_I2S1, PWR_I2S1);
        Upd(c, R_CLK_DET, POW_CLK_DET | SYS_CLK_DET, POW_CLK_DET | SYS_CLK_DET);
        Upd(c, R_PLL_TRACK_1, DAC_STO1_ASRC | ADC_STO1_ASRC | DA_ASRC | AD_ASRC,
                              DAC_STO1_ASRC | ADC_STO1_ASRC | DA_ASRC | AD_ASRC);
    } else {
        Upd(c, R_PLL_TRACK_1, DAC_STO1_ASRC | ADC_STO1_ASRC | DA_ASRC | AD_ASRC, 0);
        Upd(c, R_CLK_DET, POW_CLK_DET | SYS_CLK_DET, 0);
        Upd(c, R_PWR_DIG_1, PWR_I2S1, 0);
        Upd(c, R_PWR_ANLG_1, PWR_VREF1, 0);
        Upd(c, R_PWR_ANLG_3, PWR_LDO2 | PWR_PLL, 0);
        Upd(c, R_PWR_DIG_1, PWR_LDO, 0);               /* bias STANDBY */
    }
    c->commonOn = on;
}

/* headset mic: IN1P(CBJ) -> BST1 -> RECMIX1L -> ADC1 L -> Stereo1 ADC MIX (L and R from ADC1 L) -> AIF1TX */
static VOID MicPower(CODEC *c, BOOLEAN on)
{
    if (on == c->micOn) return;
    if (on) {
        Upd(c, R_PLL_TRACK_3, FILTER_CLK_DIV_MASK, 1u << 8);    /* set_filter_clk (ADC) */
        Upd(c, R_PWR_DIG_2, PWR_ADC_S1F, PWR_ADC_S1F);
        Upd(c, R_PWR_ANLG_2, PWR_RM1_L, PWR_RM1_L);
        Upd(c, R_PWR_DIG_1, PWR_ADC_L1, PWR_ADC_L1);
        Upd(c, R_CHOP_ADC, 1u << 12, 1u << 12);                 /* ADC1 clock */
        Upd(c, R_STO1_ADC_DIG_VOL, L_MUTE | R_MUTE, 0);          /* Stereo1 ADC MIXL/R */
        Upd(c, R_I2S1_SDP, SEL_ADCDAT, 0);                       /* AIF1TX */
    } else {
        Upd(c, R_I2S1_SDP, SEL_ADCDAT, SEL_ADCDAT);
        Upd(c, R_STO1_ADC_DIG_VOL, L_MUTE | R_MUTE, L_MUTE | R_MUTE);
        Upd(c, R_CHOP_ADC, 1u << 12, 0);
        Upd(c, R_PWR_DIG_1, PWR_ADC_L1, 0);
        Upd(c, R_PWR_ANLG_2, PWR_RM1_L, 0);
        Upd(c, R_PWR_DIG_2, PWR_ADC_S1F, 0);
    }
    c->micOn = on;
    Log("headset mic path %s", on ? "ON" : "off");
}

/* DAPM power-up of AIF1RX -> DAC L1/R1 -> Stereo1 DAC mixer -> HP Amp -> HPO (common supplies first) */
static VOID HeadphonePower(CODEC *c, BOOLEAN on)
{
    if (on == c->hpOn) return;
    if (on) {
        /* DAC Stereo1 Filter (set_filter_clk done at configure time), HP Amp L/R */
        Upd(c, R_PWR_DIG_2, PWR_DAC_S1F, PWR_DAC_S1F);
        Upd(c, R_PWR_ANLG_1, PWR_HA_L | PWR_HA_R, PWR_HA_L | PWR_HA_R);
        /* sequenced supplies: charge pump (1), capless (2), DAC1 clock (3) */
        Upd(c, R_DEPOP_1, 1u << 3, 1u << 3);
        Upd(c, R_DEPOP_1, 1u << 0, 1u << 0);
        Upd(c, R_CHOP_DAC, 1u << 12, 1u << 12);
        /* DACs */
        Upd(c, R_PWR_DIG_1, PWR_DAC_L1 | PWR_DAC_R1, PWR_DAC_L1 | PWR_DAC_R1);
        /* HP Amp PRE_PMU (rt5682_hp_event) */
        Upd(c, R_HP_CTRL_2, HP_C2_DAC_AMP_MUTE, 0);
        Upd(c, R_HP_LOGIC_CTRL_2, HP_LC2_SIG_SOUR2_MASK, HP_LC2_SIG_SOUR2_MASK);
        Upd(c, R_DEPOP_1, 0x60, 0x60);
        Upd(c, R_DAC_ADC_DIG_VOL1, 0x00c0, 0x0080);
        Upd(c, R_HP_CTRL_2, HP_C2_DAC_L_EN | HP_C2_DAC_R_EN, HP_C2_DAC_L_EN | HP_C2_DAC_R_EN);
        SleepMs(10);
        Upd(c, R_CHARGE_PUMP_1, CP_SW_SIZE_MASK, CP_SW_SIZE_L);
    } else {
        /* HP Amp POST_PMD */
        Upd(c, R_HP_CTRL_2, HP_C2_DAC_L_EN | HP_C2_DAC_R_EN, 0);
        Upd(c, R_CHARGE_PUMP_1, CP_SW_SIZE_MASK, CP_SW_SIZE_M);
        Upd(c, R_DEPOP_1, 0x60, 0);
        Upd(c, R_DAC_ADC_DIG_VOL1, 0x00c0, 0);
        Upd(c, R_PWR_DIG_1, PWR_DAC_L1 | PWR_DAC_R1, 0);
        Upd(c, R_CHOP_DAC, 1u << 12, 0);
        Upd(c, R_DEPOP_1, (1u << 3) | (1u << 0), 0);
        Upd(c, R_PWR_ANLG_1, PWR_HA_L | PWR_HA_R, 0);
        Upd(c, R_PWR_DIG_2, PWR_DAC_S1F, 0);
    }
    c->hpOn = on;
    Log("headphone path %s", on ? "ON" : "off");
}

/* rt5682_i2c_probe (minus regulators / DMIC pins / IRQ) */
static NTSTATUS CodecInit(CODEC *c)
{
    USHORT id = 0, iid = 0; NTSTATUS st; ULONG i;
    c->i2cErrors = 0;
    st = RegWrite(c, R_I2C_MODE, 1);
    SleepMs(15);
    if (NT_SUCCESS(st)) st = RegRead(c, R_DEVICE_ID, &id);
    if (!NT_SUCCESS(st) || id != DEVICE_ID_RT5682) { Log("not an RT5682: st=%08X id=%04X", st, id); return STATUS_DEVICE_NOT_CONNECTED; }
    RegRead(c, R_INT_DEVICE_ID, &iid);
    c->veIc = (iid == 0x6956);
    Log("RT5682 found (internal id %04X%s)", iid, c->veIc ? ", VE" : "");

    Calibrate(c);
    for (i = 0; i < ARRAYSIZE(g_patch); i++) Wr(c, g_patch[i][0], g_patch[i][1]);
    Wr(c, 0x008e /* DEPOP_1 */, 0x0000);
    Upd(c, R_PWR_ANLG_1, LDO1_DVO_MASK | HP_DRIVER_MASK, LDO1_DVO_12 | HP_DRIVER_5X);
    Wr(c, R_MICBIAS_2, 0x0080);
    Upd(c, R_GPIO_CTRL_1, GP45_PIN_MASK, GP4_ADCDAT1_GP5_DACDAT1);
    Wr(c, R_TEST_MODE_CTRL_1, 0x0000);
    Upd(c, R_BIAS_CUR_CTRL_8, HPA_CP_BIAS_MASK, HPA_CP_BIAS_3UA);
    Upd(c, R_CHARGE_PUMP_1, CP_CLK_HP_MASK, CP_CLK_HP_300KHZ);
    Upd(c, R_HP_CHARGE_PUMP_1, PM_HP_MASK, PM_HP_HV);
    Upd(c, R_DMIC_CTRL_1, FIFO_CLK_DIV_MASK, FIFO_CLK_DIV_2);

    ConfigureClocksAndFormat(c);
    /* bias STANDBY */
    Upd(c, R_PWR_DIG_1, DIG_GATE_CTRL, DIG_GATE_CTRL);
    EnableJackDetect(c);
    c->jackType = 0; c->hpOn = c->micOn = c->commonOn = FALSE;
    Log("init done, i2c errors=%u", c->i2cErrors);
    return STATUS_SUCCESS;
}

static VOID AllPathsOff(CODEC *c)
{
    MicPower(c, FALSE);
    HeadphonePower(c, FALSE);
    CommonPower(c, FALSE);
}

/* rt5682_jack_detect_handler, polled */
static VOID JackPoll(CODEC *c)
{
    BOOLEAN in = !(Rd(c, R_AJD1_CTRL) & JDH_RS_MASK);
    if (in && c->jackType == 0) {
        c->jackType = HeadsetDetect(c, TRUE);
        CommonPower(c, TRUE);
        HeadphonePower(c, TRUE);
        if (c->jackType == 2) MicPower(c, TRUE);
    } else if (!in && c->jackType != 0) {
        AllPathsOff(c);
        c->jackType = HeadsetDetect(c, FALSE);
    }
}

/* ------------------------------------------------------------------ KMDF plumbing */
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_DEVICE_PREPARE_HARDWARE EvtPrepareHardware;
EVT_WDF_DEVICE_RELEASE_HARDWARE EvtReleaseHardware;
EVT_WDF_DEVICE_D0_ENTRY EvtD0Entry;
EVT_WDF_DEVICE_D0_EXIT EvtD0Exit;
EVT_WDF_TIMER EvtTimer;

VOID EvtTimer(WDFTIMER t)
{
    CODEC *c = Ctx(WdfTimerGetParentObject(t));
    BOOLEAN again;
    WdfWaitLockAcquire(c->lock, NULL);
    if (c->ready) JackPoll(c);
    again = c->ready;
    WdfWaitLockRelease(c->lock);
    if (again) WdfTimerStart(t, WDF_REL_TIMEOUT_IN_MS(500));
}

NTSTATUS EvtPrepareHardware(WDFDEVICE dev, WDFCMRESLIST raw, WDFCMRESLIST xlat)
{
    CODEC *c = Ctx(dev);
    ULONG n = WdfCmResourceListGetCount(xlat), i;
    UNREFERENCED_PARAMETER(raw);
    Log("=== jslcodec start, %u resources ===", n);
    for (i = 0; i < n; i++) {
        PCM_PARTIAL_RESOURCE_DESCRIPTOR d = WdfCmResourceListGetDescriptor(xlat, i);
        if (d->Type == CmResourceTypeConnection && d->u.Connection.Class == CM_RESOURCE_CONNECTION_CLASS_SERIAL &&
            d->u.Connection.Type == CM_RESOURCE_CONNECTION_TYPE_SERIAL_I2C && !c->haveI2c) {
            c->i2cId.LowPart = d->u.Connection.IdLowPart;
            c->i2cId.HighPart = d->u.Connection.IdHighPart;
            c->haveI2c = TRUE;
        }
    }
    if (!c->haveI2c) { Log("no I2C connection resource"); return STATUS_NOT_FOUND; }
    {
        WCHAR pathBuf[64]; UNICODE_STRING path;
        WDF_IO_TARGET_OPEN_PARAMS op; NTSTATUS st;
        st = RtlStringCchPrintfW(pathBuf, ARRAYSIZE(pathBuf), L"\\??\\RESOURCE_HUB\\%016I64x", (ULONG64)c->i2cId.QuadPart);
        RtlInitUnicodeString(&path, pathBuf);
        if (NT_SUCCESS(st)) st = WdfIoTargetCreate(dev, WDF_NO_OBJECT_ATTRIBUTES, &c->i2c);
        if (NT_SUCCESS(st)) {
            WDF_IO_TARGET_OPEN_PARAMS_INIT_OPEN_BY_NAME(&op, &path, GENERIC_READ | GENERIC_WRITE);
            st = WdfIoTargetOpen(c->i2c, &op);
        }
        Log("open I2C target %08X:%08X -> %08X", c->i2cId.HighPart, c->i2cId.LowPart, st);
        return st;
    }
}

NTSTATUS EvtReleaseHardware(WDFDEVICE dev, WDFCMRESLIST xlat)
{
    CODEC *c = Ctx(dev);
    UNREFERENCED_PARAMETER(xlat);
    if (c->i2c) { WdfIoTargetClose(c->i2c); WdfObjectDelete(c->i2c); c->i2c = NULL; }
    c->haveI2c = FALSE;
    Log("release hardware");
    if (g_log) { ZwClose(g_log); g_log = NULL; }
    return STATUS_SUCCESS;
}

NTSTATUS EvtD0Entry(WDFDEVICE dev, WDF_POWER_DEVICE_STATE prev)
{
    CODEC *c = Ctx(dev);
    NTSTATUS st;
    UNREFERENCED_PARAMETER(prev);
    SleepMs(300);                                  /* rt5682_i2c_probe: >= 300 ms after power */
    WdfWaitLockAcquire(c->lock, NULL);
    st = CodecInit(c);
    c->ready = NT_SUCCESS(st);
    WdfWaitLockRelease(c->lock);
    if (c->ready) WdfTimerStart(c->timer, WDF_REL_TIMEOUT_IN_MS(250));
    return STATUS_SUCCESS;                         /* never fail D0: keep the device alive for diagnosis */
}

NTSTATUS EvtD0Exit(WDFDEVICE dev, WDF_POWER_DEVICE_STATE target)
{
    CODEC *c = Ctx(dev);
    BOOLEAN wasReady;
    UNREFERENCED_PARAMETER(target);
    WdfWaitLockAcquire(c->lock, NULL);
    wasReady = c->ready;
    c->ready = FALSE;                              /* stops the callback from re-arming */
    WdfWaitLockRelease(c->lock);
    WdfTimerStop(c->timer, TRUE);
    WdfWaitLockAcquire(c->lock, NULL);
    if (wasReady) {
        AllPathsOff(c);
        Reset(c);
        c->jackType = 0;
    }
    WdfWaitLockRelease(c->lock);
    Log("D0 exit");
    return STATUS_SUCCESS;
}

NTSTATUS EvtDeviceAdd(WDFDRIVER drv, PWDFDEVICE_INIT init)
{
    WDF_PNPPOWER_EVENT_CALLBACKS cb; WDF_OBJECT_ATTRIBUTES a; WDFDEVICE dev; NTSTATUS st;
    WDF_TIMER_CONFIG tc; WDF_OBJECT_ATTRIBUTES ta;
    UNREFERENCED_PARAMETER(drv);
    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&cb);
    cb.EvtDevicePrepareHardware = EvtPrepareHardware;
    cb.EvtDeviceReleaseHardware = EvtReleaseHardware;
    cb.EvtDeviceD0Entry = EvtD0Entry;
    cb.EvtDeviceD0Exit = EvtD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(init, &cb);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&a, CODEC);
    st = WdfDeviceCreate(&init, &a, &dev);
    if (!NT_SUCCESS(st)) return st;
    st = WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &Ctx(dev)->lock);
    if (!NT_SUCCESS(st)) return st;
    /* passive-level timers cannot be periodic: one-shot, re-armed at the end of every callback */
    WDF_TIMER_CONFIG_INIT(&tc, EvtTimer);
    tc.AutomaticSerialization = FALSE;               /* serialised by c->lock instead */
    WDF_OBJECT_ATTRIBUTES_INIT(&ta);
    ta.ParentObject = dev;
    ta.ExecutionLevel = WdfExecutionLevelPassive;    /* I2C is synchronous: poll at PASSIVE_LEVEL */
    return WdfTimerCreate(&tc, &ta, &Ctx(dev)->timer);
}

DRIVER_INITIALIZE DriverEntry;
NTSTATUS DriverEntry(PDRIVER_OBJECT drv, PUNICODE_STRING reg)
{
    WDF_DRIVER_CONFIG cfg;
    RtlStringCbCopyNW(g_regPath, sizeof(g_regPath), reg->Buffer, reg->Length);
    WDF_DRIVER_CONFIG_INIT(&cfg, EvtDeviceAdd);
    return WdfDriverCreate(drv, reg, WDF_NO_OBJECT_ATTRIBUTES, &cfg, WDF_NO_HANDLE);
}
