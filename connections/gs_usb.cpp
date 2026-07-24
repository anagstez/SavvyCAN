#include <QDateTime>
#include <QDebug>
#include <math.h>

#include "gs_usb.h"

/* ---- protocol constants -------------------------------------------------- */
#define GS_REQ_HOST_FORMAT   0
#define GS_REQ_BITTIMING     1
#define GS_REQ_MODE          2
#define GS_REQ_BT_CONST      4
#define GS_REQ_DEV_CONFIG    5

#define GS_MODE_RESET        0
#define GS_MODE_START        1

#define GS_FLAG_LISTEN_ONLY  (1u << 0)
#define GS_FLAG_HW_TIMESTAMP (1u << 4)

#define GS_FEAT_LISTEN_ONLY  (1u << 0)
#define GS_FEAT_HW_TIMESTAMP (1u << 4)

#define GS_ECHO_ID_RX        0xFFFFFFFFu

#define GS_EP_IN             0x81
#define GS_EP_OUT            0x02

#define RT_OUT (LIBUSB_ENDPOINT_OUT | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE)
#define RT_IN  (LIBUSB_ENDPOINT_IN  | LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_INTERFACE)

/* ---- bit timing solver --------------------------------------------------- */
static bool calcTiming(const gs_device_bt_const &btc, int bitrate, gs_device_bittiming &out)
{
    if (btc.fclk_can == 0 || bitrate <= 0) return false;

    double  bestErr = 1e9;
    bool    found   = false;
    uint32_t inc    = btc.brp_inc ? btc.brp_inc : 1;

    for (uint32_t brp = btc.brp_min; brp <= btc.brp_max; brp += inc)
    {
        uint32_t denom   = (uint32_t)bitrate * brp;
        uint32_t tqTotal = (btc.fclk_can + denom / 2) / denom;   /* 1 + tseg1 + tseg2 */
        if (tqTotal < 4) break;
        if (tqTotal > 1 + btc.tseg1_max + btc.tseg2_max) continue;

        /* aim at an 87.5% sample point */
        uint32_t tseg1 = (tqTotal * 875 + 500) / 1000;
        if (tseg1 < 1) tseg1 = 1;
        tseg1 -= 1;
        if (tseg1 < btc.tseg1_min) tseg1 = btc.tseg1_min;
        if (tseg1 > btc.tseg1_max) tseg1 = btc.tseg1_max;
        if (tqTotal < tseg1 + 2) continue;

        uint32_t tseg2 = tqTotal - tseg1 - 1;
        if (tseg2 < btc.tseg2_min || tseg2 > btc.tseg2_max) continue;

        double actual = (double)btc.fclk_can / (double)(brp * tqTotal);
        double err    = fabs(actual - (double)bitrate) / (double)bitrate;
        if (err < bestErr)
        {
            bestErr = err;
            found   = true;

            uint32_t prop = tseg1 / 2;
            if (prop < 1) prop = 1;

            out.prop_seg   = prop;
            out.phase_seg1 = tseg1 - prop;
            out.phase_seg2 = tseg2;
            out.sjw        = qMin(qMin(4u, tseg2), btc.sjw_max ? btc.sjw_max : 1u);
            out.brp        = brp;
        }
        if (bestErr == 0.0) break;
    }
    return found;
}

/* ---- ctor / dtor --------------------------------------------------------- */
GSUsbConnection::GSUsbConnection(QString pDeviceID) :
    CANConnection(pDeviceID, "GSUSB", CANCon::GSUSB,
                  0,        /* serial speed - unused */
                  500000,   /* default bus speed  */
                  false,    /* canFd              */
                  0,        /* data rate          */
                  2,        /* max buses          */
                  4000,     /* queue length       */
                  true)     /* run in own thread  */
{
    qDebug() << "GSUsbConnection()" << pDeviceID;

    QStringList parts = pDeviceID.split(':');
    if (parts.size() >= 2)
    {
        vid = (uint16_t)parts[0].toUInt(nullptr, 16);
        pid = (uint16_t)parts[1].toUInt(nullptr, 16);
    }
    if (parts.size() >= 3) devIndex = parts[2].toInt();

    qRegisterMetaType<CANFrame>("CANFrame");
    connect(this, &GSUsbConnection::frameFromReader,
            this, &GSUsbConnection::enqueueFrame, Qt::QueuedConnection);
}

