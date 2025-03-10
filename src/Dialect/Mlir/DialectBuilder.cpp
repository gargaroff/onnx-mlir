/*
 * SPDX-License-Identifier: Apache-2.0
 */

//===------ DialectBuilder.cpp - Helper functions for MLIR dialects -------===//
//
// Copyright 2019-2023 The IBM Research Authors.
//
// =============================================================================
//
// This file contains helper functions for building MLIR operations.
//
//===----------------------------------------------------------------------===//

#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/IR/SCF.h"
#include "mlir/Dialect/Shape/IR/Shape.h"
#include "mlir/Dialect/Utils/ReshapeOpsUtils.h"
#include "mlir/Dialect/Vector/IR/VectorOps.h"
#include "mlir/IR/IRMapping.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Support/Debug.h"

// Please do not add dependences on ONNX or KRNL dialects.
#include "src/Dialect/Mlir/DialectBuilder.hpp"
#include "src/Dialect/Mlir/VectorMachineSupport.hpp"

#include <algorithm>

#define DEBUG_TYPE "dialect_builder"

using namespace mlir;

namespace onnx_mlir {

//===----------------------------------------------------------------------===//
// Original code for MathBuilder is copied from LLVM MLIR Utils.cpp
// Modified here to add operations, add super class.
// License added here for this class for completeness.
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//===----------------------------------------------------------------------===//

// Test for unsigned as signless are treated as signed. For reference, check in
// MLIR AffineToStandard where comparison of indices are done with slt and sgt,
// for example. Indices are signless. Also, in ONNX, we currently treat all
// ONNX Integers as MLIR signless, and only flag the ONNX Unsigned Integer as
// MLIR unsigned integer.

/* static */ Type MathBuilder::elementTypeWithVector(Type elementOrVectorType) {
  VectorType vectorType = elementOrVectorType.dyn_cast<VectorType>();
  if (vectorType)
    return vectorType.getElementType();
  return elementOrVectorType;
}

/* static */ Type MathBuilder::getTypeWithVector(
    VectorType vectorType, Type elementType) {
  if (vectorType)
    return VectorType::get(vectorType.getShape(), elementType);
  return elementType;
}

/* static */ bool MathBuilder::isIntegerWithVector(Type elementOrVectorType) {
  Type elementType = elementTypeWithVector(elementOrVectorType);
  return elementType.isa<IntegerType>() || elementType.isa<IndexType>();
}

/* static */ bool MathBuilder::isUnsignedIntegerWithVector(
    Type elementOrVectorType) {
  Type elementType = elementTypeWithVector(elementOrVectorType);
  return elementType.isUnsignedInteger();
}

/* static */ bool MathBuilder::isFloatWithVector(Type elementOrVectorType) {
  Type elementType = elementTypeWithVector(elementOrVectorType);
  return elementType.isa<FloatType>();
}

Value MathBuilder::abs(Value val) const {
  if (isIntegerWithVector(val.getType()))
    return b().create<math::AbsIOp>(loc(), val);
  if (isFloatWithVector(val.getType()))
    return b().create<math::AbsFOp>(loc(), val);
  llvm_unreachable("expected int or float");
}

Value MathBuilder::andi(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isIntegerWithVector(lhs.getType()))
    return b().create<arith::AndIOp>(loc(), lhs, rhs);
  llvm_unreachable("expected int");
}

Value MathBuilder::ori(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isIntegerWithVector(lhs.getType()))
    return b().create<arith::OrIOp>(loc(), lhs, rhs);
  llvm_unreachable("expected int");
}

Value MathBuilder::xori(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isIntegerWithVector(lhs.getType()))
    return b().create<arith::XOrIOp>(loc(), lhs, rhs);
  llvm_unreachable("expected int");
}

Value MathBuilder::add(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isIntegerWithVector(lhs.getType())) {
    Type elemType = elementTypeWithVector(lhs.getType());
    if (elemType.isUnsignedInteger()) {
      unsigned elemWidth = elemType.cast<IntegerType>().getWidth();
      Value castLhs = castToSignless(lhs, elemWidth);
      Value castRhs = castToSignless(rhs, elemWidth);
      Value castAdd =
          b().create<arith::AddUIExtendedOp>(loc(), castLhs, castRhs).getSum();
      return castToUnsigned(castAdd, elemWidth);
    } else
      return b().create<arith::AddIOp>(loc(), lhs, rhs);
  }
  if (isFloatWithVector(lhs.getType()))
    return b().create<arith::AddFOp>(loc(), lhs, rhs);
  llvm_unreachable("expected int or float");
}

Value MathBuilder::sub(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isIntegerWithVector(lhs.getType()))
    return b().create<arith::SubIOp>(loc(), lhs, rhs);
  if (isFloatWithVector(lhs.getType()))
    return b().create<arith::SubFOp>(loc(), lhs, rhs);
  llvm_unreachable("expected int or float");
}

Value MathBuilder::mul(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isIntegerWithVector(lhs.getType())) {
    Type elemType = elementTypeWithVector(lhs.getType());
    if (elemType.isUnsignedInteger()) {
      unsigned elemWidth = elemType.cast<IntegerType>().getWidth();
      Value castLhs = castToSignless(lhs, elemWidth);
      Value castRhs = castToSignless(rhs, elemWidth);
      Value castMul =
          b().create<arith::MulUIExtendedOp>(loc(), castLhs, castRhs).getLow();
      return castToUnsigned(castMul, elemWidth);
    } else
      return b().create<arith::MulIOp>(loc(), lhs, rhs);
  }
  if (isFloatWithVector(lhs.getType()))
    return b().create<arith::MulFOp>(loc(), lhs, rhs);
  llvm_unreachable("expected int or float");
}

Value MathBuilder::div(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isFloatWithVector(lhs.getType()))
    return b().create<arith::DivFOp>(loc(), lhs, rhs);
  if (isUnsignedIntegerWithVector(lhs.getType()))
    return b().create<arith::DivUIOp>(loc(), lhs, rhs);
  if (isIntegerWithVector(lhs.getType()))
    return b().create<arith::DivSIOp>(loc(), lhs, rhs);
  llvm_unreachable("expected int or float");
}

Value MathBuilder::rem(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isFloatWithVector(lhs.getType()))
    return b().create<arith::RemFOp>(loc(), lhs, rhs);
  if (isUnsignedIntegerWithVector(lhs.getType()))
    return b().create<arith::RemUIOp>(loc(), lhs, rhs);
  if (isIntegerWithVector(lhs.getType()))
    return b().create<arith::RemSIOp>(loc(), lhs, rhs);
  llvm_unreachable("expected int or float");
}

Value MathBuilder::copySign(mlir::Value rem, mlir::Value dividend) const {
  assert(rem.getType() == dividend.getType() && "expected same type");
  if (isFloatWithVector(rem.getType()))
    return b().create<math::CopySignOp>(loc(), rem, dividend);
  llvm_unreachable("expected float");
}

Value MathBuilder::ceilDiv(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isUnsignedIntegerWithVector(lhs.getType()))
    return b().create<arith::CeilDivUIOp>(loc(), lhs, rhs);
  if (isIntegerWithVector(lhs.getType()))
    return b().create<arith::CeilDivSIOp>(loc(), lhs, rhs);
  llvm_unreachable("expected int");
}

Value MathBuilder::floorDiv(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isUnsignedIntegerWithVector(lhs.getType()))
    // Using regular unsigned div is ok as it rounds toward zero.
    return b().create<arith::DivUIOp>(loc(), lhs, rhs);
  if (isIntegerWithVector(lhs.getType()))
    return b().create<arith::FloorDivSIOp>(loc(), lhs, rhs);
  llvm_unreachable("expected int");
}

// return (lhs * rhs) + acc
Value MathBuilder::fma(Value lhs, Value rhs, Value acc) const {
  assert((lhs.getType() == rhs.getType()) && (rhs.getType() == acc.getType()) &&
         "expected same type");
  if (isFloatWithVector(lhs.getType()) && !isa<FloatType>(lhs.getType()))
    return b().create<vector::FMAOp>(loc(), lhs, rhs, acc);
  return add(mul(lhs, rhs), acc);
}

Value MathBuilder::exp(Value val) const {
  if (isFloatWithVector(val.getType()))
    return b().create<math::ExpOp>(loc(), val);
  llvm_unreachable("expected float");
}

Value MathBuilder::exp2(Value val) const {
  if (isFloatWithVector(val.getType()))
    return b().create<math::Exp2Op>(loc(), val);
  llvm_unreachable("expected float");
}

Value MathBuilder::log(Value val) const {
  if (isFloatWithVector(val.getType()))
    return b().create<math::LogOp>(loc(), val);
  llvm_unreachable("expected float");
}

Value MathBuilder::log2(Value val) const {
  if (isFloatWithVector(val.getType()))
    return b().create<math::Log2Op>(loc(), val);
  llvm_unreachable("expected float");
}

Value MathBuilder::sqrt(Value val) const {
  if (isFloatWithVector(val.getType()))
    return b().create<math::SqrtOp>(loc(), val);
  llvm_unreachable("expected float");
}

