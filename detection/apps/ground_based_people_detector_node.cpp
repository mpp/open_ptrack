/*
 * Software License Agreement (BSD License)
 *
 * Copyright (c) 2013-, Matteo Munaro [matteo.munaro@dei.unipd.it]
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following
 * disclaimer in the documentation and/or other materials provided
 * with the distribution.
 * * Neither the name of the copyright holder(s) nor the names of its
 * contributors may be used to endorse or promote products derived
 * from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 * ground_based_people_detector_node.cpp
 * Created on: Jul 07, 2013
 * Author: Matteo Munaro
 *
 * ROS node which performs people detection assuming that people stand/walk on a ground plane.
 * As a first step, the ground is manually initialized, then people detection is performed with the GroundBasedPeopleDetectionApp class,
 * which implements the people detection algorithm described here:
 * M. Munaro, F. Basso and E. Menegatti,
 * Tracking people within groups with RGB-D data,
 * In Proceedings of the International Conference on Intelligent Robots and Systems (IROS) 2012, Vilamoura (Portugal), 2012.
 */

// ROS includes:
#include <ros/ros.h>
#include <ros/package.h>

// PCL includes:
#include <pcl/ros/conversions.h>
#include <pcl_ros/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl/visualization/pcl_visualizer.h>
#include <pcl/sample_consensus/sac_model_plane.h>
#include <pcl_conversions/pcl_conversions.h>

// Open PTrack includes:
#include <open_ptrack/detection/ground_segmentation.h>
#include <open_ptrack/detection/ground_based_people_detection_app.h>
#include <open_ptrack/opt_utils/conversions.h>

//Publish Messages
#include <opt_msgs/RoiRect.h>
#include <opt_msgs/Rois.h>
#include <std_msgs/String.h>
#include <std_msgs/Int32.h>
#include <sensor_msgs/CameraInfo.h>
#include <opt_msgs/Detection.h>
#include <opt_msgs/DetectionArray.h>
#include <opt_msgs/DetectionAndIndices.h>
#include <opt_msgs/DetectionAndIndicesArray.h>
#include <opt_msgs/DetectionIndicesAndTrajectoryID.h>
#include <opt_msgs/DetectionIndicesAndTrajectoryIDArray.h>

using namespace opt_msgs;
using namespace sensor_msgs;

typedef pcl::PointXYZRGB PointT;
typedef pcl::PointCloud<PointT> PointCloudT;

bool new_cloud_available_flag = false;
PointCloudT::Ptr cloud(new PointCloudT);
bool intrinsics_already_set = false;
Eigen::Matrix3f intrinsics_matrix;

int trajectory_id = -1;

void trajectory_id_cb(const std_msgs::Int32ConstPtr& idx_msg)
{
    /// TODO: check if a mutex block is necessary
    trajectory_id = idx_msg->data;
}

void
cloud_cb (const PointCloudT::ConstPtr& callback_cloud)
{
  *cloud = *callback_cloud;
  cloud->width = 640;
  cloud->height = 480;
  new_cloud_available_flag = true;
}

void
cameraInfoCallback (const sensor_msgs::CameraInfo::ConstPtr & msg)
{
  if (!intrinsics_already_set)
  {
    intrinsics_matrix << msg->K.elems[0], msg->K.elems[1], msg->K.elems[2],
        msg->K.elems[3], msg->K.elems[4], msg->K.elems[5],
        msg->K.elems[6], msg->K.elems[7], msg->K.elems[8];
    intrinsics_already_set = true;
  }
}