GSUsbConnection::~GSUsbConnection()
{
    stop();
}

/* ---- device enumeration for the connection dialog ------------------------ */
QStringList GSUsbConnection::enumerateDevices()
{
    QStringList result;
    libusb_context *c = nullptr;
    if (libusb_init(&c) != 0) return result;

    libusb_device **list = nullptr;
    ssize_t count = libusb_get_device_list(c, &list);

    for (ssize_t i = 0; i < count; i++)
    {
        libusb_device_descriptor dd;
        if (libusb_get_device_descriptor(list[i], &dd) != 0) continue;

        libusb_config_descriptor *cfg = nullptr;
        if (libusb_get_active_config_descriptor(list[i], &cfg) != 0) continue;

        bool candidate = false;
        for (int ifc = 0; ifc < cfg->bNumInterfaces && !candidate; ifc++)
        {
            const libusb_interface_descriptor *id = &cfg->interface[ifc].altsetting[0];
            if (id->bInterfaceClass != LIBUSB_CLASS_VENDOR_SPEC) continue;

            int bulkIn = 0, bulkOut = 0;
            for (int e = 0; e < id->bNumEndpoints; e++)
            {
                const libusb_endpoint_descriptor *ep = &id->endpoint[e];
                if ((ep->bmAttributes & 0x03) != LIBUSB_TRANSFER_TYPE_BULK) continue;
                if (ep->bEndpointAddress & 0x80) bulkIn++; else bulkOut++;
            }
            if (bulkIn && bulkOut) candidate = true;
        }
        libusb_free_config_descriptor(cfg);

        if (candidate)
            result.append(QString("%1:%2")
                          .arg(dd.idVendor, 4, 16, QChar('0'))
                          .arg(dd.idProduct, 4, 16, QChar('0')));
    }

    if (list) libusb_free_device_list(list, 1);
    libusb_exit(c);
    return result;
}

/* ---- usb plumbing -------------------------------------------------------- */
bool GSUsbConnection::ctrlOut(uint8_t req, uint16_t channel, void *data, uint16_t len)
{
    int rc = libusb_control_transfer(dev, RT_OUT, req, channel, 0,
                                     (unsigned char *)data, len, 1000);
    if (rc != len) qDebug() << "gs_usb: ctrlOut req" << req << "failed:" << rc;
    return rc == len;
}

bool GSUsbConnection::ctrlIn(uint8_t req, uint16_t channel, void *data, uint16_t len)
{
    int rc = libusb_control_transfer(dev, RT_IN, req, channel, 0,
                                     (unsigned char *)data, len, 1000);
    if (rc != len) qDebug() << "gs_usb: ctrlIn req" << req << "failed:" << rc;
    return rc == len;
}

bool GSUsbConnection::openDevice()
{
    if (libusb_init(&ctx) != 0) return false;

    if (vid && pid)
        dev = libusb_open_device_with_vid_pid(ctx, vid, pid);

    if (!dev)
    {
        qDebug() << "gs_usb: device" << QString("%1:%2").arg(vid, 4, 16, QChar('0'))
                                                        .arg(pid, 4, 16, QChar('0'))
                 << "not found";
        libusb_exit(ctx);
        ctx = nullptr;
        return false;
    }

    /* interface 0 is the vendor class one on candleLight style firmware.
       macOS does not bind a driver to vendor class interfaces, so no detach needed. */
    if (libusb_claim_interface(dev, 0) != 0)
    {
        qDebug() << "gs_usb: claim_interface(0) failed";
        closeDevice();
        return false;
    }

    gs_host_config hc{};
    hc.byte_order = 0x0000BEEF;
    if (!ctrlOut(GS_REQ_HOST_FORMAT, 0, &hc, sizeof(hc)))
    {
        closeDevice();
        return false;
    }

    gs_device_config dc{};
    if (ctrlIn(GS_REQ_DEV_CONFIG, 0, &dc, sizeof(dc)))
    {
        numChannels = dc.icount + 1;
        if (numChannels < 1) numChannels = 1;
        if (numChannels > 2) numChannels = 2;   /* mBusData is sized for 2 here */
        qDebug() << "gs_usb: channels" << numChannels
                 << "sw" << dc.sw_version << "hw" << dc.hw_version;
    }

    if (!ctrlIn(GS_REQ_BT_CONST, 0, &btConst, sizeof(btConst)))
    {
        closeDevice();
        return false;
    }
    qDebug() << "gs_usb: fclk_can" << btConst.fclk_can << "feature" << Qt::hex << btConst.feature;

    hwTimestamp = (btConst.feature & GS_FEAT_HW_TIMESTAMP) != 0;
    frameSize   = hwTimestamp ? (int)sizeof(gs_host_frame)
                              : (int)(sizeof(gs_host_frame) - sizeof(uint32_t));
    return true;
}

