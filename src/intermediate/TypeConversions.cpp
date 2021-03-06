/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#include "TypeConversions.h"

#include "Helper.h"

using namespace vc4c;
using namespace vc4c::intermediate;

/*
 * Inserts a bit-cast where the destination element-type is larger than the source element-type, combining multiple
 * elements into a single one.
 *
 * This also means, the source vector has more elements (of smaller type-size) than the destination vector
 */
static InstructionWalker insertCombiningBitcast(
    InstructionWalker it, Method& method, const Value& src, const Value& dest)
{
    // the number of source elements to combine in a single destination element
    auto sizeFactor = dest.type.getScalarBitCount() / src.type.getScalarBitCount();
    // the number of bits to shift per element
    auto shift = src.type.getScalarBitCount();

    /*
     * By shifting and ANDing whole source vector, we save a few instructions for sources with more than 1 element
     *
     * E.g. short4 -> int2 can be written as
     * (short4 & 0xFFFF) << 0 -> int2 (lower half-words in elements 0 and 2)
     * (short4 & 0xFFFF) << 16 -> int2 (upper half-words in element 1 and 3)
     * -> we only need 2 shifts and 2 ANDs instead of 4 (per element)
     */

    const Value truncatedSource = method.addNewLocal(src.type, "%bit_cast");
    it.emplace(new Operation(OP_AND, truncatedSource, src, Value(Literal(src.type.getScalarWidthMask()), TYPE_INT32)));
    it.nextInBlock();

    std::vector<Value> shiftedTruncatedVectors;
    shiftedTruncatedVectors.reserve(sizeFactor);
    for(auto i = 0; i < sizeFactor; ++i)
    {
        shiftedTruncatedVectors.emplace_back(
            method.addNewLocal(dest.type.toVectorType(src.type.getVectorWidth()), "%bit_cast"));
        const Value& result = shiftedTruncatedVectors.back();
        it.emplace(new Operation(
            OP_SHL, result, truncatedSource, Value(Literal(static_cast<unsigned>(shift * i)), TYPE_INT8)));
        it.nextInBlock();
    }

    /*
     * The up to 8 destination elements are now distributed across the shiftedTruncatedVectors (stvs) as follows:
     *
     * Size-factor of 2:
     * stv0[0] | stv1[1], stv0[2] | stv1[3], stv0[4] | stv1[5], ...
     *
     * Size-factor of 4;
     * stv0[0] | stv1[1] | stv2[2] | stv3[3], stv0[4] | stv1[5] | stv2[6] | stv3[7], ...
     *
     * To simplify the assembly of the destination, we rotate the vectors, so their element-numbers align
     */

    std::vector<Value> rotatedVectors;
    rotatedVectors.reserve(shiftedTruncatedVectors.size());
    for(unsigned i = 0; i < shiftedTruncatedVectors.size(); ++i)
    {
        if(i == 0)
            // no need to rotate
            rotatedVectors.emplace_back(shiftedTruncatedVectors.front());
        else
        {
            rotatedVectors.emplace_back(
                method.addNewLocal(dest.type.toVectorType(src.type.getVectorWidth()), "%bit_cast"));
            const Value& result = rotatedVectors.back();
            it = insertVectorRotation(
                it, shiftedTruncatedVectors[i], Value(Literal(i), TYPE_INT8), result, Direction::DOWN);
        }
    }

    /*
     * The up to 8 destination elements are now distributed across the rotatedVectors (rvs) as follows:
     *
     * Size-factor of 2:
     * rv0[0] | rv1[0], rv0[2] | rv1[2], rv0[4] | rv1[4], ...
     *
     * Size-factor of 4;
     * rv0[0] | rv1[0] | rv2[0] | rv3[0], rv0[4] | rv1[4] | rv2[4] | rv3[4], ...
     *
     * In the next step, we OR the separate vectors to a single one
     */
    Value combinedVector = INT_ZERO;
    for(const Value& rv : rotatedVectors)
    {
        Value newCombinedVector = method.addNewLocal(dest.type.toVectorType(src.type.getVectorWidth()), "%bit_cast");
        it.emplace(new Operation(OP_OR, newCombinedVector, combinedVector, rv));
        it.nextInBlock();
        combinedVector = newCombinedVector;
    }

    /*
     * Now, we have the destination elements as follows:
     *
     * Size-factor of 2:
     * cv[0], cv[2], cv[4], cv[6], ...
     *
     * Size-factor of 4;
     * cv[0], cv[4], cv[8], cv[12], ...
     *
     * Finally, we rotate the single elements to fit their position in the destination
     */

    Value destination = method.addNewLocal(dest.type, "%bit_cast");
    // initialize destination with zero so register-allocation finds unconditional assignment
    it.emplace(new MoveOperation(destination, INT_ZERO));
    it.nextInBlock();

    for(unsigned i = 0; i < dest.type.getVectorWidth(); ++i)
    {
        unsigned sourceIndex = i * sizeFactor;

        const Value tmp = method.addNewLocal(dest.type, "%bit_cast");
        // the vector-rotation to element 0 and then to the destination element should be combined by optimization-step
        // #combineVectorRotations
        it = insertVectorExtraction(it, method, combinedVector, Value(Literal(sourceIndex), TYPE_INT8), tmp);
        it = insertVectorInsertion(it, method, destination, Value(Literal(i), TYPE_INT8), tmp);
    }

    it.emplace(new MoveOperation(dest, destination));
    return it;
}

