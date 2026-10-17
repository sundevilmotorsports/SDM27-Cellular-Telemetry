#include "mcp2518fd.h"
#include <SPI.h>

// ---- Registers (DS20006027) ------------------------------------------------
#define REG_CiCON      0x000
#define REG_CiNBTCFG   0x004
#define REG_CiDBTCFG   0x008
#define REG_CiTDC      0x00C
#define REG_CiTBC      0x010
#define REG_CiTSCON    0x014
#define REG_CiINT      0x01C
#define REG_CiTREC     0x034
#define REG_CiBDIAG1   0x03C
#define REG_CiFIFOCON1 0x05C  // FIFO n at 0x050 + 12n
#define REG_CiFIFOSTA1 0x060
#define REG_CiFIFOUA1  0x064
#define REG_CiFLTCON0  0x1D0
#define REG_CiFLTOBJ0  0x1F0
#define REG_CiMASK0    0x1F4
#define REG_OSC        0xE00
#define RAM_START      0x400

#define OPMODE_NORMAL_FD 0
#define OPMODE_CONFIG    4

#define INSTR_RESET 0x0
#define INSTR_READ  0x3
#define INSTR_WRITE 0x2

#define SYSCLK_HZ 40000000UL  // board's 40 MHz crystal, PLL off, SCLKDIV=0

// RX object in RAM: ID word, control word, timestamp word, then payload.
// 26 x (12 + 64) = 1976 of the 2048 bytes of message RAM.
#define RX_FIFO_DEPTH 26

static const SPISettings kSpi(8000000, MSBFIRST, SPI_MODE0);
static int s_cs = -1;
static uint32_t s_rxOverflows = 0;

static void spiHeader(uint8_t instr, uint16_t addr) {
    SPI.transfer((uint8_t)((instr << 4) | ((addr >> 8) & 0x0F)));
    SPI.transfer((uint8_t)(addr & 0xFF));
}

static void mcpReset() {
    SPI.beginTransaction(kSpi);
    digitalWrite(s_cs, LOW);
    spiHeader(INSTR_RESET, 0);
    digitalWrite(s_cs, HIGH);
    SPI.endTransaction();
}

static void mcpWrite(uint16_t addr, const uint8_t *buf, size_t n) {
    SPI.beginTransaction(kSpi);
    digitalWrite(s_cs, LOW);
    spiHeader(INSTR_WRITE, addr);
    for (size_t i = 0; i < n; i++) SPI.transfer(buf[i]);
    digitalWrite(s_cs, HIGH);
    SPI.endTransaction();
}

static void mcpRead(uint16_t addr, uint8_t *buf, size_t n) {
    SPI.beginTransaction(kSpi);
    digitalWrite(s_cs, LOW);
    spiHeader(INSTR_READ, addr);
    for (size_t i = 0; i < n; i++) buf[i] = SPI.transfer(0x00);
    digitalWrite(s_cs, HIGH);
    SPI.endTransaction();
}

static uint32_t le32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

// Registers and RAM are little-endian: the byte at addr is bits 7:0.
static void mcpWriteWord(uint16_t addr, uint32_t v) {
    uint8_t b[4] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)};
    mcpWrite(addr, b, 4);
}

static uint32_t mcpReadWord(uint16_t addr) {
    uint8_t b[4];
    mcpRead(addr, b, 4);
    return le32(b);
}

static void mcpWriteByte(uint16_t addr, uint8_t v) { mcpWrite(addr, &v, 1); }

static uint8_t mcpReadByte(uint16_t addr) {
    uint8_t v;
    mcpRead(addr, &v, 1);
    return v;
}

static uint8_t mcpOpMode() {
    return (mcpReadByte(REG_CiCON + 2) >> 5) & 0x07;  // OPMOD, CiCON bits 23:21
}

static bool mcpRequestMode(uint8_t mode) {
    uint8_t b = mcpReadByte(REG_CiCON + 3);
    mcpWriteByte(REG_CiCON + 3, (b & ~0x07) | mode);  // REQOP, CiCON bits 26:24
    for (int i = 0; i < 100; i++) {
        if (mcpOpMode() == mode) return true;
        delay(1);
    }
    return false;
}

static uint8_t dlcToLen(uint8_t dlc) {
    static const uint8_t kLen[16] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 12, 16, 20, 24, 32, 48, 64};
    return kLen[dlc & 0x0F];
}

