// Copyright 2014-2015 Josh Pieper, jjp@pobox.com.  All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "linux_input.h"

#include <linux/input.h>

#include <boost/asio/posix/stream_descriptor.hpp>
#include <boost/format.hpp>

namespace mjmech {
namespace base {
class LinuxInput::Impl : boost::noncopyable {
 public:
  Impl(boost::asio::io_service& service) : service_(service) {}

  boost::asio::io_service& service_;
  boost::asio::posix::stream_descriptor stream_{service_};

  struct input_event input_event_;

  std::map<int, AbsInfo> abs_info_;
};

LinuxInput::LinuxInput(boost::asio::io_service& service)
    : impl_(new Impl(service)) {}

LinuxInput::LinuxInput(boost::asio::io_service& service,
                       const std::string& device)
    : LinuxInput(service) {
  Open(device);
}

LinuxInput::~LinuxInput() {}

boost::asio::io_service& LinuxInput::get_io_service() {
  return impl_->service_;
}

void LinuxInput::Open(const std::string& device) {
  int fd = ::open(device.c_str(), O_RDONLY | O_NONBLOCK);
  if (fd < 0) { throw SystemError::syserrno("opening device: " + device); }

  impl_->stream_.assign(fd);

  auto f = features(EV_ABS);
  for (size_t i = 0; i < f.capabilities.size(); ++i) {
    if (!f.capabilities.test(i)) { continue; }

    struct cgabs {
      cgabs(int axis) : axis(axis) {}
      int name() const { return EVIOCGABS(axis); }
      void* data() { return &abs_info; }

      const int axis;
      struct input_absinfo abs_info;
    };

    cgabs op(i);
    impl_->stream_.io_control(op);
    AbsInfo info;
    info.axis = i;
    info.minimum = op.abs_info.minimum;
    info.maximum = op.abs_info.maximum;
    info.fuzz = op.abs_info.fuzz;
    info.flat = op.abs_info.flat;
    info.resolution = op.abs_info.resolution;
    info.value = op.abs_info.value;

    impl_->abs_info_[i] = info;
  }
}

int LinuxInput::fileno() const {
  return impl_->stream_.native_handle();
}

std::string LinuxInput::name() const {
  struct cgname {
    int name() const { return EVIOCGNAME(256); }
    void* data() { return buffer; }

    char buffer[257] = {};
  };

  cgname op;
  impl_->stream_.io_control(op);

  return std::string(op.buffer);
}

LinuxInput::AbsInfo LinuxInput::abs_info(int axis) const {
  auto it = impl_->abs_info_.find(axis);
  if (it != impl_->abs_info_.end()) { return it->second; }

  AbsInfo result;
  result.axis = axis;
  result.minimum = -100;
  result.maximum = 100;
  result.resolution = 100;
  return result;
}

LinuxInput::Features LinuxInput::features(int ev_type) const {
  struct cgbit {
    cgbit(int name) : name_(name) {}

    int name() const { return EVIOCGBIT(name_, sizeof(buffer)); }
    void* data() { return buffer; }

    const int name_;
    char buffer[256] = {};
  };

  cgbit op(ev_type);
  impl_->stream_.io_control(op);

  size_t end = [ev_type]() {
    switch (ev_type) {
      case EV_SYN: { return SYN_MAX; }
      case EV_KEY: { return KEY_MAX; }
      case EV_REL: { return REL_MAX; }
      case EV_ABS: { return ABS_MAX; }
    }
    BOOST_ASSERT(false);
  }() / 8 + 1;

  Features result;
  result.ev_type = ev_type;

  for (size_t i = 0; i < end; i++) {
    for (int bit = 0; bit < 8; bit++) {
      result.capabilities.push_back((op.buffer[i] >> bit) & 0x1 ? true : false);
    }
  }

  return result;
}

void LinuxInput::AsyncRead(Event* event, ErrorHandler handler) {
  impl_->stream_.async_read_some(
      boost::asio::buffer(&impl_->input_event_, sizeof(impl_->input_event_)),
      [event, handler, this](ErrorCode ec, std::size_t size) {
        if (ec) {
          ec.Append("reading input event");
          impl_->service_.post(std::bind(handler, ec));
          return;
        }

        if (size != sizeof(impl_->input_event_)) {
          impl_->service_.post(
              std::bind(
                  handler, ErrorCode::einval("short read for input event")));
          return;
        }

        event->ev_type = impl_->input_event_.type;
        event->code = impl_->input_event_.code;
        event->value = impl_->input_event_.value;

        /// Update our internal absolute structure if necessary.
        if (event->ev_type == EV_ABS) {
          auto it = impl_->abs_info_.find(event->code);
          if (it != impl_->abs_info_.end()) {
            it->second.value = event->value;
          }
        }

        impl_->service_.post(std::bind(handler, ErrorCode()));
      });
}

void LinuxInput::cancel() {
  impl_->stream_.cancel();
}

std::ostream& operator<<(std::ostream& ostr, const LinuxInput& rhs) {
  ostr << boost::format("<LinuxInput '%s'>") % rhs.name();
  return ostr;
}

namespace {
std::string MapBitmask(boost::dynamic_bitset<> bitset,
                       std::function<std::string (int)> mapper) {
  std::vector<std::string> elements;
  for (size_t i = 0; i < bitset.size(); i++) {
    if (bitset.test(i)) { elements.push_back(mapper(i)); }
  }

  std::ostringstream result;
  for (size_t i = 0; i < elements.size(); i++) {
    if (i != 0) { result << "|"; }
    result << elements[i];
  }
  return result.str();
}

std::string MapEvType(int ev_type) {
  switch (ev_type) {
    case EV_SYN: { return "EV_SYN"; }
    case EV_KEY: { return "EV_KEY"; }
    case EV_REL: { return "EV_REL"; }
    case EV_ABS: { return "EV_ABS"; }
    default: { return (boost::format("EV_%02X") % ev_type).str(); }
  }
}

std::string MapSyn(int code) {
  switch (code) {
    case SYN_REPORT: { return "SYN_REPORT"; }
    case SYN_CONFIG: { return "SYN_CONFIG"; }
    case SYN_MT_REPORT: { return "SYN_MT_REPORT"; }
    case SYN_DROPPED: { return "SYN_DROPPED"; }
    default: { return (boost::format("SYN_%02X") % code).str(); }
  }
}

std::string MapKey(int code) {
  return (boost::format("KEY_%03X") % code).str();
}

std::string MapRel(int code) {
  switch (code) {
    case REL_X: { return "REL_X"; }
    case REL_Y: { return "REL_Y"; }
    case REL_Z: { return "REL_Z"; }
    case REL_RX: { return "REL_RX"; }
    case REL_RY: { return "REL_RY"; }
    case REL_RZ: { return "REL_RZ"; }
    case REL_HWHEEL: { return "REL_HWHEEL"; }
    case REL_DIAL: { return "REL_DIAL"; }
    case REL_WHEEL: { return "REL_WHEEL"; }
    case REL_MISC: { return "REL_MISC"; }
    default: { return (boost::format("REL_%02X") % code).str(); }
  }
}

std::string MapAbs(int code) {
  switch (code) {
    case ABS_X: { return "ABS_X"; }
    case ABS_Y: { return "ABS_Y"; }
    case ABS_Z: { return "ABS_Z"; }
    case ABS_RX: { return "ABS_RX"; }
    case ABS_RY: { return "ABS_RY"; }
    case ABS_RZ: { return "ABS_RZ"; }
    case ABS_HAT0X: { return "ABS_HAT0X"; }
    case ABS_HAT0Y: { return "ABS_HAT0Y"; }
    case ABS_HAT1X: { return "ABS_HAT1X"; }
    case ABS_HAT1Y: { return "ABS_HAT1Y"; }
    case ABS_HAT2X: { return "ABS_HAT2X"; }
    case ABS_HAT2Y: { return "ABS_HAT2Y"; }
    case ABS_HAT3X: { return "ABS_HAT3X"; }
    case ABS_HAT3Y: { return "ABS_HAT3Y"; }

    default: { return (boost::format("ABS_%02X") % code).str(); }
  }
}

std::string MapUnknown(int code) {
  return (boost::format("%03X") % code).str();
}

std::function<std::string (int)> MakeCodeMapper(int ev_type) {
  switch (ev_type) {
    case EV_SYN: { return MapSyn; }
    case EV_KEY: { return MapKey; }
    case EV_REL: { return MapRel; }
    case EV_ABS: { return MapAbs; }
    default: { return MapUnknown; }
  }
}
}

std::ostream& operator<<(std::ostream& ostr, const LinuxInput::AbsInfo& rhs) {
  ostr << boost::format("<AbsInfo %s val=%d min=%d max=%d scaled=%f>") %
      MapAbs(rhs.axis) % rhs.value % rhs.minimum % rhs.maximum % rhs.scaled();
  return ostr;
}

std::ostream& operator<<(std::ostream& ostr, const LinuxInput::Features& rhs) {
  ostr << boost::format("<Features type=%s %s>") %
      MapEvType(rhs.ev_type) %
      MapBitmask(rhs.capabilities, MakeCodeMapper(rhs.ev_type));
  return ostr;
}

std::ostream& operator<<(std::ostream& ostr, const LinuxInput::Event& rhs) {
  ostr << boost::format("<Event ev_type=%s code=%s value=%d>") %
      MapEvType(rhs.ev_type) %
      MakeCodeMapper(rhs.ev_type)(rhs.code) %
      rhs.value;
  return ostr;
}

}
}
