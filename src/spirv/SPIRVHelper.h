/*
 * Author: doe300
 *
 * See the file "LICENSE" for the full license governing this code.
 */

#ifndef SPIRVHELPER_H
#define SPIRVHELPER_H

#ifdef SPIRV_FRONTEND

#include "spirv-tools/libspirv.hpp"
#include "spirv/unified1/OpenCL.std.h"
#include "spirv/unified1/spirv.h"
#include "spirv/unified1/spirv.hpp11"

#include "../Locals.h"

#include <sstream>

namespace vc4c
{
    namespace spirv2qasm
    {
        std::string getOpenCLMethodName(uint32_t instructionID);
        std::string getErrorMessage(spv_result_t error);
        spv_result_t checkCapability(spv::Capability cap);
        Optional<uint32_t> getDecoration(
            const std::vector<std::pair<spv::Decoration, uint32_t>>& decorations, spv::Decoration deco);
        void setParameterDecorations(
            Parameter& param, const std::vector<std::pair<spv::Decoration, uint32_t>>& decorations);
        DataType getIntegerType(uint32_t bitWidth, uint32_t signedness);
        AddressSpace toAddressSpace(spv::StorageClass storageClass);
        void consumeSPIRVMessage(
            spv_message_level_t level, const char* source, const spv_position_t& position, const char* message);

        std::vector<uint32_t> readStreamOfWords(std::istream* in);
        void linkSPIRVModules(const std::vector<std::istream*>& inputModules, std::ostream& output);
    } // namespace spirv2qasm
} // namespace vc4c

#endif
#endif /* SPIRVHELPER_H */
