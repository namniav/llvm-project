//===- CodegenEnv.cpp -  Code generation environment class ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "CodegenEnv.h"

#include "mlir/Dialect/Bufferization/IR/Bufferization.h"
#include "mlir/Dialect/Linalg/Utils/Utils.h"
#include "mlir/Dialect/SparseTensor/IR/SparseTensorType.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"

#include <optional>

using namespace mlir;
using namespace mlir::sparse_tensor;

//===----------------------------------------------------------------------===//
// Code generation environment helper functions
//===----------------------------------------------------------------------===//

/// Returns true if tensor materializes uninitialized into the computation.
static bool isMaterializing(Value val) {
  return val.getDefiningOp<tensor::EmptyOp>() ||
         val.getDefiningOp<bufferization::AllocTensorOp>();
}

//===----------------------------------------------------------------------===//
// Code generation environment constructor and general methods
//===----------------------------------------------------------------------===//

CodegenEnv::CodegenEnv(linalg::GenericOp linop, SparsificationOptions opts,
                       unsigned numTensors, unsigned numLoops,
                       unsigned numFilterLoops)
    : linalgOp(linop), sparseOptions(opts),
      latticeMerger(numTensors, numLoops, numFilterLoops), loopEmitter(),
      topSort(), sparseOut(nullptr), outerParNest(-1u), insChain(), expValues(),
      expFilled(), expAdded(), expCount(), redVal(), redExp(kInvalidId),
      redCustom(kInvalidId), redValidLexInsert() {}

LogicalResult CodegenEnv::initTensorExp() {
  // Builds the tensor expression for the Linalg operation in SSA form.
  std::optional<ExprId> optExp = latticeMerger.buildTensorExpFromLinalg(op());
  if (!optExp || !isAdmissibleTensorExp(*optExp))
    return failure();

  tensorExp = *optExp;
  return success();
}

void CodegenEnv::startEmit() {
  assert(insChain == nullptr && "must only start emitting once");
  if (sparseOut) {
    insChain = sparseOut->get();
    latticeMerger.setHasSparseOut(true);
  }
  // Initialize loop emitter.
  SmallVector<Value> tensors;
  for (OpOperand &t : linalgOp->getOpOperands())
    tensors.push_back(t.get());
  loopEmitter.initialize(tensors,
                         StringAttr::get(linalgOp.getContext(),
                                         linalg::GenericOp::getOperationName()),
                         /*hasOutput=*/true,
                         /*isSparseOut=*/sparseOut != nullptr, topSort);
}

std::optional<Operation *> CodegenEnv::genLoopBoundary(
    function_ref<std::optional<Operation *>(MutableArrayRef<Value> parameters)>
        callback) {
  SmallVector<Value> params;
  if (isReduc()) {
    params.push_back(redVal);
    if (redValidLexInsert)
      params.push_back(redValidLexInsert);
  } else {
    assert(!redValidLexInsert);
  }
  if (isExpand())
    params.push_back(expCount);
  if (insChain != nullptr)
    params.push_back(insChain);
  auto r = callback(params); // may update parameters
  unsigned i = 0;
  if (isReduc()) {
    updateReduc(params[i++]);
    if (redValidLexInsert)
      setValidLexInsert(params[i++]);
  }
  if (isExpand())
    updateExpandCount(params[i++]);
  if (insChain != nullptr)
    updateInsertionChain(params[i]);
  return r;
}

//===----------------------------------------------------------------------===//
// Code generation environment verify functions.
//===----------------------------------------------------------------------===//

bool CodegenEnv::isAdmissibleTensorExp(ExprId exp) {
  // We reject any expression that makes a reduction from `-outTensor`, as those
  // expressions create a dependency between the current iteration (i) and the
  // previous iteration (i-1). It would require iterating over the whole
  // coordinate space, which prevent exploiting sparsity for faster code.
  for (utils::IteratorType it : linalgOp.getIteratorTypesArray()) {
    if (it == utils::IteratorType::reduction) {
      if (latticeMerger.hasNegateOnOut(exp))
        return false;
      break;
    }
  }

  OpOperand *lhs = linalgOp.getDpsInitOperand(0);
  // That the operand number is a valid `TensorId` will be verified
  // by the call to `isSingleCondition` below; though we may want to add
  // assertions to check it here, in order to give better error messages.
  const TensorId tensor = lhs->getOperandNumber();
  // An non-annotated output tensor is assumed dense, and becomes a random
  // access n-dim memref. Admissible since insertions cannot occur.
  if (getSparseTensorType(lhs->get()).isAllDense())
    return true;

  // A tensor expression with a sparse output tensor that changes its values
  // but not its nonzero structure, an operation called "simply dynamic" in
  // [Bik96,Ch9], is also admissible without special env.
  if (latticeMerger.isSingleCondition(tensor, exp))
    return true;

  // Accept "truly dynamic" if the output tensor materializes uninitialized
  // into the computation and insertions occur in lexicographic index order.
  sparseOut = lhs;
  return isMaterializing(lhs->get());
}

