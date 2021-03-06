// Copyright 2014-2016 Josh Pieper, jjp@pobox.com.  All rights reserved.
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

#pragma once

#include <memory>

#include <boost/noncopyable.hpp>
#include <boost/asio/io_service.hpp>

namespace mjmech {
namespace base {

class ConcreteStreamFactory;
class I2CFactory;
class TelemetryLog;
class TelemetryRemoteDebugServer;
class TelemetryRegistry;

struct Context : boost::noncopyable {
  Context();
  ~Context();

  boost::asio::io_service service;
  std::unique_ptr<TelemetryLog> telemetry_log;
  std::unique_ptr<TelemetryRemoteDebugServer> remote_debug;
  std::unique_ptr<TelemetryRegistry> telemetry_registry;
  std::unique_ptr<ConcreteStreamFactory> factory;
  std::unique_ptr<I2CFactory> i2c_factory;
};

}
}
