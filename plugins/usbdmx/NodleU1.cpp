/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Library General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * NodleU1.cpp
 * The synchronous and asynchronous Nodle widgets.
 * Copyright (C) 2015 Stefan Krupop
 */

#include "plugins/usbdmx/NodleU1.h"

#include <string.h>
#include <unistd.h>
#include <algorithm>
#include <string>

#include "ola/Logging.h"
#include "ola/Constants.h"
#include "ola/StringUtils.h"
#include "plugins/usbdmx/AsyncUsbReceiver.h"
#include "plugins/usbdmx/AsyncUsbSender.h"
#include "plugins/usbdmx/LibUsbAdaptor.h"
#include "plugins/usbdmx/ThreadedUsbReceiver.h"
#include "plugins/usbdmx/ThreadedUsbSender.h"

namespace ola {
namespace plugin {
namespace usbdmx {

using std::string;

const char NodleU1::NODLE_MODE_KEY[] = "nodle_mode";
int NodleU1::NODLE_DEFAULT_MODE = 6;
int NodleU1::NODLE_MIN_MODE = 0;
int NodleU1::NODLE_MAX_MODE = 7;

namespace {

static const unsigned char WRITE_ENDPOINT = 0x02;
static const unsigned char READ_ENDPOINT = 0x81;
static const unsigned int URB_TIMEOUT_MS = 50;
static const int CONFIGURATION = 1;
static const int INTERFACE = 0;

static const unsigned int DATABLOCK_SIZE = 33;

/*
 * @brief Send chosen mode to the DMX device
 * @param handle the libusb_device_handle to use.
 * @returns true when mode was set 
 */
bool SetInterfaceMode(LibUsbAdaptor *adaptor,
                      libusb_device_handle *handle,
                      uint8_t mode) {
  unsigned char usb_data[DATABLOCK_SIZE];
  int transferred;

  memset(usb_data, 0, sizeof(usb_data));
  usb_data[0] = 16;
  usb_data[1] = mode;

  int ret = adaptor->InterruptTransfer(
      handle, WRITE_ENDPOINT, reinterpret_cast<unsigned char*>(usb_data),
      DATABLOCK_SIZE, &transferred, URB_TIMEOUT_MS);
  if (ret) {
    OLA_WARN << "InterruptTransfer(): " << adaptor->ErrorCodeToString(ret)
             << ", transferred " << transferred << " / " << DATABLOCK_SIZE;
  }
  return ret == 0;
}


/*
 * @brief Attempt to open a handle to a Nodle widget.
 * @param adaptor the LibUsbAdaptor to use.
 * @param usb_device the libusb_device to use.
 * @returns A libusb_device_handle or NULL if it failed.
 */
libusb_device_handle *OpenNodleU1Widget(LibUsbAdaptor *adaptor,
                                        libusb_device *usb_device) {
  libusb_device_handle *usb_handle;
  bool ok = adaptor->OpenDevice(usb_device, &usb_handle);
  if (!ok) {
    return NULL;
  }

  int ret_code = adaptor->DetachKernelDriver(usb_handle, INTERFACE);
  if (ret_code != 0 && ret_code != LIBUSB_ERROR_NOT_FOUND) {
    OLA_WARN << "Failed to detach kernel driver: "
             << adaptor->ErrorCodeToString(ret_code);
    adaptor->Close(usb_handle);
    return NULL;
  }

  // this device only has one configuration
  ret_code = adaptor->SetConfiguration(usb_handle, CONFIGURATION);
  if (ret_code) {
    OLA_WARN << "Nodle set config failed, with libusb error code "
             << adaptor->ErrorCodeToString(ret_code);
    adaptor->Close(usb_handle);
    return NULL;
  }

  if (adaptor->ClaimInterface(usb_handle, INTERFACE)) {
    OLA_WARN << "Failed to claim Nodle USB device";
    adaptor->Close(usb_handle);
    return NULL;
  }

  return usb_handle;
}
}  // namespace

// NodleU1ThreadedSender
// -----------------------------------------------------------------------------

/*
 * Sends messages to a Nodle device in a separate thread.
 */
class NodleU1ThreadedSender: public ThreadedUsbSender {
 public:
  NodleU1ThreadedSender(LibUsbAdaptor *adaptor,
                        libusb_device *usb_device,
                        libusb_device_handle *handle)
      : ThreadedUsbSender(usb_device, handle),
        m_adaptor(adaptor) {
    m_tx_buffer.Blackout();
    m_last_tx_buffer.Blackout();
  }

