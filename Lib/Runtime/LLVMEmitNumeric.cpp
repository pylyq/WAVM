#include "IR/Operators.h"
#include "IR/Types.h"
#include "Inline/Assert.h"
#include "LLVMEmitFunctionContext.h"
#include "LLVMEmitModuleContext.h"
#include "LLVMEmitWorkarounds.h"
#include "LLVMJIT.h"

using namespace LLVMJIT;
using namespace IR;

//
// Constant operators
//

#define EMIT_CONST(typeId, NativeType)                                                             \
	void EmitFunctionContext::typeId##_const(LiteralImm<NativeType> imm)                           \
	{                                                                                              \
		push(emitLiteral(imm.value));                                                              \
	}
EMIT_CONST(i32, I32)
EMIT_CONST(i64, I64)
EMIT_CONST(f32, F32)
EMIT_CONST(f64, F64)
EMIT_CONST(v128, V128)

//
// Numeric operator macros
//

#define EMIT_BINARY_OP(typeId, name, emitCode)                                                     \
	void EmitFunctionContext::typeId##_##name(NoImm)                                               \
	{                                                                                              \
		const ValueType type = ValueType::typeId;                                                  \
		SUPPRESS_UNUSED(type);                                                                     \
		auto right = pop();                                                                        \
		auto left  = pop();                                                                        \
		push(emitCode);                                                                            \
	}

#define EMIT_INT_BINARY_OP(name, emitCode)                                                         \
	EMIT_BINARY_OP(i32, name, emitCode)                                                            \
	EMIT_BINARY_OP(i64, name, emitCode)

#define EMIT_FP_BINARY_OP(name, emitCode)                                                          \
	EMIT_BINARY_OP(f32, name, emitCode)                                                            \
	EMIT_BINARY_OP(f64, name, emitCode)

#define EMIT_UNARY_OP(typeId, name, emitCode)                                                      \
	void EmitFunctionContext::typeId##_##name(NoImm)                                               \
	{                                                                                              \
		const ValueType type = ValueType::typeId;                                                  \
		SUPPRESS_UNUSED(type);                                                                     \
		auto operand = pop();                                                                      \
		push(emitCode);                                                                            \
	}

#define EMIT_INT_UNARY_OP(name, emitCode)                                                          \
	EMIT_UNARY_OP(i32, name, emitCode)                                                             \
	EMIT_UNARY_OP(i64, name, emitCode)

#define EMIT_FP_UNARY_OP(name, emitCode)                                                           \
	EMIT_UNARY_OP(f32, name, emitCode)                                                             \
	EMIT_UNARY_OP(f64, name, emitCode)

#define EMIT_SIMD_BINARY_OP(name, llvmType, emitCode)                                              \
	void EmitFunctionContext::name(IR::NoImm)                                                      \
	{                                                                                              \
		llvm::Type* vectorType = llvmType;                                                         \
		SUPPRESS_UNUSED(vectorType);                                                               \
		auto right = irBuilder.CreateBitCast(pop(), llvmType);                                     \
		SUPPRESS_UNUSED(right);                                                                    \
		auto left = irBuilder.CreateBitCast(pop(), llvmType);                                      \
		SUPPRESS_UNUSED(left);                                                                     \
		push(emitCode);                                                                            \
	}
#define EMIT_SIMD_UNARY_OP(name, llvmType, emitCode)                                               \
	void EmitFunctionContext::name(IR::NoImm)                                                      \
	{                                                                                              \
		auto operand = irBuilder.CreateBitCast(pop(), llvmType);                                   \
		SUPPRESS_UNUSED(operand);                                                                  \
		push(emitCode);                                                                            \
	}

