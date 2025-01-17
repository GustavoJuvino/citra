// Copyright 2017 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the license.txt file included.

#include <exception>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <fmt/format.h>
#include <nihstro/shader_bytecode.h>
#include "common/assert.h"
#include "common/common_types.h"
#include "video_core/renderer_opengl/gl_shader_decompiler.h"

namespace OpenGL::ShaderDecompiler {

using nihstro::Instruction;
using nihstro::OpCode;
using nihstro::RegisterType;
using nihstro::SourceRegister;
using nihstro::SwizzlePattern;

constexpr u32 PROGRAM_END = Pica::Shader::MAX_PROGRAM_CODE_LENGTH;

class DecompileFail : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Describes the behaviour of code path of a given entry point and a return point.
enum class ExitMethod {
    Undetermined, ///< Internal value. Only occur when analyzing JMP loop.
    AlwaysReturn, ///< All code paths reach the return point.
    Conditional,  ///< Code path reaches the return point or an END instruction conditionally.
    AlwaysEnd,    ///< All code paths reach a END instruction.
};

/// A subroutine is a range of code refereced by a CALL, IF or LOOP instruction.
struct Subroutine {
    /// Generates a name suitable for GLSL source code.
    std::string GetName() const {
        return fmt::format("sub_{}_{}", begin, end);
    }

    u32 begin;              ///< Entry point of the subroutine.
    u32 end;                ///< Return point of the subroutine.
    ExitMethod exit_method; ///< Exit method of the subroutine.
    std::set<u32> labels;   ///< Addresses refereced by JMP instructions.

    bool operator<(const Subroutine& rhs) const {
        return std::tie(begin, end) < std::tie(rhs.begin, rhs.end);
    }
};

/// Analyzes shader code and produces a set of subroutines.
class ControlFlowAnalyzer {
public:
    ControlFlowAnalyzer(const Pica::Shader::ProgramCode& program_code, u32 main_offset)
        : program_code(program_code) {

        // Recursively finds all subroutines.
        const Subroutine& program_main = AddSubroutine(main_offset, PROGRAM_END);
        if (program_main.exit_method != ExitMethod::AlwaysEnd)
            throw DecompileFail("Program does not always end");
    }

    std::set<Subroutine> MoveSubroutines() {
        return std::move(subroutines);
    }

private:
    const Pica::Shader::ProgramCode& program_code;
    std::set<Subroutine> subroutines;
    std::map<std::pair<u32, u32>, ExitMethod> exit_method_map;

    /// Adds and analyzes a new subroutine if it is not added yet.
    const Subroutine& AddSubroutine(u32 begin, u32 end) {
        auto iter = subroutines.find(Subroutine{begin, end});
        if (iter != subroutines.end())
            return *iter;

        Subroutine subroutine{begin, end};
        subroutine.exit_method = Scan(begin, end, subroutine.labels);
        if (subroutine.exit_method == ExitMethod::Undetermined)
            throw DecompileFail("Recursive function detected");
        return *subroutines.insert(std::move(subroutine)).first;
    }

    /// Merges exit method of two parallel branches.
    static ExitMethod ParallelExit(ExitMethod a, ExitMethod b) {
        if (a == ExitMethod::Undetermined) {
            return b;
        }
        if (b == ExitMethod::Undetermined) {
            return a;
        }
        if (a == b) {
            return a;
        }
        return ExitMethod::Conditional;
    }

    /// Cascades exit method of two blocks of code.
    static ExitMethod SeriesExit(ExitMethod a, ExitMethod b) {
        // This should be handled before evaluating b.
        DEBUG_ASSERT(a != ExitMethod::AlwaysEnd);

        if (a == ExitMethod::Undetermined) {
            return ExitMethod::Undetermined;
        }

        if (a == ExitMethod::AlwaysReturn) {
            return b;
        }

        if (b == ExitMethod::Undetermined || b == ExitMethod::AlwaysEnd) {
            return ExitMethod::AlwaysEnd;
        }

        return ExitMethod::Conditional;
    }