 private:
  LibUsbAdaptor* const m_adaptor;
  DmxBuffer m_tx_buffer;
  DmxBuffer m_last_tx_buffer;

  bool TransmitBuffer(libusb_device_handle *handle,
                      const DmxBuffer &buffer);
  bool SendDataChunk(libusb_device_handle *handle,
                     uint8_t *usb_data);
};

bool NodleU1ThreadedSender::TransmitBuffer(libusb_device_handle *handle,
                                           const DmxBuffer &buffer) {
  m_tx_buffer.SetRange(0, buffer.GetRaw(), buffer.Size());

  unsigned char usb_data[DATABLOCK_SIZE];
  const unsigned int size = m_tx_buffer.Size();
  const uint8_t *data = m_tx_buffer.GetRaw();
  const uint8_t *last_data = m_last_tx_buffer.GetRaw();
  unsigned int i = 0;

  memset(usb_data, 0, sizeof(usb_data));

  while (i < size - 32) {
    if (memcmp(data + i, last_data + i, 32) != 0) {
      usb_data[0] = i / 32;
      memcpy(usb_data + 1, data + i, 32);
      m_last_tx_buffer.SetRange(i, data + i, 32);
      if (!SendDataChunk(handle, usb_data)) {
        return false;
      }
    }
    i += 32;
  }

  if (memcmp(data + i, last_data + i, size - i) != 0) {
    usb_data[0] = i / 32;
    memcpy(usb_data + 1, data + i, size - i);
    m_last_tx_buffer.SetRange(i, data + i, size - i);
    if (!SendDataChunk(handle, usb_data)) {
      return false;
    }
  }

  return true;
}

bool NodleU1ThreadedSender::SendDataChunk(libusb_device_handle *handle,
                                          uint8_t *usb_data) {
  int transferred;
  int ret = m_adaptor->InterruptTransfer(
      handle, WRITE_ENDPOINT, reinterpret_cast<unsigned char*>(usb_data),
      DATABLOCK_SIZE, &transferred, URB_TIMEOUT_MS);
  if (ret) {
    OLA_WARN << "InterruptTransfer(): " << m_adaptor->ErrorCodeToString(ret)
             << ", transferred " << transferred << " / " << DATABLOCK_SIZE;
  }
  return ret == 0;
}

// NodleU1ThreadedReceiver
// -----------------------------------------------------------------------------

/*
 * Receives messages from a Nodle device in a separate thread.
 */
class NodleU1ThreadedReceiver: public ThreadedUsbReceiver {
 public:
  NodleU1ThreadedReceiver(LibUsbAdaptor *adaptor,
                          libusb_device *usb_device,
                          libusb_device_handle *handle)
      : ThreadedUsbReceiver(usb_device, handle),
        m_adaptor(adaptor) {
  }

 private:
  LibUsbAdaptor* const m_adaptor;

