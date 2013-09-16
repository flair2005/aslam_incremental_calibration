/******************************************************************************
 * Copyright (C) 2013 by Jerome Maye                                          *
 * jerome.maye@gmail.com                                                      *
 *                                                                            *
 * This program is free software; you can redistribute it and/or modify       *
 * it under the terms of the Lesser GNU General Public License as published by*
 * the Free Software Foundation; either version 3 of the License, or          *
 * (at your option) any later version.                                        *
 *                                                                            *
 * This program is distributed in the hope that it will be useful,            *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of             *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the              *
 * Lesser GNU General Public License for more details.                        *
 *                                                                            *
 * You should have received a copy of the Lesser GNU General Public License   *
 * along with this program. If not, see <http://www.gnu.org/licenses/>.       *
 ******************************************************************************/

/** \file calibrateCarOdometryNewSplineBatch.cpp
    \brief This file calibrates the car parameters from a ROS bag file.
  */

#include <iostream>
#include <vector>
#include <string>

#include <boost/make_shared.hpp>

#include <rosbag/bag.h>
#include <rosbag/view.h>
#include <rosbag/message_instance.h>

#include <sm/kinematics/EulerAnglesYawPitchRoll.hpp>
#include <sm/kinematics/rotations.hpp>
#include <sm/kinematics/quaternion_algebra.hpp>

#include <sm/timing/TimestampCorrector.hpp>
#include <sm/timing/NsecTimeUtilities.hpp>

#include <aslam/backend/EuclideanPoint.hpp>
#include <aslam/backend/RotationQuaternion.hpp>
#include <aslam/backend/OptimizationProblem.hpp>
#include <aslam/backend/Optimizer2Options.hpp>
#include <aslam/backend/SparseQrLinearSystemSolver.hpp>
#include <aslam/backend/SparseQRLinearSolverOptions.h>
#include <aslam/backend/Optimizer2.hpp>
#include <aslam/backend/EuclideanExpression.hpp>
#include <aslam/backend/RotationExpression.hpp>
#include <aslam/backend/Vector2RotationQuaternionExpressionAdapter.hpp>
#include <aslam/backend/GaussNewtonTrustRegionPolicy.hpp>

#include <aslam/splines/OPTBSpline.hpp>
#include <aslam/splines/OPTUnitQuaternionBSpline.hpp>

#include <bsplines/BSplineFitter.hpp>
#include <bsplines/EuclideanBSpline.hpp>
#include <bsplines/UnitQuaternionBSpline.hpp>
#include <bsplines/SimpleTypeTimePolicy.hpp>

#include <poslv/VehicleNavigationSolutionMsg.h>
#include <poslv/VehicleNavigationPerformanceMsg.h>
#include <poslv/TimeTaggedDMIDataMsg.h>

#include <can_prius/FrontWheelsSpeedMsg.h>
#include <can_prius/RearWheelsSpeedMsg.h>
#include <can_prius/Steering1Msg.h>

#include <libposlv/geo-tools/Geo.h>

#include <robot-odometry/generic/DifferentialOdometry.h>

#include <aslam/calibration/data-structures/VectorDesignVariable.h>
#include <aslam/calibration/algorithms/matrixOperations.h>

#include "aslam/calibration/car/MeasurementsContainer.h"
#include "aslam/calibration/car/ApplanixNavigationMeasurement.h"
#include "aslam/calibration/car/WheelsSpeedMeasurement.h"
#include "aslam/calibration/car/SteeringMeasurement.h"
#include "aslam/calibration/car/ApplanixDMIMeasurement.h"
#include "aslam/calibration/car/ErrorTermPose.h"
#include "aslam/calibration/car/ErrorTermFws.h"
#include "aslam/calibration/car/ErrorTermRws.h"
#include "aslam/calibration/car/ErrorTermSteering.h"
#include "aslam/calibration/car/ErrorTermDMI.h"
#include "aslam/calibration/car/utils.h"

using namespace aslam::calibration;
using namespace sm::kinematics;
using namespace sm::timing;
using namespace bsplines;
using namespace aslam::splines;
using namespace aslam::backend;