/*
 * Inserts a bit-cast where the destination element-type is smaller than the source element-type, splitting a single
 * element into several ones.
 *
 * This also means, the source vector has less elements (of larger type-size) than the destination vector
 */
static InstructionWalker insertSplittingBitcast(
    InstructionWalker it, Method& method, const Value& src, const Value& dest)
{
    // the number of destination elements to extract from a single source element
    auto sizeFactor = src.type.getScalarBitCount() / dest.type.getScalarBitCount();
    // the number of bits to shift per element
    auto shift = dest.type.getScalarBitCount();

    /*
     * By shifting and ANDing whole source vector, we save a few instructions for sources with more than 1 element
     *
     * E.g. int2 -> short4 can be written as
     * (int2 >> 0) & 0xFFFF -> short4 (lower half-words)
     * (int2 >> 16) & 0xFFFF -> short4 (upper half-words)
     * -> we only need 2 shifts and 2 ANDs instead of 4 (per element)
     */
    std::vector<Value> shiftedTruncatedVectors;
    shiftedTruncatedVectors.reserve(sizeFactor);
    for(auto i = 0; i < sizeFactor; ++i)
    {
        shiftedTruncatedVectors.emplace_back(method.addNewLocal(dest.type, "%bit_cast"));
        const Value& result = shiftedTruncatedVectors.back();
        const Value tmp = method.addNewLocal(dest.type, "%bit_cast");
        it.emplace(new Operation(OP_SHR, tmp, src, Value(Literal(static_cast<unsigned>(shift * i)), TYPE_INT8)));
        it.nextInBlock();
        it.emplace(new Operation(OP_AND, result, tmp, Value(Literal(dest.type.getScalarWidthMask()), TYPE_INT32)));
        it.nextInBlock();
    }

    /*
     * The up to 16 destination elements are now distributed across the shiftedTruncatedVectors (stvs) as follows:
     *
     * Size-factor of 2:
     * stv0[0], stv1[0], stv0[1], stv1[1], stv0[2], ...
     *
     * Size-factor of 4;
     * stv0[0], stv1[0], stv2[0], stv3[0], stv0[1], ...
     *
     * So we need to assemble the destination vector from these vectors
     */

    const Value destination = method.addNewLocal(dest.type, "%bit_cast");
    // initialize destination with zero so register-allocation finds unconditional assignment
    it.emplace(new MoveOperation(destination, INT_ZERO));
    it.nextInBlock();

    for(unsigned i = 0; i < dest.type.getVectorWidth(); ++i)
    {
        const Value& stv = shiftedTruncatedVectors.at(i % shiftedTruncatedVectors.size());
        unsigned sourceElement = static_cast<unsigned>(i / shiftedTruncatedVectors.size());

        const Value tmp = method.addNewLocal(dest.type, "%bit_cast");
        // the vector-rotation to element 0 and then to the destination element should be combined by optimization-step
        // #combineVectorRotations
        it = insertVectorExtraction(it, method, stv, Value(Literal(sourceElement), TYPE_INT8), tmp);
        it = insertVectorInsertion(it, method, destination, Value(Literal(i), TYPE_INT8), tmp);
    }

    it.emplace(new MoveOperation(dest, destination));
    return it;
}

