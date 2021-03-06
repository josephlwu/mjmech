// Copyright 2014 Josh Pieper, jjp@pobox.com.  All rights reserved.
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

#include <functional>

#include "ukf_filter.h"
#include "quaternion.h"

typedef double Float;

namespace imu {
Float DegToRad(Float val) {
  return val / static_cast<Float>(180.0) * static_cast<Float>(M_PI);
}

class AttitudeEstimator {
 public:
  typedef UkfFilter<Float, 7> Filter;

  AttitudeEstimator(Float process_noise_gyro,
                    Float process_noise_bias,
                    Float measurement_noise_accel,
                    Float measurement_noise_stationary,
                    Float initial_noise_attitude,
                    Float initial_noise_bias)
      : initialized_(false),
        filter_(
          (Filter::State() <<
           1.0, 0.0, 0.0, 0.0,
           0.0, 0.0, 0.0).finished(),
          Eigen::DiagonalMatrix<Float, 7, 7>(
              (Filter::State() <<
               initial_noise_attitude,
               initial_noise_attitude,
               initial_noise_attitude,
               initial_noise_attitude,
               initial_noise_bias,
               initial_noise_bias,
               initial_noise_bias).finished()),
          Eigen::DiagonalMatrix<Float, 7, 7>(
              (Filter::State() <<
               process_noise_gyro,
               process_noise_gyro,
               process_noise_gyro,
               process_noise_gyro,
               process_noise_bias,
               process_noise_bias,
               process_noise_bias).finished())),
        measurement_noise_accel_(measurement_noise_accel),
        measurement_noise_stationary_(measurement_noise_stationary),
        initial_bias_uncertainty_(initial_noise_bias) {
  }

  std::vector<std::string> state_names() const {
    return { "w", "x", "y", "z", "gx", "gy", "gz" };
  }

  void SetInitialGyroBias(
      Float yaw_rps, Float pitch_rps, Float roll_rps) {
    filter_.state()(4) = yaw_rps;
    filter_.state()(5) = pitch_rps;
    filter_.state()(6) = roll_rps;
  }

  void SetInitialAccel(Float x_g, Float y_g, Float z_g) {
    Float norm = std::sqrt(x_g * x_g + y_g * y_g + z_g * z_g);
    x_g /= norm;
    y_g /= norm;
    z_g /= norm;

    Quaternion<Float> a = AccelToOrientation(x_g, y_g, z_g);
    filter_.state()(0) = a.w();
    filter_.state()(1) = a.x();
    filter_.state()(2) = a.y();
    filter_.state()(3) = a.z();

    initialized_ = true;
  }

  Filter::Covariance covariance() const {
    return filter_.covariance();
  }

  std::vector<Float> covariance_diag() const {
    std::vector<Float> result;
    for (int i = 0; i < 7; i++) {
      result.push_back(filter_.covariance()(i, i));
    }
    return result;
  }

  Float pitch_error(Float pitch) const {
    return (attitude() *
            Quaternion<Float>::FromEuler(0., pitch, 0).
            conjugated()).euler().pitch_rad;
  }

  Float yaw_rad() const { return attitude().euler().yaw_rad; }
  Float pitch_rad() const { return attitude().euler().pitch_rad; }
  Float roll_rad() const { return attitude().euler().roll_rad; }

  Float yaw_rps() const {
    return current_gyro_.yaw_rps + filter_.state()(4);
  }

  Float pitch_rps() const {
    return current_gyro_.pitch_rps + filter_.state()(5);
  }

  Float roll_rps() const {
    return current_gyro_.roll_rps + filter_.state()(6);
  }

  Float gyro_bias_rps() const { return filter_.state()(4, 0); }

  Quaternion<Float> attitude() const {
    return Quaternion<Float>(filter_.state()(0),
                             filter_.state()(1),
                             filter_.state()(2),
                             filter_.state()(3));
  }

  Filter::State ProcessFunction(
      const Filter::State& state, Float dt_s) const {
    Filter::State result = state;

    Quaternion<Float> this_attitude = Quaternion<Float>(
        result(0), result(1), result(2), result(3)).normalized();
    Quaternion<Float> delta;

    Quaternion<Float> advanced = Quaternion<Float>::IntegrateRotationRate(
        current_gyro_.roll_rps + result(6),
        current_gyro_.pitch_rps + result(5),
        current_gyro_.yaw_rps + result(4),
        dt_s);
    delta = delta * advanced;

    Quaternion<Float> next_attitude = (this_attitude * delta).normalized();
    result(0) = next_attitude.w();
    result(1) = next_attitude.x();
    result(2) = next_attitude.y();
    result(3) = next_attitude.z();
    return result;
  }

