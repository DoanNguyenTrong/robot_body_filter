#include <utility>

#include <functional>
#include <memory>

/* HACK HACK HACK */
/* We use it to access mesh bounding box. */
#define protected public
#include <geometric_shapes/bodies.h>
#undef protected

#include "robot_self_filter/RobotSelfFilter.h"

#include "pluginlib/class_list_macros.h"

#include <geometric_shapes/body_operations.h>
#include <geometric_shapes/shape_operations.h>
#include <geometric_shapes/shape_to_marker.h>

#include <sensor_msgs/point_cloud_conversion.h>
#include <eigen_conversions/eigen_msg.h>
#include <sensor_msgs/point_cloud2_iterator.h>

#include <pcl_conversions/pcl_conversions.h>
#include <tf2_sensor_msgs/tf2_sensor_msgs.h>
#include <tf2_eigen/tf2_eigen.h>

#include <robot_self_filter/utils/bodies.h>
#include <robot_self_filter/utils/time_utils.hpp>
#include <robot_self_filter/utils/set_utils.hpp>
#include <robot_self_filter/utils/string_utils.hpp>
#include <robot_self_filter/utils/shapes.h>
#include <robot_self_filter/utils/tf2_eigen.h>
#include <robot_self_filter/utils/urdf_eigen.hpp>

using namespace std;
using namespace sensor_msgs;
using namespace filters;

