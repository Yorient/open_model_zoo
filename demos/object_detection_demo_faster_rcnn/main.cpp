// Copyright (C) 2018-2019 Intel Corporation
// SPDX-License-Identifier: Apache-2.0
//

#include <gflags/gflags.h>
#include <algorithm>
#include <iostream>
#include <string>
#include <memory>
#include <vector>
#include <limits>

#include <inference_engine.hpp>
#ifdef WITH_EXTENSIONS
#include <ext_list.hpp>
#endif

#include <opencv2/core.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <samples/ocv_common.hpp>
#include <samples/slog.hpp>
#include <samples/args_helper.hpp>
#include "object_detection_demo_faster_rcnn.h"
#include "detectionoutput.h"

using namespace InferenceEngine;

ConsoleErrorListener error_listener;

bool ParseAndCheckCommandLine(int argc, char *argv[]) {
    // ---------------------------Parsing and validation of input args--------------------------------------

    gflags::ParseCommandLineNonHelpFlags(&argc, &argv, true);
    if (FLAGS_h) {
        showUsage();
        showAvailableDevices();
        return false;
    }

    slog::info << "Parsing input parameters" << slog::endl;

    if (FLAGS_i.empty()) {
        throw std::logic_error("Parameter -i is not set");
    }

    if (FLAGS_m.empty()) {
        throw std::logic_error("Parameter -m is not set");
    }

    return true;
}