const char *mcp2518fdBegin(const Mcp2518fdPins &pins, uint32_t nominalBitrate,
                           uint32_t dataBitrate) {
    // Bit timing with BRP=1 (one time quantum per 40 MHz clock), sample
    // points at 80% to match the ESP32 TWAI presets. Every CiNBTCFG/CiDBTCFG
    // field holds (length - 1). Nominal: TSEG1 2..256, TSEG2 1..128 TQ.
    // Data: TSEG1 1..32, TSEG2 1..16 TQ.
    if (nominalBitrate == 0 || SYSCLK_HZ % nominalBitrate || dataBitrate == 0 ||
        SYSCLK_HZ % dataBitrate) {
        return "bitrates must divide 40 MHz evenly";
    }
    const uint32_t ntq = SYSCLK_HZ / nominalBitrate;
    const uint32_t ntseg2 = ntq / 5;
    const uint32_t ntseg1 = ntq - 1 - ntseg2;
    const uint32_t dtq = SYSCLK_HZ / dataBitrate;
    const uint32_t dtseg2 = dtq / 5 ? dtq / 5 : 1;
    const uint32_t dtseg1 = dtq - 1 - dtseg2;
    if (ntseg1 < 2 || ntseg1 > 256 || ntseg2 > 128) return "nominal bitrate out of range (~125k-1M)";
    if (dtseg1 < 1 || dtseg1 > 32 || dtseg2 > 16) return "data bitrate out of range (~1M-8M)";

    s_cs = pins.cs;
    pinMode(s_cs, OUTPUT);
    digitalWrite(s_cs, HIGH);
    SPI.begin(pins.sck, pins.miso, pins.mosi, -1);

    // Leave whatever mode a previous boot left it in before resetting, so a
    // reset never lands in the middle of a frame on the bus.
    mcpRequestMode(OPMODE_CONFIG);
    mcpReset();
    delay(5);

    // SPI sanity: write two patterns into message RAM and read them back. A
    // missing/miswired SDO reads all 0x00 or 0xFF; a miswired SDI/SCK/NCS
    // never writes.
    for (uint32_t pattern : {0x5AA5C33Cu, 0xA55A3CC3u}) {
        mcpWriteWord(RAM_START, pattern);
        if (mcpReadWord(RAM_START) != pattern) {
            return "SPI readback failed -- check SCK/SDI/SDO/NCS wiring and 5V on the board's VCC";
        }
    }
    if (!(mcpReadWord(REG_OSC) & (1u << 10))) return "oscillator not ready (OSC.OSCRDY=0)";
    if (mcpOpMode() != OPMODE_CONFIG) return "not in configuration mode after reset";

    // CiCON: keep reset defaults (ISO CRC on, matching ESP32 TWAI-FD's
    // default) but drop the TEF and TXQ (STEF bit 19, TXQEN bit 20) -- this
    // node never transmits, so they would only take message RAM from RX.
    uint32_t con = mcpReadWord(REG_CiCON);
    con &= ~((1u << 19) | (1u << 20));
    mcpWriteWord(REG_CiCON, con);

    mcpWriteWord(REG_CiNBTCFG, ((ntseg1 - 1) << 16) | ((ntseg2 - 1) << 8) | (ntseg2 - 1));
    mcpWriteWord(REG_CiDBTCFG, ((dtseg1 - 1) << 16) | ((dtseg2 - 1) << 8) | (dtseg2 - 1));
    // Transmitter delay compensation: auto (TDCMOD=2), offset = data TSEG1
    // per Microchip's reference timings. Only matters when transmitting, which
    // this node never does beyond ACK bits, but keeps the config coherent.
    mcpWriteWord(REG_CiTDC, (2u << 16) | (dtseg1 << 8));

    // Time base: 40 MHz / (TBCPRE + 1) = 1 MHz, timestamp at end of frame
    // (TSEOF), i.e. when the frame became valid -- the same instant the
    // ESP32 TWAI path timestamps at.
    mcpWriteWord(REG_CiTSCON, (1u << 17) | (1u << 16) | (SYSCLK_HZ / 1000000UL - 1));

    // FIFO1 as RX: 64-byte payload (PLSIZE=7), RX_FIFO_DEPTH deep,
    // timestamps on (RXTSEN), not-empty and overflow interrupts on.
    mcpWriteWord(REG_CiFIFOCON1, (7u << 29) | ((uint32_t)(RX_FIFO_DEPTH - 1) << 24) |
                                     (1u << 5) | (1u << 3) | (1u << 0));

    // Filter 0 with an all-zero mask accepts every standard and extended ID
    // (MIDE=0), routed to FIFO1.
    mcpWriteWord(REG_CiFLTOBJ0, 0);
    mcpWriteWord(REG_CiMASK0, 0);
    mcpWriteByte(REG_CiFLTCON0, 0x80 | 1);  // FLTEN0 | F0BP=FIFO1

    // INT pin: RX (RXIE, bit 17) and RX overflow (RXOVIE, bit 27) only.
    uint8_t ie[2] = {0x02, 0x08};
    mcpWrite(REG_CiINT + 2, ie, 2);

    s_rxOverflows = 0;
    if (!mcpRequestMode(OPMODE_NORMAL_FD)) return "controller did not enter normal CAN FD mode";

    Serial.printf("[mcp] nominal %lu bit/s (%lu TQ, sp %lu%%), data %lu bit/s (%lu TQ, sp %lu%%), "
                  "RX FIFO %d x 64B\n",
                  (unsigned long)(SYSCLK_HZ / ntq), (unsigned long)ntq,
                  (unsigned long)((1 + ntseg1) * 100 / ntq), (unsigned long)(SYSCLK_HZ / dtq),
                  (unsigned long)dtq, (unsigned long)((1 + dtseg1) * 100 / dtq), RX_FIFO_DEPTH);
    return nullptr;
}

