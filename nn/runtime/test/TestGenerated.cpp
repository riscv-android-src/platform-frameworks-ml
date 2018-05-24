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

// Top level driver for models and examples generated by test_generator.py

#include "NeuralNetworksWrapper.h"
#include "TestHarness.h"

#include <gtest/gtest.h>
#include <cassert>
#include <cmath>
#include <iostream>
#include <map>

namespace generated_tests {
using namespace android::nn::wrapper;

template <typename T>
class Example {
   public:
    typedef T ElementType;
    typedef std::pair<std::map<int, std::vector<T>>,
                      std::map<int, std::vector<T>>>
        ExampleType;

    static bool Execute(std::function<void(Model*)> create_model,
                        std::vector<ExampleType>& examples,
                        std::function<bool(const T, const T)> compare) {
        Model model;
        create_model(&model);

        int example_no = 1;
        bool error = false;
        for (auto& example : examples) {
            Compilation compilation(&model);
            compilation.finish();
            Execution execution(&compilation);

            // Go through all inputs
            for (auto& i : example.first) {
                std::vector<T>& input = i.second;
                // We interpret an empty vector as an optional argument
                // that has been omitted.
                if (input.size() == 0) {
                    execution.setInput(i.first, nullptr, 0);
                } else {
                    execution.setInput(i.first, (const void*)input.data(),
                                       input.size() * sizeof(T));
                }
            }

            std::map<int, std::vector<T>> test_outputs;

            assert(example.second.size() == 1);
            int output_no = 0;
            for (auto& i : example.second) {
                std::vector<T>& output = i.second;
                test_outputs[i.first].resize(output.size());
                std::vector<T>& test_output = test_outputs[i.first];
                execution.setOutput(output_no++, (void*)test_output.data(),
                                    test_output.size() * sizeof(T));
            }
            Result r = execution.compute();
            if (r != Result::NO_ERROR)
                std::cerr << "Execution was not completed normally\n";
            bool mismatch = false;
            for (auto& i : example.second) {
                const std::vector<T>& test = test_outputs[i.first];
                const std::vector<T>& golden = i.second;
                for (unsigned i = 0; i < golden.size(); i++) {
                    if (compare(golden[i], test[i])) {
                        std::cerr << " output[" << i << "] = " << (float)test[i]
                                  << " (should be " << (float)golden[i]
                                  << ")\n";
                        error = error || true;
                        mismatch = mismatch || true;
                    }
                }
            }
            if (mismatch) {
                std::cerr << "Example: " << example_no++;
                std::cerr << " failed\n";
            }
        }
        return error;
    }

    // Test driver for those generated from ml/nn/runtime/test/spec
    static void Execute(std::function<void(Model*)> create_model,
                        std::function<bool(int)> is_ignored,
                        std::vector<MixedTypedExampleType>& examples) {
        Model model;
        create_model(&model);

        int example_no = 1;
        for (auto& example : examples) {
            SCOPED_TRACE(example_no++);
            MixedTyped inputs = example.first;
            const MixedTyped& golden = example.second;

            Compilation compilation(&model);
            compilation.finish();
            Execution execution(&compilation);

            // Set all inputs
            for_all(inputs, [&execution](int idx, const void* p, size_t s) {
                ASSERT_EQ(Result::NO_ERROR, execution.setInput(idx, p, s));
            });

            MixedTyped test;
            // Go through all typed outputs
            resize_accordingly(golden, test);
            for_all(test, [&execution](int idx, void* p, size_t s) {
                ASSERT_EQ(Result::NO_ERROR, execution.setOutput(idx, p, s));
            });

            Result r = execution.compute();
            ASSERT_EQ(Result::NO_ERROR, r);
            // Filter out don't cares
            MixedTyped filtered_golden = filter(golden, is_ignored);
            MixedTyped filtered_test = filter(test, is_ignored);
            // We want "close-enough" results for float
            compare(filtered_golden, filtered_test);
        }
    }
};
};  // namespace generated_tests

using namespace android::nn::wrapper;
// Float32 examples
typedef generated_tests::Example<float>::ExampleType Example;
// Mixed-typed examples
typedef generated_tests::MixedTypedExampleType MixedTypedExample;

