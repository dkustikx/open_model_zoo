// Copyright (C) 2018-2019 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <gflags/gflags.h>
#include <functional>
#include <iostream>
#include <fstream>
#include <random>
#include <memory>
#include <chrono>
#include <vector>
#include <string>
#include <utility>
#include <algorithm>
#include <iterator>
#include <map>

#include <inference_engine.hpp>
#include <ngraph/ngraph.hpp>

#include <utils/ocv_common.hpp>
#include <utils/slog.hpp>

#include "detectors.hpp"

BaseDetection::BaseDetection(const std::string &topoName,
                             const std::string &pathToModel,
                             const std::string &deviceForInference,
                             int maxBatch, bool isBatchDynamic, bool isAsync,
                             bool doRawOutputMessages)
    : topoName(topoName), pathToModel(pathToModel), deviceForInference(deviceForInference),
      maxBatch(maxBatch), isBatchDynamic(isBatchDynamic), isAsync(isAsync),
      enablingChecked(false), _enabled(false), doRawOutputMessages(doRawOutputMessages) {
    if (isAsync) {
        slog::debug << "Use async mode for " << topoName << slog::endl;
    }
}

BaseDetection::~BaseDetection() {}

InferenceEngine::ExecutableNetwork* BaseDetection::operator ->() {
    return &net;
}

void BaseDetection::submitRequest() {
    if (!enabled() || request == nullptr) return;
    if (isAsync) {
        request->StartAsync();
    } else {
        request->Infer();
    }
}

void BaseDetection::wait() {
    if (!enabled()|| !request || !isAsync)
        return;
    request->Wait(InferenceEngine::InferRequest::WaitMode::RESULT_READY);
}

bool BaseDetection::enabled() const  {
    if (!enablingChecked) {
        _enabled = !pathToModel.empty();
        if (!_enabled) {
            slog::info << topoName << " DISABLED" << slog::endl;
        }
        enablingChecked = true;
    }
    return _enabled;
}

FaceDetection::FaceDetection(const std::string &pathToModel,
                             const std::string &deviceForInference,
                             int maxBatch, bool isBatchDynamic, bool isAsync,
                             double detectionThreshold, bool doRawOutputMessages,
                             float bb_enlarge_coefficient, float bb_dx_coefficient, float bb_dy_coefficient)
    : BaseDetection("Face Detection", pathToModel, deviceForInference, maxBatch, isBatchDynamic, isAsync, doRawOutputMessages),
      detectionThreshold(detectionThreshold),
      maxProposalCount(0), objectSize(0), enquedFrames(0), width(0), height(0),
      network_input_width(0), network_input_height(0),
      bb_enlarge_coefficient(bb_enlarge_coefficient), bb_dx_coefficient(bb_dx_coefficient),
      bb_dy_coefficient(bb_dy_coefficient), resultsFetched(false) {}

void FaceDetection::submitRequest() {
    if (!enquedFrames) return;
    enquedFrames = 0;
    resultsFetched = false;
    results.clear();
    BaseDetection::submitRequest();
}

void FaceDetection::enqueue(const cv::Mat &frame) {
    if (!enabled()) return;

    if (!request) {
        request = std::make_shared<InferenceEngine::InferRequest>(net.CreateInferRequest());
    }

    width = static_cast<float>(frame.cols);
    height = static_cast<float>(frame.rows);

    InferenceEngine::Blob::Ptr  inputBlob = request->GetBlob(input);

    matToBlob(frame, inputBlob);

    enquedFrames = 1;
}

