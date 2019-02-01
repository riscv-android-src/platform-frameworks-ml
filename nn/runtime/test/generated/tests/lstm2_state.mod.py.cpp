// clang-format off
// Generated file (from: lstm2_state.mod.py). Do not edit
#include "../../TestGenerated.h"

namespace lstm2_state {
// Generated lstm2_state test
#include "generated/examples/lstm2_state.example.cpp"
// Generated model constructor
#include "generated/models/lstm2_state.model.cpp"
} // namespace lstm2_state

TEST_F(GeneratedTests, lstm2_state) {
    execute(lstm2_state::CreateModel,
            lstm2_state::is_ignored,
            lstm2_state::get_examples());
}

#if 0
TEST_F(DynamicOutputShapeTests, lstm2_state_dynamic_output_shape) {
    execute(lstm2_state::CreateModel_dynamic_output_shape,
            lstm2_state::is_ignored_dynamic_output_shape,
            lstm2_state::get_examples_dynamic_output_shape());
}

#endif
