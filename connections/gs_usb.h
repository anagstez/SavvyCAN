#ifndef GS_USB_H
#define GS_USB_H

#include <QThread>
#include <QStringList>
#include <atomic>
#include <stdint.h>
#include <libusb.h>

#include "canconnection.h"
#include "canconconst.h"

/* ---- gs_usb wire structures (little endian, packed) ---------------------- */
#pragma pack(push, 1)
struct gs_host_config {
    uint32_t byte_order;                    /* 0x0000BEEF */
};

struct gs_device_config {
    uint8_t  reserved1, reserved2, reserved3;
    uint8_t  icount;                        /* number of channels - 1 */
    uint32_t sw_version;
    uint32_t hw_version;
};

struct gs_device_mode {
    uint32_t mode;
    uint32_t flags;
};

struct gs_device_bittiming {
    uint32_t prop_seg;
    uint32_t phase_seg1;
    uint32_t phase_seg2;
    uint32_t sjw;
    uint32_t brp;
};

struct gs_device_bt_const {
    uint32_t feature;
    uint32_t fclk_can;
    uint32_t tseg1_min, tseg1_max;
    uint32_t tseg2_min, tseg2_max;
    uint32_t sjw_max;
    uint32_t brp_min, brp_max, brp_inc;
};

struct gs_host_frame {
    uint32_t echo_id;                       /* 0xFFFFFFFF == received frame */
    uint32_t can_id;
    uint8_t  can_dlc;
    uint8_t  channel;
    uint8_t  flags;
    uint8_t  reserved;
    uint8_t  data[8];
    uint32_t timestamp_us;                  /* only if HW timestamping enabled */
};
#pragma pack(pop)

class GSUsbConnection : public CANConnection
{
    Q_OBJECT

public:
    /* pDeviceID format: "vid:pid" or "vid:pid:index", hex, e.g. "1d50:606f" */
    explicit GSUsbConnection(QString pDeviceID);
    virtual ~GSUsbConnection();

    /* helper for the connection dialog - lists attached gs_usb style devices */
    static QStringList enumerateDevices();

signals:
    void frameFromReader(CANFrame frame);   /* USB thread -> connection thread */

protected:
    void piStarted() override;
    void piStop() override;
    void piSetBusSettings(int pBusIdx, CANBus pBus) override;
    bool piGetBusSettings(int pBusIdx, CANBus& pBus) override;
    void piSuspend(bool pSuspend) override;
    bool piSendFrame(const CANFrame&) override;

private slots:
    void enqueueFrame(CANFrame frame);

private:
    bool openDevice();
    void closeDevice();
    bool ctrlOut(uint8_t req, uint16_t channel, void *data, uint16_t len);
    bool ctrlIn(uint8_t req, uint16_t channel, void *data, uint16_t len);
    bool startChannel(int channel, int bitrate, bool listenOnly);
    bool resetChannel(int channel);
    void readerLoop();

    libusb_context       *ctx    = nullptr;
    libusb_device_handle *dev    = nullptr;
    QThread              *reader = nullptr;

    std::atomic<bool> running{false};
    uint32_t          echoCounter = 0;
    int               numChannels = 1;
    int               frameSize   = 20;     /* 24 when HW timestamps are on */
    bool              hwTimestamp = false;
    gs_device_bt_const btConst{};

    uint16_t vid = 0, pid = 0;
    int      devIndex = 0;
};

#endif // GS_USB_H