/**
* \brief The entry point for the Inference Engine object_detection demo Faster RCNN application
* \file object_detection_demo_faster_rcnn/main.cpp
* \example object_detection_demo_faster_rcnn/main.cpp
*/
int main(int argc, char *argv[]) {
    try {
        /** This demo covers certain topology and cannot be generalized for any object detection one **/
        slog::info << "InferenceEngine: " << GetInferenceEngineVersion() << "\n";

        // ------------------------------ Parsing and validation of input args ---------------------------------
        if (!ParseAndCheckCommandLine(argc, argv)) {
            return 0;
        }

        /** This vector stores paths to the processed images **/
        std::vector<std::string> imagePaths;
        parseInputFilesArguments(imagePaths);
        if (imagePaths.empty()) throw std::logic_error("No suitable images were found");
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 1. Load inference engine -------------------------------------
        slog::info << "Loading Inference Engine" << slog::endl;
        Core ie;

        if (FLAGS_p_msg) {
            ie.SetLogCallback(error_listener);
        }

#ifdef WITH_EXTENSIONS
        /*If CPU device, load default library with extensions that comes with the product*/
        if (FLAGS_d.find("CPU") != std::string::npos) {
            /**
            * cpu_extensions library is compiled from "extension" folder containing
            * custom MKLDNNPlugin layer implementations. These layers are not supported
            * by mkldnn, but they can be useful for inferencing custom topologies.
            **/
            ie.AddExtension(std::make_shared<Extensions::Cpu::CpuExtensions>(), "CPU");
        }
#endif

        if (!FLAGS_l.empty()) {
            // CPU(MKLDNN) extensions are loaded as a shared library and passed as a pointer to base extension
            IExtensionPtr extension_ptr = make_so_pointer<IExtension>(FLAGS_l);
            ie.AddExtension(extension_ptr, "CPU");
            slog::info << "CPU Extension loaded: " << FLAGS_l << slog::endl;
        }

        if (!FLAGS_c.empty()) {
            // clDNN Extensions are loaded from an .xml description and OpenCL kernel files
            ie.SetConfig({ { PluginConfigParams::KEY_CONFIG_FILE, FLAGS_c } }, "GPU");
            slog::info << "GPU Extension loaded: " << FLAGS_c << slog::endl;
        }

        /** Printing plugin version **/
        slog::info << "Device info: " << slog::endl;
        std::cout << ie.GetVersions(FLAGS_d) << std::endl;
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 2. Read IR Generated by ModelOptimizer (.xml and .bin files) ------------
        slog::info << "Loading network file:"
            "\n\t" << FLAGS_m <<
            slog::endl;

        /** Read network model **/
        CNNNetwork network = ie.ReadNetwork(FLAGS_m);

        Precision precision = network.getPrecision();
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 3. Configure input & output ---------------------------------------------

        // ------------------------------ Adding DetectionOutput -----------------------------------------------

        /**
         * The only meaningful difference between Faster-RCNN and SSD-like topologies is the interpretation
         * of the output data. Faster-RCNN has 2 output layers which (the same format) are presented inside SSD.
         *
         * But SSD has an additional post-processing DetectionOutput layer that simplifies output filtering.
         * So here we are adding 3 Reshapes and the DetectionOutput to the end of Faster-RCNN so it will return the
         * same result as SSD and we can easily parse it.
         */

        std::string firstLayerName = network.getInputsInfo().begin()->first;

        int inputWidth = network.getInputsInfo().begin()->second->getTensorDesc().getDims()[3];
        int inputHeight = network.getInputsInfo().begin()->second->getTensorDesc().getDims()[2];

        DataPtr bbox_pred_reshapeInPort = ((ICNNNetwork&)network).getData(FLAGS_bbox_name.c_str());
        if (bbox_pred_reshapeInPort == nullptr) {
            throw std::logic_error(std::string("Can't find output layer named ") + FLAGS_bbox_name);
        }

        SizeVector bbox_pred_reshapeOutDims = {
            bbox_pred_reshapeInPort->getTensorDesc().getDims()[0] *
            bbox_pred_reshapeInPort->getTensorDesc().getDims()[1], 1
        };
        DataPtr rois_reshapeInPort = ((ICNNNetwork&)network).getData(FLAGS_proposal_name.c_str());
        if (rois_reshapeInPort == nullptr) {
            throw std::logic_error(std::string("Can't find output layer named ") + FLAGS_proposal_name);
        }

        SizeVector rois_reshapeOutDims = {rois_reshapeInPort->getTensorDesc().getDims()[0] * rois_reshapeInPort->getTensorDesc().getDims()[1], 1};

        DataPtr cls_prob_reshapeInPort = ((ICNNNetwork&)network).getData(FLAGS_prob_name.c_str());
        if (cls_prob_reshapeInPort == nullptr) {
            throw std::logic_error(std::string("Can't find output layer named ") + FLAGS_prob_name);
        }

        SizeVector cls_prob_reshapeOutDims = {cls_prob_reshapeInPort->getTensorDesc().getDims()[0] * cls_prob_reshapeInPort->getTensorDesc().getDims()[1], 1};

        /*
            Detection output
        */

        int normalized = 0;
        int prior_size = normalized ? 4 : 5;
        int num_priors = rois_reshapeOutDims[0] / prior_size;

        // num_classes guessed from the output dims
        if (bbox_pred_reshapeOutDims[0] % (num_priors * 4) != 0) {
            throw std::logic_error("Can't guess number of classes. Something's wrong with output layers dims");
        }
        int num_classes = bbox_pred_reshapeOutDims[0] / (num_priors * 4);
        slog::info << "num_classes guessed: " << num_classes << slog::endl;

        LayerParams detectionOutParams;
        detectionOutParams.name = "detection_out";
        detectionOutParams.type = "DetectionOutput";
        detectionOutParams.precision = precision;
        CNNLayerPtr detectionOutLayer = CNNLayerPtr(new CNNLayer(detectionOutParams));
        detectionOutLayer->params["background_label_id"] = "0";
        detectionOutLayer->params["code_type"] = "caffe.PriorBoxParameter.CENTER_SIZE";
        detectionOutLayer->params["eta"] = "1.0";
        detectionOutLayer->params["input_height"] = std::to_string(inputHeight);
        detectionOutLayer->params["input_width"] = std::to_string(inputWidth);
        detectionOutLayer->params["keep_top_k"] = "200";
        detectionOutLayer->params["nms_threshold"] = "0.3";
        detectionOutLayer->params["normalized"] = std::to_string(normalized);
        detectionOutLayer->params["num_classes"] = std::to_string(num_classes);
        detectionOutLayer->params["share_location"] = "0";
        detectionOutLayer->params["top_k"] = "400";
        detectionOutLayer->params["variance_encoded_in_target"] = "1";
        detectionOutLayer->params["visualize"] = "False";

        detectionOutLayer->insData.push_back(bbox_pred_reshapeInPort);
        detectionOutLayer->insData.push_back(cls_prob_reshapeInPort);
        detectionOutLayer->insData.push_back(rois_reshapeInPort);

        SizeVector detectionOutLayerOutDims = {1, 1, 200, 7};
        DataPtr detectionOutLayerOutPort = DataPtr(new Data("detection_out", {precision, detectionOutLayerOutDims,
                                                            TensorDesc::getLayoutByDims(detectionOutLayerOutDims)}));
        detectionOutLayerOutPort->getCreatorLayer() = detectionOutLayer;
        detectionOutLayer->outData.push_back(detectionOutLayerOutPort);

        DetectionOutputPostProcessor detOutPostProcessor(detectionOutLayer.get());

        network.addOutput(FLAGS_bbox_name, 0);
        network.addOutput(FLAGS_prob_name, 0);
        network.addOutput(FLAGS_proposal_name, 0);

        // --------------------------- Prepare input blobs -----------------------------------------------------
        slog::info << "Preparing input blobs" << slog::endl;

        /** Taking information about all topology inputs **/
        InputsDataMap inputsInfo(network.getInputsInfo());

        /** SSD network has one input and one output **/
        if (inputsInfo.size() != 1 && inputsInfo.size() != 2) throw std::logic_error("Demo supports topologies only with 1 or 2 inputs");

        std::string imageInputName, imInfoInputName;

        InputInfo::Ptr inputInfo = inputsInfo.begin()->second;

        SizeVector inputImageDims;
        /** Stores input image **/

        /** Iterating over all input blobs **/
        for (auto & item : inputsInfo) {
            /** Working with first input tensor that stores image **/
            if (item.second->getInputData()->getTensorDesc().getDims().size() == 4) {
                imageInputName = item.first;

                slog::info << "Batch size is " << std::to_string(network.getBatchSize()) << slog::endl;

                /** Creating first input blob **/
                Precision inputPrecision = Precision::U8;
                item.second->setPrecision(inputPrecision);

            } else if (item.second->getInputData()->getTensorDesc().getDims().size() == 2) {
                imInfoInputName = item.first;

                Precision inputPrecision = Precision::FP32;
                item.second->setPrecision(inputPrecision);
                if ((item.second->getTensorDesc().getDims()[1] != 3 && item.second->getTensorDesc().getDims()[1] != 6) ||
                     item.second->getTensorDesc().getDims()[0] != 1) {
                    throw std::logic_error("Invalid input info. Should be 3 or 6 values length");
                }
            }
        }

        // ------------------------------ Prepare output blobs -------------------------------------------------
        slog::info << "Preparing output blobs" << slog::endl;

        OutputsDataMap outputsInfo(network.getOutputsInfo());

        const int maxProposalCount = detectionOutLayerOutDims[2];
        const int objectSize = detectionOutLayerOutDims[3];

        /** Set the precision of output data provided by the user, should be called before load of the network to the device **/

        outputsInfo[FLAGS_bbox_name]->setPrecision(Precision::FP32);
        outputsInfo[FLAGS_prob_name]->setPrecision(Precision::FP32);
        outputsInfo[FLAGS_proposal_name]->setPrecision(Precision::FP32);
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 4. Loading model to the device ------------------------------------------
        slog::info << "Loading model to the device" << slog::endl;
        ExecutableNetwork executable_network = ie.LoadNetwork(network, FLAGS_d);
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 5. Create infer request -------------------------------------------------
        slog::info << "Create infer request" << slog::endl;
        InferRequest infer_request = executable_network.CreateInferRequest();
        // -----------------------------------------------------------------------------------------------------

        // --------------------------- 6. Prepare input --------------------------------------------------------
        /** Collect images **/
        std::vector<cv::Mat> images;
        for (auto &path: imagePaths) {
            cv::Mat image = cv::imread(path, cv::IMREAD_COLOR);
            if (image.empty()) {
                slog::warn << "Image " + path + " cannot be read!" << slog::endl;
                continue;
            }
            images.push_back(image);
        }
        if (images.empty()) throw std::logic_error("Valid input images were not found!");

        size_t batchSize = network.getBatchSize();
        slog::info << "Batch size is " << std::to_string(batchSize) << slog::endl;
        if (batchSize != images.size()) {
            slog::warn << "Number of images " + std::to_string(images.size()) + \
                " doesn't match batch size " + std::to_string(batchSize) << slog::endl;
            batchSize = std::min(batchSize, images.size());
            slog::warn << "Number of images to be processed is "<< std::to_string(batchSize) << slog::endl;
        }

        /** Filling input tensor with images **/
        Blob::Ptr imageInput = infer_request.GetBlob(imageInputName);

        for (size_t image_id = 0; image_id < std::min(images.size(), batchSize); ++image_id)
            matU8ToBlob<unsigned char>(images[image_id], imageInput, image_id);

        if (!imInfoInputName.empty()) {
            Blob::Ptr input2 = infer_request.GetBlob(imInfoInputName);
            auto imInfoDim = inputsInfo.find(imInfoInputName)->second->getTensorDesc().getDims()[1];

            /** Fill input tensor with values **/
            float *p = input2->buffer().as<PrecisionTrait<Precision::FP32>::value_type*>();

            for (size_t image_id = 0; image_id < std::min(images.size(), batchSize); ++image_id) {
                p[image_id * imInfoDim + 0] = static_cast<float>(inputsInfo[imageInputName]->getTensorDesc().getDims()[2]);
                p[image_id * imInfoDim + 1] = static_cast<float>(inputsInfo[imageInputName]->getTensorDesc().getDims()[3]);
                for (size_t k = 2; k < imInfoDim; k++) {
                    p[image_id * imInfoDim + k] = 1.0f;  // all scale factors are set to 1.0
                }
            }
        }
        // -----------------------------------------------------------------------------------------------------

        // ---------------------------- 7. Do inference --------------------------------------------------------
        slog::info << "Start inference" << slog::endl;
        infer_request.Infer();
        // -----------------------------------------------------------------------------------------------------

        // ---------------------------- 8. Process output ------------------------------------------------------
        slog::info << "Processing output blobs" << slog::endl;

        Blob::Ptr bbox_output_blob = infer_request.GetBlob(FLAGS_bbox_name);
        Blob::Ptr prob_output_blob = infer_request.GetBlob(FLAGS_prob_name);
        Blob::Ptr rois_output_blob = infer_request.GetBlob(FLAGS_proposal_name);

        std::vector<Blob::Ptr> detOutInBlobs = { bbox_output_blob, prob_output_blob, rois_output_blob };

        Blob::Ptr output_blob = std::make_shared<TBlob<float>>(TensorDesc(Precision::FP32, detectionOutLayerOutDims, Layout::NCHW));
        output_blob->allocate();
        std::vector<Blob::Ptr> detOutOutBlobs = { output_blob };

        detOutPostProcessor.execute(detOutInBlobs, detOutOutBlobs, nullptr);

        const float* detection = static_cast<PrecisionTrait<Precision::FP32>::value_type*>(output_blob->buffer());

        /* Each detection has image_id that denotes processed image */
        for (int curProposal = 0; curProposal < maxProposalCount; curProposal++) {
            auto image_id = static_cast<int>(detection[curProposal * objectSize + 0]);
            if (image_id < 0) {
                break;
            }

            float confidence = detection[curProposal * objectSize + 2];
            auto label = static_cast<int>(detection[curProposal * objectSize + 1]);
            auto xmin = static_cast<int>(detection[curProposal * objectSize + 3] * images[image_id].cols);
            auto ymin = static_cast<int>(detection[curProposal * objectSize + 4] * images[image_id].rows);
            auto xmax = static_cast<int>(detection[curProposal * objectSize + 5] * images[image_id].cols);
            auto ymax = static_cast<int>(detection[curProposal * objectSize + 6] * images[image_id].rows);

            std::cout << "[" << curProposal << "," << label << "] element, prob = " << confidence <<
                "    (" << xmin << "," << ymin << ")-(" << xmax << "," << ymax << ")" << " batch id : " << image_id;

            if (confidence > 0.5) {
                /** Drawing only objects with >50% probability **/
                std::cout << " WILL BE PRINTED!";

                const auto &color = CITYSCAPES_COLORS[label % arraySize(CITYSCAPES_COLORS)];
                cv::rectangle(images[image_id],
                    cv::Point(xmin, ymin), cv::Point(xmax, ymax),
                    cv::Scalar(color.blue(), color.green(), color.red()));
            }
            std::cout << std::endl;
        }

        for (size_t batch_id = 0; batch_id < batchSize; ++batch_id) {
            const std::string image_path = "out_" + std::to_string(batch_id) + ".bmp";

            if (cv::imwrite(image_path, images[batch_id])) {
                slog::info << "Image " + image_path + " created!" << slog::endl;
            } else {
                throw std::logic_error(std::string("Can't create a file: ") + image_path);
            }
        }
        // -----------------------------------------------------------------------------------------------------
    }
    catch (const std::exception& error) {
        slog::err << error.what() << slog::endl;
        return 1;
    }
    catch (...) {
        slog::err << "Unknown/internal exception happened." << slog::endl;
        return 1;
    }

    slog::info << "Execution successful" << slog::endl;
    slog::info << slog::endl << "This demo is an API example, for any performance measurements "
                                "please use the dedicated benchmark_app tool from the openVINO toolkit" << slog::endl;
    return 0;
}
