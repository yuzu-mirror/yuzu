// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <string_view>

#include "shader_recompiler/backend/glsl/emit_glsl_instructions.h"
#include "shader_recompiler/backend/glsl/glsl_emit_context.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::GLSL {
namespace {
constexpr char SWIZZLE[]{"xyzw"};

u32 CbufIndex(u32 offset) {
    return (offset / 4) % 4;
}

char OffsetSwizzle(u32 offset) {
    return SWIZZLE[CbufIndex(offset)];
}

bool IsInputArray(Stage stage) {
    return stage == Stage::Geometry || stage == Stage::TessellationControl ||
           stage == Stage::TessellationEval;
}

std::string InputVertexIndex(EmitContext& ctx, std::string_view vertex) {
    return IsInputArray(ctx.stage) ? fmt::format("[{}]", vertex) : "";
}

std::string_view OutputVertexIndex(EmitContext& ctx) {
    return ctx.stage == Stage::TessellationControl ? "[gl_InvocationID]" : "";
}

std::string ChooseCbuf(EmitContext& ctx, const IR::Value& binding, std::string_view index) {
    if (binding.IsImmediate()) {
        return fmt::format("{}_cbuf{}[{}]", ctx.stage_name, binding.U32(), index);
    } else {
        const auto binding_var{ctx.var_alloc.Consume(binding)};
        return fmt::format("GetCbufIndirect({},{})", binding_var, index);
    }
}

void GetCbuf(EmitContext& ctx, std::string_view ret, const IR::Value& binding,
             const IR::Value& offset, u32 num_bits, std::string_view cast = {},
             std::string_view bit_offset = {}) {
    const bool is_immediate{offset.IsImmediate()};
    const bool component_indexing_bug{!is_immediate && ctx.profile.has_gl_component_indexing_bug};
    if (is_immediate) {
        const s32 signed_offset{static_cast<s32>(offset.U32())};
        static constexpr u32 cbuf_size{0x10000};
        if (signed_offset < 0 || offset.U32() > cbuf_size) {
            LOG_WARNING(Shader_GLSL, "Immediate constant buffer offset is out of bounds");
            ctx.Add("{}=0u;", ret);
            return;
        }
    }
    const auto offset_var{ctx.var_alloc.Consume(offset)};
    const auto index{is_immediate ? fmt::format("{}", offset.U32() / 16)
                                  : fmt::format("{}>>4", offset_var)};
    const auto swizzle{is_immediate ? fmt::format(".{}", OffsetSwizzle(offset.U32()))
                                    : fmt::format("[({}>>2)%4]", offset_var)};

    const auto cbuf{ChooseCbuf(ctx, binding, index)};
    const auto cbuf_cast{fmt::format("{}({}{{}})", cast, cbuf)};
    const auto extraction{num_bits == 32 ? cbuf_cast
                                         : fmt::format("bitfieldExtract({},int({}),{})", cbuf_cast,
                                                       bit_offset, num_bits)};
    if (!component_indexing_bug) {
        const auto result{fmt::format(fmt::runtime(extraction), swizzle)};
        ctx.Add("{}={};", ret, result);
        return;
    }
    const auto cbuf_offset{fmt::format("{}>>2", offset_var)};
    for (u32 i = 0; i < 4; ++i) {
        const auto swizzle_string{fmt::format(".{}", "xyzw"[i])};
        const auto result{fmt::format(fmt::runtime(extraction), swizzle_string)};
        ctx.Add("if(({}&3)=={}){}={};", cbuf_offset, i, ret, result);
    }
}

void GetCbuf8(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding, const IR::Value& offset,
              std::string_view cast) {
    const auto ret{ctx.var_alloc.Define(inst, GlslVarType::U32)};
    if (offset.IsImmediate()) {
        const auto bit_offset{fmt::format("{}", (offset.U32() % 4) * 8)};
        GetCbuf(ctx, ret, binding, offset, 8, cast, bit_offset);
    } else {
        const auto offset_var{ctx.var_alloc.Consume(offset)};
        const auto bit_offset{fmt::format("({}%4)*8", offset_var)};
        GetCbuf(ctx, ret, binding, offset, 8, cast, bit_offset);
    }
}

void GetCbuf16(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding, const IR::Value& offset,
               std::string_view cast) {
    const auto ret{ctx.var_alloc.Define(inst, GlslVarType::U32)};
    if (offset.IsImmediate()) {
        const auto bit_offset{fmt::format("{}", ((offset.U32() / 2) % 2) * 16)};
        GetCbuf(ctx, ret, binding, offset, 16, cast, bit_offset);
    } else {
        const auto offset_var{ctx.var_alloc.Consume(offset)};
        const auto bit_offset{fmt::format("(({}>>1)%2)*16", offset_var)};
        GetCbuf(ctx, ret, binding, offset, 16, cast, bit_offset);
    }
}
} // Anonymous namespace