void GSUsbConnection::closeDevice()
{
    if (dev)
    {
        libusb_release_interface(dev, 0);
        libusb_close(dev);
        dev = nullptr;
    }
    if (ctx)
    {
        libusb_exit(ctx);
        ctx = nullptr;
    }
}

bool GSUsbConnection::startChannel(int channel, int bitrate, bool listenOnly)
{
    gs_device_bittiming bt{};
    if (!calcTiming(btConst, bitrate, bt))
    {
        qDebug() << "gs_usb: cannot reach bitrate" << bitrate;
        return false;
    }
    qDebug() << "gs_usb: ch" << channel << "brp" << bt.brp
             << "prop" << bt.prop_seg << "ps1" << bt.phase_seg1
             << "ps2" << bt.phase_seg2 << "sjw" << bt.sjw;

    if (!ctrlOut(GS_REQ_BITTIMING, (uint16_t)channel, &bt, sizeof(bt))) return false;

    gs_device_mode md{};
    md.mode  = GS_MODE_START;
    md.flags = 0;
    if (listenOnly && (btConst.feature & GS_FEAT_LISTEN_ONLY)) md.flags |= GS_FLAG_LISTEN_ONLY;
    if (hwTimestamp)                                           md.flags |= GS_FLAG_HW_TIMESTAMP;

    return ctrlOut(GS_REQ_MODE, (uint16_t)channel, &md, sizeof(md));
}

bool GSUsbConnection::resetChannel(int channel)
{
    gs_device_mode md{};
    md.mode = GS_MODE_RESET;
    return ctrlOut(GS_REQ_MODE, (uint16_t)channel, &md, sizeof(md));
}

/* ---- reader thread ------------------------------------------------------- */
void GSUsbConnection::readerLoop()
{
    unsigned char buf[64];

    while (running.load())
    {
        int len = 0;
        int rc  = libusb_bulk_transfer(dev, GS_EP_IN, buf, frameSize, &len, 200);

        if (rc == LIBUSB_ERROR_TIMEOUT) continue;
        if (rc != 0)
        {
            qDebug() << "gs_usb: bulk in error" << rc;
            if (rc == LIBUSB_ERROR_NO_DEVICE || rc == LIBUSB_ERROR_PIPE) break;
            continue;
        }
        if (len < 20) continue;

        gs_host_frame hf{};
        memcpy(&hf, buf, qMin((size_t)len, sizeof(hf)));

        CANFrame frame;
        bool ext = (hf.can_id & 0x80000000u) != 0;
        bool rtr = (hf.can_id & 0x40000000u) != 0;

        frame.setExtendedFrameFormat(ext);
        frame.setFrameId(hf.can_id & (ext ? 0x1FFFFFFFu : 0x7FFu));
        frame.setFrameType(rtr ? QCanBusFrame::RemoteRequestFrame
                               : QCanBusFrame::DataFrame);
        frame.bus        = hf.channel;
        frame.isReceived = (hf.echo_id == GS_ECHO_ID_RX);

        int dlc = hf.can_dlc > 8 ? 8 : hf.can_dlc;
        frame.setPayload(QByteArray((const char *)hf.data, dlc));

        uint64_t ts = hwTimestamp && len >= 24
                    ? (uint64_t)hf.timestamp_us
                    : (uint64_t)QDateTime::currentMSecsSinceEpoch() * 1000ull;
        frame.setTimeStamp(QCanBusFrame::TimeStamp(0, ts));

        emit frameFromReader(frame);
    }
}