int
main (int argc, char** argv)
{
  ros::init(argc, argv, "ground_based_people_detector");
  ros::NodeHandle nh("~");

  // Read some parameters from launch file:
  int ground_estimation_mode;
  nh.param("ground_estimation_mode", ground_estimation_mode, 0);
  std::string svm_filename;

  nh.param("classifier_file", svm_filename, std::string("./"));


  bool use_rgb;
  nh.param("use_rgb", use_rgb, false);
  int minimum_luminance;
  nh.param("minimum_luminance", minimum_luminance, 20);
  double min_confidence;
  nh.param("ground_based_people_detection_min_confidence", min_confidence, -1.5);
  double max_distance;
  nh.param("max_distance", max_distance, 50.0);
  double min_height;
  nh.param("minimum_person_height", min_height, 1.3);
  double max_height;
  nh.param("maximum_person_height", max_height, 2.3);
  int sampling_factor;
  nh.param("sampling_factor", sampling_factor, 1);
  std::string pointcloud_topic;
  nh.param("pointcloud_topic", pointcloud_topic, std::string("/camera/depth_registered/points"));
  std::string output_topic;
  nh.param("output_topic", output_topic, std::string("/ground_based_people_detector/detections"));
  std::string camera_info_topic;
  nh.param("camera_info_topic", camera_info_topic, std::string("/camera/rgb/camera_info"));
  double rate_value;
  nh.param("rate", rate_value, 30.0);
  // If true, exploit extrinsic calibration for estimatin the ground plane equation:
  bool ground_from_extrinsic_calibration;
  nh.param("ground_from_extrinsic_calibration", ground_from_extrinsic_calibration, false);
  bool lock_ground; // if true, do not update the ground plane at every frame
  nh.param("lock_ground", lock_ground, false);
  bool sensor_tilt_compensation;
  nh.param("sensor_tilt_compensation", sensor_tilt_compensation, false);
  double valid_points_threshold;
  nh.param("valid_points_threshold", valid_points_threshold, 0.2);

  // EUROC parameters
  std::string detection_and_indices_topic;
  nh.param("detection_and_indices_topic", detection_and_indices_topic, std::string("/ground_based_people_detector/detection_and_indices"));
  std::string noground_cloud_topic;
  nh.param("noground_cloud_topic", noground_cloud_topic, std::string("/ground_based_people_detector/noground_cloud"));

  // Fixed parameters:
  float voxel_size = 0.03;
  //	Eigen::Matrix3f intrinsics_matrix;
  intrinsics_matrix << 525, 0.0, 319.5, 0.0, 525, 239.5, 0.0, 0.0, 1.0; // Kinect RGB camera intrinsics

  // Initialize transforms to be used to correct sensor tilt to identity matrix:
  Eigen::Affine3f transform, anti_transform;
  transform = transform.Identity();
  anti_transform = transform.inverse();

  // Subscribers:
  ros::Subscriber sub = nh.subscribe(pointcloud_topic, 1, cloud_cb);
  ros::Subscriber camera_info_sub = nh.subscribe(camera_info_topic, 1, cameraInfoCallback);

  // EUROC Subscribers:
  ros::Subscriber trajectory_id_sub = nh.subscribe("/t1_1_trajectory_index", 1, trajectory_id_cb);

  // Publishers:
  ros::Publisher detection_pub;
  detection_pub= nh.advertise<DetectionArray>(output_topic, 3);

  // EUROC publishers:
  ros::Publisher noground_cloud = nh.advertise< sensor_msgs::PointCloud2/*pcl::PointCloud<pcl::PointXYZRGB>*/ >(noground_cloud_topic, 1);
  ros::Publisher detection_and_indices_pub = nh.advertise< opt_msgs::DetectionIndicesAndTrajectoryIDArray >(detection_and_indices_topic, 1);

  Rois output_rois_;
  open_ptrack::opt_utils::Conversions converter;

  ros::Rate rate(rate_value);
  while(ros::ok() && !new_cloud_available_flag)
  {
    ros::spinOnce();
    rate.sleep();
  }

  // Loop until a valid point cloud is found
  open_ptrack::detection::GroundplaneEstimation<PointT> ground_estimator(ground_estimation_mode);
  bool first_valid_frame = false;
  while (!first_valid_frame)
  {
    if (!ground_estimator.tooManyNaN(cloud, 1 - valid_points_threshold))
    { // A point cloud is valid if the ratio #NaN / #valid points is lower than a threshold
      first_valid_frame = true;
      std::cout << "Valid frame found! Ground plane initialization starting..." << std::endl;
    }
    else
    {
      std::cout << "No valid frame. Move the camera to a better position..." << std::endl;
    }

    // Execute callbacks:
    ros::spinOnce();
    rate.sleep();
  }

  // Ground estimation:
  Eigen::VectorXf ground_coeffs;
  // Ground plane equation is computed from the current point cloud data:
  ground_estimator.setInputCloud(cloud);
  ground_coeffs = ground_estimator.compute();

  if (ground_from_extrinsic_calibration)
  { // Ground plane equation derived from extrinsic calibration:
    int pos = pointcloud_topic.find("/", 1);
    std::string camera_name = pointcloud_topic.substr(1, pos-1);

    // Read worldToCam transform from file:
    std::string filename = ros::package::getPath("detection") + "/launch/camera_poses.txt";
    tf::Transform worldToCamTransform = ground_estimator.readTFFromFile (filename, camera_name);

    // Compute ground coeffs from world to camera transform:
    Eigen::VectorXf ground_coeffs_calib = ground_estimator.computeFromTF (worldToCamTransform);

    // If ground could not be well estimated from point cloud data, use calibration data:
    // (if error in ground plane estimation from point cloud OR if d coefficient estimated from point cloud
    // is too different from d coefficient obtained from calibration)
    bool updated = false; // states if ground plane coefficients are updated according to the point cloud or not
    if ((ground_coeffs.sum() == 0.0) | (std::fabs(float(ground_coeffs_calib(3) - ground_coeffs(3))) > 0.2))
    {
      updated = ground_estimator.refineGround (10, voxel_size, 300 * 0.06 / voxel_size / std::pow (static_cast<double> (sampling_factor), 2), ground_coeffs_calib);

      ground_coeffs = ground_coeffs_calib;

      if (updated)
        std::cout << "Chosen ground plane estimate obtained from calibration and refined with point cloud." << std::endl;
      else
        std::cout << "Chosen ground plane estimate obtained from calibration." << std::endl;
    }
  }

  // Create classifier for people detection:
  open_ptrack::detection::PersonClassifier<pcl::RGB> person_classifier;
  person_classifier.loadSVMFromFile(svm_filename);   // load trained SVM

  // People detection app initialization:
  open_ptrack::detection::GroundBasedPeopleDetectionApp<PointT> people_detector;    // people detection object
  people_detector.setVoxelSize(voxel_size);                        // set the voxel size
  people_detector.setMaxDistance(max_distance);                    // set maximum distance of people from the sensor
  people_detector.setIntrinsics(intrinsics_matrix);                // set RGB camera intrinsic parameters
  people_detector.setClassifier(person_classifier);                // set person classifier
  people_detector.setHeightLimits(min_height, max_height);         // set person classifier
  people_detector.setSamplingFactor(sampling_factor);              // set sampling factor
  people_detector.setUseRGB(use_rgb);                              // set if RGB should be used or not
  people_detector.setSensorTiltCompensation(sensor_tilt_compensation);						 // enable point cloud rotation correction
  people_detector.setMinimumDistanceBetweenHeads(2.0);

  // Main loop:
  while(ros::ok())
  {
    if (new_cloud_available_flag)
    {
      new_cloud_available_flag = false;

      // Convert PCL cloud header to ROS header:
      std_msgs::Header cloud_header = pcl_conversions::fromPCL(cloud->header);

      // Perform people detection on the new cloud:
      std::vector<pcl::people::PersonCluster<PointT> > clusters;   // vector containing persons clusters
      people_detector.setInputCloud(cloud);
      people_detector.setGround(ground_coeffs);                    // set floor coefficients
      people_detector.compute(clusters);                           // perform people detection

      // EUROC get no ground cloud
      pcl::PointCloud<pcl::PointXYZRGB>::Ptr no_ground(new pcl::PointCloud<pcl::PointXYZRGB>);
      no_ground = people_detector.getNoGroundCloud();

      // If not lock_ground, update ground coefficients:
      if (not lock_ground)
        ground_coeffs = people_detector.getGround();                 // get updated floor coefficients

      if (sensor_tilt_compensation)
        people_detector.getTiltCompensationTransforms(transform, anti_transform);

      /// Write detection message:
      DetectionArray::Ptr detection_array_msg(new DetectionArray);
      DetectionIndicesAndTrajectoryIDArray::Ptr detection_and_indices_array_msg(new DetectionIndicesAndTrajectoryIDArray);
      // Set camera-specific fields:
      detection_array_msg->header = cloud_header;
      detection_and_indices_array_msg->header = cloud_header;
      for(int i = 0; i < 3; i++)
        for(int j = 0; j < 3; j++)
        {
            detection_array_msg->intrinsic_matrix.push_back(intrinsics_matrix(i, j));
            detection_and_indices_array_msg->intrinsic_matrix.push_back(intrinsics_matrix(i, j));
        }

      // Add all valid detections:
      for(std::vector<pcl::people::PersonCluster<PointT> >::iterator it = clusters.begin(); it != clusters.end(); ++it)
      {
        if((!use_rgb) | (people_detector.getMeanLuminance() < minimum_luminance) |      // if RGB is not used or luminance is too low
            ((people_detector.getMeanLuminance() >= minimum_luminance) & (it->getPersonConfidence() > min_confidence)))            // if RGB is used, keep only people with confidence above a threshold
        {
          // Create detection message:
          Detection detection_msg;
          DetectionAndIndices detectionAndIndices_msg;
          converter.Vector3fToVector3(anti_transform * it->getMin(), detection_msg.box_3D.p1);
          converter.Vector3fToVector3(anti_transform * it->getMax(), detection_msg.box_3D.p2);

          float head_centroid_compensation = 0.05;

          // theoretical person centroid:
          Eigen::Vector3f centroid3d = anti_transform * it->getTCenter();
          Eigen::Vector3f centroid2d = converter.world2cam(centroid3d, intrinsics_matrix);
          // theoretical person top point:
          Eigen::Vector3f top3d = anti_transform * it->getTTop();
          Eigen::Vector3f top2d = converter.world2cam(top3d, intrinsics_matrix);
          // theoretical person bottom point:
          Eigen::Vector3f bottom3d = anti_transform * it->getTBottom();
          Eigen::Vector3f bottom2d = converter.world2cam(bottom3d, intrinsics_matrix);
          float enlarge_factor = 1.1;
          float pixel_xc = centroid2d(0);
          float pixel_yc = centroid2d(1);
          float pixel_height = (bottom2d(1) - top2d(1)) * enlarge_factor;
          float pixel_width = pixel_height / 2;
          detection_msg.box_2D.x = int(centroid2d(0) - pixel_width/2.0);
          detection_msg.box_2D.y = int(centroid2d(1) - pixel_height/2.0);
          detection_msg.box_2D.width = int(pixel_width);
          detection_msg.box_2D.height = int(pixel_height);
          detection_msg.height = it->getHeight();
          detection_msg.confidence = it->getPersonConfidence();
          detection_msg.distance = it->getDistance();

          converter.Vector3fToVector3((1+head_centroid_compensation/centroid3d.norm())*centroid3d, detection_msg.centroid);
          converter.Vector3fToVector3((1+head_centroid_compensation/top3d.norm())*top3d, detection_msg.top);
          converter.Vector3fToVector3((1+head_centroid_compensation/bottom3d.norm())*bottom3d, detection_msg.bottom);

          detectionAndIndices_msg.detection = detection_msg;

          detectionAndIndices_msg.indices.header = cloud_header;
          //pcl::PointIndices indices = it->getIndices();
          //indices.indices
          detectionAndIndices_msg.indices.indices = (it->getIndices()).indices;

          // Add message:
          detection_array_msg->detections.push_back(detection_msg);
          detection_and_indices_array_msg->detections.push_back(detectionAndIndices_msg);
        }
      }
      detection_and_indices_array_msg->trajectory_id.data = trajectory_id;
      detection_pub.publish(detection_array_msg);		 // publish message

      // EUROC publish messages
      detection_and_indices_pub.publish(detection_and_indices_array_msg);
      sensor_msgs::PointCloud2 no_ground_ros_msg;
      pcl::toROSMsg(*no_ground, no_ground_ros_msg);
      noground_cloud.publish(no_ground_ros_msg);
    }

    // Execute callbacks:
    ros::spinOnce();
    rate.sleep();
  }
  return 0;
}