InstructionWalker intermediate::insertBitcast(
    InstructionWalker it, Method& method, const Value& src, const Value& dest, const InstructionDecorations deco)
{
    /*
     * TODO room for optimization:
     * to extract e.g. a single char from a char4, LLVM generates something like:
     *
     * int tmp = bitcast char4 in to int
     * char out = convert tmp to char
     *
     * For example in test_vector.cl kernel test_vector_load.
     *
     * => If we can detect the bit-cast to be used only to extract an element (of the original type before casting), we
     * can skip it?!
     *
     */
    if(src.isUndefined())
        it.emplace(new intermediate::MoveOperation(dest, UNDEFINED_VALUE));
    else if(src.isZeroInitializer())
        it.emplace(new intermediate::MoveOperation(dest, INT_ZERO));
    else if(src.type.getVectorWidth() > dest.type.getVectorWidth())
        it = insertCombiningBitcast(it, method, src, dest);
    else if(src.type.getVectorWidth() < dest.type.getVectorWidth())
        it = insertSplittingBitcast(it, method, src, dest);
    else
        // bit-casts with types of same vector-size (and therefore same element-size) are simple moves
        it.emplace((new intermediate::MoveOperation(dest, src))->addDecorations(deco));

    // last step: map destination to source (if bit-cast of pointers)
    if(dest.hasType(ValueType::LOCAL) && src.hasType(ValueType::LOCAL) && dest.type.isPointerType() &&
        src.type.isPointerType())
        // this helps recognizing lifetime-starts of bit-cast stack-allocations
        const_cast<std::pair<Local*, int>&>(dest.local->reference) = std::make_pair(src.local, 0);
    it->addDecorations(deco);
    it.nextInBlock();
    return it;
}

