/**
 * Copyright (c) 2021, NVIDIA CORPORATION.  All rights reserved.
 *
 * NVIDIA CORPORATION and its licensors retain all intellectual property
 * and proprietary rights in and to this software, related documentation
 * and any modifications thereto.  Any use, reproduction, disclosure or
 * distribution of this software and related documentation without an express
 * license agreement from NVIDIA CORPORATION is strictly prohibited.
 */

// Copyright 2008, 2019 Willow Garage, Inc., Andreas Klintberg, Joshua Whitley
// All rights reserved.
//
// Software License Agreement (BSD License 2.0)
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions
// are met:
//
// * Redistributions of source code must retain the above copyright
//   notice, this list of conditions and the following disclaimer.
// * Redistributions in binary form must reproduce the above
//   copyright notice, this list of conditions and the following
//   disclaimer in the documentation and/or other materials provided
//   with the distribution.
// * Neither the name of {copyright_holder} nor the names of its
//   contributors may be used to endorse or promote products derived
//   from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
// FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
// COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
// INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
// BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
// LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
// LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
// ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "isaac_ros_image_proc/rectify_node.hpp"

#include <string>
#include <unordered_map>

#include "cv_bridge/cv_bridge.h"
#include "image_transport/image_transport.hpp"
#include "opencv2/opencv.hpp"
#include "opencv2/calib3d/calib3d.hpp"
#include "rclcpp/rclcpp.hpp"
#include "sensor_msgs/distortion_models.hpp"
#include "vpi/algo/ConvertImageFormat.h"
#include "vpi/algo/Remap.h"
#include "vpi/LensDistortionModels.h"
#include "vpi/OpenCVInterop.hpp"
#include "vpi/VPI.h"

#include "isaac_ros_common/vpi_utilities.hpp"
#include "tracetools_image_pipeline/tracetools.h"

namespace
{
// Map the encoding desired string to the VPI Image Format needed
const std::unordered_map<std::string, VPIImageFormat> g_str_to_vpi_format({
    {"mono8", VPI_IMAGE_FORMAT_U8},
    {"mono16", VPI_IMAGE_FORMAT_U16},
    {"bgr8", VPI_IMAGE_FORMAT_BGR8},
    {"rgb8", VPI_IMAGE_FORMAT_RGB8},
    {"bgra8", VPI_IMAGE_FORMAT_BGRA8},
    {"rgba8", VPI_IMAGE_FORMAT_RGBA8}});
}  // namespace

