/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

// This file implements logic for lowering XLA dialect to Standard dialect.

#include "llvm/ADT/StringSwitch.h"
#include "mlir/Dialect/StandardOps/Ops.h"  // TF:llvm-project
#include "mlir/IR/Function.h"  // TF:llvm-project
#include "mlir/IR/PatternMatch.h"  // TF:llvm-project
#include "mlir/Pass/Pass.h"  // TF:llvm-project
#include "tensorflow/compiler/mlir/xla/ir/hlo_ops.h"
#include "tensorflow/compiler/mlir/xla/transforms/passes.h"
#include "tensorflow/compiler/mlir/xla/transforms/rewriters.h"

namespace mlir {
namespace {
#include "tensorflow/compiler/mlir/xla/transforms/generated_legalize_to_standard.inc"
}  // end anonymous namespace
namespace xla_hlo {
namespace {

class CompareIConvert : public OpRewritePattern<xla_hlo::CompareOp> {
 public:
  using OpRewritePattern::OpRewritePattern;

  PatternMatchResult matchAndRewrite(xla_hlo::CompareOp op,
                                     PatternRewriter &rewriter) const override {
    auto lhs = op.lhs();
    auto rhs = op.rhs();
    auto lhs_type = lhs.getType().cast<TensorType>();
    auto rhs_type = rhs.getType().cast<TensorType>();

    // Broadcasting not supported by this rewrite.
    if (lhs_type.getShape() != rhs_type.getShape()) return matchFailure();

    if (!lhs_type.getElementType().isa<IntegerType>() ||
        !rhs_type.getElementType().isa<IntegerType>())
      return matchFailure();

    auto comparison_direction = op.comparison_direction();
    auto compare_predicate =
        llvm::StringSwitch<Optional<CmpIPredicate>>(comparison_direction)
            .Case("EQ", CmpIPredicate::eq)
            .Case("NE", CmpIPredicate::ne)
            .Case("LT", CmpIPredicate::slt)
            .Case("LE", CmpIPredicate::sle)
            .Case("GT", CmpIPredicate::sgt)
            .Case("GE", CmpIPredicate::sge)
            .Default(llvm::None);

    if (!compare_predicate.hasValue()) return matchFailure();

    rewriter.replaceOpWithNewOp<CmpIOp>(op, compare_predicate.getValue(), lhs,
                                        rhs);
    return matchSuccess();
  }
};

class CompareFConvert : public OpRewritePattern<xla_hlo::CompareOp> {
 public:
  using OpRewritePattern::OpRewritePattern;

  PatternMatchResult matchAndRewrite(xla_hlo::CompareOp op,
                                     PatternRewriter &rewriter) const override {
    auto lhs = op.lhs();
    auto rhs = op.rhs();
    auto lhs_type = lhs.getType().cast<TensorType>();
    auto rhs_type = rhs.getType().cast<TensorType>();

    // Broadcasting not supported by this rewrite.
    if (lhs_type.getShape() != rhs_type.getShape()) return matchFailure();

    if (!lhs_type.getElementType().isa<FloatType>() ||
        !rhs_type.getElementType().isa<FloatType>())
      return matchFailure();

    auto comparison_direction = op.comparison_direction();
    CmpFPredicate compare_predicate =
        llvm::StringSwitch<CmpFPredicate>(comparison_direction)
            .Case("EQ", CmpFPredicate::OEQ)
            .Case("NE", CmpFPredicate::UNE)
            .Case("LT", CmpFPredicate::OLT)
            .Case("LE", CmpFPredicate::OLE)
            .Case("GT", CmpFPredicate::OGT)
            .Case("GE", CmpFPredicate::OGE)
            .Default(CmpFPredicate::NumPredicates);

    if (compare_predicate == CmpFPredicate::NumPredicates)
      return matchFailure();

    rewriter.replaceOpWithNewOp<CmpFOp>(op, compare_predicate, lhs, rhs);
    return matchSuccess();
  }
};

class ConvertIotaOp : public OpRewritePattern<xla_hlo::IotaOp> {
 public:
  using OpRewritePattern::OpRewritePattern;

  PatternMatchResult matchAndRewrite(xla_hlo::IotaOp op,
                                     PatternRewriter &rewriter) const override {
    auto output_type = op.getType().cast<ShapedType>();
    // TODO(prakalps): Handle FP and ComplexType iota ops.
    if (!output_type.getElementType().isa<IntegerType>()) return matchFailure();
    auto output_size = output_type.getNumElements();
    auto dimension = op.iota_dimension().getSExtValue();
    auto max_dim_size = output_type.getDimSize(dimension);
    int bitwidth = output_type.getElementType().getIntOrFloatBitWidth();

    llvm::SmallVector<APInt, 10> values;
    values.reserve(output_size);

    int64_t increase_stride = output_size;
    for (int i = 0; i <= dimension; i++) {
      increase_stride /= output_type.getDimSize(i);
    }

    int64_t current_value = 0;
    for (int i = 0; i < output_size; i++) {
      int64_t value = (current_value / increase_stride) % max_dim_size;
      values.push_back(APInt(bitwidth, value));
      ++current_value;
    }

    rewriter.replaceOpWithNewOp<mlir::ConstantOp>(
        op, DenseIntElementsAttr::get(output_type, values));
    return matchSuccess();
  }
};

}  // end anonymous namespace

namespace {
struct LegalizeToStandard : public FunctionPass<LegalizeToStandard> {
  /// Perform the lowering to Standard dialect.
  void runOnFunction() override;
};
}  // end anonymous namespace

std::unique_ptr<mlir::OpPassBase<mlir::FuncOp>> createLegalizeToStdPass() {
  return std::make_unique<LegalizeToStandard>();
}

void PopulateXlaToStdPatterns(OwningRewritePatternList *patterns,
                              mlir::MLIRContext *ctx) {
  mlir::populateWithGenerated(ctx, patterns);
  patterns->insert<CompareFConvert, CompareIConvert, ConvertIotaOp>(ctx);
}

/// Perform the lowering to standard dialect.
void LegalizeToStandard::runOnFunction() {
  OwningRewritePatternList patterns;
  mlir::xla_hlo::PopulateXlaToStdPatterns(&patterns, &getContext());
  applyPatternsGreedily(getFunction(), patterns);
}

static PassRegistration<LegalizeToStandard> legalize_pass(
    "xla-legalize-to-std", "Legalize from XLA dialect to standard dialect");

}  // end namespace xla_hlo
}  // end namespace mlir