Value MathBuilder::pow(Value base, Value exp) const {
  if (isFloatWithVector(base.getType()))
    return b().create<math::PowFOp>(loc(), base, exp);
  llvm_unreachable("expected base float");
}

Value MathBuilder::neg(Value val) const {
  if (isIntegerWithVector(val.getType()))
    // Returns 0 - val.
    return sub(constant(val.getType(), 0), val);
  if (isFloatWithVector(val.getType()))
    return b().create<arith::NegFOp>(loc(), val);
  llvm_unreachable("expected int or float");
}

Value MathBuilder::ceil(Value val) const {
  if (isFloatWithVector(val.getType()))
    return b().create<math::CeilOp>(loc(), val);
  llvm_unreachable("expected float");
}

Value MathBuilder::floor(Value val) const {
  if (isFloatWithVector(val.getType()))
    return b().create<math::FloorOp>(loc(), val);
  llvm_unreachable("expected float");
}

Value MathBuilder::min(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isFloatWithVector(lhs.getType()))
    return b().create<arith::MinFOp>(loc(), lhs, rhs);
  if (isUnsignedIntegerWithVector(lhs.getType()))
    return b().create<arith::MinUIOp>(loc(), lhs, rhs);
  if (isIntegerWithVector(lhs.getType()))
    return b().create<arith::MinSIOp>(loc(), lhs, rhs);
  llvm_unreachable("expected int or float");
}

Value MathBuilder::max(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isFloatWithVector(lhs.getType()))
    return b().create<arith::MaxFOp>(loc(), lhs, rhs);
  if (isUnsignedIntegerWithVector(lhs.getType()))
    return b().create<arith::MaxUIOp>(loc(), lhs, rhs);
  if (isIntegerWithVector(lhs.getType()))
    return b().create<arith::MaxSIOp>(loc(), lhs, rhs);
  llvm_unreachable("expected int or float");
}

Value MathBuilder::sgt(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isIntegerWithVector(lhs.getType()))
    return createArithCmp(lhs, rhs, arith::CmpIPredicate::sgt);
  if (isFloatWithVector(lhs.getType()))
    return createArithCmp(lhs, rhs, arith::CmpFPredicate::OGT);
  llvm_unreachable("expected int or float");
}

Value MathBuilder::sge(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isIntegerWithVector(lhs.getType()))
    return createArithCmp(lhs, rhs, arith::CmpIPredicate::sge);
  if (isFloatWithVector(lhs.getType()))
    return createArithCmp(lhs, rhs, arith::CmpFPredicate::OGE);
  llvm_unreachable("expected int or float");
}

Value MathBuilder::slt(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isIntegerWithVector(lhs.getType()))
    return createArithCmp(lhs, rhs, arith::CmpIPredicate::slt);
  if (isFloatWithVector(lhs.getType()))
    return createArithCmp(lhs, rhs, arith::CmpFPredicate::OLT);
  llvm_unreachable("expected int or float");
}

Value MathBuilder::sle(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isIntegerWithVector(lhs.getType()))
    return createArithCmp(lhs, rhs, arith::CmpIPredicate::sle);
  if (isFloatWithVector(lhs.getType()))
    return createArithCmp(lhs, rhs, arith::CmpFPredicate::OLE);
  llvm_unreachable("expected int or float");
}

Value MathBuilder::ugt(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isUnsignedIntegerWithVector(lhs.getType()))
    return createArithCmp(lhs, rhs, arith::CmpIPredicate::ugt);
  llvm_unreachable("expected unsigned int");
}

Value MathBuilder::uge(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isUnsignedIntegerWithVector(lhs.getType()))
    return createArithCmp(lhs, rhs, arith::CmpIPredicate::uge);
  llvm_unreachable("expected unsigned int");
}

Value MathBuilder::ult(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isUnsignedIntegerWithVector(lhs.getType()))
    return createArithCmp(lhs, rhs, arith::CmpIPredicate::ult);
  llvm_unreachable("expected unsigned int");
}

Value MathBuilder::ule(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isUnsignedIntegerWithVector(lhs.getType()))
    return createArithCmp(lhs, rhs, arith::CmpIPredicate::ule);
  llvm_unreachable("expected unsigned int");
}

Value MathBuilder::gt(Value lhs, Value rhs) const {
  if (isUnsignedIntegerWithVector(lhs.getType()))
    return ugt(lhs, rhs);
  return sgt(lhs, rhs);
}

Value MathBuilder::ge(Value lhs, Value rhs) const {
  if (isUnsignedIntegerWithVector(lhs.getType()))
    return uge(lhs, rhs);
  return sge(lhs, rhs);
}

Value MathBuilder::lt(Value lhs, Value rhs) const {
  if (isUnsignedIntegerWithVector(lhs.getType()))
    return ult(lhs, rhs);
  return slt(lhs, rhs);
}

Value MathBuilder::le(Value lhs, Value rhs) const {
  if (isUnsignedIntegerWithVector(lhs.getType()))
    return ule(lhs, rhs);
  return sle(lhs, rhs);
}

Value MathBuilder::eq(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isIntegerWithVector(lhs.getType()))
    return createArithCmp(lhs, rhs, arith::CmpIPredicate::eq);
  if (isFloatWithVector(lhs.getType()))
    return createArithCmp(lhs, rhs, arith::CmpFPredicate::OEQ);
  llvm_unreachable("expected int or float");
}

Value MathBuilder::neq(Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  if (isIntegerWithVector(lhs.getType()))
    return createArithCmp(lhs, rhs, arith::CmpIPredicate::ne);
  if (isFloatWithVector(lhs.getType()))
    return createArithCmp(lhs, rhs, arith::CmpFPredicate::ONE);
  llvm_unreachable("expected int or float");
}

Value MathBuilder::select(Value cmp, Value lhs, Value rhs) const {
  assert(lhs.getType() == rhs.getType() && "expected same type");
  return b().create<arith::SelectOp>(loc(), cmp, lhs, rhs);
}

Value MathBuilder::constant(Type type, double val) const {
  Value constant = nullptr;
  // Could be a vector type; look at the element type.
  Type elementType = elementTypeWithVector(type);
  TypeSwitch<Type>(elementType)
      .Case<Float16Type>([&](Type) {
        constant =
            b().create<arith::ConstantOp>(loc(), b().getF16FloatAttr(val));
      })
      .Case<Float32Type>([&](Type) {
        constant =
            b().create<arith::ConstantOp>(loc(), b().getF32FloatAttr(val));
      })
      .Case<Float64Type>([&](Type) {
        constant =
            b().create<arith::ConstantOp>(loc(), b().getF64FloatAttr(val));
      })
      .Case<IntegerType>([&](IntegerType elementType) {
        assert(val == (int64_t)val && "value is ambiguous");
        unsigned width = elementType.getWidth();

        if (width == 1)
          constant =
              b().create<arith::ConstantOp>(loc(), b().getBoolAttr(val != 0));
        else {
          // If unsigned, create a signless constant, then cast it to unsigned.
          if (elementType.isUnsignedInteger()) {
            Type signlessTy = b().getIntegerType(width);
            constant = b().create<arith::ConstantOp>(loc(),
                b().getIntegerAttr(signlessTy, APInt(width, (int64_t)val)));
            constant = castToUnsigned(constant, width);
          } else {
            constant = b().create<arith::ConstantOp>(loc(),
                b().getIntegerAttr(elementType, APInt(width, (int64_t)val)));
          }
        }
      })
      .Case<IndexType>([&](Type elementType) {
        constant = b().create<arith::ConstantOp>(
            loc(), b().getIntegerAttr(elementType, val));
      })
      .Default([](Type) { llvm_unreachable("unsupported element type"); });

  assert(constant != nullptr && "Expecting valid constant value");
  if (type.isa<VectorType>()) {
    // For vectors, need to splat the constant.
    MultiDialectBuilder<VectorBuilder> create(*this);
    VectorType vecType = type.dyn_cast<VectorType>();
    constant = create.vec.splat(vecType, constant);
  }
  return constant;
}

Value MathBuilder::constantIndex(int64_t val) const {
  Attribute constantAttr = b().getIntegerAttr(b().getIndexType(), val);
  return b().create<arith::ConstantOp>(loc(), constantAttr);
}

Attribute MathBuilder::negativeInfAttr(mlir::Type type) const {
  Attribute attr;
  TypeSwitch<Type>(type)
      .Case<Float32Type>([&](Type) {
        attr = b().getF32FloatAttr(-std::numeric_limits<float>::infinity());
      })
      .Case<Float64Type>([&](Type) {
        attr = b().getF64FloatAttr(-std::numeric_limits<double>::infinity());
      })
      .Case<IntegerType>([&](IntegerType type) {
        unsigned width = type.getWidth();
        bool isSignless = type.isSignless();
        bool isSigned = type.isSigned();
        int64_t value;
        switch (width) {
        case 8:
          value = (isSignless || isSigned)
                      ? std::numeric_limits<int8_t>::min()
                      : std::numeric_limits<uint8_t>::min();
          break;
        case 16:
          value = (isSignless || isSigned)
                      ? std::numeric_limits<int16_t>::min()
                      : std::numeric_limits<uint16_t>::min();
          break;
        case 32:
          value = (isSignless || isSigned)
                      ? std::numeric_limits<int32_t>::min()
                      : std::numeric_limits<uint32_t>::min();
          break;
        case 64:
          value = (isSignless || isSigned)
                      ? std::numeric_limits<int64_t>::min()
                      : std::numeric_limits<uint64_t>::min();
          break;
        default:
          llvm_unreachable("unsupported element type");
        }
        attr = b().getIntegerAttr(type, APInt(width, value));
      })
      .Default([](Type) { llvm_unreachable("unsupported element type"); });
  assert(attr != nullptr && "Expecting valid attribute");
  return attr;
}