struct NsecTimePolicy :
  public SimpleTypeTimePolicy<NsecTime> {
  inline static NsecTime getOne() {
    return NsecTime(1e9);
  }
};

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << "Usage: " << argv[0] << " <ros_bag_file>" << std::endl;
    return -1;
  }
  rosbag::Bag bag(argv[1]);
  std::vector<std::string> topics;
  topics.push_back(std::string("/can_prius/front_wheels_speed"));
  topics.push_back(std::string("/can_prius/rear_wheels_speed"));
  topics.push_back(std::string("/can_prius/steering1"));
  topics.push_back(std::string("/poslv/vehicle_navigation_solution"));
  topics.push_back(std::string("/poslv/vehicle_navigation_performance"));
  topics.push_back(std::string("/poslv/time_tagged_dmi_data"));
  rosbag::View view(bag, rosbag::TopicQuery(topics));
  std::cout << "Processing BAG file..." << std::endl;
  poslv::VehicleNavigationPerformanceMsgConstPtr lastVnp;
  bool firstVNS = true;
  double latRef = 0;
  double longRef = 0;
  double altRef = 0;
  size_t viewCounter = 0;
  MeasurementsContainer<ApplanixNavigationMeasurement>::Type
    navigationMeasurements;
  MeasurementsContainer<ApplanixDMIMeasurement>::Type encoderMeasurements;
  MeasurementsContainer<WheelsSpeedMeasurement>::Type
    frontWheelsSpeedMeasurements;
  MeasurementsContainer<WheelsSpeedMeasurement>::Type
    rearWheelsSpeedMeasurements;
  MeasurementsContainer<SteeringMeasurement>::Type steeringMeasurements;
  TimestampCorrector<double> timestampCorrector1;
  TimestampCorrector<double> timestampCorrector2;
  for (auto it = view.begin(); it != view.end(); ++it) {
    std::cout << std::fixed << std::setw(3)
      << viewCounter++ / (double)view.size() * 100 << " %" << '\r';
    if (it->isType<poslv::VehicleNavigationPerformanceMsg>()) {
      poslv::VehicleNavigationPerformanceMsgConstPtr vnp(
        it->instantiate<poslv::VehicleNavigationPerformanceMsg>());
      lastVnp = vnp;
    }
    if (it->isType<poslv::VehicleNavigationSolutionMsg>()) {
      if (!lastVnp)
        continue;
      poslv::VehicleNavigationSolutionMsgConstPtr vns(
        it->instantiate<poslv::VehicleNavigationSolutionMsg>());
      if (firstVNS) {
        latRef = vns->latitude;
        longRef = vns->longitude;
        altRef = vns->altitude;
        firstVNS = false;
      }
      double x_ecef, y_ecef, z_ecef;
      Geo::wgs84ToEcef(vns->latitude, vns->longitude, vns->altitude, x_ecef,
        y_ecef, z_ecef);
      double x_enu, y_enu, z_enu;
      Geo::ecefToEnu(x_ecef, y_ecef, z_ecef, latRef, longRef, altRef, x_enu,
        y_enu, z_enu);
      ApplanixNavigationMeasurement data;
      data.x = x_enu;
      data.y = y_enu;
      data.z = z_enu;
      data.yaw = angleMod(deg2rad(-vns->heading) + M_PI / 2);
      data.pitch = deg2rad(-vns->pitch);
      data.roll = deg2rad(vns->roll);
      Eigen::Vector3d linearVelocity =
        Geo::R_ENU_NED::getInstance().getMatrix() * Eigen::Vector3d(
        vns->northVelocity, vns->eastVelocity, vns->downVelocity);
      data.v_x = linearVelocity(0);
      data.v_y = linearVelocity(1);
      data.v_z = linearVelocity(2);
      data.om_x = deg2rad(vns->angularRateLong);
      data.om_y = -deg2rad(vns->angularRateTrans);
      data.om_z = -deg2rad(vns->angularRateDown);
      data.a_x = vns->accLong;
      data.a_y = -vns->accTrans;
      data.a_z = -vns->accDown;
      data.v = vns->speed;
      data.x_sigma2 = lastVnp->eastPositionRMSError *
        lastVnp->eastPositionRMSError;
      data.y_sigma2 = lastVnp->northPositionRMSError *
        lastVnp->northPositionRMSError;
      data.z_sigma2 = lastVnp->downPositionRMSError *
        lastVnp->downPositionRMSError;
      data.roll_sigma2 = deg2rad(lastVnp->rollRMSError) *
        deg2rad(lastVnp->rollRMSError);
      data.pitch_sigma2 = deg2rad(lastVnp->pitchRMSError) *
        deg2rad(lastVnp->pitchRMSError);
      data.yaw_sigma2 = deg2rad(lastVnp->headingRMSError) *
        deg2rad(lastVnp->headingRMSError);
      data.v_x_sigma2 = lastVnp->eastVelocityRMSError *
        lastVnp->eastVelocityRMSError;
      data.v_y_sigma2 = lastVnp->northVelocityRMSError *
        lastVnp->northVelocityRMSError;
      data.v_z_sigma2 = lastVnp->downVelocityRMSError *
        lastVnp->downVelocityRMSError;
      navigationMeasurements.push_back(
        std::make_pair(round(timestampCorrector1.correctTimestamp(
        secToNsec(vns->timeDistance.time1), vns->header.stamp.toNSec())),
        data));
    }
    if (it->getTopic() == "/can_prius/front_wheels_speed") {
      can_prius::FrontWheelsSpeedMsgConstPtr fws(
        it->instantiate<can_prius::FrontWheelsSpeedMsg>());
      WheelsSpeedMeasurement data;
      data.left = fws->Left;
      data.right = fws->Right;
      frontWheelsSpeedMeasurements.push_back(std::make_pair(
        fws->header.stamp.toNSec(), data));
      
    }
    if (it->getTopic() == "/can_prius/rear_wheels_speed") {
      can_prius::RearWheelsSpeedMsgConstPtr rws(
        it->instantiate<can_prius::RearWheelsSpeedMsg>());
      WheelsSpeedMeasurement data;
      data.left = rws->Left;
      data.right = rws->Right;
      rearWheelsSpeedMeasurements.push_back(std::make_pair(
        rws->header.stamp.toNSec(), data));
    }
    if (it->isType<can_prius::Steering1Msg>()) {
      can_prius::Steering1MsgConstPtr st(
        it->instantiate<can_prius::Steering1Msg>());
      SteeringMeasurement data;
      data.value = st->value;
      steeringMeasurements.push_back(std::make_pair(st->header.stamp.toNSec(),
        data));
    }
    if (it->isType<poslv::TimeTaggedDMIDataMsg>()) {
      poslv::TimeTaggedDMIDataMsgConstPtr dmi(
        it->instantiate<poslv::TimeTaggedDMIDataMsg>());
      ApplanixDMIMeasurement data;
      data.signedDistanceTraveled = dmi->signedDistanceTraveled;
      data.unsignedDistanceTraveled = dmi->unsignedDistanceTraveled;
      encoderMeasurements.push_back(
        std::make_pair(round(timestampCorrector2.correctTimestamp(
        secToNsec(dmi->timeDistance.time1), dmi->header.stamp.toNSec())),
        data));
    }
  }

  std::cout << "Building spline..." << std::endl;
  const size_t numMeasurements = navigationMeasurements.size();
  std::vector<NsecTime> timestamps;
  timestamps.reserve(numMeasurements);
  std::vector<Eigen::Vector3d> transPoses;
  transPoses.reserve(numMeasurements);
  std::vector<Eigen::Vector4d> rotPoses;
  rotPoses.reserve(numMeasurements);
  auto ypr = boost::make_shared<EulerAnglesYawPitchRoll>();
  for (auto it = navigationMeasurements.cbegin();
      it != navigationMeasurements.cend(); ++it) {
    Eigen::Vector4d quat = r2quat(
      ypr->parametersToRotationMatrix(Eigen::Vector3d(it->second.yaw,
      it->second.pitch, it->second.roll)));
    if (!rotPoses.empty()) {
      const Eigen::Vector4d lastRotPose = rotPoses.back();
      quat = bestQuat(lastRotPose, quat);
    }
    timestamps.push_back(it->first);
    rotPoses.push_back(quat);
    transPoses.push_back(Eigen::Vector3d(it->second.x, it->second.y,
      it->second.z));
  }
  const double elapsedTime = (timestamps[numMeasurements - 1] - timestamps[0]) /
    (double)NsecTimePolicy::getOne();
  const int measPerSec = std::round(numMeasurements / elapsedTime);
  int numSegments;
  const double lambda = 0;
  const int measPerSecDesired = 5;
  if (measPerSec > measPerSecDesired)
    numSegments = std::ceil(measPerSecDesired * elapsedTime);
  else
    numSegments = numMeasurements;
  const int transSplineOrder = 4;
  const int rotSplineOrder = 4;
  OPTBSpline<EuclideanBSpline<Eigen::Dynamic, 3, NsecTimePolicy>::CONF>::BSpline
    translationSpline(EuclideanBSpline<Eigen::Dynamic, 3, NsecTimePolicy>::CONF(
    EuclideanBSpline<Eigen::Dynamic, 3, NsecTimePolicy>::CONF::ManifoldConf(3),
    transSplineOrder));
  BSplineFitter<OPTBSpline<EuclideanBSpline<Eigen::Dynamic, 3, NsecTimePolicy>::
    CONF>::BSpline>::initUniformSpline(translationSpline, timestamps,
    transPoses, numSegments, lambda);
  OPTBSpline<UnitQuaternionBSpline<Eigen::Dynamic, NsecTimePolicy>::CONF>::
    BSpline rotationSpline(UnitQuaternionBSpline<Eigen::Dynamic, NsecTimePolicy>
    ::CONF(UnitQuaternionBSpline<Eigen::Dynamic, NsecTimePolicy>::CONF::
    ManifoldConf(), rotSplineOrder));
  BSplineFitter<OPTBSpline<UnitQuaternionBSpline<Eigen::Dynamic, NsecTimePolicy>
    ::CONF>::BSpline>::initUniformSpline(rotationSpline, timestamps, rotPoses,
    numSegments, lambda);

  std::cout << "Outputting spline data before optimization..." << std::endl;
  std::ofstream applanixSplineFile("applanix-spline.txt");
  for (auto it = timestamps.cbegin(); it != timestamps.cend(); ++it) {
    auto translationExpressionFactory =
      translationSpline.getExpressionFactoryAt<2>(*it);
    auto rotationExpressionFactory =
      rotationSpline.getExpressionFactoryAt<1>(*it);
    Eigen::Matrix3d C_wi = Vector2RotationQuaternionExpressionAdapter::adapt(
      rotationExpressionFactory.getValueExpression()).toRotationMatrix();
    applanixSplineFile << std::fixed << std::setprecision(18)
      << *it << " "
      << translationExpressionFactory.getValueExpression().toValue().
        transpose() << " "
      << ypr->rotationMatrixToParameters(C_wi).transpose() << " "
      << translationExpressionFactory.getValueExpression(1).toValue().
        transpose() << " "
      << -(C_wi.transpose() *
        rotationExpressionFactory.getAngularVelocityExpression().toValue()).
        transpose() << " "
      << (C_wi.transpose() *
        translationExpressionFactory.getValueExpression(2).toValue()).
        transpose()
      << std::endl;
  }

  std::cout << "Building optimization problem..." << std::endl;
  auto problem = boost::make_shared<OptimizationProblem>();
  for (size_t i = 0; i < translationSpline.numDesignVariables(); ++i) {
//    translationSpline.designVariable(i)->setActive(true);
    problem->addDesignVariable(translationSpline.designVariable(i), false);
  }
  for (size_t i = 0; i < rotationSpline.numDesignVariables(); ++i) {
//    rotationSpline.designVariable(i)->setActive(true);
    problem->addDesignVariable(rotationSpline.designVariable(i), false);
  }
  for (auto it = navigationMeasurements.cbegin();
      it != navigationMeasurements.cend(); ++it) {
    ErrorTermPose::Input xm;
    xm.head<3>() = Eigen::Vector3d(it->second.x, it->second.y,
      it->second.z);
    xm.tail<3>() = Eigen::Vector3d(it->second.yaw, it->second.pitch,
      it->second.roll);
    ErrorTermPose::Covariance Q = ErrorTermPose::Covariance::Zero();
    Q(0, 0) = it->second.x_sigma2;
    Q(1, 1) = it->second.y_sigma2;
    Q(2, 2) = it->second.z_sigma2;
    Q(3, 3) = it->second.yaw_sigma2;
    Q(4, 4) = it->second.pitch_sigma2;
    Q(5, 5) = it->second.roll_sigma2;
    auto translationExpressionFactory =
      translationSpline.getExpressionFactoryAt<0>(it->first);
    auto rotationExpressionFactory =
      rotationSpline.getExpressionFactoryAt<0>(it->first);
    auto e_pose = boost::make_shared<ErrorTermPose>(
      TransformationExpression(
      Vector2RotationQuaternionExpressionAdapter::adapt(
      rotationExpressionFactory.getValueExpression()),
      translationExpressionFactory.getValueExpression()),
      xm, Q);
//    problem->addErrorTerm(e_pose);
  }
  const double L = 2.7; // wheelbase [m]
  const double e_r = 0.74; // half-track rear [m]
  const double e_f = 0.755; // half-track front [m]
  const double a0 = 0; // steering coefficient
  const double a1 = (M_PI / 180 / 10); // steering coefficient
  const double a2 = 0; // steering coefficient
  const double a3 = 0; // steering coefficient
  const double k_rl = 1.0 / 3.6 / 100.0; // wheel coefficient
  const double k_rr = 1.0 / 3.6 / 100.0; // wheel coefficient
  const double k_fl = 1.0 / 3.6 / 100.0; // wheel coefficient
  const double k_fr = 1.0 / 3.6 / 100.0; // wheel coefficient
  auto cpdv = boost::make_shared<VectorDesignVariable<11> >(
    (VectorDesignVariable<11>::Container() <<
    L, e_r, e_f, a0, a1, a2, a3, k_rl, k_rr, k_fl, k_fr).finished());
  cpdv->setActive(true);
  problem->addDesignVariable(cpdv);
  auto t_io_dv = boost::make_shared<EuclideanPoint>(
    Eigen::Vector3d(0, 0, -0.785));
  t_io_dv->setActive(true);
  auto C_io_dv = boost::make_shared<RotationQuaternion>(
    ypr->parametersToRotationMatrix(Eigen::Vector3d(0, 0, 0)));
  C_io_dv->setActive(true);
  RotationExpression C_io(C_io_dv);
  EuclideanExpression t_io(t_io_dv);
  problem->addDesignVariable(C_io_dv);
  problem->addDesignVariable(t_io_dv);
  auto T_io = TransformationExpression(C_io, t_io);
  TransformationExpression T_wi_km1;
  NsecTime lastTimestamp = -1;
  double lastDistance = -1;
  std::ofstream errorDmiPreFile("error_dmi_pre.txt");
  std::ofstream errorDmiPreChiFile("error_dmi_pre_chi.txt");
  for (auto it = encoderMeasurements.cbegin(); it != encoderMeasurements.cend();
      ++it) {
    if (timestamps[0] > it->first || timestamps[numMeasurements - 1]
        < it->first)
      continue;
    const double displacement = it->second.signedDistanceTraveled -
      lastDistance;
//    if (std::fabs(displacement) < 1e-2)
//      continue;
    auto translationExpressionFactory =
      translationSpline.getExpressionFactoryAt<0>(it->first);
    auto rotationExpressionFactory =
      rotationSpline.getExpressionFactoryAt<0>(it->first);
    auto t_wi = translationExpressionFactory.getValueExpression();
    auto C_wi = Vector2RotationQuaternionExpressionAdapter::adapt(
      rotationExpressionFactory.getValueExpression());
    auto T_wi_k = TransformationExpression(C_wi, t_wi);
    if (lastTimestamp != -1) {
      const Eigen::Matrix<double, 1, 1> meas((Eigen::Matrix<double, 1, 1>()
        << displacement).finished());
      auto T_o_km1_o_k = T_io.inverse() * T_wi_km1.inverse() * T_wi_k * T_io;
      auto t_o_km1_o_k = T_o_km1_o_k.toEuclideanExpression();
      auto C_o_km1_o_k = T_o_km1_o_k.toRotationExpression();
      auto ypr_o_km1_o_k = C_o_km1_o_k.toParameters(ypr);
      auto e_dmi = boost::make_shared<ErrorTermDMI>(t_o_km1_o_k, ypr_o_km1_o_k,
        cpdv.get(), meas,
        (Eigen::Matrix<double, 1, 1>() << 1000).finished());
//      problem->addErrorTerm(e_dmi);
      errorDmiPreChiFile << std::fixed << std::setprecision(18)
        << e_dmi->evaluateError() << std::endl;
      errorDmiPreFile << std::fixed << std::setprecision(18)
        << it->first << " " << e_dmi->error().transpose() << std::endl;
    }
    lastTimestamp = it->first;
    lastDistance = it->second.signedDistanceTraveled;
    T_wi_km1 = T_wi_k;
  }
  std::ofstream errorFwsPreFile("error_fws_pre.txt");
  std::ofstream errorFwsPreChiFile("error_fws_pre_chi.txt");
  for (auto it = frontWheelsSpeedMeasurements.cbegin();
      it != frontWheelsSpeedMeasurements.cend(); ++it) {
    if (timestamps[0] > it->first || timestamps[numMeasurements - 1]
        < it->first || it->second.left == 0 || it->second.right == 0)
      continue;
    auto translationExpressionFactory =
      translationSpline.getExpressionFactoryAt<1>(it->first);
    auto rotationExpressionFactory =
      rotationSpline.getExpressionFactoryAt<1>(it->first);
    auto C_wi = Vector2RotationQuaternionExpressionAdapter::adapt(
      rotationExpressionFactory.getValueExpression());
    auto v_ii = C_wi.inverse() *
      translationExpressionFactory.getValueExpression(1);
    auto om_ii = -(C_wi.inverse() *
      rotationExpressionFactory.getAngularVelocityExpression());
    auto v_oo = C_io.inverse() * (v_ii + om_ii.cross(t_io));
    auto om_oo = C_io.inverse() * om_ii;
    const double v_oo_x = v_oo.toValue()(0);
    const double om_oo_z = om_oo.toValue()(2);
    const double phi_L = atan(L * om_oo_z / (v_oo_x - e_f * om_oo_z));
    const double phi_R = atan(L * om_oo_z / (v_oo_x + e_f * om_oo_z));
    if ((v_oo_x - e_f * om_oo_z) / cos(phi_L) / k_fl < 0)
      continue;
    if ((v_oo_x + e_f * om_oo_z) / cos(phi_R) / k_fr < 0)
      continue;
    auto e_fws = boost::make_shared<ErrorTermFws>(v_oo, om_oo, cpdv.get(),
      Eigen::Vector2d(it->second.left, it->second.right),
      (Eigen::Matrix2d() << 2000, 0, 0, 2000).finished());
    problem->addErrorTerm(e_fws);
    errorFwsPreChiFile << std::fixed << std::setprecision(18)
      << e_fws->evaluateError() << std::endl;
    errorFwsPreFile << std::fixed << std::setprecision(18)
      << it->first << " " << e_fws->error().transpose() << std::endl;
  }
  std::ofstream errorRwsPreFile("error_rws_pre.txt");
  std::ofstream errorRwsPreChiFile("error_rws_pre_chi.txt");
  for (auto it = rearWheelsSpeedMeasurements.cbegin();
      it != rearWheelsSpeedMeasurements.cend(); ++it) {
    if (timestamps[0] > it->first || timestamps[numMeasurements - 1]
        < it->first || it->second.left == 0 || it->second.right == 0)
      continue;
    auto translationExpressionFactory =
      translationSpline.getExpressionFactoryAt<1>(it->first);
    auto rotationExpressionFactory =
      rotationSpline.getExpressionFactoryAt<1>(it->first);
    auto C_wi = Vector2RotationQuaternionExpressionAdapter::adapt(
      rotationExpressionFactory.getValueExpression());
    auto v_ii = C_wi.inverse() *
      translationExpressionFactory.getValueExpression(1);
    auto om_ii = -(C_wi.inverse() *
      rotationExpressionFactory.getAngularVelocityExpression());
    auto v_oo = C_io.inverse() * (v_ii + om_ii.cross(t_io));
    auto om_oo = C_io.inverse() * om_ii;
    const double v_oo_x = v_oo.toValue()(0);
    const double om_oo_z = om_oo.toValue()(2);
    if ((v_oo_x - e_r * om_oo_z) / k_rl < 0)
      continue;
    if ((v_oo_x + e_r * om_oo_z) / k_rr < 0)
      continue;
    auto e_rws = boost::make_shared<ErrorTermRws>(v_oo, om_oo, cpdv.get(),
      Eigen::Vector2d(it->second.left, it->second.right),
      (Eigen::Matrix2d() << 2000, 0, 0, 2000).finished());
    problem->addErrorTerm(e_rws);
    errorRwsPreChiFile << std::fixed << std::setprecision(18) <<
       e_rws->evaluateError() << std::endl;
    errorRwsPreFile << std::fixed << std::setprecision(18)
      << it->first << " " << e_rws->error().transpose() << std::endl;
  }
  std::ofstream errorStPreFile("error_st_pre.txt");
  std::ofstream errorStPreChiFile("error_st_pre_chi.txt");
  for (auto it = steeringMeasurements.cbegin();
      it != steeringMeasurements.cend(); ++it) {
    if (timestamps[0] > it->first || timestamps[numMeasurements - 1]
        < it->first)
      continue;
    auto translationExpressionFactory =
      translationSpline.getExpressionFactoryAt<1>(it->first);
    auto rotationExpressionFactory =
      rotationSpline.getExpressionFactoryAt<1>(it->first);
    auto C_wi = Vector2RotationQuaternionExpressionAdapter::adapt(
      rotationExpressionFactory.getValueExpression());
    auto v_ii = C_wi.inverse() *
      translationExpressionFactory.getValueExpression(1);
    auto om_ii = -(C_wi.inverse() *
      rotationExpressionFactory.getAngularVelocityExpression());
    auto v_oo = C_io.inverse() * (v_ii + om_ii.cross(t_io));
    auto om_oo = C_io.inverse() * om_ii;
    if (std::fabs(v_oo.toValue()(0)) < 1e-1)
      continue;
    Eigen::Matrix<double, 1, 1> meas;
    meas << it->second.value;
    auto e_st = boost::make_shared<ErrorTermSteering>(v_oo, om_oo, cpdv.get(),
      meas, (Eigen::Matrix<double, 1, 1>() << 1000).finished());
    problem->addErrorTerm(e_st);
    errorStPreChiFile << std::fixed << std::setprecision(18) <<
      e_st->evaluateError() << std::endl;
    errorStPreFile << std::fixed << std::setprecision(18)
      << it->first << " " << e_st->error().transpose() << std::endl;
  }

  std::cout << "Calibration before optimization: " << std::endl;
  std::cout << "CAN intrinsic: " << std::fixed << std::setprecision(18)
    << *cpdv << std::endl;
  std::cout << "Translation IMU-ODO: " << std::endl;
  std::cout << std::fixed << std::setprecision(18)
    << t_io.toValue().transpose() << std::endl;
  std::cout << "Rotation IMU-ODO: " << std::endl;
  std::cout << std::fixed << std::setprecision(18)
    << ypr->rotationMatrixToParameters(C_io.toRotationMatrix()).transpose()
    << std::endl;

  std::cout << "Optimizing..." << std::endl;
  Optimizer2Options options;
  options.verbose = true;
  options.linearSystemSolver = boost::make_shared<SparseQrLinearSystemSolver>();
  options.trustRegionPolicy =
    boost::make_shared<GaussNewtonTrustRegionPolicy>();
  SparseQRLinearSolverOptions linearSolverOptions;
  linearSolverOptions.colNorm = true;
  linearSolverOptions.qrTol = 0.02;
  Optimizer2 optimizer(options);
  optimizer.getSolver<SparseQrLinearSystemSolver>()->setOptions(
    linearSolverOptions);
  optimizer.setProblem(problem);
  optimizer.optimize();

  std::cout << "Calibration after optimization: " << std::endl;
  std::cout << "CAN intrinsic: " << std::fixed << std::setprecision(18)
    << *cpdv << std::endl;
  std::cout << "Translation IMU-ODO: " << std::endl;
  std::cout << std::fixed << std::setprecision(18)
    << t_io.toValue().transpose() << std::endl;
  std::cout << "Rotation IMU-ODO: " << std::endl;
  std::cout << std::fixed << std::setprecision(18)
    << ypr->rotationMatrixToParameters(C_io.toRotationMatrix()).transpose()
    << std::endl;
  const CompressedColumnMatrix<ssize_t>& RFactor =
    optimizer.getSolver<SparseQrLinearSystemSolver>()->getR();
  const size_t numCols = RFactor.cols();