InferenceEngine::CNNNetwork FaceDetection::read(const InferenceEngine::Core& ie)  {
    /** Read network model **/
    auto network = ie.ReadNetwork(pathToModel);
    /** Set batch size to 1 **/
    network.setBatchSize(maxBatch);
    // -----------------------------------------------------------------------------------------------------

    // ---------------------------Check inputs -------------------------------------------------------------
    InferenceEngine::InputsDataMap inputInfo(network.getInputsInfo());
    if (inputInfo.size() != 1) {
        throw std::logic_error("Face Detection network should have only one input");
    }
    InferenceEngine::InputInfo::Ptr inputInfoFirst = inputInfo.begin()->second;
    inputInfoFirst->setPrecision(InferenceEngine::Precision::U8);

    const InferenceEngine::SizeVector inputDims = inputInfoFirst->getTensorDesc().getDims();
    network_input_height = inputDims[2];
    network_input_width = inputDims[3];

    // -----------------------------------------------------------------------------------------------------

    // ---------------------------Check outputs ------------------------------------------------------------
    InferenceEngine::OutputsDataMap outputInfo(network.getOutputsInfo());
    if (outputInfo.size() == 1) {
        InferenceEngine::DataPtr& _output = outputInfo.begin()->second;
        output = outputInfo.begin()->first;
        const InferenceEngine::SizeVector outputDims = _output->getTensorDesc().getDims();
        maxProposalCount = outputDims[2];
        objectSize = outputDims[3];
        if (objectSize != 7) {
            throw std::logic_error("Face Detection network output layer should have 7 as a last dimension");
        }
        if (outputDims.size() != 4) {
            throw std::logic_error("Face Detection network output should have 4 dimentions, but had " +
                                   std::to_string(outputDims.size()));
        }
        _output->setPrecision(InferenceEngine::Precision::FP32);
    } else {
        for (const auto& outputLayer: outputInfo) {
            const InferenceEngine::SizeVector outputDims = outputLayer.second->getTensorDesc().getDims();
            if (outputDims.size() == 2 && outputDims.back() == 5) {
                output = outputLayer.first;
                maxProposalCount = outputDims[0];
                objectSize = outputDims.back();
                outputLayer.second->setPrecision(InferenceEngine::Precision::FP32);
            } else if (outputDims.size() == 1 && outputLayer.second->getPrecision() == InferenceEngine::Precision::I32) {
                labels_output = outputLayer.first;
            }
        }
        if (output.empty() || labels_output.empty()) {
            throw std::logic_error("Face Detection network must contain either single DetectionOutput or "
                                   "'boxes' [nx5] and 'labels' [n] at least, where 'n' is a number of detected objects.");
        }
    }

    input = inputInfo.begin()->first;
    return network;
}

