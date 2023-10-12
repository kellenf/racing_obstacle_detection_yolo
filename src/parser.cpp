// Copyright (c) 2022，Horizon Robotics.
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

#include "racing_obstacle_detection/parser.h"

#include "rapidjson/document.h"
#include "rapidjson/istreamwrapper.h"
#include "rapidjson/writer.h"

#include <memory>

using hobot::dnn_node::DNNTensor;

namespace hobot {
namespace dnn_node {
namespace racing_obstacle_detection {
// 算法输出解析参数
struct PTQYolo5Config {
  std::vector<int> strides;
  std::vector<std::vector<std::pair<double, double>>> anchors_table;
  int class_num;
  std::vector<std::string> class_names;
  std::vector<std::vector<float>> dequantize_scale;
};
bool load_config = false;
float score_threshold_ = 0.3;
float nms_threshold_ = 0.65;
int nms_top_k_ = 5000;

PTQYolo5Config yolo5_config_ = {
    {8, 16, 32},
    {{{10, 13}, {16, 30}, {33, 23}},
     {{30, 61}, {62, 45}, {59, 119}},
     {{116, 90}, {156, 198}, {373, 326}}},
    1,
    {"construction_cone"}};

void ParseTensor(std::shared_ptr<DNNTensor> tensor,
                 int layer,
                 std::vector<YoloV5Result> &results);

void yolo5_nms(std::vector<YoloV5Result> &input,
               float iou_threshold,
               int top_k,
               std::vector<std::shared_ptr<YoloV5Result>> &result,
               bool suppress);

int get_tensor_hw(std::shared_ptr<DNNTensor> tensor, int *height, int *width);

/**
 * Finds the greatest element in the range [first, last)
 * @tparam[in] ForwardIterator: iterator type
 * @param[in] first: fist iterator
 * @param[in] last: last iterator
 * @return Iterator to the greatest element in the range [first, last)
 */
template <class ForwardIterator>
inline size_t argmax(ForwardIterator first, ForwardIterator last) {
  return std::distance(first, std::max_element(first, last));
}

void ParseTensor(std::shared_ptr<DNNTensor> tensor,
                 int layer,
                 std::vector<YoloV5Result> &results) {
  hbSysFlushMem(&(tensor->sysMem[0]), HB_SYS_MEM_CACHE_INVALIDATE);
  int num_classes = yolo5_config_.class_num;
  int stride = yolo5_config_.strides[layer];
  int num_pred = yolo5_config_.class_num + 4 + 1;

  std::vector<float> class_pred(yolo5_config_.class_num, 0.0);
  std::vector<std::pair<double, double>> &anchors =
      yolo5_config_.anchors_table[layer];

  //  int *shape = tensor->data_shape.d;
  int height, width;
  auto ret = get_tensor_hw(tensor, &height, &width);
  if (ret != 0) {
    RCLCPP_ERROR(rclcpp::get_logger("Yolo5_detection_parser"),
                 "get_tensor_hw failed");
  }

  int anchor_num = anchors.size();
  auto *data = reinterpret_cast<float *>(tensor->sysMem[0].virAddr);
  for (int h = 0; h < height; h++) {
    for (int w = 0; w < width; w++) {
      for (int k = 0; k < anchor_num; k++) {
        double anchor_x = anchors[k].first;
        double anchor_y = anchors[k].second;
        float *cur_data = data + k * num_pred;
        float objness = cur_data[4];

        int id = argmax(cur_data + 5, cur_data + 5 + num_classes);
        double x1 = 1 / (1 + std::exp(-objness)) * 1;
        double x2 = 1 / (1 + std::exp(-cur_data[id + 5]));
        double confidence = x1 * x2;

        if (confidence < score_threshold_) {
          continue;
        }

        float center_x = cur_data[0];
        float center_y = cur_data[1];
        float scale_x = cur_data[2];
        float scale_y = cur_data[3];

        double box_center_x =
            ((1.0 / (1.0 + std::exp(-center_x))) * 2 - 0.5 + w) * stride;
        double box_center_y =
            ((1.0 / (1.0 + std::exp(-center_y))) * 2 - 0.5 + h) * stride;

        double box_scale_x =
            std::pow((1.0 / (1.0 + std::exp(-scale_x))) * 2, 2) * anchor_x;
        double box_scale_y =
            std::pow((1.0 / (1.0 + std::exp(-scale_y))) * 2, 2) * anchor_y;

        double xmin = (box_center_x - box_scale_x / 2.0);
        double ymin = (box_center_y - box_scale_y / 2.0);
        double xmax = (box_center_x + box_scale_x / 2.0);
        double ymax = (box_center_y + box_scale_y / 2.0);

        if (xmax <= 0 || ymax <= 0) {
          continue;
        }

        if (xmin > xmax || ymin > ymax) {
          continue;
        }
        std::string name_conf = std::to_string(confidence);
        name_conf = name_conf + yolo5_config_.class_names[static_cast<int>(id)];
        results.emplace_back(
            YoloV5Result(static_cast<int>(id),
                         xmin,
                         ymin,
                         xmax,
                         ymax,
                         confidence,
                         yolo5_config_.class_names[static_cast<int>(id)]));
      }
      data = data + num_pred * anchors.size();
    }
  }
}

int InitClassNames(const std::string &cls_name_file) {
  std::ifstream fi(cls_name_file);
  if (fi) {
    yolo5_config_.class_names.clear();
    std::string line;
    while (std::getline(fi, line)) {
      yolo5_config_.class_names.push_back(line);
    }
    int size = yolo5_config_.class_names.size();
    if(size != yolo5_config_.class_num){
      RCLCPP_ERROR(rclcpp::get_logger("Yolo5_detection_parser"),
                 "class_names length %d is not equal to class_num %d",
                 size, yolo5_config_.class_num);
      return -1;
    }
  } else {
    RCLCPP_ERROR(rclcpp::get_logger("Yolo5_detection_parser"),
                 "can not open cls name file: %s",
                 cls_name_file.c_str());
    return -1;
  }
  return 0;
}

int InitClassNum(const int &class_num) {
  if(class_num > 0){
    yolo5_config_.class_num = class_num;
  } else {
    RCLCPP_ERROR(rclcpp::get_logger("Yolo5_detection_parser"),
                 "class_num = %d is not allowed, only support class_num > 0",
                 class_num);
    return -1;
  }
  return 0;
}

void LoadConfig(const std::string &config_file) {
  if (config_file.empty()) {
    RCLCPP_ERROR(rclcpp::get_logger("LoadConfig"),
                 "Config file [%s] is empty!",
                 config_file.data());
    return;
  }
  // Parsing config
  std::ifstream ifs(config_file.c_str());
  if (!ifs) {
    RCLCPP_ERROR(rclcpp::get_logger("LoadConfig"),
                 "Read config file [%s] fail!",
                 config_file.data());
    return;
  }
  rapidjson::IStreamWrapper isw(ifs);
  rapidjson::Document document;
  document.ParseStream(isw);
  if (document.HasParseError()) {
    RCLCPP_ERROR(rclcpp::get_logger("LoadConfig"),
                 "Parsing config file %s failed",
                 config_file.data());
    return;
  }

  if (document.HasMember("class_num")){
    int class_num = document["class_num"].GetInt();
    if (InitClassNum(class_num) < 0) {
      return;
    }
  }
  if (document.HasMember("cls_names_list")) {
    std::string cls_name_file = document["cls_names_list"].GetString();
    if (InitClassNames(cls_name_file) < 0) {
      return;
    }
  }
  return;
}

void yolo5_nms(std::vector<YoloV5Result> &input,
               float iou_threshold,
               int top_k,
               std::vector<std::shared_ptr<YoloV5Result>> &result,
               bool suppress) {
  // sort order by score desc
  std::stable_sort(input.begin(), input.end(), std::greater<YoloV5Result>());

  std::vector<bool> skip(input.size(), false);

  // pre-calculate boxes area
  std::vector<float> areas;
  areas.reserve(input.size());
  for (size_t i = 0; i < input.size(); i++) {
    float width = input[i].xmax - input[i].xmin;
    float height = input[i].ymax - input[i].ymin;
    areas.push_back(width * height);
  }

  int count = 0;
  for (size_t i = 0; count < top_k && i < skip.size(); i++) {
    if (skip[i]) {
      continue;
    }
    skip[i] = true;
    ++count;

    for (size_t j = i + 1; j < skip.size(); ++j) {
      if (skip[j]) {
        continue;
      }
      if (suppress == false) {
        if (input[i].id != input[j].id) {
          continue;
        }
      }

      // intersection area
      float xx1 = std::max(input[i].xmin, input[j].xmin);
      float yy1 = std::max(input[i].ymin, input[j].ymin);
      float xx2 = std::min(input[i].xmax, input[j].xmax);
      float yy2 = std::min(input[i].ymax, input[j].ymax);

      if (xx2 > xx1 && yy2 > yy1) {
        float area_intersection = (xx2 - xx1) * (yy2 - yy1);
        float iou_ratio =
            area_intersection / (areas[j] + areas[i] - area_intersection);
        if (iou_ratio > iou_threshold) {
          skip[j] = true;
        }
      }
    }

    auto yolo_res = std::make_shared<YoloV5Result>(input[i].id,
                                                   input[i].xmin,
                                                   input[i].ymin,
                                                   input[i].xmax,
                                                   input[i].ymax,
                                                   input[i].score,
                                                   input[i].class_name);
    if (!yolo_res) {
      RCLCPP_ERROR(rclcpp::get_logger("Yolo5_detection_parser"),
                   "invalid yolo_res");
    }

    result.push_back(yolo_res);
  }
}

int get_tensor_hw(std::shared_ptr<DNNTensor> tensor, int *height, int *width) {
  int h_index = 0;
  int w_index = 0;
  if (tensor->properties.tensorLayout == HB_DNN_LAYOUT_NHWC) {
    h_index = 1;
    w_index = 2;
  } else if (tensor->properties.tensorLayout == HB_DNN_LAYOUT_NCHW) {
    h_index = 2;
    w_index = 3;
  } else {
    return -1;
  }
  *height = tensor->properties.validShape.dimensionSize[h_index];
  *width = tensor->properties.validShape.dimensionSize[w_index];
  return 0;
}

int32_t Parse(
    const std::shared_ptr<hobot::dnn_node::DnnNodeOutput> &node_output,
    std::vector<std::shared_ptr<YoloV5Result>> &results,
    const std::string &config_file) {
  std::vector<YoloV5Result> parse_results;
  if (load_config == false){
    LoadConfig(config_file);
    load_config = true;
  }
  for (size_t i = 0; i < node_output->output_tensors.size(); i++) {
    ParseTensor(
        node_output->output_tensors[i], static_cast<int>(i), parse_results);
  }
  yolo5_nms(parse_results, nms_threshold_, nms_top_k_, results, false);

  return 0;
}

}  // namespace racing_obstacle_detection
}  // namespace dnn_node
}  // namespace hobot