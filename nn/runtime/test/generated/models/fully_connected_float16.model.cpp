// clang-format off
// Generated file (from: fully_connected_float16.mod.py). Do not edit
void CreateModel(Model *model) {
  OperandType type0(Type::TENSOR_FLOAT16, {3, 1});
  OperandType type1(Type::TENSOR_FLOAT16, {1, 1});
  OperandType type2(Type::TENSOR_FLOAT16, {1});
  OperandType type3(Type::INT32, {});
  // Phase 1, operands
  auto op1 = model->addOperand(&type0);
  auto op2 = model->addOperand(&type1);
  auto b0 = model->addOperand(&type2);
  auto act = model->addOperand(&type3);
  auto op3 = model->addOperand(&type0);
  // Phase 2, operations
  static _Float16 op2_init[] = {2.0f};
  model->setOperandValue(op2, op2_init, sizeof(_Float16) * 1);
  static _Float16 b0_init[] = {4.0f};
  model->setOperandValue(b0, b0_init, sizeof(_Float16) * 1);
  static int32_t act_init[] = {0};
  model->setOperandValue(act, act_init, sizeof(int32_t) * 1);
  model->addOperation(ANEURALNETWORKS_FULLY_CONNECTED, {op1, op2, b0, act}, {op3});
  // Phase 3, inputs and outputs
  model->identifyInputsAndOutputs(
    {op1},
    {op3});
  assert(model->isValid());
}

inline bool is_ignored(int i) {
  static std::set<int> ignore = {};
  return ignore.find(i) != ignore.end();
}

void CreateModel_dynamic_output_shape(Model *model) {
  OperandType type0(Type::TENSOR_FLOAT16, {3, 1});
  OperandType type1(Type::TENSOR_FLOAT16, {1, 1});
  OperandType type2(Type::TENSOR_FLOAT16, {1});
  OperandType type3(Type::INT32, {});
  OperandType type4(Type::TENSOR_FLOAT16, {0, 0});
  // Phase 1, operands
  auto op1 = model->addOperand(&type0);
  auto op2 = model->addOperand(&type1);
  auto b0 = model->addOperand(&type2);
  auto act = model->addOperand(&type3);
  auto op3 = model->addOperand(&type4);
  // Phase 2, operations
  static _Float16 op2_init[] = {2.0f};
  model->setOperandValue(op2, op2_init, sizeof(_Float16) * 1);
  static _Float16 b0_init[] = {4.0f};
  model->setOperandValue(b0, b0_init, sizeof(_Float16) * 1);
  static int32_t act_init[] = {0};
  model->setOperandValue(act, act_init, sizeof(int32_t) * 1);
  model->addOperation(ANEURALNETWORKS_FULLY_CONNECTED, {op1, op2, b0, act}, {op3});
  // Phase 3, inputs and outputs
  model->identifyInputsAndOutputs(
    {op1},
    {op3});
  assert(model->isValid());
}

inline bool is_ignored_dynamic_output_shape(int i) {
  static std::set<int> ignore = {};
  return ignore.find(i) != ignore.end();
}

