// Generated file (from: softmax_float_2.mod.py). Do not edit
void CreateModel(Model *model) {
  OperandType type1(Type::FLOAT32, {});
  OperandType type0(Type::TENSOR_FLOAT32, {2, 5});
  // Phase 1, operands
  auto input = model->addOperand(&type0);
  auto beta = model->addOperand(&type1);
  auto output = model->addOperand(&type0);
  // Phase 2, operations
  static float beta_init[] = {1.0f};
  model->setOperandValue(beta, beta_init, sizeof(float) * 1);
  model->addOperation(ANEURALNETWORKS_SOFTMAX, {input, beta}, {output});
  // Phase 3, inputs and outputs
  model->identifyInputsAndOutputs(
    {input},
    {output});
  assert(model->isValid());
  model->finish();
}

bool is_ignored(int i) {
  static std::set<int> ignore = {};
  return ignore.find(i) != ignore.end();
}