  bool ReceiveBuffer(libusb_device_handle *handle,
                     DmxBuffer *buffer,
                     bool *buffer_updated);
  bool ReadDataChunk(libusb_device_handle *handle,
                     uint8_t *usb_data);
};

bool NodleU1ThreadedReceiver::ReceiveBuffer(libusb_device_handle *handle,
                                            DmxBuffer *buffer,
                                            bool *buffer_updated) {
  unsigned char usb_data[DATABLOCK_SIZE];

  if (ReadDataChunk(handle, usb_data)) {
    if (usb_data[0] < 16) {
      uint16_t startOff = usb_data[0] * 32;
      for (int i = 0; i < 32; i++) {
        buffer->SetChannel(startOff + i, usb_data[i + 1]);
      }
      *buffer_updated = true;
    }
  }

  return true;
}

bool NodleU1ThreadedReceiver::ReadDataChunk(libusb_device_handle *handle,
                                            uint8_t *usb_data) {
  int transferred;
  int ret = m_adaptor->InterruptTransfer(
      handle, READ_ENDPOINT, reinterpret_cast<unsigned char*>(usb_data),
      DATABLOCK_SIZE, &transferred, URB_TIMEOUT_MS);
  if (ret && ret != LIBUSB_ERROR_TIMEOUT) {
    OLA_WARN << "InterruptTransfer(): " << m_adaptor->ErrorCodeToString(ret)
             << ", transferred " << transferred << " / " << DATABLOCK_SIZE;
  }
  return ret == 0;
}

// SynchronousNodleU1
// -----------------------------------------------------------------------------

SynchronousNodleU1::SynchronousNodleU1(
    LibUsbAdaptor *adaptor,
    libusb_device *usb_device,
    const string &serial,
    unsigned int mode)
    : NodleU1(adaptor, serial, mode),
      m_usb_device(usb_device) {
}

bool SynchronousNodleU1::Init() {
  libusb_device_handle *usb_handle = OpenNodleU1Widget(
      m_adaptor, m_usb_device);

  if (!usb_handle) {
    return false;
  }

  SetInterfaceMode(m_adaptor, usb_handle, m_mode);

  if (m_mode & 2) {  // output port active
    std::auto_ptr<NodleU1ThreadedSender> sender(
        new NodleU1ThreadedSender(m_adaptor, m_usb_device, usb_handle));
    if (!sender->Start()) {
      return false;
    }
    m_sender.reset(sender.release());
  }

  if (m_mode & 4) {  // input port active
    std::auto_ptr<NodleU1ThreadedReceiver> receiver(
        new NodleU1ThreadedReceiver(m_adaptor, m_usb_device, usb_handle));
    if (!receiver->Start()) {
      return false;
    }
    m_receiver.reset(receiver.release());
  }

  return true;
}

bool SynchronousNodleU1::SendDMX(const DmxBuffer &buffer) {
  return m_sender.get() ? m_sender->SendDMX(buffer) : false;
}

void SynchronousNodleU1::SetDmxCallback(Callback0<void> *callback) {
  if (m_receiver.get()) {
    m_receiver->SetReceiveCallback(callback);
  }
}

const DmxBuffer &SynchronousNodleU1::GetDmxInBuffer() const {
  return m_receiver->GetDmxInBuffer();
}

// NodleU1AsyncUsbReceiver
// -----------------------------------------------------------------------------
class NodleU1AsyncUsbReceiver : public AsyncUsbReceiver {
 public:
  NodleU1AsyncUsbReceiver(LibUsbAdaptor *adaptor,
                          libusb_device *usb_device,
                          unsigned int mode)
      : AsyncUsbReceiver(adaptor, usb_device),
        m_mode(mode) {
    m_packet = new uint8_t[DATABLOCK_SIZE];
  }

  ~NodleU1AsyncUsbReceiver() {
    CancelTransfer();
    if (m_packet) {
      delete[] m_packet;
    }
  }

  libusb_device_handle* SetupHandle() {
    libusb_device_handle *handle = OpenNodleU1Widget(m_adaptor, m_usb_device);
    if (handle) {
      SetInterfaceMode(m_adaptor, handle, m_mode);
    }
    return handle;
  }

  bool PerformTransfer();

  bool TransferCompleted(DmxBuffer *buffer);

 private:
  unsigned int m_mode;
  uint8_t *m_packet;

  DISALLOW_COPY_AND_ASSIGN(NodleU1AsyncUsbReceiver);
};

bool NodleU1AsyncUsbReceiver::PerformTransfer() {
  FillInterruptTransfer(READ_ENDPOINT, m_packet,
                        DATABLOCK_SIZE, URB_TIMEOUT_MS);
  return (SubmitTransfer() == 0);
}

bool NodleU1AsyncUsbReceiver::TransferCompleted(DmxBuffer *buffer) {
  if (m_packet[0] < 16) {
    uint16_t startOff = m_packet[0] * 32;
    for (int i = 0; i < 32; i++) {
      buffer->SetChannel(startOff + i, m_packet[i + 1]);
    }
    return true;
  }
  return false;
}

// NodleU1AsyncUsbSender
// -----------------------------------------------------------------------------
class NodleU1AsyncUsbSender : public AsyncUsbSender {
 public:
  NodleU1AsyncUsbSender(LibUsbAdaptor *adaptor,
                        libusb_device *usb_device,
                        unsigned int mode)
      : AsyncUsbSender(adaptor, usb_device),
        m_mode(mode),
        m_buffer_offset(0),
        m_packet(NULL) {
    m_tx_buffer.Blackout();
  }

  ~NodleU1AsyncUsbSender() {
    CancelTransfer();
    if (m_packet) {
      delete[] m_packet;
    }
  }