void GSUsbConnection::enqueueFrame(CANFrame frame)
{
    if (isCapSuspended()) return;

    CANFrame *frame_p = getQueue().get();
    if (frame_p)
    {
        *frame_p = frame;
        checkTargettedFrame(frame);
        getQueue().queue();
    }
}

/* ---- CANConnection interface --------------------------------------------- */
void GSUsbConnection::piStarted()
{
    CANConStatus stats;

    if (!openDevice())
    {
        setStatus(CANCon::NOT_CONNECTED);
        stats.conStatus       = getStatus();
        stats.numHardwareBuses = 0;
        emit status(stats);
        return;
    }

    mNumBuses = numChannels;

    for (int i = 0; i < numChannels; i++)
    {
        int speed = mBusData[i].mBus.getSpeed();
        if (speed <= 0) speed = 500000;
        startChannel(i, speed, false);
        mBusData[i].mConfigured = true;
        mBusData[i].mBus.setSpeed(speed);
        mBusData[i].mBus.setActive(true);
    }

    running.store(true);
    reader = QThread::create([this]() { this->readerLoop(); });
    reader->start();

    setStatus(CANCon::CONNECTED);
    stats.conStatus        = getStatus();
    stats.numHardwareBuses = mNumBuses;
    emit status(stats);
}

void GSUsbConnection::piStop()
{
    running.store(false);
    if (reader)
    {
        reader->wait(2000);
        delete reader;
        reader = nullptr;
    }

    if (dev)
        for (int i = 0; i < numChannels; i++) resetChannel(i);

    closeDevice();

    CANConStatus stats;
    setStatus(CANCon::NOT_CONNECTED);
    stats.conStatus        = getStatus();
    stats.numHardwareBuses = mNumBuses;
    emit status(stats);
}

void GSUsbConnection::piSetBusSettings(int pBusIdx, CANBus pBus)
{
    if (pBusIdx < 0 || pBusIdx >= numChannels) return;
    if (!dev) return;

    mBusData[pBusIdx].mBus        = pBus;
    mBusData[pBusIdx].mConfigured = true;

    /* gs_usb needs the channel stopped before bit timing can be changed */
    resetChannel(pBusIdx);
    if (pBus.isActive())
        startChannel(pBusIdx, pBus.getSpeed(), pBus.isListenOnly());
}

bool GSUsbConnection::piGetBusSettings(int pBusIdx, CANBus &pBus)
{
    if (pBusIdx < 0 || pBusIdx >= numChannels) return false;
    pBus = mBusData[pBusIdx].mBus;
    return true;
}

void GSUsbConnection::piSuspend(bool pSuspend)
{
    /* hardware keeps running, we simply stop feeding the queue */
    setCapSuspended(pSuspend);
    if (pSuspend) getQueue().flush();
}

bool GSUsbConnection::piSendFrame(const CANFrame &frame)
{
    if (!dev) return false;

    int ch = frame.bus;
    if (ch < 0 || ch >= numChannels) ch = 0;

    gs_host_frame hf{};
    hf.echo_id = echoCounter++;
    if (hf.echo_id == GS_ECHO_ID_RX) hf.echo_id = echoCounter++;

    hf.can_id  = frame.frameId() & (frame.hasExtendedFrameFormat() ? 0x1FFFFFFFu : 0x7FFu);
    if (frame.hasExtendedFrameFormat())                     hf.can_id |= 0x80000000u;
    if (frame.frameType() == QCanBusFrame::RemoteRequestFrame) hf.can_id |= 0x40000000u;

    QByteArray payload = frame.payload();
    int dlc = payload.length() > 8 ? 8 : payload.length();
    hf.can_dlc = (uint8_t)dlc;
    hf.channel = (uint8_t)ch;
    memcpy(hf.data, payload.constData(), dlc);

    int len = 0;
    int rc  = libusb_bulk_transfer(dev, GS_EP_OUT, (unsigned char *)&hf, frameSize, &len, 1000);
    if (rc != 0)
    {
        qDebug() << "gs_usb: bulk out error" << rc;
        return false;
    }
    /* the frame comes back on the IN endpoint as a TX echo, so it is not
       queued here - readerLoop() will queue it with isReceived == false */
    return true;
}
