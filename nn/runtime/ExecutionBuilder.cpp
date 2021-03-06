/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "ExecutionBuilder"

#include "ExecutionBuilder.h"

#include <algorithm>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "CompilationBuilder.h"
#include "ControlFlow.h"
#include "CpuExecutor.h"
#include "ExecutionBurstController.h"
#include "HalInterfaces.h"
#include "Manager.h"
#include "ModelArgumentInfo.h"
#include "ModelBuilder.h"
#include "Tracing.h"
#include "TypeManager.h"
#include "Utils.h"

namespace android {
namespace nn {

// Partial validation of output shapes returned from driver, to ensure they
// conform to a very specific set of rules.
static bool validateOutputShapesFromDriver(ErrorStatus executionStatus, const ModelBuilder* model,
                                           const std::vector<OutputShape>& shapes) {
    // Enforces the following rules (some of which are from b/154054474):
    // - shapes vector is empty except in the case of NONE or OUTPUT_INSUFFICIENT_SIZE.
    //   If the vector is not empty, it must have as many entries as the step model has outputs.
    // - If NONE, then either shapes vector is empty, or every shape is
    //   marked isSufficient and, if a tensor, has known rank.
    // - If OUTPUT_INSUFFICIENT_SIZE, then the vector is not empty.  At least one entry
    //   is marked !isSufficient.
    switch (executionStatus) {
        case ErrorStatus::NONE: {
            NN_RET_CHECK(shapes.size() == 0 || shapes.size() == model->outputCount())
                    << "With execution ErrorStatus " << executionStatus
                    << " output shapes vector must be empty or of length " << model->outputCount()
                    << " but has length " << shapes.size();
            NN_RET_CHECK(std::all_of(shapes.begin(), shapes.end(),
                                     [](const OutputShape& shape) { return shape.isSufficient; }))
                    << "With execution ErrorStatus " << executionStatus
                    << " at least one output shape is unexpectedly marked !isSufficient";

            const TypeManager* tm = TypeManager::get();
            for (uint32_t outputIndex = 0, outputCount = shapes.size(); outputIndex < outputCount;
                 ++outputIndex) {
                const Operand& outputOperand = model->getOutputOperand(outputIndex);
                NN_RET_CHECK(!tm->isTensorType(outputOperand.type) ||
                             (shapes[outputIndex].dimensions.size() != 0))
                        << "With execution ErrorStatus " << executionStatus << " output#"
                        << outputIndex << " shape unexpectedly has zero rank";
            }

            break;
        }
        case ErrorStatus::OUTPUT_INSUFFICIENT_SIZE: {
            NN_RET_CHECK(shapes.size() == model->outputCount())
                    << "With execution ErrorStatus " << executionStatus
                    << " output shapes vector must be of length " << model->outputCount()
                    << " but has length " << shapes.size();
            NN_RET_CHECK(std::any_of(shapes.begin(), shapes.end(),
                                     [](const OutputShape& shape) { return !shape.isSufficient; }))
                    << "With execution ErrorStatus " << executionStatus
                    << " at least one output shape must have been marked !isSufficient";
            break;
        }
        default: {
            NN_RET_CHECK(shapes.size() == 0)
                    << "With execution ErrorStatus " << executionStatus
                    << " output shapes vector must be empty but has length " << shapes.size();
            break;
        }
    }
    return true;
}
static bool validateOutputShapesFromDriver(int executionResultCode, const ModelBuilder* model,
                                           const std::vector<OutputShape>& shapes) {
    return validateOutputShapesFromDriver(convertResultCodeToErrorStatus(executionResultCode),
                                          model, shapes);
}

static MeasureTiming measureTiming(const ExecutionBuilder* execution) {
    return execution->measureTiming() ? MeasureTiming::YES : MeasureTiming::NO;
}

static bool checkDimensionInfo(const Operand& operand, const ANeuralNetworksOperandType* newType,
                               const char* tag, bool allowUnspecified) {
    if (newType != nullptr) {
        const Extension::OperandTypeInformation* info = nullptr;
        if (isExtension(operand.type)) {
            NN_RET_CHECK(TypeManager::get()->getExtensionOperandTypeInfo(operand.type, &info));
        }
        if (validateOperandType(*newType, info, tag, allowUnspecified) !=
            ANEURALNETWORKS_NO_ERROR) {
            LOG(ERROR) << tag << ": Invalid newType";
            return false;
        }
        if (operand.dimensions.size() == 0) {
            return true;
        }
        if (operand.dimensions.size() != newType->dimensionCount) {
            LOG(ERROR) << tag << ": Setting with incompatible dimension count";
            return false;
        }
        for (uint32_t i = 0; i < newType->dimensionCount; i++) {
            if (operand.dimensions[i] != newType->dimensions[i] && operand.dimensions[i] != 0) {
                LOG(ERROR) << tag << ": Overriding a fully specified dimension is disallowed";
                return false;
            }
        }
    } else {
        if (!allowUnspecified && TypeManager::get()->isTensorType(operand.type) &&
            tensorHasUnspecifiedDimensions(operand)) {
            LOG(ERROR) << tag << ": Setting with operand type that is not fully specified";
            return false;
        }
    }
    return true;
}

ExecutionBuilder::ExecutionBuilder(const CompilationBuilder* compilation)
    : mCompilation(compilation),
      mModel(compilation->mModel),
      mPlan(&compilation->mPlan),
      mPartitioning(compilation->mPartitioning),
      mInputs(mModel->inputCount()),
      mOutputs(mModel->outputCount()) {
    VLOG(EXECUTION) << "ExecutionBuilder::ExecutionBuilder with " << mInputs.size()
                    << " inputs and " << mOutputs.size() << " outputs";
}

const ModelBuilder* ExecutionBuilder::getSourceModel(uint32_t index) const {
    return mPlan->getSourceModels().getModel(index);
}

bool ExecutionBuilder::isFinished() const {
    CHECK(!(mFinishedWithoutSyncFence && hasSyncFence()));
    if (mFinishedWithoutSyncFence) {
        return true;
    }
    if (hasSyncFence()) {
        auto r = syncWait(mSyncFenceFd, 0);
        CHECK(r != FenceState::UNKNOWN);
        return r != FenceState::ACTIVE;
    }
    return false;
}

ExecutionBuilder::Completion ExecutionBuilder::completedWith() const {
    CHECK(isFinished());
    if (hasSyncFence()) {
        auto r = syncWait(mSyncFenceFd, 0);
        CHECK(r == FenceState::SIGNALED || r == FenceState::ERROR);
        return (r == FenceState::SIGNALED) ? Completion::NO_ERROR : Completion::OTHER_ERROR;
    } else {
        return mCompletionWithoutSyncFence;
    }
}

int ExecutionBuilder::setInput(uint32_t index, const ANeuralNetworksOperandType* type,
                               const void* buffer, size_t length) {
    if (mStarted) {
        LOG(ERROR) << "ANeuralNetworksExecution_setInput called after the "
                      "execution has started.";
        return ANEURALNETWORKS_BAD_STATE;
    }
    uint32_t count = static_cast<uint32_t>(mInputs.size());
    if (index >= count) {
        LOG(ERROR) << "ANeuralNetworksExecution_setInput bad index " << index << " " << count;
        return ANEURALNETWORKS_BAD_DATA;
    }
    if (!checkDimensionInfo(mModel->getInputOperand(index), type,
                            "ANeuralNetworksExecution_setInput", buffer == nullptr)) {
        return ANEURALNETWORKS_BAD_DATA;
    }
    if (length > 0xFFFFFFFF) {
        LOG(ERROR) << "ANeuralNetworksExecution_setInput input exceeds max length " << length;
        return ANEURALNETWORKS_BAD_DATA;
    }
    uint32_t l = static_cast<uint32_t>(length);
    if (!mInputs[index].unspecified()) {
        LOG(ERROR) << "ANeuralNetworksExecution_setInput called when an input has already been "
                      "provided";
        return ANEURALNETWORKS_BAD_STATE;
    }
    int n;
    std::tie(n, mInputs[index]) = ModelArgumentInfo::createFromPointer(
            mModel->getInputOperand(index), type, const_cast<void*>(buffer), l);
    return n;
}

int ExecutionBuilder::setInputFromMemory(uint32_t index, const ANeuralNetworksOperandType* type,
                                         const RuntimeMemory* memory, size_t offset,
                                         size_t length) {
    // Should be similar to StepExecutor::setInputOrOutputFromMemory()

    if (mStarted) {
        LOG(ERROR) << "ANeuralNetworksExecution_setInputFromMemory called after the "
                      "execution has started.";
        return ANEURALNETWORKS_BAD_STATE;
    }
    uint32_t count = static_cast<uint32_t>(mInputs.size());
    if (index >= count) {
        LOG(ERROR) << "ANeuralNetworksExecution_setInputFromMemory bad index " << index << " "
                   << count;
        return ANEURALNETWORKS_BAD_DATA;
    }
    if (!checkDimensionInfo(mModel->getInputOperand(index), type,
                            "ANeuralNetworksExecution_setInputFromMemory", false)) {
        return ANEURALNETWORKS_BAD_DATA;
    }
    if (!memory->getValidator().validate(mCompilation, IOType::INPUT, index, type, offset,
                                         length)) {
        return ANEURALNETWORKS_BAD_DATA;
    }
    // For some types of memory, e.g. MemoryRuntimeAHWB allocated from ANNMemory_createFromDesc, we
    // allow the client to specify offset == 0 && length == 0 indicating that the entire memory
    // region is used. We update the length here because the drivers are still expecting a real
    // length. For other memories that do not allow this semantic, it is checked in
    // MemoryValidatorBase::validate before reaching here.
    if (memory->getHidlMemory().valid() && offset == 0 && length == 0) {
        length = memory->getHidlMemory().size();
    }
    // TODO validate the rest
    uint32_t poolIndex = mMemories.add(memory);
    if (!mInputs[index].unspecified()) {
        LOG(ERROR)
                << "ANeuralNetworksExecution_setInputFromMemory called when an input has already "
                   "been provided";
        return ANEURALNETWORKS_BAD_STATE;
    }
    int n;
    std::tie(n, mInputs[index]) = ModelArgumentInfo::createFromMemory(
            mModel->getInputOperand(index), type, poolIndex, offset, length);
    return n;
}

int ExecutionBuilder::setOutput(uint32_t index, const ANeuralNetworksOperandType* type,
                                void* buffer, size_t length) {
    if (mStarted) {
        LOG(ERROR) << "ANeuralNetworksExecution_setOutput called after the "
                      "execution has started.";
        return ANEURALNETWORKS_BAD_STATE;
    }
    uint32_t count = static_cast<uint32_t>(mOutputs.size());
    if (index >= count) {
        LOG(ERROR) << "ANeuralNetworksExecution_setOutput bad index " << index << " " << count;
        return ANEURALNETWORKS_BAD_DATA;
    }
    if (!checkDimensionInfo(mModel->getOutputOperand(index), type,
                            "ANeuralNetworksExecution_setOutput", true)) {
        return ANEURALNETWORKS_BAD_DATA;
    }
    if (length > 0xFFFFFFFF) {
        LOG(ERROR) << "ANeuralNetworksExecution_setOutput input exceeds max length " << length;
        return ANEURALNETWORKS_BAD_DATA;
    }
    uint32_t l = static_cast<uint32_t>(length);
    if (!mOutputs[index].unspecified()) {
        LOG(ERROR) << "ANeuralNetworksExecution_setOutput called when an output has already been "
                      "provided";
        return ANEURALNETWORKS_BAD_STATE;
    }
    int n;
    std::tie(n, mOutputs[index]) =
            ModelArgumentInfo::createFromPointer(mModel->getOutputOperand(index), type, buffer, l);
    return n;
}

int ExecutionBuilder::setOutputFromMemory(uint32_t index, const ANeuralNetworksOperandType* type,
                                          const RuntimeMemory* memory, size_t offset,
                                          size_t length) {
    // Should be similar to StepExecutor::setInputOrOutputFromMemory()

    if (mStarted) {
        LOG(ERROR) << "ANeuralNetworksExecution_setOutputFromMemory called after the "
                      "execution has started.";
        return ANEURALNETWORKS_BAD_STATE;
    }
    uint32_t count = static_cast<uint32_t>(mOutputs.size());
    if (index >= count) {
        LOG(ERROR) << "ANeuralNetworksExecution_setOutputFromMemory bad index " << index << " "
                   << count;
        return ANEURALNETWORKS_BAD_DATA;
    }
    if (!checkDimensionInfo(mModel->getOutputOperand(index), type,
                            "ANeuralNetworksExecution_setOutputFromMemory", true)) {
        return ANEURALNETWORKS_BAD_DATA;
    }
    if (!memory->getValidator().validate(mCompilation, IOType::OUTPUT, index, type, offset,
                                         length)) {
        return ANEURALNETWORKS_BAD_DATA;
    }
    // For some types of memory, e.g. MemoryRuntimeAHWB allocated from ANNMemory_createFromDesc, we
    // allow the client to specify offset == 0 && length == 0 indicating that the entire memory
    // region is used. We update the length here because the drivers are still expecting a real
    // length. For other memories that do not allow this semantic, it is checked in
    // MemoryValidatorBase::validate before reaching here.
    if (memory->getHidlMemory().valid() && offset == 0 && length == 0) {
        length = memory->getHidlMemory().size();
    }
    // TODO validate the rest
    uint32_t poolIndex = mMemories.add(memory);
    if (!mOutputs[index].unspecified()) {
        LOG(ERROR) << "ANeuralNetworksExecution_setOutputFromMemory called when an output has "
                      "already been provided";
        return ANEURALNETWORKS_BAD_STATE;
    }
    int n;
    std::tie(n, mOutputs[index]) = ModelArgumentInfo::createFromMemory(
            mModel->getOutputOperand(index), type, poolIndex, offset, length);
    return n;
}

int ExecutionBuilder::setMeasureTiming(bool measure) {
    if (!mCompilation->mExplicitDeviceList || (mCompilation->mDevices.size() != 1)) {
        LOG(ERROR) << "ANeuralNetworksExecution_setMeasureTiming called on "
                   << "an ANeuralNetworksExecution created from an ANeuralNetworksCompilation "
                   << "that was not created by ANeuralNetworksCompilation_createForDevices "
                   << "with numDevices = 1";
        return ANEURALNETWORKS_BAD_DATA;
    }
    if (mStarted) {
        LOG(ERROR) << "ANeuralNetworksExecution_setMeasureTiming called after the "
                      "execution has started.";
        return ANEURALNETWORKS_BAD_STATE;
    }
    mMeasureTiming = measure;
    return ANEURALNETWORKS_NO_ERROR;
}

int ExecutionBuilder::getDuration(int32_t durationCode, uint64_t* duration) const {
    if (!isFinished()) {
        LOG(ERROR) << "ANeuralNetworksExecution_getDuration called before the "
                      "execution has finished.";
        *duration = UINT64_MAX;
        return ANEURALNETWORKS_BAD_STATE;
    }
    if (completedWith() != Completion::NO_ERROR) {
        LOG(ERROR) << "ANeuralNetworksExecution_getDuration called on an execution "
                      "that has encountered an error.";
        *duration = UINT64_MAX;
        return ANEURALNETWORKS_BAD_STATE;
    }

    // NOTE: At the HAL level, timing is in microseconds. At the NDK level, nanoseconds.
    const uint64_t kNanoPerMicro = 1000;

    if (!mMeasureTiming) {
        *duration = UINT64_MAX;
        return ANEURALNETWORKS_BAD_STATE;
    }

    Timing timingLaunched = mTimingWithoutFencedExecutionCallback;
    Timing timingFenced = timingLaunched;
    if (mFencedExecutionCallback != nullptr) {
        ErrorStatus status;
        const hardware::Return<void> ret = mFencedExecutionCallback->getExecutionInfo(
                [&status, &timingLaunched, &timingFenced](
                        V1_3::ErrorStatus error, V1_2::Timing tLaunched, V1_2::Timing tFenced) {
                    status = uncheckedConvert(error);
                    timingLaunched = uncheckedConvert(tLaunched);
                    timingFenced = uncheckedConvert(tFenced);
                });
        if (!ret.isOk()) {
            *duration = UINT64_MAX;
            return ANEURALNETWORKS_OP_FAILED;
        }
        if (status != ErrorStatus::NONE) {
            *duration = UINT64_MAX;
            return ANEURALNETWORKS_BAD_STATE;
        }
    }
    uint64_t microDuration = UINT64_MAX;
    switch (durationCode) {
        case ANEURALNETWORKS_DURATION_ON_HARDWARE:
            microDuration = timingLaunched.timeOnDevice;
            break;
        case ANEURALNETWORKS_DURATION_IN_DRIVER:
            microDuration = timingLaunched.timeInDriver;
            break;
        case ANEURALNETWORKS_FENCED_DURATION_ON_HARDWARE:
            microDuration = timingFenced.timeOnDevice;
            break;
        case ANEURALNETWORKS_FENCED_DURATION_IN_DRIVER:
            microDuration = timingFenced.timeInDriver;
            break;
        default:
            CHECK(!"unexpected");
    }
    *duration = (microDuration == UINT64_MAX) ? UINT64_MAX : kNanoPerMicro * microDuration;

    VLOG(EXECUTION) << "getDuration(" << durationCode << "): " << *duration;
    return ANEURALNETWORKS_NO_ERROR;
}

int ExecutionBuilder::setTimeoutDuration(uint64_t duration) {
    if (!mCompilation->mExplicitDeviceList || (mCompilation->mDevices.size() != 1)) {
        LOG(ERROR) << "ANeuralNetworksExecution_setTimeout called on an ANeuralNetworksExecution "
                      "created from an ANeuralNetworksCompilation that was not created by "
                      "ANeuralNetworksCompilation_createForDevices with numDevices = 1";
        return ANEURALNETWORKS_BAD_DATA;
    }
    if (mStarted) {
        LOG(ERROR) << "ANeuralNetworksExecution_setTimeout called after the execution has started.";
        return ANEURALNETWORKS_BAD_STATE;
    }
    if (duration > 0) {
        mTimeoutDuration = duration;
    } else {
        mTimeoutDuration.reset();
    }
    return ANEURALNETWORKS_NO_ERROR;
}

std::optional<uint64_t> ExecutionBuilder::getTimeoutDuration() const {
    return mTimeoutDuration;
}

int ExecutionBuilder::setLoopTimeout(uint64_t duration) {
    if (mStarted) {
        LOG(ERROR) << "ANeuralNetworksExecution_setLoopTimeout called after the "
                      "execution has started.";
        return ANEURALNETWORKS_BAD_STATE;
    }
    if (duration > operation_while::kTimeoutNsMaximum) {
        LOG(WARNING) << "ANeuralNetworksExecution_setLoopTimeout input exceeds the maximum allowed "
                     << "duration: " << duration << " > " << operation_while::kTimeoutNsMaximum;
        duration = operation_while::kTimeoutNsMaximum;
    }
    mLoopTimeoutDuration = duration;
    return ANEURALNETWORKS_NO_ERROR;
}

int ExecutionBuilder::getOutputOperandDimensions(uint32_t index, uint32_t* dimensions) {
    if (!isFinished()) {
        LOG(ERROR) << "ANeuralNetworksExecution_getOutputOperandDimensions called before the "
                      "execution has finished.";
        return ANEURALNETWORKS_BAD_STATE;
    }
    if (completedWith() == Completion::OTHER_ERROR) {
        LOG(ERROR) << "ANeuralNetworksExecution_getOutputOperandDimensions called on an execution "
                      "that has encountered an error.";
        return ANEURALNETWORKS_BAD_STATE;
    }

    uint32_t count = static_cast<uint32_t>(mOutputs.size());
    if (index >= count) {
        LOG(ERROR) << "ANeuralNetworksExecution_getOutputOperandDimensions bad index " << index
                   << " " << count;
        return ANEURALNETWORKS_BAD_DATA;
    }
    const auto& dims = mOutputs[index].dimensions();
    if (dims.empty()) {
        LOG(ERROR) << "ANeuralNetworksExecution_getOutputOperandDimensions can not query "
                      "dimensions of a scalar";
        return ANEURALNETWORKS_BAD_DATA;
    }
    std::copy(dims.begin(), dims.end(), dimensions);
    return mOutputs[index].isSufficient() ? ANEURALNETWORKS_NO_ERROR
                                          : ANEURALNETWORKS_OUTPUT_INSUFFICIENT_SIZE;
}

int ExecutionBuilder::getOutputOperandRank(uint32_t index, uint32_t* rank) {
    if (!isFinished()) {
        LOG(ERROR) << "ANeuralNetworksExecution_getOutputOperandRank called before the "
                      "execution has finished.";
        return ANEURALNETWORKS_BAD_STATE;
    }
    if (completedWith() == Completion::OTHER_ERROR) {
        LOG(ERROR) << "ANeuralNetworksExecution_getOutputOperandRank called on an execution "
                      "that has encountered an error.";
        return ANEURALNETWORKS_BAD_STATE;
    }
    uint32_t count = static_cast<uint32_t>(mOutputs.size());
    if (index >= count) {
        LOG(ERROR) << "ANeuralNetworksExecution_getOutputOperandRank bad index " << index << " "
                   << count;
        return ANEURALNETWORKS_BAD_DATA;
    }
    *rank = static_cast<uint32_t>(mOutputs[index].dimensions().size());
    return mOutputs[index].isSufficient() ? ANEURALNETWORKS_NO_ERROR
                                          : ANEURALNETWORKS_OUTPUT_INSUFFICIENT_SIZE;
}

// Attempt synchronous execution of full model on CPU.
// TODO: How should we handle timing in this case?
//       For Q this is irrelevant: We only support timing in conjunction
//         with an explicit device list; and we do not support CPU fallback
//         with an explicit device list.  See CompilationBuilder::mExplicitDeviceList.
static std::tuple<int, std::vector<OutputShape>, Timing> cpuFallbackFull(
        ExecutionBuilder* executionBuilder) {
    CHECK(executionBuilder != nullptr);
    NNTRACE_RT(NNTRACE_PHASE_EXECUTION, "cpuFallbackFull");
    VLOG(EXECUTION) << "cpuFallbackFull";

    // Get fallback executor.
    StepExecutor executor(executionBuilder, executionBuilder->getModel(),
                          DeviceManager::getCpuDevice(), /*preparedModel=*/nullptr);
    executor.mapInputsAndOutputsTrivially();

    // Attempt fallback execution.
    return executor.computeOnCpuFallback();
}

// Attempt synchronous execution on CPU.
// TODO: How should we handle timing in this case?
//       For Q this is irrelevant: We only support timing in conjunction
//         with an explicit device list; and we do not support CPU fallback
//         with an explicit device list.  See CompilationBuilder::mExplicitDeviceList.
static std::tuple<int, std::vector<OutputShape>, Timing, std::shared_ptr<StepExecutor>>
cpuFallbackPartial(const ExecutionPlan& plan,
                   std::shared_ptr<ExecutionPlan::Controller> controller) {
    NNTRACE_RT(NNTRACE_PHASE_EXECUTION, "cpuFallbackPartial");
    VLOG(EXECUTION) << "cpuFallbackPartial";

    // Get fallback executor.
    std::shared_ptr<StepExecutor> executor;
    int n1 = plan.fallback(controller, &executor, nullptr, nullptr);
    if (n1 != ANEURALNETWORKS_NO_ERROR) {
        return {n1, {}, {}, nullptr};
    }
    CHECK(executor != nullptr);

    // Attempt fallback execution.
    auto [n2, outputShapes, timing] = executor->computeOnCpuFallback();
    return {n2, std::move(outputShapes), timing, executor};
}

static void asyncStartComputePartitioned(ExecutionBuilder* executionBuilder,
                                         const ExecutionPlan& plan,
                                         std::shared_ptr<ExecutionPlan::Controller> controller,
                                         bool allowCpuFallback,
                                         const std::optional<Deadline>& deadline,
                                         const sp<ExecutionCallback>& executionCallback) {
    CHECK(executionBuilder != nullptr);
    VLOG(EXECUTION) << "ExecutionBuilder::compute (from plan, iteratively)";

    std::vector<OutputShape> outputShapes = executionBuilder->getInitialOutputShapes();
    Timing timing;
    // Disallow CPU fallback when the ExecutionPlan is simple on CPU.
    allowCpuFallback &= !plan.isSimpleCpu();

    // On this iteration, do I need to repeat the previous step because it
    // reported insufficient size?
    bool doInsufficientSizeFallback = false;

    while (true) {
        VLOG(EXECUTION) << "looking for next StepExecutor";

        // Get the current step of the execution.
        std::shared_ptr<StepExecutor> executor;
        std::shared_ptr<ExecutionBurstController> burstController;
        int n = doInsufficientSizeFallback
                        ? plan.fallback(controller, &executor, &burstController, &outputShapes)
                        : plan.next(controller, &executor, &burstController, &outputShapes);
        doInsufficientSizeFallback = false;
        if (n != ANEURALNETWORKS_NO_ERROR) {
            // During the interpreted execution of control flow, a loop timeout
            // might occur in ExecutionPlan::next().
            bool missedDeadline = n == ANEURALNETWORKS_MISSED_DEADLINE_TRANSIENT ||
                                  n == ANEURALNETWORKS_MISSED_DEADLINE_PERSISTENT;
            if (allowCpuFallback && !missedDeadline) break;
            executionCallback->notify(convertResultCodeToErrorStatus(n), {}, {});
            return;
        }

        // If the code reached the end of the plan without error, then return
        // with no error.
        if (executor == nullptr) {
            executionCallback->notify(ErrorStatus::NONE, outputShapes, timing);
            return;
        }
        const bool executorIsCpu = executor->isCpu();

        // Attempt to execute a single step of the execution.
        auto [stepN, stepOutputShapes, stepTiming] = executor->compute(deadline, burstController);

        // Update global outputs and dynamic temporaries.
        StepExecutor::UpdateOutputShapes updateOutputShapes = {};
        if (!executor->updateOutputShapes(stepN, stepOutputShapes, &outputShapes,
                                          &updateOutputShapes)) {
            stepN = ANEURALNETWORKS_OP_FAILED;
        }

        // If execution was successful, continue to next step.
        if (stepN == ANEURALNETWORKS_NO_ERROR) {
            if (updateOutputShapes.zeroSizedInput) {
                // We'll need to do full model CPU fallback
                VLOG(EXECUTION) << "updateOutputShapes.zeroSizedInput";
                stepN = ANEURALNETWORKS_OP_FAILED;
            } else {
                CHECK(executor->areDynamicTemporariesAllocated());
                // We only support collection of timing information in the case
                // of a single step, so it's safe to just keep track of the last
                // step's timing information.
                timing = stepTiming;
                continue;
            }
        }

        if (stepN == ANEURALNETWORKS_OUTPUT_INSUFFICIENT_SIZE) {
            VLOG(EXECUTION) << "OUTPUT_INSUFFICIENT_SIZE: " << toString(updateOutputShapes);
            if (updateOutputShapes.mainOutputInsufficient ||
                !updateOutputShapes.updatedDynamicTemporary) {
                // Either:
                // - At least one main model output is not of sufficient size; or
                // - we didn't learn anything new about dynamic temporaries.
                // Neither of these is recoverable, so end execution.
                const ErrorStatus stepStatus = convertResultCodeToErrorStatus(stepN);
                executionCallback->notify(stepStatus, outputShapes, {});
                return;
            }
            // Every main model output is of sufficient size.  This implies that
            // at least one dynamic temporary is not of sufficient size.  This
            // is recoverable.
            doInsufficientSizeFallback = true;
            continue;
        }

        // If CPU fallback is not allowed and there was an error, end execution.
        if (!allowCpuFallback) {
            const ErrorStatus stepStatus = convertResultCodeToErrorStatus(stepN);
            executionCallback->notify(stepStatus, {}, {});
            return;
        }

        // If CPU execution was already attempted, either:
        // (1) perform a full CPU fallback if the plan is not simple, or
        // (2) return from the function with an error
        if (executorIsCpu) {
            if (!plan.isSimple()) break;
            executionCallback->notify(convertResultCodeToErrorStatus(stepN), {}, {});
            return;
        }

        // If the code reaches this point, attempt a partial fallback to CPU.
        CHECK(allowCpuFallback);
        if (updateOutputShapes.zeroSizedInput) {
            // Do not attempt a partial fallback.
            break;
        }
        while (true) {
            auto [fallbackN, fallbackOutputShapes, fallbackTiming, fallbackExecutor] =
                    cpuFallbackPartial(plan, controller);

            // Update global outputs and dynamic temporaries.
            StepExecutor::UpdateOutputShapes fallbackUpdateOutputShapes = {};
            if (fallbackExecutor != nullptr &&
                !fallbackExecutor->updateOutputShapes(fallbackN, fallbackOutputShapes,
                                                      &outputShapes, &fallbackUpdateOutputShapes)) {
                fallbackN = ANEURALNETWORKS_OP_FAILED;
            }

            // If execution was successful, continue to next step.
            if (fallbackN == ANEURALNETWORKS_NO_ERROR) {
                if (fallbackUpdateOutputShapes.zeroSizedInput) {
                    // We'll need to do full model CPU fallback
                    VLOG(EXECUTION) << "fallbackUpdateOutputShapes.zeroSizedInput";
                    fallbackN = ANEURALNETWORKS_OP_FAILED;
                    break;
                }
                CHECK(fallbackExecutor->areDynamicTemporariesAllocated());
                // We only support collection of timing information in the case of a
                // single step, so it's safe to just keep track of the last step's
                // timing information.
                timing = fallbackTiming;
                goto nextStep;
            }

            if (fallbackN == ANEURALNETWORKS_OUTPUT_INSUFFICIENT_SIZE) {
                VLOG(EXECUTION) << "OUTPUT_INSUFFICIENT_SIZE: "
                                << toString(fallbackUpdateOutputShapes);
                if (fallbackUpdateOutputShapes.mainOutputInsufficient ||
                    !fallbackUpdateOutputShapes.updatedDynamicTemporary) {
                    // Either:
                    // - At least one main model output is not of sufficient size; or
                    // - we didn't learn anything new about dynamic temporaries.
                    // Neither of these is recoverable, so end execution.
                    const ErrorStatus fallbackStatus = convertResultCodeToErrorStatus(fallbackN);
                    executionCallback->notify(fallbackStatus, outputShapes, {});
                    return;
                }
                // Every main model output is of sufficient size.  This implies
                // that at least one dynamic temporary is not of sufficient
                // size.  This is recoverable.
                continue;
            }

            // Do not fallback twice if the ExecutionPlan is simple.
            if (plan.isSimple()) {
                const ErrorStatus fallbackStatus = convertResultCodeToErrorStatus(fallbackN);
                executionCallback->notify(fallbackStatus, {}, {});
                return;
            }

            // If the code reaches this point, then there was an error with the
            // fallback. In this case, attempt full fallback.
            break;
        }

        // If the code reaches this point, then there was an error with the
        // fallback. In this case, attempt full fallback.
        break;

    nextStep:
        // Bottom of the outer loop
        continue;
    }

    // If the code has reached this point, a potentially recoverable error
    // occurred during the step executions. Instead, do a full execution
    // fallback on the CPU.
    auto [fullN, fullOutputShapes, fullTiming] = cpuFallbackFull(executionBuilder);
    const ErrorStatus fullStatus = convertResultCodeToErrorStatus(fullN);
    executionCallback->notify(fullStatus, fullOutputShapes, fullTiming);
}

// In case of partitioned execution, startComputeFenced call will return the sync
// fence and the fenced compute callback returned from the last partition.
// Any failed partition will result in the whole execution fallback to CPU if
// allowCpuFallback is set to true.
static std::tuple<int, int, sp<V1_3::IFencedExecutionCallback>> startComputeFenced(
        ExecutionBuilder* executionBuilder, const ExecutionPlan& plan,
        std::shared_ptr<ExecutionPlan::Controller> controller, const std::vector<int>& waitFor,
        uint64_t timeoutDurationAfterFence, const std::optional<Deadline>& deadline,
        bool allowCpuFallback) {
    // We should have detected this earlier in the call chain and fallen back to
    // non-fenced execution.  This is an implementation limitation: In order to
    // support dynamic temporarires in this code, we'd need to implement
    // something like the following:
    // - If a partition has outputs of unknown size, execute that partition in a
    //   non fenced fashion, just as if it were scheduled on a driver that does
    //   not support fenced execution.
    // - Implement something similar to the code in asyncStartComputePartitioned()
    //   that handles a step execution that fails with
    //   ANEURALNETWORKS_OUTPUT_INSUFFICIENT_SIZE.
    CHECK(!executionBuilder->getCompilation()->hasDynamicTemporaries());

    CHECK(executionBuilder != nullptr);
    VLOG(EXECUTION) << "ExecutionBuilder::computeFenced (from plan, iteratively)";
    // Disallow fallback when the ExecutionPlan is simple on CPU.
    allowCpuFallback &= !plan.isSimpleCpu();

    // Initiate waitForFds, syncFence for the first step.
    std::vector<int> waitForFds = waitFor;
    int syncFence = -1;
    sp<V1_3::IFencedExecutionCallback> computeFencedCallback;

    while (true) {
        VLOG(EXECUTION) << "looking for next StepExecutor";

        // Get the current step of the execution.
        std::shared_ptr<StepExecutor> executor;
        int n = plan.next(controller, &executor, nullptr, nullptr, syncFence);
        if (n != ANEURALNETWORKS_NO_ERROR) {
            // During the interpreted execution of control flow, a loop timeout
            // might occur in ExecutionPlan::next().
            bool missedDeadline = n == ANEURALNETWORKS_MISSED_DEADLINE_TRANSIENT ||
                                  n == ANEURALNETWORKS_MISSED_DEADLINE_PERSISTENT;
            if (allowCpuFallback && !missedDeadline) break;
            // Return -1 for the sync fence fd, and nullptr for the callback.
            return std::make_tuple(n, -1, nullptr);
        }

        // If the code reached the end of the plan without error, then return
        // with no error.
        if (executor == nullptr) {
            // If the final step returns a -1 for sync fence, the execution is finished.
            // Update the output shapes.
            if (syncFence == -1) {
                // TODO(miaowang): support dynamic output shape only with memory domain.
                // For now just return the initial output shapes.
                executionBuilder->finishWithoutSyncFence(
                        ErrorStatus::NONE, executionBuilder->getInitialOutputShapes());
            }
            return std::make_tuple(ANEURALNETWORKS_NO_ERROR, syncFence, computeFencedCallback);
        }
        const bool executorIsCpu = executor->isCpu();

        // Attempt to execute a single step of the execution.
        auto [stepN, syncFd, callback] =
                executor->computeFenced(waitForFds, timeoutDurationAfterFence, deadline);

        // Update waitForFds, syncFence for the next step.
        syncFence = syncFd;
        computeFencedCallback = callback;
        waitForFds.clear();
        if (syncFd > 0) {
            waitForFds = {syncFd};
        }

        // If execution was successful, continue to next step.
        if (stepN == ANEURALNETWORKS_NO_ERROR) {
            continue;
        }
        // If CPU fallback is not allowed and there was an error, end execution.
        if (!allowCpuFallback) {
            return std::make_tuple(stepN, -1, nullptr);
        }

        // If CPU execution was already attempted, either:
        // (1) perform a full fallback if the plan is not simple, or
        // (2) return from the function with an error
        if (executorIsCpu) {
            if (!plan.isSimple()) break;
            return std::make_tuple(stepN, -1, nullptr);
        }
        // If the code reaches this point, then there was an error with the
        // fallback. In this case, attempt full fallback.
        break;
    }

    // If the code has reached this point, a potentially recoverable error
    // occurred during the step executions. Instead, do a full execution
    // fallback on the CPU.
    VLOG(EXECUTION) << "Performing full fallback on the CPU.";
    for (int syncFd : waitFor) {
        if (syncFd > 0) {
            auto r = syncWait(syncFd, -1);
            if (r != FenceState::SIGNALED) {
                VLOG(EXECUTION) << "syncWait failed, fd: " << syncFd;
                return std::make_tuple(ANEURALNETWORKS_OP_FAILED, -1, nullptr);
            }
        }
    }
    auto [fullN, fullOutputShapes, fullTiming] = cpuFallbackFull(executionBuilder);
    const ErrorStatus fullStatus = convertResultCodeToErrorStatus(fullN);
    syncFence = -1;
    executionBuilder->finishWithoutSyncFence(fullStatus, fullOutputShapes);
    executionBuilder->reportTimingWithoutFencedExecutionCallback(fullTiming);
    return std::make_tuple(fullN, syncFence, nullptr);
}

int ExecutionBuilder::computeFenced(const std::vector<int>& waitFor,
                                    uint64_t timeoutDurationAfterFence, int* syncFence) {
    CHECK(syncFence != nullptr);
    if (mStarted) {
        LOG(ERROR) << "ANeuralNetworksExecution_startComputeWithDependencies"
                      " called on an execution that has already started";
        return ANEURALNETWORKS_BAD_STATE;
    }
    if (timeoutDurationAfterFence > 0) {
        if (!mCompilation->mExplicitDeviceList || (mCompilation->mDevices.size() != 1)) {
            LOG(ERROR)
                    << "ANeuralNetworksExecution_startComputeWithDependencies called with non-zero "
                       "duration on an ANeuralNetworksExecution "
                       "created from an ANeuralNetworksCompilation that was not created by "
                       "ANeuralNetworksCompilation_createForDevices with numDevices = 1";
            return ANEURALNETWORKS_BAD_DATA;
        }
    }
    const auto deadline = makeDeadline(mTimeoutDuration);
    for (auto& p : mInputs) {
        if (p.state() == ModelArgumentInfo::UNSPECIFIED) {
            LOG(ERROR) << "ANeuralNetworksExecution_startComputeWithDependencies"
                          " not all inputs specified";
            return ANEURALNETWORKS_BAD_DATA;
        }
    }
    for (auto& p : mOutputs) {
        if (p.state() == ModelArgumentInfo::UNSPECIFIED) {
            LOG(ERROR) << "ANeuralNetworksExecution_startComputeWithDependencies"
                          " not all outputs specified";
            return ANEURALNETWORKS_BAD_DATA;
        }
    }
    for (uint32_t i = 0; i < mOutputs.size(); i++) {
        if (mOutputs[i].state() != ModelArgumentInfo::HAS_NO_VALUE &&
            !checkDimensionInfo(mModel->getOutputOperand(i), nullptr,
                                "ANeuralNetworksExecution_startComputeWithDependencies", false)) {
            LOG(ERROR) << "ANeuralNetworksExecution_startComputeWithDependencies"
                          " not all outputs have fully specified dimensions";
            return ANEURALNETWORKS_BAD_DATA;
        }
    }
    mStarted = true;
    const bool allowCpuFallback = DeviceManager::partitioningAllowsFallback(mPartitioning);
    std::shared_ptr<ExecutionPlan::Controller> controller = mPlan->makeController(this, nullptr);
    VLOG(EXECUTION) << "ExecutionBuilder::computeFenced";
    int result;
    std::tie(result, mSyncFenceFd, mFencedExecutionCallback) =
            startComputeFenced(this, *mPlan, controller, waitFor, timeoutDurationAfterFence,
                               deadline, allowCpuFallback);
    *syncFence = mSyncFenceFd;
    return result;
}

int ExecutionBuilder::compute(sp<ExecutionCallback>* synchronizationCallback,
                              BurstBuilder* burstBuilder) {
    CHECK(synchronizationCallback == nullptr || burstBuilder == nullptr)
            << "synchronizationCallback and burstBuilder cannot simultaneously be used";

    const bool synchronous = (synchronizationCallback == nullptr);
    if (!synchronous) {
        *synchronizationCallback = nullptr;
    }

    const auto deadline = makeDeadline(mTimeoutDuration);

    // TODO validate that we have full types for all inputs and outputs,
    // that the graph is not cyclic,

    auto name = [synchronous, burstBuilder] {
        return burstBuilder ? "burstCompute" : synchronous ? "compute" : "startCompute";
    };
    if (mStarted) {
        LOG(ERROR) << "ANeuralNetworksExecution_" << name()
                   << " called on an execution that has already started";
        return ANEURALNETWORKS_BAD_STATE;
    }
    for (auto& p : mInputs) {
        if (p.state() == ModelArgumentInfo::UNSPECIFIED) {
            LOG(ERROR) << "ANeuralNetworksExecution_" << name() << " not all inputs specified";
            return ANEURALNETWORKS_BAD_DATA;
        } else if (p.state() == ModelArgumentInfo::MEMORY) {
            const RuntimeMemory* memory = mMemories[p.locationAndLength().poolIndex];
            if (!memory->getValidator().validateInputDimensions(p.dimensions())) {
                return ANEURALNETWORKS_OP_FAILED;
            }
        }
    }
    for (auto& p : mOutputs) {
        if (p.state() == ModelArgumentInfo::UNSPECIFIED) {
            LOG(ERROR) << "ANeuralNetworksExecution_" << name() << " not all outputs specified";
            return ANEURALNETWORKS_BAD_DATA;
        }
    }

    auto wrappedFinish = [this](ErrorStatus error, const std::vector<OutputShape>& outputShapes) {
        return finishWithoutSyncFence(error, outputShapes);
    };

    // TODO: For asynchronous execution, entire plan-based-path should run in an
    // asynchronous thread -- take the asynchronous thread logic out of
    // CpuPreparedModel::execute() and use it to wrap the plan-based-path.
    mStarted = true;
    const bool allowCpuFallback = DeviceManager::partitioningAllowsFallback(mPartitioning);
    std::shared_ptr<ExecutionPlan::Controller> controller =
            mPlan->makeController(this, burstBuilder);
    if (synchronous) {
        if (burstBuilder) {
            VLOG(EXECUTION) << "ExecutionBuilder::compute (synchronous API, burst)";
        } else {
            VLOG(EXECUTION) << "ExecutionBuilder::compute (synchronous API)";
        }
        sp<ExecutionCallback> localSynchronizationCallback = new ExecutionCallback();
        localSynchronizationCallback->setOnFinish(wrappedFinish);
        asyncStartComputePartitioned(this, *mPlan, controller, allowCpuFallback, deadline,
                                     localSynchronizationCallback);
        localSynchronizationCallback->wait();
        if (mMeasureTiming) {
            mTimingWithoutFencedExecutionCallback = localSynchronizationCallback->getTiming();
        }
        return convertErrorStatusToResultCode(localSynchronizationCallback->getStatus());
    } else /* asynchronous */ {
        // TODO: use a thread pool
        // TODO(mikie): this could have NNTRACE so we could measure the overhead
        //              of spinning up a new thread.

        // Prepare the callback for asynchronous execution.
        // sp<ExecutionCallback> object is returned when the
        // execution has been successfully launched, otherwise a
        // nullptr is returned.  The executionCallback is
        // abstracted in the NN API as an "event".
        sp<ExecutionCallback> executionCallback = new ExecutionCallback();
        executionCallback->setOnFinish(wrappedFinish);
        if (DeviceManager::get()->syncExecRuntime()) {
            VLOG(EXECUTION) << "ExecutionBuilder::compute (asynchronous API, non-threaded)";
            asyncStartComputePartitioned(this, *mPlan, controller, allowCpuFallback, deadline,
                                         executionCallback);
        } else {
            VLOG(EXECUTION) << "ExecutionBuilder::compute (asynchronous API)";
            std::thread asyncExecution(
                    [this, controller, allowCpuFallback, deadline, executionCallback] {
                        asyncStartComputePartitioned(this, *mPlan, controller, allowCpuFallback,
                                                     deadline, executionCallback);
                    });
            executionCallback->bindThread(std::move(asyncExecution));
        }
        *synchronizationCallback = executionCallback;
        return ANEURALNETWORKS_NO_ERROR;
    }
}

std::vector<OutputShape> ExecutionBuilder::getInitialOutputShapes() const {
    std::vector<OutputShape> outputShapes(mOutputs.size());
    std::transform(mOutputs.begin(), mOutputs.end(), outputShapes.begin(),
                   [](const auto& x) -> OutputShape {
                       std::vector<uint32_t> dimensions;
                       if (x.state() != ModelArgumentInfo::HAS_NO_VALUE) {
                           dimensions = x.dimensions();
                       }
                       return {.dimensions = std::move(dimensions), .isSufficient = true};
                   });
    return outputShapes;
}

// Check if the dimensions "to" is updatable by dimensions "from", where "from" must
// have no lower a specification level.
static bool isUpdatable(const std::vector<uint32_t>& to, const std::vector<uint32_t>& from) {
    if (to.size() == 0) return true;
    NN_RET_CHECK_EQ(to.size(), from.size());
    for (uint32_t i = 0; i < to.size(); i++) {
        NN_RET_CHECK(to[i] == from[i] || to[i] == 0);
    }
    return true;
}

static bool isZeroSizedTensor(int executionResultCode, const OutputShape& outputShape) {
    return (executionResultCode == ANEURALNETWORKS_NO_ERROR) && outputShape.isSufficient &&
           outputShape.dimensions.size() &&
           (std::find(outputShape.dimensions.begin(), outputShape.dimensions.end(), uint32_t(0)) !=
            outputShape.dimensions.end());
}

bool ExecutionBuilder::updateOutputShapes(ErrorStatus status,
                                          const std::vector<OutputShape>& outputShapes) {
    NN_RET_CHECK(validateOutputShapesFromDriver(status, mModel, outputShapes));

    if (outputShapes.size() == 0) {
        return true;
    }
    NN_RET_CHECK_EQ(outputShapes.size(), mOutputs.size());
    for (uint32_t i = 0; i < outputShapes.size(); i++) {
        // Check if only unspecified dimensions or rank are overwritten.
        NN_RET_CHECK(isUpdatable(mOutputs[i].dimensions(), outputShapes[i].dimensions));
        const OperandType operandType = mModel->getOutputOperand(i).type;
        NN_RET_CHECK(!TypeManager::get()->sizeOfDataOverflowsUInt32(operandType,
                                                                    outputShapes[i].dimensions));
    }
    for (uint32_t i = 0; i < outputShapes.size(); i++) {
        mOutputs[i].dimensions() = outputShapes[i].dimensions;
        mOutputs[i].isSufficient() = outputShapes[i].isSufficient;
    }
    return true;
}

bool ExecutionBuilder::updateMemories() {
    for (const auto& output : mOutputs) {
        if (output.state() != ModelArgumentInfo::MEMORY) continue;
        const RuntimeMemory* memory = mMemories[output.locationAndLength().poolIndex];
        NN_RET_CHECK(memory->getValidator().updateMetadata({.dimensions = output.dimensions()}));
    }
    return true;
}

ErrorStatus ExecutionBuilder::finishWithoutSyncFence(ErrorStatus status,
                                                     const std::vector<OutputShape>& outputShapes) {
    CHECK(!mFinishedWithoutSyncFence) << "ExecutionBuilder::finishWithoutSyncFence is called twice";
    CHECK(!hasSyncFence())
            << "ExecutionBuilder::finishWithoutSyncFence is called when hasSyncFence()";
    if (!updateOutputShapes(status, outputShapes) || !updateMemories()) {
        status = ErrorStatus::GENERAL_FAILURE;
    }
    bool success = status == ErrorStatus::NONE;
    for (const auto& output : mOutputs) {
        if (output.state() != ModelArgumentInfo::MEMORY) continue;
        const RuntimeMemory* memory = mMemories[output.locationAndLength().poolIndex];
        memory->getValidator().setInitialized(success);
    }
    switch (convertErrorStatusToResultCode(status)) {
        case ANEURALNETWORKS_NO_ERROR:
            mCompletionWithoutSyncFence = Completion::NO_ERROR;
            break;
        case ANEURALNETWORKS_OUTPUT_INSUFFICIENT_SIZE:
            mCompletionWithoutSyncFence = Completion::OUTPUT_INSUFFICIENT_SIZE;
            break;
        default:
            mCompletionWithoutSyncFence = Completion::OTHER_ERROR;
            break;
    }
    mFinishedWithoutSyncFence = true;
    return status;
}

std::string toString(StepExecutor::UpdateOutputShapes updateOutputShapes) {
    return "{ .updatedDynamicTemporary = " +
           std::to_string(updateOutputShapes.updatedDynamicTemporary) +
           ", .mainOutputInsufficient = " +
           std::to_string(updateOutputShapes.mainOutputInsufficient) + "}";
}

bool StepExecutor::updateOutputShapes(int executionResultCode, const std::vector<OutputShape>& from,
                                      std::vector<OutputShape>* to, UpdateOutputShapes* update) {
    CHECK(update != nullptr);
    *update = {.updatedDynamicTemporary = false,
               .mainOutputInsufficient = false,
               .zeroSizedInput = false};

    NN_RET_CHECK(validateOutputShapesFromDriver(executionResultCode, mModel, from));

    if (from.size() == 0) {
        return true;
    }

    if (VLOG_IS_ON(EXECUTION)) {
        for (const auto& shape : from) {
            VLOG(EXECUTION) << "updateOutputShapes: " << shape;
        }
    }

    if (mExecutionStep != nullptr) {
        const auto& indexMapping = mExecutionStep->getOutputIndexStepModelToMainModel();
        NN_RET_CHECK_LE(indexMapping.size(), from.size());
        for (uint32_t i = 0, e = indexMapping.size(); i < e; i++) {
            const uint32_t toIndex = indexMapping[i];
            NN_RET_CHECK_GT(to->size(), toIndex);
            NN_RET_CHECK(isUpdatable(to->at(toIndex).dimensions, from[i].dimensions));
            (*to)[toIndex] = from[i];
            update->mainOutputInsufficient |= !(*to)[toIndex].isSufficient;
            if (mExecutionStep->getModelOutputsThatAreDownstreamInputs().count(toIndex) &&
                isZeroSizedTensor(executionResultCode, from[i])) {
                update->zeroSizedInput = true;
            }
        }

        if (!mDynamicTemporaries->empty()) {
            // TODO(b/157236079): Instead of computing this here, precompute it in ExecutionStep?
            std::map<uint32_t, uint32_t> operandIndexStepModelOutputToSourceModelTemp;
            for (const auto& entry : mExecutionStep->getTempsAsStepModelOutputs()) {
                operandIndexStepModelOutputToSourceModelTemp.emplace(entry.second, entry.first);
            }

            const uint32_t sourceModelIndex = mExecutionStep->getSourceModelIndex();
            for (uint32_t i = 0, e = mModel->outputCount(); i < e; i++) {
                const uint32_t stepModelOperandIndex = mModel->getOutputOperandIndex(i);
                const auto it =
                        operandIndexStepModelOutputToSourceModelTemp.find(stepModelOperandIndex);
                if (it == operandIndexStepModelOutputToSourceModelTemp.end()) {
                    continue;
                }
                const auto sourceOperandIndex = SourceOperandIndex(sourceModelIndex, it->second);
                VLOG(EXECUTION) << "updateOutputShapes checking to see if output#" << i
                                << " sourceOperandIndex = (" << sourceOperandIndex.first << ", "
                                << sourceOperandIndex.second << ") is a dynamic temporary";
                // This is a temporary, but it might not be a dynamic temporary.
                const auto loc = mDynamicTemporaries->lookup(sourceOperandIndex, false);
                if (loc == std::nullopt) {
                    continue;
                }
                NN_RET_CHECK(isUpdatable(*loc->dimensions, from[i].dimensions));
                bool changedShape = false;
                const uint32_t actualSize = TypeManager::get()->getSizeOfData(
                        mModel->getOperand(stepModelOperandIndex).type, from[i].dimensions);
                if (actualSize > 0) {
                    changedShape = mDynamicTemporaries->redeclare(sourceOperandIndex,
                                                                  from[i].dimensions, actualSize);
                } else if (!from[i].isSufficient) {
                    NN_RET_CHECK(loc->length < UINT32_MAX / 2)
                            << "output#" << i << " length overflow";
                    changedShape = mDynamicTemporaries->redeclare(
                            sourceOperandIndex, from[i].dimensions, 2 * loc->length);
                } else {
                    // The combination of not-fully-specified dimensions
                    // and isSufficient means that we have no
                    // information about whether the size of the dynamic
                    // temporary is adequate.
                    VLOG(EXECUTION) << "updateOutputShapes skipping redeclaration for output#" << i;
                    if (executionResultCode == ANEURALNETWORKS_NO_ERROR) {
                        NN_RET_CHECK(isZeroSizedTensor(executionResultCode, from[i]));
                        // This is a zero-sized tensor, and by
                        // definition, any dynamic temporary is an input
                        // to an execution step.
                        update->zeroSizedInput = true;
                    }
                }
                if (changedShape) {
                    // TODO: find a better place for this comment.
                    //
                    // isUpdatable(a, b) imposes a partial ordering a <=
                    // b.  Every fully specified dimensions vector is an
                    // upper bound of that ordering.  Therefore, any
                    // change in dimensions moves towards an upper
                    // bound, and hence there are a finite number of
                    // such changes possible.
                    //
                    // actualSize can only be computed from dimensions
                    // that are an upper bound.  Therefore, once
                    // actualSize is computed, it will not change.
                    //
                    // If dimensions are not fully specified, and
                    // estimated size changes, it increases.  There is
                    // an upper bound on estimated size to avoid
                    // overflow.
                    //
                    // Therefore, if we retry only when dimensions or
                    // size chage, and we stop retrying if we would
                    // otherwise overflow, we should only retry a finite
                    // number of times.
                    update->updatedDynamicTemporary = true;
                }
            }
            mDynamicTemporaries->vlogDump("finished updateOutputShapes");
        }
    } else {
        NN_RET_CHECK_EQ(from.size(), to->size());
        for (uint32_t i = 0, e = from.size(); i < e; i++) {
            NN_RET_CHECK(isUpdatable(to->at(i).dimensions, from[i].dimensions));
            (*to)[i] = from[i];
        }
    }
    return true;
}

StepExecutor::StepExecutor(ExecutionBuilder* executionBuilder, const ModelBuilder* model,
                           std::shared_ptr<Device> device,
                           std::shared_ptr<RuntimePreparedModel> preparedModel,
                           const ExecutionStep* step, DynamicTemporaries* dynamicTemporaries)
    : mExecutionBuilder(executionBuilder),
      mExecutionStep(step),
      mDynamicTemporaries(dynamicTemporaries),
      mModel(model),
      mDevice(device),
      mPreparedModel(preparedModel),
      mInputs(model->inputCount()),
      mOutputs(model->outputCount()) {
    CHECK(mDevice != nullptr);
    CHECK_EQ(step == nullptr, dynamicTemporaries == nullptr);
    VLOG(EXECUTION) << "StepExecutor::StepExecutor with " << mInputs.size() << " inputs and "
                    << mOutputs.size() << " outputs";
}

bool StepExecutor::areDynamicTemporariesAllocated() const {
    return !mDynamicTemporaries || mDynamicTemporaries->allocated(mExecutionStep->getIndex());
}

void StepExecutor::mapInputsAndOutputsTrivially() {
    mInputs = mExecutionBuilder->mInputs;
    mOutputs = mExecutionBuilder->mOutputs;
    mMemories = mExecutionBuilder->mMemories;
}

void StepExecutor::mapInputOrOutput(const ModelArgumentInfo& builderInputOrOutput,
                                    ModelArgumentInfo* executorInputOrOutput,
                                    const Dimensions* builderDimensions) {
    auto updateDimensions = [executorInputOrOutput, builderDimensions] {
        if (!builderDimensions) {
            return;
        }
        executorInputOrOutput->dimensions() = *builderDimensions;
    };

    *executorInputOrOutput = builderInputOrOutput;
    switch (executorInputOrOutput->state()) {
        default:
            CHECK(false) << "unexpected ModelArgumentInfo::state";
            break;
        case ModelArgumentInfo::HAS_NO_VALUE:
        case ModelArgumentInfo::UNSPECIFIED:
            break;
        case ModelArgumentInfo::POINTER:
            updateDimensions();
            break;
        case ModelArgumentInfo::MEMORY: {
            updateDimensions();
            const uint32_t builderPoolIndex = builderInputOrOutput.locationAndLength().poolIndex;
            const RuntimeMemory* memory = mExecutionBuilder->mMemories[builderPoolIndex];
            const uint32_t executorPoolIndex = mMemories.add(memory);
            executorInputOrOutput->locationAndLength().poolIndex = executorPoolIndex;
            break;
        }
    }
}

int StepExecutor::setInputOrOutputFromMemory(const Operand& inputOrOutputOperand,
                                             const RuntimeMemory* memory, uint32_t offset,
                                             const Dimensions& dimensions,
                                             std::optional<uint32_t> length,
                                             ModelArgumentInfo* inputOrOutputInfo) {
    // Should be similar to
    //     ExecutionBuilder::setInputFromMemory()
    //     ExecutionBuilder::setOutputFromMemory()

    uint32_t poolIndex = mMemories.add(memory);
    uint32_t lengthVal = length.value_or(TypeManager::get()->getSizeOfData(inputOrOutputOperand));
    CHECK(inputOrOutputInfo->unspecified());
    int n;
    std::tie(n, *inputOrOutputInfo) =
            ModelArgumentInfo::createFromMemory(inputOrOutputOperand,
                                                /*type=*/nullptr, poolIndex, offset, lengthVal);
    if (n == ANEURALNETWORKS_NO_ERROR && dimensions.size()) {
        CHECK(isUpdatable(inputOrOutputInfo->dimensions(), dimensions));
        inputOrOutputInfo->dimensions() = dimensions;
    }
    return n;
}

static std::string toString(std::vector<uint32_t> dimensions) {
    std::string ret = "(";
    bool wroteOne = false;
    for (uint32_t dimension : dimensions) {
        if (wroteOne) {
            ret += ", ";
        } else {
            wroteOne = true;
        }
        ret += std::to_string(dimension);
    }
    ret += ")";
    return ret;
};

static void logArguments(const char* kind, const std::vector<ModelArgumentInfo>& args) {
    for (unsigned i = 0; i < args.size(); i++) {
        const auto& arg = args[i];
        std::string prefix = kind + std::string("[") + std::to_string(i) + "] = ";
        switch (arg.state()) {
            case ModelArgumentInfo::POINTER:
                VLOG(EXECUTION) << prefix << "POINTER(" << SHOW_IF_DEBUG(arg.buffer()) << ") dim"
                                << toString(arg.dimensions());
                break;
            case ModelArgumentInfo::MEMORY:
                VLOG(EXECUTION) << prefix << "MEMORY("
                                << "pool=" << arg.locationAndLength().poolIndex << ", "
                                << "off=" << arg.locationAndLength().offset << ") dim"
                                << toString(arg.dimensions());
                break;
            case ModelArgumentInfo::HAS_NO_VALUE:
                VLOG(EXECUTION) << prefix << "HAS_NO_VALUE";
                break;
            case ModelArgumentInfo::UNSPECIFIED:
                VLOG(EXECUTION) << prefix << "UNSPECIFIED";
                break;
            default:
                VLOG(EXECUTION) << prefix << "state(" << arg.state() << ")";
                break;
        }
    }
}

bool StepExecutor::isCpu() const {
    return mDevice == DeviceManager::getCpuDevice();
}

std::tuple<int, std::vector<OutputShape>, Timing> StepExecutor::compute(
        const std::optional<Deadline>& deadline,
        const std::shared_ptr<ExecutionBurstController>& burstController) {
    return computeWithMemories(deadline, mMemories.getObjects(), burstController);
}

std::tuple<int, std::vector<OutputShape>, Timing> StepExecutor::computeWithMemories(
        const std::optional<Deadline>& deadline, const std::vector<const RuntimeMemory*>& memories,
        const std::shared_ptr<ExecutionBurstController>& burstController) {
    CHECK(mPreparedModel != nullptr);

    if (VLOG_IS_ON(EXECUTION)) {
        logArguments("input", mInputs);
        logArguments("output", mOutputs);
    }

    const MeasureTiming measure = measureTiming(mExecutionBuilder);
    const OptionalTimeoutDuration loopTimeoutDuration =
            makeTimeoutDuration(mExecutionBuilder->getLoopTimeoutDuration());
    const auto [n, outputShapes, timing] = mPreparedModel->execute(
            mInputs, mOutputs, memories, burstController, measure, deadline, loopTimeoutDuration);
    mExecutionBuilder->reportTimingWithoutFencedExecutionCallback(timing);

    return {n, std::move(outputShapes), timing};
}

std::tuple<int, int, sp<V1_3::IFencedExecutionCallback>> StepExecutor::computeFenced(
        const std::vector<int>& waitFor, uint64_t timeoutDurationAfterFence,
        const std::optional<Deadline>& deadline) {
    CHECK(mPreparedModel != nullptr);

    if (VLOG_IS_ON(EXECUTION)) {
        logArguments("input", mInputs);
        logArguments("output", mOutputs);
    }

    const MeasureTiming measure = measureTiming(mExecutionBuilder);
    const OptionalTimeoutDuration loopTimeoutDuration =
            makeTimeoutDuration(mExecutionBuilder->getLoopTimeoutDuration());
    OptionalTimeoutDuration optionalTimeoutDurationAfterFence;
    if (timeoutDurationAfterFence > 0) {
        optionalTimeoutDurationAfterFence = makeTimeoutDuration(timeoutDurationAfterFence);
    }
    const auto [n, syncFence, computeFencedCallback, timing] = mPreparedModel->executeFenced(
            mInputs, mOutputs, mMemories.getObjects(), waitFor, measure, deadline,
            loopTimeoutDuration, optionalTimeoutDurationAfterFence);
    if (syncFence < 0 && computeFencedCallback == nullptr) {
        mExecutionBuilder->reportTimingWithoutFencedExecutionCallback(timing);
    }
    return {n, syncFence, computeFencedCallback};
}

// For cpuFallback{Partial,Full}, recompile the model on CPU and then start compute.
std::tuple<int, std::vector<OutputShape>, Timing> StepExecutor::computeOnCpuFallback() {
    NNTRACE_RT(NNTRACE_PHASE_EXECUTION, "StepExecutor::computeOnCpuFallback");
    VLOG(EXECUTION) << "Re-compile the model on CPU";
    mDevice = DeviceManager::getCpuDevice();
    mPreparedModel = nullptr;
    const ModelFactory makeModel = [this] { return mModel->makeModel(); };
    // TODO: Propagate user preference and compilation priority to this point instead of using
    // default values of ANEURALNETWORKS_PREFER_FAST_SINGLE_ANSWER and
    // ANEURALNETWORKS_PRIORITY_MEDIUM
    const ExecutionPreference preference =
            static_cast<ExecutionPreference>(ANEURALNETWORKS_PREFER_FAST_SINGLE_ANSWER);
    const Priority priority = convertToCanonicalPriority(ANEURALNETWORKS_PRIORITY_DEFAULT);
    auto [n, preparedModel] = mDevice->prepareModel(makeModel, preference, priority, {}, {}, {});
    mPreparedModel = std::move(preparedModel);
    if (n != ANEURALNETWORKS_NO_ERROR) {
        return {n, {}, {}};
    }

    // Prepare device memories for CPU fallback.
    std::vector<const RuntimeMemory*> memories = mMemories.getObjects();
    std::vector<bool> isUsedAsInput(memories.size(), false);
    std::vector<bool> isUsedAsOutput(memories.size(), false);
    std::vector<std::unique_ptr<RuntimeMemory>> blobAhwbs;

    // Mark the input and output usages.
    for (auto& input : mInputs) {
        if (input.state() == ModelArgumentInfo::MEMORY) {
            const uint32_t poolIndex = input.locationAndLength().poolIndex;
            isUsedAsInput[poolIndex] = true;
        }
    }
    for (auto& output : mOutputs) {
        if (output.state() == ModelArgumentInfo::MEMORY) {
            const uint32_t poolIndex = output.locationAndLength().poolIndex;
            // Cannot allocate output buffers with unknown shapes.
            if (mMemories[poolIndex]->getValidator().createdWithUnknownShape()) {
                LOG(ERROR) << "Cannot fallback to CPU because at least one of the output operands "
                              "has unknown shape.";
                return {ANEURALNETWORKS_OP_FAILED, {}, {}};
            }
            isUsedAsOutput[poolIndex] = true;
        }
    }

    // Allocate BLOB mode AHardwareBuffers and read the data from input device memories.
    for (uint32_t i = 0; i < memories.size(); i++) {
        const RuntimeMemory* memory = mMemories[i];
        if (memory->getIBuffer() != nullptr) {
            const uint32_t size = memory->getValidator().getMetadata().logicalSize;
            auto [nAhwb, blobAhwb] = MemoryRuntimeAHWB::create(size);
            if (nAhwb != ANEURALNETWORKS_NO_ERROR) {
                return {nAhwb, {}, {}};
            }
            if (isUsedAsInput[i]) {
                n = copyIBufferToHidlMemory(memory->getIBuffer(), blobAhwb->getHidlMemory());
                if (n != ANEURALNETWORKS_NO_ERROR) {
                    return {n, {}, {}};
                }
            }
            memories[i] = blobAhwb.get();
            blobAhwbs.push_back(std::move(blobAhwb));
        }
    }

    auto [nCompute, outputShapes, timing] = computeWithMemories({}, memories);
    if (nCompute != ANEURALNETWORKS_NO_ERROR) {
        return {nCompute, std::move(outputShapes), timing};
    }

    // Write back to output device memories.
    for (uint32_t i = 0; i < memories.size(); i++) {
        const RuntimeMemory* memory = mMemories[i];
        if (memory->getIBuffer() != nullptr && isUsedAsOutput[i]) {
            n = copyHidlMemoryToIBuffer(memories[i]->getHidlMemory(), memory->getIBuffer(), {});
            if (n != ANEURALNETWORKS_NO_ERROR) {
                return {n, {}, {}};
            }
        }
    }
    return {ANEURALNETWORKS_NO_ERROR, std::move(outputShapes), timing};
}

}  // namespace nn
}  // namespace android