    /// Scans a range of code for labels and determines the exit method.
    ExitMethod Scan(u32 begin, u32 end, std::set<u32>& labels) {
        auto [iter, inserted] =
            exit_method_map.emplace(std::make_pair(begin, end), ExitMethod::Undetermined);
        ExitMethod& exit_method = iter->second;
        if (!inserted)
            return exit_method;

        for (u32 offset = begin; offset != end && offset != PROGRAM_END; ++offset) {
            const Instruction instr = {program_code[offset]};
            switch (instr.opcode.Value()) {
            case OpCode::Id::END: {
                return exit_method = ExitMethod::AlwaysEnd;
            }
            case OpCode::Id::JMPC:
            case OpCode::Id::JMPU: {
                labels.insert(instr.flow_control.dest_offset);
                ExitMethod no_jmp = Scan(offset + 1, end, labels);
                ExitMethod jmp = Scan(instr.flow_control.dest_offset, end, labels);
                return exit_method = ParallelExit(no_jmp, jmp);
            }
            case OpCode::Id::CALL: {
                auto& call = AddSubroutine(instr.flow_control.dest_offset,
                                           instr.flow_control.dest_offset +
                                               instr.flow_control.num_instructions);
                if (call.exit_method == ExitMethod::AlwaysEnd)
                    return exit_method = ExitMethod::AlwaysEnd;
                ExitMethod after_call = Scan(offset + 1, end, labels);
                return exit_method = SeriesExit(call.exit_method, after_call);
            }
            case OpCode::Id::LOOP: {
                auto& loop = AddSubroutine(offset + 1, instr.flow_control.dest_offset + 1);
                if (loop.exit_method == ExitMethod::AlwaysEnd)
                    return exit_method = ExitMethod::AlwaysEnd;
                ExitMethod after_loop = Scan(instr.flow_control.dest_offset + 1, end, labels);
                return exit_method = SeriesExit(loop.exit_method, after_loop);
            }
            case OpCode::Id::CALLC:
            case OpCode::Id::CALLU: {
                auto& call = AddSubroutine(instr.flow_control.dest_offset,
                                           instr.flow_control.dest_offset +
                                               instr.flow_control.num_instructions);
                ExitMethod after_call = Scan(offset + 1, end, labels);
                return exit_method = SeriesExit(
                           ParallelExit(call.exit_method, ExitMethod::AlwaysReturn), after_call);
            }
            case OpCode::Id::IFU:
            case OpCode::Id::IFC: {
                auto& if_sub = AddSubroutine(offset + 1, instr.flow_control.dest_offset);
                ExitMethod else_method;
                if (instr.flow_control.num_instructions != 0) {
                    auto& else_sub = AddSubroutine(instr.flow_control.dest_offset,
                                                   instr.flow_control.dest_offset +
                                                       instr.flow_control.num_instructions);
                    else_method = else_sub.exit_method;
                } else {
                    else_method = ExitMethod::AlwaysReturn;
                }

                ExitMethod both = ParallelExit(if_sub.exit_method, else_method);
                if (both == ExitMethod::AlwaysEnd)
                    return exit_method = ExitMethod::AlwaysEnd;
                ExitMethod after_call =
                    Scan(instr.flow_control.dest_offset + instr.flow_control.num_instructions, end,
                         labels);
                return exit_method = SeriesExit(both, after_call);
            }
            }
        }
        return exit_method = ExitMethod::AlwaysReturn;
    }
};

class ShaderWriter {
public:
    void AddLine(std::string_view text) {
        DEBUG_ASSERT(scope >= 0);
        if (!text.empty()) {
            shader_source.append(static_cast<std::size_t>(scope) * 4, ' ');
        }
        shader_source += text;
        shader_source += '\n';
    }

    std::string MoveResult() {
        return std::move(shader_source);
    }

