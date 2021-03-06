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

#include "context_full.h"

#include "linux_i2c_generator.h"

namespace mjmech {
namespace base {

Context::Context()
    : telemetry_log(new TelemetryLog),
      remote_debug(new TelemetryRemoteDebugServer(service)),
      telemetry_registry(new TelemetryRegistry(
                             telemetry_log.get(), remote_debug.get())),
      factory(new ConcreteStreamFactory(service)),
      i2c_factory(new I2CFactory(service))
{
  i2c_factory->Register(
      std::unique_ptr<I2CFactory::Generator>(
          new LinuxI2CGenerator(service)));
}

Context::~Context() {}

}
}
