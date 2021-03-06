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

#include "leg_ik.h"

#include <boost/test/auto_unit_test.hpp>

namespace {
using namespace mjmech::base;
using namespace mjmech::mech;

const int kCoxaIdent = 0;
const int kFemurIdent = 1;
const int kTibiaIdent = 2;

struct Test {
  double x_mm;
  double y_mm;
  double z_mm;

  double expected_coxa_deg;
  double expected_femur_deg;
  double expected_tibia_deg;
};

LizardIK::Config MakeLizardConfig() {
  LizardIK::Config r;

  r.coxa.ident = kCoxaIdent;
  r.femur.ident = kFemurIdent;
  r.tibia.ident = kTibiaIdent;

  r.coxa.length_mm = 50;
  r.femur.length_mm = 40;
  r.tibia.length_mm = 30;
  r.coxa.min_deg = -90;
  r.coxa.idle_deg = 0;
  r.coxa.max_deg = 90;
  r.femur.min_deg = -90;
  r.femur.idle_deg = 0;
  r.femur.max_deg = 90;
  r.tibia.min_deg = -90;
  r.tibia.idle_deg = 0;
  r.tibia.max_deg = 90;

  return r;
}

template <typename T>
double GetAngle(T joints, int ident) {
  for (const auto& joint: joints.joints) {
    if (joint.ident == ident) { return joint.angle_deg; }
  }
  BOOST_CHECK(false);
  return 0.0;
}

template <typename T>
void CheckJoints(T joints, double coxa_deg, double femur_deg, double tibia_deg) {
  BOOST_CHECK_SMALL(std::abs(GetAngle(joints, kCoxaIdent) - coxa_deg), 1e-2);
  BOOST_CHECK_SMALL(std::abs(GetAngle(joints, kFemurIdent) - femur_deg), 1e-2);
  BOOST_CHECK_SMALL(std::abs(GetAngle(joints, kTibiaIdent) - tibia_deg), 1e-2);
}
}

BOOST_AUTO_TEST_CASE(TestLizard3Dof) {
  auto config = MakeLizardConfig();

  LizardIK ik(config);

  Test tests[] = {
    { 0,  90,    -30,    0,     0,    0 },
    { 0,  90,    -25,    0,     7.18, -6.58 },
    { 0,  90,    -35,    0,    -7.18, 7.78 },
    { 0,  95,    -30,    0,    -0.60, 10.20 },
    { 20, 87.75, -30,    12.84, 0,    0 },
    { 20, 87.75, -25,    12.84, 7.18, -6.58 },
  };

  for (const Test& test: tests) {
    Point3D point(test.x_mm, test.y_mm, test.z_mm);
    auto result = ik.Solve(point);
    CheckJoints(result, test.expected_coxa_deg, test.expected_femur_deg,
                test.expected_tibia_deg);
  }

  // Try adding some idle to coxa.
  Point3D point(20, 87.75, -25);
  config.coxa.idle_deg = 3.0;
  auto result = LizardIK(config).Solve(point);
  CheckJoints(result, 15.84, 7.18, -6.58);

  // And some idle to femur.
  config.femur.idle_deg = 4.0;
  result = LizardIK(config).Solve(point);
  CheckJoints(result, 15.84, 11.18, -6.58);

  // And some idle to tibia.
  config.tibia.idle_deg = 5.0;
  result = LizardIK(config).Solve(point);
  CheckJoints(result, 15.84, 11.18, -1.58);

  // Try setting the max coxa low enough that we should get invalid.
  config.coxa.max_deg = 15.0;
  result = LizardIK(config).Solve(point);
  BOOST_CHECK(!result.Valid());

  config.coxa.max_deg = 90.0;
  result = LizardIK(config).Solve(point);
  BOOST_CHECK(result.Valid());

  // And set the tibia max deg low enough to get invalid.
  config.femur.max_deg = 10.0;
  result = LizardIK(config).Solve(point);
  BOOST_CHECK(!result.Valid());
}

namespace {
MammalIK::Config MakeMammalConfig() {
  MammalIK::Config r;

  r.shoulder.ident = kCoxaIdent;
  r.femur.ident = kFemurIdent;
  r.tibia.ident = kTibiaIdent;

  r.femur_attachment_mm.x = 0;
  r.femur_attachment_mm.y = 30;
  r.femur_attachment_mm.z = -40;

  r.shoulder.min_deg = -90.0;
  r.shoulder.idle_deg = 0.0;
  r.shoulder.max_deg = 90.0;

  r.femur.min_deg = -170.0;
  r.femur.idle_deg = 0.0;
  r.femur.max_deg = 170.0;
  r.femur.length_mm = 100.0;

  r.tibia.min_deg = -170.0;
  r.tibia.idle_deg = 0.0;
  r.tibia.max_deg = 170.0;
  r.tibia.length_mm = 110.0;

  return r;
}

void CheckVectorsClose(const Point3D& p1,
                       const Point3D& p2) {
  BOOST_CHECK_SMALL(p1.x - p2.x, 1e-3);
  BOOST_CHECK_SMALL(p1.y - p2.y, 1e-3);
  BOOST_CHECK_SMALL(p1.z - p2.z, 1e-3);
}

void TestMammalForward(const JointAngles& joints,
                       const MammalIK::Config& config,
                       const Point3D& expected_point) {
  auto result = MammalIK(config).Forward(joints);

  CheckVectorsClose(result.end, expected_point);
}
}

BOOST_AUTO_TEST_CASE(TestMammal3DoF) {
  auto config = MakeMammalConfig();

  Test tests[] = {
    {   0, 30, -250,    0.00,   0,   0 },
    {   0, 30, -240,    0.00,  -18.65, 35.55 },
    {   0, 30, -230,    0.00,  -26.52, 50.48 },
    {   0, 30, -210,    0.00,  -37.97, 72.00 },
    {   0, 30, -190,    0.00,  -47.16, 88.96 },
    {   0, 30, -150,    0.00,  -62.96, 117.04 },
    {   0, 30, -90,     0.00,  -87.71, 152.98 },
    {  20, 30, -190,    0.00,  -54.18, 87.92 },
    { -20, 30, -190,    0.00,  -39.00, 87.92 },
    {   0, 40, -190,    3.00,  -46.37, 87.52 },
    {   0, 20, -190,   -3.02,  -47.72, 89.98 },
  };

  for (const Test& test: tests) {
    auto run_test = [](const MammalIK::Config& config,
                       const Test& test) {
      MammalIK ik(config);
      Point3D point(test.x_mm, test.y_mm, test.z_mm);
      auto result = ik.Solve(point);
      CheckJoints(result, test.expected_coxa_deg, test.expected_femur_deg,
                  test.expected_tibia_deg);
      TestMammalForward(result, config, point);
    };

    run_test(config, test);

    {
      Test sign_test = test;
      MammalIK::Config sign_config = config;
      sign_config.shoulder.sign *= -1;
      sign_test.expected_coxa_deg *= -1;

      run_test(sign_config, sign_test);
    }
  }
}