void EmitGetCbufU8(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                   const IR::Value& offset) {
    const auto cast{ctx.profile.has_gl_cbuf_ftou_bug ? "" : "ftou"};
    GetCbuf8(ctx, inst, binding, offset, cast);
}

void EmitGetCbufS8(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                   const IR::Value& offset) {
    const auto cast{ctx.profile.has_gl_cbuf_ftou_bug ? "int" : "ftoi"};
    GetCbuf8(ctx, inst, binding, offset, cast);
}

void EmitGetCbufU16(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                    const IR::Value& offset) {
    const auto cast{ctx.profile.has_gl_cbuf_ftou_bug ? "" : "ftou"};
    GetCbuf16(ctx, inst, binding, offset, cast);
}

void EmitGetCbufS16(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                    const IR::Value& offset) {
    const auto cast{ctx.profile.has_gl_cbuf_ftou_bug ? "int" : "ftoi"};
    GetCbuf16(ctx, inst, binding, offset, cast);
}

void EmitGetCbufU32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                    const IR::Value& offset) {
    const auto ret{ctx.var_alloc.Define(inst, GlslVarType::U32)};
    const auto cast{ctx.profile.has_gl_cbuf_ftou_bug ? "" : "ftou"};
    GetCbuf(ctx, ret, binding, offset, 32, cast);
}

void EmitGetCbufF32(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                    const IR::Value& offset) {
    const auto ret{ctx.var_alloc.Define(inst, GlslVarType::F32)};
    const auto cast{ctx.profile.has_gl_cbuf_ftou_bug ? "utof" : ""};
    GetCbuf(ctx, ret, binding, offset, 32, cast);
}

void EmitGetCbufU32x2(EmitContext& ctx, IR::Inst& inst, const IR::Value& binding,
                      const IR::Value& offset) {
    const auto cast{ctx.profile.has_gl_cbuf_ftou_bug ? "" : "ftou"};
    if (offset.IsImmediate()) {
        const auto cbuf{fmt::format("{}_cbuf{}", ctx.stage_name, binding.U32())};
        static constexpr u32 cbuf_size{0x10000};
        const u32 u32_offset{offset.U32()};
        const s32 signed_offset{static_cast<s32>(offset.U32())};
        if (signed_offset < 0 || u32_offset > cbuf_size) {
            LOG_WARNING(Shader_GLSL, "Immediate constant buffer offset is out of bounds");
            ctx.AddU32x2("{}=uvec2(0u);", inst);
            return;
        }
        if (u32_offset % 2 == 0) {
            ctx.AddU32x2("{}={}({}[{}].{}{});", inst, cast, cbuf, u32_offset / 16,
                         OffsetSwizzle(u32_offset), OffsetSwizzle(u32_offset + 4));
        } else {
            ctx.AddU32x2("{}=uvec2({}({}[{}].{}),{}({}[{}].{}));", inst, cast, cbuf,
                         u32_offset / 16, OffsetSwizzle(u32_offset), cast, cbuf,
                         (u32_offset + 4) / 16, OffsetSwizzle(u32_offset + 4));
        }
        return;
    }
    const auto offset_var{ctx.var_alloc.Consume(offset)};
    const auto cbuf{ChooseCbuf(ctx, binding, fmt::format("{}>>4", offset_var))};
    if (!ctx.profile.has_gl_component_indexing_bug) {
        ctx.AddU32x2("{}=uvec2({}({}[({}>>2)%4]),{}({}[(({}+4)>>2)%4]));", inst, cast, cbuf,
                     offset_var, cast, cbuf, offset_var);
        return;
    }
    const auto ret{ctx.var_alloc.Define(inst, GlslVarType::U32x2)};
    const auto cbuf_offset{fmt::format("{}>>2", offset_var)};
    for (u32 swizzle = 0; swizzle < 4; ++swizzle) {
        ctx.Add("if(({}&3)=={}){}=uvec2({}({}.{}),{}({}.{}));", cbuf_offset, swizzle, ret, cast,
                cbuf, "xyzw"[swizzle], cast, cbuf, "xyzw"[(swizzle + 1) % 4]);
    }
}