    int scope = 0;

private:
    std::string shader_source;
};

/// An adaptor for getting swizzle pattern string from nihstro interfaces.
template <int getter>
std::string GetSelectorSrc(const SwizzlePattern& pattern) {
    std::string out{"."};
    SwizzlePattern::Selector selectors[4];
    if (getter == 1) {
        selectors[0] = pattern.src1_selector_0;
        selectors[1] = pattern.src1_selector_1;
        selectors[2] = pattern.src1_selector_2;
        selectors[3] = pattern.src1_selector_3;
    } else if (getter == 2) {
        selectors[0] = pattern.src2_selector_0;
        selectors[1] = pattern.src2_selector_1;
        selectors[2] = pattern.src2_selector_2;
        selectors[3] = pattern.src2_selector_3;
    } else if (getter == 3) {
        selectors[0] = pattern.src3_selector_0;
        selectors[1] = pattern.src3_selector_1;
        selectors[2] = pattern.src3_selector_2;
        selectors[3] = pattern.src3_selector_3;
    }
    bool clear = true;
    for (int i = 0; i < 4; ++i) {
        out += "xyzw"[static_cast<int>(selectors[i])];
        if (i != static_cast<int>(selectors[i])) {
            clear = false;
        }
    }
    if (clear) {
        out.clear();
    }
    return out;
}

constexpr auto GetSelectorSrc1 = GetSelectorSrc<1>;
constexpr auto GetSelectorSrc2 = GetSelectorSrc<2>;
constexpr auto GetSelectorSrc3 = GetSelectorSrc<3>;

class GLSLGenerator {
public:
    GLSLGenerator(const std::set<Subroutine>& subroutines,
                  const Pica::Shader::ProgramCode& program_code,
                  const Pica::Shader::SwizzleData& swizzle_data, u32 main_offset,
                  const RegGetter& inputreg_getter, const RegGetter& outputreg_getter,
                  u8 sanitize_mul)
        : subroutines(subroutines), program_code(program_code), swizzle_data(swizzle_data),
          main_offset(main_offset), inputreg_getter(inputreg_getter),
          outputreg_getter(outputreg_getter), sanitize_mul(sanitize_mul) {

        Generate();
    }

    std::string MoveShaderCode() {
        return shader.MoveResult();
    }

private:
    /// Gets the Subroutine object corresponding to the specified address.
    const Subroutine& GetSubroutine(u32 begin, u32 end) const {
        auto iter = subroutines.find(Subroutine{begin, end});
        ASSERT(iter != subroutines.end());
        return *iter;
    }

    /// Generates condition evaluation code for the flow control instruction.
    static std::string EvaluateCondition(Instruction::FlowControlType flow_control) {
        using Op = Instruction::FlowControlType::Op;
        switch (flow_control.op) {
        case Op::JustX:
            return flow_control.refx.Value() ? "conditional_code.x" : "!conditional_code.x";
        case Op::JustY:
            return flow_control.refy.Value() ? "conditional_code.y" : "!conditional_code.y";
        case Op::Or:
        case Op::And: {
            std::string and_or = flow_control.op == Op::Or ? "any" : "all";
            std::string bvec;
            if (flow_control.refx.Value() && flow_control.refy.Value()) {
                bvec = "conditional_code";
            } else if (!flow_control.refx.Value() && !flow_control.refy.Value()) {
                bvec = "not(conditional_code)";
            } else {
                std::string result_x{"!conditional_code.x"};
                std::string result_y{"!conditional_code.y"};
                if (flow_control.refx.Value()) {
                    result_x.erase(result_x.begin());
                }
                if (flow_control.refy.Value()) {
                    result_y.erase(result_y.begin());
                }
                bvec = "bvec2(" + result_x + ", " + result_y + ")";
            }
            return and_or + "(" + bvec + ")";
        }
        default:
            UNREACHABLE();
            return "";
        }
    }

    /// Generates code representing a source register.
    std::string GetSourceRegister(const SourceRegister& source_reg,
                                  u32 address_register_index) const {
        u32 index = static_cast<u32>(source_reg.GetIndex());
        std::string index_str = std::to_string(index);

        switch (source_reg.GetRegisterType()) {
        case RegisterType::Input:
            return inputreg_getter(index);
        case RegisterType::Temporary:
            return "reg_tmp" + index_str;
        case RegisterType::FloatUniform:
            if (address_register_index != 0) {
                index_str +=
                    std::string(" + address_registers.") + "xyz"[address_register_index - 1];
            }
            return "uniforms.f[" + index_str + "]";
        default:
            UNREACHABLE();
            return "";
        }
    }

    /// Generates code representing a destination register.
    std::string GetDestRegister(const DestRegister& dest_reg) const {
        u32 index = static_cast<u32>(dest_reg.GetIndex());

        switch (dest_reg.GetRegisterType()) {
        case RegisterType::Output:
            return outputreg_getter(index);
        case RegisterType::Temporary:
            return "reg_tmp" + std::to_string(index);
        default:
            UNREACHABLE();
            return "";
        }
    }

    /// Generates code representing a bool uniform
    std::string GetUniformBool(u32 index) const {
        return "uniforms.b[" + std::to_string(index) + "]";
    }