Attribute MathBuilder::positiveInfAttr(mlir::Type type) const {
  Attribute attr;
  TypeSwitch<Type>(type)
      .Case<Float32Type>([&](Type) {
        attr = b().getF32FloatAttr(std::numeric_limits<float>::infinity());
      })
      .Case<Float64Type>([&](Type) {
        attr = b().getF64FloatAttr(std::numeric_limits<double>::infinity());
      })
      .Case<IntegerType>([&](IntegerType type) {
        unsigned width = type.getWidth();
        bool isSignless = type.isSignless();
        bool isSigned = type.isSigned();
        int64_t value;
        switch (width) {
        case 8:
          value = (isSignless || isSigned)
                      ? std::numeric_limits<int8_t>::max()
                      : std::numeric_limits<uint8_t>::max();
          break;
        case 16:
          value = (isSignless || isSigned)
                      ? std::numeric_limits<int16_t>::max()
                      : std::numeric_limits<uint16_t>::max();
          break;
        case 32:
          value = (isSignless || isSigned)
                      ? std::numeric_limits<int32_t>::max()
                      : std::numeric_limits<uint32_t>::max();
          break;
        case 64:
          value = (isSignless || isSigned)
                      ? std::numeric_limits<int64_t>::max()
                      : std::numeric_limits<uint64_t>::max();
          break;
        default:
          llvm_unreachable("unsupported element type");
        }
        attr = b().getIntegerAttr(type, APInt(width, value));
      })
      .Default([](Type) { llvm_unreachable("unsupported element type"); });
  assert(attr != nullptr && "Expecting valid attribute");
  return attr;
}

Value MathBuilder::negativeInf(Type type) const {
  Attribute attr = negativeInfAttr(type);
  Value constant = b().create<arith::ConstantOp>(loc(), attr);
  assert(constant != nullptr && "Expecting valid constant value");
  return constant;
}

Value MathBuilder::positiveInf(Type type) const {
  Attribute attr = positiveInfAttr(type);
  Value constant = b().create<arith::ConstantOp>(loc(), attr);
  assert(constant != nullptr && "Expecting valid constant value");
  return constant;
}

Value MathBuilder::createArithCmp(
    Value lhs, Value rhs, arith::CmpIPredicate pred) const {
  Type type = lhs.getType();
  assert(type == rhs.getType() && "Operands should have the same type");
  assert(isIntegerWithVector(type) && "expected int");
  return b().create<arith::CmpIOp>(loc(), pred, lhs, rhs);
}

Value MathBuilder::createArithCmp(
    Value lhs, Value rhs, arith::CmpFPredicate pred) const {
  Type type = lhs.getType();
  assert(type == rhs.getType() && "Operands should have the same type");
  assert(isFloatWithVector(type) && "expected float");
  return b().create<arith::CmpFOp>(loc(), pred, lhs, rhs);
}

// Several operations in the arith dialect require signless integers. This
// cast remove the sign of integer types for successful processing, to the
// best of my understanding.
Value MathBuilder::castToSignless(Value val, int64_t width) const {
  Type valType = val.getType();
  VectorType vecType = valType.dyn_cast<VectorType>();
  Type valElemType = elementTypeWithVector(valType);
  assert(valElemType.isa<IntegerType>() && !valElemType.isSignlessInteger() &&
         "Expecting signed integer type");
  Type destType = getTypeWithVector(vecType, b().getIntegerType(width));
  return b()
      .create<UnrealizedConversionCastOp>(loc(), destType, val)
      .getResult(0);
}

Value MathBuilder::castToUnsigned(Value val, int64_t width) const {
  Type valType = val.getType();
  VectorType vecType = valType.dyn_cast<VectorType>();
  Type valElemType = elementTypeWithVector(valType);
  assert(valElemType.isa<IntegerType>() && "Expecting integer type");
  Type destType =
      getTypeWithVector(vecType, b().getIntegerType(width, false /*signed*/));
  return b()
      .create<UnrealizedConversionCastOp>(loc(), destType, val)
      .getResult(0);
}

// Methods inspired from MLIR TosaToLinalg CastOp.
Value MathBuilder::cast(Type destType, Value src) const {
  // Get element type and vector types (if any, i.e. possibly nullptr).
  Type srcType = src.getType();
  VectorType srcVecType = srcType.dyn_cast<VectorType>();
  VectorType destVecType = destType.dyn_cast<VectorType>();
  Type srcElemType = elementTypeWithVector(srcType);
  Type destElemType = elementTypeWithVector(destType);
  // Make sure we don't mix vector and scalars.
  assert(((srcVecType && destVecType) || (!srcVecType && !destVecType)) &&
         "expect both to be scalars or vectors");
  // Check if we even need a cast.
  if (srcType == destType)
    return src;

  // Process index types first.
  if (srcElemType.isa<IndexType>()) {
    // If the source is an index type, first convert it into a signless int of
    // size 64.
    srcElemType = b().getIntegerType(64);
    srcType = getTypeWithVector(srcVecType, srcElemType);
    src = b().create<arith::IndexCastOp>(loc(), srcType, src);
  }
  bool destIsIndex = false;
  Type savedDestType = destType; // Used when destIsIndex is true.
  if (destElemType.isa<IndexType>()) {
    // If the dest is an index type, pretend for now that we want it to be
    // converted to signless int of size 64.
    destElemType = b().getIntegerType(64);
    destType = getTypeWithVector(destVecType, destElemType);
    destIsIndex = true;
  }

  // Only support Integer or Float type at this stage. Index were transformed
  // to signless int.
  // TODO: add support for shaped tensor (MemRef, Vector, Tensor?) if needed.
  assert((srcElemType.isa<IntegerType>() || srcElemType.isa<FloatType>()) &&
         "support only float or int");
  assert((destElemType.isa<IntegerType>() || destElemType.isa<FloatType>()) &&
         "support only float or int");
  // Get source and dest type width.
  int64_t srcElemWidth = srcElemType.getIntOrFloatBitWidth();
  int64_t destElemWidth = destElemType.getIntOrFloatBitWidth();
  bool bitExtend = srcElemWidth < destElemWidth;
  bool bitTrunc = srcElemWidth > destElemWidth;

  LLVM_DEBUG(llvm::dbgs() << "srcType: " << srcType << "\n";
             llvm::dbgs() << "destType: " << destType << "\n";);

  // Handle boolean first because they need special handling.
  // Boolean to int/float conversions. Boolean are unsigned.
  if (srcElemType.isInteger(1)) {
    if (destElemType.isa<FloatType>()) {
      return b().create<arith::UIToFPOp>(loc(), destType, src);
    } else {
      Value dest = b().create<arith::ExtUIOp>(loc(), destType, src);
      if (destIsIndex)
        dest = b().create<arith::IndexCastOp>(loc(), savedDestType, dest);
      return dest;
    }
  }

  // Int/Float to booleans, just compare value to be unequal zero.
  if (destElemType.isInteger(1)) {
    Type constantType = srcType;
    if (srcElemType.isa<IntegerType>() && !srcElemType.isSignlessInteger()) {
      // An integer constant must be signless.
      unsigned srcElemWidth = srcElemType.cast<IntegerType>().getWidth();
      constantType = getTypeWithVector(
          srcVecType, IntegerType::get(srcElemType.getContext(), srcElemWidth));
      src = castToSignless(src, srcElemWidth);
    }
    Value zero = constant(constantType, 0);
    return neq(src, zero);
  }

  // Float to float conversions.
  if (srcElemType.isa<FloatType>() && destElemType.isa<FloatType>()) {
    assert((bitExtend || bitTrunc) && "expected extend or trunc");
    if (bitExtend)
      return b().create<arith::ExtFOp>(loc(), destType, src);
    else
      return b().create<arith::TruncFOp>(loc(), destType, src);
  }

  // Float to int conversions.
  if (srcElemType.isa<FloatType>() && destElemType.isa<IntegerType>()) {
    // TosaToLinalg in MLIR uses a fancier algorithm that clamps values to
    // min/max signed/unsigned integer values.
    if (destType.isUnsignedInteger()) {
      Type castType = b().getIntegerType(destElemWidth);
      Value cast = b().create<arith::FPToUIOp>(loc(), castType, src);
      return castToUnsigned(cast, destElemWidth);
    } else {
      // Handle signed int.
      Value dest = b().create<arith::FPToSIOp>(loc(), destType, src);
      if (destIsIndex)
        dest = b().create<arith::IndexCastOp>(loc(), savedDestType, dest);
      return dest;
    }
  }

  // Int to float conversion.
  if (srcElemType.isa<IntegerType>() && destElemType.isa<FloatType>()) {
    if (srcElemType.isUnsignedInteger()) {
      Value cast = castToSignless(src, srcElemWidth);
      return b().create<arith::UIToFPOp>(loc(), destType, cast);
    } else {
      // Handle signed int.
      return b().create<arith::SIToFPOp>(loc(), destType, src);
    }
  }

  // Int to int conversion.
  if (srcType.isa<IntegerType>() && destType.isa<IntegerType>()) {
    if (srcType.isUnsignedInteger()) {
      // Unsigned to unsigned/signed conversion.
      // Same bit width for unsigned to signed conversion.
      if ((srcElemWidth == destElemWidth) && destType.isSignlessInteger())
        return castToSignless(src, srcElemWidth);
      // Different bit width.
      assert((bitExtend || bitTrunc) && "expected extend or trunc");
      // Has to convert to signless first, and reconvert output to unsigned.
      Value cast = castToSignless(src, srcElemWidth);
      Type castType = b().getIntegerType(destElemWidth);
      if (bitExtend) {
        cast = b().create<arith::ExtUIOp>(loc(), castType, cast);
      } else {
        // TosaToLinalg use a clipping algo, not sure if needed.
        cast = b().create<arith::TruncIOp>(loc(), castType, cast);
      }
      if (destType.isUnsignedInteger()) {
        // Unsigned to unsigned conversion.
        return castToUnsigned(cast, destElemWidth);
      } else {
        // Unsigned to signed conversion.
        return cast;
      }
    } else {
      // Signed to unsigned/signed conversion.
      // Handle signed integer
      // Same bit width for signed to unsigned conversion.
      if ((srcElemWidth == destElemWidth) && destType.isUnsignedInteger())
        return castToUnsigned(src, srcElemWidth);
      // Different bit width.
      Value dest = src;
      if (bitExtend)
        dest = b().create<arith::ExtSIOp>(loc(), destType, src);
      if (bitTrunc)
        // TosaToLinalg use a clipping algo
        dest = b().create<arith::TruncIOp>(loc(), destType, src);
      if (destIsIndex)
        return b().create<arith::IndexCastOp>(loc(), b().getIndexType(), dest);
      if (destType.isUnsignedInteger()) {
        return castToUnsigned(dest, destElemWidth);
      } else {
        return dest;
      }
    }
  }

  // Handled all the cases supported so far.
  llvm_unreachable("unsupported element type");
  return nullptr;
}