  libusb_device_handle* SetupHandle() {
    libusb_device_handle *handle = OpenNodleU1Widget(m_adaptor, m_usb_device);
    m_packet = new uint8_t[DATABLOCK_SIZE];
    if (handle) {
      SetInterfaceMode(m_adaptor, handle, m_mode);
    }
    return handle;
  }

  bool PerformTransfer(const DmxBuffer &buffer);

  void PostTransferHook();

 private:
  unsigned int m_mode;
  DmxBuffer m_tx_buffer;
  // This tracks where we are in m_tx_buffer. A value of 0 means we're at the
  // start of a DMX frame.
  unsigned int m_buffer_offset;
  uint8_t *m_packet;

  bool ContinueTransfer();

  bool SendInitialChunk(const DmxBuffer &buffer);

  bool SendChunk() {
    FillInterruptTransfer(WRITE_ENDPOINT, m_packet,
                          DATABLOCK_SIZE, URB_TIMEOUT_MS);
    return (SubmitTransfer() == 0);
  }

  DISALLOW_COPY_AND_ASSIGN(NodleU1AsyncUsbSender);
};

bool NodleU1AsyncUsbSender::PerformTransfer(const DmxBuffer &buffer) {
  if (m_buffer_offset == 0) {
    return SendInitialChunk(buffer);
  }
  // Otherwise we're part way through a transfer, do nothing.
  return true;
}

void NodleU1AsyncUsbSender::PostTransferHook() {
  if (m_buffer_offset < m_tx_buffer.Size()) {
    ContinueTransfer();
  } else if (m_buffer_offset >= m_tx_buffer.Size()) {
    // That was the last chunk.
    m_buffer_offset = 0;

    if (TransferPending()) {
      // If we have a pending transfer, the next chunk is going to be sent
      // once we return.
      m_tx_buffer.Reset();
    }
  }
}

bool NodleU1AsyncUsbSender::ContinueTransfer() {
  unsigned int length = 32;

  m_packet[0] = m_buffer_offset / 32;

  m_tx_buffer.GetRange(m_buffer_offset, m_packet + 1, &length);
  memset(m_packet + 1 + length, 0, 32 - length);
  m_buffer_offset += length;
  return (SendChunk() == 0);
}

bool NodleU1AsyncUsbSender::SendInitialChunk(const DmxBuffer &buffer) {
  unsigned int length = 32;

  m_tx_buffer.SetRange(0, buffer.GetRaw(), buffer.Size());

  m_packet[0] = 0;
  m_tx_buffer.GetRange(0, m_packet + 1, &length);
  memset(m_packet + 1 + length, 0, 32 - length);

  unsigned int slots_sent = length;
  if (slots_sent < m_tx_buffer.Size()) {
    // There are more frames to send.
    m_buffer_offset = slots_sent;
  }
  return (SendChunk() == 0);
}

// AsynchronousNodleU1
// -----------------------------------------------------------------------------

AsynchronousNodleU1::AsynchronousNodleU1(
    LibUsbAdaptor *adaptor,
    libusb_device *usb_device,
    const string &serial,
    unsigned int mode)
    : NodleU1(adaptor, serial, mode) {
  if (mode & 2) {  // output port active
    m_sender.reset(new NodleU1AsyncUsbSender(m_adaptor, usb_device, mode));
  }

  if (mode & 4) {  // input port active
    m_receiver.reset(new NodleU1AsyncUsbReceiver(m_adaptor, usb_device, mode));
  }
}

bool AsynchronousNodleU1::Init() {
  bool err = false;
  if (m_sender.get()) {
    err |= !m_sender->Init();
  }
  if (m_receiver.get()) {
    if (m_sender.get()) {
      // If we have a sender, use it's USB handle
      err |= !m_receiver->Init(m_sender->GetHandle());
    } else {
      err |= !m_receiver->Init();
    }
    if (!err) {
      m_receiver->Start();
    }
  }
  return !err;
}

bool AsynchronousNodleU1::SendDMX(const DmxBuffer &buffer) {
  return m_sender.get() ? m_sender->SendDMX(buffer) : false;
}

void AsynchronousNodleU1::SetDmxCallback(Callback0<void> *callback) {
  if (m_receiver.get()) {
    m_receiver->SetReceiveCallback(callback);
  }
}

const DmxBuffer &AsynchronousNodleU1::GetDmxInBuffer() const {
  return m_receiver->GetDmxInBuffer();
}
}  // namespace usbdmx
}  // namespace plugin
}  // namespace ola