    /**
     * Adds code that calls a subroutine.
     * @param subroutine the subroutine to call.
     */
    void CallSubroutine(const Subroutine& subroutine) {
        if (subroutine.exit_method == ExitMethod::AlwaysEnd) {
            shader.AddLine(subroutine.GetName() + "();");
            shader.AddLine("return true;");
        } else if (subroutine.exit_method == ExitMethod::Conditional) {
            shader.AddLine("if (" + subroutine.GetName() + "()) { return true; }");
        } else {
            shader.AddLine(subroutine.GetName() + "();");
        }
    }

    /**
     * Writes code that does an assignment operation.
     * @param swizzle the swizzle data of the current instruction.
     * @param reg the destination register code.
     * @param value the code representing the value to assign.
     * @param dest_num_components number of components of the destination register.
     * @param value_num_components number of components of the value to assign.
     */
    void SetDest(const SwizzlePattern& swizzle, const std::string& reg, const std::string& value,
                 u32 dest_num_components, u32 value_num_components) {
        u32 dest_mask_num_components = 0;
        std::string dest_mask_swizzle = ".";

        for (u32 i = 0; i < dest_num_components; ++i) {
            if (swizzle.DestComponentEnabled(static_cast<int>(i))) {
                dest_mask_swizzle += "xyzw"[i];
                ++dest_mask_num_components;
            }
        }

        if (reg.empty() || dest_mask_num_components == 0) {
            return;
        }

        std::string dest = reg;
        if (dest_num_components > 1 && dest_mask_num_components < 4) {
            dest += dest_mask_swizzle;
        }

        std::string src = value;
        if (value_num_components == 1) {
            if (dest_mask_num_components != 1) {
                src = "vec" + std::to_string(dest_mask_num_components) + "(" + value + ")";
            }
        } else if (value_num_components != dest_mask_num_components) {
            src = "(" + value + ")" + dest_mask_swizzle;
        }

        shader.AddLine(dest + " = " + src + ";");
    }