void FaceDetection::fetchResults() {
    if (!enabled()) return;
    results.clear();
    if (resultsFetched) return;
    resultsFetched = true;
    InferenceEngine::LockedMemory<const void> outputMapped =
        InferenceEngine::as<InferenceEngine::MemoryBlob>(request->GetBlob(output))->rmap();
    const float *detections = outputMapped.as<float *>();
    if (!labels_output.empty()) {
        InferenceEngine::LockedMemory<const void> labelsMapped =
            InferenceEngine::as<InferenceEngine::MemoryBlob>(request->GetBlob(labels_output))->rmap();
        const int32_t *labels = labelsMapped.as<int32_t *>();

        for (int i = 0; i < maxProposalCount && objectSize == 5; i++) {
            Result r;
            r.label = labels[i];
            r.confidence = detections[i * objectSize + 4];

            if (r.confidence <= detectionThreshold && !doRawOutputMessages) {
                continue;
            }

            r.location.x = static_cast<int>(detections[i * objectSize + 0] / network_input_width * width);
            r.location.y = static_cast<int>(detections[i * objectSize + 1] / network_input_height * height);
            r.location.width = static_cast<int>(detections[i * objectSize + 2] / network_input_width * width - r.location.x);
            r.location.height = static_cast<int>(detections[i * objectSize + 3] / network_input_height * height - r.location.y);

            // Make square and enlarge face bounding box for more robust operation of face analytics networks
            int bb_width = r.location.width;
            int bb_height = r.location.height;

            int bb_center_x = r.location.x + bb_width / 2;
            int bb_center_y = r.location.y + bb_height / 2;

            int max_of_sizes = std::max(bb_width, bb_height);

            int bb_new_width = static_cast<int>(bb_enlarge_coefficient * max_of_sizes);
            int bb_new_height = static_cast<int>(bb_enlarge_coefficient * max_of_sizes);

            r.location.x = bb_center_x - static_cast<int>(std::floor(bb_dx_coefficient * bb_new_width / 2));
            r.location.y = bb_center_y - static_cast<int>(std::floor(bb_dy_coefficient * bb_new_height / 2));

            r.location.width = bb_new_width;
            r.location.height = bb_new_height;

            if (doRawOutputMessages) {
                slog::debug << "[" << i << "," << r.label << "] element, prob = " << r.confidence <<
                             "    (" << r.location.x << "," << r.location.y << ")-(" << r.location.width << ","
                          << r.location.height << ")"
                          << ((r.confidence > detectionThreshold) ? " WILL BE RENDERED!" : "") << slog::endl;
            }
            if (r.confidence > detectionThreshold) {
                results.push_back(r);
            }
        }
    }

    for (int i = 0; i < maxProposalCount && objectSize == 7; i++) {
        float image_id = detections[i * objectSize + 0];
        if (image_id < 0) {
            break;
        }
        Result r;
        r.label = static_cast<int>(detections[i * objectSize + 1]);
        r.confidence = detections[i * objectSize + 2];

        if (r.confidence <= detectionThreshold && !doRawOutputMessages) {
            continue;
        }

        r.location.x = static_cast<int>(detections[i * objectSize + 3] * width);
        r.location.y = static_cast<int>(detections[i * objectSize + 4] * height);
        r.location.width = static_cast<int>(detections[i * objectSize + 5] * width - r.location.x);
        r.location.height = static_cast<int>(detections[i * objectSize + 6] * height - r.location.y);

        // Make square and enlarge face bounding box for more robust operation of face analytics networks
        int bb_width = r.location.width;
        int bb_height = r.location.height;

        int bb_center_x = r.location.x + bb_width / 2;
        int bb_center_y = r.location.y + bb_height / 2;

        int max_of_sizes = std::max(bb_width, bb_height);

        int bb_new_width = static_cast<int>(bb_enlarge_coefficient * max_of_sizes);
        int bb_new_height = static_cast<int>(bb_enlarge_coefficient * max_of_sizes);

        r.location.x = bb_center_x - static_cast<int>(std::floor(bb_dx_coefficient * bb_new_width / 2));
        r.location.y = bb_center_y - static_cast<int>(std::floor(bb_dy_coefficient * bb_new_height / 2));

        r.location.width = bb_new_width;
        r.location.height = bb_new_height;

        if (doRawOutputMessages) {
            slog::debug << "[" << i << "," << r.label << "] element, prob = " << r.confidence <<
                         "    (" << r.location.x << "," << r.location.y << ")-(" << r.location.width << ","
                      << r.location.height << ")"
                      << ((r.confidence > detectionThreshold) ? " WILL BE RENDERED!" : "") << slog::endl;
        }
        if (r.confidence > detectionThreshold) {
            results.push_back(r);
        }
    }
}

AntispoofingClassifier::AntispoofingClassifier(const std::string& pathToModel,
    const std::string& deviceForInference,
    int maxBatch, bool isBatchDynamic, bool isAsync, bool doRawOutputMessages)
    : BaseDetection("Antispoofing", pathToModel, deviceForInference, maxBatch, isBatchDynamic, isAsync, doRawOutputMessages),
    enquedFaces(0) {
}

void AntispoofingClassifier::submitRequest() {
    if (!enquedFaces)
        return;
    if (isBatchDynamic) {
        request->SetBatch(enquedFaces);
    }
    BaseDetection::submitRequest();
    enquedFaces = 0;
}

void AntispoofingClassifier::enqueue(const cv::Mat& face) {
    if (!enabled()) {
        return;
    }
    if (enquedFaces == maxBatch) {
        slog::warn << "Number of detected faces more than maximum(" << maxBatch << ") processed by Antispoofing Classifier network" << slog::endl;
        return;
    }
    if (!request) {
        request = std::make_shared<InferenceEngine::InferRequest>(net.CreateInferRequest());
    }

    InferenceEngine::Blob::Ptr  inputBlob = request->GetBlob(input);

    matToBlob(face, inputBlob, enquedFaces);

    enquedFaces++;
}