void EmitGetAttribute(EmitContext& ctx, IR::Inst& inst, IR::Attribute attr,
                      std::string_view vertex) {
    const u32 element{static_cast<u32>(attr) % 4};
    const char swizzle{"xyzw"[element]};
    if (IR::IsGeneric(attr)) {
        const u32 index{IR::GenericAttributeIndex(attr)};
        if (!ctx.runtime_info.previous_stage_stores.Generic(index, element)) {
            if (element == 3) {
                ctx.AddF32("{}=1.f;", inst, attr);
            } else {
                ctx.AddF32("{}=0.f;", inst, attr);
            }
            return;
        }
        ctx.AddF32("{}=in_attr{}{}.{};", inst, index, InputVertexIndex(ctx, vertex), swizzle);
        return;
    }
    switch (attr) {
    case IR::Attribute::PrimitiveId:
        ctx.AddF32("{}=itof(gl_PrimitiveID);", inst);
        break;
    case IR::Attribute::Layer:
        ctx.AddF32("{}=itof(gl_Layer);", inst);
        break;
    case IR::Attribute::PositionX:
    case IR::Attribute::PositionY:
    case IR::Attribute::PositionZ:
    case IR::Attribute::PositionW: {
        const bool is_array{IsInputArray(ctx.stage)};
        const auto input_decorator{is_array ? fmt::format("gl_in[{}].", vertex) : ""};
        ctx.AddF32("{}={}{}.{};", inst, input_decorator, ctx.position_name, swizzle);
        break;
    }
    case IR::Attribute::PointSpriteS:
    case IR::Attribute::PointSpriteT:
        ctx.AddF32("{}=gl_PointCoord.{};", inst, swizzle);
        break;
    case IR::Attribute::TessellationEvaluationPointU:
    case IR::Attribute::TessellationEvaluationPointV:
        ctx.AddF32("{}=gl_TessCoord.{};", inst, swizzle);
        break;
    case IR::Attribute::InstanceId:
        ctx.AddF32("{}=itof(gl_InstanceID);", inst);
        break;
    case IR::Attribute::VertexId:
        ctx.AddF32("{}=itof(gl_VertexID);", inst);
        break;
    case IR::Attribute::FrontFace:
        ctx.AddF32("{}=itof(gl_FrontFacing?-1:0);", inst);
        break;
    case IR::Attribute::BaseInstance:
        ctx.AddF32("{}=itof(gl_BaseInstance);", inst);
        break;
    case IR::Attribute::BaseVertex:
        ctx.AddF32("{}=itof(gl_BaseVertex);", inst);
        break;
    case IR::Attribute::DrawID:
        ctx.AddF32("{}=itof(gl_DrawID);", inst);
        break;
    default:
        throw NotImplementedException("Get attribute {}", attr);
    }
}

void EmitGetAttributeU32(EmitContext& ctx, IR::Inst& inst, IR::Attribute attr, std::string_view) {
    switch (attr) {
    case IR::Attribute::PrimitiveId:
        ctx.AddU32("{}=uint(gl_PrimitiveID);", inst);
        break;
    case IR::Attribute::InstanceId:
        ctx.AddU32("{}=uint(gl_InstanceID);", inst);
        break;
    case IR::Attribute::VertexId:
        ctx.AddU32("{}=uint(gl_VertexID);", inst);
        break;
    case IR::Attribute::BaseInstance:
        ctx.AddU32("{}=uint(gl_BaseInstance);", inst);
        break;
    case IR::Attribute::BaseVertex:
        ctx.AddU32("{}=uint(gl_BaseVertex);", inst);
        break;
    case IR::Attribute::DrawID:
        ctx.AddU32("{}=uint(gl_DrawID);", inst);
        break;
    default:
        throw NotImplementedException("Get U32 attribute {}", attr);
    }
}