#define EMIT_SIMD_INT_BINARY_OP(name, emitCode)                                                    \
	EMIT_SIMD_BINARY_OP(i8x16##_##name, llvmI8x16Type, emitCode)                                   \
	EMIT_SIMD_BINARY_OP(i16x8##_##name, llvmI16x8Type, emitCode)                                   \
	EMIT_SIMD_BINARY_OP(i32x4##_##name, llvmI32x4Type, emitCode)                                   \
	EMIT_SIMD_BINARY_OP(i64x2##_##name, llvmI64x2Type, emitCode)

#define EMIT_SIMD_FP_BINARY_OP(name, emitCode)                                                     \
	EMIT_SIMD_BINARY_OP(f32x4##_##name, llvmF32x4Type, emitCode)                                   \
	EMIT_SIMD_BINARY_OP(f64x2##_##name, llvmF64x2Type, emitCode)

#define EMIT_SIMD_INT_UNARY_OP(name, emitCode)                                                     \
	EMIT_SIMD_UNARY_OP(i8x16##_##name, llvmI8x16Type, emitCode)                                    \
	EMIT_SIMD_UNARY_OP(i16x8##_##name, llvmI16x8Type, emitCode)                                    \
	EMIT_SIMD_UNARY_OP(i32x4##_##name, llvmI32x4Type, emitCode)                                    \
	EMIT_SIMD_UNARY_OP(i64x2##_##name, llvmI64x2Type, emitCode)

#define EMIT_SIMD_FP_UNARY_OP(name, emitCode)                                                      \
	EMIT_SIMD_UNARY_OP(f32x4##_##name, llvmF32x4Type, emitCode)                                    \
	EMIT_SIMD_UNARY_OP(f64x2##_##name, llvmF64x2Type, emitCode)

//
// Int operators
//

llvm::Value* EmitFunctionContext::emitSRem(ValueType type, llvm::Value* left, llvm::Value* right)
{
	// Trap if the dividend is zero.
	trapDivideByZero(type, right);

	// LLVM's srem has undefined behavior where WebAssembly's rem_s defines that it should not trap
	// if the corresponding division would overflow a signed integer. To avoid this case, we just
	// branch around the srem if the INT_MAX%-1 case that overflows is detected.
	auto preOverflowBlock = irBuilder.GetInsertBlock();
	auto noOverflowBlock  = llvm::BasicBlock::Create(*llvmContext, "sremNoOverflow", llvmFunction);
	auto endBlock         = llvm::BasicBlock::Create(*llvmContext, "sremEnd", llvmFunction);
	auto noOverflow       = irBuilder.CreateOr(
        irBuilder.CreateICmpNE(
            left,
            type == ValueType::i32 ? emitLiteral((U32)INT32_MIN) : emitLiteral((U64)INT64_MIN)),
        irBuilder.CreateICmpNE(
            right, type == ValueType::i32 ? emitLiteral((U32)-1) : emitLiteral((U64)-1)));
	irBuilder.CreateCondBr(
		noOverflow, noOverflowBlock, endBlock, moduleContext.likelyTrueBranchWeights);

	irBuilder.SetInsertPoint(noOverflowBlock);
	auto noOverflowValue = irBuilder.CreateSRem(left, right);
	irBuilder.CreateBr(endBlock);

	irBuilder.SetInsertPoint(endBlock);
	auto phi = irBuilder.CreatePHI(asLLVMType(type), 2);
	phi->addIncoming(typedZeroConstants[(Uptr)type], preOverflowBlock);
	phi->addIncoming(noOverflowValue, noOverflowBlock);
	return phi;
}

static llvm::Value* emitShiftCountMask(llvm::IRBuilder<>& irBuilder,
									   ValueType type,
									   llvm::Value* shiftCount)
{
	// LLVM's shifts have undefined behavior where WebAssembly specifies that the shift count will
	// wrap numbers greater than the bit count of the operands. This matches x86's native shift
	// instructions, but explicitly mask the shift count anyway to support other platforms, and
	// ensure the optimizer doesn't take advantage of the UB.
	auto bitsMinusOne
		= irBuilder.CreateZExt(emitLiteral((U8)(getTypeBitWidth(type) - 1)), asLLVMType(type));
	return irBuilder.CreateAnd(shiftCount, bitsMinusOne);
}

llvm::Value* EmitFunctionContext::emitRotl(ValueType type, llvm::Value* left, llvm::Value* right)
{
	auto bitWidthMinusRight
		= irBuilder.CreateSub(zext(emitLiteral(getTypeBitWidth(type)), asLLVMType(type)), right);
	return irBuilder.CreateOr(
		irBuilder.CreateShl(left, emitShiftCountMask(irBuilder, type, right)),
		irBuilder.CreateLShr(left, emitShiftCountMask(irBuilder, type, bitWidthMinusRight)));
}

llvm::Value* EmitFunctionContext::emitRotr(ValueType type, llvm::Value* left, llvm::Value* right)
{
	auto bitWidthMinusRight
		= irBuilder.CreateSub(zext(emitLiteral(getTypeBitWidth(type)), asLLVMType(type)), right);
	return irBuilder.CreateOr(
		irBuilder.CreateShl(left, emitShiftCountMask(irBuilder, type, bitWidthMinusRight)),
		irBuilder.CreateLShr(left, emitShiftCountMask(irBuilder, type, right)));
}

EMIT_INT_BINARY_OP(add, irBuilder.CreateAdd(left, right))
EMIT_INT_BINARY_OP(sub, irBuilder.CreateSub(left, right))
EMIT_INT_BINARY_OP(mul, irBuilder.CreateMul(left, right))
EMIT_INT_BINARY_OP(and, irBuilder.CreateAnd(left, right))
EMIT_INT_BINARY_OP(or, irBuilder.CreateOr(left, right))
EMIT_INT_BINARY_OP(xor, irBuilder.CreateXor(left, right))
EMIT_INT_BINARY_OP(rotr, emitRotr(type, left, right))
EMIT_INT_BINARY_OP(rotl, emitRotl(type, left, right))

// Divides use trapDivideByZero to avoid the undefined behavior in LLVM's division instructions.
EMIT_INT_BINARY_OP(div_s,
				   (trapDivideByZeroOrIntegerOverflow(type, left, right),
					irBuilder.CreateSDiv(left, right)))
EMIT_INT_BINARY_OP(rem_s, emitSRem(type, left, right))
EMIT_INT_BINARY_OP(div_u, (trapDivideByZero(type, right), irBuilder.CreateUDiv(left, right)))
EMIT_INT_BINARY_OP(rem_u, (trapDivideByZero(type, right), irBuilder.CreateURem(left, right)))

// Explicitly mask the shift amount operand to the word size to avoid LLVM's undefined behavior.
EMIT_INT_BINARY_OP(shl, irBuilder.CreateShl(left, emitShiftCountMask(irBuilder, type, right)))
EMIT_INT_BINARY_OP(shr_s, irBuilder.CreateAShr(left, emitShiftCountMask(irBuilder, type, right)))
EMIT_INT_BINARY_OP(shr_u, irBuilder.CreateLShr(left, emitShiftCountMask(irBuilder, type, right)))

EMIT_INT_UNARY_OP(clz,
				  callLLVMIntrinsic({operand->getType()},
									llvm::Intrinsic::ctlz,
									{operand, emitLiteral(false)}))
EMIT_INT_UNARY_OP(ctz,
				  callLLVMIntrinsic({operand->getType()},
									llvm::Intrinsic::cttz,
									{operand, emitLiteral(false)}))
EMIT_INT_UNARY_OP(popcnt,
				  callLLVMIntrinsic({operand->getType()}, llvm::Intrinsic::ctpop, {operand}))
EMIT_INT_UNARY_OP(eqz,
				  coerceBoolToI32(irBuilder.CreateICmpEQ(operand, typedZeroConstants[(Uptr)type])))

//
// FP operators
//

EMIT_FP_BINARY_OP(add,
				  callLLVMIntrinsic({left->getType()},
									llvm::Intrinsic::experimental_constrained_fadd,
									{left,
									 right,
									 moduleContext.fpRoundingModeMetadata,
									 moduleContext.fpExceptionMetadata}))
EMIT_FP_BINARY_OP(sub,
				  callLLVMIntrinsic({left->getType()},
									llvm::Intrinsic::experimental_constrained_fsub,
									{left,
									 right,
									 moduleContext.fpRoundingModeMetadata,
									 moduleContext.fpExceptionMetadata}))
EMIT_FP_BINARY_OP(mul,
				  callLLVMIntrinsic({left->getType()},
									llvm::Intrinsic::experimental_constrained_fmul,
									{left,
									 right,
									 moduleContext.fpRoundingModeMetadata,
									 moduleContext.fpExceptionMetadata}))
EMIT_FP_BINARY_OP(div,
				  callLLVMIntrinsic({left->getType()},
									llvm::Intrinsic::experimental_constrained_fdiv,
									{left,
									 right,
									 moduleContext.fpRoundingModeMetadata,
									 moduleContext.fpExceptionMetadata}))
EMIT_FP_BINARY_OP(copysign,
				  callLLVMIntrinsic({left->getType()}, llvm::Intrinsic::copysign, {left, right}))

EMIT_FP_UNARY_OP(neg, irBuilder.CreateFNeg(operand))
EMIT_FP_UNARY_OP(abs, callLLVMIntrinsic({operand->getType()}, llvm::Intrinsic::fabs, {operand}))
EMIT_FP_UNARY_OP(sqrt,
				 callLLVMIntrinsic({operand->getType()},
								   llvm::Intrinsic::experimental_constrained_sqrt,
								   {operand,
									moduleContext.fpRoundingModeMetadata,
									moduleContext.fpExceptionMetadata}))

#define EMIT_FP_COMPARE_OP(name, predicate, llvmOperandType, llvmResultType)                       \
	void EmitFunctionContext::name(NoImm)                                                          \
	{                                                                                              \
		auto right = irBuilder.CreateBitCast(pop(), llvmOperandType);                              \
		auto left  = irBuilder.CreateBitCast(pop(), llvmOperandType);                              \
		push(zext(createFCmpWithWorkaround(irBuilder, predicate, left, right), llvmResultType));   \
	}

#define EMIT_FP_COMPARE(name, predicate)                                                           \
	EMIT_FP_COMPARE_OP(f32_##name, predicate, llvmF32Type, llvmI32Type)                            \
	EMIT_FP_COMPARE_OP(f64_##name, predicate, llvmF64Type, llvmI32Type)                            \
	EMIT_FP_COMPARE_OP(f32x4_##name, predicate, llvmF32x4Type, llvmI32x4Type)                      \
	EMIT_FP_COMPARE_OP(f64x2_##name, predicate, llvmF64x2Type, llvmI64x2Type)

EMIT_FP_COMPARE(eq, llvm::CmpInst::FCMP_OEQ)
EMIT_FP_COMPARE(ne, llvm::CmpInst::FCMP_UNE)
EMIT_FP_COMPARE(lt, llvm::CmpInst::FCMP_OLT)
EMIT_FP_COMPARE(le, llvm::CmpInst::FCMP_OLE)
EMIT_FP_COMPARE(gt, llvm::CmpInst::FCMP_OGT)
EMIT_FP_COMPARE(ge, llvm::CmpInst::FCMP_OGE)

// These operations don't match LLVM's semantics exactly, so just call out to C++ implementations.
EMIT_FP_BINARY_OP(min,
				  emitRuntimeIntrinsic(type == ValueType::f32 ? "f32.min" : "f64.min",
									   FunctionType(TypeTuple(type), TypeTuple{type, type}),
									   {left, right})[0])
EMIT_FP_BINARY_OP(max,
				  emitRuntimeIntrinsic(type == ValueType::f32 ? "f32.max" : "f64.max",
									   FunctionType(TypeTuple(type), TypeTuple{type, type}),
									   {left, right})[0])
EMIT_FP_UNARY_OP(ceil,
				 emitRuntimeIntrinsic(type == ValueType::f32 ? "f32.ceil" : "f64.ceil",
									  FunctionType(TypeTuple(type), TypeTuple{type}),
									  {operand})[0])
EMIT_FP_UNARY_OP(floor,
				 emitRuntimeIntrinsic(type == ValueType::f32 ? "f32.floor" : "f64.floor",
									  FunctionType(TypeTuple(type), TypeTuple{type}),
									  {operand})[0])
EMIT_FP_UNARY_OP(trunc,
				 emitRuntimeIntrinsic(type == ValueType::f32 ? "f32.trunc" : "f64.trunc",
									  FunctionType(TypeTuple(type), TypeTuple{type}),
									  {operand})[0])
EMIT_FP_UNARY_OP(nearest,
				 emitRuntimeIntrinsic(type == ValueType::f32 ? "f32.nearest" : "f64.nearest",
									  FunctionType(TypeTuple(type), TypeTuple{type}),
									  {operand})[0])

EMIT_SIMD_INT_BINARY_OP(add, irBuilder.CreateAdd(left, right))
EMIT_SIMD_INT_BINARY_OP(sub, irBuilder.CreateSub(left, right))

static llvm::Value* emitVectorShiftCountMask(llvm::IRBuilder<>& irBuilder,
											 llvm::Type* scalarType,
											 U32 numLanes,
											 llvm::Value* shiftCount)
{
	// LLVM's shifts have undefined behavior where WebAssembly specifies that the shift count will
	// wrap numbers grather than the bit count of the operands. This matches x86's native shift
	// instructions, but explicitly mask the shift count anyway to support other platforms, and
	// ensure the optimizer doesn't take advantage of the UB.
	const U32 numScalarBits        = scalarType->getPrimitiveSizeInBits();
	llvm::APInt bitsMinusOneInt    = llvm::APInt(numScalarBits, U64(numScalarBits - 1), false);
	llvm::Value* bitsMinusOne      = llvm::ConstantInt::get(scalarType, bitsMinusOneInt);
	llvm::Value* bitsMinusOneSplat = irBuilder.CreateVectorSplat(numLanes, bitsMinusOne);
	return irBuilder.CreateAnd(shiftCount, bitsMinusOneSplat);
}

EMIT_SIMD_INT_BINARY_OP(
	shl,
	irBuilder.CreateShl(left,
						emitVectorShiftCountMask(irBuilder,
												 vectorType->getScalarType(),
												 vectorType->getVectorNumElements(),
												 right)))
EMIT_SIMD_INT_BINARY_OP(
	shr_s,
	irBuilder.CreateAShr(left,
						 emitVectorShiftCountMask(irBuilder,
												  vectorType->getScalarType(),
												  vectorType->getVectorNumElements(),
												  right)))
EMIT_SIMD_INT_BINARY_OP(
	shr_u,
	irBuilder.CreateLShr(left,
						 emitVectorShiftCountMask(irBuilder,
												  vectorType->getScalarType(),
												  vectorType->getVectorNumElements(),
												  right)))

EMIT_SIMD_BINARY_OP(i8x16_mul, llvmI8x16Type, irBuilder.CreateMul(left, right))
EMIT_SIMD_BINARY_OP(i16x8_mul, llvmI16x8Type, irBuilder.CreateMul(left, right))
EMIT_SIMD_BINARY_OP(i32x4_mul, llvmI32x4Type, irBuilder.CreateMul(left, right))

#define EMIT_INT_COMPARE_OP(name, llvmOperandType, llvmDestType, predicate)                        \
	void EmitFunctionContext::name(IR::NoImm)                                                      \
	{                                                                                              \
		auto right = irBuilder.CreateBitCast(pop(), llvmOperandType);                              \
		auto left  = irBuilder.CreateBitCast(pop(), llvmOperandType);                              \
		push(zext(createICmpWithWorkaround(irBuilder, predicate, left, right), llvmDestType));     \
	}

#define EMIT_INT_COMPARE(name, emitCode)                                                           \
	EMIT_INT_COMPARE_OP(i32_##name, llvmI32Type, llvmI32Type, emitCode)                            \
	EMIT_INT_COMPARE_OP(i64_##name, llvmI64Type, llvmI32Type, emitCode)                            \
	EMIT_INT_COMPARE_OP(i8x16_##name, llvmI8x16Type, llvmI8x16Type, emitCode)                      \
	EMIT_INT_COMPARE_OP(i16x8_##name, llvmI16x8Type, llvmI16x8Type, emitCode)                      \
	EMIT_INT_COMPARE_OP(i32x4_##name, llvmI32x4Type, llvmI32x4Type, emitCode)

EMIT_INT_COMPARE(eq, llvm::CmpInst::ICMP_EQ)
EMIT_INT_COMPARE(ne, llvm::CmpInst::ICMP_NE)
EMIT_INT_COMPARE(lt_s, llvm::CmpInst::ICMP_SLT)
EMIT_INT_COMPARE(lt_u, llvm::CmpInst::ICMP_ULT)
EMIT_INT_COMPARE(le_s, llvm::CmpInst::ICMP_SLE)
EMIT_INT_COMPARE(le_u, llvm::CmpInst::ICMP_ULE)
EMIT_INT_COMPARE(gt_s, llvm::CmpInst::ICMP_SGT)
EMIT_INT_COMPARE(gt_u, llvm::CmpInst::ICMP_UGT)
EMIT_INT_COMPARE(ge_s, llvm::CmpInst::ICMP_SGE)
EMIT_INT_COMPARE(ge_u, llvm::CmpInst::ICMP_UGE)

EMIT_SIMD_INT_UNARY_OP(neg, irBuilder.CreateNeg(operand))

static llvm::Value* emitAddUnsignedSaturated(llvm::IRBuilder<>& irBuilder,
											 llvm::Value* left,
											 llvm::Value* right,
											 llvm::Type* type)
{
	left             = irBuilder.CreateBitCast(left, type);
	right            = irBuilder.CreateBitCast(right, type);
	llvm::Value* add = irBuilder.CreateAdd(left, right);
	return irBuilder.CreateSelect(
		irBuilder.CreateICmpUGT(left, add), llvm::Constant::getAllOnesValue(left->getType()), add);
}

static llvm::Value* emitSubUnsignedSaturated(llvm::IRBuilder<>& irBuilder,
											 llvm::Value* left,
											 llvm::Value* right,
											 llvm::Type* type)
{
	left  = irBuilder.CreateBitCast(left, type);
	right = irBuilder.CreateBitCast(right, type);
	return irBuilder.CreateSub(
		irBuilder.CreateSelect(
			createICmpWithWorkaround(irBuilder, llvm::CmpInst::ICMP_UGT, left, right), left, right),
		right);
}

EMIT_SIMD_BINARY_OP(i8x16_add_saturate_s,
					llvmI8x16Type,
					callLLVMIntrinsic({}, llvm::Intrinsic::x86_sse2_padds_b, {left, right}))
EMIT_SIMD_BINARY_OP(i8x16_add_saturate_u,
					llvmI8x16Type,
					emitAddUnsignedSaturated(irBuilder, left, right, llvmI8x16Type))
EMIT_SIMD_BINARY_OP(i8x16_sub_saturate_s,
					llvmI8x16Type,
					callLLVMIntrinsic({}, llvm::Intrinsic::x86_sse2_psubs_b, {left, right}))
EMIT_SIMD_BINARY_OP(i8x16_sub_saturate_u,
					llvmI8x16Type,
					emitSubUnsignedSaturated(irBuilder, left, right, llvmI8x16Type))
EMIT_SIMD_BINARY_OP(i16x8_add_saturate_s,
					llvmI16x8Type,
					callLLVMIntrinsic({}, llvm::Intrinsic::x86_sse2_padds_w, {left, right}))
EMIT_SIMD_BINARY_OP(i16x8_add_saturate_u,
					llvmI16x8Type,
					emitAddUnsignedSaturated(irBuilder, left, right, llvmI16x8Type))
EMIT_SIMD_BINARY_OP(i16x8_sub_saturate_s,
					llvmI16x8Type,
					callLLVMIntrinsic({}, llvm::Intrinsic::x86_sse2_psubs_w, {left, right}))
EMIT_SIMD_BINARY_OP(i16x8_sub_saturate_u,
					llvmI16x8Type,
					emitSubUnsignedSaturated(irBuilder, left, right, llvmI16x8Type))

llvm::Value* EmitFunctionContext::emitBitSelect(llvm::Value* mask,
												llvm::Value* trueValue,
												llvm::Value* falseValue)
{
	return irBuilder.CreateOr(irBuilder.CreateAnd(trueValue, mask),
							  irBuilder.CreateAnd(falseValue, irBuilder.CreateNot(mask)));
}

llvm::Value* EmitFunctionContext::emitVectorSelect(llvm::Value* condition,
												   llvm::Value* trueValue,
												   llvm::Value* falseValue)
{
	llvm::Type* maskType;
	switch(condition->getType()->getVectorNumElements())
	{
	case 2: maskType = llvmI64x2Type; break;
	case 4: maskType = llvmI32x4Type; break;
	case 8: maskType = llvmI16x8Type; break;
	case 16: maskType = llvmI8x16Type; break;
	default:
		Errors::fatalf("unsupported vector length %u",
					   condition->getType()->getVectorNumElements());
	};
	llvm::Value* mask = sext(condition, maskType);

	return irBuilder.CreateBitCast(emitBitSelect(mask,
												 irBuilder.CreateBitCast(trueValue, maskType),
												 irBuilder.CreateBitCast(falseValue, maskType)),
								   trueValue->getType());
}

EMIT_SIMD_FP_BINARY_OP(add, irBuilder.CreateFAdd(left, right))
EMIT_SIMD_FP_BINARY_OP(sub, irBuilder.CreateFSub(left, right))
EMIT_SIMD_FP_BINARY_OP(mul, irBuilder.CreateFMul(left, right))
EMIT_SIMD_FP_BINARY_OP(div, irBuilder.CreateFDiv(left, right))

EMIT_SIMD_BINARY_OP(f32x4_min,
					llvmF32x4Type,
					callLLVMIntrinsic({}, llvm::Intrinsic::x86_sse_min_ps, {left, right}))
EMIT_SIMD_BINARY_OP(f64x2_min,
					llvmF64x2Type,
					callLLVMIntrinsic({}, llvm::Intrinsic::x86_sse2_min_pd, {left, right}))
EMIT_SIMD_BINARY_OP(f32x4_max,
					llvmF32x4Type,
					callLLVMIntrinsic({}, llvm::Intrinsic::x86_sse_max_ps, {left, right}))
EMIT_SIMD_BINARY_OP(f64x2_max,
					llvmF64x2Type,
					callLLVMIntrinsic({}, llvm::Intrinsic::x86_sse2_max_pd, {left, right}))

EMIT_SIMD_FP_UNARY_OP(neg, irBuilder.CreateFNeg(operand))
EMIT_SIMD_FP_UNARY_OP(abs,
					  callLLVMIntrinsic({operand->getType()}, llvm::Intrinsic::fabs, {operand}))
EMIT_SIMD_FP_UNARY_OP(sqrt,
					  callLLVMIntrinsic({operand->getType()}, llvm::Intrinsic::sqrt, {operand}))

static llvm::Value* emitAnyTrue(llvm::IRBuilder<>& irBuilder,
								llvm::Value* vector,
								llvm::Type* vectorType)
{
	vector = irBuilder.CreateBitCast(vector, vectorType);

	const U32 numScalarBits = vectorType->getScalarSizeInBits();
	const Uptr numLanes     = vectorType->getVectorNumElements();
	llvm::Constant* zero
		= llvm::ConstantInt::get(vectorType->getScalarType(), llvm::APInt(numScalarBits, 0));

	llvm::Value* result = nullptr;
	for(Uptr laneIndex = 0; laneIndex < numLanes; ++laneIndex)
	{
		llvm::Value* scalar     = irBuilder.CreateExtractElement(vector, laneIndex);
		llvm::Value* scalarBool = irBuilder.CreateICmpNE(scalar, zero);

		result = result ? irBuilder.CreateOr(result, scalarBool) : scalarBool;
	}
	return irBuilder.CreateZExt(result, llvmI32Type);
}

static llvm::Value* emitAllTrue(llvm::IRBuilder<>& irBuilder,
								llvm::Value* vector,
								llvm::Type* vectorType)
{
	vector = irBuilder.CreateBitCast(vector, vectorType);

	const U32 numScalarBits = vectorType->getScalarSizeInBits();
	const Uptr numLanes     = vectorType->getVectorNumElements();
	llvm::Constant* zero
		= llvm::ConstantInt::get(vectorType->getScalarType(), llvm::APInt(numScalarBits, 0));

	llvm::Value* result = nullptr;
	for(Uptr laneIndex = 0; laneIndex < numLanes; ++laneIndex)
	{
		llvm::Value* scalar     = irBuilder.CreateExtractElement(vector, laneIndex);
		llvm::Value* scalarBool = irBuilder.CreateICmpNE(scalar, zero);

		result = result ? irBuilder.CreateAnd(result, scalarBool) : scalarBool;
	}
	return irBuilder.CreateZExt(result, llvmI32Type);
}

EMIT_SIMD_UNARY_OP(i8x16_any_true, llvmI8x16Type, emitAnyTrue(irBuilder, operand, llvmI8x16Type))
EMIT_SIMD_UNARY_OP(i16x8_any_true, llvmI16x8Type, emitAnyTrue(irBuilder, operand, llvmI16x8Type))
EMIT_SIMD_UNARY_OP(i32x4_any_true, llvmI32x4Type, emitAnyTrue(irBuilder, operand, llvmI32x4Type))
EMIT_SIMD_UNARY_OP(i64x2_any_true, llvmI64x2Type, emitAnyTrue(irBuilder, operand, llvmI64x2Type))

EMIT_SIMD_UNARY_OP(i8x16_all_true, llvmI8x16Type, emitAllTrue(irBuilder, operand, llvmI8x16Type))
EMIT_SIMD_UNARY_OP(i16x8_all_true, llvmI16x8Type, emitAllTrue(irBuilder, operand, llvmI16x8Type))
EMIT_SIMD_UNARY_OP(i32x4_all_true, llvmI32x4Type, emitAllTrue(irBuilder, operand, llvmI32x4Type))
EMIT_SIMD_UNARY_OP(i64x2_all_true, llvmI64x2Type, emitAllTrue(irBuilder, operand, llvmI64x2Type))

void EmitFunctionContext::v128_and(IR::NoImm)
{
	auto right = irBuilder.CreateBitCast(pop(), llvmI128x1Type);
	auto left  = irBuilder.CreateBitCast(pop(), llvmI128x1Type);
	push(irBuilder.CreateAnd(left, right));
}
void EmitFunctionContext::v128_or(IR::NoImm)
{
	auto right = irBuilder.CreateBitCast(pop(), llvmI128x1Type);
	auto left  = irBuilder.CreateBitCast(pop(), llvmI128x1Type);
	push(irBuilder.CreateOr(left, right));
}
void EmitFunctionContext::v128_xor(IR::NoImm)
{
	auto right = irBuilder.CreateBitCast(pop(), llvmI128x1Type);
	auto left  = irBuilder.CreateBitCast(pop(), llvmI128x1Type);
	push(irBuilder.CreateXor(left, right));
}
void EmitFunctionContext::v128_not(IR::NoImm)
{
	auto operand = irBuilder.CreateBitCast(pop(), llvmI128x1Type);
	push(irBuilder.CreateNot(operand));
}

//
// SIMD extract_lane
//

#define EMIT_SIMD_EXTRACT_LANE_OP(name, llvmType, numLanes, coerceScalar)                          \
	void EmitFunctionContext::name(IR::LaneIndexImm<numLanes> imm)                                 \
	{                                                                                              \
		auto operand = irBuilder.CreateBitCast(pop(), llvmType);                                   \
		auto scalar  = irBuilder.CreateExtractElement(operand, imm.laneIndex);                     \
		push(coerceScalar);                                                                        \
	}
EMIT_SIMD_EXTRACT_LANE_OP(i8x16_extract_lane_s, llvmI8x16Type, 16, sext(scalar, llvmI32Type))
EMIT_SIMD_EXTRACT_LANE_OP(i8x16_extract_lane_u, llvmI8x16Type, 16, zext(scalar, llvmI32Type))
EMIT_SIMD_EXTRACT_LANE_OP(i16x8_extract_lane_s, llvmI16x8Type, 8, sext(scalar, llvmI32Type))
EMIT_SIMD_EXTRACT_LANE_OP(i16x8_extract_lane_u, llvmI16x8Type, 8, zext(scalar, llvmI32Type))
EMIT_SIMD_EXTRACT_LANE_OP(i32x4_extract_lane, llvmI32x4Type, 4, scalar)
EMIT_SIMD_EXTRACT_LANE_OP(i64x2_extract_lane, llvmI64x2Type, 2, scalar)

EMIT_SIMD_EXTRACT_LANE_OP(f32x4_extract_lane, llvmF32x4Type, 4, scalar)
EMIT_SIMD_EXTRACT_LANE_OP(f64x2_extract_lane, llvmF64x2Type, 2, scalar)

//
// SIMD replace_lane
//

#define EMIT_SIMD_REPLACE_LANE_OP(typePrefix, llvmType, numLanes, coerceScalar)                    \
	void EmitFunctionContext::typePrefix##_replace_lane(IR::LaneIndexImm<numLanes> imm)            \
	{                                                                                              \
		auto scalar = pop();                                                                       \
		auto vector = irBuilder.CreateBitCast(pop(), llvmType);                                    \
		push(irBuilder.CreateInsertElement(vector, coerceScalar, imm.laneIndex));                  \
	}

EMIT_SIMD_REPLACE_LANE_OP(i8x16, llvmI8x16Type, 16, trunc(scalar, llvmI8Type))
EMIT_SIMD_REPLACE_LANE_OP(i16x8, llvmI16x8Type, 8, trunc(scalar, llvmI16Type))
EMIT_SIMD_REPLACE_LANE_OP(i32x4, llvmI32x4Type, 4, scalar)
EMIT_SIMD_REPLACE_LANE_OP(i64x2, llvmI64x2Type, 2, scalar)

EMIT_SIMD_REPLACE_LANE_OP(f32x4, llvmF32x4Type, 4, scalar)
EMIT_SIMD_REPLACE_LANE_OP(f64x2, llvmF64x2Type, 2, scalar)

void EmitFunctionContext::v8x16_shuffle(IR::ShuffleImm<16> imm)
{
	auto right = irBuilder.CreateBitCast(pop(), llvmI8x16Type);
	auto left  = irBuilder.CreateBitCast(pop(), llvmI8x16Type);
	unsigned int laneIndices[16];
	for(Uptr laneIndex = 0; laneIndex < 16; ++laneIndex)
	{ laneIndices[laneIndex] = imm.laneIndices[laneIndex]; }
	push(irBuilder.CreateShuffleVector(left, right, llvm::ArrayRef<unsigned int>(laneIndices, 16)));
}

void EmitFunctionContext::v128_bitselect(IR::NoImm)
{
	auto mask       = irBuilder.CreateBitCast(pop(), llvmI128x1Type);
	auto falseValue = irBuilder.CreateBitCast(pop(), llvmI128x1Type);
	auto trueValue  = irBuilder.CreateBitCast(pop(), llvmI128x1Type);
	push(emitBitSelect(mask, trueValue, falseValue));
}