float AntispoofingClassifier::operator[] (int idx) const {
    InferenceEngine::Blob::Ptr  ProbBlob = request->GetBlob(prob_output);
    InferenceEngine::LockedMemory<const void> ProbBlobMapped =
        InferenceEngine::as<InferenceEngine::MemoryBlob>(ProbBlob)->rmap();
    // use prediction for real face only
    float r = ProbBlobMapped.as<float*>()[2 * idx] * 100;
    if (doRawOutputMessages) {
        slog::debug << "[" << idx << "] element, real face probability = " << r << slog::endl;
    }

    return r;
}

InferenceEngine::CNNNetwork AntispoofingClassifier::read(const InferenceEngine::Core& ie) {
    // Read network
    auto network = ie.ReadNetwork(pathToModel);
    // Set maximum batch size to be used.
    network.setBatchSize(maxBatch);

    // ---------------------------Check inputs -------------------------------------------------------------
    // Antispoofing Classifier network should have one input and one output
    InferenceEngine::InputsDataMap inputInfo(network.getInputsInfo());
    if (inputInfo.size() != 1) {
        throw std::logic_error("Antispoofing Classifier network should have only one input");
    }
    InferenceEngine::InputInfo::Ptr& inputInfoFirst = inputInfo.begin()->second;
    inputInfoFirst->setPrecision(InferenceEngine::Precision::U8);
    input = inputInfo.begin()->first;
    // -----------------------------------------------------------------------------------------------------

    // ---------------------------Check outputs ------------------------------------------------------------
    InferenceEngine::OutputsDataMap outputInfo(network.getOutputsInfo());
    if (outputInfo.size() != 1) {
        throw std::logic_error("Antispoofing Classifier network should have one output layers");
    }
    auto it = outputInfo.begin();

    InferenceEngine::DataPtr ptrProbOutput = (it++)->second;

    prob_output = ptrProbOutput->getName();

    _enabled = true;
    return network;
}

AgeGenderDetection::AgeGenderDetection(const std::string &pathToModel,
                                       const std::string &deviceForInference,
                                       int maxBatch, bool isBatchDynamic, bool isAsync, bool doRawOutputMessages)
    : BaseDetection("Age/Gender Recognition", pathToModel, deviceForInference, maxBatch, isBatchDynamic, isAsync, doRawOutputMessages),
      enquedFaces(0) {
}

void AgeGenderDetection::submitRequest()  {
    if (!enquedFaces)
        return;
    if (isBatchDynamic) {
        request->SetBatch(enquedFaces);
    }
    BaseDetection::submitRequest();
    enquedFaces = 0;
}

void AgeGenderDetection::enqueue(const cv::Mat &face) {
    if (!enabled()) {
        return;
    }
    if (enquedFaces == maxBatch) {
        slog::warn << "Number of detected faces more than maximum(" << maxBatch << ") processed by Age/Gender Recognition network" << slog::endl;
        return;
    }
    if (!request) {
        request = std::make_shared<InferenceEngine::InferRequest>(net.CreateInferRequest());
    }

    InferenceEngine::Blob::Ptr  inputBlob = request->GetBlob(input);
    matToBlob(face, inputBlob, enquedFaces);

    enquedFaces++;
}

AgeGenderDetection::Result AgeGenderDetection::operator[] (int idx) const {
    InferenceEngine::Blob::Ptr  genderBlob = request->GetBlob(outputGender);
    InferenceEngine::Blob::Ptr  ageBlob    = request->GetBlob(outputAge);

    InferenceEngine::LockedMemory<const void> ageBlobMapped =
        InferenceEngine::as<InferenceEngine::MemoryBlob>(ageBlob)->rmap();
    InferenceEngine::LockedMemory<const void> genderBlobMapped =
        InferenceEngine::as<InferenceEngine::MemoryBlob>(genderBlob)->rmap();
    AgeGenderDetection::Result r = {ageBlobMapped.as<float*>()[idx] * 100,
                                    genderBlobMapped.as<float*>()[idx * 2 + 1]};
    if (doRawOutputMessages) {
        slog::debug << "[" << idx << "] element, male prob = " << r.maleProb << ", age = " << r.age << slog::endl;
    }

    return r;
}