Value MathBuilder::castToIndex(Value src) const {
  return cast(b().getIndexType(), src);
}

// Add offsets to least significant values in indices. So if indices has 4
// values, (i, j, k, l) and offsets has 2 values (K, L), the results will be (i,
// j, k+K, l+L).
void MathBuilder::addOffsetToLeastSignificant(mlir::ValueRange indices,
    mlir::ValueRange offsets,
    llvm::SmallVectorImpl<mlir::Value> &computedIndices) const {
  int64_t indexRank = indices.size();
  int64_t offsetRank = offsets.size();
  int64_t firstOffset = indexRank - offsetRank;
  assert(firstOffset >= 0 && "indexOffset should not have a higher rank than "
                             "the indices in the memref");
  computedIndices.clear();
  for (int64_t i = 0; i < indexRank; i++) {
    if (i < firstOffset) {
      computedIndices.emplace_back(indices[i]);
    } else {
      computedIndices.emplace_back(add(offsets[i - firstOffset], indices[i]));
    }
  }
}

void MathBuilder::addOffsetToLeastSignificant(mlir::ArrayRef<IndexExpr> indices,
    ValueRange offsets, llvm::SmallVectorImpl<Value> &computedIndices) const {
  SmallVector<Value, 4> indexValues;
  IndexExpr::getValues(indices, indexValues);
  addOffsetToLeastSignificant(indexValues, offsets, computedIndices);
}

//===----------------------------------------------------------------------===//
// Shape support.
//===----------------------------------------------------------------------===//

Value ShapeBuilder::dim(Value val, int64_t index) const {
  Value inputShape = shapeOf(val);
  return getExtent(inputShape, index);
}

Value ShapeBuilder::shapeOf(Value val) const {
  return b().create<shape::ShapeOfOp>(loc(), val);
}

Value ShapeBuilder::getExtent(Value val, int64_t index) const {
  return b().create<shape::GetExtentOp>(loc(), val, index);
}

//===----------------------------------------------------------------------===//
// Memref support, including inserting default alignment.
//===----------------------------------------------------------------------===//

const int64_t MemRefBuilder::defaultAlign = -1;

//===----------------------------------------------------------------------===//
// Helper private functions.

// Compute alignment, which is at least gDefaultAllocAlign.
IntegerAttr MemRefBuilder::computeAlignment(int64_t alignment) const {
  alignment = (alignment > gDefaultAllocAlign ? alignment : gDefaultAllocAlign);
  return b().getI64IntegerAttr(alignment);
}

// Alloc calls need a list of values, only for the dynamic shapes. Extract these
// values from the list of index expressions that represent the shape of the
// memref.
void MemRefBuilder::computeDynSymbols(MemRefType type,
    llvm::SmallVectorImpl<IndexExpr> &dims,
    llvm::SmallVectorImpl<Value> &dynSymbols) const {
  dynSymbols.clear();
  int64_t rank = type.getRank();
  ArrayRef<int64_t> shape = type.getShape();
  for (int64_t i = 0; i < rank; ++i)
    if (shape[i] == ShapedType::kDynamic)
      dynSymbols.emplace_back(dims[i].getValue());
}

// Alloc calls need a list of values, only for the dynamic shapes. Extract these
// values from an existing operands that has the same shape. Use dim ops for
// each dynamic dimension.
void MemRefBuilder::computeDynSymbols(Value operandOfSameType, MemRefType type,
    llvm::SmallVectorImpl<Value> &dynSymbols) const {
  dynSymbols.clear();
  if (operandOfSameType == nullptr)
    return;
  int64_t rank = type.getRank();
  ArrayRef<int64_t> shape = type.getShape();
  for (int64_t i = 0; i < rank; ++i)
    if (shape[i] == ShapedType::kDynamic)
      dynSymbols.emplace_back(dim(operandOfSameType, i));
}

//===----------------------------------------------------------------------===//
// Alloc functions without alignment.

memref::AllocOp MemRefBuilder::alloc(MemRefType type) const {
  llvm::SmallVector<Value, 4> dynSymbols;
  return alloc(type, dynSymbols);
}

memref::AllocOp MemRefBuilder::alloc(
    MemRefType type, ValueRange dynSymbols) const {
  // Constant, ignore the dynamic symbols.
  if (dynSymbols.size() == 0)
    return b().create<memref::AllocOp>(loc(), type);
  return b().create<memref::AllocOp>(loc(), type, dynSymbols);
}

memref::AllocOp MemRefBuilder::alloc(
    Value operandOfSameType, MemRefType type) const {
  llvm::SmallVector<Value, 4> dynSymbols;
  computeDynSymbols(operandOfSameType, type, dynSymbols);
  return alloc(type, dynSymbols);
}

memref::AllocOp MemRefBuilder::alloc(
    MemRefType type, llvm::SmallVectorImpl<IndexExpr> &dims) const {
  llvm::SmallVector<Value, 4> dynSymbols;
  computeDynSymbols(type, dims, dynSymbols);
  return alloc(type, dynSymbols);
}

//===----------------------------------------------------------------------===//
// Alloc functions with alignment.

memref::AllocOp MemRefBuilder::alignedAlloc(
    MemRefType type, int64_t alignment) const {
  llvm::SmallVector<Value, 4> dynSymbols;
  return alignedAlloc(type, dynSymbols, alignment);
}

memref::AllocOp MemRefBuilder::alignedAlloc(
    MemRefType type, ValueRange dynSymbols, int64_t alignment) const {
  // Drop align for scalars.
  if (type.getShape().size() == 0)
    return alloc(type, dynSymbols);
  // Has array, use alignment.
  IntegerAttr alignmentAttr = computeAlignment(alignment);
  // Constant, ignore the dynamic symbols.
  if (dynSymbols.size() == 0)
    return b().create<memref::AllocOp>(loc(), type, alignmentAttr);
  return b().create<memref::AllocOp>(loc(), type, dynSymbols, alignmentAttr);
}

memref::AllocOp MemRefBuilder::alignedAlloc(
    Value operandOfSameType, MemRefType type, int64_t alignment) const {
  llvm::SmallVector<Value, 4> dynSymbols;
  computeDynSymbols(operandOfSameType, type, dynSymbols);
  return alignedAlloc(type, dynSymbols, alignment);
}

memref::AllocOp MemRefBuilder::alignedAlloc(MemRefType type,
    llvm::SmallVectorImpl<IndexExpr> &dims, int64_t alignment) const {
  llvm::SmallVector<Value, 4> dynSymbols;
  computeDynSymbols(type, dims, dynSymbols);
  return alignedAlloc(type, dynSymbols, alignment);
}

//===----------------------------------------------------------------------===//
// Info about memory size.