  static Eigen::Matrix<Float, 3, 1> OrientationToAccel(
      const Quaternion<Float>& attitude) {
    Quaternion<Float>::Vector3D gravity(0., 0., 1.);
    Quaternion<Float>::Vector3D expected =
        attitude.conjugated().Rotate(gravity);
    return expected;
  }

  static Eigen::Matrix<Float, 3, 1> MeasureAccel(
      const Filter::State& s) {
    return OrientationToAccel(
        Quaternion<Float>(s(0), s(1), s(2), s(3)).normalized());
  }

  Eigen::Matrix<Float, 1, 1> MeasureRotation(
      const Filter::State& s) const {
    Quaternion<Float>::Vector3D rotation(
        current_gyro_.pitch_rps + s(5),
        current_gyro_.roll_rps + s(6),
        current_gyro_.yaw_rps + s(4));
    return (Eigen::Matrix<Float, 1, 1>() << rotation.norm()).finished();
  }

  static Quaternion<Float> AccelToOrientation(
      Float x, Float y, Float z) {
    Float roll = std::atan2(-x, z);
    Float pitch = std::atan2(y, std::sqrt(x * x + z * z));

    return Quaternion<Float>::FromEuler(roll, pitch, 0.0);
  }

  void ProcessStationary() {
    using namespace std::placeholders;
    filter_.UpdateMeasurement(
        std::bind(&AttitudeEstimator::MeasureRotation, this, _1),
        (Eigen::Matrix<Float, 1, 1>() << 0.0).finished(),
        (Eigen::Matrix<Float, 1, 1>() <<
         measurement_noise_stationary_).finished());
  }

  void ProcessMeasurement(
      Float yaw_rps, Float pitch_rps, Float roll_rps,
      Float x_g, Float y_g, Float z_g) {
    current_gyro_ = Gyro(yaw_rps, pitch_rps, roll_rps);

    Float norm = std::sqrt(x_g * x_g + y_g * y_g + z_g * z_g);

    x_g /= norm;
    y_g /= norm;
    z_g /= norm;

    if (!initialized_) {
      initialized_ = true;
      Quaternion<Float> start = AccelToOrientation(x_g, y_g, z_g);
      filter_.state()(0) = start.w();
      filter_.state()(1) = start.x();
      filter_.state()(2) = start.y();
      filter_.state()(3) = start.z();
    }

    using namespace std::placeholders;
    filter_.UpdateState(
        0.01, std::bind(&AttitudeEstimator::ProcessFunction, this, _1, _2));
    filter_.UpdateMeasurement(
        MeasureAccel,
        (Eigen::Matrix<Float, 3, 1>() << x_g, y_g, z_g).finished(),
        (Eigen::DiagonalMatrix<Float, 3, 3>(
            (Eigen::Matrix<Float, 3, 1>() <<
             measurement_noise_accel_,
             measurement_noise_accel_,
             measurement_noise_accel_).finished())));
  }

 private:
  bool initialized_;
  Filter filter_;
  Float measurement_noise_accel_;
  Float measurement_noise_stationary_;
  Float initial_bias_uncertainty_;

  struct Gyro {
    Float yaw_rps;
    Float pitch_rps;
    Float roll_rps;

    Gyro(Float yaw_rps, Float pitch_rps, Float roll_rps)
        : yaw_rps(yaw_rps), pitch_rps(pitch_rps), roll_rps(roll_rps) {}
    Gyro() : yaw_rps(0.), pitch_rps(0.), roll_rps(0.) {}
  };

  Gyro current_gyro_;
};


class PitchEstimator {
 public:
  typedef UkfFilter<Float, 2> PitchFilter;