InferenceEngine::CNNNetwork AgeGenderDetection::read(const InferenceEngine::Core& ie) {
    // Read network
    auto network = ie.ReadNetwork(pathToModel);
    // Set maximum batch size to be used.
    network.setBatchSize(maxBatch);

    // ---------------------------Check inputs -------------------------------------------------------------
    // Age/Gender Recognition network should have one input and two outputs
    InferenceEngine::InputsDataMap inputInfo(network.getInputsInfo());
    if (inputInfo.size() != 1) {
        throw std::logic_error("Age/Gender Recognition network should have only one input");
    }
    InferenceEngine::InputInfo::Ptr& inputInfoFirst = inputInfo.begin()->second;
    inputInfoFirst->setPrecision(InferenceEngine::Precision::U8);
    input = inputInfo.begin()->first;
    // -----------------------------------------------------------------------------------------------------

    // ---------------------------Check outputs ------------------------------------------------------------
    InferenceEngine::OutputsDataMap outputInfo(network.getOutputsInfo());
    if (outputInfo.size() != 2) {
        throw std::logic_error("Age/Gender Recognition network should have two output layers");
    }
    auto it = outputInfo.begin();

    InferenceEngine::DataPtr ptrAgeOutput = (it++)->second;
    InferenceEngine::DataPtr ptrGenderOutput = (it++)->second;

    outputAge = ptrAgeOutput->getName();
    outputGender = ptrGenderOutput->getName();

    _enabled = true;
    return network;
}


HeadPoseDetection::HeadPoseDetection(const std::string &pathToModel,
                                     const std::string &deviceForInference,
                                     int maxBatch, bool isBatchDynamic, bool isAsync, bool doRawOutputMessages)
    : BaseDetection("Head Pose Estimation", pathToModel, deviceForInference, maxBatch, isBatchDynamic, isAsync, doRawOutputMessages),
      outputAngleR("angle_r_fc"), outputAngleP("angle_p_fc"), outputAngleY("angle_y_fc"), enquedFaces(0) {
}

void HeadPoseDetection::submitRequest()  {
    if (!enquedFaces) return;
    if (isBatchDynamic) {
        request->SetBatch(enquedFaces);
    }
    BaseDetection::submitRequest();
    enquedFaces = 0;
}

void HeadPoseDetection::enqueue(const cv::Mat &face) {
    if (!enabled()) {
        return;
    }
    if (enquedFaces == maxBatch) {
        slog::warn << "Number of detected faces more than maximum(" << maxBatch << ") processed by Head Pose estimator" << slog::endl;
        return;
    }
    if (!request) {
        request = std::make_shared<InferenceEngine::InferRequest>(net.CreateInferRequest());
    }

    InferenceEngine::Blob::Ptr inputBlob = request->GetBlob(input);

    matToBlob(face, inputBlob, enquedFaces);

    enquedFaces++;
}

HeadPoseDetection::Results HeadPoseDetection::operator[] (int idx) const {
    InferenceEngine::Blob::Ptr  angleR = request->GetBlob(outputAngleR);
    InferenceEngine::Blob::Ptr  angleP = request->GetBlob(outputAngleP);
    InferenceEngine::Blob::Ptr  angleY = request->GetBlob(outputAngleY);

    InferenceEngine::LockedMemory<const void> angleRMapped =
        InferenceEngine::as<InferenceEngine::MemoryBlob>(angleR)->rmap();
    InferenceEngine::LockedMemory<const void> anglePMapped =
        InferenceEngine::as<InferenceEngine::MemoryBlob>(angleP)->rmap();
    InferenceEngine::LockedMemory<const void> angleYMapped =
        InferenceEngine::as<InferenceEngine::MemoryBlob>(angleY)->rmap();
    HeadPoseDetection::Results r = {angleRMapped.as<float*>()[idx],
                                    anglePMapped.as<float*>()[idx],
                                    angleYMapped.as<float*>()[idx]};

    if (doRawOutputMessages) {
        slog::debug << "[" << idx << "] element, yaw = " << r.angle_y <<
                     ", pitch = " << r.angle_p <<
                     ", roll = " << r.angle_r << slog::endl;
    }

    return r;
}