namespace robot_self_filter {

template<typename T>
RobotSelfFilter<T>::RobotSelfFilter() : privateNodeHandle("~") {
  this->modelMutex.reset(new std::mutex());
}

template<typename T>
bool RobotSelfFilter<T>::configure() {
  this->tfBufferLength = this->getParamVerbose("tf_buffer_length", ros::Duration(60.0), "s");

  if (this->tfBuffer == nullptr)
  {
    this->tfBuffer = std::make_shared<tf2_ros::Buffer>(this->tfBufferLength);
    this->tfListener = std::make_unique<tf2_ros::TransformListener>(*this->tfBuffer);
  } else {
    // clear the TF buffer (useful if calling configure() after receiving old TF data)
    this->tfBuffer->clear();
  }

  this->fixedFrame = this->getParamVerbose("fixed_frame", "base_link");
  this->sensorFrame = this->getParamVerbose("sensor_frame", "laser");
  this->minDistance = this->getParamVerbose("min_distance", 0.0, "m");
  this->maxDistance = this->getParamVerbose("max_distance", 0.0, "m");
  this->inflationPadding = this->getParamVerbose("inflation_padding", 0.0, "m");
  this->inflationScale = this->getParamVerbose("inflation_scale", 1.0);
  this->robotDescriptionParam = this->getParamVerbose("robot_description_param", "robot_description");
  this->keepCloudsOrganized = this->getParamVerbose("keep_clouds_organized", true);
  this->reachableTransformTimeout = this->getParamVerbose("reachable_transform_timeout", ros::Duration(1.0), "s");
  this->unreachableTransformTimeout = this->getParamVerbose("unreachable_transform_timeout", ros::Duration(0.1), "s");
  this->publishNoBoundingSpherePointcloud = this->getParamVerbose("publish_no_bounding_sphere_pointcloud", false);
  this->publishNoBoundingBoxPointcloud = this->getParamVerbose("publish_no_bounding_box_pointcloud", false);
  this->computeBoundingSphere = this->getParamVerbose("compute_bounding_sphere", false) || this->publishNoBoundingSpherePointcloud;
  this->computeDebugBoundingSphere = this->getParamVerbose("compute_debug_bounding_sphere", false);
  this->computeBoundingBox = this->getParamVerbose("compute_bounding_box", false) || this->publishNoBoundingBoxPointcloud;
  this->computeDebugBoundingBox = this->getParamVerbose("compute_debug_bounding_box", false);
  this->publishDebugPclInside = this->getParamVerbose("publish_debug_pcl_inside", false);
  this->publishDebugPclClip = this->getParamVerbose("publish_debug_pcl_clip", false);
  this->publishDebugPclShadow = this->getParamVerbose("publish_debug_pcl_shadow", false);
  this->publishDebugContainsMarker = this->getParamVerbose("publish_debug_contains_marker", false);
  this->publishDebugShadowMarker = this->getParamVerbose("publish_debug_shadow_marker", false);

  // can contain either whole link names, or scoped names of their collisions (i.e. "link::collision_1" or "link::my_collision")
  this->linksIgnoredInBoundingSphere = this->template getParamVerboseSet<set<string>>("links_ignored_in_bounding_sphere");
  this->linksIgnoredInBoundingBox = this->template getParamVerboseSet<set<string> >("links_ignored_in_bounding_box");
  this->linksIgnoredInContainsTest = this->template getParamVerboseSet<set<string> >("links_ignored_in_contains_test");
  this->linksIgnoredInShadowTest = this->template getParamVerboseSet<set<string> >("links_ignored_in_shadow_test", { "laser" });
  this->linksIgnoredEverywhere = this->template getParamVerboseSet<set<string> >("links_ignored_everywhere");

  this->robotDescriptionUpdatesFieldName = this->getParamVerbose("dynamic_robot_description_field_name", "robot_model");
  // subscribe for robot_description param changes
  this->robotDescriptionUpdatesListener = this->nodeHandle.subscribe(
    "dynamic_robot_model_server/parameter_updates", 10, &RobotSelfFilter::robotDescriptionUpdated, this);

  // for some reason, I have to do this template black magic here :(
  this->reloadRobotModelServiceServer = this->privateNodeHandle.advertiseService<RobotSelfFilter<T>, std_srvs::TriggerRequest, std_srvs::TriggerResponse>(
      "reload_model", &RobotSelfFilter<T>::triggerModelReload, (RobotSelfFilter<T>*)this);

  if (this->computeBoundingSphere) {
    this->boundingSpherePublisher = this->nodeHandle.template advertise<SphereStamped>("robot_bounding_sphere", 100);
  }

  if (this->computeBoundingBox) {
    this->boundingBoxPublisher = this->nodeHandle.template advertise<geometry_msgs::PolygonStamped>("robot_bounding_box", 100);
  }

  if (this->publishNoBoundingBoxPointcloud)
  {
    this->scanPointCloudNoBoundingBoxPublisher = this->nodeHandle.template advertise<sensor_msgs::PointCloud2>("scan_point_cloud_no_bbox", 100);
  }

  if (this->publishNoBoundingSpherePointcloud)
  {
    this->scanPointCloudNoBoundingSpherePublisher = this->nodeHandle.template advertise<sensor_msgs::PointCloud2>("scan_point_cloud_no_bsphere", 100);
  }

  if (this->publishDebugPclInside)
  {
    this->debugPointCloudInsidePublisher = this->nodeHandle.template advertise<sensor_msgs::PointCloud2>("scan_point_cloud_inside", 100);
  }

  if (this->publishDebugPclClip)
  {
    this->debugPointCloudClipPublisher = this->nodeHandle.template advertise<sensor_msgs::PointCloud2>("scan_point_cloud_clip", 100);
  }

  if (this->publishDebugPclShadow)
  {
    this->debugPointCloudShadowPublisher = this->nodeHandle.template advertise<sensor_msgs::PointCloud2>("scan_point_cloud_shadow", 100);
  }

  if (this->publishDebugContainsMarker)
  {
    this->debugContainsMarkerPublisher = this->nodeHandle.template advertise<visualization_msgs::MarkerArray>("robot_model_for_contains_test", 100);
  }

  if (this->publishDebugShadowMarker)
  {
    this->debugShadowMarkerPublisher = this->nodeHandle.template advertise<visualization_msgs::MarkerArray>("robot_model_for_shadow_test", 100);
  }

  if (this->computeDebugBoundingBox) {
    this->boundingBoxDebugMarkerPublisher = this->nodeHandle.template advertise<visualization_msgs::MarkerArray>(
      "robot_bounding_box_debug", 100);
  }

  if (this->computeDebugBoundingSphere) {
    this->boundingSphereDebugMarkerPublisher = this->nodeHandle.template advertise<visualization_msgs::MarkerArray>(
      "robot_bounding_sphere_debug", 100);
  }

  // initialize the 3D body masking tool
  auto getShapeTransformCallback = std::bind(&RobotSelfFilter::getShapeTransform, this, std::placeholders::_1, std::placeholders::_2);
  shapeMask = std::make_unique<RayCastingShapeMask>(getShapeTransformCallback,
      this->minDistance, this->maxDistance);

  { // initialize the robot body to be masked out

    string robotUrdf;
    while (!this->nodeHandle.getParam(this->robotDescriptionParam, robotUrdf) || robotUrdf.length() == 0) {
      if (!ros::ok())
        return false;

      ROS_ERROR("RobotSelfFilter: %s is empty or not set. Please, provide the robot model. Waiting 1s.",
                robotDescriptionParam.c_str());
      ros::Duration(1.0).sleep();
    }

    if (!this->shapesToLinks.empty())
      this->clearRobotMask();
    this->addRobotMaskFromUrdf(robotUrdf);
  }

  if (this->tfFramesWatchdog == nullptr) {
    std::set<std::string> monitoredFrames;
    for (const auto& shapeToLink : this->shapesToLinks)
      monitoredFrames.insert(shapeToLink.second.link->name);

    this->tfFramesWatchdog = std::make_shared<TFFramesWatchdog>(this->fixedFrame,
        monitoredFrames, this->tfBuffer, this->unreachableTransformTimeout,
        ros::Rate(ros::Duration(1.0)));
    this->tfFramesWatchdog->start();
  } else {
    this->tfFramesWatchdog->pause();
    this->tfFramesWatchdog->clear();
    this->tfFramesWatchdog->unpause();
  }

  ROS_INFO("RobotSelfFilter: Successfully configured.");

  this->timeConfigured = ros::Time::now();

  return true;
}

bool RobotSelfFilterLaserScan::configure() {
  bool success = RobotSelfFilter::configure();
  if (!success)
    return false;

  this->pointByPointScan = this->getParamVerbose("point_by_point_scan", true);

  return true;
}

bool RobotSelfFilterPointCloud2::configure() {
  bool success = RobotSelfFilter::configure();
  if (!success)
    return false;

  this->pointByPointScan = this->getParamVerbose("point_by_point_scan", false);

  return true;
}

template <typename T>
bool RobotSelfFilter<T>::computeMask(
    const sensor_msgs::PointCloud2 &projectedPointCloud,
    std::vector<RayCastingShapeMask::MaskValue> &pointMask) {

  // this->modelMutex has to be already locked!

  const clock_t stopwatchOverall = clock();
  const auto& scanTime = projectedPointCloud.header.stamp;

  const auto sensorTf = this->tfBuffer->lookupTransform(
      this->fixedFrame, this->sensorFrame, scanTime,
      remainingTime(scanTime, this->reachableTransformTimeout));
  Eigen::Vector3d sensorPosition;
  tf2::fromMsg(sensorTf.transform.translation, sensorPosition);

  // compute a mask of point indices for points from projectedPointCloud
  // that tells if they are inside or outside robot, or shadow points

  if (!this->pointByPointScan)
  {
    // update transforms cache, which is then used in body masking
    this->updateTransformCache(scanTime);

    // updates shapes according to tf cache (by calling getShapeTransform
    // for each shape) and masks contained points
    this->shapeMask->maskContainmentAndShadows(projectedPointCloud, pointMask, sensorPosition.cast<float>());
  } else {
    CloudConstIter x_it(projectedPointCloud, "x");
    CloudConstIter y_it(projectedPointCloud, "y");
    CloudConstIter z_it(projectedPointCloud, "z");
    CloudConstIter vp_x_it(projectedPointCloud, "vp_x");
    CloudConstIter vp_y_it(projectedPointCloud, "vp_y");
    CloudConstIter vp_z_it(projectedPointCloud, "vp_z");
    CloudConstIter stamps_it(projectedPointCloud, "stamps");
    CloudIndexConstIter index_it(projectedPointCloud, "index");

    Eigen:: Vector3f point, pointSensorPosition, viewPoint;
    RayCastingShapeMask::MaskValue mask;
    pointMask.resize(num_points(projectedPointCloud));

    double scanDuration = 0.0;
    for (CloudConstIter stamps_end_it(projectedPointCloud, "stamps"); stamps_end_it != stamps_end_it.end(); ++stamps_end_it)
    {
      if ((*stamps_end_it) > static_cast<float>(scanDuration))
        scanDuration = static_cast<double>(*stamps_end_it);
    }
    const ros::Time afterScanTime(scanTime + ros::Duration().fromSec(scanDuration));

    // update transforms cache, which is then used in body masking
    this->updateTransformCache(scanTime, afterScanTime);

    for (size_t i = 0; i < num_points(projectedPointCloud); ++i, ++x_it, ++y_it, ++z_it, ++vp_x_it, ++vp_y_it, ++vp_z_it, ++stamps_it, ++index_it)
    {
      point.x() = *x_it;
      point.y() = *y_it;
      point.z() = *z_it;

      viewPoint.x() = *vp_x_it;
      viewPoint.y() = *vp_y_it;
      viewPoint.z() = *vp_z_it;

      this->cacheLookupBetweenScansRatio = static_cast<double>(*stamps_it) / scanDuration;

      // updates shapes according to tf cache (by calling getShapeTransform
      // for each shape) and masks contained points
      this->shapeMask->maskContainmentAndShadows(point, mask, viewPoint);
      pointMask[i] = mask;
    }
  }

  ROS_DEBUG("RobotSelfFilter: Mask computed in %.5f secs.", double(clock()-stopwatchOverall) / CLOCKS_PER_SEC);

  this->publishDebugPointClouds(projectedPointCloud, pointMask);
  this->publishDebugMarkers(scanTime);
  this->computeAndPublishBoundingSphere(projectedPointCloud);
  this->computeAndPublishBoundingBox(projectedPointCloud);

  ROS_DEBUG("RobotSelfFilter: Filtering run time is %.5f secs.", double(clock()-stopwatchOverall) / CLOCKS_PER_SEC);
  return true;
}

bool RobotSelfFilterLaserScan::update(const LaserScan &inputScan, LaserScan &filteredScan) {
  const auto& scanTime = inputScan.header.stamp;

  if (!this->configured_) {
    ROS_DEBUG("Ignore scan from time %u.%u - filter not yet initialized.",
              scanTime.sec, scanTime.nsec);
    return false;
  } else if ((scanTime < timeConfigured) && (scanTime >= (timeConfigured - tfBufferLength))) {
    ROS_DEBUG("Ignore scan from time %u.%u - filter not yet initialized.",
              scanTime.sec, scanTime.nsec);
    return false;
  } else if ((scanTime < timeConfigured) && (scanTime < (timeConfigured - tfBufferLength))) {
    ROS_WARN("Old TF data received. Clearing TF buffer and reconfiguring laser filter. If you're replaying a "
             "bag file, make sure rosparam /use_sim_time is set to true");
    this->configure();
    return false;
  }

  const clock_t stopwatchOverall = clock();

  // tf2 doesn't like frames starting with slash
  this->sensorFrame = inputScan.header.frame_id;
  stripLeadingSlash(this->sensorFrame, true);

  // create the output copy of the input scan
  filteredScan = inputScan;
  filteredScan.header.frame_id = this->sensorFrame;
  filteredScan.range_min = max(inputScan.range_min, (float) this->minDistance);
  if (this->maxDistance > 0.0)
    filteredScan.range_max = min(inputScan.range_max, (float) this->maxDistance);

  { // acquire the lock here, because we work with the tfBuffer all the time
    std::lock_guard<std::mutex> guard(*this->modelMutex);

    { // make sure we have all the tfs between sensor frame and fixedFrame during the time of scan acquisition
      const auto scanDuration = inputScan.ranges.size() * inputScan.time_increment;
      const auto afterScanTime = scanTime + ros::Duration().fromSec(scanDuration);

      string err;
      if (!this->tfBuffer->canTransform(this->fixedFrame, this->sensorFrame, scanTime,
            remainingTime(scanTime, this->reachableTransformTimeout), &err) ||
            !this->tfBuffer->canTransform(this->fixedFrame, this->sensorFrame, afterScanTime,
                remainingTime(afterScanTime, this->reachableTransformTimeout), &err)) {
        ROS_ERROR_THROTTLE(3, "RobotSelfFilter: Cannot transform laser scan to "
          "fixed frame. Something's wrong with TFs: %s", err.c_str());
        return false;
      }
    }

    // The point cloud will have fields x, y, z, intensity (float32) and index (int32)
    sensor_msgs::PointCloud2 projectedPointCloud;
    { // project the scan measurements to a point cloud in the fixedFrame
      const auto channelOptions =
          laser_geometry::channel_option::Intensity |
          laser_geometry::channel_option::Index |
          laser_geometry::channel_option::Timestamp |
          laser_geometry::channel_option::Viewpoint;
      laser_geometry::LaserProjection laserProjector;
      laserProjector.transformLaserScanToPointCloud(this->fixedFrame,
          inputScan, projectedPointCloud, *this->tfBuffer, -1, channelOptions);

      // the projected point cloud can omit some measurements if they are out of the defined scan's range;
      // for this case, the second channel ("index") contains indices of the point cloud's points into the scan

      projectedPointCloud.header.frame_id = this->fixedFrame;
      // according to LaserProjector, the point cloud is created so that it corresponds to the time of the first
      // measurement
      projectedPointCloud.header.stamp = scanTime;
    }

    ROS_DEBUG("RobotSelfFilter: Scan transformation run time is %.5f secs.", double(clock()-stopwatchOverall) / CLOCKS_PER_SEC);

    vector<RayCastingShapeMask::MaskValue> pointMask;
    const auto success = this->computeMask(projectedPointCloud, pointMask);
    if (!success)
      return false;

    { // remove invalid points
      const float INVALID_POINT_VALUE = std::numeric_limits<float>::quiet_NaN();
      try {
        sensor_msgs::PointCloud2Iterator<int> indexIt(projectedPointCloud, "index");

        size_t indexInScan;
        for (const auto maskValue : pointMask) {
          switch (maskValue) {
            case RayCastingShapeMask::MaskValue::INSIDE:
            case RayCastingShapeMask::MaskValue::SHADOW:
            case RayCastingShapeMask::MaskValue::CLIP:
              indexInScan = static_cast<const size_t>(*indexIt);
              filteredScan.ranges[indexInScan] = INVALID_POINT_VALUE;
              break;
            case RayCastingShapeMask::MaskValue::OUTSIDE:
              break;
          }
          ++indexIt;
        }
      }
      catch (std::runtime_error&) {
        ROS_ERROR("RobotSelfFilter: projectedPointCloud doesn't have field called 'index',"
                  " but the algorithm relies on that.");
        return false;
      }
    }
  }

  return true;
}

bool RobotSelfFilterPointCloud2::update(const sensor_msgs::PointCloud2 &inputCloud,
                                        sensor_msgs::PointCloud2 &filteredCloud)
{
  const auto& scanTime = inputCloud.header.stamp;

  if (!this->configured_) {
    ROS_DEBUG("RobotSelfFilter: Ignore cloud from time %u.%u - filter not yet initialized.",
              scanTime.sec, scanTime.nsec);
    return false;
  } else if ((scanTime < this->timeConfigured) && (scanTime >= (this->timeConfigured - this->tfBufferLength))) {
    ROS_DEBUG("RobotSelfFilter: Ignore cloud from time %u.%u - filter not yet initialized.",
              scanTime.sec, scanTime.nsec);
    return false;
  } else if ((scanTime < this->timeConfigured) && (scanTime < (this->timeConfigured - this->tfBufferLength))) {
    ROS_WARN("RobotSelfFilter: Old TF data received. Clearing TF buffer and "
             "reconfiguring laser filter. If you're replaying a bag file, make "
             "sure rosparam /use_sim_time is set to true");
    this->configure();
    return false;
  }

  bool hasIndexField = false, hasStampsField = false;
  bool hasVpXField = false, hasVpYField = false, hasVpZField = false;
  for (const auto& field : inputCloud.fields) {
    if (field.name == "index" && field.datatype == sensor_msgs::PointField::INT32)
      hasIndexField = true;
    else if (field.name == "stamps" && field.datatype == sensor_msgs::PointField::FLOAT32)
      hasStampsField = true;
    else if (field.name == "vp_x" && field.datatype == sensor_msgs::PointField::FLOAT32)
      hasVpXField = true;
    else if (field.name == "vp_y" && field.datatype == sensor_msgs::PointField::FLOAT32)
      hasVpYField = true;
    else if (field.name == "vp_z" && field.datatype == sensor_msgs::PointField::FLOAT32)
      hasVpZField = true;
  }

  // Verify the pointcloud and its fields

  if (!hasIndexField) {
    throw std::runtime_error("The input pointcloud has to contain an int32 'index' field.");
  }

  if (this->pointByPointScan) {
    if (inputCloud.height != 1 && inputCloud.is_dense == false) {
      ROS_WARN_ONCE("RobotSelfFilter: The pointcloud seems to be an organized "
                    "pointcloud, which usually means it was captured all at once."
                    " Consider setting 'point_by_point_scan' to false to get a "
                    "more efficient computation.");
    }
    if (!hasStampsField || !hasVpXField || !hasVpYField || !hasVpZField) {
      throw std::runtime_error("A point-by-point scan has to contain float32"
                               "fields 'stamps', 'vp_x', 'vp_y' and 'vp_z'.");
    }
  } else if (hasStampsField) {
    ROS_WARN_ONCE("RobotSelfFilter: The pointcloud has a 'stamps' field, "
                  "which indicates each point was probably captured at a "
                  "different time instant. Consider setting parameter "
                  "'point_by_point_scan' to true to get correct results.");
  } else if (inputCloud.height == 1 && inputCloud.is_dense == true) {
    ROS_WARN_ONCE("RobotSelfFilter: The pointcloud is dense, which usually means"
                  "it was captured each point at a different time instant. "
                  "Consider setting 'point_by_point_scan! to true to get a more"
                  " accurate version.");
  }

  // Compute the mask and use it

  vector<RayCastingShapeMask::MaskValue> pointMask;
  {
    std::lock_guard<std::mutex> guard(*this->modelMutex);

    const auto success = this->computeMask(inputCloud, pointMask);
    if (!success)
      return false;
  }

  createFilteredCloud(inputCloud,
    [pointMask] (size_t index, float, float, float) -> bool {
      return pointMask[index] == RayCastingShapeMask::MaskValue::OUTSIDE;
    }, filteredCloud, this->keepCloudsOrganized);

  return true;
}

template<typename T>
bool RobotSelfFilter<T>::getShapeTransform(point_containment_filter::ShapeHandle shapeHandle, Eigen::Isometry3d &transform) const {
  // make sure you locked this->modelMutex

  // check if the given shapeHandle has been registered to a link during addRobotMaskFromUrdf call.
  if (this->shapesToLinks.find(shapeHandle) == this->shapesToLinks.end()) {
    ROS_ERROR_STREAM("RobotSelfFilter: Invalid shape handle: " << to_string(shapeHandle));
    return false;
  }

  const auto& collision = this->shapesToLinks.at(shapeHandle);

  if (this->transformCache.find(collision.cacheKey) == this->transformCache.end()) {
    // do not log the error because shape mask would do it for us
    return false;
  }

  if (!this->pointByPointScan) {
    transform = *this->transformCache.at(collision.cacheKey);
  } else {
    if (this->transformCacheAfterScan.find(collision.cacheKey) == this->transformCache.end()) {
      // do not log the error because shape mask would do it for us
      return false;
    }

    const auto& tf1 = *this->transformCache.at(collision.cacheKey);
    const auto& tf2 = *this->transformCacheAfterScan.at(collision.cacheKey);
    const Eigen::Quaterniond quat1(tf1.rotation().matrix());
    const Eigen::Quaterniond quat2(tf1.rotation().matrix());
    const auto r = this->cacheLookupBetweenScansRatio;

    transform.translation() = tf1.translation() * r + tf2.translation() * (1 - r);
    const Eigen::Quaterniond quat3 = quat1.slerp(r, quat2);
    transform.linear() = quat3.toRotationMatrix();
  }

  return true;
}

template<typename T>
void RobotSelfFilter<T>::updateTransformCache(const ros::Time &time, const ros::Time& afterScanTime) {
  // make sure you locked this->modelMutex

  // clear the cache so that maskContainment always uses only these tf data and not some older
  this->transformCache.clear();
  if (afterScanTime.sec != 0)
    this->transformCacheAfterScan.clear();

  // iterate over all links corresponding to some masking shape and update their cached transforms relative
  // to fixed_frame
  for (auto &shapeToLink : this->shapesToLinks) {

    const auto &collisionBody = shapeToLink.second;
    const auto &collision = collisionBody.collision;
    const auto &link = collisionBody.link;

    // here we assume the tf frames' names correspond to the link names
    const auto linkFrame = link->name;

    // the collision object may have a different origin than the visual, we need to account for that
    const auto &collisionOffsetTransform = urdfPose2EigenTransform(collision->origin);

    {
      auto linkTransformTfOptional = this->tfFramesWatchdog->lookupTransform(
          linkFrame, time, this->reachableTransformTimeout);

      if (!linkTransformTfOptional.has_value())
        continue;

      const auto &linkTransformTf = linkTransformTfOptional.value();
      const auto &linkTransformEigen = tf2::transformToEigen(linkTransformTf);

      const auto &transform = linkTransformEigen * collisionOffsetTransform;

      this->transformCache[collisionBody.cacheKey] =
          std::make_shared<Eigen::Isometry3d>(transform);
    }

    if (afterScanTime.sec != 0)
    {
      auto linkTransformTfOptional = this->tfFramesWatchdog->lookupTransform(
          linkFrame, afterScanTime, this->reachableTransformTimeout);

      if (!linkTransformTfOptional.has_value())
        continue;

      const auto &linkTransformTf = linkTransformTfOptional.value();
      const auto &linkTransformEigen = tf2::transformToEigen(linkTransformTf);

      const auto &transform = linkTransformEigen * collisionOffsetTransform;

      this->transformCacheAfterScan[collisionBody.cacheKey] =
          std::make_shared<Eigen::Isometry3d>(transform);
    }
  }
}

template<typename T>
void RobotSelfFilter<T>::addRobotMaskFromUrdf(const string& urdfModel) {
  if (urdfModel.empty()) {
    ROS_ERROR("RobotSelfFilter: Empty string passed as robot model to addRobotMaskFromUrdf. "
              "Robot body filtering is not going to work.");
    return;
  }

  // parse the URDF model
  urdf::Model parsedUrdfModel;
  bool urdfParseSucceeded = parsedUrdfModel.initString(urdfModel);
  if (!urdfParseSucceeded) {
    ROS_ERROR_STREAM("RobotSelfFilter: The URDF model given in parameter '" <<
        this->robotDescriptionParam << "' cannot be parsed. See "
        "urdf::Model::initString for debugging, or try running "
        "'gzsdf my_robot.urdf'");
    return;
  }

  {
    std::lock_guard<std::mutex> guard(*this->modelMutex);

    this->shapesIgnoredInBoundingSphere.clear();
    this->shapesIgnoredInBoundingBox.clear();
    std::set<point_containment_filter::ShapeHandle> ignoreInContainsTest;
    std::set<point_containment_filter::ShapeHandle> ignoreInShadowTest;

    // add all model's collision links as masking shapes
    for (const auto &links : parsedUrdfModel.links_) {

      const auto& link = links.second;

      // every link can have multiple collision elements
      size_t collisionIndex = 0;
      for (const auto& collision : link->collision_array) {
        if (collision->geometry == nullptr) {
          ROS_WARN("RobotSelfFilter: Collision element without geometry found in link %s of robot %s. "
                   "This collision element will not be filtered out.",
                   link->name.c_str(), parsedUrdfModel.getName().c_str());
          continue;  // collisionIndex is intentionally not increased
        }

        const std::set<std::string> collisionNames = {
            link->name,
            link->name + "::" + collision->name,
            "*::" + collision->name,
            // not std::to_string - we need a locale-independent thing!
            link->name + "::" + boost::lexical_cast<string>(collisionIndex),
        };

        // if the link is ignored, go on
        if (!isSetIntersectionEmpty(collisionNames, this->linksIgnoredEverywhere)) {
          ++collisionIndex;
          continue;
        }

        const auto collisionShape = constructShape(*collision->geometry);

        // add the collision shape to shapeMask; the inflation parameters come into play here
        const auto shapeHandle = this->shapeMask->addShape(collisionShape,
            this->inflationScale, this->inflationPadding, false);
        this->shapesToLinks[shapeHandle] = CollisionBodyWithLink(collision, link, collisionIndex);

        if (!isSetIntersectionEmpty(collisionNames, this->linksIgnoredInBoundingSphere)) {
          this->shapesIgnoredInBoundingSphere.insert(shapeHandle);
        }

        if (!isSetIntersectionEmpty(collisionNames, this->linksIgnoredInBoundingBox)) {
          this->shapesIgnoredInBoundingBox.insert(shapeHandle);
        }

        if (!isSetIntersectionEmpty(collisionNames, this->linksIgnoredInContainsTest)) {
          ignoreInContainsTest.insert(shapeHandle);
        }

        if (!isSetIntersectionEmpty(collisionNames, this->linksIgnoredInShadowTest)) {
          ignoreInShadowTest.insert(shapeHandle);
        }

        ++collisionIndex;
      }

      if (collisionIndex == 0) {
        ROS_WARN(
          "RobotSelfFilter: No collision element found for link %s of robot %s. This link will not be filtered out "
          "from laser scans.", link->name.c_str(), parsedUrdfModel.getName().c_str());
      }
    }

    this->shapeMask->setIgnoreInContainsTest(ignoreInContainsTest);
    this->shapeMask->setIgnoreInShadowTest(ignoreInShadowTest);

    this->shapeMask->updateInternalShapeLists();
  }
}

template<typename T>
void RobotSelfFilter<T>::clearRobotMask() {
  {
    std::lock_guard<std::mutex> guard(*this->modelMutex);

    for (const auto& shapeToLink : this->shapesToLinks) {
      this->shapeMask->removeShape(shapeToLink.first, false);
    }
    this->shapeMask->updateInternalShapeLists();

    this->shapesToLinks.clear();
    this->shapesIgnoredInBoundingSphere.clear();
    this->shapesIgnoredInBoundingBox.clear();
    this->transformCache.clear();
    this->transformCacheAfterScan.clear();
  }

  this->tfFramesWatchdog->clear();
}

template <typename T>
void RobotSelfFilter<T>::publishDebugMarkers(const ros::Time& scanTime) const {
  // assume this->modelMutex is locked

  if (this->publishDebugContainsMarker) {
    visualization_msgs::MarkerArray markerArray;
    std_msgs::ColorRGBA color;
    color.g = 1.0;
    color.a = 0.5;
    createBodyVisualizationMsg(this->shapeMask->getBodiesForContainsTest(), scanTime,
                               color, markerArray);
    this->debugContainsMarkerPublisher.publish(markerArray);
  }

  if (this->publishDebugShadowMarker) {
    visualization_msgs::MarkerArray markerArray;
    std_msgs::ColorRGBA color;
    color.b = 1.0;
    color.a = 0.5;
    createBodyVisualizationMsg(this->shapeMask->getBodiesForShadowTest(), scanTime,
                               color, markerArray);
    this->debugShadowMarkerPublisher.publish(markerArray);
  }
}

template <typename T>
void RobotSelfFilter<T>::publishDebugPointClouds(
    const sensor_msgs::PointCloud2& projectedPointCloud,
    const std::vector<RayCastingShapeMask::MaskValue> &pointMask) const
{
  if (this->publishDebugPclInside)
  {
    sensor_msgs::PointCloud2 insideCloud;
    createFilteredCloud(projectedPointCloud,
      [&pointMask] (size_t i, float, float, float) -> bool {
        return pointMask[i] == RayCastingShapeMask::MaskValue::INSIDE;
      }, insideCloud, this->keepCloudsOrganized);
    this->debugPointCloudInsidePublisher.publish(insideCloud);
  }

  if (this->publishDebugPclClip)
  {
    sensor_msgs::PointCloud2 clipCloud;
    createFilteredCloud(projectedPointCloud,
      [&pointMask] (size_t i, float, float, float) -> bool {
        return pointMask[i] == RayCastingShapeMask::MaskValue::CLIP;
      }, clipCloud, this->keepCloudsOrganized);
    this->debugPointCloudClipPublisher.publish(clipCloud);
  }

  if (this->publishDebugPclShadow)
  {
    sensor_msgs::PointCloud2 shadowCloud;
    createFilteredCloud(projectedPointCloud,
      [&pointMask] (size_t i, float, float, float) -> bool {
        return pointMask[i] == RayCastingShapeMask::MaskValue::SHADOW;
      }, shadowCloud, this->keepCloudsOrganized);
    this->debugPointCloudShadowPublisher.publish(shadowCloud);
  }
}

template<typename T>
void RobotSelfFilter<T>::computeAndPublishBoundingSphere(
    const sensor_msgs::PointCloud2& projectedPointCloud) const
{
  if (!this->computeBoundingSphere && !this->computeDebugBoundingSphere)
    return;

  // assume this->modelMutex is locked

  const auto& scanTime = projectedPointCloud.header.stamp;
  std::vector<bodies::BoundingSphere> spheres;
  {
    visualization_msgs::MarkerArray boundingSphereDebugMsg;
    for (const auto &shapeHandleAndSphere : this->shapeMask->getBoundingSpheres())
    {
      const auto &shapeHandle = shapeHandleAndSphere.first;
      const auto &sphere = shapeHandleAndSphere.second;

      if (this->shapesIgnoredInBoundingSphere.find(shapeHandle) == this->shapesIgnoredInBoundingSphere.end())
      {
        spheres.push_back(sphere);
      }

      if (this->computeDebugBoundingSphere)
      {
        visualization_msgs::Marker msg;
        msg.header.stamp = scanTime;
        msg.header.frame_id = this->fixedFrame;

        msg.scale.x = msg.scale.y = msg.scale.z = sphere.radius * 2;

        msg.pose.position.x = sphere.center[0];
        msg.pose.position.y = sphere.center[1];
        msg.pose.position.z = sphere.center[2];

        msg.color.g = 1.0;
        msg.color.a = 0.5;
        msg.type = visualization_msgs::Marker::SPHERE;
        msg.action = visualization_msgs::Marker::ADD;
        msg.ns = "bsphere/" + this->shapesToLinks.at(shapeHandle).cacheKey;
        msg.frame_locked = static_cast<unsigned char>(true);

        boundingSphereDebugMsg.markers.push_back(msg);
      }
    }

    if (this->computeDebugBoundingSphere) {
      this->boundingSphereDebugMarkerPublisher.publish(boundingSphereDebugMsg);
    }
  }

  if (this->computeBoundingSphere)
  {
    bodies::BoundingSphere boundingSphere;
    bodies::mergeBoundingSpheres(spheres, boundingSphere);

    robot_self_filter::SphereStamped boundingSphereMsg;
    boundingSphereMsg.header.stamp = scanTime;
    boundingSphereMsg.header.frame_id = this->fixedFrame;
    boundingSphereMsg.sphere.radius =
        static_cast<float>(boundingSphere.radius);
    boundingSphereMsg.sphere.center = tf2::toMsg(boundingSphere.center);

    this->boundingSpherePublisher.publish(boundingSphereMsg);

    if (this->publishNoBoundingSpherePointcloud)
    {
      sensor_msgs::PointCloud2 noSphereCloud;
      createFilteredCloud(projectedPointCloud,
                          [&boundingSphere] (size_t i, float x, float y, float z) -> bool {
                            const Eigen::Vector3d pt(x, y, z);
                            return (pt - boundingSphere.center).norm() > boundingSphere.radius;
                          }, noSphereCloud, this->keepCloudsOrganized);
      this->scanPointCloudNoBoundingBoxPublisher.publish(noSphereCloud);
    }
  }
}

template<typename T>
void RobotSelfFilter<T>::computeAndPublishBoundingBox(
    const sensor_msgs::PointCloud2& projectedPointCloud) const
{
  if (!this->computeBoundingBox && !this->computeDebugBoundingBox)
    return;

  // assume this->modelMutex is locked

  const auto& scanTime = projectedPointCloud.header.stamp;
  std::vector<bodies::AxisAlignedBoundingBox> boxes;

  {
    visualization_msgs::MarkerArray boundingBoxDebugMsg;
    for (const auto& shapeHandleAndBox : this->shapeMask->getAxisAlignedBoundingBoxes())
    {
      const auto& shapeHandle = shapeHandleAndBox.first;
      const auto& box = shapeHandleAndBox.second;

      if (this->shapesIgnoredInBoundingBox.find(shapeHandle) == this->shapesIgnoredInBoundingBox.end())
      {
        boxes.push_back(box);
      }

      if (this->computeDebugBoundingBox) {
        visualization_msgs::Marker msg;
        msg.header.stamp = scanTime;
        msg.header.frame_id = this->fixedFrame;

        // TODO it is aligned to odom and not base_link!
        tf2::toMsg(box.sizes(), msg.scale);
        msg.pose.position = tf2::toMsg((Eigen::Vector3d)box.center());
        msg.pose.orientation.w = 1;

        msg.color.g = 1.0;
        msg.color.a = 0.5;
        msg.type = visualization_msgs::Marker::CUBE;
        msg.action = visualization_msgs::Marker::ADD;
        msg.ns = "bbox/" + this->shapesToLinks.at(shapeHandle).cacheKey;
        msg.frame_locked = static_cast<unsigned char>(true);

        boundingBoxDebugMsg.markers.push_back(msg);
      }
    }

    if (this->computeDebugBoundingBox) {
      this->boundingBoxDebugMarkerPublisher.publish(boundingBoxDebugMsg);
    }
  }

  if (this->computeBoundingBox)
  {
    bodies::AxisAlignedBoundingBox box;
    bodies::mergeAxisAlignedBoundingBoxes(boxes, box);
    const auto boxFloat = box.cast<float>();

    geometry_msgs::PolygonStamped boundingBoxMsg;

    boundingBoxMsg.header.stamp = scanTime;
    boundingBoxMsg.header.frame_id = this->fixedFrame;

    boundingBoxMsg.polygon.points.resize(2);
    tf2::toMsg(box.min(), boundingBoxMsg.polygon.points[0]);
    tf2::toMsg(box.max(), boundingBoxMsg.polygon.points[1]);

    this->boundingBoxPublisher.publish(boundingBoxMsg);

    // compute and publish the scan_point_cloud with robot bounding box removed
    if (this->publishNoBoundingBoxPointcloud) {

      pcl::PCLPointCloud2::Ptr bboxCropInput(new pcl::PCLPointCloud2());
      pcl_conversions::toPCL(projectedPointCloud, *(bboxCropInput));

      pcl::CropBox<pcl::PCLPointCloud2> cropBox;
      cropBox.setNegative(true);
      cropBox.setInputCloud(bboxCropInput);
      cropBox.setKeepOrganized(this->keepCloudsOrganized);

      cropBox.setMin(Eigen::Vector4f(boxFloat.min()[0], boxFloat.min()[1], boxFloat.min()[2], 0.0));
      cropBox.setMax(Eigen::Vector4f(boxFloat.max()[0], boxFloat.max()[1], boxFloat.max()[2], 0.0));

      pcl::PCLPointCloud2 pclOutput;
      cropBox.filter(pclOutput);

      sensor_msgs::PointCloud2Ptr boxFilteredCloud(new sensor_msgs::PointCloud2());
      pcl_conversions::moveFromPCL(pclOutput, *boxFilteredCloud);
      boxFilteredCloud->header.stamp = scanTime;  // PCL strips precision of timestamp

      this->scanPointCloudNoBoundingBoxPublisher.publish(boxFilteredCloud);
    }
  }
}

template<typename T>
void RobotSelfFilter<T>::createBodyVisualizationMsg(
    const std::map<point_containment_filter::ShapeHandle, bodies::Body*>& bodies,
    const ros::Time& stamp, const std_msgs::ColorRGBA& color,
    visualization_msgs::MarkerArray& markerArray) const
{
  for (const auto &shapeHandleAndBody : bodies)
  {
    const auto &shapeHandle = shapeHandleAndBody.first;
    auto body = shapeHandleAndBody.second;

    visualization_msgs::Marker msg;
    bodies::constructMarkerFromBody(*body, msg);

    msg.header.stamp = stamp;
    msg.header.frame_id = this->fixedFrame;

    msg.color = color;
    msg.action = visualization_msgs::Marker::ADD;
    msg.ns = this->shapesToLinks.at(shapeHandle).cacheKey;
    msg.frame_locked = static_cast<unsigned char>(true);

    markerArray.markers.push_back(msg);
  }
}

template<typename T>
void RobotSelfFilter<T>::robotDescriptionUpdated(dynamic_reconfigure::ConfigConstPtr newConfig) {
  auto robotDescriptionIdx = static_cast<size_t>(-1);
  for (size_t i = 0; i < newConfig->strs.size(); ++i) {
    if (newConfig->strs[i].name == this->robotDescriptionUpdatesFieldName) {
      robotDescriptionIdx = i;
      break;
    }
  }

  // robot_description parameter was not found, so we don't have to restart the filter
  if (robotDescriptionIdx == static_cast<size_t>(-1))
    return;

  auto urdf = newConfig->strs[robotDescriptionIdx].value;

  ROS_INFO("RobotSelfFilter: Reloading robot model because of dynamic_reconfigure update. Filter operation stopped.");

  this->tfFramesWatchdog->pause();
  this->configured_ = false;

  this->clearRobotMask();
  this->addRobotMaskFromUrdf(urdf);

  this->tfFramesWatchdog->unpause();
  this->timeConfigured = ros::Time::now();
  this->configured_ = true;

  ROS_INFO("RobotSelfFilter: Robot model reloaded, resuming filter operation.");
}

template<typename T>
bool RobotSelfFilter<T>::triggerModelReload(std_srvs::TriggerRequest &,
                                              std_srvs::TriggerResponse &)
{
  std::string urdf;
  auto success = this->nodeHandle.getParam(this->robotDescriptionParam, urdf);

  if (!success)
  {
    ROS_ERROR_STREAM("RobotSelfFilter: Parameter " << this->robotDescriptionParam
        << " doesn't exist.");
    return false;
  }

  ROS_INFO("RobotSelfFilter: Reloading robot model because of trigger. Filter operation stopped.");

  this->tfFramesWatchdog->pause();
  this->configured_ = false;

  this->clearRobotMask();
  this->addRobotMaskFromUrdf(urdf);

  this->tfFramesWatchdog->unpause();
  this->timeConfigured = ros::Time::now();
  this->configured_ = true;

  ROS_INFO("RobotSelfFilter: Robot model reloaded, resuming filter operation.");
}

}

PLUGINLIB_EXPORT_CLASS(robot_self_filter::RobotSelfFilterLaserScan, filters::FilterBase<sensor_msgs::LaserScan>)
PLUGINLIB_EXPORT_CLASS(robot_self_filter::RobotSelfFilterPointCloud2, filters::FilterBase<sensor_msgs::PointCloud2>)