  PitchEstimator(Float process_noise_gyro,
                 Float process_noise_bias,
                 Float measurement_noise_accel,
                 Float measurement_noise_stationary,
                 Float initial_noise_attitude,
                 Float initial_noise_bias)
      : process_noise_gyro_(process_noise_gyro),
        process_noise_bias_(process_noise_bias),
        measurement_noise_accel_(measurement_noise_accel),
        initial_bias_uncertainty_(initial_noise_bias),
        pitch_filter_(
            PitchFilter::State::Zero(),
            (PitchFilter::Covariance() <<
             initial_noise_attitude, 0.0,
             0.0, initial_noise_bias).finished(),
            (PitchFilter::Covariance() <<
             process_noise_gyro, 0.0,
             0.0, process_noise_bias).finished()),
        current_gyro_(0.0) {
  }

  std::vector<std::string> state_names() const {
    return { "pitch", "gyro_bias" };
  }

  void SetInitialGyroBias(
      Float yaw_rps, Float pitch_rps, Float roll_rps) {
    pitch_filter_.state()(1) = pitch_rps;
  }

  void SetInitialAccel(Float x_g, Float y_g, Float z_g) {
    Float norm = std::sqrt(x_g * x_g + y_g * y_g + z_g * z_g);
    x_g /= norm;
    y_g /= norm;
    z_g /= norm;

    pitch_filter_.state()(0) =
        std::atan2(y_g, std::sqrt(x_g * x_g + z_g * z_g));
  }

  PitchFilter::Covariance covariance() const {
    return pitch_filter_.covariance();
  }

  std::vector<Float> covariance_diag() const {
    std::vector<Float> result;
    for (int i = 0; i < 2; i++) {
      result.push_back(pitch_filter_.covariance()(i, i));
    }
    return result;
  }

  Float pitch_error(double pitch) {
    return pitch - pitch_rad();
  }

  Float yaw_rad() const { return 0.; }
  Float pitch_rad() const { return pitch_filter_.state()(0); }
  Float roll_rad() const { return 0.; }

  Float yaw_rps() const { return 0.; }
  Float pitch_rps() const {
    return current_gyro_ + pitch_filter_.state()(1);
  }
  Float roll_rps() const { return 0.; }
  Float gyro_bias_rps() const { return pitch_filter_.state()(1); }

  PitchFilter::State ProcessFunction(
      const PitchFilter::State& state, Float dt_s) const {
    PitchFilter::State result = state;

    Float delta = 0.0;
    delta += (current_gyro_ + result[1]) * dt_s;

    result[0] += delta;
    return result;
  }

  static Eigen::Matrix<Float, 2, 1> OrientationToAccel(
      Float pitch_rad) {
    Quaternion<Float>::Vector3D gravity(0., 0., 1.);
    Quaternion<Float>::Vector3D expected =
        Quaternion<Float>::FromEuler(
            0., pitch_rad, 0.).conjugated().Rotate(gravity);
    return (Eigen::Matrix<Float, 2, 1>() <<
            expected(1), expected(2)).finished();
  }

  static Eigen::Matrix<Float, 2, 1> MeasureAccel(
      const PitchFilter::State& s) {
    return OrientationToAccel(s(0));
  }

  void ProcessStationary() {
  }

  void ProcessMeasurement(
      Float gyro_yaw_rps, Float gyro_pitch_rps, Float gyro_roll_rps,
      Float x_g, Float y_g, Float z_g) {
    current_gyro_ = gyro_pitch_rps;

    Float norm = std::sqrt(y_g * y_g + z_g * z_g);

    y_g /= norm;
    z_g /= norm;

    // TODO jpieper: If this is the very first time, we should just
    // set the pitch to be the exact value as measured by the
    // accelerometers.

    using namespace std::placeholders;
    pitch_filter_.UpdateState(
        0.01, std::bind(&PitchEstimator::ProcessFunction, this, _1, _2));
    pitch_filter_.UpdateMeasurement(
        MeasureAccel,
        (Eigen::Matrix<Float, 2, 1>() << y_g, z_g).finished(),
        (Eigen::Matrix<Float, 2, 2>() <<
         measurement_noise_accel_, static_cast<Float>(0.0),
         static_cast<Float>(0.0), measurement_noise_accel_).finished());
  }

 private:
  Float process_noise_gyro_;
  Float process_noise_bias_;
  Float measurement_noise_accel_;
  Float initial_bias_uncertainty_;

  PitchFilter pitch_filter_;
  Float current_gyro_;
};
}