void EmitSetAttribute(EmitContext& ctx, IR::Attribute attr, std::string_view value,
                      [[maybe_unused]] std::string_view vertex) {
    if (IR::IsGeneric(attr)) {
        const u32 index{IR::GenericAttributeIndex(attr)};
        const u32 attr_element{IR::GenericAttributeElement(attr)};
        const GenericElementInfo& info{ctx.output_generics.at(index).at(attr_element)};
        const auto output_decorator{OutputVertexIndex(ctx)};
        if (info.num_components == 1) {
            ctx.Add("{}{}={};", info.name, output_decorator, value);
        } else {
            const u32 index_element{attr_element - info.first_element};
            ctx.Add("{}{}.{}={};", info.name, output_decorator, "xyzw"[index_element], value);
        }
        return;
    }
    const u32 element{static_cast<u32>(attr) % 4};
    const char swizzle{"xyzw"[element]};
    switch (attr) {
    case IR::Attribute::Layer:
        if (ctx.stage != Stage::Geometry &&
            !ctx.profile.support_viewport_index_layer_non_geometry) {
            LOG_WARNING(Shader_GLSL, "Shader stores viewport layer but device does not support "
                                     "viewport layer extension");
            break;
        }
        ctx.Add("gl_Layer=ftoi({});", value);
        break;
    case IR::Attribute::ViewportIndex:
        if (ctx.stage != Stage::Geometry &&
            !ctx.profile.support_viewport_index_layer_non_geometry) {
            LOG_WARNING(Shader_GLSL, "Shader stores viewport index but device does not support "
                                     "viewport layer extension");
            break;
        }
        ctx.Add("gl_ViewportIndex=ftoi({});", value);
        break;
    case IR::Attribute::ViewportMask:
        if (ctx.stage != Stage::Geometry && !ctx.profile.support_viewport_mask) {
            LOG_WARNING(
                Shader_GLSL,
                "Shader stores viewport mask but device does not support viewport mask extension");
            break;
        }
        ctx.Add("gl_ViewportMask[0]=ftoi({});", value);
        break;
    case IR::Attribute::PointSize:
        ctx.Add("gl_PointSize={};", value);
        break;
    case IR::Attribute::PositionX:
    case IR::Attribute::PositionY:
    case IR::Attribute::PositionZ:
    case IR::Attribute::PositionW:
        ctx.Add("gl_Position.{}={};", swizzle, value);
        break;
    case IR::Attribute::ClipDistance0:
    case IR::Attribute::ClipDistance1:
    case IR::Attribute::ClipDistance2:
    case IR::Attribute::ClipDistance3:
    case IR::Attribute::ClipDistance4:
    case IR::Attribute::ClipDistance5:
    case IR::Attribute::ClipDistance6:
    case IR::Attribute::ClipDistance7: {
        const u32 index{static_cast<u32>(attr) - static_cast<u32>(IR::Attribute::ClipDistance0)};
        ctx.Add("gl_ClipDistance[{}]={};", index, value);
        break;
    }
    default:
        throw NotImplementedException("Set attribute {}", attr);
    }
}

void EmitGetAttributeIndexed(EmitContext& ctx, IR::Inst& inst, std::string_view offset,
                             std::string_view vertex) {
    const bool is_array{ctx.stage == Stage::Geometry};
    const auto vertex_arg{is_array ? fmt::format(",{}", vertex) : ""};
    ctx.AddF32("{}=IndexedAttrLoad(int({}){});", inst, offset, vertex_arg);
}

void EmitSetAttributeIndexed([[maybe_unused]] EmitContext& ctx,
                             [[maybe_unused]] std::string_view offset,
                             [[maybe_unused]] std::string_view value,
                             [[maybe_unused]] std::string_view vertex) {
    NotImplemented();
}

void EmitGetPatch(EmitContext& ctx, IR::Inst& inst, IR::Patch patch) {
    if (!IR::IsGeneric(patch)) {
        throw NotImplementedException("Non-generic patch load");
    }
    const u32 index{IR::GenericPatchIndex(patch)};
    const u32 element{IR::GenericPatchElement(patch)};
    const char swizzle{"xyzw"[element]};
    ctx.AddF32("{}=patch{}.{};", inst, index, swizzle);
}