// Compute static and dynamic size of memref. Return true if has static size.
bool MemRefBuilder::getStaticAndDynamicMemSize(MemRefType type,
    ValueRange dynSymbols, int64_t &staticSize, IndexExpr &dynSize) const {
  Type elementType = type.getElementType();
  assert(!(elementType.isa<VectorType>()) && "unsupported vector type");
  ArrayRef<int64_t> shape = type.getShape();
  staticSize = 1;                // Multiplication of static sizes.
  dynSize = LiteralIndexExpr(1); // Multiplication of dyn sizes.
  bool staticShape = (dynSymbols.size() == 0);
  int64_t rank = type.getRank();
  int64_t iDim = 0;
  for (int64_t i = 0; i < rank; ++i) {
    if (shape[i] == ShapedType::kDynamic) {
      assert(!staticShape && "expected static shape");
      assert(iDim < (int64_t)dynSymbols.size() && "not enough dynamic symbols");
      dynSize = dynSize * SymbolIndexExpr(dynSymbols[iDim++]);
    } else {
      // Has constant shape.
      staticSize *= shape[i];
    }
  }
  return staticShape;
}

bool MemRefBuilder::getStaticAndDynamicMemSize(MemRefType type,
    llvm::SmallVectorImpl<IndexExpr> &dims, int64_t &staticSize,
    IndexExpr &dynSize) const {
  llvm::SmallVector<Value, 4> dynSymbols;
  computeDynSymbols(type, dims, dynSymbols);
  return getStaticAndDynamicMemSize(type, dynSymbols, staticSize, dynSize);
}

//===----------------------------------------------------------------------===//
// Alloc functions with alignment and padding for SIMD

Value MemRefBuilder::alignedAllocWithSimdPadding(
    mlir::MemRefType type, int64_t simdUnroll, int64_t alignment) const {
  llvm::SmallVector<Value, 4> dynSymbols;
  return alignedAllocWithSimdPadding(type, dynSymbols, simdUnroll, alignment);
}

Value MemRefBuilder::alignedAllocWithSimdPadding(MemRefType type,
    ValueRange dynSymbols, int64_t simdUnroll, int64_t alignment) const {
  Type elementType = type.getElementType();
  assert(!hasNonIdentityLayout(type) && "unsupported layout");
  assert(!(elementType.isa<VectorType>()) && "unsupported vector type");
  assert(simdUnroll >= 1 && "expected positive simd unroll factor");
  // Compute total size of memref (in unit of element type).
  int64_t staticSize;
  IndexExpr dynSize;
  bool staticShape =
      getStaticAndDynamicMemSize(type, dynSymbols, staticSize, dynSize);
  // Get vector length for this element type, multiplied by the unroll factor.
  MultiDialectBuilder<VectorBuilder> create(*this);
  int64_t VL = create.vec.getMachineVectorLength(elementType) * simdUnroll;
  // If the static size component is already a multiple of VL, no matter the
  // values of the dynamic shapes, the last value is part of a full SIMD. No
  // need for extra padding then.
  if (staticSize % VL == 0)
    return alignedAlloc(type, dynSymbols, alignment);

  // We now need some padding. VL as this is an upper bound on padding. Padding
  // in element size.
  int64_t paddingSize = VL;
  if (staticShape)
    // Static shape: we can pad by the exact right amount.
    paddingSize = VL - staticSize % VL;

  // Allocate data as byte.
  int64_t bitWidth = elementType.getIntOrFloatBitWidth();
  IndexExpr totPaddedByteSize;
  if (bitWidth % 8 == 0) {
    // We have elements that have sizes of 1 or more bytes.
    int64_t byteWidth = bitWidth / 8;
    IndexExpr totByteSize = LiteralIndexExpr(staticSize * byteWidth) * dynSize;
    totPaddedByteSize = totByteSize + LiteralIndexExpr(paddingSize * byteWidth);
  } else {
    // We have sub-byte element sizes. Need to do precise computations. Namely
    // first compute tot total number of bits (including static/dynamic
    // and padding bit sizes), and then doing a ceil division by
    // 8 (number of bits in a byte).
    IndexExpr totBitSize = LiteralIndexExpr(staticSize * bitWidth) * dynSize;
    IndexExpr totPaddedBitSize =
        totBitSize + LiteralIndexExpr(paddingSize * bitWidth);
    totPaddedByteSize = totPaddedBitSize.ceilDiv(LiteralIndexExpr(8));
  }
  if (staticShape)
    assert(totPaddedByteSize.isLiteral() && "expected literal padded tot size");
  // Construct memref for padded array of bytes.
  memref::AllocOp paddedAlloc;
  if (totPaddedByteSize.isLiteral()) {
    MemRefType paddedType =
        MemRefType::get({totPaddedByteSize.getLiteral()}, b().getI8Type());
    paddedAlloc = alignedAlloc(paddedType, alignment);
  } else {
    MemRefType paddedType =
        MemRefType::get({ShapedType::kDynamic}, b().getI8Type());
    paddedAlloc =
        alignedAlloc(paddedType, {totPaddedByteSize.getValue()}, alignment);
  }
  // Used to create a subview, it does not appear that the view cares about
  // whether the entire input data participates in the viewed data or not.
  return view(paddedAlloc, /*offset*/ 0, type, dynSymbols);
}

Value MemRefBuilder::alignedAllocWithSimdPadding(Value operandOfSameType,
    MemRefType type, int64_t simdUnroll, int64_t alignment) const {
  llvm::SmallVector<Value, 4> dynSymbols;
  computeDynSymbols(operandOfSameType, type, dynSymbols);
  return alignedAllocWithSimdPadding(type, dynSymbols, simdUnroll, alignment);
}

Value MemRefBuilder::alignedAllocWithSimdPadding(MemRefType type,
    llvm::SmallVectorImpl<IndexExpr> &dims, int64_t simdUnroll,
    int64_t alignment) const {
  llvm::SmallVector<Value, 4> dynSymbols;
  computeDynSymbols(type, dims, dynSymbols);
  return alignedAllocWithSimdPadding(type, dynSymbols, simdUnroll, alignment);
}

//===----------------------------------------------------------------------===//
// Alloca

memref::AllocaOp MemRefBuilder::alloca(MemRefType type) const {
  return b().create<memref::AllocaOp>(loc(), type);
}

memref::AllocaOp MemRefBuilder::alignedAlloca(
    MemRefType type, int64_t alignment) const {
  // Drop align for scalars.
  if (type.getShape().size() == 0)
    return b().create<memref::AllocaOp>(loc(), type);
  // Has array, use alignment.
  IntegerAttr alignmentAttr = computeAlignment(alignment);
  return b().create<memref::AllocaOp>(loc(), type, alignmentAttr);
}

//===----------------------------------------------------------------------===//
// Dealloc.

memref::DeallocOp MemRefBuilder::dealloc(Value val) const {
  return b().create<memref::DeallocOp>(loc(), val);
}

//===----------------------------------------------------------------------===//
// Reshape.

memref::ReshapeOp MemRefBuilder::reshape(
    MemRefType destType, Value valToReshape, Value destShapeStoredInMem) const {
  return b().create<memref::ReshapeOp>(
      loc(), destType, valToReshape, destShapeStoredInMem);
}

// Flatten the innermost dimsToFlatten of the value valToReshape. Return in
// flattenSize the cumulative size of the flattened dimensions. If flattenSize
// is -1, flatten them all. Expect to flatten at least 1 dim (which is a noop).
// Output rank is Rank(input) - dimsToFlatten + 1.
Value MemRefBuilder::reshapeToFlat(Value valToReshape,
    llvm::SmallVectorImpl<IndexExpr> &dims, Value &flattenedSize,
    int64_t dimsToFlatten) const {
  // Parse input.
  MemRefType inputType = valToReshape.getType().cast<MemRefType>();
  int64_t inputRank = inputType.getRank();
  assert(inputRank == (int64_t)dims.size() && "rank mismatch");
  Type elementType = inputType.getElementType();
  assert(!hasNonIdentityLayout(inputType) && "MemRef is not normalized");
  // Set/check dimsToFlatten.
  if (dimsToFlatten == -1)
    dimsToFlatten = inputRank;
  assert(dimsToFlatten > 0 && dimsToFlatten <= inputRank &&
         "out of range dimsToFlatten");
  // Create scope to avoid issues.
  IndexExprScope innerScope(getBuilderPtr(), loc());
  MultiDialectBuilder<AffineBuilder, MathBuilder> create(*this);
  // Compute total number of flattened elements in new scope.
  IndexExpr numOfFlattenedElements = LiteralIndexExpr(1);
  for (int64_t d = inputRank - dimsToFlatten; d < inputRank; ++d) {
    numOfFlattenedElements = numOfFlattenedElements * SymbolIndexExpr(dims[d]);
  }
  // flattenedSize is an output value corresponding to the total number of
  // elements that were flattened.
  flattenedSize = numOfFlattenedElements.getValue();
  if (dimsToFlatten == 1)
    // Flattening of the last dim is really no flattening at all. Return
    // original value before doing the actual reshaping, which is unnecessary.
    // Waited until here as we need to return a valid flattenedSize,
    return valToReshape;
  // Shape for reshaping from N-D to M-D saved into memory.
  int64_t outputRank = (inputRank - dimsToFlatten) + 1;
  Type indexType = b().getIndexType();
  Value outputShapeInMem =
      alignedAlloc(MemRefType::get({outputRank}, indexType));
  llvm::SmallVector<int64_t, 4> outputShape;
  // Compute shape and store it in memory.
  for (int64_t d = 0; d < outputRank; ++d) {
    Value dd = create.math.constantIndex(d);
    IndexExpr shapeIE =
        (d == outputRank - 1) ? numOfFlattenedElements : dims[d];
    create.affine.store(shapeIE.getValue(), outputShapeInMem, {dd});
    outputShape.emplace_back(shapeIE.getShape());
  }
  // Reshape the input N-D MemRef into a M-D MemRef.
  MemRefType outputType = MemRefType::get(outputShape, elementType);
  return reshape(outputType, valToReshape, outputShapeInMem);
}