    /**
     * Compiles a single instruction from PICA to GLSL.
     * @param offset the offset of the PICA shader instruction.
     * @return the offset of the next instruction to execute. Usually it is the current offset + 1.
     * If the current instruction is IF or LOOP, the next instruction is after the IF or LOOP block.
     * If the current instruction always terminates the program, returns PROGRAM_END.
     */
    u32 CompileInstr(u32 offset) {
        const Instruction instr = {program_code[offset]};

        std::size_t swizzle_offset =
            instr.opcode.Value().GetInfo().type == OpCode::Type::MultiplyAdd
                ? instr.mad.operand_desc_id
                : instr.common.operand_desc_id;
        const SwizzlePattern swizzle = {swizzle_data[swizzle_offset]};

        shader.AddLine("// " + std::to_string(offset) + ": " + instr.opcode.Value().GetInfo().name);

        switch (instr.opcode.Value().GetInfo().type) {
        case OpCode::Type::Arithmetic: {
            const bool is_inverted =
                (0 != (instr.opcode.Value().GetInfo().subtype & OpCode::Info::SrcInversed));

            std::string src1 = swizzle.negate_src1 ? "-" : "";
            src1 += GetSourceRegister(instr.common.GetSrc1(is_inverted),
                                      !is_inverted * instr.common.address_register_index);
            src1 += GetSelectorSrc1(swizzle);

            std::string src2 = swizzle.negate_src2 ? "-" : "";
            src2 += GetSourceRegister(instr.common.GetSrc2(is_inverted),
                                      is_inverted * instr.common.address_register_index);
            src2 += GetSelectorSrc2(swizzle);

            std::string dest_reg = GetDestRegister(instr.common.dest.Value());

            switch (instr.opcode.Value().EffectiveOpCode()) {
            case OpCode::Id::ADD: {
                SetDest(swizzle, dest_reg, src1 + " + " + src2, 4, 4);
                break;
            }

            case OpCode::Id::MUL: {
                SetDest(swizzle, dest_reg, "mul_safe(" + src1 + ", " + src2 + ")", 4, 4);
                break;
            }

            case OpCode::Id::FLR: {
                SetDest(swizzle, dest_reg, "floor(" + src1 + ")", 4, 4);
                break;
            }

            case OpCode::Id::MAX: {
                SetDest(swizzle, dest_reg, "max(" + src1 + ", " + src2 + ")", 4, 4);
                break;
            }

            case OpCode::Id::MIN: {
                SetDest(swizzle, dest_reg, "min(" + src1 + ", " + src2 + ")", 4, 4);
                break;
            }

            case OpCode::Id::DP3:
            case OpCode::Id::DP4:
            case OpCode::Id::DPH:
            case OpCode::Id::DPHI: {
                OpCode::Id opcode = instr.opcode.Value().EffectiveOpCode();
                std::string dot;
                if (opcode == OpCode::Id::DP3) {
                    if (sanitize_mul) {
                        dot = "dot(vec3(mul_safe(" + src1 + ", " + src2 + ")), vec3(1.0))";
                    } else {
                        dot = "dot(vec3(" + src1 + "), vec3(" + src2 + "))";
                    }
                } else {
                    if (sanitize_mul) {
                        const std::string src1_ =
                            (opcode == OpCode::Id::DPH || opcode == OpCode::Id::DPHI)
                                ? fmt::format("vec4({}.xyz, 1.0)", src1)
                                : std::move(src1);

                        dot = fmt::format("dot(mul_safe({}, {}), vec4(1.0))", src1_, src2);
                    } else {
                        dot = fmt::format("dot({}, {})", src1, src2);
                    }
                }

                SetDest(swizzle, dest_reg, dot, 4, 1);
                break;
            }

            case OpCode::Id::RCP: {
                SetDest(swizzle, dest_reg, "rcp_safe(" + src1 + ".x)", 4, 1);
                break;
            }

            case OpCode::Id::RSQ: {
                SetDest(swizzle, dest_reg, "rsq_safe(" + src1 + ".x)", 4, 1);
                break;
            }

            case OpCode::Id::MOVA: {
                SetDest(swizzle, "address_registers", "ivec2(" + src1 + ")", 2, 2);
                break;
            }

            case OpCode::Id::MOV: {
                SetDest(swizzle, dest_reg, src1, 4, 4);
                break;
            }

            case OpCode::Id::SGE:
            case OpCode::Id::SGEI: {
                SetDest(swizzle, dest_reg, "vec4(greaterThanEqual(" + src1 + "," + src2 + "))", 4,
                        4);
                break;
            }

            case OpCode::Id::SLT:
            case OpCode::Id::SLTI: {
                SetDest(swizzle, dest_reg, "vec4(lessThan(" + src1 + "," + src2 + "))", 4, 4);
                break;
            }

            case OpCode::Id::CMP: {
                using CompareOp = Instruction::Common::CompareOpType::Op;
                const std::map<CompareOp, std::pair<std::string, std::string>> cmp_ops{
                    {CompareOp::Equal, {"==", "equal"}},
                    {CompareOp::NotEqual, {"!=", "notEqual"}},
                    {CompareOp::LessThan, {"<", "lessThan"}},
                    {CompareOp::LessEqual, {"<=", "lessThanEqual"}},
                    {CompareOp::GreaterThan, {">", "greaterThan"}},
                    {CompareOp::GreaterEqual, {">=", "greaterThanEqual"}}};

                const CompareOp op_x = instr.common.compare_op.x.Value();
                const CompareOp op_y = instr.common.compare_op.y.Value();

                if (cmp_ops.find(op_x) == cmp_ops.end()) {
                    LOG_ERROR(HW_GPU, "Unknown compare mode {:x}", static_cast<int>(op_x));
                } else if (cmp_ops.find(op_y) == cmp_ops.end()) {
                    LOG_ERROR(HW_GPU, "Unknown compare mode {:x}", static_cast<int>(op_y));
                } else if (op_x != op_y) {
                    shader.AddLine("conditional_code.x = " + src1 + ".x " +
                                   cmp_ops.find(op_x)->second.first + " " + src2 + ".x;");
                    shader.AddLine("conditional_code.y = " + src1 + ".y " +
                                   cmp_ops.find(op_y)->second.first + " " + src2 + ".y;");
                } else {
                    shader.AddLine("conditional_code = " + cmp_ops.find(op_x)->second.second +
                                   "(vec2(" + src1 + "), vec2(" + src2 + "));");
                }
                break;
            }

            case OpCode::Id::EX2: {
                SetDest(swizzle, dest_reg, "exp2(" + src1 + ".x)", 4, 1);
                break;
            }

            case OpCode::Id::LG2: {
                SetDest(swizzle, dest_reg, "log2(" + src1 + ".x)", 4, 1);
                break;
            }

            default: {
                LOG_ERROR(HW_GPU, "Unhandled arithmetic instruction: 0x{:02x} ({}): 0x{:08x}",
                          (int)instr.opcode.Value().EffectiveOpCode(),
                          instr.opcode.Value().GetInfo().name, instr.hex);
                throw DecompileFail("Unhandled instruction");
                break;
            }
            }

            break;
        }

        case OpCode::Type::MultiplyAdd: {
            if ((instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MAD) ||
                (instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MADI)) {
                bool is_inverted = (instr.opcode.Value().EffectiveOpCode() == OpCode::Id::MADI);

                std::string src1 = swizzle.negate_src1 ? "-" : "";
                src1 += GetSourceRegister(instr.mad.GetSrc1(is_inverted), 0);
                src1 += GetSelectorSrc1(swizzle);

                std::string src2 = swizzle.negate_src2 ? "-" : "";
                src2 += GetSourceRegister(instr.mad.GetSrc2(is_inverted),
                                          !is_inverted * instr.mad.address_register_index);
                src2 += GetSelectorSrc2(swizzle);

                std::string src3 = swizzle.negate_src3 ? "-" : "";
                src3 += GetSourceRegister(instr.mad.GetSrc3(is_inverted),
                                          is_inverted * instr.mad.address_register_index);
                src3 += GetSelectorSrc3(swizzle);

                std::string dest_reg =
                    (instr.mad.dest.Value() < 0x10)
                        ? outputreg_getter(static_cast<u32>(instr.mad.dest.Value().GetIndex()))
                        : (instr.mad.dest.Value() < 0x20)
                              ? "reg_tmp" + std::to_string(instr.mad.dest.Value().GetIndex())
                              : "";
                SetDest(swizzle, dest_reg, fmt::format("fma_safe({}, {}, {})", src1, src2, src3), 4, 4);
            } else {
                LOG_ERROR(HW_GPU, "Unhandled multiply-add instruction: 0x{:02x} ({}): 0x{:08x}",
                          (int)instr.opcode.Value().EffectiveOpCode(),
                          instr.opcode.Value().GetInfo().name, instr.hex);
                throw DecompileFail("Unhandled instruction");
            }
            break;
        }

        default: {
            switch (instr.opcode.Value()) {
            case OpCode::Id::END: {
                shader.AddLine("return true;");
                offset = PROGRAM_END - 1;
                break;
            }

            case OpCode::Id::JMPC:
            case OpCode::Id::JMPU: {
                std::string condition;
                if (instr.opcode.Value() == OpCode::Id::JMPC) {
                    condition = EvaluateCondition(instr.flow_control);
                } else {
                    bool invert_test = instr.flow_control.num_instructions & 1;
                    condition = (invert_test ? "!" : "") +
                                GetUniformBool(instr.flow_control.bool_uniform_id);
                }

                shader.AddLine("if (" + condition + ") {");
                ++shader.scope;
                shader.AddLine("{ jmp_to = " + std::to_string(instr.flow_control.dest_offset) +
                               "u; break; }");

                --shader.scope;
                shader.AddLine("}");
                break;
            }

            case OpCode::Id::CALL:
            case OpCode::Id::CALLC:
            case OpCode::Id::CALLU: {
                std::string condition;
                if (instr.opcode.Value() == OpCode::Id::CALLC) {
                    condition = EvaluateCondition(instr.flow_control);
                } else if (instr.opcode.Value() == OpCode::Id::CALLU) {
                    condition = GetUniformBool(instr.flow_control.bool_uniform_id);
                }

                shader.AddLine(condition.empty() ? "{" : "if (" + condition + ") {");
                ++shader.scope;

                auto& call_sub = GetSubroutine(instr.flow_control.dest_offset,
                                               instr.flow_control.dest_offset +
                                                   instr.flow_control.num_instructions);

                CallSubroutine(call_sub);
                if (instr.opcode.Value() == OpCode::Id::CALL &&
                    call_sub.exit_method == ExitMethod::AlwaysEnd) {
                    offset = PROGRAM_END - 1;
                }

                --shader.scope;
                shader.AddLine("}");
                break;
            }

            case OpCode::Id::NOP: {
                break;
            }

            case OpCode::Id::IFC:
            case OpCode::Id::IFU: {
                std::string condition;
                if (instr.opcode.Value() == OpCode::Id::IFC) {
                    condition = EvaluateCondition(instr.flow_control);
                } else {
                    condition = GetUniformBool(instr.flow_control.bool_uniform_id);
                }

                const u32 if_offset = offset + 1;
                const u32 else_offset = instr.flow_control.dest_offset;
                const u32 endif_offset =
                    instr.flow_control.dest_offset + instr.flow_control.num_instructions;

                shader.AddLine("if (" + condition + ") {");
                ++shader.scope;

                auto& if_sub = GetSubroutine(if_offset, else_offset);
                CallSubroutine(if_sub);
                offset = else_offset - 1;

                if (instr.flow_control.num_instructions != 0) {
                    --shader.scope;
                    shader.AddLine("} else {");
                    ++shader.scope;

                    auto& else_sub = GetSubroutine(else_offset, endif_offset);
                    CallSubroutine(else_sub);
                    offset = endif_offset - 1;

                    if (if_sub.exit_method == ExitMethod::AlwaysEnd &&
                        else_sub.exit_method == ExitMethod::AlwaysEnd) {
                        offset = PROGRAM_END - 1;
                    }
                }

                --shader.scope;
                shader.AddLine("}");
                break;
            }

            case OpCode::Id::LOOP: {
                std::string int_uniform =
                    "uniforms.i[" + std::to_string(instr.flow_control.int_uniform_id) + "]";

                shader.AddLine("address_registers.z = int(" + int_uniform + ".y);");

                std::string loop_var = "loop" + std::to_string(offset);
                shader.AddLine("for (uint " + loop_var + " = 0u; " + loop_var +
                               " <= " + int_uniform + ".x; address_registers.z += int(" +
                               int_uniform + ".z), ++" + loop_var + ") {");
                ++shader.scope;

                auto& loop_sub = GetSubroutine(offset + 1, instr.flow_control.dest_offset + 1);
                CallSubroutine(loop_sub);
                offset = instr.flow_control.dest_offset;

                --shader.scope;
                shader.AddLine("}");

                if (loop_sub.exit_method == ExitMethod::AlwaysEnd) {
                    offset = PROGRAM_END - 1;
                }

                break;
            }

            case OpCode::Id::EMIT:
            case OpCode::Id::SETEMIT:
                LOG_ERROR(HW_GPU, "Geometry shader operation detected in vertex shader");
                break;

            default: {
                LOG_ERROR(HW_GPU, "Unhandled instruction: 0x{:02x} ({}): 0x{:08x}",
                          (int)instr.opcode.Value().EffectiveOpCode(),
                          instr.opcode.Value().GetInfo().name, instr.hex);
                throw DecompileFail("Unhandled instruction");
                break;
            }
            }

            break;
        }
        }
        return offset + 1;
    }

