// Copyright 2015-2016 Josh Pieper, jjp@pobox.com.  All rights reserved.
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

#include "servo_monitor.h"

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>

#include "base/common.h"
#include "base/deadline_timer.h"
#include "base/fail.h"
#include "base/logging.h"
#include "base/now.h"

#include "servo_interface.h"

namespace mjmech {
namespace mech {

class ServoMonitor::Impl : boost::noncopyable {
 public:
  Impl(ServoMonitor* parent,
       boost::asio::io_service& service,
       HerkuleXServo* servo)
      : parent_(parent),
        service_(service),
        servo_(servo),
        timer_(service) {}

  void SetupInitialServos() {
    std::vector<int> ids = SplitServoIds(parameters_.servos);

    for (int id: ids) { servo_data_[id]; }
  }

  void Start() {
    timer_.expires_from_now(
        base::ConvertSecondsToDuration(parameters_.period_s));
    timer_.async_wait(std::bind(&Impl::HandleTimeout, this,
                                std::placeholders::_1));
  }

  void HandleTimeout(base::ErrorCode ec) {
    FailIf(ec);
    Start();

    // We shouldn't have any outstanding requests by the time we get
    // here.  If we do, then our timeout is probably too short.
    BOOST_ASSERT(!outstanding_);

    switch (state_.mode) {
      case State::kIdle: { DoIdle(); break; }
      case State::kTemperature: { DoTemperature(); break; }
    }
  }

  void DoIdle() {
    // Pick out the next servo to use.  Find one that exists and isn't
    // on parole.
    boost::optional<int> next_servo = FindNext();

    if (!next_servo) {
      // Nothing is ready, let's just stick around in this state until
      // the next polling period.
      return;
    }

    state_.last_servo = *next_servo;

    // Send out a request for voltage.
    outstanding_ = true;
    servo_->RamRead(state_.last_servo,
                    HerkuleXConstants::voltage().position,
                    HerkuleXConstants::voltage().length,
                    std::bind(&Impl::HandleVoltage, this,
                                 state_.last_servo,
                                 std::placeholders::_1,
                                 std::placeholders::_2));
  }

  void DoTemperature() {
    outstanding_ = true;
    servo_->RamRead(state_.last_servo,
                    HerkuleXConstants::temperature_c().position,
                    HerkuleXConstants::temperature_c().length,
                    std::bind(&Impl::HandleTemperature, this,
                              state_.last_servo,
                              std::placeholders::_1,
                              std::placeholders::_2));
  }

  void HandleVoltage(int requested_servo,
                     base::ErrorCode ec,
                     const HerkuleXBase::MemReadResponse& response) {
    BOOST_ASSERT(outstanding_);
    outstanding_ = false;

    const auto now = base::Now(service_);
    if (ec == boost::asio::error::operation_aborted ||
        ec == herkulex_error::synchronization_error) {
      UpdateServoTimeout(requested_servo, now);
      return;
    }
    FailIf(ec);

    auto& servo = servo_data_[requested_servo];

    // Yay, we got a result!
    UpdateServoFound(requested_servo, now);

    BOOST_ASSERT(response.servo == requested_servo);
    BOOST_ASSERT(response.register_data.size() >= 1);
    servo.voltage_V =
        HerkuleXBase::CountsToVoltage(response.register_data[0]);
    servo.torque_on = response.motor_on;

    CheckFaults(response);

    // Next time, ask for the same servo's temperature.
    state_.mode = State::kTemperature;
  }

  void HandleTemperature(
      int requested_servo,
      base::ErrorCode ec,
      const HerkuleXBase::MemReadResponse& response) {
    BOOST_ASSERT(outstanding_);
    outstanding_ = false;

    const auto now = base::Now(service_);
    if (ec == boost::asio::error::operation_aborted ||
        ec == herkulex_error::synchronization_error) {
      UpdateServoTimeout(requested_servo, now);
      return;
    }
    FailIf(ec);

    auto& servo = servo_data_[requested_servo];

    UpdateServoFound(requested_servo, now);

    BOOST_ASSERT(response.servo == requested_servo);
    BOOST_ASSERT(response.register_data.size() >= 1);
    servo.temperature_C =
        HerkuleXBase::CountsToTemperatureC(response.register_data[0]);
    servo.torque_on = response.motor_on;

    CheckFaults(response);

    EmitData();

    // Switch back to idle so we move on to the next servo.
    state_.mode = State::kIdle;
  }

  void UpdateServoTimeout(int requested_servo,
                          boost::posix_time::ptime now) {
    auto& servo = servo_data_[requested_servo];

    // We got a timeout.  Update our parole state.
    if (servo.parole_time_s == 0.0 ||
        servo.next_update.is_not_a_date_time()) {
      servo.parole_time_s = parameters_.parole_min_time_s;
      servo.next_update = now;
    } else {
      servo.parole_time_s = std::min(parameters_.parole_max_time_s,
                                     servo.parole_time_s * 2);
    }

    servo.next_update += base::ConvertSecondsToDuration(servo.parole_time_s);

    // Skip on to the next servo.
    state_.mode = State::kIdle;
  }