memref::ReshapeOp MemRefBuilder::reshapeFromFlat(Value valToReshape,
    llvm::SmallVectorImpl<IndexExpr> &dims, MemRefType outputType) const {
  assert(!hasNonIdentityLayout(outputType) && "MemRef is not normalized");
  MultiDialectBuilder<AffineBuilder, MathBuilder> create(*this);
  Type indexType = b().getIndexType();
  int64_t rank = outputType.getRank();
  // Shape for reshaping from N1D to N-D saved into memory.
  Value shapeND = alignedAlloc(MemRefType::get({rank}, indexType));
  for (int64_t i = 0; i < rank; ++i) {
    Value index = create.math.constantIndex(i);
    create.affine.store(dims[i].getValue(), shapeND, {index});
  }
  // Reshape the 1-D MemRef into a N-D MemRef.
  return reshape(outputType, valToReshape, shapeND);
}

//===----------------------------------------------------------------------===//
// Casts and views.

memref::CastOp MemRefBuilder::cast(Value input, MemRefType outputType) const {
  return b().create<memref::CastOp>(loc(), outputType, input);
}

Value MemRefBuilder::reinterpretCast(
    Value input, SmallVectorImpl<IndexExpr> &outputDims) const {
  // Compute new sizes and strides.
  int64_t rank = outputDims.size();
  SmallVector<IndexExpr, 4> sizesIE, stridesIE;
  sizesIE.resize(rank);
  stridesIE.resize(rank);
  IndexExpr strideIE = LiteralIndexExpr(1);
  for (int i = rank - 1; i >= 0; --i) {
    sizesIE[i] = outputDims[i];
    stridesIE[i] = strideIE;
    if (i > 0)
      strideIE = strideIE * sizesIE[i];
  }
  // Compute output type
  SmallVector<int64_t, 4> outputShape;
  SmallVector<OpFoldResult, 4> sizes, strides;
  IndexExpr::getShape(outputDims, outputShape);
  IndexExpr::getOpOrFoldResults(sizesIE, sizes);
  IndexExpr::getOpOrFoldResults(stridesIE, strides);
  Type elementType = input.getType().cast<ShapedType>().getElementType();
  MemRefType outputMemRefType = MemRefType::get(outputShape, elementType);

  return b().create<memref::ReinterpretCastOp>(loc(), outputMemRefType, input,
      /*offset=*/b().getIndexAttr(0), sizes, strides);
}

Value MemRefBuilder::collapseShape(
    Value input, ArrayRef<ReassociationIndices> reassociation) {
  // Extract input info.
  MemRefType inputType = input.getType().cast<MemRefType>();
  assert(inputType && "expected input with memref type");
  assert(!hasNonIdentityLayout(inputType) &&
         "collapse only for identity layout at this time");
  int64_t inputRank = inputType.getRank();
  ArrayRef<int64_t> inputShape = inputType.getShape();
  // Compute shape of output.
  int64_t outputRank = reassociation.size();
  SmallVector<int64_t, 4> outputShape;
  for (int64_t r = 0; r < outputRank; ++r) {
    int64_t indexNum = reassociation[r].size();
    assert(indexNum > 0 && "expect one or more index in reassociation indices");
    // Compute the cumulative size of the output dim as the product of all dim
    // of the sizes in the input being re-associated with this output.
    int64_t currShape = 1;
    for (int64_t i = 0; i < indexNum; i++) {
      int64_t ii = reassociation[r][i];
      assert(ii >= 0 && ii < inputRank && "out of bound reassociation index");
      int64_t ss = inputShape[ii];
      if (ss == ShapedType::kDynamic) {
        // If a re-associated shapes is dynamic, output is dynamic.
        currShape = ShapedType::kDynamic;
        break;
      }
      currShape *= ss;
    }
    outputShape.emplace_back(currShape);
  }
  // Compute type of output.
  MemRefType outputType =
      MemRefType::get(outputShape, inputType.getElementType());
  // Create collapse shape op.
  return b().create<memref::CollapseShapeOp>(
      loc(), outputType, input, reassociation);
}

memref::ViewOp MemRefBuilder::view(Value input, int64_t byteOffset,
    MemRefType outputType, ValueRange outputDynSymbols) const {
  MultiDialectBuilder<MathBuilder> create(*this);
  Value offset = create.math.constantIndex(byteOffset);
  // auto offset = b().createOrFold<arith::ConstantIndexOp>(byteOffset);
  return b().create<memref::ViewOp>(
      loc(), outputType, input, offset, outputDynSymbols);
}

memref::SubViewOp MemRefBuilder::subView(Value input,
    llvm::SmallVectorImpl<IndexExpr> &offsetsIE,
    llvm::SmallVectorImpl<IndexExpr> &sizesIE,
    llvm::SmallVectorImpl<IndexExpr> &stridesIE) const {
  SmallVector<OpFoldResult, 4> offsets, sizes, strides;
  IndexExpr::getOpOrFoldResults(offsetsIE, offsets);
  IndexExpr::getOpOrFoldResults(sizesIE, sizes);
  IndexExpr::getOpOrFoldResults(stridesIE, strides);
  SmallVector<int64_t, 4> outputShape;
  IndexExpr::getShape(sizesIE, outputShape);
  MemRefType inputType = input.getType().dyn_cast<MemRefType>();
  MemRefLayoutAttrInterface layout;
  MemRefType outputType = MemRefType::get(outputShape,
      inputType.getElementType(), layout, inputType.getMemorySpace());
  return b().create<memref::SubViewOp>(
      loc(), outputType, input, offsets, sizes, strides);
}

//===----------------------------------------------------------------------===//
// Dims.

Value MemRefBuilder::dim(Value val, int64_t index) const {
  assert(index >= 0 && "Expecting a valid index");
  return dim(val, b().create<arith::ConstantIndexOp>(loc(), index));
}

Value MemRefBuilder::dim(Value val, Value index) const {
  // assert((val.getType().isa<MemRefType>() ||
  //           val.getType().isa<UnrankedMemRefType>()) &&
  //       "memref::DimOp expects input operand to have MemRefType or "
  //       "UnrankedMemRefType");
  return Value(b().createOrFold<memref::DimOp>(loc(), val, index));
}

//===----------------------------------------------------------------------===//
// Structured Control Flow (SCF).
//===----------------------------------------------------------------------===//

void SCFBuilder::ifThenElse(Value cond,
    function_ref<void(SCFBuilder &createSCF)> thenFn,
    function_ref<void(SCFBuilder &createSCF)> elseFn) const {
  if (!elseFn) {
    b().create<scf::IfOp>(loc(), cond,
        /* then */
        [&](OpBuilder &childBuilder, Location childLoc) {
          SCFBuilder scfBuilder(childBuilder, childLoc);
          thenFn(scfBuilder);
          yield();
        });
  } else {
    b().create<scf::IfOp>(
        loc(), cond,
        /* then */
        [&](OpBuilder &childBuilder, Location childLoc) {
          SCFBuilder scfBuilder(childBuilder, childLoc);
          thenFn(scfBuilder);
          b().create<scf::YieldOp>(loc());
        },
        /*else*/
        [&](OpBuilder &childBuilder, Location childLoc) {
          SCFBuilder scfBuilder(childBuilder, childLoc);
          elseFn(scfBuilder);
          yield();
        });
  }
}

void SCFBuilder::parallelLoop(ValueRange lowerBounds, ValueRange upperBounds,
    ValueRange steps,
    function_ref<void(SCFBuilder &createSCF, ValueRange)> bodyFn) const {
  // SmallVectorImpl<Value> ivStorage;
  b().create<scf::ParallelOp>(loc(), lowerBounds, upperBounds, steps,
      [&](OpBuilder &childBuilder, Location childLoc,
          ValueRange inductionVars) {
        SCFBuilder builder(childBuilder, childLoc);
        bodyFn(builder, inductionVars);
        yield();
      });
}

void SCFBuilder::yield() const { b().create<scf::YieldOp>(loc()); }

//===----------------------------------------------------------------------===//
// Vector Builder
//===----------------------------------------------------------------------===//

int64_t VectorBuilder::getMachineVectorLength(const Type &elementType) const {
  VectorMachineSupport *vms =
      VectorMachineSupport::getGlobalVectorMachineSupport();
  // Even if unsupported, we can always compute one result per vector.
  return std::max((int64_t)1, vms->getVectorLength(elementType));
}