void EmitSetPatch(EmitContext& ctx, IR::Patch patch, std::string_view value) {
    if (IR::IsGeneric(patch)) {
        const u32 index{IR::GenericPatchIndex(patch)};
        const u32 element{IR::GenericPatchElement(patch)};
        ctx.Add("patch{}.{}={};", index, "xyzw"[element], value);
        return;
    }
    switch (patch) {
    case IR::Patch::TessellationLodLeft:
    case IR::Patch::TessellationLodRight:
    case IR::Patch::TessellationLodTop:
    case IR::Patch::TessellationLodBottom: {
        const u32 index{static_cast<u32>(patch) - u32(IR::Patch::TessellationLodLeft)};
        ctx.Add("gl_TessLevelOuter[{}]={};", index, value);
        break;
    }
    case IR::Patch::TessellationLodInteriorU:
        ctx.Add("gl_TessLevelInner[0]={};", value);
        break;
    case IR::Patch::TessellationLodInteriorV:
        ctx.Add("gl_TessLevelInner[1]={};", value);
        break;
    default:
        throw NotImplementedException("Patch {}", patch);
    }
}

void EmitSetFragColor(EmitContext& ctx, u32 index, u32 component, std::string_view value) {
    const char swizzle{"xyzw"[component]};
    ctx.Add("frag_color{}.{}={};", index, swizzle, value);
}

void EmitSetSampleMask(EmitContext& ctx, std::string_view value) {
    ctx.Add("gl_SampleMask[0]=int({});", value);
}

void EmitSetFragDepth(EmitContext& ctx, std::string_view value) {
    ctx.Add("gl_FragDepth={};", value);
}

void EmitLocalInvocationId(EmitContext& ctx, IR::Inst& inst) {
    ctx.AddU32x3("{}=gl_LocalInvocationID;", inst);
}

void EmitWorkgroupId(EmitContext& ctx, IR::Inst& inst) {
    ctx.AddU32x3("{}=gl_WorkGroupID;", inst);
}

void EmitInvocationId(EmitContext& ctx, IR::Inst& inst) {
    ctx.AddU32("{}=uint(gl_InvocationID);", inst);
}

void EmitInvocationInfo(EmitContext& ctx, IR::Inst& inst) {
    switch (ctx.stage) {
    case Stage::TessellationControl:
    case Stage::TessellationEval:
        ctx.AddU32("{}=uint(gl_PatchVerticesIn)<<16;", inst);
        break;
    case Stage::Geometry: {
        u32 vertices_count = 0;
        switch (ctx.runtime_info.input_topology) {
        case InputTopology::Lines:
            vertices_count = 2;
            break;
        case InputTopology::LinesAdjacency:
            vertices_count = 4;
            break;
        case InputTopology::Triangles:
            vertices_count = 3;
            break;
        case InputTopology::TrianglesAdjacency:
            vertices_count = 6;
            break;
        case InputTopology::Points:
        default:
            vertices_count = 1;
            break;
        };

        ctx.AddU32("{}=uint({});", inst, vertices_count << 16);
        break;
    }
    default:
        LOG_WARNING(Shader, "(STUBBED) called");
        ctx.AddU32("{}=uint(0x00ff0000);", inst);
    }
}

void EmitSampleId(EmitContext& ctx, IR::Inst& inst) {
    ctx.AddU32("{}=uint(gl_SampleID);", inst);
}

void EmitIsHelperInvocation(EmitContext& ctx, IR::Inst& inst) {
    ctx.AddU1("{}=gl_HelperInvocation;", inst);
}

void EmitYDirection(EmitContext& ctx, IR::Inst& inst) {
    ctx.uses_y_direction = true;
    ctx.AddF32("{}=gl_FrontMaterial.ambient.a;", inst);
}

void EmitResolutionDownFactor(EmitContext& ctx, IR::Inst& inst) {
    ctx.AddF32("{}=scaling.z;", inst);
}

void EmitRenderArea(EmitContext& ctx, IR::Inst& inst) {
    ctx.AddF32x4("{}=render_area;", inst);
}

void EmitLoadLocal(EmitContext& ctx, IR::Inst& inst, std::string_view word_offset) {
    ctx.AddU32("{}=lmem[{}];", inst, word_offset);
}

void EmitWriteLocal(EmitContext& ctx, std::string_view word_offset, std::string_view value) {
    ctx.Add("lmem[{}]={};", word_offset, value);
}

} // namespace Shader::Backend::GLSL