    /**
     * Compiles a range of instructions from PICA to GLSL.
     * @param begin the offset of the starting instruction.
     * @param end the offset where the compilation should stop (exclusive).
     * @return the offset of the next instruction to compile. PROGRAM_END if the program terminates.
     */
    u32 CompileRange(u32 begin, u32 end) {
        u32 program_counter;
        for (program_counter = begin; program_counter < (begin > end ? PROGRAM_END : end);) {
            program_counter = CompileInstr(program_counter);
        }
        return program_counter;
    }

    void Generate() {
        std::string mul_safe{"#define mul_safe(x, y) (x * y)"};
        std::string rcp_safe{"#define rcp_safe(x) (1.0f / x)"};
        std::string rsq_safe{"#define rsq_safe(x) inversesqrt(x)"};
        std::string fma_safe{"#define fma_safe(x, y, z) fma(x, y, z)"};
        if (sanitize_mul == 1) {
            // Use a cheaper sanitize_mul on Android, as mobile GPUs struggle here
            // This seems to be sufficient at least for Ocarina of Time and Attack on Titan accurate
            // multiplication bugs
            mul_safe = "#define mul_safe(x, y) mix(x * y, vec4(0.0), isnan(x * y))";
            fma_safe = "#define fma_safe(x, y, z) (mul_safe(x, y) + z)";
        } else if (sanitize_mul == 2) {
            // On the PICA200, "infinity * 0 = 0" but in OpenGL "infinity * 0 = NaN"
            // infinity = 1.0 / 0.0;
            mul_safe = "#define mul_safe(x, y) mix(x * y, vec4(0.0), isnan(x * y))";
            fma_safe = "#define fma_safe(x, y, z) (mul_safe(x, y) + z)";
            // rcp_safe = "#define rcp_safe(x) mix(1.0f / x, 0.0f, x == 0.0f)";
            // rsq_safe = "#define rsq_safe(x) mix(inversesqrt(x), 0.0f, x == 0.0f)";
            rcp_safe = R"(
float rcp_safe(float x) {
    if (x == 0.0f) return x;
    return 1.0f / x;
}
)";
            rsq_safe = R"(
float rsq_safe(float x) {
    if (x == 0.0f) return x;
    return inversesqrt(x);
}
)";
        }
        shader.AddLine(mul_safe);
        shader.AddLine(rcp_safe);
        shader.AddLine(rsq_safe);
        shader.AddLine(fma_safe);

