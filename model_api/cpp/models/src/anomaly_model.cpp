/*
  Copyright (C) 2020-2023 Intel Corporation

  Licensed under the Apache License, Version 2.0 (the "License");
  you may not use this file except in compliance with the License.
  You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.
*/

#include "models/anomaly_model.h"

#include <memory>
#include <openvino/core/any.hpp>
#include <openvino/core/model.hpp>
#include <ostream>

#include "models/image_model.h"
#include "models/input_data.h"
#include "models/internal_model_data.h"
#include "models/results.h"
#include "utils/slog.hpp"

std::string AnomalyModel::ModelType = "AnomalyDetection";

/// @brief Initializes the model from the given configuration
/// @param top_priority  Uses this as the primary source for setting the parameters
/// @param mid_priority Fallback source for setting the parameters
void AnomalyModel::init_from_config(const ov::AnyMap &top_priority, const ov::AnyMap &mid_priority)
{
  mean_values = get_from_any_maps("mean_values", top_priority, mid_priority, mean_values);
  scale_values = get_from_any_maps("scale_values", top_priority, mid_priority, scale_values);
  imageShape = get_from_any_maps("image_shape", top_priority, mid_priority, imageShape);
  imageThreshold = get_from_any_maps("image_threshold", top_priority, mid_priority, imageThreshold);
  pixelThreshold = get_from_any_maps("pixel_threshold", top_priority, mid_priority, pixelThreshold);
  normalizationScale = get_from_any_maps("normalization_scale", top_priority, mid_priority, normalizationScale);
  task = get_from_any_maps("task", top_priority, mid_priority, task);
  labels = get_from_any_maps("labels", top_priority, mid_priority, labels);
}

AnomalyModel::AnomalyModel(std::shared_ptr<ov::Model> &model,
                           const ov::AnyMap &configuration)
    : ImageModel(model, configuration)
{
  init_from_config(configuration, model->get_rt_info<ov::AnyMap>("model_info"));
}

AnomalyModel::AnomalyModel(std::shared_ptr<InferenceAdapter> &adapter)
    : ImageModel(adapter)
{
  init_from_config(adapter->getModelConfig(), ov::AnyMap{});
}

std::unique_ptr<AnomalyResult> AnomalyModel::infer(
    const ImageInputData &inputData)
{
  auto result = ModelBase::infer(static_cast<const InputData &>(inputData));

  return std::unique_ptr<AnomalyResult>(
      static_cast<AnomalyResult *>(result.release()));
}

std::unique_ptr<ResultBase> AnomalyModel::postprocess(
    InferenceResult &infResult)
{
  ov::Tensor predictions = infResult.outputsData[outputNames[0]];
  const auto &inputImgSize =
      infResult.internalModelData->asRef<InternalImageModelData>();

  double pred_score;
  std::string pred_label;
  cv::Mat anomaly_map;
  cv::Mat pred_mask;
  std::vector<std::vector<int>> pred_boxes;
  if (predictions.get_shape().size() == 1)
  {
    pred_score = predictions.data<float>()[0];
  }
  else
  {
    anomaly_map = cv::Mat(imageShape[0], imageShape[1], CV_32FC1,
                          predictions.data<float>());
    // find the max predicted score
    cv::minMaxLoc(anomaly_map, NULL, &pred_score);
  }
  pred_label = labels[pred_score > imageThreshold ? 1 : 0];

  if (task == "segmentation" || task == "detection")
  {
    pred_mask = anomaly_map >= pixelThreshold;
    pred_mask.convertTo(pred_mask, CV_8UC1, 1/255.);
    anomaly_map = normalize(anomaly_map, pixelThreshold);
  }
  pred_score = normalize(pred_score, imageThreshold);
  if (!anomaly_map.empty())
  {
    cv::resize(
        anomaly_map, anomaly_map,
        cv::Size{inputImgSize.inputImgWidth, inputImgSize.inputImgHeight});
    cv::resize(
        pred_mask, pred_mask,
        cv::Size{inputImgSize.inputImgWidth, inputImgSize.inputImgHeight});
  }
  if (task == "detection")
  {
    pred_boxes = getBoxes(pred_mask);
  }

  AnomalyResult *result =
      new AnomalyResult(infResult.frameId, infResult.metaData);
  result->anomaly_map = anomaly_map;
  result->pred_score = pred_score;
  result->pred_label = pred_label;
  result->pred_mask = pred_mask;
  result->pred_boxes = pred_boxes;
  return std::unique_ptr<ResultBase>(result);
}