InferenceEngine::CNNNetwork HeadPoseDetection::read(const InferenceEngine::Core& ie) {
    // Read network model
    auto network = ie.ReadNetwork(pathToModel);
    // Set maximum batch size
    network.setBatchSize(maxBatch);

    // ---------------------------Check inputs -------------------------------------------------------------
    InferenceEngine::InputsDataMap inputInfo(network.getInputsInfo());
    if (inputInfo.size() != 1) {
        throw std::logic_error("Head Pose Estimation network should have only one input");
    }
    InferenceEngine::InputInfo::Ptr& inputInfoFirst = inputInfo.begin()->second;
    inputInfoFirst->setPrecision(InferenceEngine::Precision::U8);
    input = inputInfo.begin()->first;
    // -----------------------------------------------------------------------------------------------------

    // ---------------------------Check outputs ------------------------------------------------------------
    InferenceEngine::OutputsDataMap outputInfo(network.getOutputsInfo());
    for (auto& output : outputInfo) {
        output.second->setPrecision(InferenceEngine::Precision::FP32);
    }
    for (const std::string& outName : {outputAngleR, outputAngleP, outputAngleY}) {
        if (outputInfo.find(outName) == outputInfo.end()) {
            throw std::logic_error("There is no " + outName + " output in Head Pose Estimation network");
        }
    }

    _enabled = true;
    return network;
}

EmotionsDetection::EmotionsDetection(const std::string &pathToModel,
                                     const std::string &deviceForInference,
                                     int maxBatch, bool isBatchDynamic, bool isAsync, bool doRawOutputMessages)
              : BaseDetection("Emotions Recognition", pathToModel, deviceForInference, maxBatch, isBatchDynamic, isAsync, doRawOutputMessages),
                enquedFaces(0) {
}

void EmotionsDetection::submitRequest() {
    if (!enquedFaces) return;
    if (isBatchDynamic) {
        request->SetBatch(enquedFaces);
    }
    BaseDetection::submitRequest();
    enquedFaces = 0;
}

void EmotionsDetection::enqueue(const cv::Mat &face) {
    if (!enabled()) {
        return;
    }
    if (enquedFaces == maxBatch) {
        slog::warn << "Number of detected faces more than maximum(" << maxBatch << ") processed by Emotions Recognition network" << slog::endl;
        return;
    }
    if (!request) {
        request = std::make_shared<InferenceEngine::InferRequest>(net.CreateInferRequest());
    }

    InferenceEngine::Blob::Ptr inputBlob = request->GetBlob(input);

    matToBlob(face, inputBlob, enquedFaces);

    enquedFaces++;
}

std::map<std::string, float> EmotionsDetection::operator[] (int idx) const {
    auto emotionsVecSize = emotionsVec.size();

    InferenceEngine::Blob::Ptr emotionsBlob = request->GetBlob(outputEmotions);

    /* emotions vector must have the same size as number of channels
     * in model output. Default output format is NCHW, so index 1 is checked */
    size_t numOfChannels = emotionsBlob->getTensorDesc().getDims().at(1);
    if (numOfChannels != emotionsVecSize) {
        throw std::logic_error("Output size (" + std::to_string(numOfChannels) +
                               ") of the Emotions Recognition network is not equal "
                               "to used emotions vector size (" +
                               std::to_string(emotionsVec.size()) + ")");
    }

    InferenceEngine::LockedMemory<const void> emotionsBlobMapped =
        InferenceEngine::as<InferenceEngine::MemoryBlob>(emotionsBlob)->rmap();
    auto emotionsValues = emotionsBlobMapped.as<float *>();
    auto outputIdxPos = emotionsValues + idx * emotionsVecSize;
    std::map<std::string, float> emotions;

    if (doRawOutputMessages) {
        slog::debug << "[" << idx << "] element, predicted emotions (name = prob):" << slog::endl;
    }

    for (size_t i = 0; i < emotionsVecSize; i++) {
        emotions[emotionsVec[i]] = outputIdxPos[i];

        if (doRawOutputMessages) {
            slog::debug << emotionsVec[i] << " = " << outputIdxPos[i];
            if (emotionsVecSize - 1 != i) {
                slog::debug << ", ";
            } else {
                slog::debug << slog::endl;
            }
        }
    }

    return emotions;
}

