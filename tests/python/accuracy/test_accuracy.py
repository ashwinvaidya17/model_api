import json
import os
from pathlib import Path

import cv2
import pytest
from openvino.model_api.models import (
    AnomalyDetection,
    AnomalyResult,
    ClassificationModel,
    ClassificationResult,
    DetectionModel,
    DetectionResult,
    ImageResultWithSoftPrediction,
    InstanceSegmentationResult,
    MaskRCNNModel,
    SegmentationModel,
    add_rotated_rects,
)


def read_config(path: Path):
    with open(path, "r") as f:
        return json.load(f)


@pytest.fixture(scope="session")
def data(pytestconfig):
    return pytestconfig.getoption("data")


@pytest.fixture(scope="session")
def dump(pytestconfig):
    return pytestconfig.getoption("dump")


@pytest.fixture(scope="session")
def result(pytestconfig):
    return pytestconfig.test_results


@pytest.mark.parametrize(
    ("model_data"), read_config(Path(__file__).resolve().parent / "public_scope.json")
)
def test_image_models(data, dump, result, model_data):
    name = model_data["name"]
    if name.endswith(".xml"):
        name = f"{data}/{name}"
    model = eval(model_data["type"]).create_model(name, device="CPU", download_dir=data)

    if dump:
        result.append(model_data)
        inference_results = []

    for test_data in model_data["test_data"]:
        image_path = Path(data) / test_data["image"]
        image = cv2.imread(str(image_path))
        if image is None:
            raise RuntimeError("Failed to read the image")
        outputs = model(image)
        if isinstance(outputs, ClassificationResult):
            assert 1 == len(test_data["reference"])
            output_str = str(outputs)
            assert test_data["reference"][0] == output_str
            image_result = [output_str]
        elif isinstance(outputs, DetectionResult):
            assert 1 == len(
                test_data["reference"]
            )  # TODO: make "reference" to be a single element after SegmentationModel is updated
            output_str = str(outputs)
            assert test_data["reference"][0] == output_str
            image_result = [output_str]
        elif isinstance(outputs, ImageResultWithSoftPrediction):
            assert 1 == len(test_data["reference"])
            contours = model.get_contours(outputs.resultImage, outputs.soft_prediction)
            contour_str = "; "
            for contour in contours:
                contour_str += str(contour) + ", "
            output_str = str(outputs) + contour_str
            assert test_data["reference"][0] == output_str
            image_result = [output_str]
        elif isinstance(outputs, InstanceSegmentationResult):
            assert 1 == len(test_data["reference"])
            output_str = str(
                InstanceSegmentationResult(
                    add_rotated_rects(outputs.segmentedObjects),
                    outputs.saliency_map,
                    outputs.feature_vector,
                )
            )
            assert test_data["reference"][0] == output_str
            image_result = [output_str]
        elif isinstance(outputs, AnomalyResult):
            assert 1 == len(test_data["reference"])
            output_str = (
                f"anomaly_map min:{min(outputs.anomaly_map.flatten())} max:{max(outputs.anomaly_map.flatten())};"
                f"pred_score:{outputs.pred_score};"
                f"pred_label:{outputs.pred_label};"
                f"pred_mask min:{min(outputs.pred_mask.flatten())} max:{max(outputs.pred_mask.flatten())};"
            )
            assert test_data["reference"][0] == output_str
            image_result = [output_str]
        else:
            assert False
        if dump:
            inference_results.append(
                {"image": test_data["image"], "reference": image_result}
            )
    if name.endswith(".xml"):
        save_name = os.path.basename(name)
    else:
        save_name = name + ".xml"
    model.save(data + "/serialized/" + save_name)
    if dump:
        result[-1]["test_data"] = inference_results