cv::Mat AnomalyModel::normalize(cv::Mat &tensor, float threshold)
{
  cv::Mat normalized = ((tensor - threshold) / normalizationScale) + 0.5f;
  normalized = cv::min(cv::max(normalized, 0.f), 1.f);
  return normalized;
}

double AnomalyModel::normalize(double &value, float threshold)
{
  double normalized = ((value - threshold) / normalizationScale) + 0.5f;
  return std::min(std::max(normalized, 0.), 1.);
}

std::vector<std::vector<int>> AnomalyModel::getBoxes(cv::Mat &mask)
{
  std::vector<std::vector<int>> boxes;
  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(mask, contours, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);
  for (auto &contour : contours)
  {
    std::vector<int> box;
    cv::Rect rect = cv::boundingRect(contour);
    box.push_back(rect.x);
    box.push_back(rect.y);
    box.push_back(rect.x + rect.width);
    box.push_back(rect.y + rect.height);
    boxes.push_back(box);
  }
  return boxes;
}

std::unique_ptr<AnomalyModel> AnomalyModel::create_model(
    const std::string &modelFile, const ov::AnyMap &configuration,
    bool preload, const std::string &device)
{
  auto core = ov::Core();
  std::shared_ptr<ov::Model> model = core.read_model(modelFile);

  std::unique_ptr<AnomalyModel> anomalyModel {new AnomalyModel(model, configuration)};

  anomalyModel->prepare();
  if (preload)
  {
    anomalyModel->load(core, device);
  }
  return anomalyModel;
}

std::unique_ptr<AnomalyModel> AnomalyModel::create_model(
    std::shared_ptr<InferenceAdapter> &adapter)
{
  const ov::AnyMap &configuration = adapter->getModelConfig();
  auto model_type_iter = configuration.find("model_type");
  std::string model_type = AnomalyModel::ModelType;
  if (model_type_iter != configuration.end())
  {
    model_type = model_type_iter->second.as<std::string>();
  }

  if (model_type != AnomalyModel::ModelType)
  {
    throw std::runtime_error(
        "Incorrect or unsupported model_type is provided: " + model_type);
  }
  std::unique_ptr<AnomalyModel> anomalyModel{new AnomalyModel(adapter)};
  return anomalyModel;
}

std::ostream &operator<<(std::ostream &os,
                         std::unique_ptr<AnomalyModel> &model)
{
  os << "AnomalyModel: " << model->task
     << ", Image threshold: " << model->imageThreshold
     << ", Pixel threshold: " << model->pixelThreshold
     <<", Normalization scale: " << model->normalizationScale<<std::endl;
  return os;
}

void AnomalyModel::prepareInputsOutputs(std::shared_ptr<ov::Model> &model)
{
  const auto &input = model->input();
  inputNames.push_back(input.get_any_name());

  const ov::Shape &inputShape = input.get_partial_shape().get_max_shape();
  const ov::Layout &inputLayout = getInputLayout(input);

  if (!embedded_processing)
  {
    model = ImageModel::embedProcessing(
        model, inputNames[0], inputLayout, resizeMode, interpolationMode,
        ov::Shape{inputShape[ov::layout::width_idx(inputLayout)],
                  inputShape[ov::layout::height_idx(inputLayout)]},
        pad_value, reverse_input_channels, mean_values, scale_values);
    embedded_processing = true;
  }

  const auto &outputs = model->outputs();
  for (const auto &output : model->outputs())
  {
    outputNames.push_back(output.get_any_name());
  }
}

void AnomalyModel::updateModelInfo()
{
  ImageModel::updateModelInfo();

  model->set_rt_info(AnomalyModel::ModelType, "model_info", "model_type");
  model->set_rt_info(task, "model_info", "task");
  model->set_rt_info(imageShape, "model_info", "image_shape");
  model->set_rt_info(imageThreshold, "model_info", "image_threshold");
  model->set_rt_info(pixelThreshold, "model_info", "pixel_threshold");
  model->set_rt_info(normalizationScale, "model_info", "normalization_scale");
  model->set_rt_info(task, "model_info", "task");
}