namespace isaac_ros
{
namespace image_proc
{

RectifyNode::RectifyNode(const rclcpp::NodeOptions & options)
: Node("RectifyNode", options),
  sub_{image_transport::create_camera_subscription(
      this, "image", std::bind(
        &RectifyNode::RectifyCallback,
        this, std::placeholders::_1, std::placeholders::_2), "raw", rmw_qos_profile_sensor_data)},
  pub_{image_transport::create_publisher(this, "image_rect")},
  interpolation_{static_cast<VPIInterpolationType>(declare_parameter("interpolation",
    static_cast<int>(VPI_INTERP_CATMULL_ROM)))},
  vpi_backends_{isaac_ros::common::DeclareVPIBackendParameter(this, VPI_BACKEND_CUDA)}
{
  try {
    std::string backend_str = "CUDA";
    if (vpi_backends_ == VPI_BACKEND_VIC) {
      backend_str = "VIC";
    }
    RCLCPP_INFO(get_logger(), "Using backend %s", backend_str.c_str());

    // Create stream on specified backend
    CHECK_STATUS(vpiStreamCreate(vpi_backends_, &stream_));
  } catch (std::runtime_error & e) {
    RCLCPP_ERROR(get_logger(), "Error while initializing Rectify Node: %s", e.what());
  }
}

RectifyNode::~RectifyNode()
{
  vpiImageDestroy(image_);
  vpiStreamDestroy(stream_);
  vpiPayloadDestroy(remap_);
  vpiImageDestroy(tmp_in_);
  vpiImageDestroy(tmp_out_);
}

void RectifyNode::RectifyCallback(
  const sensor_msgs::msg::Image::ConstSharedPtr & image_msg,
  const sensor_msgs::msg::CameraInfo::ConstSharedPtr & info_msg)
{
  TRACEPOINT(
    image_proc_rectify_cb_init,
    static_cast<const void *>(this),
    static_cast<const void *>(&(*image_msg)),
    static_cast<const void *>(&(*info_msg)));

  // If there are no consumers for the rectified image, then don't spend compute resources
  if (pub_.getNumSubscribers() < 1) {
    TRACEPOINT(
      image_proc_rectify_cb_fini,
      static_cast<const void *>(this),
      static_cast<const void *>(&(*image_msg)),
      static_cast<const void *>(&(*info_msg)));
    return;
  }

  // Check focal length to ensure that the camera is calibrated
  if (info_msg->k[0] == 0.0) {
    RCLCPP_ERROR(
      get_logger(), "Rectified topic '%s' requested but camera publishing '%s' "
      "is uncalibrated", pub_.getTopic().c_str(), sub_.getInfoTopic().c_str());
    TRACEPOINT(
      image_proc_rectify_cb_fini,
      static_cast<const void *>(this),
      static_cast<const void *>(&(*image_msg)),
      static_cast<const void *>(&(*info_msg)));
    return;
  }

  // Make sure this is a supported image encoding
  if (g_str_to_vpi_format.find(image_msg->encoding) == g_str_to_vpi_format.end()) {
    RCLCPP_ERROR(
      get_logger(), "Requested image format %s has no known VPI correspondence",
      image_msg->encoding);
    TRACEPOINT(
      image_proc_rectify_cb_fini,
      static_cast<const void *>(this),
      static_cast<const void *>(&(*image_msg)),
      static_cast<const void *>(&(*info_msg)));
    return;
  }

  // If the image is undistorted (all distortion coeffs are 0), then no need to rectify
  // Simply republish the image
  if (std::all_of(info_msg->d.begin(), info_msg->d.end(), [](auto el) {return el == 0.0;})) {
    pub_.publish(image_msg);
    TRACEPOINT(
      image_proc_rectify_cb_fini,
      static_cast<const void *>(this),
      static_cast<const void *>(&(*image_msg)),
      static_cast<const void *>(&(*info_msg)));
    return;
  }

  TRACEPOINT(
    image_proc_rectify_init,
    static_cast<const void *>(this),
    static_cast<const void *>(&(*image_msg)),
    static_cast<const void *>(&(*info_msg)));

  // Collect pixel binning parameters from camera info
  uint32_t binning_x = info_msg->binning_x || 1;
  uint32_t binning_y = info_msg->binning_y || 1;
  cv::Size binned_resolution{static_cast<int>(info_msg->width / binning_x),
    static_cast<int>(info_msg->height / binning_y)};

  // Collect ROI parameters from camera info
  auto roi_msg = info_msg->roi;
  if (roi_msg.x_offset == 0 && roi_msg.y_offset == 0 && roi_msg.width == 0 && roi_msg.height == 0) {
    // ROI of all zeroes is treated as full resolution
    roi_msg.width = info_msg->width;
    roi_msg.height = info_msg->height;
  }
  cv::Rect roi{static_cast<int>(roi_msg.x_offset), static_cast<int>(roi_msg.y_offset),
    static_cast<int>(roi_msg.width), static_cast<int>(roi_msg.height)};

  // Convert K, P, D from arrays to cv::Mat
  cv::Matx33d K_mat{&info_msg->k[0]};
  cv::Matx34d P_mat{&info_msg->p[0]};
  cv::Matx33d R_mat{&info_msg->r[0]};
  cv::Mat_<double> D_mat(info_msg->d);

  // Since the ROI is given in full-image coordinates, adjust offset before scaling
  if (roi.x != 0 || roi.y != 0) {
    // Move principal point by the offset
    K_mat(0, 2) -= roi.x;
    K_mat(1, 2) -= roi.y;
    P_mat(0, 2) -= roi.x;
    P_mat(1, 2) -= roi.y;
  }
  if (binning_x > 1) {
    // Scale for pixel binning in x dimension
    K_mat(0, 0) /= binning_x;
    K_mat(0, 2) /= binning_x;
    P_mat(0, 0) /= binning_x;
    P_mat(0, 2) /= binning_x;
    P_mat(0, 3) /= binning_x;
    roi.x /= binning_x;
    roi.width /= binning_x;
  }
  if (binning_y > 1) {
    // Scale for pixel binning in y dimension
    K_mat(1, 1) /= binning_y;
    K_mat(1, 2) /= binning_y;
    P_mat(1, 1) /= binning_y;
    P_mat(1, 2) /= binning_y;
    P_mat(1, 3) /= binning_y;
    roi.y /= binning_y;
    roi.height /= binning_y;
  }

  // Create input/output CV Image
  auto image_ptr = cv_bridge::toCvCopy(image_msg);

  // Use VPI to rectify image
  RectifyImage(image_ptr, K_mat, P_mat, R_mat, D_mat, info_msg->distortion_model, roi);
  TRACEPOINT(
    image_proc_rectify_fini,
    static_cast<const void *>(this),
    static_cast<const void *>(&(*image_msg)),
    static_cast<const void *>(&(*info_msg)));

  // Allocate and publish new rectified image message
  pub_.publish(
    cv_bridge::CvImage(
      image_msg->header, image_msg->encoding,
      image_ptr->image).toImageMsg());
  
  TRACEPOINT(
      image_proc_rectify_cb_fini,
      static_cast<const void *>(this),
      static_cast<const void *>(&(*image_msg)),
      static_cast<const void *>(&(*info_msg)));
}

void RectifyNode::RectifyImage(
  cv_bridge::CvImagePtr & image_ptr, const cv::Matx33d & K_mat, const cv::Matx34d & P_mat,
  const cv::Matx33d & R_mat,
  const cv::Mat_<double> & D_mat,
  const std::string & distortion_model,
  cv::Rect roi)
{
  // Flag to track whether or not to regenerate warp map
  bool update_map = false;

  try {
    // Update warp map only when the camera intrinsics and extrinsics matrices are changed
    if (K_mat != curr_K_mat_) {
      curr_K_mat_ = K_mat;
      // Prepare camera intrinsics
      for (int i = 0; i < 2; ++i) {
        // Note that VPI doesn't require the bottom [0, 0, 1] row of intrinsic matrix
        for (int j = 0; j < 3; ++j) {
          K_[i][j] = static_cast<float>(K_mat(i, j));
        }
      }
      update_map = true;
    }
    if (P_mat != curr_P_mat_) {
      curr_P_mat_ = P_mat;
      // Prepare camera extrinsics assuming monocular camera.
      // Extrinsics matrix for VPI is filled by R matrix from camera info for rotation
      // it should be Identity matrix
      for (int i = 0; i < R_mat.rows; ++i) {
        for (int j = 0; j < R_mat.cols; ++j) {
          X_[i][j] = static_cast<float>(R_mat(i, j));
        }
      }
      // Translation is zero for monocular camera
      X_[0][3] = 0;
      X_[1][3] = 0;
      X_[2][3] = 0;

      update_map = true;
    }
    if (curr_D_mat_.empty() || cv::countNonZero(D_mat != curr_D_mat_) > 0) {
      curr_D_mat_ = D_mat;
      update_map = true;
    }

    if (current_roi_ != roi) {
      current_roi_ = roi;
      update_map = true;
    }

    // If either map or the camera intrinsics and extrinsics changed, regenerate warp maps
    if (update_map) {
      VPIWarpMap map{};
      map.grid.numHorizRegions = 1;
      map.grid.numVertRegions = 1;
      map.grid.horizInterval[0] = 8;
      map.grid.vertInterval[0] = 8;
      map.grid.regionWidth[0] = roi.width;
      map.grid.regionHeight[0] = roi.height;

      CHECK_STATUS(vpiWarpMapAllocData(&map));
      if (distortion_model == sensor_msgs::distortion_models::PLUMB_BOB) {
        // The VPI Fisheye model uses 4 parameters, while the ROS2 Plumb Bob model uses 5 parameters
        // We drop the 5th parameter when converting from ROS2 to VPI
        VPIFisheyeLensDistortionModel fisheye{};
        fisheye.mapping = VPI_FISHEYE_EQUIDISTANT;
        fisheye.k1 = D_mat(0);
        fisheye.k2 = D_mat(1);
        fisheye.k3 = D_mat(2);
        fisheye.k4 = D_mat(3);
        CHECK_STATUS(
          vpiWarpMapGenerateFromFisheyeLensDistortionModel(K_, X_, K_, &fisheye, &map));
      } else if (distortion_model == sensor_msgs::distortion_models::RATIONAL_POLYNOMIAL) {
        VPIPolynomialLensDistortionModel polynomial{};
        polynomial.k1 = D_mat(0);
        polynomial.k2 = D_mat(1);
        polynomial.p1 = D_mat(2);
        polynomial.p2 = D_mat(3);
        polynomial.k3 = D_mat(4);
        polynomial.k4 = D_mat(5);
        polynomial.k5 = D_mat(6);
        polynomial.k6 = D_mat(7);
        CHECK_STATUS(
          vpiWarpMapGenerateFromPolynomialLensDistortionModel(K_, X_, K_, &polynomial, &map));
      } else {  // Unknown distortion model
        throw std::runtime_error("Unrecognized distortion model: " + distortion_model);
      }

      // Create remap to undistort based on the generated distortion map
      if (remap_ != nullptr) {
        vpiPayloadDestroy(remap_);
      }
      CHECK_STATUS(vpiCreateRemap(vpi_backends_, &map, &remap_));

      vpiWarpMapFreeData(&map);
      // Prepare temporary VPI images for conversion to NV12 format
      CHECK_STATUS(
        vpiImageCreate(
          roi.width, roi.height, VPI_IMAGE_FORMAT_NV12_ER, 0,
          &tmp_in_));
      CHECK_STATUS(
        vpiImageCreate(
          roi.width, roi.height, VPI_IMAGE_FORMAT_NV12_ER, 0,
          &tmp_out_));
    }
    const bool consistent_dimensions = (image_dims_.first == image_ptr->image.rows &&
      image_dims_.second == image_ptr->image.cols);
    if (image_ == nullptr || !consistent_dimensions) {
      // Image can be "destroyed" even if it is null
      vpiImageDestroy(image_);
      CHECK_STATUS(
        vpiImageCreateOpenCVMatWrapper(
          image_ptr->image, g_str_to_vpi_format.at(image_ptr->encoding), 0, &image_));
      image_dims_ = {image_ptr->image.rows, image_ptr->image.cols};
    } else {
      CHECK_STATUS(vpiImageSetWrappedOpenCVMat(image_, image_ptr->image));
    }

    // Convert input to NV12 format, using CUDA backend for best performance
    CHECK_STATUS(vpiSubmitConvertImageFormat(stream_, VPI_BACKEND_CUDA, image_, tmp_in_, nullptr));

    // Undistort image
    CHECK_STATUS(
      vpiSubmitRemap(
        stream_, vpi_backends_, remap_, tmp_in_, tmp_out_, interpolation_,
        VPI_BORDER_ZERO, 0));

    // Convert output to original format, writing to original image for in-place modification
    // Using CUDA backend for best performance
    CHECK_STATUS(vpiSubmitConvertImageFormat(stream_, VPI_BACKEND_CUDA, tmp_out_, image_, nullptr));

    // Wait until all operations are complete
    CHECK_STATUS(vpiStreamSync(stream_));
  } catch (std::runtime_error & e) {
    RCLCPP_ERROR(get_logger(), "Error while rectifying image: %s", e.what());
  }
}

}  // namespace image_proc
}  // namespace isaac_ros

// Register as a component
#include "rclcpp_components/register_node_macro.hpp"
RCLCPP_COMPONENTS_REGISTER_NODE(isaac_ros::image_proc::RectifyNode)