bool CodegenEnv::isAdmissibleTopoOrder() {
  if (!hasSparseOutput())
    return true;

  OpOperand *lhs = linalgOp.getDpsInitOperand(0);
  // Accept "truly dynamic" if the output tensor materializes uninitialized
  // into the computation and insertions occur in lexicographic index order.
  LoopOrd nest = 0;
  const auto iteratorTypes = linalgOp.getIteratorTypesArray();
  assert(topSortSize() == latticeMerger.getNumLoops());
  for (const LoopId i : topSort) {
    if (!latticeMerger.isFilterLoop(i)) {
      // We only count non-filter loops as filter loops should be considered
      // a special type of parallel loops.
      if (linalg::isReductionIterator(iteratorTypes[i]))
        break; // terminate at first reduction
      nest++;
    }
  }
  // Determine admissible dynamic insertion situations:
  // (1) fully injective, since there are no reductions,
  // (2) admissible 1-d expansion in innermost dimension.
  if (static_cast<int64_t>(nest) >= linalgOp.getRank(lhs) - 1) {
    outerParNest = nest;
    return true;
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Code generation environment topological sort methods
//===----------------------------------------------------------------------===//

ArrayRef<LoopId> CodegenEnv::getTopSortSlice(LoopOrd n, LoopOrd m) const {
  return ArrayRef<LoopId>(topSort).slice(n, m);
}

ArrayRef<LoopId> CodegenEnv::getLoopStackUpTo(LoopOrd n) const {
  return ArrayRef<LoopId>(topSort).take_front(n);
}

ArrayRef<LoopId> CodegenEnv::getCurrentLoopStack() const {
  return getLoopStackUpTo(loopEmitter.getCurrentDepth());
}

Value CodegenEnv::getLoopVar(LoopId i) const {
  // TODO: this class should store the inverse of `topSort` so that
  // it can do this conversion directly, instead of searching through
  // `topSort` every time.  (Or else, `LoopEmitter` should handle this.)
  for (LoopOrd n = 0, numLoops = topSortSize(); n < numLoops; n++)
    if (topSort[n] == i)
      return loopEmitter.getLoopIV(n);
  llvm_unreachable("invalid loop identifier");
}

//===----------------------------------------------------------------------===//
// Code generation environment sparse tensor output and expansion methods
//===----------------------------------------------------------------------===//

void CodegenEnv::updateInsertionChain(Value chain) {
  assert(sparseOut != nullptr && insChain != nullptr);
  insChain = chain;
}

// FIXME: clarify what this "rank" is really supposed to mean/be.
bool CodegenEnv::atExpandLevel(OpOperand *o, unsigned rank, LoopOrd n) const {
  return sparseOut == o && outerParNest == static_cast<LoopOrd>(rank - 1) &&
         outerParNest == n;
}

void CodegenEnv::startExpand(Value values, Value filled, Value added,
                             Value count) {
  assert(sparseOut != nullptr && expValues == nullptr);
  expValues = values;
  expFilled = filled;
  expAdded = added;
  expCount = count;
}

void CodegenEnv::updateExpandCount(Value count) {
  assert(sparseOut != nullptr && expValues != nullptr);
  expCount = count;
}

void CodegenEnv::endExpand() {
  assert(sparseOut != nullptr && expValues != nullptr);
  expValues = expFilled = expAdded = expCount = Value();
}

//===----------------------------------------------------------------------===//
// Code generation environment reduction methods
//===----------------------------------------------------------------------===//

void CodegenEnv::startReduc(ExprId exp, Value val) {
  assert(!isReduc() && exp != kInvalidId);
  redExp = exp;
  updateReduc(val);
}

void CodegenEnv::updateReduc(Value val) {
  assert(isReduc());
  redVal = exp(redExp).val = val;
}

Value CodegenEnv::endReduc() {
  Value val = redVal;
  updateReduc(Value());
  redExp = kInvalidId;
  return val;
}

void CodegenEnv::setValidLexInsert(Value val) {
  assert(isReduc() && val);
  redValidLexInsert = val;
}

void CodegenEnv::clearValidLexInsert() {
  assert(!isReduc());
  redValidLexInsert = Value();
}

void CodegenEnv::startCustomReduc(ExprId exp) {
  assert(!isCustomReduc() && exp != kInvalidId);
  redCustom = exp;
}

Value CodegenEnv::getCustomRedId() {
  assert(isCustomReduc());
  return dyn_cast<sparse_tensor::ReduceOp>(exp(redCustom).op).getIdentity();
}

void CodegenEnv::endCustomReduc() {
  assert(isCustomReduc());
  redCustom = kInvalidId;
}
