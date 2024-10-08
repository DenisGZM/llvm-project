//===- RankReductionPatterns.cpp - Patterns related to rank reductions ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/Dialect/Tensor/Transforms/Transforms.h"
#include "mlir/IR/PatternMatch.h"
#include "llvm/Support/Debug.h"

using namespace mlir;
using namespace mlir::tensor;

namespace {
/// Fold expand_shape(extract_slice) ops that cancel itself out.
struct FoldExpandOfRankReducingExtract
    : public OpRewritePattern<ExpandShapeOp> {
  using OpRewritePattern<ExpandShapeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(ExpandShapeOp expandShapeOp,
                                PatternRewriter &rewriter) const override {
    RankedTensorType resultType = expandShapeOp.getResultType();
    auto extractSliceOp =
        expandShapeOp.getSrc().getDefiningOp<ExtractSliceOp>();
    if (!extractSliceOp)
      return failure();
    RankedTensorType srcType = extractSliceOp.getSourceType();

    // Only cases where the ExpandShapeOp can be folded away entirely are
    // supported. Moreover, only simple cases where the resulting ExtractSliceOp
    // has no rank-reduction anymore are supported at the moment.
    RankedTensorType nonReducingExtractType = ExtractSliceOp::inferResultType(
        srcType, extractSliceOp.getStaticOffsets(),
        extractSliceOp.getStaticSizes(), extractSliceOp.getStaticStrides());
    if (nonReducingExtractType != resultType)
      return failure();

    SmallVector<OpFoldResult> mixedOffsets = extractSliceOp.getMixedOffsets();
    SmallVector<OpFoldResult> mixedSizes = extractSliceOp.getMixedSizes();
    SmallVector<OpFoldResult> mixedStrides = extractSliceOp.getMixedStrides();
    rewriter.replaceOpWithNewOp<tensor::ExtractSliceOp>(
        expandShapeOp, extractSliceOp.getSource(), mixedOffsets, mixedSizes,
        mixedStrides);
    return success();
  }
};

/// Fold collapse_shape which only removes static dimensions of size `1`
/// into extract_slice.
struct FoldUnPaddingCollapseIntoExtract
    : public OpRewritePattern<tensor::CollapseShapeOp> {
  using OpRewritePattern<tensor::CollapseShapeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::CollapseShapeOp collapseShapeOp,
                                PatternRewriter &rewriter) const override {
    auto extractSliceOp =
        collapseShapeOp.getSrc().getDefiningOp<tensor::ExtractSliceOp>();
    // Collapse cannot be folded away with multiple users of the extract slice
    // and it is not necessarily beneficial to only convert the collapse into
    // another extract slice.
    if (!extractSliceOp || !extractSliceOp->hasOneUse())
      return failure();

    // Only fold away simple collapse where all removed dimensions have static
    // size `1`.
    SliceVerificationResult res = isRankReducedType(
        collapseShapeOp.getSrcType(), collapseShapeOp.getResultType());
    if (res != SliceVerificationResult::Success)
      return rewriter.notifyMatchFailure(collapseShapeOp,
                                         "expected unpadding collapse");

    Value unPaddedExtractSlice = rewriter.create<tensor::ExtractSliceOp>(
        extractSliceOp.getLoc(), collapseShapeOp.getResultType(),
        extractSliceOp.getSource(), extractSliceOp.getMixedOffsets(),
        extractSliceOp.getMixedSizes(), extractSliceOp.getMixedStrides());
    rewriter.replaceOp(collapseShapeOp, unPaddedExtractSlice);
    return success();
  }
};

/// Fold insert_slice(collapse_shape) ops that cancel itself out.
template <typename OpTy>
struct FoldInsertOfRankReducingInsert : public OpRewritePattern<OpTy> {
  using OpRewritePattern<OpTy>::OpRewritePattern;

  LogicalResult matchAndRewrite(OpTy insertSliceOp,
                                PatternRewriter &rewriter) const override {
    auto collapseShapeOp =
        insertSliceOp.getSource().template getDefiningOp<CollapseShapeOp>();
    if (!collapseShapeOp)
      return failure();
    RankedTensorType srcType = collapseShapeOp.getSrcType();

    // Only cases where the CollapseShapeOp can be folded away entirely are
    // supported. Moreover, only simple cases where the resulting InsertSliceOp
    // has no rank-reduction anymore are supported at the moment.
    RankedTensorType nonReducingInsertType =
        RankedTensorType::get(insertSliceOp.getStaticSizes(),
                              insertSliceOp.getDestType().getElementType());
    if (nonReducingInsertType != srcType)
      return failure();

    SmallVector<OpFoldResult> mixedOffsets = insertSliceOp.getMixedOffsets();
    SmallVector<OpFoldResult> mixedSizes = insertSliceOp.getMixedSizes();
    SmallVector<OpFoldResult> mixedStrides = insertSliceOp.getMixedStrides();
    rewriter.replaceOpWithNewOp<OpTy>(insertSliceOp, collapseShapeOp.getSrc(),
                                      insertSliceOp.getDest(), mixedOffsets,
                                      mixedSizes, mixedStrides);
    return success();
  }
};