InstructionWalker intermediate::insertZeroExtension(InstructionWalker it, Method& method, const Value& src,
    const Value& dest, bool allowLiteral, const ConditionCode conditional, const SetFlag setFlags)
{
    if(src.type.getScalarBitCount() == 32 && dest.type.getScalarBitCount() <= 32)
    {
        //"extend" to smaller type
        it.emplace(new MoveOperation(dest, src, conditional, setFlags));
        switch(dest.type.getScalarBitCount())
        {
        case 8:
            it->setPackMode(PACK_INT_TO_CHAR_TRUNCATE);
            break;
        case 16:
            it->setPackMode(PACK_INT_TO_SHORT_TRUNCATE);
            break;
        case 32:
            // no pack mode
            break;
        default:
            throw CompilationError(
                CompilationStep::GENERAL, "Invalid type-width for zero-extension", dest.type.to_string());
        }
    }
    else if(dest.type.getScalarBitCount() >= 32 && src.type.getScalarBitCount() >= 32)
    {
        // do nothing, is just a move, since we truncate the 64-bit integers anyway
        it.emplace(new MoveOperation(dest, src, conditional, setFlags));
    }
    else if(dest.type.getScalarBitCount() == 32 && src.hasType(ValueType::REGISTER) &&
        (has_flag(src.reg.file, RegisterFile::PHYSICAL_A) || has_flag(src.reg.file, RegisterFile::ACCUMULATOR)) &&
        src.type.getScalarBitCount() == 8)
    {
        // if we zero-extend from register-file A, use unpack-modes
        // this is applied e.g. for unpacking parameters in code-generation, since the source is UNIFORM
        it.emplace(new MoveOperation(dest, src, conditional, setFlags));
        it->setUnpackMode(UNPACK_CHAR_TO_INT_ZEXT);
    }
    else if(allowLiteral)
    {
        it.emplace(new Operation(
            OP_AND, dest, src, Value(Literal(src.type.getScalarWidthMask()), TYPE_INT32), conditional, setFlags));
    }
    else
    {
        const Value tmp = method.addNewLocal(TYPE_INT32, "%zext");
        it.emplace(new LoadImmediate(tmp, Literal(src.type.getScalarWidthMask())));
        it.nextInBlock();
        it.emplace(new Operation(OP_AND, dest, src, tmp, conditional, setFlags));
    }

    it->addDecorations(InstructionDecorations::UNSIGNED_RESULT);
    it.nextInBlock();
    return it;
}

InstructionWalker intermediate::insertSignExtension(InstructionWalker it, Method& method, const Value& src,
    const Value& dest, bool allowLiteral, const ConditionCode conditional, const SetFlag setFlags)
{
    if(dest.type.getScalarBitCount() >= 32 && src.type.getScalarBitCount() >= 32)
    {
        // do nothing, is just a move, since we truncate the 64-bit integers anyway
        it.emplace(new MoveOperation(dest, src, conditional, setFlags));
    }
    else if(dest.type.getScalarBitCount() == 32 && src.hasType(ValueType::REGISTER) &&
        (has_flag(src.reg.file, RegisterFile::PHYSICAL_A) || has_flag(src.reg.file, RegisterFile::ACCUMULATOR)) &&
        src.type.getScalarBitCount() == 16)
    {
        // if we sign-extend from register-file A, use unpack-modes
        // this is applied e.g. for unpacking parameters in code-generation, since the source is UNIFORM
        it.emplace(new MoveOperation(dest, src, conditional, setFlags));
        it->setUnpackMode(UNPACK_SHORT_TO_INT_SEXT);
    }
    else
    {
        // out = asr(shl(in, bit_diff) bit_diff)
        Value widthDiff(
            Literal(static_cast<int32_t>(dest.type.getScalarBitCount() - src.type.getScalarBitCount())), TYPE_INT8);

        if(!allowLiteral)
        {
            Value tmp = method.addNewLocal(TYPE_INT8, "%sext");
            it.emplace(new LoadImmediate(tmp, widthDiff.literal));
            it.nextInBlock();

            widthDiff = tmp;
        }

        const Value tmp = method.addNewLocal(TYPE_INT32, "%sext");
        it.emplace(new Operation(OP_SHL, tmp, src, widthDiff, conditional));
        it.nextInBlock();
        it.emplace(new Operation(OP_ASR, dest, tmp, widthDiff, conditional, setFlags));
    }

    it.nextInBlock();
    return it;
}