        // Add declarations for registers
        shader.AddLine("bvec2 conditional_code = bvec2(false);");
        shader.AddLine("ivec3 address_registers = ivec3(0);");
        for (int i = 0; i < 16; ++i) {
            shader.AddLine("vec4 reg_tmp" + std::to_string(i) + " = vec4(0.0, 0.0, 0.0, 1.0);");
        }
        shader.AddLine("");

        // Add declarations for all subroutines
        for (const auto& subroutine : subroutines) {
            shader.AddLine("bool " + subroutine.GetName() + "();");
        }
        shader.AddLine("");

        // Add the main entry point
        shader.AddLine("bool exec_shader() {");
        ++shader.scope;
        CallSubroutine(GetSubroutine(main_offset, PROGRAM_END));
        --shader.scope;
        shader.AddLine("}\n");

        // Add definitions for all subroutines
        for (const auto& subroutine : subroutines) {
            std::set<u32> labels = subroutine.labels;

            shader.AddLine("bool " + subroutine.GetName() + "() {");
            ++shader.scope;

            if (labels.empty()) {
                if (CompileRange(subroutine.begin, subroutine.end) != PROGRAM_END) {
                    shader.AddLine("return false;");
                }
            } else {
                labels.insert(subroutine.begin);
                shader.AddLine("uint jmp_to = " + std::to_string(subroutine.begin) + "u;");
                shader.AddLine("while (true) {");
                ++shader.scope;

                shader.AddLine("switch (jmp_to) {");

                for (auto label : labels) {
                    shader.AddLine("case " + std::to_string(label) + "u: {");
                    ++shader.scope;

                    auto next_it = labels.lower_bound(label + 1);
                    u32 next_label = next_it == labels.end() ? subroutine.end : *next_it;

                    u32 compile_end = CompileRange(label, next_label);
                    if (compile_end > next_label && compile_end != PROGRAM_END) {
                        // This happens only when there is a label inside a IF/LOOP block
                        shader.AddLine("{ jmp_to = " + std::to_string(compile_end) + "u; break; }");
                        labels.emplace(compile_end);
                    }

                    --shader.scope;
                    shader.AddLine("}");
                }

                shader.AddLine("default: return false;");
                shader.AddLine("}");

                --shader.scope;
                shader.AddLine("}");

                shader.AddLine("return false;");
            }

            --shader.scope;
            shader.AddLine("}");

            DEBUG_ASSERT(shader.scope == 0);
        }
    }