int64_t VectorBuilder::getMachineVectorLength(const VectorType &vecType) const {
  return getMachineVectorLength(vecType.getElementType());
}

int64_t VectorBuilder::getMachineVectorLength(Value vecValue) const {
  VectorType vecType = vecValue.getType().dyn_cast_or_null<VectorType>();
  assert(vecType && "expected vector type");
  return getMachineVectorLength(vecType.getElementType());
}

Value VectorBuilder::load(
    VectorType vecType, Value memref, ValueRange indices) const {
  return b().create<vector::LoadOp>(loc(), vecType, memref, indices);
}
mlir::Value VectorBuilder::load(mlir::VectorType vecType, mlir::Value memref,
    mlir::ValueRange indices, mlir::ValueRange offsets) const {
  llvm::SmallVector<mlir::Value, 4> computedIndices;
  MultiDialectBuilder<MathBuilder> create(*this);
  create.math.addOffsetToLeastSignificant(indices, offsets, computedIndices);
  return load(vecType, memref, computedIndices);
}

mlir::Value VectorBuilder::loadIE(mlir::VectorType vecType, mlir::Value memref,
    llvm::ArrayRef<IndexExpr> indices, mlir::ValueRange offsets) const {
  llvm::SmallVector<mlir::Value, 4> computedIndices;
  MultiDialectBuilder<MathBuilder> create(*this);
  create.math.addOffsetToLeastSignificant(indices, offsets, computedIndices);
  return load(vecType, memref, computedIndices);
}

void VectorBuilder::store(Value val, Value memref, ValueRange indices) const {
  b().create<vector::StoreOp>(loc(), val, memref, indices);
}

void VectorBuilder::store(mlir::Value val, mlir::Value memref,
    mlir::ValueRange indices, mlir::ValueRange offsets) const {
  llvm::SmallVector<mlir::Value, 4> computedIndices;
  MultiDialectBuilder<MathBuilder> create(*this);
  create.math.addOffsetToLeastSignificant(indices, offsets, computedIndices);
  store(val, memref, computedIndices);
}

void VectorBuilder::storeIE(mlir::Value val, mlir::Value memref,
    llvm::ArrayRef<IndexExpr> indices, mlir::ValueRange offsets) const {
  llvm::SmallVector<mlir::Value, 4> computedIndices;
  MultiDialectBuilder<MathBuilder> create(*this);
  create.math.addOffsetToLeastSignificant(indices, offsets, computedIndices);
  store(val, memref, computedIndices);
}

Value VectorBuilder::fma(Value lhs, Value rhs, Value acc) const {
  return b().create<vector::FMAOp>(loc(), lhs, rhs, acc);
}

// Val is required to be a index/integer/float.
Value VectorBuilder::splat(VectorType vecType, Value val) const {
  return b().create<vector::SplatOp>(loc(), vecType, val);
}

Value VectorBuilder::broadcast(VectorType vecType, Value val) const {
  return b().create<vector::BroadcastOp>(loc(), vecType, val);
}

Value VectorBuilder::shuffle(
    Value lhs, Value rhs, SmallVectorImpl<int64_t> &mask) const {
  return b().create<vector::ShuffleOp>(loc(), lhs, rhs, mask);
}

// Private vector utilities.
bool VectorBuilder::isPowerOf2(uint64_t num) const {
  return (num & (num - 1)) == 0;
}

uint64_t VectorBuilder::getLengthOf1DVector(Value vec) const {
  VectorType vecType = vec.getType().dyn_cast_or_null<VectorType>();
  assert(vecType && "expected a vector type");
  auto vecShape = vecType.getShape();
  assert(vecShape.size() == 1 && "expected a 1D vector");
  return vecShape[0];
}

Value VectorBuilder::mergeHigh(Value lhs, Value rhs, int64_t step) const {
  // Inputs: lrs <l0, l1, l2, l3, l4, l5, l6, l7>;
  //         rhs <r0, r1, r2, r3, r4, r5, r6, r7>.
  // Merge alternatively the low (least significant) values of lrs and rhs
  // Step 1:     <(l0), (r0), (l1), (r1), (l2), (r2), (l3), (r3)> (1x sizes)
  // Step 2:     <(l0, l1),   (r0, r1),   (l2, l3),   (r2, r3)>   (2x sizes)
  // Step 4:     <(l0, l1, l2, l3),       (r0, r1, r2, r3)>       (4x sizes)
  uint64_t VL = getLengthOf1DVector(lhs);
  assert(getLengthOf1DVector(rhs) == VL && "expected same sized vectors");
  assert(isPowerOf2(VL) && "expected power of 2 vector length");
  SmallVector<int64_t, 8> mask(VL, 0);
  int i = 0;
  int64_t pairsOfLhsRhs = VL / (2 * step);
  int64_t firstHalf = 0;
  for (int64_t p = 0; p < pairsOfLhsRhs; ++p) {
    // One step-sized item from the LHS
    for (int64_t e = 0; e < step; ++e)
      mask[i++] = firstHalf + p * step + e;
    // One step-sized item from the RHS (RHS offset is VL for the shuffle op).
    for (int64_t e = 0; e < step; ++e)
      mask[i++] = firstHalf + VL + p * step + e;
  }
  return shuffle(lhs, rhs, mask);
}

Value VectorBuilder::mergeLow(Value lhs, Value rhs, int64_t step) const {
  // Inputs: lrs <l0, l1, l2, l3, l4, l5, l6, l7>;
  //         rhs <r0, r1, r2, r3, r4, r5, r6, r7>.
  // Merge alternatively the low (least significant) values of lrs and rhs
  // Step 1:     <(l4), (r4), (l5), (r5), (l6), (r6), (l7), (r7)> (1x sizes)
  // Step 2:     <(l4, l5),   (r4, r5),   (l6, l7),   (r6, r7)>   (2x sizes)
  // Step 4:     <(l4, l5, l6, l7),       (r4, r5, r6, r7)>       (4x sizes)
  uint64_t VL = getLengthOf1DVector(lhs);
  assert(getLengthOf1DVector(rhs) == VL && "expected same sized vectors");
  assert(isPowerOf2(VL) && "expected power of 2 vector length");
  SmallVector<int64_t, 8> mask(VL, 0);
  int i = 0;
  int64_t pairsOfLhsRhs = VL / (2 * step);
  int64_t secondHalf = VL / 2;
  for (int64_t p = 0; p < pairsOfLhsRhs; ++p) {
    // One step-sized item from the LHS
    for (int64_t e = 0; e < step; ++e)
      mask[i++] = secondHalf + p * step + e;
    // One step-sized item from the RHS (RHS offset is VL for the shuffle op).
    for (int64_t e = 0; e < step; ++e)
      mask[i++] = secondHalf + VL + p * step + e;
  }
  return shuffle(lhs, rhs, mask);
}

// Do a parallel-simd reduction of N vectors of SIMD length VL.
// Restrictions:
// *  VL is the vector length of the machine SIMD vectors.
// *  N is a multiple of VL as we can perform consecutive VL x VL
//    reductions.
void VectorBuilder::multiReduction(SmallVectorImpl<Value> &inputVecArray,
    SmallVectorImpl<Value> &outputVecArray) {
  uint64_t N = inputVecArray.size();
  assert(N > 0 && "expected at least one value to reduce");
  uint64_t VL = getLengthOf1DVector(inputVecArray[0]);
  uint64_t machineVL = getMachineVectorLength(inputVecArray[0]);
  assert(VL == machineVL && "only natural sizes supported at this time");
  assert(N % machineVL == 0 &&
         "can only reduces multiple of VL vectors at this time");
  LLVM_DEBUG(llvm::dbgs() << "reduction with N " << N << ", VL " << VL
                          << ", mVL " << machineVL << "\n";);

  // Emplace all input vectors in a temporary array.
  SmallVector<Value, 8> tmpArray;
  for (uint64_t i = 0; i < N; ++i) {
    tmpArray.emplace_back(inputVecArray[i]);
    // Also verify that all have the same vector length.
    assert(getLengthOf1DVector(inputVecArray[i]) == VL &&
           "different vector length");
  }

  // Reductions of full physical vectors.
  outputVecArray.clear();
  MultiDialectBuilder<MathBuilder> create(*this);
  for (uint64_t r = 0; r < N; r += machineVL) {
    // Algorithm for the set of input arrays from tmp[r] to
    // tmp[r+machineVL-1].
    uint64_t numPairs = machineVL / 2; // Pair number decrease by power of 2.
    for (uint64_t step = 1; step < machineVL; step = step * 2) {
      for (uint64_t p = 0; p < numPairs; ++p) {
        Value highVal =
            mergeHigh(tmpArray[r + 2 * p], tmpArray[r + 2 * p + 1], step);
        Value lowVal =
            mergeLow(tmpArray[r + 2 * p], tmpArray[r + 2 * p + 1], step);
        Value red = create.math.add(highVal, lowVal);
        tmpArray[r + p] = red;
      }
      numPairs = numPairs / 2; // Pair number decrease by power of 2.
    }
    // Completed the machineVL x machineVL reduction, save it in the output.
    outputVecArray.emplace_back(tmpArray[r]);
  }
}