void Execute(std::function<void(Model*)> create_model,
             std::function<bool(int)> is_ignored,
             std::vector<MixedTypedExample>& examples) {
    generated_tests::Example<float>::Execute(create_model, is_ignored,
                                             examples);
}

class GeneratedTests : public ::testing::Test {
   protected:
    virtual void SetUp() {}
};

// Testcases generated from runtime/test/specs/*.mod.py
#include "generated/all_generated_tests.cpp"
// End of testcases generated from runtime/test/specs/*.mod.py

// Below are testcases geneated from TFLite testcases.
namespace conv_1_h3_w2_SAME {
std::vector<Example> examples = {
// Converted examples
#include "generated/examples/conv_1_h3_w2_SAME_tests.example.cc"
};
// Generated model constructor
#include "generated/models/conv_1_h3_w2_SAME.model.cpp"
}  // namespace conv_1_h3_w2_SAME

namespace conv_1_h3_w2_VALID {
std::vector<Example> examples = {
// Converted examples
#include "generated/examples/conv_1_h3_w2_VALID_tests.example.cc"
};
// Generated model constructor
#include "generated/models/conv_1_h3_w2_VALID.model.cpp"
}  // namespace conv_1_h3_w2_VALID

namespace conv_3_h3_w2_SAME {
std::vector<Example> examples = {
// Converted examples
#include "generated/examples/conv_3_h3_w2_SAME_tests.example.cc"
};
// Generated model constructor
#include "generated/models/conv_3_h3_w2_SAME.model.cpp"
}  // namespace conv_3_h3_w2_SAME

namespace conv_3_h3_w2_VALID {
std::vector<Example> examples = {
// Converted examples
#include "generated/examples/conv_3_h3_w2_VALID_tests.example.cc"
};
// Generated model constructor
#include "generated/models/conv_3_h3_w2_VALID.model.cpp"
}  // namespace conv_3_h3_w2_VALID

namespace depthwise_conv {
std::vector<Example> examples = {
// Converted examples
#include "generated/examples/depthwise_conv_tests.example.cc"
};
// Generated model constructor
#include "generated/models/depthwise_conv.model.cpp"
}  // namespace depthwise_conv

namespace mobilenet {
std::vector<Example> examples = {
// Converted examples
#include "generated/examples/mobilenet_224_gender_basic_fixed_tests.example.cc"
};
// Generated model constructor
#include "generated/models/mobilenet_224_gender_basic_fixed.model.cpp"
}  // namespace mobilenet

namespace {
bool Execute(std::function<void(Model*)> create_model,
             std::vector<Example>& examples) {
    return generated_tests::Example<float>::Execute(
        create_model, examples, [](float golden, float test) {
            return std::fabs(golden - test) > 1.5e-5f;
        });
}
}  // namespace

TEST_F(GeneratedTests, conv_1_h3_w2_SAME) {
    ASSERT_EQ(
        Execute(conv_1_h3_w2_SAME::CreateModel, conv_1_h3_w2_SAME::examples),
        0);
}

TEST_F(GeneratedTests, conv_1_h3_w2_VALID) {
    ASSERT_EQ(
        Execute(conv_1_h3_w2_VALID::CreateModel, conv_1_h3_w2_VALID::examples),
        0);
}

TEST_F(GeneratedTests, conv_3_h3_w2_SAME) {
    ASSERT_EQ(
        Execute(conv_3_h3_w2_SAME::CreateModel, conv_3_h3_w2_SAME::examples),
        0);
}

TEST_F(GeneratedTests, conv_3_h3_w2_VALID) {
    ASSERT_EQ(
        Execute(conv_3_h3_w2_VALID::CreateModel, conv_3_h3_w2_VALID::examples),
        0);
}

TEST_F(GeneratedTests, depthwise_conv) {
    ASSERT_EQ(Execute(depthwise_conv::CreateModel, depthwise_conv::examples),
              0);
}

TEST_F(GeneratedTests, mobilenet) {
    ASSERT_EQ(Execute(mobilenet::CreateModel, mobilenet::examples), 0);
}
