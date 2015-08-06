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

#pragma once

#include "common.h"
#include "leg_ik.h"
#include "tf.h"
#include "visitor.h"

namespace legtool {
struct Leg {
  enum class Mode {
    kStance,
      kSwing,
      kUnknown,
  };

  struct Config {
    Point3D mount_mm;
    Point3D idle_mm;
    std::shared_ptr<IKSolver> leg_ik;

    template <typename Archive>
    void Serialize(Archive* a) {
      a->Visit(LT_NVP(mount_mm));
      a->Visit(LT_NVP(idle_mm));
    }
  };

  struct Result {
    Point3D point;
    Mode mode = Mode::kUnknown;
  };

  struct State {
    Point3D point;
    Mode mode = Mode::kUnknown;
    IKSolver* leg_ik = nullptr;
    Frame* frame = nullptr;
    Frame shoulder_frame;
  };
};

struct MechanicalConfig {
  std::vector<Leg::Config> leg_config;
  Point3D body_cog_mm;

  template <typename Archive>
  void Serialize(Archive* a) {
    a->Visit(LT_NVP(leg_config));
    a->Visit(LT_NVP(body_cog_mm));
  }
};

struct Command {
  double translate_x_mm_s = 0;
  double translate_y_mm_s = 0;
  double rotate_deg_s = 0;
  double body_x_mm = 0;
  double body_y_mm = 0;
  double body_z_mm = 0;
  double body_pitch_deg = 0;
  double body_roll_deg = 0;
  double body_yaw_deg = 0;
  double lift_height_percent = 0;

  template <typename Archive>
  void Serialize(Archive* a) {
    a->Visit(LT_NVP(translate_x_mm_s));
    a->Visit(LT_NVP(translate_y_mm_s));
    a->Visit(LT_NVP(rotate_deg_s));
    a->Visit(LT_NVP(body_x_mm));
    a->Visit(LT_NVP(body_y_mm));
    a->Visit(LT_NVP(body_z_mm));
    a->Visit(LT_NVP(body_pitch_deg));
    a->Visit(LT_NVP(body_roll_deg));
    a->Visit(LT_NVP(body_yaw_deg));
    a->Visit(LT_NVP(lift_height_percent));
  }
};

struct CommonState {
  Frame world_frame;
  Frame robot_frame;
  Frame body_frame;
  Frame cog_frame;

  CommonState() :
      world_frame(),
      robot_frame({}, {}, &world_frame),
      body_frame({}, {}, &robot_frame),
      cog_frame({}, {}, &body_frame) {}

  CommonState(const CommonState& rhs) : CommonState() {
    world_frame.transform = rhs.world_frame.transform;
    robot_frame.transform = rhs.robot_frame.transform;
    body_frame.transform = rhs.body_frame.transform;
    cog_frame.transform = rhs.cog_frame.transform;
  };
};

struct JointCommand {
  struct Joint {
    int servo_number = 0;
    double angle_deg = 0.0;

    Joint() {}
    Joint(int servo_number, double angle_deg)
        : servo_number(servo_number), angle_deg(angle_deg) {}
  };

  std::vector<Joint> joints;
};

class Gait : boost::noncopyable {
 public:
  virtual ~Gait() {};
  virtual JointCommand AdvancePhase(double delpha_phase) = 0;
  virtual JointCommand AdvanceTime(double delta_s) = 0;
  enum Result {
    kValid,
    kNotSupported,
  };
  virtual Result SetCommand(const Command&) = 0;
};
}