//===----------------------------------------------------------------------===//
// LLVM Builder
//===----------------------------------------------------------------------===//

Value LLVMBuilder::add(Value lhs, Value rhs) const {
  return b().create<LLVM::AddOp>(loc(), lhs, rhs);
}

Value LLVMBuilder::addressOf(LLVM::GlobalOp op) const {
  return b().create<LLVM::AddressOfOp>(loc(), op);
}

Value LLVMBuilder::_alloca(
    Type resultType, Type elementType, Value size, int64_t alignment) const {
  return b().create<LLVM::AllocaOp>(
      loc(), resultType, elementType, size, alignment);
}

Value LLVMBuilder::bitcast(Type type, Value val) const {
  return b().create<LLVM::BitcastOp>(loc(), type, val);
}

void LLVMBuilder::br(ArrayRef<Value> destOperands, Block *destBlock) const {
  b().create<LLVM::BrOp>(loc(), destOperands, destBlock);
}

Value LLVMBuilder::call(ArrayRef<Type> resultTypes, StringRef funcName,
    ArrayRef<Value> inputs) const {
  assert((resultTypes.size() == 0 || resultTypes.size() == 1) &&
         "LLVM:CallOp must return either 0 or 1 value");
  LLVM::CallOp callOp =
      b().create<LLVM::CallOp>(loc(), resultTypes, funcName, inputs);
  // CallOp may return either 0 or 1 value.
  if (resultTypes.empty())
    return nullptr;
  return callOp.getResult();
}

Value LLVMBuilder::call(ArrayRef<Type> resultTypes,
    FlatSymbolRefAttr funcSymbol, ArrayRef<Value> inputs) const {
  assert((resultTypes.size() == 0 || resultTypes.size() == 1) &&
         "LLVM:CallOp must return either 0 or 1 value");
  LLVM::CallOp callOp =
      b().create<LLVM::CallOp>(loc(), resultTypes, funcSymbol, inputs);
  // CallOp may return either 0 or 1 value.
  if (resultTypes.empty())
    return nullptr;
  return callOp.getResult();
}

void LLVMBuilder::condBr(Value cond, Block *trueBlock,
    llvm::ArrayRef<Value> trueOperands, Block *falseBlock,
    llvm::ArrayRef<Value> falseOperands) const {
  b().create<LLVM::CondBrOp>(
      loc(), cond, trueBlock, trueOperands, falseBlock, falseOperands);
}

Value LLVMBuilder::constant(Type type, int64_t val) const {
  Value constant = nullptr;
  TypeSwitch<Type>(type)
      .Case<IntegerType>([&](IntegerType type) {
        unsigned width = type.getWidth();
        if (width == 1)
          constant = b().create<LLVM::ConstantOp>(
              loc(), type, b().getBoolAttr(val != 0));
        else {
          assert(type.isSignless() &&
                 "LLVM::ConstantOp requires a signless type.");
          constant = b().create<LLVM::ConstantOp>(loc(), type,
              b().getIntegerAttr(type, APInt(width, (int64_t)val)));
        }
      })
      .Case<IndexType>([&](Type) {
        constant = b().create<LLVM::ConstantOp>(
            loc(), type, b().getIntegerAttr(type, val));
      })
      .Default([](Type) { llvm_unreachable("unsupported element type"); });

  assert(constant != nullptr && "Expecting valid constant value");
  return constant;
}

Value LLVMBuilder::constant(Type type, double val) const {
  Value constant = nullptr;
  TypeSwitch<Type>(type)
      .Case<Float16Type>([&](Type) {
        constant =
            b().create<LLVM::ConstantOp>(loc(), type, b().getF16FloatAttr(val));
      })
      .Case<Float32Type>([&](Type) {
        constant =
            b().create<LLVM::ConstantOp>(loc(), type, b().getF32FloatAttr(val));
      })
      .Case<Float64Type>([&](Type) {
        constant =
            b().create<LLVM::ConstantOp>(loc(), type, b().getF64FloatAttr(val));
      })
      .Default([](Type) { llvm_unreachable("unsupported element type"); });

  assert(constant != nullptr && "Expecting valid constant value");
  return constant;
}

Value LLVMBuilder::extractValue(
    Type resultType, Value container, ArrayRef<int64_t> position) const {
  return b().create<LLVM::ExtractValueOp>(
      loc(), resultType, container, position);
}

LLVM::LLVMFuncOp LLVMBuilder::func(StringRef name, Type type) const {
  return b().create<LLVM::LLVMFuncOp>(loc(), name, type);
}

Value LLVMBuilder::getElemPtr(Type resultType, Type elemType, Value base,
    ArrayRef<LLVM::GEPArg> indices) const {
  return b().create<LLVM::GEPOp>(loc(), resultType, elemType, base, indices);
}

LLVM::GlobalOp LLVMBuilder::globalOp(Type resultType, bool isConstant,
    LLVM::Linkage linkage, StringRef name, Attribute valueAttr,
    uint64_t alignment) const {
  return b().create<LLVM::GlobalOp>(loc(), resultType,
      /*isConstant=*/isConstant, linkage, name, valueAttr);
}

Value LLVMBuilder::icmp(LLVM::ICmpPredicate cond, Value lhs, Value rhs) const {
  return b().create<LLVM::ICmpOp>(loc(), cond, lhs, rhs);
}

Value LLVMBuilder::insertValue(Type resultType, Value container, Value val,
    llvm::ArrayRef<int64_t> position) const {
  return b().create<LLVM::InsertValueOp>(
      loc(), resultType, container, val, position);
}

Value LLVMBuilder::inttoptr(Type type, Value val) const {
  return b().create<LLVM::IntToPtrOp>(loc(), type, val);
}

Value LLVMBuilder::load(Type elementType, Value addr) const {
  return b().create<LLVM::LoadOp>(loc(), elementType, addr);
}

Value LLVMBuilder::mul(Value lhs, Value rhs) const {
  return b().create<LLVM::MulOp>(loc(), lhs, rhs);
}

Value LLVMBuilder::null(Type type) const {
  return b().create<LLVM::NullOp>(loc(), type);
}

Value LLVMBuilder::ptrtoint(Type type, Value val) const {
  return b().create<LLVM::PtrToIntOp>(loc(), type, val);
}

void LLVMBuilder::_return(Value val) const {
  b().create<LLVM::ReturnOp>(loc(), ArrayRef<Value>({val}));
}

Value LLVMBuilder::sext(Type type, Value val) const {
  return b().create<LLVM::SExtOp>(loc(), type, val);
}

void LLVMBuilder::store(Value val, Value addr) const {
  b().create<LLVM::StoreOp>(loc(), val, addr);
}

FlatSymbolRefAttr LLVMBuilder::getOrInsertSymbolRef(ModuleOp module,
    StringRef funcName, Type resultType, ArrayRef<Type> operandTypes,
    bool isVarArg) const {
  if (!module.lookupSymbol<LLVM::LLVMFuncOp>(funcName)) {
    OpBuilder::InsertionGuard guard(b());
    b().setInsertionPointToStart(module.getBody());
    LLVM::LLVMFunctionType funcType =
        LLVM::LLVMFunctionType::get(resultType, operandTypes, isVarArg);
    b().create<LLVM::LLVMFuncOp>(module.getLoc(), funcName, funcType);
  }
  return SymbolRefAttr::get(b().getContext(), funcName);
}

void LLVMBuilder::ifThenElse(
    valueFuncRef cond, voidFuncRef thenFn, voidFuncRef elseFn) const {
  LLVMBuilder createLLVM(b(), loc());

  // Split the current block into IF, THEN, ELSE and END blocks.
  Block *ifBlock, *thenBlock, *elseBlock, *endBlock;
  ifBlock = b().getInsertionBlock();
  thenBlock = ifBlock->splitBlock(b().getInsertionPoint());
  elseBlock = b().createBlock(
      thenBlock->getParent(), std::next(Region::iterator(thenBlock)));
  if (elseFn)
    endBlock = b().createBlock(
        elseBlock->getParent(), std::next(Region::iterator(elseBlock)));
  else
    endBlock = elseBlock;

  // Emit code for the IF block.
  b().setInsertionPointToEnd(ifBlock);
  Value condVal = cond(createLLVM);

  // Branch the block into the THEN and ELSE blocks.
  createLLVM.condBr(condVal, thenBlock, {}, elseBlock, {});

  // Emit code for the THEN block.
  b().setInsertionPointToStart(thenBlock);
  thenFn(createLLVM);
  if (thenBlock->hasNoSuccessors() && !isa<LLVM::ReturnOp>(thenBlock->back()))
    br({}, endBlock);

  // Emit code for the ELSE block if required.
  b().setInsertionPointToStart(elseBlock);
  if (elseFn) {
    elseFn(createLLVM);
    if (elseBlock->hasNoSuccessors() && !isa<LLVM::ReturnOp>(elseBlock->back()))
      br({}, endBlock);
  }

  // End if-then-else and return to the main body.
  b().setInsertionPointToStart(endBlock);
}

} // namespace onnx_mlir