private:
    const std::set<Subroutine>& subroutines;
    const Pica::Shader::ProgramCode& program_code;
    const Pica::Shader::SwizzleData& swizzle_data;
    const u32 main_offset;
    const RegGetter& inputreg_getter;
    const RegGetter& outputreg_getter;
    const u8 sanitize_mul;

    ShaderWriter shader;
};

std::string GetCommonDeclarations() {
    return R"(
struct pica_uniforms {
    bool b[16];
    uvec4 i[4];
    vec4 f[96];
};

bool exec_shader();

)";
}

std::optional<ProgramResult> DecompileProgram(const Pica::Shader::ProgramCode& program_code,
                                              const Pica::Shader::SwizzleData& swizzle_data,
                                              u32 main_offset, const RegGetter& inputreg_getter,
                                              const RegGetter& outputreg_getter, u8 sanitize_mul) {

    try {
        auto subroutines = ControlFlowAnalyzer(program_code, main_offset).MoveSubroutines();
        GLSLGenerator generator(subroutines, program_code, swizzle_data, main_offset,
                                inputreg_getter, outputreg_getter, sanitize_mul);
        return {ProgramResult{generator.MoveShaderCode()}};
    } catch (const DecompileFail& exception) {
        LOG_INFO(HW_GPU, "Shader decompilation failed: {}", exception.what());
        return {};
    }
}

} // namespace OpenGL::ShaderDecompiler
