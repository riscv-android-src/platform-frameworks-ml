// clang-format off
// Generated file (from: strided_slice_float_3.mod.py). Do not edit
#include "../../TestGenerated.h"

namespace strided_slice_float_3 {
// Generated strided_slice_float_3 test
#include "generated/examples/strided_slice_float_3.example.cpp"
// Generated model constructor
#include "generated/models/strided_slice_float_3.model.cpp"
} // namespace strided_slice_float_3

TEST_F(GeneratedTests, strided_slice_float_3) {
    execute(strided_slice_float_3::CreateModel,
            strided_slice_float_3::is_ignored,
            strided_slice_float_3::get_examples());
}

