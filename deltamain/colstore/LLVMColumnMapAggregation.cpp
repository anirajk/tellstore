/*
 * (C) Copyright 2015 ETH Zurich Systems Group (http://www.systems.ethz.ch/) and others.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Contributors:
 *     Markus Pilman <mpilman@inf.ethz.ch>
 *     Simon Loesing <sloesing@inf.ethz.ch>
 *     Thomas Etter <etterth@gmail.com>
 *     Kevin Bocksrocker <kevin.bocksrocker@gmail.com>
 *     Lucas Braun <braunl@inf.ethz.ch>
 */

#include "LLVMColumnMapAggregation.hpp"

#include "ColumnMapContext.hpp"
#include "LLVMColumnMapUtils.hpp"

#include <util/ScanQuery.hpp>

namespace tell {
namespace store {
namespace deltamain {

LLVMColumnMapAggregationBuilder::LLVMColumnMapAggregationBuilder(const ColumnMapContext& context, llvm::Module& module,
        llvm::TargetMachine* target, const std::string& name)
        : FunctionBuilder(module, target, buildReturnTy(module.getContext()), buildParamTy(module.getContext()), name),
          mContext(context),
          mMainPageStructTy(getColumnMapMainPageTy(module.getContext())),
          mRegisterWidth(mTargetInfo.getRegisterBitWidth(true)) {
    // Set noalias hints (data pointers are not allowed to overlap)
    mFunction->setDoesNotAlias(1);
    mFunction->setOnlyReadsMemory(1);
    mFunction->setDoesNotAlias(4);
    mFunction->setOnlyReadsMemory(4);
    mFunction->setDoesNotAlias(5);
}

void LLVMColumnMapAggregationBuilder::build(ScanQuery* query) {
    auto& srcRecord = mContext.record();
    auto& destRecord = query->record();

    // -> auto mainPage = reinterpret_cast<const ColumnMapMainPage*>(page);
    auto mainPage = CreateBitCast(getParam(page), mMainPageStructTy->getPointerTo());

    // -> auto count = static_cast<uint64_t>(mainPage->count);
    auto count = CreateInBoundsGEP(mainPage, { getInt64(0), getInt32(0) });
    count = CreateZExt(CreateAlignedLoad(count, 4u), getInt64Ty());

    llvm::Value* headerData = nullptr;
    if (!destRecord.allNotNull()) {
        // -> auto headerOffset = static_cast<uint64_t>(mainPage->headerOffset);
        auto headerOffset = CreateInBoundsGEP(mainPage, { getInt64(0), getInt32(1) });
        headerOffset = CreateZExt(CreateAlignedLoad(headerOffset, 4u), getInt64Ty());

        // -> auto headerData = page + headerOffset;
        headerData = CreateInBoundsGEP(getParam(page), headerOffset);
    }

    // -> auto fixedOffset = static_cast<uint64_t>(mainPage->fixedOffset);
    auto fixedOffset = CreateInBoundsGEP(mainPage, { getInt64(0), getInt32(2) });
    fixedOffset = CreateZExt(CreateAlignedLoad(fixedOffset, 4u), getInt64Ty());

    // -> auto fixedData = page + fixedOffset;
    auto fixedData = CreateInBoundsGEP(getParam(page), fixedOffset);

    auto i = query->aggregationBegin();
    for (decltype(destRecord.fieldCount()) j = 0u; j < destRecord.fieldCount(); ++i, ++j) {
        uint16_t srcFieldIdx;
        AggregationType aggregationType;
        std::tie(srcFieldIdx, aggregationType) = *i;

        uint16_t destFieldIdx;
        destRecord.idOf(crossbow::to_string(j), destFieldIdx);

        auto& srcMeta = srcRecord.getFieldMeta(srcFieldIdx);
        auto& srcField = srcMeta.field;
        auto srcFieldAlignment = srcField.alignOf();
        auto srcFieldPtrType = getFieldPtrTy(srcField.type());

        auto& fixedMetaData = mContext.fixedMetaData();
        auto srcFieldOffset = fixedMetaData[srcFieldIdx].offset;

        auto& destMeta = destRecord.getFieldMeta(destFieldIdx);
        auto& destField = destMeta.field;
        auto destFieldAlignment = destField.alignOf();
        auto destFieldSize = destField.staticSize();
        auto destFieldType = getFieldTy(destField.type());
        auto vectorSize = mRegisterWidth / (destFieldSize * 8);
        auto destFieldVectorType = llvm::VectorType::get(destFieldType, vectorSize);
        auto destFieldPtrType = getFieldPtrTy(destField.type());
        auto destFieldOffset = destMeta.offset;
        LOG_ASSERT(srcField.isFixedSized() && destField.isFixedSized(), "Only fixed size supported");

        auto isFloat = (srcField.type() == FieldType::FLOAT) || (srcField.type() == FieldType::DOUBLE);

        // Aggregation function used by the vector and scalar code paths
        auto buildAggregation = [this, &srcField, isFloat, aggregationType]
                (llvm::Value* src, llvm::Value* dest, llvm::Value* result) {
            switch (aggregationType) {
            case AggregationType::MIN: {
                auto cond = (isFloat
                        ? CreateFCmp(llvm::CmpInst::FCMP_OLT, src, dest)
                        : CreateICmp(llvm::CmpInst::ICMP_SLT, src, dest));
                cond = CreateAnd(result, cond);
                return CreateSelect(cond, src, dest);
            } break;

            case AggregationType::MAX: {
                auto cond = (isFloat
                        ? CreateFCmp(llvm::CmpInst::FCMP_OGT, src, dest)
                        : CreateICmp(llvm::CmpInst::ICMP_SGT, src, dest));
                cond = CreateAnd(result, cond);
                return CreateSelect(cond, src, dest);
            } break;

            case AggregationType::SUM: {
                if (srcField.type() == FieldType::SMALLINT || srcField.type() == FieldType::INT) {
                    src = CreateSExt(src, dest->getType());
                } else if (srcField.type() == FieldType::FLOAT) {
                    src = CreateFPExt(src, dest->getType());
                }

                auto res = (isFloat
                        ? CreateFAdd(dest, src)
                        : CreateAdd(dest, src));
                return CreateSelect(result, res, dest);
            } break;

            case AggregationType::CNT: {
                return CreateAdd(dest, CreateZExt(result, dest->getType()));
            } break;

            default: {
                LOG_ASSERT(false, "Unknown aggregation type");
                return static_cast<llvm::Value*>(nullptr);
            } break;
            }
        };

        // Create code blocks
        auto previousBlock = GetInsertBlock();
        auto vectorHeaderBlock = createBasicBlock("agg.vectorheader." + llvm::Twine(destFieldIdx));
        auto vectorBodyBlock = createBasicBlock("agg.vectorbody." + llvm::Twine(destFieldIdx));
        auto vectorMergeBlock = createBasicBlock("agg.vectormerge." + llvm::Twine(destFieldIdx));
        auto vectorEndBlock = createBasicBlock("agg.vectorend." + llvm::Twine(destFieldIdx));
        auto scalarBodyBlock = createBasicBlock("agg.scalarbody." + llvm::Twine(destFieldIdx));
        auto scalarEndBlock = createBasicBlock("agg.scalarend." + llvm::Twine(destFieldIdx));

        // Compute the pointer to the first result element
        auto resultStartData = CreateInBoundsGEP(getParam(result), getParam(startIdx));
        auto resultEndData = CreateInBoundsGEP(getParam(result), getParam(endIdx));

        // Compute the pointer to the first element in the aggregation column
        llvm::Value* srcData = nullptr;
        if (aggregationType != AggregationType::CNT) {
            srcData = fixedData;
            if (srcFieldOffset != 0) {
                srcData = CreateInBoundsGEP(srcData, createConstMul(count, srcFieldOffset));
            }
            srcData = CreateBitCast(srcData, srcFieldPtrType);
            srcData = CreateInBoundsGEP(srcData, getParam(startIdx));
        }

        // Compute the pointer to the first element in the null bytevector
        llvm::Value* srcNullData = nullptr;
        if (!srcField.isNotNull()) {
            auto startOffset = getParam(startIdx);
            if (srcMeta.nullIdx != 0) {
                startOffset = CreateAdd(startOffset, createConstMul(count, srcMeta.nullIdx));
            }
            srcNullData = CreateInBoundsGEP(headerData, startOffset);
        }

        // Load the aggregation value from the previous run
        auto destData = getParam(dest);
        if (destFieldOffset != 0) {
            destData = CreateInBoundsGEP(destData, getInt64(destFieldOffset));
        }
        destData = CreateBitCast(destData, destFieldPtrType);
        auto destValue = CreateAlignedLoad(destData, destFieldAlignment);

        llvm::Value* destNullData = nullptr;
        llvm::Value* destNullValue = nullptr;
        if (!destField.isNotNull()) {
            destNullData = getParam(dest);
            if (destMeta.nullIdx != 0) {
                destNullData = CreateInBoundsGEP(destNullData, getInt64(destMeta.nullIdx));
            }
            destNullValue = CreateAlignedLoad(destNullData, 1u);
        }

        // Check how many vector iterations can be executed
        // Skip to the vector end if no vectorized iterations can be executed
        auto vectorCount = CreateSub(getParam(endIdx), getParam(startIdx));
        vectorCount = CreateAnd(vectorCount, getInt64(-vectorSize));
        CreateCondBr(CreateICmp(llvm::CmpInst::ICMP_NE, vectorCount, getInt64(0)), vectorHeaderBlock, vectorEndBlock);

        // Vector header
        // Initialize the start vector: In case of min/max the current min/max element is broadcasted to the complete
        // vector, for sum and count the current sum/cnt value is stored in the first element and the remaining vector
        // filled with zeroes.
        SetInsertPoint(vectorHeaderBlock);
        llvm::Value* vectorDestNull = nullptr;
        llvm::Value* vectorDestValue;
        switch (aggregationType) {
        case AggregationType::MIN:
        case AggregationType::MAX: {
            if (!destField.isNotNull()) {
                vectorDestNull = CreateVectorSplat(vectorSize, destNullValue);
            }
            vectorDestValue = CreateVectorSplat(vectorSize, destValue);
        } break;

        case AggregationType::SUM: {
            if (!destField.isNotNull()) {
                vectorDestNull = getInt8Vector(vectorSize, 1);
                vectorDestNull = CreateInsertElement(vectorDestNull, destNullValue, getInt64(0));
            }
            vectorDestValue = isFloat
                    ? getDoubleVector(vectorSize, 0)
                    : getInt64Vector(vectorSize, 0);
            vectorDestValue = CreateInsertElement(vectorDestValue, destValue, getInt64(0));
        } break;

        case AggregationType::CNT: {
            if (!destField.isNotNull()) {
                vectorDestNull = getInt8Vector(vectorSize, 1);
                vectorDestNull = CreateInsertElement(vectorDestNull, destNullValue, getInt64(0));
            }
            vectorDestValue = getInt64Vector(vectorSize, 0);
            vectorDestValue = CreateInsertElement(vectorDestValue, destValue, getInt64(0));
        } break;

        default: {
            LOG_ASSERT(false, "Unknown aggregation type");
            vectorDestValue = nullptr;
        } break;
        }

        auto vectorResultEnd = CreateInBoundsGEP(resultStartData, vectorCount);

        CreateBr(vectorBodyBlock);

        // Vector Body
        // Contains the aggregation loop
        SetInsertPoint(vectorBodyBlock);
        auto vectorResultPhi = CreatePHI(resultStartData->getType(), 2);
        vectorResultPhi->addIncoming(resultStartData, vectorHeaderBlock);

        llvm::PHINode* vectorSrcPhi = nullptr;
        if (aggregationType != AggregationType::CNT) {
            vectorSrcPhi = CreatePHI(srcData->getType(), 2);
            vectorSrcPhi->addIncoming(srcData, vectorHeaderBlock);
        }

        llvm::PHINode* vectorNullPhi = nullptr;
        if (!srcField.isNotNull()) {
            vectorNullPhi = CreatePHI(srcNullData->getType(), 2);
            vectorNullPhi->addIncoming(srcNullData, vectorHeaderBlock);
        }

        // Create PHI node containing the immediate null bytes in the loop
        llvm::PHINode* vectorNull = nullptr;
        if (!destField.isNotNull()) {
            vectorNull = CreatePHI(getInt8VectorTy(vectorSize), 2);
            vectorNull->addIncoming(vectorDestNull, vectorHeaderBlock);
        }

        // Create PHI node containing the immediate result in the loop
        auto vectorDest = CreatePHI(destFieldVectorType, 2);
        vectorDest->addIncoming(vectorDestValue, vectorHeaderBlock);

        // Load source vector (not required for count aggregation)
        llvm::Value* vectorSrc = nullptr;
        if (aggregationType != AggregationType::CNT) {
            vectorSrc = CreateBitCast(vectorSrcPhi, getFieldVectorPtrTy(srcField.type(), vectorSize));
            vectorSrc = CreateAlignedLoad(vectorSrc, srcFieldAlignment);
        }

        // Load result vector
        auto vectorResult = CreateBitCast(vectorResultPhi, getInt8VectorPtrTy(vectorSize));
        vectorResult = CreateAlignedLoad(vectorResult, 1u);

        // Load null vector
        if (!srcField.isNotNull()) {
            auto srcNullValue = CreateBitCast(vectorNullPhi, getInt8VectorPtrTy(vectorSize));
            srcNullValue = CreateAlignedLoad(srcNullValue, 1u);

            vectorResult = CreateAnd(vectorResult, CreateXor(srcNullValue, getInt8Vector(vectorSize, 1)));
        }

        // Aggregate null vectors
        llvm::Value* vectorNullAgg = nullptr;
        if (!destField.isNotNull()) {
            vectorNullAgg = CreateXor(vectorResult, getInt8Vector(vectorSize, 1));
            vectorNullAgg = CreateAnd(vectorNull, vectorNullAgg);
            vectorNull->addIncoming(vectorNullAgg, vectorBodyBlock);
        }

        vectorResult = CreateTruncOrBitCast(vectorResult, getInt1VectorTy(vectorSize));

        // Evaluate aggregation
        auto vectorAgg = buildAggregation(vectorSrc, vectorDest, vectorResult);
        vectorDest->addIncoming(vectorAgg, vectorBodyBlock);

        // Advance the loop
        auto vectorResultNext = CreateInBoundsGEP(vectorResultPhi, getInt64(vectorSize));
        vectorResultPhi->addIncoming(vectorResultNext, vectorBodyBlock);

        llvm::Value* vectorSrcNext = nullptr;
        if (aggregationType != AggregationType::CNT) {
            vectorSrcNext = CreateInBoundsGEP(vectorSrcPhi, getInt64(vectorSize));
            vectorSrcPhi->addIncoming(vectorSrcNext, vectorBodyBlock);
        }

        llvm::Value* vectorNullNext = nullptr;
        if (!srcField.isNotNull()) {
            vectorNullNext = CreateInBoundsGEP(vectorNullPhi, getInt64(vectorSize));
            vectorNullPhi->addIncoming(vectorNullNext, vectorBodyBlock);
        }

        CreateCondBr(CreateICmp(llvm::CmpInst::ICMP_NE, vectorResultNext, vectorResultEnd),
                vectorBodyBlock, vectorMergeBlock);

        // Vector Merge
        // Reduce the individual aggregations in the vector to one value by recursively aggregating the upper with the
        // lower values in the vector until only one value is left.
        SetInsertPoint(vectorMergeBlock);
        std::vector<llvm::Constant*> reduceIdx;
        reduceIdx.reserve(vectorSize);
        for (auto i = vectorSize; i > 1; i /= 2) {
            for (auto j = i / 2; j < i; ++j) {
                reduceIdx.emplace_back(getInt32(j));
            }
            for (auto j = i / 2; j < vectorSize; ++j) {
                reduceIdx.emplace_back(llvm::UndefValue::get(getInt32Ty()));
            }

            if (!destField.isNotNull()) {
                auto nullReduce = CreateShuffleVector(vectorNullAgg, llvm::UndefValue::get(getInt8VectorTy(vectorSize)),
                        llvm::ConstantVector::get(reduceIdx));
                vectorNullAgg = CreateAnd(vectorNullAgg, nullReduce);
            }

            auto reduce = CreateShuffleVector(vectorAgg, llvm::UndefValue::get(destFieldVectorType),
                    llvm::ConstantVector::get(reduceIdx));

            switch (aggregationType) {
            case AggregationType::MIN: {
                auto cond = (isFloat
                        ? CreateFCmp(llvm::CmpInst::FCMP_OLT, vectorAgg, reduce)
                        : CreateICmp(llvm::CmpInst::ICMP_SLT, vectorAgg, reduce));
                vectorAgg = CreateSelect(cond, vectorAgg, reduce);
            } break;

            case AggregationType::MAX: {
                auto cond = (isFloat
                        ? CreateFCmp(llvm::CmpInst::FCMP_OGT, vectorAgg, reduce)
                        : CreateICmp(llvm::CmpInst::ICMP_SGT, vectorAgg, reduce));
                vectorAgg = CreateSelect(cond, vectorAgg, reduce);
            } break;

            case AggregationType::SUM: {
                vectorAgg = (isFloat
                        ? CreateFAdd(vectorAgg, reduce)
                        : CreateAdd(vectorAgg, reduce));
            } break;

            case AggregationType::CNT: {
                vectorAgg = CreateAdd(vectorAgg, reduce);
            } break;

            default: {
                LOG_ASSERT(false, "Unknown aggregation type");
            } break;
            }
            reduceIdx.clear();
        }
        if (!destField.isNotNull()) {
            vectorNullAgg = CreateExtractElement(vectorNullAgg, getInt64(0));
        }
        vectorAgg = CreateExtractElement(vectorAgg, getInt64(0));
        CreateBr(vectorEndBlock);

        // Vector end block
        // Merge result from vectorized code or the result from the previous run if no vector iterations were executed.
        // Branch to scalar code if additional scalar iterations are required.
        SetInsertPoint(vectorEndBlock);

        auto vectorResultData = CreatePHI(resultStartData->getType(), 2);
        vectorResultData->addIncoming(resultStartData, previousBlock);
        vectorResultData->addIncoming(vectorResultNext, vectorMergeBlock);

        llvm::PHINode* vectorSrcData = nullptr;
        if (aggregationType != AggregationType::CNT) {
            vectorSrcData = CreatePHI(srcData->getType(), 2);
            vectorSrcData->addIncoming(srcData, previousBlock);
            vectorSrcData->addIncoming(vectorSrcNext, vectorMergeBlock);
        }

        llvm::PHINode* vectorNullData = nullptr;
        if (!srcField.isNotNull()) {
            vectorNullData  = CreatePHI(srcNullData->getType(), 2);
            vectorNullData ->addIncoming(srcNullData, previousBlock);
            vectorNullData ->addIncoming(vectorNullNext, vectorMergeBlock);
        }

        llvm::PHINode* vectorNullResult = nullptr;
        if (!destField.isNotNull()) {
            vectorNullResult = CreatePHI(getInt8Ty(), 2);
            vectorNullResult->addIncoming(destNullValue, previousBlock);
            vectorNullResult->addIncoming(vectorNullAgg, vectorMergeBlock);
        }

        auto vectorAggResult = CreatePHI(destFieldType, 2);
        vectorAggResult->addIncoming(destValue, previousBlock);
        vectorAggResult->addIncoming(vectorAgg, vectorMergeBlock);

        CreateCondBr(CreateICmp(llvm::CmpInst::ICMP_NE, vectorResultData, resultEndData),
                scalarBodyBlock, scalarEndBlock);

        // Scalar Body
        // Contains the aggregation loop
        SetInsertPoint(scalarBodyBlock);

        auto scalarResultPhi = CreatePHI(vectorResultData->getType(), 2);
        scalarResultPhi->addIncoming(vectorResultData, vectorEndBlock);

        llvm::PHINode* scalarSrcPhi = nullptr;
        if (aggregationType != AggregationType::CNT) {
            scalarSrcPhi = CreatePHI(vectorSrcData->getType(), 2);
            scalarSrcPhi->addIncoming(vectorSrcData, vectorEndBlock);
        }

        llvm::PHINode* scalarNullPhi = nullptr;
        if (!srcField.isNotNull()) {
            scalarNullPhi = CreatePHI(vectorNullData->getType(), 2);
            scalarNullPhi->addIncoming(vectorNullData, vectorEndBlock);
        }

        // Create PHI node containing the immediate null byte in the loop
        llvm::PHINode* scalarNull = nullptr;
        if (!destField.isNotNull()) {
            scalarNull = CreatePHI(getInt8Ty(), 2);
            scalarNull->addIncoming(vectorNullResult, vectorEndBlock);
        }

        // Create PHI node containing the immediate result in the loop
        auto scalarDest = CreatePHI(destFieldType, 2);
        scalarDest->addIncoming(vectorAggResult, vectorEndBlock);

        // Load source scalar (not required for count aggregation)
        llvm::Value* scalarSrc = nullptr;
        if (aggregationType != AggregationType::CNT) {
            scalarSrc = CreateAlignedLoad(scalarSrcPhi, srcFieldAlignment);
        }

        // Load result scalar
        llvm::Value* scalarResult = CreateAlignedLoad(scalarResultPhi, 1u);

        // Load null scalar
        if (!srcField.isNotNull()) {
            auto srcNullValue = CreateAlignedLoad(scalarNullPhi, 1u);

            scalarResult = CreateAnd(scalarResult, CreateXor(srcNullValue, getInt8(1)));
        }

        // Aggregate null scalars
        llvm::Value* scalarNullAgg = nullptr;
        if (!destField.isNotNull()) {
            scalarNullAgg = CreateXor(scalarResult, getInt8(1));
            scalarNullAgg = CreateAnd(scalarNull, scalarNullAgg);
            scalarNull->addIncoming(scalarNullAgg, scalarBodyBlock);
        }

        scalarResult = CreateTruncOrBitCast(scalarResult, getInt1Ty());

        // Evaluate aggregation
        auto scalarAgg = buildAggregation(scalarSrc, scalarDest, scalarResult);
        scalarDest->addIncoming(scalarAgg, scalarBodyBlock);

        // Advance the loop
        auto scalarResultNext = CreateInBoundsGEP(scalarResultPhi, getInt64(1));
        scalarResultPhi->addIncoming(scalarResultNext, scalarBodyBlock);

        if (aggregationType != AggregationType::CNT) {
            auto scalarSrcNext = CreateInBoundsGEP(scalarSrcPhi, getInt64(1));
            scalarSrcPhi->addIncoming(scalarSrcNext, scalarBodyBlock);
        }

        if (!srcField.isNotNull()) {
            auto scalarNullNext = CreateInBoundsGEP(scalarNullPhi, getInt64(1));
            scalarNullPhi->addIncoming(scalarNullNext, scalarBodyBlock);
        }

        CreateCondBr(CreateICmp(llvm::CmpInst::ICMP_NE, scalarResultNext, resultEndData),
                scalarBodyBlock, scalarEndBlock);

        // Scalar end block
        SetInsertPoint(scalarEndBlock);
        llvm::PHINode* nullResult = nullptr;
        if (!destField.isNotNull()) {
            nullResult = CreatePHI(getInt8Ty(), 2);
            nullResult->addIncoming(vectorNullResult, vectorEndBlock);
            nullResult->addIncoming(scalarNullAgg, scalarBodyBlock);
        }
        auto aggResult = CreatePHI(destFieldType, 2);
        aggResult->addIncoming(vectorAggResult, vectorEndBlock);
        aggResult->addIncoming(scalarAgg, scalarBodyBlock);

        if (!destField.isNotNull()) {
            CreateAlignedStore(nullResult, destNullData, 1u);
        }

        CreateAlignedStore(aggResult, destData, destFieldAlignment);
    }

    // -> return destRecord.staticSize();
    CreateRet(getInt32(destRecord.staticSize()));
}

} // namespace deltamain
} // namespace store
} // namespace tell