InstructionWalker intermediate::insertSaturation(
    InstructionWalker it, Method& method, const Value& src, const Value& dest, bool isSigned)
{
    // saturation = clamping to min/max of type
    //-> dest = max(min(src, destType.max), destType.min)
    //-> or via pack-modes

    if(!dest.type.isSimpleType() || dest.type.isFloatingType())
        throw CompilationError(CompilationStep::GENERAL, "Invalid target type for saturation", dest.type.to_string());

    if(src.getLiteralValue())
    {
        switch(dest.type.getScalarBitCount())
        {
        case 8:
            return it.emplace((new MoveOperation(dest,
                                   Value(Literal(isSigned ? saturate<int8_t>(src.getLiteralValue()->signedInt()) :
                                                            saturate<uint8_t>(src.getLiteralValue()->unsignedInt())),
                                       dest.type)))
                                  ->addDecorations(isSigned ? InstructionDecorations::NONE :
                                                              InstructionDecorations::UNSIGNED_RESULT));
        case 16:
            return it.emplace((new MoveOperation(dest,
                                   Value(Literal(isSigned ? saturate<int16_t>(src.getLiteralValue()->signedInt()) :
                                                            saturate<uint16_t>(src.getLiteralValue()->unsignedInt())),
                                       dest.type)))
                                  ->addDecorations(isSigned ? InstructionDecorations::NONE :
                                                              InstructionDecorations::UNSIGNED_RESULT));
        case 32:
            return it.emplace((new MoveOperation(dest,
                                   Value(Literal(isSigned ? saturate<int32_t>(src.getLiteralValue()->signedInt()) :
                                                            saturate<uint32_t>(src.getLiteralValue()->unsignedInt())),
                                       dest.type)))
                                  ->addDecorations(isSigned ? InstructionDecorations::NONE :
                                                              InstructionDecorations::UNSIGNED_RESULT));
        default:
            throw CompilationError(
                CompilationStep::GENERAL, "Invalid target type for saturation", dest.type.to_string());
        }
    }
    else // saturation can be easily done via pack-modes
    {
        if(dest.type.getScalarBitCount() == 8 && !isSigned)
            return it.emplace((new MoveOperation(dest, src))
                                  ->setPackMode(PACK_INT_TO_UNSIGNED_CHAR_SATURATE)
                                  ->addDecorations(InstructionDecorations::UNSIGNED_RESULT));
        else if(dest.type.getScalarBitCount() == 16 && isSigned)
            return it.emplace((new MoveOperation(dest, src))->setPackMode(PACK_INT_TO_SIGNED_SHORT_SATURATE));
        else if(dest.type.getScalarBitCount() == 32)
            return it.emplace((new MoveOperation(dest, src))->setPackMode(PACK_32_32));
        // TODO need to saturate manually
        throw CompilationError(
            CompilationStep::GENERAL, "Saturation to this type is not yet supported", dest.type.to_string());
    }
}

InstructionWalker intermediate::insertTruncate(
    InstructionWalker it, Method& method, const Value& src, const Value& dest)
{
    if(dest.type.getScalarBitCount() >= src.type.getScalarBitCount())
        //"truncate" to larger type, simply move
        it.emplace(new MoveOperation(dest, src));
    else
        it.emplace(new Operation(OP_AND, dest, src, Value(Literal(dest.type.getScalarWidthMask()), TYPE_INT32)));

    return it.nextInBlock();
}

InstructionWalker intermediate::insertFloatingPointConversion(
    InstructionWalker it, Method& method, const Value& src, const Value& dest)
{
    if(src.type.getScalarBitCount() == dest.type.getScalarBitCount())
        it.emplace(new MoveOperation(dest, src));
    else if(src.type.getScalarBitCount() == 16 && dest.type.getScalarBitCount() == 32)
        it.emplace((new Operation(OP_FMUL, dest, src, OpCode::getRightIdentity(OP_FMUL).value()))
                       ->setUnpackMode(UNPACK_HALF_TO_FLOAT));
    else if(src.type.getScalarBitCount() == 32 && dest.type.getScalarBitCount() == 16)
        it.emplace((new intermediate::Operation(OP_FMUL, dest, src, OpCode::getRightIdentity(OP_FMUL).value()))
                       ->setPackMode(PACK_FLOAT_TO_HALF_TRUNCATE));
    else
        throw CompilationError(CompilationStep::GENERAL, "Unsupported floating-point conversion");
    return it.nextInBlock();
}