bool mcp2518fdRead(Mcp2518fdFrame &out) {
    uint8_t sta = mcpReadByte(REG_CiFIFOSTA1);
    if (sta & 0x08) {  // RXOVIF: the FIFO was full when a frame arrived
        s_rxOverflows++;
        mcpWriteByte(REG_CiFIFOSTA1, 0);  // clears RXOVIF; the other bits are read-only
    }
    if (!(sta & 0x01)) return false;  // TFNRFNIF: FIFO not empty

    uint16_t addr = RAM_START + (mcpReadWord(REG_CiFIFOUA1) & 0xFFF);
    uint8_t hdr[12];
    mcpRead(addr, hdr, sizeof hdr);
    uint32_t r0 = le32(hdr), r1 = le32(hdr + 4);

    // R0: SID 10:0, EID 28:11 (extended ID = SID << 18 | EID).
    // R1: DLC 3:0, IDE 4, RTR 5, BRS 6, FDF 7.
    out.extended = r1 & (1u << 4);
    out.id = out.extended ? (((r0 & 0x7FF) << 18) | ((r0 >> 11) & 0x3FFFF)) : (r0 & 0x7FF);
    out.fd = r1 & (1u << 7);
    out.brs = out.fd && (r1 & (1u << 6));
    bool rtr = !out.fd && (r1 & (1u << 5));
    uint8_t dlc = r1 & 0x0F;
    out.len = rtr ? 0 : (out.fd ? dlcToLen(dlc) : (dlc > 8 ? 8 : dlc));
    out.timestamp = le32(hdr + 8);
    if (out.len) {
        mcpRead(addr + sizeof hdr, out.data, (out.len + 3) & ~3);  // RAM reads are whole words
    }

    mcpWriteByte(REG_CiFIFOCON1 + 1, 0x01);  // UINC (bit 8): release the slot
    return true;
}

uint32_t mcp2518fdTimeBase() {
    return mcpReadWord(REG_CiTBC);
}

void mcp2518fdGetStatus(Mcp2518fdStatus &out) {
    uint32_t trec = mcpReadWord(REG_CiTREC);
    out.rec = trec & 0xFF;
    out.tec = (trec >> 8) & 0xFF;
    out.rxWarning = trec & (1u << 17);
    out.rxPassive = trec & (1u << 19);
    out.busOff = trec & (1u << 21);
    out.errorFlags = mcpReadWord(REG_CiBDIAG1) & 0x3FFF0000;
    mcpWriteWord(REG_CiBDIAG1, 0);  // error flags are sticky; clear for the next interval
    out.rxOverflows = s_rxOverflows;
}