//  std::cout << "Sigma: " << std::endl
//    << computeCovariance(RFactor, numCols - cpdv->minimalDimensions() -
//    C_io_dv->minimalDimensions() - t_io_dv->minimalDimensions(), numCols - 1).
//    diagonal().transpose() << std::endl;
//  std::cout << "Sigma: " << std::endl
//    << computeCovariance(RFactor, numCols - cpdv->minimalDimensions(), numCols - 1).
//    diagonal().transpose() << std::endl;
  std::ofstream RFile("R.txt");
  RFactor.writeMATLAB(RFile);
  const CompressedColumnMatrix<ssize_t>& J =
    optimizer.getSolver<SparseQrLinearSystemSolver>()->getJacobianTranspose();
  std::ofstream JFile("J.txt");
  J.writeMATLAB(JFile);
  std::cout << "Rank: " << optimizer.getSolver<SparseQrLinearSystemSolver>()
    ->getRank() << std::endl;

  std::cout << "Outputting spline data after optimization..." << std::endl;
  std::ofstream applanixSplineOptFile("applanix-spline-opt.txt");
  for (auto it = timestamps.cbegin(); it != timestamps.cend(); ++it) {
    auto translationExpressionFactory =
      translationSpline.getExpressionFactoryAt<2>(*it);
    auto rotationExpressionFactory =
      rotationSpline.getExpressionFactoryAt<1>(*it);
    Eigen::Matrix3d C_wi = Vector2RotationQuaternionExpressionAdapter::adapt(
      rotationExpressionFactory.getValueExpression()).toRotationMatrix();
    applanixSplineOptFile << std::fixed << std::setprecision(18)
      << *it << " "
      << translationExpressionFactory.getValueExpression().toValue().
        transpose() << " "
      << ypr->rotationMatrixToParameters(C_wi).transpose() << " "
      << translationExpressionFactory.getValueExpression(1).toValue().
        transpose() << " "
      << -(C_wi.transpose() *
        rotationExpressionFactory.getAngularVelocityExpression().toValue()).
        transpose() << " "
      << (C_wi.transpose() *
        translationExpressionFactory.getValueExpression(2).toValue()).
        transpose()
      << std::endl;
  }

  std::cout << "Integrating odometry..." << std::endl;
  janeth::DifferentialOdometry::Parameters params = {0.285, 0.285, 0.285, 0.285,
    e_r * 2, e_f * 2, 0.000045};
  std::ofstream diffOdoFile("diffOdo.txt");
  janeth::DifferentialOdometry odometry(params);
  bool firstPose = true;
  for (auto it = rearWheelsSpeedMeasurements.cbegin();
      it != rearWheelsSpeedMeasurements.cend(); ++it) {
    if (timestamps[0] > it->first || timestamps[numMeasurements - 1]
        < it->first)
      continue;
    if (firstPose) {
      auto translationExpressionFactory =
        translationSpline.getExpressionFactoryAt<0>(it->first);
      auto rotationExpressionFactory =
        rotationSpline.getExpressionFactoryAt<0>(it->first);
      Eigen::Vector3d tk = translationExpressionFactory.getValueExpression().
        toValue();
      Eigen::Matrix3d C_wi = Vector2RotationQuaternionExpressionAdapter::adapt(
        rotationExpressionFactory.getValueExpression()).toRotationMatrix();
      Eigen::Vector3d C_wi_param = ypr->rotationMatrixToParameters(C_wi);
      odometry.reset(Eigen::Vector3d(tk(0), tk(1), C_wi_param(0)),
        nsecToSec(it->first));
      firstPose = false;
    }
    odometry.updateRearWheelTranslationalVelocities(it->second.left * k_rl,
      it->second.right * k_rr, nsecToSec(it->first));
    diffOdoFile << std::fixed << std::setprecision(18)
      << odometry.getPose().transpose() << std::endl;
  }

  return 0;
}