InferenceEngine::CNNNetwork EmotionsDetection::read(const InferenceEngine::Core& ie) {
    // Read network model
    auto network = ie.ReadNetwork(pathToModel);
    // Set maximum batch size
    network.setBatchSize(maxBatch);

    // -----------------------------------------------------------------------------------------------------
    // Emotions Recognition network should have one input and one output.
    // ---------------------------Check inputs -------------------------------------------------------------
    InferenceEngine::InputsDataMap inputInfo(network.getInputsInfo());
    if (inputInfo.size() != 1) {
        throw std::logic_error("Emotions Recognition network should have only one input");
    }
    auto& inputInfoFirst = inputInfo.begin()->second;
    inputInfoFirst->setPrecision(InferenceEngine::Precision::U8);
    input = inputInfo.begin()->first;
    // -----------------------------------------------------------------------------------------------------

    // ---------------------------Check outputs ------------------------------------------------------------
    InferenceEngine::OutputsDataMap outputInfo(network.getOutputsInfo());
    if (outputInfo.size() != 1) {
        throw std::logic_error("Emotions Recognition network should have one output layer");
    }
    for (auto& output : outputInfo) {
        output.second->setPrecision(InferenceEngine::Precision::FP32);
    }

    outputEmotions = outputInfo.begin()->first;

    _enabled = true;
    return network;
}


FacialLandmarksDetection::FacialLandmarksDetection(const std::string &pathToModel,
                                                   const std::string &deviceForInference,
                                                   int maxBatch, bool isBatchDynamic, bool isAsync, bool doRawOutputMessages)
    : BaseDetection("Facial Landmarks Estimation", pathToModel, deviceForInference, maxBatch, isBatchDynamic, isAsync, doRawOutputMessages),
      outputFacialLandmarksBlobName("align_fc3"), enquedFaces(0) {
}

void FacialLandmarksDetection::submitRequest() {
    if (!enquedFaces) return;
    if (isBatchDynamic) {
        request->SetBatch(enquedFaces);
    }
    BaseDetection::submitRequest();
    enquedFaces = 0;
}

void FacialLandmarksDetection::enqueue(const cv::Mat &face) {
    if (!enabled()) {
        return;
    }
    if (enquedFaces == maxBatch) {
        slog::warn << "Number of detected faces more than maximum(" << maxBatch << ") processed by Facial Landmarks estimator" << slog::endl;
        return;
    }
    if (!request) {
        request = std::make_shared<InferenceEngine::InferRequest>(net.CreateInferRequest());
    }

    InferenceEngine::Blob::Ptr inputBlob = request->GetBlob(input);

    matToBlob(face, inputBlob, enquedFaces);

    enquedFaces++;
}

std::vector<float> FacialLandmarksDetection::operator[] (int idx) const {
    std::vector<float> normedLandmarks;

    auto landmarksBlob = request->GetBlob(outputFacialLandmarksBlobName);
    auto n_lm = getTensorChannels(landmarksBlob->getTensorDesc());
    InferenceEngine::LockedMemory<const void> facialLandmarksBlobMapped =
        InferenceEngine::as<InferenceEngine::MemoryBlob>(request->GetBlob(outputFacialLandmarksBlobName))->rmap();
    const float *normed_coordinates = facialLandmarksBlobMapped.as<float *>();

    if (doRawOutputMessages) {
        slog::debug << "[" << idx << "] element, normed facial landmarks coordinates (x, y):" << slog::endl;
    }

    auto begin = n_lm / 2 * idx;
    auto end = begin + n_lm / 2;
    for (auto i_lm = begin; i_lm < end; ++i_lm) {
        float normed_x = normed_coordinates[2 * i_lm];
        float normed_y = normed_coordinates[2 * i_lm + 1];

        if (doRawOutputMessages) {
            slog::debug <<'\t' << normed_x << ", " << normed_y << slog::endl;
        }

        normedLandmarks.push_back(normed_x);
        normedLandmarks.push_back(normed_y);
    }

    return normedLandmarks;
}