  void UpdateServoFound(int requested_servo,
                        boost::posix_time::ptime now) {
    auto& servo = servo_data_[requested_servo];

    servo.parole_time_s = 0.0;
    servo.next_update = boost::posix_time::ptime();
    servo.last_update = now;
  }

  boost::optional<int> FindNext() const {
    const auto now = base::Now(service_);

    int start = state_.last_servo;
    int current = start;
    do {
      auto it = servo_data_.upper_bound(current);
      if (it != servo_data_.end()) {
        // Is this a valid candidate?
        const auto& servo = it->second;
        if (servo.next_update.is_not_a_date_time() ||
            now > servo.next_update) {
          return it->first;
        }
        // Nope, he is on parole.  Keep looking.
        current = it->first;
      } else {
        // We hit the end, go back to the start.
        current = -1;
      }

    } while (current != start);

    // Oops, nothing is ready.
    return boost::none;
  }

  void EmitData() {
    ServoData data;
    data.timestamp = base::Now(service_);

    for (const auto& pair: servo_data_) {
      ServoData::Servo servo;
      servo.address = pair.first;
      servo.last_update = pair.second.last_update;
      servo.voltage_V = pair.second.voltage_V;
      servo.temperature_C = pair.second.temperature_C;
      servo.torque_on = pair.second.torque_on;
      data.servos.push_back(servo);
    }

    parent_->servo_data_signal_(&data);
  }

  void CheckFaults(const HerkuleXBase::MemReadResponse& response) {
    if (response.reg48) {
      log_.warn((boost::format("ServoMonitor: %02X err=%s")
                 % response.servo % FaultToString(response)).str());
      servo_->ClearStatus(response.servo,
                          std::bind(&Impl::HandleClear, this,
                                    std::placeholders::_1));
    }

    if (!response.motor_on && expect_torque_on_) {
      log_.warn((boost::format("%02X has motor unexpectedly off!") %
                 response.servo).str());
    }
  }

  void HandleClear(base::ErrorCode ec) {
    FailIf(ec);
  }

  std::string FaultToString(const HerkuleXBase::MemReadResponse& response) {
    std::vector<std::string> errors;

#define CHECK(x) if (response.x) { errors.push_back(#x); }
    CHECK(exceeded_input_voltage_limit);
    CHECK(exceeded_allowed_pot_limit);
    CHECK(exceeded_temperature_limit);
    CHECK(invalid_packet);
    CHECK(overload_detected);
    CHECK(driver_fault_detected);
    CHECK(eep_reg_distorted);
    CHECK(checksum_error);
    CHECK(unknown_command);
    CHECK(exceeded_reg_range);
    CHECK(garbage_detected);
#undef CHECK

    std::ostringstream ostr;
    bool first = true;
    for (const auto item: errors) {
      if (!first) { ostr << "|"; }
      ostr << item;
      first = false;
    }

    return ostr.str();
  }

  ServoMonitor* const parent_;
  boost::asio::io_service& service_;
  HerkuleXServo* const servo_;

  base::LogRef log_ = base::GetLogInstance("ServoMonitor");

  base::DeadlineTimer timer_;

  struct Servo {
    boost::posix_time::ptime last_update;
    double voltage_V = 0;
    double temperature_C = 0;
    bool torque_on = false;

    boost::posix_time::ptime next_update;
    double parole_time_s = 0;
  };

  std::map<int, Servo> servo_data_;

  struct State {
    int last_servo = -1;

    enum Mode {
      kIdle,
      kTemperature,
    } mode = kIdle;

  } state_;

  bool outstanding_ = false;

  bool expect_torque_on_ = false;

  Parameters parameters_;
};

ServoMonitor::ServoMonitor(boost::asio::io_service& service,
                           HerkuleXServo* servo)
    : impl_(new Impl(this, service, servo)) {}

ServoMonitor::~ServoMonitor() {}

void ServoMonitor::AsyncStart(base::ErrorHandler handler) {
  impl_->SetupInitialServos();
  impl_->Start();
  impl_->service_.post(std::bind(handler, base::ErrorCode()));
}

ServoMonitor::Parameters* ServoMonitor::parameters() {
  return &impl_->parameters_;
}

void ServoMonitor::ExpectTorqueOn() {
  impl_->expect_torque_on_ = true;
}

void ServoMonitor::ExpectTorqueOff() {
  impl_->expect_torque_on_ = false;
}

std::vector<int> ServoMonitor::SplitServoIds(const std::string& input) {
  // Make a single query of a bunch of servo IDs so that the
  // ServoMonitor will know who to talk to.
  std::vector<std::string> fields;
  boost::split(fields, input, boost::is_any_of(","));
  std::vector<int> ids;
  for (const auto& field: fields) {
    if (field.empty()) { continue; }
    size_t pos = field.find_first_of('-');
    if (pos == std::string::npos) {
      ids.push_back(std::stoi(field));
    } else {
      int start = std::stoi(field.substr(0, pos));
      int end = std::stoi(field.substr(pos + 1));
      if (start < 0 || start > 254 ||
          end < 0 || end > 254) {
        throw base::SystemError::einval("invalid servo spec");
      }
      for (int i = start; i <= end; i++) { ids.push_back(i); }
    }
  }

  return ids;
}

}
}