/// Fold expand_shape which only adds static dimensions of size `1`
/// into insert_slice.
template <typename OpTy>
struct FoldPaddingExpandIntoInsert : public OpRewritePattern<OpTy> {
  using OpRewritePattern<OpTy>::OpRewritePattern;

  LogicalResult matchAndRewrite(OpTy insertSliceOp,
                                PatternRewriter &rewriter) const override {
    auto expandShapeOp = insertSliceOp.getSource()
                             .template getDefiningOp<tensor::ExpandShapeOp>();
    if (!expandShapeOp)
      return failure();

    // Only fold away simple expansion where all added dimensions have static
    // size `1`.
    SliceVerificationResult res = isRankReducedType(
        expandShapeOp.getResultType(), expandShapeOp.getSrcType());
    if (res != SliceVerificationResult::Success)
      return rewriter.notifyMatchFailure(insertSliceOp,
                                         "expected rank increasing expansion");

    rewriter.modifyOpInPlace(insertSliceOp, [&]() {
      insertSliceOp.getSourceMutable().assign(expandShapeOp.getSrc());
    });
    return success();
  }
};

/// Pattern to bubble up a tensor.expand_shape op through a producer
/// tensor.collapse_shape op that has non intersecting reassociations.
struct BubbleUpExpandThroughParallelCollapse
    : public OpRewritePattern<tensor::ExpandShapeOp> {
  using OpRewritePattern<tensor::ExpandShapeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::ExpandShapeOp expandOp,
                                PatternRewriter &rewriter) const override {
    auto collapseOp =
        expandOp.getSrc().getDefiningOp<tensor::CollapseShapeOp>();
    if (!collapseOp)
      return failure();
    auto expandReInds = expandOp.getReassociationIndices();
    auto collapseReInds = collapseOp.getReassociationIndices();

    // Reshapes are parallel to each other if none of the reassociation indices
    // have greater than 1 index for both reshapes.
    for (auto [expandReassociation, collapseReassociation] :
         llvm::zip_equal(expandReInds, collapseReInds)) {
      if (collapseReassociation.size() != 1 && expandReassociation.size() != 1)
        return failure();
    }

    // Compute new reassociation indices and expanded/collaped shapes.
    SmallVector<ReassociationIndices> newExpandReInds, newCollapseReInds;
    Location loc = expandOp->getLoc();
    SmallVector<OpFoldResult> collapseSizes =
        tensor::getMixedSizes(rewriter, loc, collapseOp.getSrc());
    SmallVector<OpFoldResult> expandSizes(getMixedValues(
        expandOp.getStaticOutputShape(), expandOp.getOutputShape(), rewriter));
    SmallVector<OpFoldResult> newExpandSizes;
    int64_t index = 0, expandIndex = 0, collapseIndex = 0;
    for (auto [idx, collapseReassociation] : llvm::enumerate(collapseReInds)) {
      if (collapseReassociation.size() != 1) {
        ReassociationIndices newCollapseReassociation;
        for (size_t i = 0; i < collapseReassociation.size(); ++i) {
          newCollapseReassociation.push_back(index);
          newExpandReInds.push_back({index++});
          newExpandSizes.push_back(collapseSizes[collapseIndex++]);
        }
        newCollapseReInds.push_back(newCollapseReassociation);
        expandIndex++;
        continue;
      }
      ReassociationIndices newExpandReassociation;
      auto expandReassociation = expandReInds[idx];
      for (size_t i = 0; i < expandReassociation.size(); ++i) {
        newExpandReassociation.push_back(index);
        newCollapseReInds.push_back({index++});
        newExpandSizes.push_back(expandSizes[expandIndex++]);
      }
      newExpandReInds.push_back(newExpandReassociation);
      collapseIndex++;
    }

    // Swap reshape order.
    SmallVector<Value> dynamicSizes;
    SmallVector<int64_t> staticSizes;
    dispatchIndexOpFoldResults(newExpandSizes, dynamicSizes, staticSizes);
    auto expandResultType = expandOp.getResultType().clone(staticSizes);
    auto newExpand = rewriter.create<tensor::ExpandShapeOp>(
        loc, expandResultType, collapseOp.getSrc(), newExpandReInds,
        newExpandSizes);
    rewriter.replaceOpWithNewOp<tensor::CollapseShapeOp>(
        expandOp, newExpand.getResult(), newCollapseReInds);
    return success();
  }
};

} // namespace

void mlir::tensor::populateReassociativeReshapeFoldingPatterns(
    RewritePatternSet &patterns) {
  patterns
      .add<FoldExpandOfRankReducingExtract, FoldUnPaddingCollapseIntoExtract,
           FoldInsertOfRankReducingInsert<tensor::InsertSliceOp>,
           FoldInsertOfRankReducingInsert<tensor::ParallelInsertSliceOp>,
           FoldPaddingExpandIntoInsert<tensor::InsertSliceOp>,
           FoldPaddingExpandIntoInsert<tensor::ParallelInsertSliceOp>>(
          patterns.getContext());
}

void mlir::tensor::populateBubbleUpExpandShapePatterns(
    RewritePatternSet &patterns) {
  patterns.add<BubbleUpExpandThroughParallelCollapse>(patterns.getContext());
}