InferenceEngine::CNNNetwork FacialLandmarksDetection::read(const InferenceEngine::Core& ie) {
    // Read network model
    auto network = ie.ReadNetwork(pathToModel);
    // Set maximum batch size
    network.setBatchSize(maxBatch);

    // ---------------------------Check inputs -------------------------------------------------------------
    InferenceEngine::InputsDataMap inputInfo(network.getInputsInfo());
    if (inputInfo.size() != 1) {
        throw std::logic_error("Facial Landmarks Estimation network should have only one input");
    }
    InferenceEngine::InputInfo::Ptr& inputInfoFirst = inputInfo.begin()->second;
    inputInfoFirst->setPrecision(InferenceEngine::Precision::U8);
    input = inputInfo.begin()->first;
    // -----------------------------------------------------------------------------------------------------

    // ---------------------------Check outputs ------------------------------------------------------------
    InferenceEngine::OutputsDataMap outputInfo(network.getOutputsInfo());
    const std::string outName = outputInfo.begin()->first;
    if (outName != outputFacialLandmarksBlobName) {
        throw std::logic_error("Facial Landmarks Estimation network output layer unknown: " + outName
                               + ", should be " + outputFacialLandmarksBlobName);
    }
    InferenceEngine::Data& data = *outputInfo.begin()->second;
    data.setPrecision(InferenceEngine::Precision::FP32);
    const InferenceEngine::SizeVector& outSizeVector = data.getTensorDesc().getDims();
    if (outSizeVector.size() != 2 && outSizeVector.back() != 70) {
        throw std::logic_error("Facial Landmarks Estimation network output layer should have 2 dimensions and 70 as"
                               " the last dimension");
    }

    _enabled = true;
    return network;
}


Load::Load(BaseDetection& detector) : detector(detector) {
}

void Load::into(InferenceEngine::Core & ie, const std::string & deviceName, bool enable_dynamic_batch) const {
    if (detector.enabled()) {
        std::map<std::string, std::string> config = { };
        bool isPossibleDynBatch = deviceName.find("CPU") != std::string::npos ||
                                  deviceName.find("GPU") != std::string::npos;

        if (enable_dynamic_batch && isPossibleDynBatch) {
            config[InferenceEngine::PluginConfigParams::KEY_DYN_BATCH_ENABLED] = InferenceEngine::PluginConfigParams::YES;
        }

        detector.net = ie.LoadNetwork(detector.read(ie), deviceName, config);
        logExecNetworkInfo(detector.net, detector.pathToModel, deviceName, detector.topoName);
        slog::info << "\tBatch size is set to " << detector.maxBatch << slog::endl;
    }
}


CallStat::CallStat():
    _number_of_calls(0), _total_duration(0.0), _last_call_duration(0.0), _smoothed_duration(-1.0) {
}

double CallStat::getSmoothedDuration() {
    // Additional check is needed for the first frame while duration of the first
    // visualisation is not calculated yet.
    if (_smoothed_duration < 0) {
        auto t = std::chrono::steady_clock::now();
        return std::chrono::duration_cast<ms>(t - _last_call_start).count();
    }
    return _smoothed_duration;
}

double CallStat::getTotalDuration() {
    return _total_duration;
}

double CallStat::getLastCallDuration() {
    return _last_call_duration;
}

void CallStat::calculateDuration() {
    auto t = std::chrono::steady_clock::now();
    _last_call_duration = std::chrono::duration_cast<ms>(t - _last_call_start).count();
    _number_of_calls++;
    _total_duration += _last_call_duration;
    if (_smoothed_duration < 0) {
        _smoothed_duration = _last_call_duration;
    }
    double alpha = 0.1;
    _smoothed_duration = _smoothed_duration * (1.0 - alpha) + _last_call_duration * alpha;
    _last_call_start = t;
}

void CallStat::setStartTime() {
    _last_call_start = std::chrono::steady_clock::now();
}

void Timer::start(const std::string& name) {
    if (_timers.find(name) == _timers.end()) {
        _timers[name] = CallStat();
    }
    _timers[name].setStartTime();
}

void Timer::finish(const std::string& name) {
    auto& timer = (*this)[name];
    timer.calculateDuration();
}

CallStat& Timer::operator[](const std::string& name) {
    if (_timers.find(name) == _timers.end()) {
        throw std::logic_error("No timer with name " + name + ".");
    }
    return _timers[name];
}
