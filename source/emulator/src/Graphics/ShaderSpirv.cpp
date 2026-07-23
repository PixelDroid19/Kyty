#include "Emulator/Graphics/ShaderSpirv.h"

#include "Kyty/Core/ArrayWrapper.h"
#include "Kyty/Core/Common.h"
#include "Kyty/Core/DbgAssert.h"
#include "Kyty/Core/Hashmap.h"
#include "Kyty/Core/MagicEnum.h"
#include "Kyty/Core/String8.h"
#include "Kyty/Core/Vector.h"

#include "Emulator/Config.h"
#include "Emulator/Graphics/Shader.h"

#include <cstring>

#ifdef KYTY_EMU_ENABLED

#define KYTY_RECOMPILER_ARGS                                                                                                               \
	[[maybe_unused]] uint32_t index, [[maybe_unused]] const ShaderCode &code, [[maybe_unused]] String8 *dst_source,                        \
	    [[maybe_unused]] Spirv *spirv, [[maybe_unused]] const char *const *param, [[maybe_unused]] SccCheck scc_check
#define KYTY_RECOMPILER_FUNC(f) static bool f(KYTY_RECOMPILER_ARGS)

namespace Kyty::Core {

KYTY_HASH_DEFINE_CALC(Kyty::Libs::Graphics::ShaderInstructionTypeFormat)
{
	return hash32(static_cast<uint32_t>(key->type)) ^ hash64(static_cast<uint64_t>(key->format));
}

KYTY_HASH_DEFINE_EQUALS(Kyty::Libs::Graphics::ShaderInstructionTypeFormat)
{
	return key_a->type == key_b->type && key_a->format == key_b->format;
}
} // namespace Kyty::Core

namespace Kyty::Libs::Graphics {

constexpr char FUNC_FETCH_4[] = R"(
       ; Function fetch_f1_f1_f1_f1_vf4_
       ; void fetch(out float p1, out float p2, out float p3, out float p4, in vec4 attr)
       ; {
       ; p1 = attr.x;
       ; p2 = attr.y;
       ; p3 = attr.z;
       ; p4 = attr.w;
       ; }
%fetch_f1_f1_f1_f1_vf4_ = OpFunction %void None %function_fetch4
 %fetch_p1 = OpFunctionParameter %_ptr_Function_float
 %fetch_p2 = OpFunctionParameter %_ptr_Function_float
 %fetch_p3 = OpFunctionParameter %_ptr_Function_float
 %fetch_p4 = OpFunctionParameter %_ptr_Function_float
%fetch_attr = OpFunctionParameter %_ptr_Function_v4float
%fetch_label = OpLabel
 %fetch_20 = OpAccessChain %_ptr_Function_float %fetch_attr %uint_0
 %fetch_21 = OpLoad %float %fetch_20
             OpStore %fetch_p1 %fetch_21
 %fetch_23 = OpAccessChain %_ptr_Function_float %fetch_attr %uint_1
 %fetch_24 = OpLoad %float %fetch_23
             OpStore %fetch_p2 %fetch_24
 %fetch_26 = OpAccessChain %_ptr_Function_float %fetch_attr %uint_2
 %fetch_27 = OpLoad %float %fetch_26
             OpStore %fetch_p3 %fetch_27
 %fetch_29 = OpAccessChain %_ptr_Function_float %fetch_attr %uint_3
 %fetch_30 = OpLoad %float %fetch_29
             OpStore %fetch_p4 %fetch_30
             OpReturn
             OpFunctionEnd
)";

constexpr char FUNC_FETCH_3[] = R"(
       ; Function fetch_f1_f1_f1_vf3_
       ; void fetch(out float p1, out float p2, out float p3, in vec3 attr)
       ; {
       ; p1 = attr.x;
       ; p2 = attr.y;
       ; p3 = attr.z;
       ; }
%fetch_f1_f1_f1_vf3_ = OpFunction %void None %function_fetch3
       %fetch3_p1_0 = OpFunctionParameter %_ptr_Function_float
       %fetch3_p2_0 = OpFunctionParameter %_ptr_Function_float
       %fetch3_p3_0 = OpFunctionParameter %_ptr_Function_float
     %fetch3_attr_0 = OpFunctionParameter %_ptr_Function_v3float
         %fetch3_26 = OpLabel
         %fetch3_53 = OpAccessChain %_ptr_Function_float %fetch3_attr_0 %uint_0
         %fetch3_54 = OpLoad %float %fetch3_53
               OpStore %fetch3_p1_0 %fetch3_54
         %fetch3_55 = OpAccessChain %_ptr_Function_float %fetch3_attr_0 %uint_1
         %fetch3_56 = OpLoad %float %fetch3_55
               OpStore %fetch3_p2_0 %fetch3_56
         %fetch3_57 = OpAccessChain %_ptr_Function_float %fetch3_attr_0 %uint_2
         %fetch3_58 = OpLoad %float %fetch3_57
               OpStore %fetch3_p3_0 %fetch3_58
               OpReturn
               OpFunctionEnd
)";

constexpr char FUNC_FETCH_2[] = R"(
       ; Function fetch_f1_f1_vf2_
       ; void fetch(out float p1, out float p2, in vec2 attr)
       ; {
       ; p1 = attr.x;
       ; p2 = attr.y;
       ; }
%fetch_f1_f1_vf2_ = OpFunction %void None %function_fetch2
       %fetch2_p1_1 = OpFunctionParameter %_ptr_Function_float
       %fetch2_p2_1 = OpFunctionParameter %_ptr_Function_float
     %fetch2_attr_1 = OpFunctionParameter %_ptr_Function_v2float
         %fetch2_34 = OpLabel
         %fetch2_59 = OpAccessChain %_ptr_Function_float %fetch2_attr_1 %uint_0
         %fetch2_60 = OpLoad %float %fetch2_59
               OpStore %fetch2_p1_1 %fetch2_60
         %fetch2_61 = OpAccessChain %_ptr_Function_float %fetch2_attr_1 %uint_1
         %fetch2_62 = OpLoad %float %fetch2_61
               OpStore %fetch2_p2_1 %fetch2_62
               OpReturn
               OpFunctionEnd
)";

constexpr char FUNC_FETCH_1[] = R"(
       ; Function fetch_f1_f1_
       ; void fetch(out float p1, in float attr)
       ; {
       ; p1 = attr;
       ; }
%fetch_f1_f1_ = OpFunction %void None %function_fetch1
       %fetch1_p1_2 = OpFunctionParameter %_ptr_Function_float
     %fetch1_attr_2 = OpFunctionParameter %_ptr_Function_float
         %fetch1_39 = OpLabel
         %fetch1_63 = OpLoad %float %fetch1_attr_2
               OpStore %fetch1_p1_2 %fetch1_63
               OpReturn
               OpFunctionEnd
)";

constexpr char FUNC_ABS_DIFF[] = R"(
                    ; uint abs_diff(uint u1, uint u2)
                    ; {
                    ; 	return max(u1,u2)-min(u1,u2);
                    ; }
%abs_diff = OpFunction %uint None %function_u_u
         %abs_diff_18 = OpFunctionParameter %uint
         %abs_diff_19 = OpFunctionParameter %uint
         %abs_diff_21 = OpLabel
         %abs_diff_50 = OpExtInst %uint %GLSL_std_450 UMax %abs_diff_18 %abs_diff_19
         %abs_diff_53 = OpExtInst %uint %GLSL_std_450 UMin %abs_diff_18 %abs_diff_19
         %abs_diff_54 = OpISub %uint %abs_diff_50 %abs_diff_53
               OpReturnValue %abs_diff_54
               OpFunctionEnd
)";

constexpr char FUNC_WQM[] = R"(
                    ; uint w(uint u, uint s, uint m)
                    ; {
                    ; 	return ((u >> s) & 0xF) != 0 ? m : 0;
                    ; }
         %wqm = OpFunction %uint None %function_u_u_u
         %wqm_155 = OpFunctionParameter %uint
         %wqm_156 = OpFunctionParameter %uint
         %wqm_161 = OpFunctionParameter %uint
         %wqm_50 = OpLabel
        %wqm_157 = OpShiftRightLogical %uint %wqm_155 %wqm_156
        %wqm_159 = OpBitwiseAnd %uint %wqm_157 %uint_15
        %wqm_160 = OpINotEqual %bool %wqm_159 %uint_0
        %wqm_162 = OpSelect %uint %wqm_160 %wqm_161 %uint_0
               OpReturnValue %wqm_162
               OpFunctionEnd
)";

constexpr char FUNC_ADDC[] = R"(
                  ; uvec2 addc(uint a, uint b, uint c)
                  ; {
                  ; 	uint cc = 0;
                  ; 	uint sum = uaddCarry(a, b, cc) + c;
                  ; 	return uvec2(sum, (cc != 0 || (c !=0 && sum == 0)) ? 1u : 0u);
                  ; }
         %addc = OpFunction %v2uint None %function_u2_u_u_u
         %addc_47 = OpFunctionParameter %uint
         %addc_48 = OpFunctionParameter %uint
         %addc_49 = OpFunctionParameter %uint
         %addc_51 = OpLabel
        %addc_156 = OpIAddCarry %ResTypeU %addc_47 %addc_48
        %addc_157 = OpCompositeExtract %uint %addc_156 1
        %addc_158 = OpCompositeExtract %uint %addc_156 0
        %addc_160 = OpIAdd %uint %addc_158 %addc_49
        %addc_163 = OpINotEqual %bool %addc_157 %uint_0
        %addc_164 = OpLogicalNot %bool %addc_163
               OpSelectionMerge %addc_166 None
               OpBranchConditional %addc_164 %addc_165 %addc_166
        %addc_165 = OpLabel
        %addc_168 = OpINotEqual %bool %addc_49 %uint_0
        %addc_170 = OpIEqual %bool %addc_160 %uint_0
        %addc_171 = OpLogicalAnd %bool %addc_168 %addc_170
               OpBranch %addc_166
        %addc_166 = OpLabel
        %addc_172 = OpPhi %bool %addc_163 %addc_51 %addc_171 %addc_165
        %addc_173 = OpSelect %uint %addc_172 %uint_1 %uint_0
        %addc_174 = OpCompositeConstruct %v2uint %addc_160 %addc_173
               OpReturnValue %addc_174
               OpFunctionEnd
)";

constexpr char FUNC_LSHL_ADD[] = R"(
                  ; uvec2 lshl_add(uint a, uint b, uint n)
                  ; {
                  ; 	uint cc = 0;
                  ; 	uint sum = uaddCarry(a << n, b, cc);
                  ; 	return uvec2(sum, ((a >> (32-n)) !=0) ? 1u : cc);
                  ; }
        %lshl_add = OpFunction %v2uint None %function_u2_u_u_u
         %ladd_25 = OpFunctionParameter %uint
         %ladd_26 = OpFunctionParameter %uint
         %ladd_27 = OpFunctionParameter %uint
         %ladd_29 = OpLabel
        %ladd_124 = OpShiftLeftLogical %uint %ladd_25 %ladd_27
        %ladd_127 = OpIAddCarry %ResTypeU %ladd_124 %ladd_26
        %ladd_128 = OpCompositeExtract %uint %ladd_127 1
        %ladd_129 = OpCompositeExtract %uint %ladd_127 0
        %ladd_133 = OpISub %uint %uint_32 %ladd_27
        %ladd_134 = OpShiftRightLogical %uint %ladd_25 %ladd_133
        %ladd_135 = OpINotEqual %bool %ladd_134 %uint_0
        %ladd_138 = OpSelect %uint %ladd_135 %uint_1 %ladd_128
        %ladd_139 = OpCompositeConstruct %v2uint %ladd_129 %ladd_138
               OpReturnValue %ladd_139
               OpFunctionEnd
)";

constexpr char FUNC_MIPMAP[] = R"(
                  ; uvec2 mipmap(uint lod, uint width, uint height)
                  ; {
                  ; 	uint mip_width  = width;
                  ; 	uint mip_height = height;
                  ; 	uint mip_x      = 0;
                  ; 	uint mip_y      = 0;
                  ; 	for (uint i = 0; i < 16; i++)
                  ; 	{
                  ; 		if (i == lod)
                  ; 		{
                  ; 			return uvec2(mip_x, mip_y);
                  ; 		}
                  ; 		bool odd = ((i & 1u) != 0u);
                  ; 		mip_x += (odd ? mip_width : 0u);
                  ; 		mip_y += (odd ? 0u : mip_height);
                  ; 		mip_width >>= (mip_width > 1u ? 1u : 0u);
                  ; 		mip_height >>= (mip_height > 1u ? 1u : 0u);
                  ; 	}
                  ; 	return uvec2(mip_x, mip_y);
                  ; }
         %mipmap = OpFunction %v2uint None %function_u2_u_u_u
         %mipmap_33 = OpFunctionParameter %uint
         %mipmap_16 = OpFunctionParameter %uint
         %mipmap_18 = OpFunctionParameter %uint
         %mipmap_14 = OpLabel
               OpSelectionMerge %mipmap_188 None
               OpSwitch %uint_0 %mipmap_191
        %mipmap_191 = OpLabel
               OpBranch %mipmap_23
         %mipmap_23 = OpLabel
        %mipmap_296 = OpPhi %uint %uint_0 %mipmap_191 %mipmap_56 %mipmap_26
        %mipmap_295 = OpPhi %uint %mipmap_18 %mipmap_191 %mipmap_66 %mipmap_26
        %mipmap_294 = OpPhi %uint %uint_0 %mipmap_191 %mipmap_51 %mipmap_26
        %mipmap_293 = OpPhi %uint %mipmap_16 %mipmap_191 %mipmap_61 %mipmap_26
        %mipmap_292 = OpPhi %uint %uint_0 %mipmap_191 %mipmap_70 %mipmap_26
               OpLoopMerge %mipmap_25 %mipmap_26 None
               OpBranch %mipmap_27
         %mipmap_27 = OpLabel
         %mipmap_31 = OpULessThan %bool %mipmap_292 %uint_16
               OpBranchConditional %mipmap_31 %mipmap_24 %mipmap_25
         %mipmap_24 = OpLabel
         %mipmap_34 = OpIEqual %bool %mipmap_292 %mipmap_33
               OpSelectionMerge %mipmap_36 None
               OpBranchConditional %mipmap_34 %mipmap_35 %mipmap_36
         %mipmap_35 = OpLabel
         %mipmap_39 = OpCompositeConstruct %v2uint %mipmap_294 %mipmap_296
               OpBranch %mipmap_25
         %mipmap_36 = OpLabel
         %mipmap_45 = OpBitwiseAnd %uint %mipmap_292 %uint_1
         %mipmap_46 = OpINotEqual %bool %mipmap_45 %uint_0
         %mipmap_49 = OpSelect %uint %mipmap_46 %mipmap_293 %uint_0
         %mipmap_51 = OpIAdd %uint %mipmap_294 %mipmap_49
         %mipmap_54 = OpSelect %uint %mipmap_46 %uint_0 %mipmap_295
         %mipmap_56 = OpIAdd %uint %mipmap_296 %mipmap_54
         %mipmap_58 = OpUGreaterThan %bool %mipmap_293 %uint_1
         %mipmap_59 = OpSelect %uint %mipmap_58 %uint_1 %uint_0
         %mipmap_61 = OpShiftRightLogical %uint %mipmap_293 %mipmap_59
         %mipmap_63 = OpUGreaterThan %bool %mipmap_295 %uint_1
         %mipmap_64 = OpSelect %uint %mipmap_63 %uint_1 %uint_0
         %mipmap_66 = OpShiftRightLogical %uint %mipmap_295 %mipmap_64
               OpBranch %mipmap_26
         %mipmap_26 = OpLabel
         %mipmap_70 = OpIAdd %uint %mipmap_292 %int_1
               OpBranch %mipmap_23
         %mipmap_25 = OpLabel
        %mipmap_302 = OpPhi %v2uint %undef_v2uint %mipmap_27 %mipmap_39 %mipmap_35
        %mipmap_297 = OpPhi %bool %false %mipmap_27 %true %mipmap_35
               OpSelectionMerge %mipmap_195 None
               OpBranchConditional %mipmap_297 %mipmap_188 %mipmap_195
        %mipmap_195 = OpLabel
         %mipmap_73 = OpCompositeConstruct %v2uint %mipmap_294 %mipmap_296
               OpBranch %mipmap_188
        %mipmap_188 = OpLabel
        %mipmap_301 = OpPhi %v2uint %mipmap_302 %mipmap_25 %mipmap_73 %mipmap_195
               OpReturnValue %mipmap_301
               OpFunctionEnd
)";

constexpr char FUNC_ORDERED[] = R"(
                  ; bool unordered(float f1, float f2)
                  ; {
                  ; 	return (isnan(f1) || isnan(f2));
                  ; }
                  ; bool ordered(float f1, float f2)
                  ; {
                  ; 	return !unordered(f1, f2);
                  ; }
  %unordered = OpFunction %bool None %function_b_f_f
         %ord_49 = OpFunctionParameter %float
         %ord_50 = OpFunctionParameter %float
         %ord_52 = OpLabel
        %ord_156 = OpIsNan %bool %ord_49
        %ord_157 = OpLogicalNot %bool %ord_156
               OpSelectionMerge %ord_159 None
               OpBranchConditional %ord_157 %ord_158 %ord_159
        %ord_158 = OpLabel
        %ord_161 = OpIsNan %bool %ord_50
               OpBranch %ord_159
        %ord_159 = OpLabel
        %ord_162 = OpPhi %bool %ord_156 %ord_52 %ord_161 %ord_158
               OpReturnValue %ord_162
               OpFunctionEnd
    %ordered = OpFunction %bool None %function_b_f_f
         %ord_53 = OpFunctionParameter %float
         %ord_54 = OpFunctionParameter %float
         %ord_56 = OpLabel
        %ord_169 = OpFunctionCall %bool %unordered %ord_53 %ord_54
        %ord_170 = OpLogicalNot %bool %ord_169
               OpReturnValue %ord_170
               OpFunctionEnd
)";

constexpr char FUNC_MUL_EXTENDED[] = R"(
               ; uint mul_lo_uint(uint u1, uint u2)
               ; {
               ; 	uint r1, r2;
               ; 	umulExtended(u1, u2, r1, r2);
               ; 	return r2;
               ; }
               ; uint mul_hi_uint(uint u1, uint u2)
               ; {
               ; 	uint r1, r2;
               ; 	umulExtended(u1, u2, r1, r2);
               ; 	return r1;
               ; }
               ; int mul_lo_int(int i1, int i2)
               ; {
               ; 	int r1, r2;
               ; 	imulExtended(i1, i2, r1, r2);
               ; 	return r2;
               ; }
               ; int mul_hi_int(int i1, int i2)
               ; {
               ; 	int r1, r2;
               ; 	imulExtended(i1, i2, r1, r2);
               ; 	return r1;
               ; }
         %mul_lo_uint = OpFunction %uint None %function_u_u
         %22 = OpFunctionParameter %uint
         %23 = OpFunctionParameter %uint
         %25 = OpLabel
         %79 = OpUMulExtended %ResTypeU %22 %23
         %80 = OpCompositeExtract %uint %79 0
               OpReturnValue %80
               OpFunctionEnd
         %mul_hi_uint = OpFunction %uint None %function_u_u
         %26 = OpFunctionParameter %uint
         %27 = OpFunctionParameter %uint
         %29 = OpLabel
         %89 = OpUMulExtended %ResTypeU %26 %27
         %91 = OpCompositeExtract %uint %89 1
               OpReturnValue %91
               OpFunctionEnd
         %mul_lo_int = OpFunction %int None %function_i_i
         %31 = OpFunctionParameter %int
         %32 = OpFunctionParameter %int
         %34 = OpLabel
        %100 = OpSMulExtended %ResTypeI %31 %32
        %101 = OpCompositeExtract %int %100 0
               OpReturnValue %101
               OpFunctionEnd
         %mul_hi_int = OpFunction %int None %function_i_i
         %35 = OpFunctionParameter %int
         %36 = OpFunctionParameter %int
         %38 = OpLabel
        %110 = OpSMulExtended %ResTypeI %35 %36
        %112 = OpCompositeExtract %int %110 1
               OpReturnValue %112
               OpFunctionEnd
)";

constexpr char FUNC_SHIFT_RIGHT[] = R"(
                    ; void shift_r(out uint d0, out uint d1, in uint s0, in uint s1, in uint n)
                    ; {
                    ; 	d0 = n < 32 ? (s0 >> n) | (n != 0 ? (s1 << (32 - n)) : 0) : (n < 64 ? s1 >> (n - 32) : 0) ;
                    ; 	d1 = n < 32 ? s1 >> n : 0;
                    ; }
%shift_right = OpFunction %void None %function_shift
          %shr_9 = OpFunctionParameter %_ptr_Function_uint
         %shr_10 = OpFunctionParameter %_ptr_Function_uint
         %shr_11 = OpFunctionParameter %_ptr_Function_uint
         %shr_12 = OpFunctionParameter %_ptr_Function_uint
         %shr_13 = OpFunctionParameter %_ptr_Function_uint
         %shr_15 = OpLabel
         %shr_27 = OpVariable %_ptr_Function_uint Function
         %shr_36 = OpVariable %_ptr_Function_uint Function
         %shr_50 = OpVariable %_ptr_Function_uint Function
         %shr_62 = OpVariable %_ptr_Function_uint Function
         %shr_23 = OpLoad %uint %shr_13
         %shr_26 = OpULessThan %bool %shr_23 %uint_32
               OpSelectionMerge %shr_29 None
               OpBranchConditional %shr_26 %shr_28 %shr_46
         %shr_28 = OpLabel
         %shr_30 = OpLoad %uint %shr_11
         %shr_31 = OpLoad %uint %shr_13
         %shr_32 = OpShiftRightLogical %uint %shr_30 %shr_31
         %shr_33 = OpLoad %uint %shr_13
         %shr_35 = OpINotEqual %bool %shr_33 %uint_0
               OpSelectionMerge %shr_38 None
               OpBranchConditional %shr_35 %shr_37 %shr_43
         %shr_37 = OpLabel
         %shr_39 = OpLoad %uint %shr_12
         %shr_40 = OpLoad %uint %shr_13
         %shr_41 = OpISub %uint %uint_32 %shr_40
         %shr_42 = OpShiftLeftLogical %uint %shr_39 %shr_41
               OpStore %shr_36 %shr_42
               OpBranch %shr_38
         %shr_43 = OpLabel
               OpStore %shr_36 %uint_0
               OpBranch %shr_38
         %shr_38 = OpLabel
        %shr_331 = OpPhi %uint %shr_42 %shr_37 %uint_0 %shr_43
         %shr_45 = OpBitwiseOr %uint %shr_32 %shr_331
               OpStore %shr_27 %shr_45
               OpBranch %shr_29
         %shr_46 = OpLabel
         %shr_47 = OpLoad %uint %shr_13
         %shr_49 = OpULessThan %bool %shr_47 %uint_64
               OpSelectionMerge %shr_52 None
               OpBranchConditional %shr_49 %shr_51 %shr_57
         %shr_51 = OpLabel
         %shr_53 = OpLoad %uint %shr_12
         %shr_54 = OpLoad %uint %shr_13
         %shr_55 = OpISub %uint %shr_54 %uint_32
         %shr_56 = OpShiftRightLogical %uint %shr_53 %shr_55
               OpStore %shr_50 %shr_56
               OpBranch %shr_52
         %shr_57 = OpLabel
               OpStore %shr_50 %uint_0
               OpBranch %shr_52
         %shr_52 = OpLabel
        %shr_330 = OpPhi %uint %shr_56 %shr_51 %uint_0 %shr_57
               OpStore %shr_27 %shr_330
               OpBranch %shr_29
         %shr_29 = OpLabel
        %shr_332 = OpPhi %uint %shr_45 %shr_38 %shr_330 %shr_52
               OpStore %shr_9 %shr_332
         %shr_60 = OpLoad %uint %shr_13
         %shr_61 = OpULessThan %bool %shr_60 %uint_32
               OpSelectionMerge %shr_64 None
               OpBranchConditional %shr_61 %shr_63 %shr_68
         %shr_63 = OpLabel
         %shr_65 = OpLoad %uint %shr_12
         %shr_66 = OpLoad %uint %shr_13
         %shr_67 = OpShiftRightLogical %uint %shr_65 %shr_66
               OpStore %shr_62 %shr_67
               OpBranch %shr_64
         %shr_68 = OpLabel
               OpStore %shr_62 %uint_0
               OpBranch %shr_64
         %shr_64 = OpLabel
        %shr_333 = OpPhi %uint %shr_67 %shr_63 %uint_0 %shr_68
               OpStore %shr_10 %shr_333
               OpReturn
               OpFunctionEnd
)";

constexpr char FUNC_SHIFT_LEFT[] = R"(
                    ; void shift_l(out uint d0, out uint d1, in uint s0, in uint s1, in uint n)
                    ; {
                    ; 	d0 = n < 32 ? s0 << n : 0;
                    ; 	d1 = n < 32 ? (n!=0 ? s0 >> (32-n) : 0) | (s1 << n) : (n < 64 ? s0 << (n-32) : 0);
                    ; }
%shift_left = OpFunction %void None %function_shift
         %shl_16 = OpFunctionParameter %_ptr_Function_uint
         %shl_17 = OpFunctionParameter %_ptr_Function_uint
         %shl_18 = OpFunctionParameter %_ptr_Function_uint
         %shl_19 = OpFunctionParameter %_ptr_Function_uint
         %shl_20 = OpFunctionParameter %_ptr_Function_uint
         %shl_22 = OpLabel
         %shl_72 = OpVariable %_ptr_Function_uint Function
         %shl_82 = OpVariable %_ptr_Function_uint Function
         %shl_87 = OpVariable %_ptr_Function_uint Function
        %shl_103 = OpVariable %_ptr_Function_uint Function
         %shl_70 = OpLoad %uint %shl_20
         %shl_71 = OpULessThan %bool %shl_70 %uint_32
               OpSelectionMerge %shl_74 None
               OpBranchConditional %shl_71 %shl_73 %shl_78
         %shl_73 = OpLabel
         %shl_75 = OpLoad %uint %shl_18
         %shl_76 = OpLoad %uint %shl_20
         %shl_77 = OpShiftLeftLogical %uint %shl_75 %shl_76
               OpStore %shl_72 %shl_77
               OpBranch %shl_74
         %shl_78 = OpLabel
               OpStore %shl_72 %uint_0
               OpBranch %shl_74
         %shl_74 = OpLabel
        %shl_334 = OpPhi %uint %shl_77 %shl_73 %uint_0 %shl_78
               OpStore %shl_16 %shl_334
         %shl_80 = OpLoad %uint %shl_20
         %shl_81 = OpULessThan %bool %shl_80 %uint_32
               OpSelectionMerge %shl_84 None
               OpBranchConditional %shl_81 %shl_83 %shl_100
         %shl_83 = OpLabel
         %shl_85 = OpLoad %uint %shl_20
         %shl_86 = OpINotEqual %bool %shl_85 %uint_0
               OpSelectionMerge %shl_89 None
               OpBranchConditional %shl_86 %shl_88 %shl_94
         %shl_88 = OpLabel
         %shl_90 = OpLoad %uint %shl_18
         %shl_91 = OpLoad %uint %shl_20
         %shl_92 = OpISub %uint %uint_32 %shl_91
         %shl_93 = OpShiftRightLogical %uint %shl_90 %shl_92
               OpStore %shl_87 %shl_93
               OpBranch %shl_89
         %shl_94 = OpLabel
               OpStore %shl_87 %uint_0
               OpBranch %shl_89
         %shl_89 = OpLabel
        %shl_336 = OpPhi %uint %shl_93 %shl_88 %uint_0 %shl_94
         %shl_96 = OpLoad %uint %shl_19
         %shl_97 = OpLoad %uint %shl_20
         %shl_98 = OpShiftLeftLogical %uint %shl_96 %shl_97
         %shl_99 = OpBitwiseOr %uint %shl_336 %shl_98
               OpStore %shl_82 %shl_99
               OpBranch %shl_84
        %shl_100 = OpLabel
        %shl_101 = OpLoad %uint %shl_20
        %shl_102 = OpULessThan %bool %shl_101 %uint_64
               OpSelectionMerge %shl_105 None
               OpBranchConditional %shl_102 %shl_104 %shl_110
        %shl_104 = OpLabel
        %shl_106 = OpLoad %uint %shl_18
        %shl_107 = OpLoad %uint %shl_20
        %shl_108 = OpISub %uint %shl_107 %uint_32
        %shl_109 = OpShiftLeftLogical %uint %shl_106 %shl_108
               OpStore %shl_103 %shl_109
               OpBranch %shl_105
        %shl_110 = OpLabel
               OpStore %shl_103 %uint_0
               OpBranch %shl_105
        %shl_105 = OpLabel
        %shl_335 = OpPhi %uint %shl_109 %shl_104 %uint_0 %shl_110
               OpStore %shl_82 %shl_335
               OpBranch %shl_84
         %shl_84 = OpLabel
        %shl_337 = OpPhi %uint %shl_99 %shl_89 %shl_335 %shl_105
               OpStore %shl_17 %shl_337
               OpReturn
               OpFunctionEnd
)";

constexpr char BUFFER_LOAD_UBYTE[] = R"(
             ; void buffer_load_ubyte(out uint p1, in int index, in int offset, in int stride, in int buffer_index)
             ; {
             ; 	int byte_addr = offset + index * stride;
             ; 	int word_addr = byte_addr / 4;
             ; 	uint byte_shift = uint(byte_addr & 3) * 8;
             ; 	uint word = floatBitsToUint(buf[buffer_index].data[word_addr]);
             ; 	p1 = (word >> byte_shift) & 0xff;
             ; }
%buffer_load_ubyte = OpFunction %void None %function_buffer_load_store_ubyte
         %buf_l_ub_11 = OpFunctionParameter %_ptr_Function_uint
         %buf_l_ub_12 = OpFunctionParameter %_ptr_Function_int
         %buf_l_ub_13 = OpFunctionParameter %_ptr_Function_int
         %buf_l_ub_14 = OpFunctionParameter %_ptr_Function_int
         %buf_l_ub_15 = OpFunctionParameter %_ptr_Function_int
         %buf_l_ub_17 = OpLabel
         %buf_l_ub_43 = OpLoad %int %buf_l_ub_12
         %buf_l_ub_44 = OpLoad %int %buf_l_ub_13
         %buf_l_ub_45 = OpLoad %int %buf_l_ub_14
         %buf_l_ub_46 = OpIMul %int %buf_l_ub_43 %buf_l_ub_45
         %buf_l_ub_47 = OpIAdd %int %buf_l_ub_44 %buf_l_ub_46
         %buf_l_ub_49 = OpSDiv %int %buf_l_ub_47 %int_4
         %buf_l_ub_50 = OpBitcast %uint %buf_l_ub_47
         %buf_l_ub_51 = OpBitwiseAnd %uint %buf_l_ub_50 %uint_3
         %buf_l_ub_52 = OpShiftLeftLogical %uint %buf_l_ub_51 %uint_3
         %buf_l_ub_57 = OpLoad %int %buf_l_ub_15
         %buf_l_ub_62 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_l_ub_57 %int_0 %buf_l_ub_49
         %buf_l_ub_63 = OpLoad %float %buf_l_ub_62
         %buf_l_ub_64 = OpBitcast %uint %buf_l_ub_63
         %buf_l_ub_65 = OpShiftRightLogical %uint %buf_l_ub_64 %buf_l_ub_52
         %buf_l_ub_66 = OpBitwiseAnd %uint %buf_l_ub_65 %uint_255
               OpStore %buf_l_ub_11 %buf_l_ub_66
               OpReturn
               OpFunctionEnd
)";

constexpr char BUFFER_LOAD_FLOAT1[] = R"(
             ; void buffer_load_float1(out float p1, in int index, in int offset, in int stride, in int buffer_index)
             ; {
             ; 	int addr = (offset + index * stride)/4;
             ; 	p1 = buf[buffer_index].data[addr+0];
             ; }
%buffer_load_float1 = OpFunction %void None %function_buffer_load_store_float1
         %buf_l_f1_11 = OpFunctionParameter %_ptr_Function_float
         %buf_l_f1_12 = OpFunctionParameter %_ptr_Function_int
         %buf_l_f1_13 = OpFunctionParameter %_ptr_Function_int
         %buf_l_f1_14 = OpFunctionParameter %_ptr_Function_int
         %buf_l_f1_15 = OpFunctionParameter %_ptr_Function_int
         %buf_l_f1_17 = OpLabel
         %buf_l_f1_42 = OpVariable %_ptr_Function_int Function
         %buf_l_f1_43 = OpLoad %int %buf_l_f1_13
         %buf_l_f1_44 = OpLoad %int %buf_l_f1_12
         %buf_l_f1_45 = OpLoad %int %buf_l_f1_14
         %buf_l_f1_46 = OpIMul %int %buf_l_f1_44 %buf_l_f1_45
         %buf_l_f1_47 = OpIAdd %int %buf_l_f1_43 %buf_l_f1_46
         %buf_l_f1_49 = OpSDiv %int %buf_l_f1_47 %int_4
               OpStore %buf_l_f1_42 %buf_l_f1_49
         %buf_l_f1_57 = OpLoad %int %buf_l_f1_15
         %buf_l_f1_62 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_l_f1_57 %int_0 %buf_l_f1_49
         %buf_l_f1_63 = OpLoad %float %buf_l_f1_62
               OpStore %buf_l_f1_11 %buf_l_f1_63
               OpReturn
               OpFunctionEnd
)";

constexpr char BUFFER_LOAD_FLOAT4[] = R"(
             ; Function buffer_load_float4
             ;void buffer_load_float4(out float p1, out float p2, out float p3, out float p4, in int index,
             ;                                in int offset, in int stride, in int buffer_index)
             ;{
             ;	int addr = (offset + index * stride)/4;
             ;	p1 = buf[buffer_index].data[addr+0];
             ;	p2 = buf[buffer_index].data[addr+1];
             ;	p3 = buf[buffer_index].data[addr+2];
             ;	p4 = buf[buffer_index].data[addr+3];
             ;}
%buffer_load_float4 = OpFunction %void None %function_buffer_load_store_float4
  %buf_l_f4_21 = OpFunctionParameter %_ptr_Function_float
  %buf_l_f4_22 = OpFunctionParameter %_ptr_Function_float
  %buf_l_f4_23 = OpFunctionParameter %_ptr_Function_float
  %buf_l_f4_24 = OpFunctionParameter %_ptr_Function_float
  %buf_l_f4_25 = OpFunctionParameter %_ptr_Function_int
  %buf_l_f4_26 = OpFunctionParameter %_ptr_Function_int
  %buf_l_f4_27 = OpFunctionParameter %_ptr_Function_int
  %buf_l_f4_28 = OpFunctionParameter %_ptr_Function_int
  %buf_l_f4_30 = OpLabel
  %buf_l_f4_44 = OpVariable %_ptr_Function_int Function
  %buf_l_f4_45 = OpLoad %int %buf_l_f4_26
  %buf_l_f4_46 = OpLoad %int %buf_l_f4_25
  %buf_l_f4_47 = OpLoad %int %buf_l_f4_27
  %buf_l_f4_48 = OpIMul %int %buf_l_f4_46 %buf_l_f4_47
  %buf_l_f4_49 = OpIAdd %int %buf_l_f4_45 %buf_l_f4_48
  %buf_l_f4_51 = OpSDiv %int %buf_l_f4_49 %int_4
        OpStore %buf_l_f4_44 %buf_l_f4_51
  %buf_l_f4_58 = OpLoad %int %buf_l_f4_28
  %buf_l_f4_63 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_l_f4_58 %int_0 %buf_l_f4_51
  %buf_l_f4_64 = OpLoad %float %buf_l_f4_63
        OpStore %buf_l_f4_21 %buf_l_f4_64
  %buf_l_f4_65 = OpLoad %int %buf_l_f4_28
  %buf_l_f4_68 = OpIAdd %int %buf_l_f4_51 %int_1
  %buf_l_f4_69 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_l_f4_65 %int_0 %buf_l_f4_68
  %buf_l_f4_70 = OpLoad %float %buf_l_f4_69
        OpStore %buf_l_f4_22 %buf_l_f4_70
  %buf_l_f4_71 = OpLoad %int %buf_l_f4_28
  %buf_l_f4_74 = OpIAdd %int %buf_l_f4_51 %int_2
  %buf_l_f4_75 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_l_f4_71 %int_0 %buf_l_f4_74
  %buf_l_f4_76 = OpLoad %float %buf_l_f4_75
        OpStore %buf_l_f4_23 %buf_l_f4_76
  %buf_l_f4_77 = OpLoad %int %buf_l_f4_28
  %buf_l_f4_80 = OpIAdd %int %buf_l_f4_51 %int_3
  %buf_l_f4_81 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_l_f4_77 %int_0 %buf_l_f4_80
  %buf_l_f4_82 = OpLoad %float %buf_l_f4_81
        OpStore %buf_l_f4_24 %buf_l_f4_82
        OpReturn
        OpFunctionEnd
			)";

constexpr char BUFFER_STORE_FLOAT1[] = R"(
             ; void buffer_store_float1(in float p1, in int index, in int offset, in int stride, in int buffer_index)
             ; {
             ; 	int addr = (offset + index * stride)/4;
             ; 	buf[buffer_index].data[addr+0] = p1;
             ; }
%buffer_store_float1 = OpFunction %void None %function_buffer_load_store_float1
         %buf_s_f1_18 = OpFunctionParameter %_ptr_Function_float
         %buf_s_f1_19 = OpFunctionParameter %_ptr_Function_int
         %buf_s_f1_20 = OpFunctionParameter %_ptr_Function_int
         %buf_s_f1_21 = OpFunctionParameter %_ptr_Function_int
         %buf_s_f1_22 = OpFunctionParameter %_ptr_Function_int
         %buf_s_f1_24 = OpLabel
         %buf_s_f1_64 = OpVariable %_ptr_Function_int Function
         %buf_s_f1_65 = OpLoad %int %buf_s_f1_20
         %buf_s_f1_66 = OpLoad %int %buf_s_f1_19
         %buf_s_f1_67 = OpLoad %int %buf_s_f1_21
         %buf_s_f1_68 = OpIMul %int %buf_s_f1_66 %buf_s_f1_67
         %buf_s_f1_69 = OpIAdd %int %buf_s_f1_65 %buf_s_f1_68
         %buf_s_f1_70 = OpSDiv %int %buf_s_f1_69 %int_4
               OpStore %buf_s_f1_64 %buf_s_f1_70
         %buf_s_f1_71 = OpLoad %int %buf_s_f1_22
         %buf_s_f1_74 = OpLoad %float %buf_s_f1_18
         %buf_s_f1_75 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_s_f1_71 %int_0 %buf_s_f1_70
               OpStore %buf_s_f1_75 %buf_s_f1_74
               OpReturn
               OpFunctionEnd
)";

constexpr char BUFFER_STORE_FLOAT2[] = R"(
                      ; void buffer_store_float2(in float p1, in float p2, in int index, in int offset, in int stride, in int buffer_index)
                      ; {
                      ; 	int addr = (offset + index * stride)/4;
                      ; 	buf[buffer_index].data[addr+0] = p1;
                      ; 	buf[buffer_index].data[addr+1] = p2;
                      ; }
%buffer_store_float2 = OpFunction %void None %function_buffer_load_store_float2
         %buf_s_f2_51 = OpFunctionParameter %_ptr_Function_float
         %buf_s_f2_52 = OpFunctionParameter %_ptr_Function_float
         %buf_s_f2_53 = OpFunctionParameter %_ptr_Function_int
         %buf_s_f2_54 = OpFunctionParameter %_ptr_Function_int
         %buf_s_f2_55 = OpFunctionParameter %_ptr_Function_int
         %buf_s_f2_56 = OpFunctionParameter %_ptr_Function_int
         %buf_s_f2_58 = OpLabel
        %buf_s_f2_143 = OpVariable %_ptr_Function_int Function
        %buf_s_f2_144 = OpLoad %int %buf_s_f2_54
        %buf_s_f2_145 = OpLoad %int %buf_s_f2_53
        %buf_s_f2_146 = OpLoad %int %buf_s_f2_55
        %buf_s_f2_147 = OpIMul %int %buf_s_f2_145 %buf_s_f2_146
        %buf_s_f2_148 = OpIAdd %int %buf_s_f2_144 %buf_s_f2_147
        %buf_s_f2_149 = OpSDiv %int %buf_s_f2_148 %int_4
               OpStore %buf_s_f2_143 %buf_s_f2_149
        %buf_s_f2_150 = OpLoad %int %buf_s_f2_56
        %buf_s_f2_153 = OpLoad %float %buf_s_f2_51
        %buf_s_f2_154 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_s_f2_150 %int_0 %buf_s_f2_149
               OpStore %buf_s_f2_154 %buf_s_f2_153
        %buf_s_f2_155 = OpLoad %int %buf_s_f2_56
        %buf_s_f2_158 = OpIAdd %int %buf_s_f2_149 %int_1
        %buf_s_f2_159 = OpLoad %float %buf_s_f2_52
        %buf_s_f2_160 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_s_f2_155 %int_0 %buf_s_f2_158
               OpStore %buf_s_f2_160 %buf_s_f2_159
               OpReturn
               OpFunctionEnd
)";

constexpr char BUFFER_STORE_FLOAT4[] = R"(
                      ; void buffer_store_float4(in float p1, in float p2, in float p3, in float p4, in int index, in int offset, in int stride, in int buffer_index)
%buffer_store_float4 = OpFunction %void None %function_buffer_load_store_float4
         %buf_s_f4_1 = OpFunctionParameter %_ptr_Function_float
         %buf_s_f4_2 = OpFunctionParameter %_ptr_Function_float
         %buf_s_f4_3 = OpFunctionParameter %_ptr_Function_float
         %buf_s_f4_4 = OpFunctionParameter %_ptr_Function_float
         %buf_s_f4_5 = OpFunctionParameter %_ptr_Function_int
         %buf_s_f4_6 = OpFunctionParameter %_ptr_Function_int
         %buf_s_f4_7 = OpFunctionParameter %_ptr_Function_int
         %buf_s_f4_8 = OpFunctionParameter %_ptr_Function_int
        %buf_s_f4_10 = OpLabel
        %buf_s_f4_11 = OpLoad %int %buf_s_f4_6
        %buf_s_f4_12 = OpLoad %int %buf_s_f4_5
        %buf_s_f4_13 = OpLoad %int %buf_s_f4_7
        %buf_s_f4_14 = OpIMul %int %buf_s_f4_12 %buf_s_f4_13
        %buf_s_f4_15 = OpIAdd %int %buf_s_f4_11 %buf_s_f4_14
        %buf_s_f4_16 = OpSDiv %int %buf_s_f4_15 %int_4
        %buf_s_f4_17 = OpLoad %int %buf_s_f4_8
        %buf_s_f4_18 = OpLoad %float %buf_s_f4_1
        %buf_s_f4_19 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_s_f4_17 %int_0 %buf_s_f4_16
               OpStore %buf_s_f4_19 %buf_s_f4_18
        %buf_s_f4_20 = OpIAdd %int %buf_s_f4_16 %int_1
        %buf_s_f4_21 = OpLoad %float %buf_s_f4_2
        %buf_s_f4_22 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_s_f4_17 %int_0 %buf_s_f4_20
               OpStore %buf_s_f4_22 %buf_s_f4_21
        %buf_s_f4_23 = OpIAdd %int %buf_s_f4_16 %int_2
        %buf_s_f4_24 = OpLoad %float %buf_s_f4_3
        %buf_s_f4_25 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_s_f4_17 %int_0 %buf_s_f4_23
               OpStore %buf_s_f4_25 %buf_s_f4_24
        %buf_s_f4_26 = OpIAdd %int %buf_s_f4_16 %int_3
        %buf_s_f4_27 = OpLoad %float %buf_s_f4_4
        %buf_s_f4_28 = OpAccessChain %_ptr_StorageBuffer_float %buf %buf_s_f4_17 %int_0 %buf_s_f4_26
               OpStore %buf_s_f4_28 %buf_s_f4_27
               OpReturn
               OpFunctionEnd
)";

constexpr char TBUFFER_LOAD_FORMAT_XYZW[] = R"(
             ; Function tbuffer_load_format_xyzw
             ; void tbuffer_load_format_xyzw(out float p1, out float p2, out float p3, out float p4,
             ;                               in int index, in int offset, in int stride, in int buffer_index, in int dfmt_nfmt)
             ; {
             ; 	if (dfmt_nfmt == 119) // dfmt = 14, nfmt = 7
             ; 	{
             ; 		buffer_load_float4(p1, p2, p3, p4, index, offset, stride, buffer_index);
             ; 	}
             ; }
%tbuffer_load_format_xyzw = OpFunction %void None %function_tbuffer_load_store_format_xyzw
%tbuf_l_f_xyzw_54 = OpFunctionParameter %_ptr_Function_float
%tbuf_l_f_xyzw_55 = OpFunctionParameter %_ptr_Function_float
%tbuf_l_f_xyzw_56 = OpFunctionParameter %_ptr_Function_float
%tbuf_l_f_xyzw_57 = OpFunctionParameter %_ptr_Function_float
%tbuf_l_f_xyzw_58 = OpFunctionParameter %_ptr_Function_int
%tbuf_l_f_xyzw_59 = OpFunctionParameter %_ptr_Function_int
%tbuf_l_f_xyzw_60 = OpFunctionParameter %_ptr_Function_int
%tbuf_l_f_xyzw_61 = OpFunctionParameter %_ptr_Function_int
%tbuf_l_f_xyzw_62 = OpFunctionParameter %_ptr_Function_int
%tbuf_l_f_xyzw_64 = OpLabel
%tbuf_l_f_xyzw_166 = OpVariable %_ptr_Function_float Function
%tbuf_l_f_xyzw_167 = OpVariable %_ptr_Function_float Function
%tbuf_l_f_xyzw_168 = OpVariable %_ptr_Function_float Function
%tbuf_l_f_xyzw_169 = OpVariable %_ptr_Function_float Function
%tbuf_l_f_xyzw_170 = OpVariable %_ptr_Function_int Function
%tbuf_l_f_xyzw_172 = OpVariable %_ptr_Function_int Function
%tbuf_l_f_xyzw_174 = OpVariable %_ptr_Function_int Function
%tbuf_l_f_xyzw_176 = OpVariable %_ptr_Function_int Function
%tbuf_l_f_xyzw_161 = OpLoad %int %tbuf_l_f_xyzw_62
%tbuf_l_f_xyzw_162 = OpSGreaterThanEqual %bool %tbuf_l_f_xyzw_161 %int_75
%tbuf_l_f_xyzw_163 = OpSLessThanEqual %bool %tbuf_l_f_xyzw_161 %int_77
%tbuf_l_f_xyzw_160 = OpLogicalAnd %bool %tbuf_l_f_xyzw_162 %tbuf_l_f_xyzw_163
%tbuf_l_f_xyzw_159 = OpIEqual %bool %tbuf_l_f_xyzw_161 %int_119
%tbuf_l_f_xyzw_158 = OpLogicalOr %bool %tbuf_l_f_xyzw_160 %tbuf_l_f_xyzw_159
   OpSelectionMerge %tbuf_l_f_xyzw_165 None
   OpBranchConditional %tbuf_l_f_xyzw_158 %tbuf_l_f_xyzw_164 %tbuf_l_f_xyzw_165
%tbuf_l_f_xyzw_164 = OpLabel
%tbuf_l_f_xyzw_171 = OpLoad %int %tbuf_l_f_xyzw_58
   OpStore %tbuf_l_f_xyzw_170 %tbuf_l_f_xyzw_171
%tbuf_l_f_xyzw_173 = OpLoad %int %tbuf_l_f_xyzw_59
   OpStore %tbuf_l_f_xyzw_172 %tbuf_l_f_xyzw_173
%tbuf_l_f_xyzw_175 = OpLoad %int %tbuf_l_f_xyzw_60
   OpStore %tbuf_l_f_xyzw_174 %tbuf_l_f_xyzw_175
%tbuf_l_f_xyzw_177 = OpLoad %int %tbuf_l_f_xyzw_61
   OpStore %tbuf_l_f_xyzw_176 %tbuf_l_f_xyzw_177
%tbuf_l_f_xyzw_178 = OpFunctionCall %void %buffer_load_float4 %tbuf_l_f_xyzw_166 %tbuf_l_f_xyzw_167 %tbuf_l_f_xyzw_168 %tbuf_l_f_xyzw_169 %tbuf_l_f_xyzw_170 %tbuf_l_f_xyzw_172 %tbuf_l_f_xyzw_174 %tbuf_l_f_xyzw_176
%tbuf_l_f_xyzw_179 = OpLoad %float %tbuf_l_f_xyzw_166
   OpStore %tbuf_l_f_xyzw_54 %tbuf_l_f_xyzw_179
%tbuf_l_f_xyzw_180 = OpLoad %float %tbuf_l_f_xyzw_167
   OpStore %tbuf_l_f_xyzw_55 %tbuf_l_f_xyzw_180
%tbuf_l_f_xyzw_181 = OpLoad %float %tbuf_l_f_xyzw_168
   OpStore %tbuf_l_f_xyzw_56 %tbuf_l_f_xyzw_181
%tbuf_l_f_xyzw_182 = OpLoad %float %tbuf_l_f_xyzw_169
   OpStore %tbuf_l_f_xyzw_57 %tbuf_l_f_xyzw_182
   OpBranch %tbuf_l_f_xyzw_165
%tbuf_l_f_xyzw_165 = OpLabel
   OpReturn
   OpFunctionEnd
			)";

constexpr char TBUFFER_LOAD_FORMAT_X[] = R"(
             ; void tbuffer_load_format_x(out float p1, in int index, in int offset, in int stride, in int buffer_index, in int dfmt_nfmt)
             ; {
             ; 	if (dfmt_nfmt == 36 || dfmt_nfmt == 39) // dmft = 4, nfmt = 4 or 7
             ; 	{
             ; 		buffer_load_float1(p1, index, offset, stride, buffer_index);
             ; 	}
             ; }
%tbuffer_load_format_x = OpFunction %void None %function_tbuffer_load_store_format_x
         %tbuf_l_f_x_26 = OpFunctionParameter %_ptr_Function_float
         %tbuf_l_f_x_27 = OpFunctionParameter %_ptr_Function_int
         %tbuf_l_f_x_28 = OpFunctionParameter %_ptr_Function_int
         %tbuf_l_f_x_29 = OpFunctionParameter %_ptr_Function_int
         %tbuf_l_f_x_30 = OpFunctionParameter %_ptr_Function_int
         %tbuf_l_f_x_31 = OpFunctionParameter %_ptr_Function_int
         %tbuf_l_f_x_33 = OpLabel
         %tbuf_l_f_x_82 = OpVariable %_ptr_Function_float Function
         %tbuf_l_f_x_83 = OpVariable %_ptr_Function_int Function
         %tbuf_l_f_x_85 = OpVariable %_ptr_Function_int Function
         %tbuf_l_f_x_87 = OpVariable %_ptr_Function_int Function
         %tbuf_l_f_x_89 = OpVariable %_ptr_Function_int Function
         %tbuf_l_f_x_76 = OpLoad %int %tbuf_l_f_x_31
         %tbuf_l_f_x_79 = OpIEqual %bool %tbuf_l_f_x_76 %int_36
         %tbuf_l_f_x_79_2 = OpIEqual %bool %tbuf_l_f_x_76 %int_39
         %tbuf_l_f_x_79_3 = OpLogicalOr %bool %tbuf_l_f_x_79 %tbuf_l_f_x_79_2
               OpSelectionMerge %tbuf_l_f_x_81 None
               OpBranchConditional %tbuf_l_f_x_79_3 %tbuf_l_f_x_80 %tbuf_l_f_x_81
         %tbuf_l_f_x_80 = OpLabel
         %tbuf_l_f_x_84 = OpLoad %int %tbuf_l_f_x_27
               OpStore %tbuf_l_f_x_83 %tbuf_l_f_x_84
         %tbuf_l_f_x_86 = OpLoad %int %tbuf_l_f_x_28
               OpStore %tbuf_l_f_x_85 %tbuf_l_f_x_86
         %tbuf_l_f_x_88 = OpLoad %int %tbuf_l_f_x_29
               OpStore %tbuf_l_f_x_87 %tbuf_l_f_x_88
         %tbuf_l_f_x_90 = OpLoad %int %tbuf_l_f_x_30
               OpStore %tbuf_l_f_x_89 %tbuf_l_f_x_90
         %tbuf_l_f_x_91 = OpFunctionCall %void %buffer_load_float1 %tbuf_l_f_x_82 %tbuf_l_f_x_83 %tbuf_l_f_x_85 %tbuf_l_f_x_87 %tbuf_l_f_x_89
         %tbuf_l_f_x_92 = OpLoad %float %tbuf_l_f_x_82
               OpStore %tbuf_l_f_x_26 %tbuf_l_f_x_92
               OpBranch %tbuf_l_f_x_81
         %tbuf_l_f_x_81 = OpLabel
               OpReturn
               OpFunctionEnd
)";

constexpr char TBUFFER_STORE_FORMAT_X[] = R"(
             ; void tbuffer_store_format_x(in float p1, in int index, in int offset, in int stride, in int buffer_index, in int dfmt_nfmt)
             ; {
             ; 	if (dfmt_nfmt == 36 || dfmt_nfmt == 39) // dmft = 4, nfmt = 4 or 7
             ; 	{
             ; 		buffer_store_float1(p1, index, offset, stride, buffer_index);
             ; 	}
             ; }
%tbuffer_store_format_x = OpFunction %void None %function_tbuffer_load_store_format_x
         %tbuf_s_f_x_34 = OpFunctionParameter %_ptr_Function_float
         %tbuf_s_f_x_35 = OpFunctionParameter %_ptr_Function_int
         %tbuf_s_f_x_36 = OpFunctionParameter %_ptr_Function_int
         %tbuf_s_f_x_37 = OpFunctionParameter %_ptr_Function_int
         %tbuf_s_f_x_38 = OpFunctionParameter %_ptr_Function_int
         %tbuf_s_f_x_39 = OpFunctionParameter %_ptr_Function_int
         %tbuf_s_f_x_41 = OpLabel
         %tbuf_s_f_x_97 = OpVariable %_ptr_Function_float Function
         %tbuf_s_f_x_99 = OpVariable %_ptr_Function_int Function
        %tbuf_s_f_x_101 = OpVariable %_ptr_Function_int Function
        %tbuf_s_f_x_103 = OpVariable %_ptr_Function_int Function
        %tbuf_s_f_x_105 = OpVariable %_ptr_Function_int Function
         %tbuf_s_f_x_93 = OpLoad %int %tbuf_s_f_x_39
         %tbuf_s_f_x_94 = OpIEqual %bool %tbuf_s_f_x_93 %int_36
         %tbuf_s_f_x_94_2 = OpIEqual %bool %tbuf_s_f_x_93 %int_39
         %tbuf_s_f_x_94_3 = OpLogicalOr %bool %tbuf_s_f_x_94 %tbuf_s_f_x_94_2
               OpSelectionMerge %tbuf_s_f_x_96 None
               OpBranchConditional %tbuf_s_f_x_94_3 %tbuf_s_f_x_95 %tbuf_s_f_x_96
         %tbuf_s_f_x_95 = OpLabel
         %tbuf_s_f_x_98 = OpLoad %float %tbuf_s_f_x_34
               OpStore %tbuf_s_f_x_97 %tbuf_s_f_x_98
        %tbuf_s_f_x_100 = OpLoad %int %tbuf_s_f_x_35
               OpStore %tbuf_s_f_x_99 %tbuf_s_f_x_100
        %tbuf_s_f_x_102 = OpLoad %int %tbuf_s_f_x_36
               OpStore %tbuf_s_f_x_101 %tbuf_s_f_x_102
        %tbuf_s_f_x_104 = OpLoad %int %tbuf_s_f_x_37
               OpStore %tbuf_s_f_x_103 %tbuf_s_f_x_104
        %tbuf_s_f_x_106 = OpLoad %int %tbuf_s_f_x_38
               OpStore %tbuf_s_f_x_105 %tbuf_s_f_x_106
        %tbuf_s_f_x_107 = OpFunctionCall %void %buffer_store_float1 %tbuf_s_f_x_97 %tbuf_s_f_x_99 %tbuf_s_f_x_101 %tbuf_s_f_x_103 %tbuf_s_f_x_105
               OpBranch %tbuf_s_f_x_96
         %tbuf_s_f_x_96 = OpLabel
               OpReturn
               OpFunctionEnd
)";

constexpr char TBUFFER_STORE_FORMAT_XY[] = R"(
                        ; void tbuffer_store_format_xy(in float p1, in float p2, in int index, in int offset, in int stride, in int buffer_index, in int dfmt_nfmt)
                        ; {
                        ; 	if (dfmt_nfmt == 92 || dfmt_nfmt == 95) // dmft = 11, nfmt = 4 or 7
                        ; 	{
                        ; 		buffer_store_float2(p1, p2, index, offset, stride, buffer_index);
                        ; 	}
                        ; }
%tbuffer_store_format_xy = OpFunction %void None %function_tbuffer_load_store_format_xy
         %tbuf_s_f_xy_60 = OpFunctionParameter %_ptr_Function_float
         %tbuf_s_f_xy_61 = OpFunctionParameter %_ptr_Function_float
         %tbuf_s_f_xy_62 = OpFunctionParameter %_ptr_Function_int
         %tbuf_s_f_xy_63 = OpFunctionParameter %_ptr_Function_int
         %tbuf_s_f_xy_64 = OpFunctionParameter %_ptr_Function_int
         %tbuf_s_f_xy_65 = OpFunctionParameter %_ptr_Function_int
         %tbuf_s_f_xy_66 = OpFunctionParameter %_ptr_Function_int
         %tbuf_s_f_xy_68 = OpLabel
        %tbuf_s_f_xy_170 = OpVariable %_ptr_Function_float Function
        %tbuf_s_f_xy_172 = OpVariable %_ptr_Function_float Function
        %tbuf_s_f_xy_174 = OpVariable %_ptr_Function_int Function
        %tbuf_s_f_xy_176 = OpVariable %_ptr_Function_int Function
        %tbuf_s_f_xy_178 = OpVariable %_ptr_Function_int Function
        %tbuf_s_f_xy_180 = OpVariable %_ptr_Function_int Function
        %tbuf_s_f_xy_161 = OpLoad %int %tbuf_s_f_xy_66
        %tbuf_s_f_xy_163 = OpIEqual %bool %tbuf_s_f_xy_161 %int_92
        %tbuf_s_f_xy_164 = OpLoad %int %tbuf_s_f_xy_66
        %tbuf_s_f_xy_166 = OpIEqual %bool %tbuf_s_f_xy_164 %int_95
        %tbuf_s_f_xy_167 = OpLogicalOr %bool %tbuf_s_f_xy_163 %tbuf_s_f_xy_166
               OpSelectionMerge %tbuf_s_f_xy_169 None
               OpBranchConditional %tbuf_s_f_xy_167 %tbuf_s_f_xy_168 %tbuf_s_f_xy_169
        %tbuf_s_f_xy_168 = OpLabel
        %tbuf_s_f_xy_171 = OpLoad %float %tbuf_s_f_xy_60
               OpStore %tbuf_s_f_xy_170 %tbuf_s_f_xy_171
        %tbuf_s_f_xy_173 = OpLoad %float %tbuf_s_f_xy_61
               OpStore %tbuf_s_f_xy_172 %tbuf_s_f_xy_173
        %tbuf_s_f_xy_175 = OpLoad %int %tbuf_s_f_xy_62
               OpStore %tbuf_s_f_xy_174 %tbuf_s_f_xy_175
        %tbuf_s_f_xy_177 = OpLoad %int %tbuf_s_f_xy_63
               OpStore %tbuf_s_f_xy_176 %tbuf_s_f_xy_177
        %tbuf_s_f_xy_179 = OpLoad %int %tbuf_s_f_xy_64
               OpStore %tbuf_s_f_xy_178 %tbuf_s_f_xy_179
        %tbuf_s_f_xy_181 = OpLoad %int %tbuf_s_f_xy_65
               OpStore %tbuf_s_f_xy_180 %tbuf_s_f_xy_181
        %tbuf_s_f_xy_182 = OpFunctionCall %void %buffer_store_float2 %tbuf_s_f_xy_170 %tbuf_s_f_xy_172 %tbuf_s_f_xy_174 %tbuf_s_f_xy_176 %tbuf_s_f_xy_178 %tbuf_s_f_xy_180
               OpBranch %tbuf_s_f_xy_169
        %tbuf_s_f_xy_169 = OpLabel
               OpReturn
               OpFunctionEnd
)";

constexpr char TBUFFER_STORE_FORMAT_XYZW[] = R"(
; void tbuffer_store_format_xyzw(in float p1, in float p2, in float p3, in float p4, in int index, in int offset, in int stride, in int buffer_index, in int dfmt_nfmt)
%tbuffer_store_format_xyzw = OpFunction %void None %function_tbuffer_load_store_format_xyzw
 %tbuf_s_f_xyzw_1 = OpFunctionParameter %_ptr_Function_float
 %tbuf_s_f_xyzw_2 = OpFunctionParameter %_ptr_Function_float
 %tbuf_s_f_xyzw_3 = OpFunctionParameter %_ptr_Function_float
 %tbuf_s_f_xyzw_4 = OpFunctionParameter %_ptr_Function_float
 %tbuf_s_f_xyzw_5 = OpFunctionParameter %_ptr_Function_int
 %tbuf_s_f_xyzw_6 = OpFunctionParameter %_ptr_Function_int
 %tbuf_s_f_xyzw_7 = OpFunctionParameter %_ptr_Function_int
 %tbuf_s_f_xyzw_8 = OpFunctionParameter %_ptr_Function_int
 %tbuf_s_f_xyzw_9 = OpFunctionParameter %_ptr_Function_int
%tbuf_s_f_xyzw_10 = OpLabel
%tbuf_s_f_xyzw_11 = OpLoad %int %tbuf_s_f_xyzw_9
%tbuf_s_f_xyzw_12 = OpSGreaterThanEqual %bool %tbuf_s_f_xyzw_11 %int_75
%tbuf_s_f_xyzw_16 = OpSLessThanEqual %bool %tbuf_s_f_xyzw_11 %int_77
%tbuf_s_f_xyzw_17 = OpLogicalAnd %bool %tbuf_s_f_xyzw_12 %tbuf_s_f_xyzw_16
%tbuf_s_f_xyzw_18 = OpIEqual %bool %tbuf_s_f_xyzw_11 %int_119
%tbuf_s_f_xyzw_19 = OpLogicalOr %bool %tbuf_s_f_xyzw_17 %tbuf_s_f_xyzw_18
               OpSelectionMerge %tbuf_s_f_xyzw_14 None
               OpBranchConditional %tbuf_s_f_xyzw_19 %tbuf_s_f_xyzw_13 %tbuf_s_f_xyzw_14
%tbuf_s_f_xyzw_13 = OpLabel
%tbuf_s_f_xyzw_15 = OpFunctionCall %void %buffer_store_float4 %tbuf_s_f_xyzw_1 %tbuf_s_f_xyzw_2 %tbuf_s_f_xyzw_3 %tbuf_s_f_xyzw_4 %tbuf_s_f_xyzw_5 %tbuf_s_f_xyzw_6 %tbuf_s_f_xyzw_7 %tbuf_s_f_xyzw_8
               OpBranch %tbuf_s_f_xyzw_14
%tbuf_s_f_xyzw_14 = OpLabel
               OpReturn
               OpFunctionEnd
)";

constexpr char SBUFFER_LOAD_DWORD[] = R"(
                     ; void sbuffer_load_dword(out uint p1, in int offset, in int buffer_index)
                     ; {
                     ; 	int addr = offset/4;
                     ; 	p1 = floatBitsToUint(buf[buffer_index].data[addr+0]);
                     ; }
%sbuffer_load_dword = OpFunction %void None %function_sbuffer_load_dword
         %sbuf_dw_45 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw_46 = OpFunctionParameter %_ptr_Function_int
         %sbuf_dw_47 = OpFunctionParameter %_ptr_Function_int
         %sbuf_dw_49 = OpLabel
        %sbuf_dw_115 = OpVariable %_ptr_Function_int Function
        %sbuf_dw_116 = OpLoad %int %sbuf_dw_46
        %sbuf_dw_117 = OpSDiv %int %sbuf_dw_116 %int_4
               OpStore %sbuf_dw_115 %sbuf_dw_117
        %sbuf_dw_118 = OpLoad %int %sbuf_dw_47
        %sbuf_dw_121 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw_118 %int_0 %sbuf_dw_117
        %sbuf_dw_122 = OpLoad %float %sbuf_dw_121
        %sbuf_dw_123 = OpBitcast %uint %sbuf_dw_122
               OpStore %sbuf_dw_45 %sbuf_dw_123
               OpReturn
               OpFunctionEnd
)";

constexpr char SBUFFER_LOAD_DWORD_2[] = R"(
                      ; void sbuffer_load_dwordx2(out uint p1, out uint p2, in int offset, in int buffer_index)
                      ; {
                      ; 	int addr = offset/4;
                      ; 	p1 = floatBitsToUint(buf[buffer_index].data[addr+0]);
                      ; 	p2 = floatBitsToUint(buf[buffer_index].data[addr+1]);
                      ; }
%sbuffer_load_dword_2 = OpFunction %void None %function_sbuffer_load_dword_2
         %sbuf_dw2_11 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw2_12 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw2_13 = OpFunctionParameter %_ptr_Function_int
         %sbuf_dw2_14 = OpFunctionParameter %_ptr_Function_int
         %sbuf_dw2_16 = OpLabel
         %sbuf_dw2_17 = OpVariable %_ptr_Function_int Function
         %sbuf_dw2_18 = OpLoad %int %sbuf_dw2_13
         %sbuf_dw2_20 = OpSDiv %int %sbuf_dw2_18 %int_4
               OpStore %sbuf_dw2_17 %sbuf_dw2_20
         %sbuf_dw2_28 = OpLoad %int %sbuf_dw2_14
         %sbuf_dw2_33 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw2_28 %int_0 %sbuf_dw2_20
         %sbuf_dw2_34 = OpLoad %float %sbuf_dw2_33
         %sbuf_dw2_35 = OpBitcast %uint %sbuf_dw2_34
               OpStore %sbuf_dw2_11 %sbuf_dw2_35
         %sbuf_dw2_36 = OpLoad %int %sbuf_dw2_14
         %sbuf_dw2_39 = OpIAdd %int %sbuf_dw2_20 %int_1
         %sbuf_dw2_40 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw2_36 %int_0 %sbuf_dw2_39
         %sbuf_dw2_41 = OpLoad %float %sbuf_dw2_40
         %sbuf_dw2_42 = OpBitcast %uint %sbuf_dw2_41
               OpStore %sbuf_dw2_12 %sbuf_dw2_42
               OpReturn
               OpFunctionEnd
)";

constexpr char SBUFFER_LOAD_DWORD_4[] = R"(
                     ; void sbuffer_load_dwordx4(out uint p1, out uint p2, out uint p3, out uint p4, in int offset, in int buffer_index)
                     ; {
                     ; 	int addr = offset/4;
                     ; 	p1 = floatBitsToUint(buf[buffer_index].data[addr+0]);
                     ; 	p2 = floatBitsToUint(buf[buffer_index].data[addr+1]);
                     ; 	p3 = floatBitsToUint(buf[buffer_index].data[addr+2]);
                     ; 	p4 = floatBitsToUint(buf[buffer_index].data[addr+3]);
                     ; }
%sbuffer_load_dword_4 = OpFunction %void None %function_sbuffer_load_dword_4
         %sbuf_dw4_51 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw4_52 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw4_53 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw4_54 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw4_55 = OpFunctionParameter %_ptr_Function_int
         %sbuf_dw4_56 = OpFunctionParameter %_ptr_Function_int
         %sbuf_dw4_58 = OpLabel
        %sbuf_dw4_133 = OpVariable %_ptr_Function_int Function
        %sbuf_dw4_134 = OpLoad %int %sbuf_dw4_55
        %sbuf_dw4_135 = OpSDiv %int %sbuf_dw4_134 %int_4
               OpStore %sbuf_dw4_133 %sbuf_dw4_135
        %sbuf_dw4_136 = OpLoad %int %sbuf_dw4_56
        %sbuf_dw4_139 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw4_136 %int_0 %sbuf_dw4_135
        %sbuf_dw4_140 = OpLoad %float %sbuf_dw4_139
        %sbuf_dw4_141 = OpBitcast %uint %sbuf_dw4_140
               OpStore %sbuf_dw4_51 %sbuf_dw4_141
        %sbuf_dw4_142 = OpLoad %int %sbuf_dw4_56
        %sbuf_dw4_145 = OpIAdd %int %sbuf_dw4_135 %int_1
        %sbuf_dw4_146 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw4_142 %int_0 %sbuf_dw4_145
        %sbuf_dw4_147 = OpLoad %float %sbuf_dw4_146
        %sbuf_dw4_148 = OpBitcast %uint %sbuf_dw4_147
               OpStore %sbuf_dw4_52 %sbuf_dw4_148
        %sbuf_dw4_149 = OpLoad %int %sbuf_dw4_56
        %sbuf_dw4_152 = OpIAdd %int %sbuf_dw4_135 %int_2
        %sbuf_dw4_153 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw4_149 %int_0 %sbuf_dw4_152
        %sbuf_dw4_154 = OpLoad %float %sbuf_dw4_153
        %sbuf_dw4_155 = OpBitcast %uint %sbuf_dw4_154
               OpStore %sbuf_dw4_53 %sbuf_dw4_155
        %sbuf_dw4_156 = OpLoad %int %sbuf_dw4_56
        %sbuf_dw4_159 = OpIAdd %int %sbuf_dw4_135 %int_3
        %sbuf_dw4_160 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw4_156 %int_0 %sbuf_dw4_159
        %sbuf_dw4_161 = OpLoad %float %sbuf_dw4_160
        %sbuf_dw4_162 = OpBitcast %uint %sbuf_dw4_161
               OpStore %sbuf_dw4_54 %sbuf_dw4_162
               OpReturn
               OpFunctionEnd
)";

constexpr char SBUFFER_LOAD_DWORD_8[] = R"(
                     ; void sbuffer_load_dwordx8(out uint p1, out uint p2, out uint p3, out uint p4,
                     ;                           out uint p5, out uint p6, out uint p7, out uint p8, in int offset, in int buffer_index)
                     ; {
                     ; 	int addr = offset/4;
                     ; 	p1 = floatBitsToUint(buf[buffer_index].data[addr+0]);
                     ; 	p2 = floatBitsToUint(buf[buffer_index].data[addr+1]);
                     ; 	p3 = floatBitsToUint(buf[buffer_index].data[addr+2]);
                     ; 	p4 = floatBitsToUint(buf[buffer_index].data[addr+3]);
                     ; 	p5 = floatBitsToUint(buf[buffer_index].data[addr+4]);
                     ; 	p6 = floatBitsToUint(buf[buffer_index].data[addr+5]);
                     ; 	p7 = floatBitsToUint(buf[buffer_index].data[addr+6]);
                     ; 	p8 = floatBitsToUint(buf[buffer_index].data[addr+7]);
                     ; }
%sbuffer_load_dword_8 = OpFunction %void None %function_sbuffer_load_dword_8
         %sbuf_dw8_60 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw8_61 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw8_62 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw8_63 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw8_64 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw8_65 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw8_66 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw8_67 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw8_68 = OpFunctionParameter %_ptr_Function_int
         %sbuf_dw8_69 = OpFunctionParameter %_ptr_Function_int
         %sbuf_dw8_71 = OpLabel
        %sbuf_dw8_197 = OpVariable %_ptr_Function_int Function
        %sbuf_dw8_198 = OpLoad %int %sbuf_dw8_68
        %sbuf_dw8_199 = OpSDiv %int %sbuf_dw8_198 %int_4
               OpStore %sbuf_dw8_197 %sbuf_dw8_199
        %sbuf_dw8_200 = OpLoad %int %sbuf_dw8_69
        %sbuf_dw8_203 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw8_200 %int_0 %sbuf_dw8_199
        %sbuf_dw8_204 = OpLoad %float %sbuf_dw8_203
        %sbuf_dw8_205 = OpBitcast %uint %sbuf_dw8_204
               OpStore %sbuf_dw8_60 %sbuf_dw8_205
        %sbuf_dw8_206 = OpLoad %int %sbuf_dw8_69
        %sbuf_dw8_208 = OpIAdd %int %sbuf_dw8_199 %int_1
        %sbuf_dw8_209 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw8_206 %int_0 %sbuf_dw8_208
        %sbuf_dw8_210 = OpLoad %float %sbuf_dw8_209
        %sbuf_dw8_211 = OpBitcast %uint %sbuf_dw8_210
               OpStore %sbuf_dw8_61 %sbuf_dw8_211
        %sbuf_dw8_212 = OpLoad %int %sbuf_dw8_69
        %sbuf_dw8_214 = OpIAdd %int %sbuf_dw8_199 %int_2
        %sbuf_dw8_215 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw8_212 %int_0 %sbuf_dw8_214
        %sbuf_dw8_216 = OpLoad %float %sbuf_dw8_215
        %sbuf_dw8_217 = OpBitcast %uint %sbuf_dw8_216
               OpStore %sbuf_dw8_62 %sbuf_dw8_217
        %sbuf_dw8_218 = OpLoad %int %sbuf_dw8_69
        %sbuf_dw8_220 = OpIAdd %int %sbuf_dw8_199 %int_3
        %sbuf_dw8_221 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw8_218 %int_0 %sbuf_dw8_220
        %sbuf_dw8_222 = OpLoad %float %sbuf_dw8_221
        %sbuf_dw8_223 = OpBitcast %uint %sbuf_dw8_222
               OpStore %sbuf_dw8_63 %sbuf_dw8_223
        %sbuf_dw8_224 = OpLoad %int %sbuf_dw8_69
        %sbuf_dw8_226 = OpIAdd %int %sbuf_dw8_199 %int_4
        %sbuf_dw8_227 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw8_224 %int_0 %sbuf_dw8_226
        %sbuf_dw8_228 = OpLoad %float %sbuf_dw8_227
        %sbuf_dw8_229 = OpBitcast %uint %sbuf_dw8_228
               OpStore %sbuf_dw8_64 %sbuf_dw8_229
        %sbuf_dw8_230 = OpLoad %int %sbuf_dw8_69
        %sbuf_dw8_233 = OpIAdd %int %sbuf_dw8_199 %int_5
        %sbuf_dw8_234 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw8_230 %int_0 %sbuf_dw8_233
        %sbuf_dw8_235 = OpLoad %float %sbuf_dw8_234
        %sbuf_dw8_236 = OpBitcast %uint %sbuf_dw8_235
               OpStore %sbuf_dw8_65 %sbuf_dw8_236
        %sbuf_dw8_237 = OpLoad %int %sbuf_dw8_69
        %sbuf_dw8_240 = OpIAdd %int %sbuf_dw8_199 %int_6
        %sbuf_dw8_241 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw8_237 %int_0 %sbuf_dw8_240
        %sbuf_dw8_242 = OpLoad %float %sbuf_dw8_241
        %sbuf_dw8_243 = OpBitcast %uint %sbuf_dw8_242
               OpStore %sbuf_dw8_66 %sbuf_dw8_243
        %sbuf_dw8_244 = OpLoad %int %sbuf_dw8_69
        %sbuf_dw8_247 = OpIAdd %int %sbuf_dw8_199 %int_7
        %sbuf_dw8_248 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw8_244 %int_0 %sbuf_dw8_247
        %sbuf_dw8_249 = OpLoad %float %sbuf_dw8_248
        %sbuf_dw8_250 = OpBitcast %uint %sbuf_dw8_249
               OpStore %sbuf_dw8_67 %sbuf_dw8_250
               OpReturn
               OpFunctionEnd
)";

constexpr char SBUFFER_LOAD_DWORD_16[] = R"(
                     ; void sbuffer_load_dwordx16(out uint p1, out uint p2, out uint p3, out uint p4,
                     ;                            out uint p5, out uint p6, out uint p7, out uint p8,
                     ;                            out uint p9, out uint p10, out uint p11, out uint p12,
                     ;                            out uint p13, out uint p14, out uint p15, out uint p16, in int offset, in int buffer_index)
                     ; {
                     ; 	int addr = offset/4;
                     ; 	p1 = floatBitsToUint(buf[buffer_index].data[addr+0]);
                     ; 	p2 = floatBitsToUint(buf[buffer_index].data[addr+1]);
                     ; 	p3 = floatBitsToUint(buf[buffer_index].data[addr+2]);
                     ; 	p4 = floatBitsToUint(buf[buffer_index].data[addr+3]);
                     ; 	p5 = floatBitsToUint(buf[buffer_index].data[addr+4]);
                     ; 	p6 = floatBitsToUint(buf[buffer_index].data[addr+5]);
                     ; 	p7 = floatBitsToUint(buf[buffer_index].data[addr+6]);
                     ; 	p8 = floatBitsToUint(buf[buffer_index].data[addr+7]);
                     ; 	p9 = floatBitsToUint(buf[buffer_index].data[addr+8]);
                     ; 	p10 = floatBitsToUint(buf[buffer_index].data[addr+9]);
                     ; 	p11 = floatBitsToUint(buf[buffer_index].data[addr+10]);
                     ; 	p12 = floatBitsToUint(buf[buffer_index].data[addr+11]);
                     ; 	p13 = floatBitsToUint(buf[buffer_index].data[addr+12]);
                     ; 	p14 = floatBitsToUint(buf[buffer_index].data[addr+13]);
                     ; 	p15 = floatBitsToUint(buf[buffer_index].data[addr+14]);
                     ; 	p16 = floatBitsToUint(buf[buffer_index].data[addr+15]);
                     ; }
%sbuffer_load_dword_16 = OpFunction %void None %function_sbuffer_load_dword_16
         %sbuf_dw16_60 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_61 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_62 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_63 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_64 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_65 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_66 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_67 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_68 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_69 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_70 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_71 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_72 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_73 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_74 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_75 = OpFunctionParameter %_ptr_Function_uint
         %sbuf_dw16_76 = OpFunctionParameter %_ptr_Function_int
         %sbuf_dw16_77 = OpFunctionParameter %_ptr_Function_int
         %sbuf_dw16_79 = OpLabel
        %sbuf_dw16_184 = OpVariable %_ptr_Function_int Function
        %sbuf_dw16_185 = OpLoad %int %sbuf_dw16_76
        %sbuf_dw16_186 = OpSDiv %int %sbuf_dw16_185 %int_4
               OpStore %sbuf_dw16_184 %sbuf_dw16_186
        %sbuf_dw16_187 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_190 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_187 %int_0 %sbuf_dw16_186
        %sbuf_dw16_191 = OpLoad %float %sbuf_dw16_190
        %sbuf_dw16_192 = OpBitcast %uint %sbuf_dw16_191
               OpStore %sbuf_dw16_60 %sbuf_dw16_192
        %sbuf_dw16_193 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_195 = OpIAdd %int %sbuf_dw16_186 %int_1
        %sbuf_dw16_196 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_193 %int_0 %sbuf_dw16_195
        %sbuf_dw16_197 = OpLoad %float %sbuf_dw16_196
        %sbuf_dw16_198 = OpBitcast %uint %sbuf_dw16_197
               OpStore %sbuf_dw16_61 %sbuf_dw16_198
        %sbuf_dw16_199 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_201 = OpIAdd %int %sbuf_dw16_186 %int_2
        %sbuf_dw16_202 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_199 %int_0 %sbuf_dw16_201
        %sbuf_dw16_203 = OpLoad %float %sbuf_dw16_202
        %sbuf_dw16_204 = OpBitcast %uint %sbuf_dw16_203
               OpStore %sbuf_dw16_62 %sbuf_dw16_204
        %sbuf_dw16_205 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_207 = OpIAdd %int %sbuf_dw16_186 %int_3
        %sbuf_dw16_208 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_205 %int_0 %sbuf_dw16_207
        %sbuf_dw16_209 = OpLoad %float %sbuf_dw16_208
        %sbuf_dw16_210 = OpBitcast %uint %sbuf_dw16_209
               OpStore %sbuf_dw16_63 %sbuf_dw16_210
        %sbuf_dw16_211 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_213 = OpIAdd %int %sbuf_dw16_186 %int_4
        %sbuf_dw16_214 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_211 %int_0 %sbuf_dw16_213
        %sbuf_dw16_215 = OpLoad %float %sbuf_dw16_214
        %sbuf_dw16_216 = OpBitcast %uint %sbuf_dw16_215
               OpStore %sbuf_dw16_64 %sbuf_dw16_216
        %sbuf_dw16_217 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_220 = OpIAdd %int %sbuf_dw16_186 %int_5
        %sbuf_dw16_221 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_217 %int_0 %sbuf_dw16_220
        %sbuf_dw16_222 = OpLoad %float %sbuf_dw16_221
        %sbuf_dw16_223 = OpBitcast %uint %sbuf_dw16_222
               OpStore %sbuf_dw16_65 %sbuf_dw16_223
        %sbuf_dw16_224 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_227 = OpIAdd %int %sbuf_dw16_186 %int_6
        %sbuf_dw16_228 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_224 %int_0 %sbuf_dw16_227
        %sbuf_dw16_229 = OpLoad %float %sbuf_dw16_228
        %sbuf_dw16_230 = OpBitcast %uint %sbuf_dw16_229
               OpStore %sbuf_dw16_66 %sbuf_dw16_230
        %sbuf_dw16_231 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_234 = OpIAdd %int %sbuf_dw16_186 %int_7
        %sbuf_dw16_235 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_231 %int_0 %sbuf_dw16_234
        %sbuf_dw16_236 = OpLoad %float %sbuf_dw16_235
        %sbuf_dw16_237 = OpBitcast %uint %sbuf_dw16_236
               OpStore %sbuf_dw16_67 %sbuf_dw16_237
        %sbuf_dw16_238 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_241 = OpIAdd %int %sbuf_dw16_186 %int_8
        %sbuf_dw16_242 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_238 %int_0 %sbuf_dw16_241
        %sbuf_dw16_243 = OpLoad %float %sbuf_dw16_242
        %sbuf_dw16_244 = OpBitcast %uint %sbuf_dw16_243
               OpStore %sbuf_dw16_68 %sbuf_dw16_244
        %sbuf_dw16_245 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_248 = OpIAdd %int %sbuf_dw16_186 %int_9
        %sbuf_dw16_249 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_245 %int_0 %sbuf_dw16_248
        %sbuf_dw16_250 = OpLoad %float %sbuf_dw16_249
        %sbuf_dw16_251 = OpBitcast %uint %sbuf_dw16_250
               OpStore %sbuf_dw16_69 %sbuf_dw16_251
        %sbuf_dw16_252 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_255 = OpIAdd %int %sbuf_dw16_186 %int_10
        %sbuf_dw16_256 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_252 %int_0 %sbuf_dw16_255
        %sbuf_dw16_257 = OpLoad %float %sbuf_dw16_256
        %sbuf_dw16_258 = OpBitcast %uint %sbuf_dw16_257
               OpStore %sbuf_dw16_70 %sbuf_dw16_258
        %sbuf_dw16_259 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_262 = OpIAdd %int %sbuf_dw16_186 %int_11
        %sbuf_dw16_263 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_259 %int_0 %sbuf_dw16_262
        %sbuf_dw16_264 = OpLoad %float %sbuf_dw16_263
        %sbuf_dw16_265 = OpBitcast %uint %sbuf_dw16_264
               OpStore %sbuf_dw16_71 %sbuf_dw16_265
        %sbuf_dw16_266 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_269 = OpIAdd %int %sbuf_dw16_186 %int_12
        %sbuf_dw16_270 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_266 %int_0 %sbuf_dw16_269
        %sbuf_dw16_271 = OpLoad %float %sbuf_dw16_270
        %sbuf_dw16_272 = OpBitcast %uint %sbuf_dw16_271
               OpStore %sbuf_dw16_72 %sbuf_dw16_272
        %sbuf_dw16_273 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_276 = OpIAdd %int %sbuf_dw16_186 %int_13
        %sbuf_dw16_277 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_273 %int_0 %sbuf_dw16_276
        %sbuf_dw16_278 = OpLoad %float %sbuf_dw16_277
        %sbuf_dw16_279 = OpBitcast %uint %sbuf_dw16_278
               OpStore %sbuf_dw16_73 %sbuf_dw16_279
        %sbuf_dw16_280 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_283 = OpIAdd %int %sbuf_dw16_186 %int_14
        %sbuf_dw16_284 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_280 %int_0 %sbuf_dw16_283
        %sbuf_dw16_285 = OpLoad %float %sbuf_dw16_284
        %sbuf_dw16_286 = OpBitcast %uint %sbuf_dw16_285
               OpStore %sbuf_dw16_74 %sbuf_dw16_286
        %sbuf_dw16_287 = OpLoad %int %sbuf_dw16_77
        %sbuf_dw16_290 = OpIAdd %int %sbuf_dw16_186 %int_15
        %sbuf_dw16_291 = OpAccessChain %_ptr_StorageBuffer_float %buf %sbuf_dw16_287 %int_0 %sbuf_dw16_290
        %sbuf_dw16_292 = OpLoad %float %sbuf_dw16_291
        %sbuf_dw16_293 = OpBitcast %uint %sbuf_dw16_292
               OpStore %sbuf_dw16_75 %sbuf_dw16_293
               OpReturn
               OpFunctionEnd
)";

constexpr char EMBEDDED_SHADER_VS_0[] = R"(
               ; #version 450
               ;
               ; void main()
               ; {
               ; 	float x = gl_VertexIndex == 0 || gl_VertexIndex == 2 ? 1.0 : -1.0;
               ; 	float y = gl_VertexIndex == 2 || gl_VertexIndex == 3 ? -1.0 : 1.0;
               ;
               ;     gl_Position = vec4(x,y, 0.0, 1.0);
               ; }

               OpCapability Shader
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint Vertex %4 "main" %gl_VertexIndex %43

               ; Annotations
               OpDecorate %gl_VertexIndex BuiltIn VertexIndex
               OpMemberDecorate %_struct_41 0 BuiltIn Position
               OpMemberDecorate %_struct_41 1 BuiltIn PointSize
               OpMemberDecorate %_struct_41 2 BuiltIn ClipDistance
               OpMemberDecorate %_struct_41 3 BuiltIn CullDistance
               OpDecorate %_struct_41 Block

               ; Types, variables and constants
       %void = OpTypeVoid
          %3 = OpTypeFunction %void
      %float = OpTypeFloat 32
%_ptr_Function_float = OpTypePointer Function %float
       %bool = OpTypeBool
        %int = OpTypeInt 32 1
%_ptr_Input_int = OpTypePointer Input %int
%gl_VertexIndex = OpVariable %_ptr_Input_int Input
      %int_0 = OpConstant %int 0
      %int_2 = OpConstant %int 2
    %float_1 = OpConstant %float 1
   %float_n1 = OpConstant %float -1
      %int_3 = OpConstant %int 3
    %v4float = OpTypeVector %float 4
       %uint = OpTypeInt 32 0
     %uint_1 = OpConstant %uint 1
%_arr_float_uint_1 = OpTypeArray %float %uint_1
 %_struct_41 = OpTypeStruct %v4float %float %_arr_float_uint_1 %_arr_float_uint_1
%_ptr_Output__struct_41 = OpTypePointer Output %_struct_41
         %43 = OpVariable %_ptr_Output__struct_41 Output
    %float_0 = OpConstant %float 0
%_ptr_Output_v4float = OpTypePointer Output %v4float

               ; Function 4
          %4 = OpFunction %void None %3
          %5 = OpLabel
          %8 = OpVariable %_ptr_Function_float Function
         %26 = OpVariable %_ptr_Function_float Function
         %13 = OpLoad %int %gl_VertexIndex
         %15 = OpIEqual %bool %13 %int_0
         %16 = OpLogicalNot %bool %15
               OpSelectionMerge %18 None
               OpBranchConditional %16 %17 %18
         %17 = OpLabel
         %21 = OpIEqual %bool %13 %int_2
               OpBranch %18
         %18 = OpLabel
         %22 = OpPhi %bool %15 %5 %21 %17
         %25 = OpSelect %float %22 %float_1 %float_n1
               OpStore %8 %25
         %28 = OpIEqual %bool %13 %int_2
         %29 = OpLogicalNot %bool %28
               OpSelectionMerge %31 None
               OpBranchConditional %29 %30 %31
         %30 = OpLabel
         %34 = OpIEqual %bool %13 %int_3
               OpBranch %31
         %31 = OpLabel
         %35 = OpPhi %bool %28 %18 %34 %30
         %36 = OpSelect %float %35 %float_n1 %float_1
               OpStore %26 %36
         %47 = OpCompositeConstruct %v4float %25 %36 %float_0 %float_1
         %49 = OpAccessChain %_ptr_Output_v4float %43 %int_0
               OpStore %49 %47
               OpReturn
               OpFunctionEnd
)";

constexpr char EMBEDDED_SHADER_PS_0[] = R"(
               ; #version 450
               ;
               ; layout(location = 0) out vec4 outColor;
               ;
               ; void main() {
               ; 	outColor = vec4(0);
               ; }

               OpCapability Shader
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %4 "main" %9
               OpExecutionMode %4 OriginUpperLeft

               ; Annotations
               OpDecorate %9 Location 0

               ; Types, variables and constants
       %void = OpTypeVoid
          %3 = OpTypeFunction %void
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
%_ptr_Output_v4float = OpTypePointer Output %v4float
          %9 = OpVariable %_ptr_Output_v4float Output
    %float_0 = OpConstant %float 0
         %11 = OpConstantComposite %v4float %float_0 %float_0 %float_0 %float_0

               ; Function 4
          %4 = OpFunction %void None %3
          %5 = OpLabel
               OpStore %9 %11
               OpReturn
               OpFunctionEnd
)";

constexpr char EXECZ[] = R"(
        %z191_<index> = OpLoad %uint %exec_lo
        %z192_<index> = OpIEqual %bool %z191_<index> %uint_0
        %z193_<index> = OpLoad %uint %exec_hi
        %z194_<index> = OpIEqual %bool %z193_<index> %uint_0
        %z195_<index> = OpLogicalAnd %bool %z192_<index> %z194_<index>
        %z196_<index> = OpSelect %uint %z195_<index> %uint_1 %uint_0
               OpStore %execz %z196_<index>
)";

constexpr char SCC_NZ_1[] = R"(
        %snz1_118_<index> = OpLoad %uint %<dst>
        %snz1_121_<index> = OpINotEqual %bool %snz1_118_<index> %uint_0
        %snz1_123_<index> = OpSelect %uint %snz1_121_<index> %uint_1 %uint_0
               OpStore %scc %snz1_123_<index>
)";

constexpr char SCC_NZ_2[] = R"(
        %snz2_124_<index> = OpLoad %uint %<dst0>
        %snz2_125_<index> = OpINotEqual %bool %snz2_124_<index> %uint_0
        %snz2_127_<index> = OpLoad %uint %<dst1>
        %snz2_128_<index> = OpINotEqual %bool %snz2_127_<index> %uint_0
        %snz2_129_<index> = OpLogicalOr %bool %snz2_125_<index> %snz2_128_<index>
        %snz2_130_<index> = OpSelect %uint %snz2_129_<index> %uint_1 %uint_0
               OpStore %scc %snz2_130_<index>
)";

constexpr char SCC_EXEC_NZ_2[] = R"(
        %snez2_124_<index> = OpINotEqual %bool %t194_<index> %uint_0
        %snez2_127_<index> = OpINotEqual %bool %t197_<index> %uint_0
        %snez2_129_<index> = OpLogicalOr %bool %snez2_124_<index> %snez2_127_<index>
        %snez2_130_<index> = OpSelect %uint %snez2_129_<index> %uint_1 %uint_0
               OpStore %scc %snez2_130_<index>
)";

constexpr char SCC_OVERFLOW_ADD_1[] = R"(
        %so1_124_<index> = OpExtInst %int %GLSL_std_450 SSign %t0_<index>
        %so1_127_<index> = OpExtInst %int %GLSL_std_450 SSign %t1_<index>
        %so1_129_<index> = OpLoad %uint %<dst>
        %so1_130_<index> = OpBitcast %int %so1_129_<index>
        %so1_131_<index> = OpExtInst %int %GLSL_std_450 SSign %so1_130_<index>
        %so1_135_<index> = OpIEqual %bool %so1_124_<index> %so1_127_<index>
        %so1_138_<index> = OpINotEqual %bool %so1_131_<index> %so1_124_<index>
        %so1_139_<index> = OpLogicalAnd %bool %so1_135_<index> %so1_138_<index>
        %so1_142_<index> = OpSelect %uint %so1_139_<index> %uint_1 %uint_0
               OpStore %scc %so1_142_<index>
)";

constexpr char SCC_OVERFLOW_SUB_1[] = R"(
        %so1_124_<index> = OpExtInst %int %GLSL_std_450 SSign %t0_<index>
        %so1_127_<index> = OpExtInst %int %GLSL_std_450 SSign %t1_<index>
        %so1_129_<index> = OpLoad %uint %<dst>
        %so1_130_<index> = OpBitcast %int %so1_129_<index>
        %so1_131_<index> = OpExtInst %int %GLSL_std_450 SSign %so1_130_<index>
        %so1_135_<index> = OpINotEqual %bool %so1_124_<index> %so1_127_<index>
        %so1_138_<index> = OpINotEqual %bool %so1_131_<index> %so1_124_<index>
        %so1_139_<index> = OpLogicalAnd %bool %so1_135_<index> %so1_138_<index>
        %so1_142_<index> = OpSelect %uint %so1_139_<index> %uint_1 %uint_0
               OpStore %scc %so1_142_<index>
)";

constexpr char SCC_CARRY_1[] = R"(
        OpStore %scc %carry_<index>
)";

constexpr char CLAMP[] = R"(
		%c197_<index> = OpLoad %float %<dst>
        %c200_<index> = OpExtInst %float %GLSL_std_450 FClamp %c197_<index> %float_0_000000 %float_1_000000
               OpStore %<dst> %c200_<index>
)";

constexpr char MULTIPLY[] = R"(
		%m197_<index> = OpLoad %float %<dst>
        %m200_<index> = OpFMul %float %m197_<index> %<mul>
               OpStore %<dst> %m200_<index>
)";

class Spirv;

enum class SccCheck
{
	None,
	NonZero,
	ExecNonZero,
	OverflowAdd,
	OverflowSub,
	CarryOut,
};

using inst_recompile_func_t = bool (*)(KYTY_RECOMPILER_ARGS);

enum class SpirvType
{
	Unknown,
	Float,
	Int,
	Uint
};

struct SpirvValue
{
	SpirvType type = SpirvType::Unknown;
	String8   value;
};

class Spirv
{
public:
	Spirv()          = default;
	virtual ~Spirv() = default;
	KYTY_CLASS_DEFAULT_COPY(Spirv);

	[[nodiscard]] const ShaderCode& GetCode() const { return m_code; }
	ShaderCode&                     GetCode() { return m_code; }
	void                            SetCode(const ShaderCode& m_code) { this->m_code = m_code; }

	void GenerateSource();

	[[nodiscard]] const String8& GetSource() const { return m_source; }
	[[nodiscard]] bool           CanLoadPackedHalfForExport(int export_index, ShaderOperand op) const;

	void                                       SetVsInputInfo(const ShaderVertexInputInfo* input_info) { m_vs_input_info = input_info; }
	[[nodiscard]] const ShaderVertexInputInfo* GetVsInputInfo() const { return m_vs_input_info; }

	void                                        SetCsInputInfo(const ShaderComputeInputInfo* input_info) { m_cs_input_info = input_info; }
	[[nodiscard]] const ShaderComputeInputInfo* GetCsInputInfo() const { return m_cs_input_info; }

	void                                      SetPsInputInfo(const ShaderPixelInputInfo* input_info) { m_ps_input_info = input_info; }
	[[nodiscard]] const ShaderPixelInputInfo* GetPsInputInfo() const { return m_ps_input_info; }

	[[nodiscard]] const ShaderBindResources* GetBindInfo() const { return m_bind; }
	//[[nodiscard]] const ShaderBindParameters& GetBindParams() const { return m_bind_params; }

	void                  AddConstantUint(uint32_t u);
	void                  AddConstantInt(int i);
	void                  AddConstantFloat(float f);
	void                  AddConstant(ShaderOperand op);
	[[nodiscard]] String8 GetConstantUint(uint32_t u) const;
	[[nodiscard]] String8 GetConstantInt(int i) const;
	[[nodiscard]] String8 GetConstantFloat(float f) const;
	[[nodiscard]] String8 GetConstant(ShaderOperand op) const;

	void GetMappedIndex(int offset, int* buffer, int* field) const
	{
		EXIT_NOT_IMPLEMENTED(offset >= m_extended_mapping.Size());
		*buffer = m_extended_mapping[offset][0];
		*field  = m_extended_mapping[offset][1];
	}

private:
	struct Variable
	{
		ShaderOperand op;
	};

	struct Constant
	{
		SpirvType      type     = SpirvType::Unknown;
		ShaderConstant constant = {0};
		String8        type_str;
		String8        value_str;
		String8        literal_str;
		String8        id;
	};

	void AddConstant(SpirvType type, ShaderConstant constant);
	void AddVariable(ShaderOperand op);
	void AddVariable(ShaderOperandType type, int register_id, int size);

	void WriteHeader();
	void WriteDebug();
	void WriteAnnotations();
	void WriteTypes();
	void WriteConstants();
	void WriteGlobalVariables();
	void WriteMainProlog();
	void WriteLocalVariables();
	void WriteInstructions();
	void WriteMainEpilog();
	void WriteFunctions();
	void WriteLabel(int index);

	void FindConstants();
	void FindVariables();

	void ModifyCode();

	void DetectFetch();

	String8                       m_source;
	ShaderCode                    m_code;
	Vector<Constant>              m_constants;
	Vector<Variable>              m_variables;
	const ShaderVertexInputInfo*  m_vs_input_info = nullptr;
	const ShaderComputeInputInfo* m_cs_input_info = nullptr;
	const ShaderPixelInputInfo*   m_ps_input_info = nullptr;
	const ShaderBindResources*    m_bind          = nullptr;
	// ShaderBindParameters          m_bind_params;

	Core::Array2<int, 64, 2> m_extended_mapping {};
};

// NOLINTNEXTLINE(clang-analyzer-optin.performance.Padding)
struct RecompilerFunc
{
	inst_recompile_func_t           func      = nullptr;
	ShaderInstructionType           type      = ShaderInstructionType::Unknown;
	ShaderInstructionFormat::Format format    = ShaderInstructionFormat::Unknown;
	const char*                     param[4]  = {nullptr, nullptr, nullptr, nullptr};
	SccCheck                        scc_check = SccCheck::None;
};

static bool operand_is_constant(ShaderOperand op)
{
	return (op.type == ShaderOperandType::LiteralConstant || op.type == ShaderOperandType::IntegerInlineConstant ||
	        op.type == ShaderOperandType::FloatInlineConstant);
}

static bool operand_is_variable(ShaderOperand op)
{
	return (op.type == ShaderOperandType::Vgpr || op.type == ShaderOperandType::VccLo || op.type == ShaderOperandType::VccHi ||
	        op.type == ShaderOperandType::Sgpr || op.type == ShaderOperandType::ExecLo || op.type == ShaderOperandType::ExecHi ||
	        op.type == ShaderOperandType::ExecZ || op.type == ShaderOperandType::Scc || op.type == ShaderOperandType::M0);
}

static bool operand_covers_vgpr(ShaderOperand op, int reg)
{
	if (op.type != ShaderOperandType::Vgpr || reg < 0)
	{
		return false;
	}

	const int size = (op.size > 0 ? op.size : 1);
	return reg >= op.register_id && reg < op.register_id + size;
}

static bool instruction_writes_vgpr(const ShaderInstruction& inst, int reg)
{
	return operand_covers_vgpr(inst.dst, reg) || operand_covers_vgpr(inst.dst2, reg);
}

static bool instruction_changes_control_flow(const ShaderInstruction& inst)
{
	switch (inst.type)
	{
		case ShaderInstructionType::SBranch:
		case ShaderInstructionType::SCbranchExecz:
		case ShaderInstructionType::SCbranchScc0:
		case ShaderInstructionType::SCbranchScc1:
		case ShaderInstructionType::SCbranchVccz:
		case ShaderInstructionType::SCbranchVccnz:
		case ShaderInstructionType::SSetpcB64:
		case ShaderInstructionType::SSwappcB64: return true;
		default: break;
	}
	return false;
}

static bool instruction_is_conditional_branch(const ShaderInstruction& inst)
{
	switch (inst.type)
	{
		case ShaderInstructionType::SCbranchExecz:
		case ShaderInstructionType::SCbranchScc0:
		case ShaderInstructionType::SCbranchScc1:
		case ShaderInstructionType::SCbranchVccz:
		case ShaderInstructionType::SCbranchVccnz: return true;
		default: return false;
	}
}

static bool loop_exit_is_in_header_block(const ShaderCode& code, const ShaderLabel& backedge, const ShaderInstruction& exit)
{
	for (const auto& label: code.GetLabels())
	{
		if (!label.IsDisabled() && label.GetDst() > backedge.GetDst() && label.GetDst() <= exit.pc)
		{
			return false;
		}
	}

	return true;
}

static String8 find_backward_loop_merge(const ShaderCode& code, const ShaderLabel& backedge)
{
	String8 merge;

	for (const auto& inst: code.GetInstructions())
	{
		if (inst.pc < backedge.GetDst() || inst.pc >= backedge.GetSrc() || !instruction_is_conditional_branch(inst))
		{
			continue;
		}

		const auto exit = ShaderLabel(inst);
		if (exit.GetDst() <= backedge.GetSrc())
		{
			continue;
		}

		EXIT_NOT_IMPLEMENTED(!loop_exit_is_in_header_block(code, backedge, inst));

		if (merge.Size() == 0)
		{
			merge = exit.ToString();
			continue;
		}

		EXIT_NOT_IMPLEMENTED(merge != exit.ToString());
	}

	return merge;
}

static uint32_t find_backward_loop_for_exit(const ShaderCode& code, const ShaderLabel& exit)
{
	uint32_t owner = 0;

	for (const auto& inst: code.GetInstructions())
	{
		if (inst.type != ShaderInstructionType::SBranch)
		{
			continue;
		}

		const auto backedge = ShaderLabel(inst);
		if (backedge.GetDst() >= backedge.GetSrc() || find_backward_loop_merge(code, backedge) != exit.ToString())
		{
			continue;
		}

		EXIT_NOT_IMPLEMENTED(owner != 0);
		owner = backedge.GetSrc();
	}

	return owner;
}

static String8 packed_half_shadow_to_str(ShaderOperand op)
{
	EXIT_NOT_IMPLEMENTED(op.type != ShaderOperandType::Vgpr || op.size != 1);
	return String8::FromPrintf("v%d_packed_half", op.register_id);
}

static SpirvValue operand_variable_to_str(ShaderOperand op)
{
	SpirvValue ret;

	EXIT_IF(op.size != 1);

	switch (op.type)
	{
		case ShaderOperandType::Vgpr:
			ret.value = String8::FromPrintf("v%d", op.register_id);
			ret.type  = SpirvType::Float;
			break;
		case ShaderOperandType::Sgpr:
			ret.value = String8::FromPrintf("s%d", op.register_id);
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::VccLo:
			ret.value = "vcc_lo";
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::VccHi:
			ret.value = "vcc_hi";
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::ExecLo:
			ret.value = "exec_lo";
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::ExecHi:
			ret.value = "exec_hi";
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::ExecZ:
			ret.value = "execz";
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::Scc:
			ret.value = "scc";
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::M0:
			ret.value = "m0";
			ret.type  = SpirvType::Uint;
			break;
		default: break;
	}

	return ret;
}

static SpirvValue operand_variable_to_str(ShaderOperand op, int shift)
{
	SpirvValue ret;

	EXIT_IF(op.size < 2 || op.size <= shift);

	switch (op.type)
	{
		case ShaderOperandType::Vgpr:
			ret.value = String8::FromPrintf("v%d", op.register_id + shift);
			ret.type  = SpirvType::Float;
			break;
		case ShaderOperandType::Sgpr:
			ret.value = String8::FromPrintf("s%d", op.register_id + shift);
			ret.type  = SpirvType::Uint;
			break;
		case ShaderOperandType::VccLo:
			if (shift == 0)
			{
				ret.value = "vcc_lo";
				ret.type  = SpirvType::Uint;
			} else if (shift == 1)
			{
				ret.value = "vcc_hi";
				ret.type  = SpirvType::Uint;
			}
			break;
		case ShaderOperandType::ExecLo:
			if (shift == 0)
			{
				ret.value = "exec_lo";
				ret.type  = SpirvType::Uint;
			} else if (shift == 1)
			{
				ret.value = "exec_hi";
				ret.type  = SpirvType::Uint;
			}
			break;
		default: break;
	}

	return ret;
}

static SpirvValue buffer_index_variable_to_str(const ShaderInstruction& inst)
{
	if (inst.format == ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen)
	{
		return operand_variable_to_str(inst.src[0]);
	}

	EXIT_NOT_IMPLEMENTED(inst.format != ShaderInstructionFormat::Vdata1Vaddr2SvSoffsOffenIdxen);
	return operand_variable_to_str(inst.src[0], 1);
}

static SpirvValue mimg_address_to_str(const ShaderInstruction& inst, int address)
{
	EXIT_IF(address < 0);
	if (inst.mimg_address_num != 0)
	{
		EXIT_IF(address >= inst.mimg_address_num);
		return operand_variable_to_str(inst.mimg_address[address]);
	}
	return operand_variable_to_str(inst.src[0], address);
}

static bool ImageSampleLzUses2dTexture(const ShaderBindResources& bind)
{
	if (bind.textures2D.textures2d_sampled_num <= 0 || bind.textures2D.textures_num <= 0)
	{
		return false;
	}

	int sampled_num = 0;
	for (int texture = 0; texture < bind.textures2D.textures_num; ++texture)
	{
		const auto& descriptor = bind.textures2D.desc[texture];
		if (descriptor.usage != ShaderTextureUsage::ReadOnly)
		{
			continue;
		}
		++sampled_num;
		if (descriptor.texture.Type() != 9u)
		{
			return false;
		}
	}

	return sampled_num == bind.textures2D.textures2d_sampled_num;
}

static void ValidateImageSampleLz2dAddresses(const ShaderInstruction& inst)
{
	if (inst.mimg_address_num == 0)
	{
		return;
	}

	// A 2D NSA image sample has two logical coordinates. Gen5 still allocates
	// one four-byte NSA word, so the remaining three byte lanes are padding.
	// Preserve those operands in ShaderInstruction, but do not consume them as
	// coordinates and reject a shape that carries additional meaningful lanes.
	EXIT_NOT_IMPLEMENTED(inst.mimg_address_num != 5);
	for (int address = 2; address < inst.mimg_address_num; ++address)
	{
		EXIT_NOT_IMPLEMENTED(inst.mimg_address[address].type != ShaderOperandType::Vgpr || inst.mimg_address[address].register_id != 0);
	}
}

static bool UsesArrayed2dImages(const ShaderBindResources* bind)
{
	if (bind == nullptr || bind->textures2D.textures_num == 0)
	{
		return false;
	}

	bool has_arrayed = false;
	bool has_flat    = false;
	for (int i = 0; i < bind->textures2D.textures_num; ++i)
	{
		if (bind->textures2D.desc[i].texture.Type() == 13u)
		{
			has_arrayed = true;
		} else
		{
			has_flat = true;
		}
	}

	// A single SPIR-V descriptor array cannot contain both Image2D and
	// Image2DArray elements. Keep this boundary explicit until bindings are
	// split by image shape.
	EXIT_NOT_IMPLEMENTED(has_arrayed && has_flat);
	return has_arrayed;
}

static bool UsesThreeDimensionalImages(const ShaderBindResources* bind)
{
	if (bind == nullptr || bind->textures2D.textures_num == 0)
	{
		return false;
	}

	bool has_3d    = false;
	bool has_other = false;
	for (int i = 0; i < bind->textures2D.textures_num; ++i)
	{
		if (bind->textures2D.desc[i].texture.Type() == 10u)
		{
			has_3d = true;
		} else
		{
			has_other = true;
		}
	}
	EXIT_NOT_IMPLEMENTED(has_3d && has_other);
	return has_3d;
}

static bool SupportsArrayed2dImageInstruction(const ShaderInstruction& inst)
{
	return (inst.type == ShaderInstructionType::ImageLoad || inst.type == ShaderInstructionType::ImageStore) &&
	       inst.format == ShaderInstructionFormat::Vdata4Vaddr3StDmaskF;
}

static bool UsesUnsignedIntegerImages(const ShaderBindResources* bind)
{
	if (bind == nullptr || bind->textures2D.textures_num == 0)
	{
		return false;
	}

	for (int i = 0; i < bind->textures2D.textures_num; ++i)
	{
		if (bind->textures2D.desc[i].texture.Format() != 20u)
		{
			return false;
		}
	}
	return true;
}

static bool IsImageInstruction(const ShaderInstruction& inst)
{
	switch (inst.type)
	{
		case ShaderInstructionType::ImageLoad:
		case ShaderInstructionType::ImageSample:
		case ShaderInstructionType::ImageSampleL:
		case ShaderInstructionType::ImageSampleLz:
		case ShaderInstructionType::ImageSampleLzO:
		case ShaderInstructionType::ImageStore:
		case ShaderInstructionType::ImageStoreMip: return true;
		default: return false;
	}
}

static bool operand_is_exec(ShaderOperand op)
{
	switch (op.type)
	{
		case ShaderOperandType::ExecLo:
		case ShaderOperandType::ExecHi:
		case ShaderOperandType::ExecZ: return true;
		default: break;
	}
	return false;
}

// SDWA SEL (GCN/RDNA): zero-extend BYTE_n / WORD_n from a uint register value.
// sel 6 (DWORD) is a no-op. Returns SPIR-V that writes <result_id> from <input_id>.
static String8 sdwa_swizzle_uint(const String8& input_id, const String8& result_id, const String8& index, uint8_t sel)
{
	if (sel == 6u)
	{
		return {};
	}
	EXIT_NOT_IMPLEMENTED(sel > 6u);

	// offset,count for OpBitFieldUExtract
	uint32_t offset = 0;
	uint32_t count  = 32;
	switch (sel)
	{
		case 0:
			offset = 0;
			count  = 8;
			break; // BYTE_0
		case 1:
			offset = 8;
			count  = 8;
			break; // BYTE_1
		case 2:
			offset = 16;
			count  = 8;
			break; // BYTE_2
		case 3:
			offset = 24;
			count  = 8;
			break; // BYTE_3
		case 4:
			offset = 0;
			count  = 16;
			break; // WORD_0
		case 5:
			offset = 16;
			count  = 16;
			break; // WORD_1
		default: break;
	}

	return String8("%<result_id> = OpBitFieldUExtract %uint %<input_id> %uint_<off> %uint_<cnt>\n")
	    .ReplaceStr("<result_id>", result_id)
	    .ReplaceStr("<input_id>", input_id)
	    .ReplaceStr("<off>", String8::FromPrintf("%u", offset))
	    .ReplaceStr("<cnt>", String8::FromPrintf("%u", count))
	    .ReplaceStr("<index>", index);
}

static bool operand_load_int(Spirv* spirv, ShaderOperand op, const String8& result_id, const String8& index, String8* load)
{
	EXIT_IF(load == nullptr);

	EXIT_NOT_IMPLEMENTED(op.negate || op.absolute);

	if (operand_is_constant(op))
	{
		String8 id = spirv->GetConstant(op);

		*load = String8("%<result_id> = OpBitcast %int %<id>")
		            .ReplaceStr("<index>", index)
		            .ReplaceStr("<id>", id)
		            .ReplaceStr("<result_id>", result_id);
	} else if (operand_is_variable(op))
	{
		auto value = operand_variable_to_str(op);

		if (value.type == SpirvType::Float)
		{
			*load = (String8("%t<result_id> = OpLoad %float %<id>\n") + String8(' ', 10) +
			         String8("%<result_id> = OpBitcast %int %t<result_id>\n"))
			            .ReplaceStr("<index>", index)
			            .ReplaceStr("<id>", value.value)
			            .ReplaceStr("<result_id>", result_id);
		} else if (value.type == SpirvType::Uint)
		{
			*load = (String8("%t<result_id> = OpLoad %uint %<id>\n") + String8(' ', 10) +
			         String8("%<result_id> = OpBitcast %int %t<result_id>\n"))
			            .ReplaceStr("<index>", index)
			            .ReplaceStr("<id>", value.value)
			            .ReplaceStr("<result_id>", result_id);
		}
	} else
	{
		return false;
	}
	return true;
}

static bool operand_load_uint(Spirv* spirv, ShaderOperand op, const String8& result_id, const String8& index, String8* load, int shift = -1)
{
	EXIT_IF(load == nullptr);

	EXIT_NOT_IMPLEMENTED(op.negate || op.absolute);

	const bool    need_swizzle = (op.swizzle != 6u);
	const String8 raw_id       = need_swizzle ? ("raw" + result_id) : result_id;

	if (operand_is_constant(op))
	{
		if (op.size == 2)
		{
			EXIT_NOT_IMPLEMENTED(shift < 0 || shift >= 2);

			if (shift == 0)
			{
				String8 id = spirv->GetConstant(op);
				*load      = String8("%<result_id> = OpBitcast %uint %<id>")
				                 .ReplaceStr("<index>", index)
				                 .ReplaceStr("<id>", id)
				                 .ReplaceStr("<result_id>", raw_id);
			} else
			{
				if (op.type == ShaderOperandType::IntegerInlineConstant && op.constant.i < 0)
				{
					*load = String8("%<result_id> = OpBitcast %uint %uint_0xffffffff")
					            .ReplaceStr("<index>", index)
					            .ReplaceStr("<result_id>", raw_id);
				} else
				{
					*load =
					    String8("%<result_id> = OpBitcast %uint %uint_0").ReplaceStr("<index>", index).ReplaceStr("<result_id>", raw_id);
				}
			}
		} else
		{
			String8 id = spirv->GetConstant(op);
			*load      = String8("%<result_id> = OpBitcast %uint %<id>")
			                 .ReplaceStr("<index>", index)
			                 .ReplaceStr("<id>", id)
			                 .ReplaceStr("<result_id>", raw_id);
		}
	} else if (operand_is_variable(op))
	{
		auto value = (shift >= 0 ? operand_variable_to_str(op, shift) : operand_variable_to_str(op));

		if (value.type == SpirvType::Float)
		{
			*load = (String8("%t<result_id> = OpLoad %float %<id>\n") + String8(' ', 10) +
			         String8("%<result_id> = OpBitcast %uint %t<result_id>\n"))
			            .ReplaceStr("<index>", index)
			            .ReplaceStr("<id>", value.value)
			            .ReplaceStr("<result_id>", raw_id);
		} else if (value.type == SpirvType::Uint)
		{
			*load = (String8("%<result_id> = OpLoad %uint %<id>"))
			            .ReplaceStr("<index>", index)
			            .ReplaceStr("<id>", value.value)
			            .ReplaceStr("<result_id>", raw_id);
		} else
		{
			return false;
		}
	} else
	{
		return false;
	}

	if (need_swizzle)
	{
		*load += String8(' ', 10) + sdwa_swizzle_uint(raw_id, result_id, index, op.swizzle);
	}
	return true;
}

static bool operand_load_float(Spirv* spirv, ShaderOperand op, const String8& result_id, const String8& index, String8* load)
{
	EXIT_IF(load == nullptr);

	String8    l;
	const bool need_swizzle = (op.swizzle != 6u);

	// SDWA BYTE/WORD selects operate on the raw 32-bit register image, then
	// the extracted uint is bitcast back to float for VGPR storage.
	if (need_swizzle)
	{
		String8 uint_load;
		if (!operand_load_uint(spirv, op, "su_" + result_id, index, &uint_load))
		{
			return false;
		}
		if (op.negate && op.absolute)
		{
			l     = uint_load + String8(' ', 10) +
			        String8("%swf_<index> = OpBitcast %float %su_<result_id>\n").ReplaceStr("<result_id>", result_id) + String8(' ', 10) +
			        String8("%abs_<index> = OpExtInst %float %GLSL_std_450 FAbs %swf_<index>\n") + String8(' ', 10) +
			        String8("%<result> = OpFNegate %float %abs_<index>\n");
			*load = l.ReplaceStr("<index>", index).ReplaceStr("<result>", result_id);
			return true;
		}
		if (op.absolute)
		{
			l     = uint_load + String8(' ', 10) +
			        String8("%swf_<index> = OpBitcast %float %su_<result_id>\n").ReplaceStr("<result_id>", result_id) + String8(' ', 10) +
			        String8("%<result> = OpExtInst %float %GLSL_std_450 FAbs %swf_<index>\n");
			*load = l.ReplaceStr("<index>", index).ReplaceStr("<result>", result_id);
			return true;
		}
		if (op.negate)
		{
			l     = uint_load + String8(' ', 10) +
			        String8("%swf_<index> = OpBitcast %float %su_<result_id>\n").ReplaceStr("<result_id>", result_id) + String8(' ', 10) +
			        String8("%<result> = OpFNegate %float %swf_<index>\n");
			*load = l.ReplaceStr("<index>", index).ReplaceStr("<result>", result_id);
			return true;
		}
		l = uint_load + String8(' ', 10) + String8("%<result> = OpBitcast %float %su_<result_id>\n").ReplaceStr("<result_id>", result_id);
		*load = l.ReplaceStr("<index>", index).ReplaceStr("<result>", result_id);
		return true;
	}

	if (operand_is_constant(op))
	{
		String8 id = spirv->GetConstant(op);

		const char* operation = (op.type == ShaderOperandType::FloatInlineConstant ? "OpCopyObject" : "OpBitcast");
		l = String8("%<result_id> = <operation> %float %<id>").ReplaceStr("<operation>", operation).ReplaceStr("<id>", id);
	} else if (operand_is_variable(op))
	{
		auto value = operand_variable_to_str(op);

		if (value.type == SpirvType::Float)
		{
			l = String8("%<result_id> = OpLoad %float %<id>\n").ReplaceStr("<id>", value.value);
		} else if (value.type == SpirvType::Uint)
		{
			l = (String8("%t<result_id> = OpLoad %uint %<id>\n") + String8(' ', 10) +
			     String8("%<result_id> = OpBitcast %float %t<result_id>\n"))
			        .ReplaceStr("<id>", value.value);
		} else
		{
			return false;
		}
	} else
	{
		return false;
	}

	if (op.negate && op.absolute)
	{
		l += String8(' ', 10) + String8("%abs_<index> = OpExtInst %float %GLSL_std_450 FAbs %<result_id>\n") + String8(' ', 10) +
		     String8("%<result> = OpFNegate %float %abs_<index>\n");

		*load = l.ReplaceStr("<index>", index).ReplaceStr("<result_id>", "a" + result_id).ReplaceStr("<result>", result_id);

		return true;
	}

	if (op.absolute)
	{
		l += String8(' ', 10) + String8("%<result> = OpExtInst %float %GLSL_std_450 FAbs %<result_id>\n");
		*load = l.ReplaceStr("<index>", index).ReplaceStr("<result_id>", "a" + result_id).ReplaceStr("<result>", result_id);
	} else if (op.negate)
	{
		l += String8(' ', 10) + String8("%<result> = OpFNegate %float %<result_id>\n");
		*load = l.ReplaceStr("<index>", index).ReplaceStr("<result_id>", "n" + result_id).ReplaceStr("<result>", result_id);
	} else
	{
		*load = l.ReplaceStr("<index>", index).ReplaceStr("<result_id>", result_id);
	}

	return true;
}

static String8 get_scc_check(SccCheck scc_check, int dst_num)
{
	EXIT_IF(dst_num < 1 || dst_num > 2);

	if (dst_num == 1)
	{
		switch (scc_check)
		{
			case SccCheck::NonZero: return SCC_NZ_1; break;
			case SccCheck::OverflowAdd: return SCC_OVERFLOW_ADD_1; break;
			case SccCheck::OverflowSub: return SCC_OVERFLOW_SUB_1; break;
			case SccCheck::CarryOut: return SCC_CARRY_1; break;
			default: break;
		}
	} else if (dst_num == 2)
	{
		switch (scc_check)
		{
			case SccCheck::NonZero: return SCC_NZ_2; break;
			case SccCheck::ExecNonZero: return SCC_EXEC_NZ_2; break;
			case SccCheck::OverflowAdd: KYTY_NOT_IMPLEMENTED; break;
			case SccCheck::OverflowSub: KYTY_NOT_IMPLEMENTED; break;
			case SccCheck::CarryOut: KYTY_NOT_IMPLEMENTED; break;
			default: break;
		}
	}
	return "";
}

// MUBUF/MTBUF soffset is stored into %temp_int_* (signed int pointers).
// GetConstant() returns %uint_N for LiteralConstant, which fails OpStore into
// %_ptr_Function_int. Always materialize the offset as an Int constant id.
// FindConstants must register the Int twin for every LiteralConstant.
static String8 GetBufferOffsetIntConstant(Spirv* spirv, ShaderOperand op)
{
	EXIT_NOT_IMPLEMENTED(!operand_is_constant(op));
	int value = 0;
	if (op.type == ShaderOperandType::IntegerInlineConstant)
	{
		value = op.constant.i;
	} else if (op.type == ShaderOperandType::LiteralConstant)
	{
		value = static_cast<int>(op.constant.u);
	} else if (op.type == ShaderOperandType::FloatInlineConstant)
	{
		// Rare: treat bit pattern as unsigned immediate.
		value = static_cast<int>(op.constant.u);
	} else
	{
		EXIT_NOT_IMPLEMENTED(true);
	}
	String8 id = spirv->GetConstantInt(value);
	EXIT_NOT_IMPLEMENTED(id == "unknown_int_constant");
	return id;
}

KYTY_RECOMPILER_FUNC(Recompile_BufferLoadUbyte_Vdata1VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value   = operand_variable_to_str(inst.dst);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
        %t110_<index> = OpFunctionCall %void %buffer_load_ubyte %temp_uint_0 %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
        %t111_<index> = OpLoad %uint %temp_uint_0
        %t112_<index> = OpBitcast %float %t111_<index>
               OpStore %<dst> %t112_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<dst>", dst_value.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_BufferLoadDword)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		const bool has_vgpr_offset = inst.format == ShaderInstructionFormat::Vdata1Vaddr2SvSoffsOffenIdxen;
		EXIT_NOT_IMPLEMENTED(!has_vgpr_offset && inst.format != ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen);

		auto dst_value   = operand_variable_to_str(inst.dst);
		auto src0_index  = buffer_index_variable_to_str(inst);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src1_value1 = operand_variable_to_str(inst.src[1], 1);
		// auto   src1_value3 = operand_variable_to_str(inst.src[1], 3);
		String8 offset    = GetBufferOffsetIntConstant(spirv, inst.src[2]);
		String8 index_str = String8::FromPrintf("%u", index);

		EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_index.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);
		// EXIT_NOT_IMPLEMENTED(src1_value3.type != SpirvType::Uint);

		String8 load_offset = R"(
               OpStore %temp_int_2 %<offset>
)";
		if (has_vgpr_offset)
		{
			auto src0_offset = operand_variable_to_str(inst.src[0], 0);
			EXIT_NOT_IMPLEMENTED(src0_offset.type != SpirvType::Float);
			load_offset = R"(
       %to100_<index> = OpLoad %float %<src0_offset>
       %to101_<index> = OpBitcast %int %to100_<index>
       %to102_<index> = OpIAdd %int %to101_<index> %<offset>
               OpStore %temp_int_2 %to102_<index>
)";
			load_offset = load_offset.ReplaceStr("<src0_offset>", src0_offset.value);
		}
		load_offset = load_offset.ReplaceStr("<offset>", offset).ReplaceStr("<index>", index_str);

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0_index>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
        <load_offset>
		;%t206_<index> = OpLoad %uint %<src1_value3>
        ;%t208_<index> = OpShiftRightLogical %uint %t206_<index> %int_12
        ;%t210_<index> = OpBitwiseAnd %uint %t208_<index> %uint_127
        ;%t211_<index> = OpBitcast %int %t210_<index>
        ;       OpStore %temp_int_5 %t211_<index>
        %t110_<index> = OpFunctionCall %void %buffer_load_float1 %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", index_str)
		                   .ReplaceStr("<src0_index>", src0_index.value)
		                   .ReplaceStr("<load_offset>", load_offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   //.ReplaceStr("<src1_value3>", src1_value3.value)
		                   .ReplaceStr("<p0>", dst_value.value);

		return true;
	}

	return false;
}

// buffer_load_dwordx2: two raw dwords at consecutive addresses (offset, offset+4).
KYTY_RECOMPILER_FUNC(Recompile_BufferLoadDwordx2_Vdata2VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(dst_value1.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
        %t110_<index> = OpFunctionCall %void %buffer_load_float1 %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
        %t200_<index> = OpLoad %int %temp_int_2
        %t201_<index> = OpIAdd %int %t200_<index> %int_4
               OpStore %temp_int_2 %t201_<index>
        %t210_<index> = OpFunctionCall %void %buffer_load_float1 %<p1> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value);

		return true;
	}

	return false;
}

// buffer_load_dwordx4: four raw dwords via existing buffer_load_float4 helper.
KYTY_RECOMPILER_FUNC(Recompile_BufferLoadDwordx4_Vdata4VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto    dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
        %t110_<index> = OpFunctionCall %void %buffer_load_float4 %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value)
		                   .ReplaceStr("<p3>", dst_value3.value);

		return true;
	}

	return false;
}

// buffer_load_dwordx3: three consecutive raw dwords via three float1 loads.
KYTY_RECOMPILER_FUNC(Recompile_BufferLoadDwordx3_Vdata3VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
        %t110_<index> = OpFunctionCall %void %buffer_load_float1 %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
        %t200_<index> = OpLoad %int %temp_int_2
        %t201_<index> = OpIAdd %int %t200_<index> %int_4
               OpStore %temp_int_2 %t201_<index>
        %t210_<index> = OpFunctionCall %void %buffer_load_float1 %<p1> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
        %t220_<index> = OpLoad %int %temp_int_2
        %t221_<index> = OpIAdd %int %t220_<index> %int_4
               OpStore %temp_int_2 %t221_<index>
        %t230_<index> = OpFunctionCall %void %buffer_load_float1 %<p2> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_BufferLoadFormatX_Vdata1VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		// EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));
		// EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value   = operand_variable_to_str(inst.dst);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		auto    src1_value3 = operand_variable_to_str(inst.src[1], 3);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value3.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
		%t206_<index> = OpLoad %uint %<src1_value3>
        %t208_<index> = OpShiftRightLogical %uint %t206_<index> %int_12
        %t210_<index> = OpBitwiseAnd %uint %t208_<index> %uint_127
        %t211_<index> = OpBitcast %int %t210_<index>
               OpStore %temp_int_5 %t211_<index>
        %t110_<index> = OpFunctionCall %void %tbuffer_load_format_x %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<src1_value3>", src1_value3.value)
		                   .ReplaceStr("<p0>", dst_value.value);

		return true;
	}

	return false;
}

// buffer_load_format_xyzw v[0:3], v4, s[0:3], 0, idxen — same addressing as
// BufferLoadFormatX but four-component destination (captured post-menu load).
KYTY_RECOMPILER_FUNC(Recompile_BufferLoadFormatXyzw_Vdata4VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto    dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		auto    src1_value3 = operand_variable_to_str(inst.src[1], 3);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value3.type != SpirvType::Uint);

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
        %t206_<index> = OpLoad %uint %<src1_value3>
        %t208_<index> = OpShiftRightLogical %uint %t206_<index> %int_12
        %t210_<index> = OpBitwiseAnd %uint %t208_<index> %uint_127
        %t211_<index> = OpBitcast %int %t210_<index>
               OpStore %temp_int_5 %t211_<index>
        %t110_<index> = OpFunctionCall %void %tbuffer_load_format_xyzw %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<src1_value3>", src1_value3.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value)
		                   .ReplaceStr("<p3>", dst_value3.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_BufferStoreDword_Vdata1VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		const bool has_vgpr_offset = inst.format == ShaderInstructionFormat::Vdata1VaddrSvSoffsOffen;
		EXIT_NOT_IMPLEMENTED(!has_vgpr_offset && inst.format != ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen);

		auto dst_value   = operand_variable_to_str(inst.dst);
		auto src0_value  = operand_variable_to_str(inst.src[0]);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src1_value1 = operand_variable_to_str(inst.src[1], 1);
		// auto   src1_value3 = operand_variable_to_str(inst.src[1], 3);
		String8 offset = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);
		// EXIT_NOT_IMPLEMENTED(src1_value3.type != SpirvType::Uint);

		// TODO() check VSKIP

		String8 address_setup = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
               OpStore %temp_int_2 %<offset>
)";
		if (has_vgpr_offset)
		{
			address_setup = R"(
               OpStore %temp_int_1 %int_0
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
        %t102_<index> = OpIAdd %int %t101_<index> %<offset>
               OpStore %temp_int_2 %t102_<index>
)";
		}

		static const char* text = R"(
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %t278_<index> None
               OpBranchConditional %exec_lo_b_<index> %t277_<index> %t278_<index>
		%t277_<index> = OpLabel

        <address_setup>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
		;%t206_<index> = OpLoad %uint %<src1_value3>
        ;%t208_<index> = OpShiftRightLogical %uint %t206_<index> %int_12
        ;%t210_<index> = OpBitwiseAnd %uint %t208_<index> %uint_127
        ;%t211_<index> = OpBitcast %int %t210_<index>
        ;       OpStore %temp_int_5 %t211_<index>
        %t110_<index> = OpFunctionCall %void %buffer_store_float1 %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4

               OpBranch %t278_<index>
        %t278_<index> = OpLabel
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<address_setup>", address_setup)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   //.ReplaceStr("<src1_value3>", src1_value3.value)
		                   .ReplaceStr("<p0>", dst_value.value);

		return true;
	}

	return false;
}

// buffer_store_dwordx2: two raw dwords via buffer_store_float2.
KYTY_RECOMPILER_FUNC(Recompile_BufferStoreDwordx2_Vdata2VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);

		static const char* text = R"(
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %t278_<index> None
               OpBranchConditional %exec_lo_b_<index> %t277_<index> %t278_<index>
		%t277_<index> = OpLabel
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
        %t110_<index> = OpFunctionCall %void %buffer_store_float2 %<p0> %<p1> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
               OpBranch %t278_<index>
        %t278_<index> = OpLabel
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value);

		return true;
	}

	return false;
}

// buffer_store_dwordx4: four raw dwords via buffer_store_float4.
KYTY_RECOMPILER_FUNC(Recompile_BufferStoreDwordx4_Vdata4VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto    dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);

		static const char* text = R"(
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %t278_<index> None
               OpBranchConditional %exec_lo_b_<index> %t277_<index> %t278_<index>
		%t277_<index> = OpLabel
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
        %t110_<index> = OpFunctionCall %void %buffer_store_float4 %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
               OpBranch %t278_<index>
        %t278_<index> = OpLabel
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value)
		                   .ReplaceStr("<p3>", dst_value3.value);

		return true;
	}

	return false;
}

// buffer_store_dwordx3: three consecutive raw dwords via three float1 stores.
KYTY_RECOMPILER_FUNC(Recompile_BufferStoreDwordx3_Vdata3VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);

		static const char* text = R"(
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %t278_<index> None
               OpBranchConditional %exec_lo_b_<index> %t277_<index> %t278_<index>
		%t277_<index> = OpLabel
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
        %t110_<index> = OpFunctionCall %void %buffer_store_float1 %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
        %t200_<index> = OpLoad %int %temp_int_2
        %t201_<index> = OpIAdd %int %t200_<index> %int_4
               OpStore %temp_int_2 %t201_<index>
        %t210_<index> = OpFunctionCall %void %buffer_store_float1 %<p1> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
        %t220_<index> = OpLoad %int %temp_int_2
        %t221_<index> = OpIAdd %int %t220_<index> %int_4
               OpStore %temp_int_2 %t221_<index>
        %t230_<index> = OpFunctionCall %void %buffer_store_float1 %<p2> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4
               OpBranch %t278_<index>
        %t278_<index> = OpLabel
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_BufferStoreFormatX_Vdata1VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value   = operand_variable_to_str(inst.dst);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		auto    src1_value3 = operand_variable_to_str(inst.src[1], 3);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value3.type != SpirvType::Uint);

		// TODO() check VSKIP

		static const char* text = R"(
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %t278_<index> None
               OpBranchConditional %exec_lo_b_<index> %t277_<index> %t278_<index>
		%t277_<index> = OpLabel

        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
		%t206_<index> = OpLoad %uint %<src1_value3>
        %t208_<index> = OpShiftRightLogical %uint %t206_<index> %int_12
        %t210_<index> = OpBitwiseAnd %uint %t208_<index> %uint_127
        %t211_<index> = OpBitcast %int %t210_<index>
               OpStore %temp_int_5 %t211_<index>
        %t110_<index> = OpFunctionCall %void %tbuffer_store_format_x %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5

               OpBranch %t278_<index>
        %t278_<index> = OpLabel
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<src1_value3>", src1_value3.value)
		                   .ReplaceStr("<p0>", dst_value.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_BufferStoreFormatXy_Vdata2VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		auto    src1_value3 = operand_variable_to_str(inst.src[1], 3);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value3.type != SpirvType::Uint);

		// TODO() check VSKIP

		static const char* text = R"(
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %t278_<index> None
               OpBranchConditional %exec_lo_b_<index> %t277_<index> %t278_<index>
		%t277_<index> = OpLabel

        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
		%t206_<index> = OpLoad %uint %<src1_value3>
        %t208_<index> = OpShiftRightLogical %uint %t206_<index> %int_12
        %t210_<index> = OpBitwiseAnd %uint %t208_<index> %uint_127
        %t211_<index> = OpBitcast %int %t210_<index>
               OpStore %temp_int_5 %t211_<index>
        %t110_<index> = OpFunctionCall %void %tbuffer_store_format_xy %<p0> %<p1> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5

               OpBranch %t278_<index>
        %t278_<index> = OpLabel
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<src1_value3>", src1_value3.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_BufferStoreFormatXyzw_Vdata4VaddrSvSoffsIdxen)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto    dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		auto    src1_value3 = operand_variable_to_str(inst.src[1], 3);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(dst_value1.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(dst_value2.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(dst_value3.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value3.type != SpirvType::Uint);

		static const char* text = R"(
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %t278_<index> None
               OpBranchConditional %exec_lo_b_<index> %t277_<index> %t278_<index>
		%t277_<index> = OpLabel

        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
		%t206_<index> = OpLoad %uint %<src1_value3>
        %t208_<index> = OpShiftRightLogical %uint %t206_<index> %int_12
        %t210_<index> = OpBitwiseAnd %uint %t208_<index> %uint_127
        %t211_<index> = OpBitcast %int %t210_<index>
               OpStore %temp_int_5 %t211_<index>
        %t110_<index> = OpFunctionCall %void %tbuffer_store_format_xyzw %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5

               OpBranch %t278_<index>
        %t278_<index> = OpLabel
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<src1_value3>", src1_value3.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value)
		                   .ReplaceStr("<p3>", dst_value3.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_DsAppend_VdstGds)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->gds_pointers.pointers_num > 0)
	{
		String8 index_str = String8::FromPrintf("%u", index);

		EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

		auto dst_value = operand_variable_to_str(inst.dst);

		EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t192_<index> = OpLoad %uint %m0
        %t194_<index> = OpShiftRightLogical %uint %t192_<index> %int_16
        %t196_<index> = OpAccessChain %_ptr_StorageBuffer_uint %gds %int_0 %t194_<index>
        %t198_<index> = OpAtomicIAdd %uint %t196_<index> %uint_1 %uint_0 %uint_1
        %t199_<index> = OpBitcast %float %t198_<index>
               OpStore %<dst> %t199_<index>
               OpMemoryBarrier %uint_1 %uint_72
)";
		*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<index>", index_str);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_DsConsume_VdstGds)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->gds_pointers.pointers_num > 0)
	{
		String8 index_str = String8::FromPrintf("%u", index);

		EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

		auto dst_value = operand_variable_to_str(inst.dst);

		EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t192_<index> = OpLoad %uint %m0
        %t194_<index> = OpShiftRightLogical %uint %t192_<index> %int_16
        %t196_<index> = OpAccessChain %_ptr_StorageBuffer_uint %gds %int_0 %t194_<index>
        %t198_<index> = OpAtomicISub %uint %t196_<index> %uint_1 %uint_0 %uint_1
        %t199_<index> = OpBitcast %float %t198_<index>
               OpStore %<dst> %t199_<index>
               OpMemoryBarrier %uint_1 %uint_72
)";
		*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<index>", index_str);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_DsWriteB32_VaddrVdataOffset)
{
	const auto& inst = code.GetInstructions().At(index);
	const auto* input_info = spirv->GetCsInputInfo();

	if (input_info == nullptr || input_info->lds_dwords == 0)
	{
		return false;
	}

	auto address = operand_variable_to_str(inst.src[0]);
	auto data    = operand_variable_to_str(inst.src[1]);

	EXIT_NOT_IMPLEMENTED(address.type != SpirvType::Float);
	EXIT_NOT_IMPLEMENTED(data.type != SpirvType::Float);

	const auto index_str  = String8::FromPrintf("%u", index);
	const auto offset_str = spirv->GetConstantUint(inst.ds_offset);

	static const char* text = R"(
        %lds_addr_f_<index> = OpLoad %float %<address>
        %lds_addr_u_<index> = OpBitcast %uint %lds_addr_f_<index>
        %lds_byte_addr_<index> = OpIAdd %uint %lds_addr_u_<index> %<offset>
        %lds_index_<index> = OpShiftRightLogical %uint %lds_byte_addr_<index> %uint_2
        %lds_ptr_<index> = OpAccessChain %_ptr_Workgroup_uint %lds %lds_index_<index>
        %lds_data_f_<index> = OpLoad %float %<data>
        %lds_data_u_<index> = OpBitcast %uint %lds_data_f_<index>
               OpStore %lds_ptr_<index> %lds_data_u_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<index>", index_str)
	                   .ReplaceStr("<address>", address.value)
	                   .ReplaceStr("<data>", data.value)
	                   .ReplaceStr("<offset>", offset_str);
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_DsRead2B32_Vdst2VaddrOffset01)
{
	const auto& inst       = code.GetInstructions().At(index);
	const auto* input_info = spirv->GetCsInputInfo();

	if (input_info == nullptr || input_info->lds_dwords == 0)
	{
		return false;
	}

	auto address = operand_variable_to_str(inst.src[0]);
	auto dst0    = operand_variable_to_str(inst.dst, 0);
	auto dst1    = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(address.type != SpirvType::Float);
	EXIT_NOT_IMPLEMENTED(dst0.type != SpirvType::Float);
	EXIT_NOT_IMPLEMENTED(dst1.type != SpirvType::Float);

	const uint32_t offset0       = inst.ds_offset & 0xffu;
	const uint32_t offset1       = (inst.ds_offset >> 8u) & 0xffu;
	const auto     index_str     = String8::FromPrintf("%u", index);
	const auto     offset0_bytes = spirv->GetConstantUint(offset0 * 4u);
	const auto     offset1_bytes = spirv->GetConstantUint(offset1 * 4u);

	// Same Workgroup byte-addressed storage as ds_write_b32; read2 offsets are
	// dword-scaled, so convert to byte offsets before the shared >>2 index path.
	static const char* text = R"(
        %lds_addr_f_<index> = OpLoad %float %<address>
        %lds_addr_u_<index> = OpBitcast %uint %lds_addr_f_<index>
        %lds_byte_addr0_<index> = OpIAdd %uint %lds_addr_u_<index> %<offset0>
        %lds_index0_<index> = OpShiftRightLogical %uint %lds_byte_addr0_<index> %uint_2
        %lds_ptr0_<index> = OpAccessChain %_ptr_Workgroup_uint %lds %lds_index0_<index>
        %lds_data0_u_<index> = OpLoad %uint %lds_ptr0_<index>
        %lds_data0_f_<index> = OpBitcast %float %lds_data0_u_<index>
               OpStore %<dst0> %lds_data0_f_<index>
        %lds_byte_addr1_<index> = OpIAdd %uint %lds_addr_u_<index> %<offset1>
        %lds_index1_<index> = OpShiftRightLogical %uint %lds_byte_addr1_<index> %uint_2
        %lds_ptr1_<index> = OpAccessChain %_ptr_Workgroup_uint %lds %lds_index1_<index>
        %lds_data1_u_<index> = OpLoad %uint %lds_ptr1_<index>
        %lds_data1_f_<index> = OpBitcast %float %lds_data1_u_<index>
               OpStore %<dst1> %lds_data1_f_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<index>", index_str)
	                   .ReplaceStr("<address>", address.value)
	                   .ReplaceStr("<dst0>", dst0.value)
	                   .ReplaceStr("<dst1>", dst1.value)
	                   .ReplaceStr("<offset0>", offset0_bytes)
	                   .ReplaceStr("<offset1>", offset1_bytes);
	return true;
}

static constexpr uint32_t SPIRV_SCOPE_WORKGROUP          = 2;
static constexpr uint32_t SPIRV_ACQUIRE_RELEASE          = 0x8;
static constexpr uint32_t SPIRV_WORKGROUP_MEMORY         = 0x100;
static constexpr uint32_t SPIRV_WORKGROUP_MEMORY_ACQ_REL = SPIRV_ACQUIRE_RELEASE | SPIRV_WORKGROUP_MEMORY;

KYTY_RECOMPILER_FUNC(Recompile_SBarrier_Empty)
{
	if (spirv->GetCsInputInfo() == nullptr)
	{
		return false;
	}

	const auto execution_scope = spirv->GetConstantUint(SPIRV_SCOPE_WORKGROUP);
	const auto memory_scope    = spirv->GetConstantUint(SPIRV_SCOPE_WORKGROUP);
	const auto semantics       = spirv->GetConstantUint(SPIRV_WORKGROUP_MEMORY_ACQ_REL);

	*dst_source += String8::FromPrintf("               OpControlBarrier %%%s %%%s %%%s\n", execution_scope.c_str(),
	                                  memory_scope.c_str(), semantics.c_str());
	return true;
}

static uint32_t null_mrt_target(ShaderInstructionFormat::Format format)
{
	switch (format)
	{
		case ShaderInstructionFormat::Mrt0OffOffComprVmDone: return 0;
		case ShaderInstructionFormat::Mrt1OffOffComprVmDone: return 1;
		case ShaderInstructionFormat::Mrt2OffOffComprVmDone: return 2;
		case ShaderInstructionFormat::Mrt3OffOffComprVmDone: return 3;
		default: EXIT("not a null MRT done format");
	}
	return 0;
}

// Null MRT done exports are no-ops unless they form the captured discard tail:
// exec=0; EXP MRTn null/done; endpgm. The target number does not change the
// discard semantics.
KYTY_RECOMPILER_FUNC(Recompile_Exp_MrtNullDone)
{
	const auto& inst = code.GetInstructions().At(index);
	EXIT_NOT_IMPLEMENTED(!ShaderIsNullMrtDoneFormat(inst.format));
	EXIT_NOT_IMPLEMENTED(inst.src_num > 0);

	if (index > 0 && index + 1 < code.GetInstructions().Size())
	{
		const auto& prev_inst = code.GetInstructions().At(index - 1);
		if (code.ReadBlock(prev_inst.pc).is_discard)
		{
			const auto* info   = spirv->GetPsInputInfo();
			const auto  target = null_mrt_target(inst.format);
			EXIT_NOT_IMPLEMENTED(info == nullptr || !info->ps_pixel_kill_enable);
			EXIT_NOT_IMPLEMENTED(info->target_output_mode[target] != 4);
			*dst_source += "        OpKill\n";
			return true;
		}
	}

	// MRT0 null/done has only been evidenced as a discard tail. Keep unsupported
	// standalone MRT0 behavior strict; MRT1-3 remain captured no-op terminators.
	if (inst.format == ShaderInstructionFormat::Mrt0OffOffComprVmDone)
	{
		return false;
	}
	return true;
}

// Compressed half2 MRT export → Location <mrt>. param[0] is the SPIR-V output
// variable name (outColor, outColor1, …).
KYTY_RECOMPILER_FUNC(Recompile_Exp_Mrt_Compr_Vsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);
	const auto* info = spirv->GetPsInputInfo();
	EXIT_NOT_IMPLEMENTED(info == nullptr);
	EXIT_NOT_IMPLEMENTED(param[0] == nullptr);

	// MRT index from trailing digit of outColor / outColorN.
	int mrt = 0;
	if (param[0][0] != '\0' && strcmp(param[0], "outColor") != 0)
	{
		const char* p = param[0];
		while (*p != '\0' && (*p < '0' || *p > '9'))
		{
			p++;
		}
		EXIT_NOT_IMPLEMENTED(*p == '\0');
		mrt = *p - '0';
	}
	EXIT_NOT_IMPLEMENTED(mrt < 0 || mrt > 7);
	EXIT_NOT_IMPLEMENTED(info->target_output_mode[mrt] != 4);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[1]));

	auto src0_value = operand_variable_to_str(inst.src[0]);
	auto src1_value = operand_variable_to_str(inst.src[1]);

	// TODO() check VSKIP
	// TODO() check EXEC

	const auto index_str = String8::FromPrintf("%u", index);
	String8    load_src0;
	String8    load_src1;
	if (spirv->CanLoadPackedHalfForExport(index, inst.src[0]))
	{
		load_src0 = String8("%t2_<index> = OpLoad %uint %<src0_packed>")
		                .ReplaceStr("<index>", index_str)
		                .ReplaceStr("<src0_packed>", packed_half_shadow_to_str(inst.src[0]));
	} else
	{
		load_src0 =
		    (String8("%t1_<index> = OpLoad %float %<src0>\n") + String8(' ', 9) + String8("%t2_<index> = OpBitcast %uint %t1_<index>"))
		        .ReplaceStr("<index>", index_str)
		        .ReplaceStr("<src0>", src0_value.value);
	}
	if (spirv->CanLoadPackedHalfForExport(index, inst.src[1]))
	{
		load_src1 = String8("%t7_<index> = OpLoad %uint %<src1_packed>")
		                .ReplaceStr("<index>", index_str)
		                .ReplaceStr("<src1_packed>", packed_half_shadow_to_str(inst.src[1]));
	} else
	{
		load_src1 =
		    (String8("%t6_<index> = OpLoad %float %<src1>\n") + String8(' ', 9) + String8("%t7_<index> = OpBitcast %uint %t6_<index>"))
		        .ReplaceStr("<index>", index_str)
		        .ReplaceStr("<src1>", src1_value.value);
	}

	static const char* text           = R"(
         %exp_exec_u_<index> = OpLoad %uint %exec_lo
         %exp_exec_b_<index> = OpINotEqual %bool %exp_exec_u_<index> %uint_0
               OpSelectionMerge %exp_merge_<index> None
               OpBranchConditional %exp_exec_b_<index> %exp_store_<index> %exp_kill_<index>
         %exp_kill_<index> = OpLabel
               OpKill
         %exp_store_<index> = OpLabel
         <load_src0>
         %t3_<index> = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %t2_<index>
         %t4_<index> = OpCompositeExtract %float %t3_<index> 0
         %t5_<index> = OpCompositeExtract %float %t3_<index> 1
         <load_src1>
		 %t8_<index> = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %t7_<index>
		 %t9_<index> = OpCompositeExtract %float %t8_<index> 0
		 %t10_<index> = OpCompositeExtract %float %t8_<index> 1
		 <export_value>
		       OpStore %<mrt> %t11_<index>
               OpBranch %exp_merge_<index>
         %exp_merge_<index> = OpLabel
)";
	const uint32_t     component0     = ShaderColorExportSourceComponent(info->target_output_order[mrt], 0);
	const uint32_t     component1     = ShaderColorExportSourceComponent(info->target_output_order[mrt], 1);
	const uint32_t     component2     = ShaderColorExportSourceComponent(info->target_output_order[mrt], 2);
	const uint32_t     component3     = ShaderColorExportSourceComponent(info->target_output_order[mrt], 3);
	const char*        source_names[] = {"t4", "t5", "t9", "t10"};
	const String8      export_value =
	    String8::FromPrintf("%%t11_<index> = OpCompositeConstruct %%v4float %%%s_<index> %%%s_<index> %%%s_<index> %%%s_<index>",
	                        source_names[component0], source_names[component1], source_names[component2], source_names[component3]);

	*dst_source += String8(text)
	                   .ReplaceStr("<export_value>", export_value)
	                   .ReplaceStr("<index>", index_str)
	                   .ReplaceStr("<load_src0>", load_src0)
	                   .ReplaceStr("<load_src1>", load_src1)
	                   .ReplaceStr("<src0>", src0_value.value)
	                   .ReplaceStr("<src1>", src1_value.value)
	                   .ReplaceStr("<mrt>", param[0]);

	return true;
}

// Full float32 MRT export → Location <mrt>.
KYTY_RECOMPILER_FUNC(Recompile_Exp_Mrt_Full_Vsrc0Vsrc1Vsrc2Vsrc3)
{
	const auto& inst = code.GetInstructions().At(index);
	const auto* info = spirv->GetPsInputInfo();
	EXIT_NOT_IMPLEMENTED(info == nullptr);
	EXIT_NOT_IMPLEMENTED(param[0] == nullptr);

	int mrt = 0;
	if (strcmp(param[0], "outColor") != 0)
	{
		const char* p = param[0];
		while (*p != '\0' && (*p < '0' || *p > '9'))
		{
			p++;
		}
		EXIT_NOT_IMPLEMENTED(*p == '\0');
		mrt = *p - '0';
	}
	EXIT_NOT_IMPLEMENTED(mrt < 0 || mrt > 7);
	EXIT_NOT_IMPLEMENTED(info->target_output_mode[mrt] != 9);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[1]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[2]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[3]));

	auto src0_value = operand_variable_to_str(inst.src[0]);
	auto src1_value = operand_variable_to_str(inst.src[1]);
	auto src2_value = operand_variable_to_str(inst.src[2]);
	auto src3_value = operand_variable_to_str(inst.src[3]);

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text           = R"(
         %exp_exec_u_<index> = OpLoad %uint %exec_lo
         %exp_exec_b_<index> = OpINotEqual %bool %exp_exec_u_<index> %uint_0
               OpSelectionMerge %exp_merge_<index> None
               OpBranchConditional %exp_exec_b_<index> %exp_store_<index> %exp_kill_<index>
         %exp_kill_<index> = OpLabel
               OpKill
         %exp_store_<index> = OpLabel
         %t0_<index> = OpLoad %float %<src0>
		 %t1_<index> = OpLoad %float %<src1>
		 %t2_<index> = OpLoad %float %<src2>
		 %t3_<index> = OpLoad %float %<src3>
		 <export_value>
		       OpStore %<mrt> %t11_<index>
               OpBranch %exp_merge_<index>
         %exp_merge_<index> = OpLabel
)";
	const uint32_t     component0     = ShaderColorExportSourceComponent(info->target_output_order[mrt], 0);
	const uint32_t     component1     = ShaderColorExportSourceComponent(info->target_output_order[mrt], 1);
	const uint32_t     component2     = ShaderColorExportSourceComponent(info->target_output_order[mrt], 2);
	const uint32_t     component3     = ShaderColorExportSourceComponent(info->target_output_order[mrt], 3);
	const char*        source_names[] = {"t0", "t1", "t2", "t3"};
	const String8      export_value =
	    String8::FromPrintf("%%t11_<index> = OpCompositeConstruct %%v4float %%%s_<index> %%%s_<index> %%%s_<index> %%%s_<index>",
	                        source_names[component0], source_names[component1], source_names[component2], source_names[component3]);

	*dst_source += String8(text)
	                   .ReplaceStr("<export_value>", export_value)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<src0>", src0_value.value)
	                   .ReplaceStr("<src1>", src1_value.value)
	                   .ReplaceStr("<src2>", src2_value.value)
	                   .ReplaceStr("<src3>", src3_value.value)
	                   .ReplaceStr("<mrt>", param[0]);

	return true;
}

/* XXX: 0, 1, 2, 3, 4 */
KYTY_RECOMPILER_FUNC(Recompile_Exp_Param_XXX_Vsrc0Vsrc1Vsrc2Vsrc3)
{
	const auto& inst = code.GetInstructions().At(index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[1]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[2]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[3]));

	auto src0_value = operand_variable_to_str(inst.src[0]);
	auto src1_value = operand_variable_to_str(inst.src[1]);
	auto src2_value = operand_variable_to_str(inst.src[2]);
	auto src3_value = operand_variable_to_str(inst.src[3]);

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
         %t0_<index> = OpLoad %float %<src0>
         %t1_<index> = OpLoad %float %<src1>
         %t2_<index> = OpLoad %float %<src2>
         %t3_<index> = OpLoad %float %<src3>
         %t4_<index> = OpCompositeConstruct %v4float %t0_<index> %t1_<index> %t2_<index> %t3_<index>
               OpStore %<param> %t4_<index>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<src0>", src0_value.value)
	                   .ReplaceStr("<src1>", src1_value.value)
	                   .ReplaceStr("<src2>", src2_value.value)
	                   .ReplaceStr("<src3>", src3_value.value)
	                   .ReplaceStr("<param>", param[0]);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_Exp_Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done)
{
	const auto& inst = code.GetInstructions().At(index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[1]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[2]));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[3]));

	auto src0_value = operand_variable_to_str(inst.src[0]);
	auto src1_value = operand_variable_to_str(inst.src[1]);
	auto src2_value = operand_variable_to_str(inst.src[2]);
	auto src3_value = operand_variable_to_str(inst.src[3]);

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
         %t0_<index> = OpLoad %float %<src0>
         %t1_<index> = OpLoad %float %<src1>
         %t2_<index> = OpLoad %float %<src2>
         %t3_<index> = OpLoad %float %<src3>
         %t4_<index> = OpCompositeConstruct %v4float %t0_<index> %t1_<index> %t2_<index> %t3_<index>
         %t5_<index> = OpAccessChain %_ptr_Output_v4float %outPerVertex %int_per_vertex_0
               OpStore %t5_<index> %t4_<index>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<src0>", src0_value.value)
	                   .ReplaceStr("<src1>", src1_value.value)
	                   .ReplaceStr("<src2>", src2_value.value)
	                   .ReplaceStr("<src3>", src3_value.value);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_Exp_PrimVsrc0OffOffOffDone)
{
	const auto& inst    = code.GetInstructions().At(index);
	const auto* vs_info = spirv->GetVsInputInfo();

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));

	return (vs_info != nullptr && vs_info->gs_prolog);
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata1Vaddr3StSsDmask1)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata1Vaddr3StSsDmask2)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// dmask 0x2 → sample and keep G (component 1).
		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata1Vaddr3StSsDmask4)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// dmask 0x4 → sample and keep B (component 2).
		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata1Vaddr3StSsDmask8)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata2Vaddr3StSsDmask3)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value1> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata2Vaddr3StSsDmask5)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value1> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata2Vaddr3StSsDmask9)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value1> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata2Vaddr3StSsDmaskA)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// dmask 0xa -> G+A, stored compactly into vdata[0:1].
		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value1> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata3Vaddr3StSsDmask7)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	// const auto& bind_params = spirv->GetBindParams();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value);

		return true;
	}

	return false;
}

// dmask 0xb: R+G+A → store sample components 0,1,3 into three VGPRs.
KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata3Vaddr3StSsDmaskB)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSampleLz_Vdata1Vaddr3StSsDmask1)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && ImageSampleLzUses2dTexture(*bind_info) && bind_info->samplers.samplers_num > 0)
	{
		ValidateImageSampleLz2dAddresses(inst);

		auto dst_value0  = operand_variable_to_str(inst.dst);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value1.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleExplicitLod %v4float %t38_<index> %t42_<index> Lod %float_0_000000
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSampleLz_Vdata2Vaddr3StSsDmask3)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info == nullptr || !ImageSampleLzUses2dTexture(*bind_info) || bind_info->samplers.samplers_num <= 0)
	{
		return false;
	}

	ValidateImageSampleLz2dAddresses(inst);

	const auto dst_value0  = operand_variable_to_str(inst.dst, 0);
	const auto dst_value1  = operand_variable_to_str(inst.dst, 1);
	const auto src0_value0 = mimg_address_to_str(inst, 0);
	const auto src0_value1 = mimg_address_to_str(inst, 1);
	const auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
	const auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float || dst_value1.type != SpirvType::Float);
	EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float || src0_value1.type != SpirvType::Float);
	EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint || src2_value0.type != SpirvType::Uint);

	static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleExplicitLod %v4float %t38_<index> %t42_<index> Lod %float_0_000000
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value1> %t55_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<src0_value0>", src0_value0.value)
	                   .ReplaceStr("<src0_value1>", src0_value1.value)
	                   .ReplaceStr("<src1_value0>", src1_value0.value)
	                   .ReplaceStr("<src2_value0>", src2_value0.value)
	                   .ReplaceStr("<dst_value0>", dst_value0.value)
	                   .ReplaceStr("<dst_value1>", dst_value1.value);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSampleLz_Vdata3Vaddr3StSsDmask7)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>

         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>

         %t43_<index> = OpImageSampleExplicitLod %v4float %t38_<index> %t42_<index> Lod %float_0_000000
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSampleLzO_Vdata3Vaddr4StSsDmask7)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src0_value3 = mimg_address_to_str(inst, 3);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>

         %t39_<index> = OpLoad %float %<src0_value1>
         %t40_<index> = OpLoad %float %<src0_value2>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>

         %90_<index> = OpLoad %float %<src0_value0>
         %91_<index> = OpBitcast %int %90_<index>
         %98_<index> = OpBitFieldSExtract %int %91_<index> %int_0 %int_6
        %101_<index> = OpBitFieldSExtract %int %91_<index> %int_8 %int_6
        %102_<index> = OpCompositeConstruct %v2int %98_<index> %101_<index>

         %130_<index> = OpConvertSToF %v2float %102_<index>
         %138_<index> = OpImage %ImageS %t38_<index>
        %139_<index> = OpImageQuerySizeLod %v2int %138_<index> %int_0
        %140_<index> = OpConvertSToF %v2float %139_<index>
        %141_<index> = OpFDiv %v2float %130_<index> %140_<index>
        %142_<index> = OpFAdd %v2float %t42_<index> %141_<index>

         %t43_<index> = OpImageSampleExplicitLod %v4float %t38_<index> %142_<index> Lod %float_0_000000
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src0_value3>", src0_value3.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSample_Vdata4Vaddr3StSsDmaskF)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	// const auto& bind_params = spirv->GetBindParams();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleImplicitLod %v4float %t38_<index> %t42_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
         %t57_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t58_<index> = OpLoad %float %t57_<index>
               OpStore %<dst_value3> %t58_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value)
		                   .ReplaceStr("<dst_value3>", dst_value3.value);

		return true;
	}

	return false;
}

// image_sample_lz dmask 0xf: same as image_sample RGBA but LOD forced to 0.
KYTY_RECOMPILER_FUNC(Recompile_ImageSampleLz_Vdata4Vaddr3StSsDmaskF)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0 && bind_info->samplers.samplers_num > 0)
	{
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src2_value0.type != SpirvType::Uint);

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleExplicitLod %v4float %t38_<index> %t42_<index> Lod %float_0_000000
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
         %t57_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t58_<index> = OpLoad %float %t57_<index>
               OpStore %<dst_value3> %t58_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src2_value0>", src2_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value)
		                   .ReplaceStr("<dst_value3>", dst_value3.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageSampleL_Vdata4Vaddr3StSsDmaskF)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info == nullptr || bind_info->textures2D.textures2d_sampled_num <= 0 || bind_info->samplers.samplers_num <= 0)
	{
		return false;
	}

	const auto dst_value0  = operand_variable_to_str(inst.dst, 0);
	const auto dst_value1  = operand_variable_to_str(inst.dst, 1);
	const auto dst_value2  = operand_variable_to_str(inst.dst, 2);
	const auto dst_value3  = operand_variable_to_str(inst.dst, 3);
	const auto src0_value0 = mimg_address_to_str(inst, 0);
	const auto src0_value1 = mimg_address_to_str(inst, 1);
	const auto src0_value2 = mimg_address_to_str(inst, 2);
	const auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
	const auto src2_value0 = operand_variable_to_str(inst.src[2], 0);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float || dst_value1.type != SpirvType::Float ||
	                     dst_value2.type != SpirvType::Float || dst_value3.type != SpirvType::Float);
	EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float || src0_value1.type != SpirvType::Float ||
	                     src0_value2.type != SpirvType::Float);
	EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint || src2_value0.type != SpirvType::Uint);

	static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t33_<index> = OpLoad %uint %<src2_value0>
         %t35_<index> = OpAccessChain %_ptr_UniformConstant_Sampler %samplers %t33_<index>
         %t36_<index> = OpLoad %Sampler %t35_<index>
         %t38_<index> = OpSampledImage %SampledImage %t27_<index> %t36_<index>
         %t39_<index> = OpLoad %float %<src0_value0>
         %t40_<index> = OpLoad %float %<src0_value1>
         %t41_<index> = OpLoad %float %<src0_value2>
         %t42_<index> = OpCompositeConstruct %v2float %t39_<index> %t40_<index>
         %t43_<index> = OpImageSampleExplicitLod %v4float %t38_<index> %t42_<index> Lod %t41_<index>
               OpStore %temp_v4float %t43_<index>
         %t46_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_0
         %t47_<index> = OpLoad %float %t46_<index>
               OpStore %<dst_value0> %t47_<index>
         %t50_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_1
         %t51_<index> = OpLoad %float %t50_<index>
               OpStore %<dst_value1> %t51_<index>
         %t54_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_2
         %t55_<index> = OpLoad %float %t54_<index>
               OpStore %<dst_value2> %t55_<index>
         %t57_<index> = OpAccessChain %_ptr_Function_float %temp_v4float %uint_3
         %t58_<index> = OpLoad %float %t57_<index>
               OpStore %<dst_value3> %t58_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<src0_value0>", src0_value0.value)
	                   .ReplaceStr("<src0_value1>", src0_value1.value)
	                   .ReplaceStr("<src0_value2>", src0_value2.value)
	                   .ReplaceStr("<src1_value0>", src1_value0.value)
	                   .ReplaceStr("<src2_value0>", src2_value0.value)
	                   .ReplaceStr("<dst_value0>", dst_value0.value)
	                   .ReplaceStr("<dst_value1>", dst_value1.value)
	                   .ReplaceStr("<dst_value2>", dst_value2.value)
	                   .ReplaceStr("<dst_value3>", dst_value3.value);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageLoad_Vdata4Vaddr3StDmaskF)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	// const auto& bind_params = spirv->GetBindParams();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_sampled_num > 0)
	{
		const bool arrayed = UsesArrayed2dImages(bind_info);
		const bool three_dimensional = UsesThreeDimensionalImages(bind_info);
		const bool uint_images = UsesUnsignedIntegerImages(bind_info);
		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);
		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED
		// TODO() swizzle channels
		// TODO() convert SRGB -> LINEAR if SRGB format was replaced with UNORM

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t67_<index> = OpLoad %float %<src0_value0>
         %t69_<index> = OpBitcast %uint %t67_<index>
         %t70_<index> = OpLoad %float %<src0_value1>
         %t71_<index> = OpBitcast %uint %t70_<index>
<array_coordinate_load>         %t73_<index> = OpCompositeConstruct %<coordinate_type> %t69_<index> %t71_<index><array_coordinate_value>
         %t74_<index> = OpImageFetch %<image_vector> %t27_<index> %t73_<index>
         %t741_<index> = OpCompositeExtract %<image_scalar> %t74_<index> 0
         %t47_<index> = <scalar_to_float> %float %t741_<index>
               OpStore %<dst_value0> %t47_<index>
         %t751_<index> = OpCompositeExtract %<image_scalar> %t74_<index> 1
         %t51_<index> = <scalar_to_float> %float %t751_<index>
               OpStore %<dst_value1> %t51_<index>
         %t761_<index> = OpCompositeExtract %<image_scalar> %t74_<index> 2
         %t55_<index> = <scalar_to_float> %float %t761_<index>
               OpStore %<dst_value2> %t55_<index>
         %t771_<index> = OpCompositeExtract %<image_scalar> %t74_<index> 3
         %t58_<index> = <scalar_to_float> %float %t771_<index>
               OpStore %<dst_value3> %t58_<index>
)";
		const String8 index_string = String8::FromPrintf("%u", index);
		const String8 array_coordinate_load =
		    ((arrayed || three_dimensional) ? String8("         %t72_<index> = OpLoad %float %<src0_value2>\n         %t721_<index> = OpBitcast %uint %t72_<index>\n")
		                   .ReplaceStr("<index>", index_string)
		                   .ReplaceStr("<src0_value2>", src0_value2.value) :
		               String8(""));
		*dst_source += String8(text)
		                   .ReplaceStr("<array_coordinate_load>", array_coordinate_load)
		                   .ReplaceStr("<coordinate_type>", (arrayed || three_dimensional) ? "v3uint" : "v2uint")
		                   .ReplaceStr("<array_coordinate_value>", (arrayed || three_dimensional) ? String8::FromPrintf(" %%t721_%u", index) : String8(""))
		                   .ReplaceStr("<image_scalar>", uint_images ? "uint" : "float")
		                   .ReplaceStr("<image_vector>", uint_images ? "v4uint" : "v4float")
		                   .ReplaceStr("<scalar_to_float>", uint_images ? "OpBitcast" : "OpCopyObject")
		                   .ReplaceStr("<index>", index_string)
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value)
		                   .ReplaceStr("<dst_value3>", dst_value3.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageLoad_Vdata1Vaddr3StDmask1)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info == nullptr || bind_info->textures2D.textures2d_sampled_num == 0)
	{
		return false;
	}

	auto dst_value0  = operand_variable_to_str(inst.dst);
	auto src0_value0 = mimg_address_to_str(inst, 0);
	auto src0_value1 = mimg_address_to_str(inst, 1);
	auto src1_value0 = operand_variable_to_str(inst.src[1], 0);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
	EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
	EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);

	static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageS %textures2D_S %t24_<index>
         %t27_<index> = OpLoad %ImageS %t26_<index>
         %t67_<index> = OpLoad %float %<src0_value0>
         %t69_<index> = OpBitcast %uint %t67_<index>
         %t70_<index> = OpLoad %float %<src0_value1>
         %t71_<index> = OpBitcast %uint %t70_<index>
         %t73_<index> = OpCompositeConstruct %v2uint %t69_<index> %t71_<index>
         %t74_<index> = OpImageFetch %v4float %t27_<index> %t73_<index>
         %t46_<index> = OpCompositeExtract %float %t74_<index> 0
               OpStore %<dst_value0> %t46_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<src0_value0>", src0_value0.value)
	                   .ReplaceStr("<src0_value1>", src0_value1.value)
	                   .ReplaceStr("<src1_value0>", src1_value0.value)
	                   .ReplaceStr("<dst_value0>", dst_value0.value);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageStore_Vdata4Vaddr3StDmaskF)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	// const auto& bind_params = spirv->GetBindParams();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_storage_num > 0)
	{
		const bool arrayed = UsesArrayed2dImages(bind_info);
		const bool three_dimensional = UsesThreeDimensionalImages(bind_info);
		const bool uint_images = UsesUnsignedIntegerImages(bind_info);
		auto dst_value0 = operand_variable_to_str(inst.dst, 0);
		auto dst_value1 = operand_variable_to_str(inst.dst, 1);
		auto dst_value2 = operand_variable_to_str(inst.dst, 2);
		auto dst_value3 = operand_variable_to_str(inst.dst, 3);

		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);

		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src1_value2 = operand_variable_to_str(inst.src[1], 2);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED
		// TODO() swizzle channels
		// TODO() convert SRGB -> LINEAR if SRGB format was replaced with UNORM

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t25_<index> = OpLoad %uint %<src1_value2>
		%t143_<index> = OpShiftRightLogical %uint %t25_<index> %uint_0
        %t145_<index> = OpBitwiseAnd %uint %t143_<index> %uint_0x00003fff
        %t146_<index> = OpIAdd %uint %t145_<index> %uint_1
        %t149_<index> = OpShiftRightLogical %uint %t25_<index> %uint_14
        %t150_<index> = OpBitwiseAnd %uint %t149_<index> %uint_0x00003fff
        %t151_<index> = OpIAdd %uint %t150_<index> %uint_1
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageL %textures2D_L %t24_<index>
         %t27_<index> = OpLoad %ImageL %t26_<index>
         %t67_<index> = OpLoad %float %<src0_value0>
         %t69_<index> = OpBitcast %uint %t67_<index>
         %t70_<index> = OpLoad %float %<src0_value1>
         %t71_<index> = OpBitcast %uint %t70_<index>
<array_coordinate_load>         %t73_<index> = OpCompositeConstruct %<coordinate_type> %t69_<index> %t71_<index><array_coordinate_value>
         %t84_<index> = OpLoad %float %<dst_value0>
         %t85_<index> = OpLoad %float %<dst_value1>
         %t86_<index> = OpLoad %float %<dst_value2>
         %t87_<index> = OpLoad %float %<dst_value3>
         %t881_<index> = <float_to_scalar> %<image_scalar> %t84_<index>
         %t882_<index> = <float_to_scalar> %<image_scalar> %t85_<index>
         %t883_<index> = <float_to_scalar> %<image_scalar> %t86_<index>
         %t884_<index> = <float_to_scalar> %<image_scalar> %t87_<index>
         %t88_<index> = OpCompositeConstruct %<image_vector> %t881_<index> %t882_<index> %t883_<index> %t884_<index>
               OpImageWrite %t27_<index> %t73_<index> %t88_<index>
)";
		const auto src0_value2 = mimg_address_to_str(inst, 2);
		const String8 index_string = String8::FromPrintf("%u", index);
		const String8 array_coordinate_load =
		    ((arrayed || three_dimensional) ? String8("         %t72_<index> = OpLoad %float %<src0_value2>\n         %t721_<index> = OpBitcast %uint %t72_<index>\n")
		                   .ReplaceStr("<index>", index_string)
		                   .ReplaceStr("<src0_value2>", src0_value2.value) :
		               String8(""));
		*dst_source += String8(text)
		                   .ReplaceStr("<array_coordinate_load>", array_coordinate_load)
		                   .ReplaceStr("<coordinate_type>", (arrayed || three_dimensional) ? "v3uint" : "v2uint")
		                   .ReplaceStr("<array_coordinate_value>", (arrayed || three_dimensional) ? String8::FromPrintf(" %%t721_%u", index) : String8(""))
		                   .ReplaceStr("<image_scalar>", uint_images ? "uint" : "float")
		                   .ReplaceStr("<image_vector>", uint_images ? "v4uint" : "v4float")
		                   .ReplaceStr("<float_to_scalar>", uint_images ? "OpBitcast" : "OpCopyObject")
		                   .ReplaceStr("<index>", index_string)
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value2>", src1_value2.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value)
		                   .ReplaceStr("<dst_value3>", dst_value3.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_ImageStoreMip_Vdata4Vaddr4StDmaskF)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();
	// const auto& bind_params = spirv->GetBindParams();

	if (bind_info != nullptr && bind_info->textures2D.textures2d_storage_num > 0)
	{
		auto dst_value0 = operand_variable_to_str(inst.dst, 0);
		auto dst_value1 = operand_variable_to_str(inst.dst, 1);
		auto dst_value2 = operand_variable_to_str(inst.dst, 2);
		auto dst_value3 = operand_variable_to_str(inst.dst, 3);

		auto src0_value0 = mimg_address_to_str(inst, 0);
		auto src0_value1 = mimg_address_to_str(inst, 1);
		auto src0_value2 = mimg_address_to_str(inst, 2);

		auto src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto src1_value2 = operand_variable_to_str(inst.src[1], 2);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check LOD_CLAMPED
		// TODO() swizzle channels
		// TODO() convert SRGB -> LINEAR if SRGB format was replaced with UNORM

		static const char* text = R"(
         %t24_<index> = OpLoad %uint %<src1_value0>
         %t25_<index> = OpLoad %uint %<src1_value2>
		%t143_<index> = OpShiftRightLogical %uint %t25_<index> %uint_0
        %t145_<index> = OpBitwiseAnd %uint %t143_<index> %uint_0x00003fff
        %t146_<index> = OpIAdd %uint %t145_<index> %uint_1
        %t149_<index> = OpShiftRightLogical %uint %t25_<index> %uint_14
        %t150_<index> = OpBitwiseAnd %uint %t149_<index> %uint_0x00003fff
        %t151_<index> = OpIAdd %uint %t150_<index> %uint_1
         %t26_<index> = OpAccessChain %_ptr_UniformConstant_ImageL %textures2D_L %t24_<index>
         %t27_<index> = OpLoad %ImageL %t26_<index>
         %t67_<index> = OpLoad %float %<src0_value0>
         %t69_<index> = OpBitcast %uint %t67_<index>
         %t70_<index> = OpLoad %float %<src0_value1>
         %t71_<index> = OpBitcast %uint %t70_<index>
         %t701_<index> = OpLoad %float %<src0_value2>
         %t711_<index> = OpBitcast %uint %t701_<index>
         %t160_<index> = OpFunctionCall %v2uint %mipmap %t711_<index> %t146_<index> %t151_<index>
         %t73_<index> = OpCompositeConstruct %v2uint %t69_<index> %t71_<index>
         %t84_<index> = OpLoad %float %<dst_value0>
         %t85_<index> = OpLoad %float %<dst_value1>
         %t86_<index> = OpLoad %float %<dst_value2>
         %t87_<index> = OpLoad %float %<dst_value3>
         %t172_<index> = OpIAdd %v2uint %t160_<index> %t73_<index>
         %t88_<index> = OpCompositeConstruct %v4float %t84_<index> %t85_<index> %t86_<index> %t87_<index>
               OpImageWrite %t27_<index> %t172_<index> %t88_<index>
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<src0_value2>", src0_value2.value)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value2>", src1_value2.value)
		                   .ReplaceStr("<dst_value0>", dst_value0.value)
		                   .ReplaceStr("<dst_value1>", dst_value1.value)
		                   .ReplaceStr("<dst_value2>", dst_value2.value)
		                   .ReplaceStr("<dst_value3>", dst_value3.value);

		return true;
	}

	return false;
}

/* XXX: Andn2, Or, Nor, Cselect */
KYTY_RECOMPILER_FUNC(Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	String8 load0;
	String8 load1;
	String8 load2;
	String8 load3;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t2_<index>", index_str, &load2, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t3_<index>", index_str, &load3, 1))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
    <load1>
    <load2>
    <load3>
    <param0>
    <param1>
    <param2>
    <param3>
    OpStore %<dst0> %tb_<index>
    OpStore %<dst1> %td_<index>
    <execz>
    <scc>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<load3>", load3)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", param[1])
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 2))
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_S_Lshl_B64_Sdst2Ssrc02Ssrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	String8 load0;
	String8 load1;
	String8 load2;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t2_<index>", index_str, &load2))
	{
		return false;
	}

	static const char* text = R"(
     <load0>
     <load1>
     <load2>
     <param0>
     <param1>
     <param2>
     <param3>
%t22_<index> = OpBitwiseAnd %uint %t2_<index> %uint_63
     OpStore %temp_uint_2 %t0_<index>
     OpStore %temp_uint_3 %t1_<index>
     OpStore %temp_uint_4 %t22_<index>
%t_<index> = OpFunctionCall %void %shift_left %temp_uint_0 %temp_uint_1 %temp_uint_2 %temp_uint_3 %temp_uint_4
%r0_<index> = OpLoad %uint %temp_uint_0
%r1_<index> = OpLoad %uint %temp_uint_1
     OpStore %<dst0> %r0_<index>
     OpStore %<dst1> %r1_<index>
     <execz>
     <scc>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 2))
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_S_Lshr_B64_Sdst2Ssrc02Ssrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	String8 load0;
	String8 load1;
	String8 load2;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t2_<index>", index_str, &load2))
	{
		return false;
	}

	static const char* text = R"(
     <load0>
     <load1>
     <load2>
     <param0>
     <param1>
     <param2>
     <param3>
%t22_<index> = OpBitwiseAnd %uint %t2_<index> %uint_63
     OpStore %temp_uint_2 %t0_<index>
     OpStore %temp_uint_3 %t1_<index>
     OpStore %temp_uint_4 %t22_<index>
%t_<index> = OpFunctionCall %void %shift_right %temp_uint_0 %temp_uint_1 %temp_uint_2 %temp_uint_3 %temp_uint_4
%r0_<index> = OpLoad %uint %temp_uint_0
%r1_<index> = OpLoad %uint %temp_uint_1
     OpStore %<dst0> %r0_<index>
     OpStore %<dst1> %r1_<index>
     <execz>
     <scc>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 2))
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_S_Bfe_U64_Sdst2Ssrc02Ssrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	String8 load0;
	String8 load1;
	String8 load2;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t2_<index>", index_str, &load2))
	{
		return false;
	}

	static const char* text = R"(
     <load0>
     <load1>
     <load2>
     <param0>
     <param1>
     <param2>
     <param3>
 %to_<index> = OpBitFieldUExtract %uint %t2_<index> %uint_0  %uint_6
 %ts_<index> = OpBitFieldUExtract %uint %t2_<index> %uint_16 %uint_7
%tn0_<index> = OpISub %uint %uint_64 %to_<index>
%ts2_<index> = OpExtInst %uint %GLSL_std_450 UMin %ts_<index> %tn0_<index>
%tn1_<index> = OpISub %uint %uint_64 %ts2_<index>
%tn2_<index> = OpISub %uint %tn1_<index> %to_<index>
     OpStore %temp_uint_2 %t0_<index>
     OpStore %temp_uint_3 %t1_<index>
     OpStore %temp_uint_4 %tn2_<index>
%tf1_<index> = OpFunctionCall %void %shift_left %temp_uint_0 %temp_uint_1 %temp_uint_2 %temp_uint_3 %temp_uint_4
     OpStore %temp_uint_4 %tn1_<index>
%tf2_<index> = OpFunctionCall %void %shift_right %temp_uint_2 %temp_uint_3 %temp_uint_0 %temp_uint_1 %temp_uint_4
 %r0_<index> = OpLoad %uint %temp_uint_2
 %r1_<index> = OpLoad %uint %temp_uint_3
     OpStore %<dst0> %r0_<index>
     OpStore %<dst1> %r1_<index>
     <execz>
     <scc>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 2))
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: And, Lshl, Lshr, CSelect, Or */
KYTY_RECOMPILER_FUNC(Recompile_S_XXX_B32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Uint);
	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	static const char* text = R"(
              <load0>
              <load1>
              <param0>
              <param1>
              <param2>
              OpStore %<dst> %t_<index>
              <scc>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 1))
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Add, Mul, Sub */
KYTY_RECOMPILER_FUNC(Recompile_S_XXX_I32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Uint);
	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_int(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	static const char* text = R"(
              <load0>
              <load1>
              <param>
              %tu_<index> = OpBitcast %uint %t_<index>
              OpStore %<dst> %tu_<index>
              <scc>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 1))
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Add, Addc, Bfe, Lshl4Add, MulHi */
KYTY_RECOMPILER_FUNC(Recompile_S_XXX_U32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Uint);
	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	static const char* text = R"(
              <load0>
              <load1>
              <param0>
              <param1>
              <param2>
              <param3>
              OpStore %<dst> %t_<index>
              <scc>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 1))
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_SSaveexecB64_Sdst2Ssrc02)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	String8 load0;
	String8 load1;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}

	String8 exec_update;
	switch (inst.type)
	{
		case ShaderInstructionType::SAndSaveexecB64:
			exec_update = R"(
        %t194_<index> = OpBitwiseAnd %uint %t0_<index> %t190_<index>
               OpStore %exec_lo %t194_<index>
        %t197_<index> = OpBitwiseAnd %uint %t1_<index> %t191_<index>
               OpStore %exec_hi %t197_<index>
)";
			break;
		case ShaderInstructionType::SAndn1SaveexecB64:
			exec_update = R"(
        %t193_<index> = OpNot %uint %t0_<index>
        %t194_<index> = OpBitwiseAnd %uint %t193_<index> %t190_<index>
               OpStore %exec_lo %t194_<index>
        %t196_<index> = OpNot %uint %t1_<index>
        %t197_<index> = OpBitwiseAnd %uint %t196_<index> %t191_<index>
               OpStore %exec_hi %t197_<index>
)";
			break;
		default: return false;
	}

	static const char* text = R"(
        <load0>
        <load1>
        %t190_<index> = OpLoad %uint %exec_lo
               OpStore %<dst0> %t190_<index>
        %t191_<index> = OpLoad %uint %exec_hi
               OpStore %<dst1> %t191_<index>
        <exec_update>
        <execz>
        <scc>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<exec_update>", exec_update)
	                   .ReplaceStr("<execz>", EXECZ)
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 2))
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Eq, Ge, Gt, Lg, Lt, Le */
KYTY_RECOMPILER_FUNC(Recompile_SCmp_XXX_I32_Ssrc0Ssrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_int(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <param> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %scc %t3_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Eq, Le, Lg, Gt, Lt */
KYTY_RECOMPILER_FUNC(Recompile_SCmp_XXX_U32_Ssrc0Ssrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <param> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %scc %t3_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_SBufferLoadDword_SdstSvSoffset)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		auto    dst_value   = operand_variable_to_str(inst.dst);
		auto    src0_value0 = operand_variable_to_str(inst.src[0], 0);
		String8 index_str   = String8::FromPrintf("%u", index);
		String8 load1;

		EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));
		if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
		{
			return false;
		}

		static const char* text_plain = R"(
        <load1>
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
        %t102_<index> = OpBitcast %int %t1_<index>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword %<p0> %temp_int_1 %temp_int_2
)";
		static const char* text_imm   = R"(
        <load1>
        %t1imm_<index> = OpIAdd %uint %t1_<index> %<imm>
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
        %t102_<index> = OpBitcast %int %t1imm_<index>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword %<p0> %temp_int_1 %temp_int_2
)";
		const char*        text       = (inst.smem_imm_offset != 0) ? text_imm : text_plain;
		*dst_source += String8(text)
		                   .ReplaceStr("<load1>", load1)
		                   .ReplaceStr("<index>", index_str)
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<p0>", dst_value.value)
		                   .ReplaceStr("<imm>", spirv->GetConstantUint(static_cast<uint32_t>(inst.smem_imm_offset)));

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_SBufferLoadDwordx2_Sdst2SvSoffset)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    src0_value0 = operand_variable_to_str(inst.src[0], 0);
		String8 index_str   = String8::FromPrintf("%u", index);
		String8 load1;

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));
		if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
		{
			return false;
		}

		static const char* text_plain = R"(
        <load1>
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
        %t102_<index> = OpBitcast %int %t1_<index>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword_2 %<p0> %<p1> %temp_int_1 %temp_int_2
)";
		static const char* text_imm   = R"(
        <load1>
        %t1imm_<index> = OpIAdd %uint %t1_<index> %<imm>
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
        %t102_<index> = OpBitcast %int %t1imm_<index>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword_2 %<p0> %<p1> %temp_int_1 %temp_int_2
)";
		const char*        text       = (inst.smem_imm_offset != 0) ? text_imm : text_plain;
		*dst_source += String8(text)
		                   .ReplaceStr("<load1>", load1)
		                   .ReplaceStr("<index>", index_str)
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<imm>", spirv->GetConstantUint(static_cast<uint32_t>(inst.smem_imm_offset)));

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_SBufferLoadDwordx4_Sdst4SvSoffset)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		// EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[1]));

		auto dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
		// String8 offset      = spirv->GetConstant(inst.src[1]);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

		EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

		String8 index_str = String8::FromPrintf("%u", index);

		String8 load1;

		if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
		{
			return false;
		}

		// Optional SMEM immediate: final byte offset = SGPR soffset + signed imm
		// (captured s_buffer_load_dwordx4 s[…], s[…], s24 offset:0x10).
		static const char* text_plain = R"(
        <load1>
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
        %t102_<index> = OpBitcast %int %t1_<index>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword_4 %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2
)";
		static const char* text_imm   = R"(
        <load1>
        %t1imm_<index> = OpIAdd %uint %t1_<index> %<imm>
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
        %t102_<index> = OpBitcast %int %t1imm_<index>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword_4 %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2
)";
		const char*        text       = (inst.smem_imm_offset != 0) ? text_imm : text_plain;
		*dst_source += String8(text)
		                   .ReplaceStr("<load1>", load1)
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value)
		                   .ReplaceStr("<p3>", dst_value3.value)
		                   .ReplaceStr("<imm>", spirv->GetConstantUint(static_cast<uint32_t>(inst.smem_imm_offset)))
		                   .ReplaceStr("<index>", index_str);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_SBufferLoadDwordx8_Sdst8SvSoffset)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[1]));

		SpirvValue dst_value[8];

		for (int i = 0; i < 8; i++)
		{
			dst_value[i] = operand_variable_to_str(inst.dst, i);
		}

		auto    src0_value0 = operand_variable_to_str(inst.src[0], 0);
		String8 offset      = spirv->GetConstant(inst.src[1]);

		EXIT_NOT_IMPLEMENTED(dst_value[0].type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Uint);

		EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

		String8 text = R"(
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
        %t102_<index> = OpBitcast %int %<offset>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword_8 %<p0> %<p1> %<p2> %<p3> %<p4> %<p5> %<p6> %<p7> %temp_int_1 %temp_int_2
)";

		for (int i = 0; i < 8; i++)
		{
			text = text.ReplaceStr(String8::FromPrintf("<p%d>", i), dst_value[i].value);
		}

		*dst_source += text.ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src0_value0>", src0_value0.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_SBufferLoadDwordx16_Sdst16SvSoffset)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[1]));

		SpirvValue dst_value[16];

		for (int i = 0; i < 16; i++)
		{
			dst_value[i] = operand_variable_to_str(inst.dst, i);
		}

		auto    src0_value0 = operand_variable_to_str(inst.src[0], 0);
		String8 offset      = spirv->GetConstant(inst.src[1]);

		EXIT_NOT_IMPLEMENTED(dst_value[0].type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Uint);

		EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

		String8 text = R"(
        %t100_<index> = OpLoad %uint %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_2 %t101_<index>
        %t102_<index> = OpBitcast %int %<offset>
               OpStore %temp_int_1 %t102_<index>
        %t110_<index> = OpFunctionCall %void %sbuffer_load_dword_16 %<p0> %<p1> %<p2> %<p3> %<p4> %<p5> %<p6> %<p7> %<p8> %<p9> %<p10> %<p11> %<p12> %<p13> %<p14> %<p15> %temp_int_1 %temp_int_2
)";

		for (int i = 0; i < 16; i++)
		{
			text = text.ReplaceStr(String8::FromPrintf("<p%d>", i), dst_value[i].value);
		}

		*dst_source += text.ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src0_value0>", src0_value0.value);

		return true;
	}

	return false;
}

// KYTY_RECOMPILER_FUNC(Recompile_SCbranchExecz_Label)
//{
//	const auto& inst = code.GetInstructions().At(index);
//
//	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));
//
//	EXIT_NOT_IMPLEMENTED(code.ReadBlock(ShaderLabel(inst).GetDst()).is_discard);
//
//	String8 label = ShaderLabel(inst).ToString();
//
//	static const char* text = R"(
//         %execz_u_<index> = OpLoad %uint %execz
//         %execz_b_<index> = OpINotEqual %bool %execz_u_<index> %uint_0
//                OpSelectionMerge %<label> None
//                OpBranchConditional %execz_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//
//	*dst_source += String8(text).ReplaceStr("<index>", String8::FromPrintf("%u", index)).ReplaceStr("<label>", label);
//
//	return true;
// }

KYTY_RECOMPILER_FUNC(Recompile_SBranch_Label)
{
	const auto& inst = code.GetInstructions().At(index);

	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));

	EXIT_NOT_IMPLEMENTED(code.ReadBlock(ShaderLabel(inst).GetDst()).is_discard);

	const auto branch = ShaderLabel(inst);
	String8    label  = branch.ToString();

	if (branch.GetDst() < inst.pc)
	{
		String8 continue_label = String8::FromPrintf("loop_continue_%04" PRIx32, inst.pc);
		String8 merge_label    = find_backward_loop_merge(code, branch);
		const bool has_exit    = merge_label.Size() != 0;
		if (!has_exit)
		{
			merge_label = String8::FromPrintf("loop_merge_%04" PRIx32, inst.pc);
		}

		static const char* loop_text = R"(
                OpBranch %<continue>
       %<continue> = OpLabel
                OpBranch %<label>
        <unreachable_merge>
)";

		*dst_source +=
		    String8(loop_text)
		        .ReplaceStr("<continue>", continue_label)
		        .ReplaceStr("<label>", label)
		        .ReplaceStr("<unreachable_merge>",
		                    has_exit ? ""
		                             : String8::FromPrintf("%%%s = OpLabel\n                OpUnreachable", merge_label.c_str()));
		return true;
	}

	static const char* text = R"(
                OpBranch %<label>
)";

	*dst_source += String8(text).ReplaceStr("<index>", String8::FromPrintf("%u", index)).ReplaceStr("<label>", label);

	return true;
}

/* XXX: Execz, Scc0, Scc1, Vccz, Vccnz */
KYTY_RECOMPILER_FUNC(Recompile_SCbranch_XXX_Label)
{
	EXIT_NOT_IMPLEMENTED(index + 1 >= code.GetInstructions().Size());

	const auto& inst      = code.GetInstructions().At(index);
	const auto& next_inst = code.GetInstructions().At(index + 1);

	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));

	// TODO(): analyze control flow graph
	auto label            = ShaderLabel(inst);
	auto dst_block        = code.ReadBlock(label.GetDst());
	auto next_block       = code.ReadBlock(next_inst.pc);
	bool discard          = dst_block.is_discard;
	auto label_next_block = ShaderLabel(next_block.last);
	auto label_dst_block  = ShaderLabel(dst_block.last);

	bool if_else = next_block.is_valid && !next_block.is_discard && dst_block.is_valid && !dst_block.is_discard &&
	               ((next_block.last.type == ShaderInstructionType::SBranch && label_next_block.GetDst() >= dst_block.pc &&
	                 label_next_block.GetDst() <= dst_block.last.pc) ||
	                (dst_block.last.type == ShaderInstructionType::SBranch && label_dst_block.GetDst() >= next_block.pc &&
	                 label_dst_block.GetDst() <= next_block.last.pc));

	String8 label_str = label.ToString();
	String8 label_merge =
	    if_else ? (dst_block.last.type == ShaderInstructionType::SBranch ? label_dst_block.ToString() : label_next_block.ToString()) : "";
	const uint32_t loop_backedge = find_backward_loop_for_exit(code, label);

	if (!if_else && label.GetDst() > inst.pc)
	{
		const auto& instructions = code.GetInstructions();
		for (uint32_t i = 1; i < instructions.Size(); i++)
		{
			if (instructions.At(i).pc == label.GetDst())
			{
				const auto& previous = instructions.At(i - 1);
				if (previous.type == ShaderInstructionType::SBranch)
				{
					auto previous_branch = ShaderLabel(previous);
					if (previous_branch.GetDst() > label.GetDst())
					{
						if_else     = true;
						label_merge = previous_branch.ToString();
					}
				}
				break;
			}
		}
	}

	//	if (condition)
	//	{
	//		L1:
	//		...
	//	}
	// L2: /* merge */
	//	...
	static const char* text_variant_a = R"(
        <param0>
        <param1>
               OpSelectionMerge %<label> None
               OpBranchConditional %cc_b_<index> %<label> %t230_<index>
        %t230_<index> = OpLabel
)";

	//	if (condition)
	//	{
	//		L2:
	//		...
	//		discard;
	//	}
	// L1: /* merge */
	//	...
	static const char* text_variant_b = R"(
        <param0>
        <param1>
               OpSelectionMerge %t230_<index> None
               OpBranchConditional %cc_b_<index> %<label> %t230_<index>
        %t230_<index> = OpLabel
)";

	//	if (condition)
	//	{
	//		L1:
	//		...
	//	} else
	//	{
	// 		L2:
	//		...
	//	}
	//	 /* merge */
	static const char* text_variant_c = R"(
        <param0>
        <param1>
               OpSelectionMerge %<merge> None
               OpBranchConditional %cc_b_<index> %<label> %t230_<index>
        %t230_<index> = OpLabel
)";

	static const char* text_loop_exit = R"(
        <param0>
        <param1>
               OpLoopMerge %<label> %loop_continue_<backedge> None
               OpBranchConditional %cc_b_<index> %<label> %t230_<index>
        %t230_<index> = OpLabel
)";

	const char* text = text_variant_a;
	if (discard)
	{
		text = text_variant_b;
	}
	if (if_else)
	{
		text = text_variant_c;
	}
	if (loop_backedge != 0)
	{
		text = text_loop_exit;
	}

	*dst_source += String8(text)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", param[1])
	                   .ReplaceStr("<merge>", label_merge)
	                   .ReplaceStr("<backedge>", String8::FromPrintf("%04" PRIx32, loop_backedge))
	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
	                   .ReplaceStr("<label>", label_str);

	return true;
}

// KYTY_RECOMPILER_FUNC(Recompile_SCbranchScc0_Label)
//{
//	const auto& inst = code.GetInstructions().At(index);
//
//	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));
//
//	auto label = ShaderLabel(inst);
//
//	// TODO(): analyze control flow graph
//	bool discard = code.ReadBlock(label.GetDst()).is_discard;
//
//	String8 label_str = label.ToString();
//
//	static const char* text_variant_a = R"(
//         %scc_u_<index> = OpLoad %uint %scc
//         %scc_b_<index> = OpIEqual %bool %scc_u_<index> %uint_0
//                OpSelectionMerge %<label> None
//                OpBranchConditional %scc_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//	static const char* text_variant_b = R"(
//         %scc_u_<index> = OpLoad %uint %scc
//         %scc_b_<index> = OpIEqual %bool %scc_u_<index> %uint_0
//                OpSelectionMerge %t230_<index> None
//                OpBranchConditional %scc_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//
//	*dst_source += String8(discard ? text_variant_b : text_variant_a)
//	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
//	                   .ReplaceStr("<label>", label_str);
//
//	return true;
// }

// KYTY_RECOMPILER_FUNC(Recompile_SCbranchScc1_Label)
//{
//	const auto& inst = code.GetInstructions().At(index);
//
//	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));
//
//	auto label = ShaderLabel(inst);
//
//	// TODO(): analyze control flow graph
//	bool discard = code.ReadBlock(label.GetDst()).is_discard;
//
//	String8 label_str = label.ToString();
//
//	static const char* text_variant_a = R"(
//         %scc_u_<index> = OpLoad %uint %scc
//         %scc_b_<index> = OpIEqual %bool %scc_u_<index> %uint_1
//                OpSelectionMerge %<label> None
//                OpBranchConditional %scc_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//	static const char* text_variant_b = R"(
//         %scc_u_<index> = OpLoad %uint %scc
//         %scc_b_<index> = OpIEqual %bool %scc_u_<index> %uint_1
//                OpSelectionMerge %t230_<index> None
//                OpBranchConditional %scc_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//
//	*dst_source += String8(discard ? text_variant_b : text_variant_a)
//	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
//	                   .ReplaceStr("<label>", label_str);
//
//	return true;
// }
//
// KYTY_RECOMPILER_FUNC(Recompile_SCbranchVccz_Label)
//{
//	const auto& inst = code.GetInstructions().At(index);
//
//	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));
//
//	auto label = ShaderLabel(inst);
//
//	// TODO(): analyze control flow graph
//	bool discard = code.ReadBlock(label.GetDst()).is_discard;
//
//	String8 label_str = label.ToString();
//
//	static const char* text_variant_a = R"(
//         %vcc_lo_u_<index> = OpLoad %uint %vcc_lo
//         %vcc_lo_b_<index> = OpIEqual %bool %vcc_lo_u_<index> %uint_0
//                OpSelectionMerge %<label> None
//                OpBranchConditional %vcc_lo_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//	static const char* text_variant_b = R"(
//         %vcc_lo_u_<index> = OpLoad %uint %vcc_lo
//         %vcc_lo_b_<index> = OpIEqual %bool %vcc_lo_u_<index> %uint_0
//                OpSelectionMerge %t230_<index> None
//                OpBranchConditional %vcc_lo_b_<index> %<label> %t230_<index>
//         %t230_<index> = OpLabel
//)";
//
//	*dst_source += String8(discard ? text_variant_b : text_variant_a)
//	                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
//	                   .ReplaceStr("<label>", label_str);
//
//	return true;
// }

KYTY_RECOMPILER_FUNC(Recompile_SEndpgm_Empty)
{
	// const auto* info = spirv->GetPsInputInfo();

	// EXIT_NOT_IMPLEMENTED(info == nullptr || !info->ps_pixel_kill_enable);

	static const char* text = R"(
       OpReturn
)";

	EXIT_NOT_IMPLEMENTED(index < 2);

	const auto& prev_prev_inst = code.GetInstructions().At(index - 2);
	const auto& prev_inst      = code.GetInstructions().At(index - 1);

	bool after_kill =
	    (prev_prev_inst.type == ShaderInstructionType::SMovB64 && prev_prev_inst.format == ShaderInstructionFormat::Sdst2Ssrc02 &&
	     prev_prev_inst.dst.type == ShaderOperandType::ExecLo && prev_prev_inst.src[0].type == ShaderOperandType::IntegerInlineConstant &&
	     prev_prev_inst.src[0].constant.i == 0 && prev_inst.type == ShaderInstructionType::Exp &&
	     ShaderIsNullMrtDoneFormat(prev_inst.format));

	if (!after_kill)
	{
		*dst_source += String8(text);
	}

	return true;
}

// Materialize S_LOAD of `dword_count` dwords from the extended user-data mapping
// (push-constant vsharp slots). Shared by SLoadDword / x2 / x4 / x8.
static bool recompile_sload_from_extended(uint32_t index, const ShaderInstruction& inst, Spirv* spirv, String8* dst_source, int dword_count)
{
	const auto* bind_info = spirv->GetBindInfo();
	if (bind_info == nullptr || !bind_info->extended.used)
	{
		return false;
	}

	const auto* vs_info    = spirv->GetVsInputInfo();
	int         shift_regs = (vs_info != nullptr && vs_info->gs_prolog ? 8 : 0);

	EXIT_NOT_IMPLEMENTED(shift_regs != 0);
	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[1]));
	EXIT_NOT_IMPLEMENTED(inst.src[0].register_id != bind_info->extended.start_register);

	// TODO() check pointer

	EXIT_NOT_IMPLEMENTED(dword_count <= 0 || dword_count > 8);
	EXIT_NOT_IMPLEMENTED(inst.dst.size != dword_count);

	SpirvValue dst_value[8];
	for (int i = 0; i < dword_count; i++)
	{
		dst_value[i] = (dword_count == 1 ? operand_variable_to_str(inst.dst) : operand_variable_to_str(inst.dst, i));
		EXIT_NOT_IMPLEMENTED(dst_value[i].type != SpirvType::Uint);
	}

	auto src0_value0 = operand_variable_to_str(inst.src[0], 0);
	auto src0_value1 = operand_variable_to_str(inst.src[0], 1);
	int  offset      = static_cast<int>(inst.src[1].constant.u >> 2u);

	EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Uint);
	EXIT_NOT_IMPLEMENTED(src0_value1.type != SpirvType::Uint);

	static const char* text = R"(
		         %vsharp_<index>_<reg> = OpAccessChain %_ptr_PushConstant_uint %vsharp %int_0 %int_<buffer> %int_<field>
		         %vsharp_<index>_value_<reg> = OpLoad %uint %vsharp_<index>_<reg>
		               OpStore %<reg> %vsharp_<index>_value_<reg>
				)";

	for (int i = 0; i < dword_count; i++)
	{
		int buffer = 0;
		int field  = 0;
		spirv->GetMappedIndex(offset + i, &buffer, &field);

		*dst_source += String8(text)
		                   .ReplaceStr("<reg>", dst_value[i].value)
		                   .ReplaceStr("<buffer>", String8::FromPrintf("%d", buffer))
		                   .ReplaceStr("<field>", String8::FromPrintf("%d", field))
		                   .ReplaceStr("<index>", String8::FromPrintf("%u_%d", index, i));
	}

	return true;
}

// Materialize S_LOAD from the Gen5 vertex attribute table (fetch_attrib_reg).
// Destinations receive the snapshotted guest dwords so later SBfe/SLshl see defined values.
static bool recompile_sload_from_fetch_attrib(uint32_t index, const ShaderInstruction& inst, Spirv* spirv, String8* dst_source,
                                              int dword_count)
{
	const auto* vs_info = spirv->GetVsInputInfo();
	if (vs_info == nullptr || !vs_info->fetch_embedded || vs_info->fetch_external || vs_info->fetch_inline)
	{
		return false;
	}

	int shift_regs = (vs_info->gs_prolog ? 8 : 0);
	if (inst.src[0].register_id != vs_info->fetch_attrib_reg + shift_regs)
	{
		return false;
	}

	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[1]));
	EXIT_NOT_IMPLEMENTED(dword_count <= 0 || dword_count > 4);
	EXIT_NOT_IMPLEMENTED(inst.dst.size != dword_count);

	int dword_index = static_cast<int>(inst.src[1].constant.u >> 2u);
	EXIT_NOT_IMPLEMENTED(dword_index < 0);
	EXIT_NOT_IMPLEMENTED(dword_index + dword_count > vs_info->fetch_attrib_data_num);

	static const char* text = R"(
		         %sload_attr_<index> = OpBitcast %uint %<const>
		               OpStore %<reg> %sload_attr_<index>
				)";

	for (int i = 0; i < dword_count; i++)
	{
		auto dst = (dword_count == 1 ? operand_variable_to_str(inst.dst) : operand_variable_to_str(inst.dst, i));
		EXIT_NOT_IMPLEMENTED(dst.type != SpirvType::Uint);

		uint32_t value = vs_info->fetch_attrib_data[dword_index + i];
		*dst_source += String8(text)
		                   .ReplaceStr("<reg>", dst.value)
		                   .ReplaceStr("<const>", spirv->GetConstantUint(value))
		                   .ReplaceStr("<index>", String8::FromPrintf("%u_%d", index, i));
	}

	return true;
}

// fetch_buffer_reg SLoads feed DetectFetch buffer-descriptor tracking; destinations
// are not consumed after BufferLoad→Fetch rewrite. Keep as recognized no-op.
static bool recompile_sload_fetch_buffer_meta(const ShaderInstruction& inst, Spirv* spirv)
{
	const auto* vs_info = spirv->GetVsInputInfo();
	if (vs_info == nullptr || !vs_info->fetch_embedded || vs_info->fetch_external || vs_info->fetch_inline)
	{
		return false;
	}

	int shift_regs = (vs_info->gs_prolog ? 8 : 0);
	return inst.src[0].register_id == vs_info->fetch_buffer_reg + shift_regs;
}

KYTY_RECOMPILER_FUNC(Recompile_SLoadDword_SdstSbaseSoffset)
{
	const auto& inst = code.GetInstructions().At(index);

	if (recompile_sload_from_fetch_attrib(index, inst, spirv, dst_source, 1))
	{
		return true;
	}
	if (recompile_sload_fetch_buffer_meta(inst, spirv))
	{
		return true;
	}
	return recompile_sload_from_extended(index, inst, spirv, dst_source, 1);
}

KYTY_RECOMPILER_FUNC(Recompile_SLoadDwordx2_Sdst2Ssrc02Ssrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	if (recompile_sload_from_fetch_attrib(index, inst, spirv, dst_source, 2))
	{
		return true;
	}
	if (recompile_sload_fetch_buffer_meta(inst, spirv))
	{
		return true;
	}
	return recompile_sload_from_extended(index, inst, spirv, dst_source, 2);
}

KYTY_RECOMPILER_FUNC(Recompile_SLoadDwordx4_Sdst4SbaseSoffset)
{
	const auto& inst = code.GetInstructions().At(index);

	if (recompile_sload_from_fetch_attrib(index, inst, spirv, dst_source, 4))
	{
		return true;
	}
	if (recompile_sload_fetch_buffer_meta(inst, spirv))
	{
		return true;
	}
	return recompile_sload_from_extended(index, inst, spirv, dst_source, 4);
}

KYTY_RECOMPILER_FUNC(Recompile_SLoadDwordx8_Sdst8SbaseSoffset)
{
	const auto& inst = code.GetInstructions().At(index);

	if (recompile_sload_fetch_buffer_meta(inst, spirv))
	{
		return true;
	}
	return recompile_sload_from_extended(index, inst, spirv, dst_source, 8);
}

KYTY_RECOMPILER_FUNC(Recompile_SMulkI32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Uint);
	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	String8 load0;

	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	String8 load_dst;

	if (!operand_load_int(spirv, inst.dst, "tdst_<index>", index_str, &load_dst))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
    <load_dst>
%t_<index> = OpIMul %int %tdst_<index> %t0_<index>
%tu_<index> = OpBitcast %uint %t_<index>
    OpStore %<dst> %tu_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load_dst>", load_dst)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_SMovB32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Uint);
	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	String8 load0;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
    OpStore %<dst> %t0_<index>
)";
	*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<load0>", load0).ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_SMovB64_Sdst2Ssrc02)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	// EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	String8 load0;
	String8 load1;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
    <load1>
    OpStore %<dst0> %t0_<index>
    OpStore %<dst1> %t1_<index>
    <execz>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

// s_not_b32: dst = ~src0; SCC = (dst != 0).
KYTY_RECOMPILER_FUNC(Recompile_SNotB32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Uint);
	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	String8 load0;
	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
    %t_<index> = OpNot %uint %t0_<index>
    OpStore %<dst> %t_<index>
    <scc>
)";
	// Insert <scc> before <dst> so SCC_NZ_1 %<dst> resolves.
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 1))
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

// s_not_b64: dst[63:0] = ~src0[63:0]; SCC = (dst != 0).
KYTY_RECOMPILER_FUNC(Recompile_SNotB64_Sdst2Ssrc02)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	String8 load0;
	String8 load1;
	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}

	static const char* text = R"(
    <load0>
    <load1>
    %tb_<index> = OpNot %uint %t0_<index>
    %td_<index> = OpNot %uint %t1_<index>
    OpStore %<dst0> %tb_<index>
    OpStore %<dst1> %td_<index>
    <execz>
    <scc>
)";
	// Insert <scc> before <dst0>/<dst1> so SCC_NZ_2 placeholders resolve.
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 2))
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_SSwappcB64_Sdst2Ssrc02)
{
	const auto& inst       = code.GetInstructions().At(index);
	const auto* input_info = spirv->GetVsInputInfo();

	if (input_info != nullptr)
	{
		EXIT_NOT_IMPLEMENTED(!input_info->fetch_external);
		EXIT_NOT_IMPLEMENTED(input_info->fetch_shader_reg != 0);
	}

	if (input_info != nullptr && input_info->fetch_external && inst.dst.type == ShaderOperandType::Sgpr && inst.dst.register_id == 0 &&
	    inst.src[0].type == ShaderOperandType::Sgpr && inst.src[0].register_id == 0 && index == 1)
	{
		for (int i = 0; i < input_info->resources_num; i++)
		{
			const auto& r = input_info->resources_dst[i];

			String8 text;

			switch (r.registers_num)
			{
				case 1:
					text = R"(
				         %t1_<index> = OpLoad %float %<attr>
				                       OpStore %temp_float %t1_<index>
				         %t2_<index> = OpFunctionCall %void %fetch_f1_f1_ %<p0> %temp_float
				)";
					break;
				case 2:
					text = R"(
				         %t1_<index> = OpLoad %v2float %<attr>
				                       OpStore %temp_v2float %t1_<index>
				         %t2_<index> = OpFunctionCall %void %fetch_f1_f1_vf2_ %<p0> %<p1> %temp_v2float
				)";
					break;
				case 3:
					text = R"(
				         %t1_<index> = OpLoad %v3float %<attr>
				                       OpStore %temp_v3float %t1_<index>
				         %t2_<index> = OpFunctionCall %void %fetch_f1_f1_f1_vf3_ %<p0> %<p1> %<p2> %temp_v3float
				)";
					break;
				case 4:
					text = R"(
				         %t1_<index> = OpLoad %v4float %<attr>
				                       OpStore %temp_v4float %t1_<index>
				         %t2_<index> = OpFunctionCall %void %fetch_f1_f1_f1_f1_vf4_ %<p0> %<p1> %<p2> %<p3> %temp_v4float
				)";
					break;
				default: EXIT("invalid registers_num: %d\n", r.registers_num);
			}

			*dst_source += String8(text)
			                   .ReplaceStr("<index>", String8::FromPrintf("%d_%u", i, index))
			                   .ReplaceStr("<p0>", String8::FromPrintf("v%d", r.register_start + 0))
			                   .ReplaceStr("<p1>", String8::FromPrintf("v%d", r.register_start + 1))
			                   .ReplaceStr("<p2>", String8::FromPrintf("v%d", r.register_start + 2))
			                   .ReplaceStr("<p3>", String8::FromPrintf("v%d", r.register_start + 3))
			                   .ReplaceStr("<attr>", String8::FromPrintf("attr%d", i));
		}
		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_SWqmB64_Sdst2Ssrc02)
{
	const auto& inst = code.GetInstructions().At(index);

	if (inst.dst.type == ShaderOperandType::ExecLo && inst.src[0].type == ShaderOperandType::ExecLo)
	{
		return true;
	}

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	// EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	String8 load0;
	String8 load1;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0, 0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[0], "t1_<index>", index_str, &load1, 1))
	{
		return false;
	}

	static const char* text = R"(
        <load0>
        <load1>
        %t170_<index> = OpFunctionCall %uint %wqm %t0_<index> %uint_0 %uint_15
        %t172_<index> = OpBitwiseOr %uint %uint_0 %t170_<index>
        %t179_<index> = OpFunctionCall %uint %wqm %t0_<index> %uint_4 %uint_240
        %t181_<index> = OpBitwiseOr %uint %t172_<index> %t179_<index>
        %t188_<index> = OpFunctionCall %uint %wqm %t0_<index> %uint_8 %uint_0x00000f00
        %t190_<index> = OpBitwiseOr %uint %t181_<index> %t188_<index>
        %t197_<index> = OpFunctionCall %uint %wqm %t0_<index> %uint_12 %uint_0x0000f000
        %t199_<index> = OpBitwiseOr %uint %t190_<index> %t197_<index>
        %t206_<index> = OpFunctionCall %uint %wqm %t0_<index> %uint_16 %uint_0x000f0000
        %t208_<index> = OpBitwiseOr %uint %t199_<index> %t206_<index>
        %t215_<index> = OpFunctionCall %uint %wqm %t0_<index> %uint_20 %uint_0x00f00000
        %t217_<index> = OpBitwiseOr %uint %t208_<index> %t215_<index>
        %t224_<index> = OpFunctionCall %uint %wqm %t0_<index> %uint_24 %uint_0x0f000000
        %t226_<index> = OpBitwiseOr %uint %t217_<index> %t224_<index>
        %t233_<index> = OpFunctionCall %uint %wqm %t0_<index> %uint_28 %uint_0xf0000000
        %t235_<index> = OpBitwiseOr %uint %t226_<index> %t233_<index>
        %t1701_<index> = OpFunctionCall %uint %wqm %t1_<index> %uint_0 %uint_15
        %t1721_<index> = OpBitwiseOr %uint %uint_0 %t1701_<index>
        %t1791_<index> = OpFunctionCall %uint %wqm %t1_<index> %uint_4 %uint_240
        %t1811_<index> = OpBitwiseOr %uint %t1721_<index> %t1791_<index>
        %t1881_<index> = OpFunctionCall %uint %wqm %t1_<index> %uint_8 %uint_0x00000f00
        %t1901_<index> = OpBitwiseOr %uint %t1811_<index> %t1881_<index>
        %t1971_<index> = OpFunctionCall %uint %wqm %t1_<index> %uint_12 %uint_0x0000f000
        %t1991_<index> = OpBitwiseOr %uint %t1901_<index> %t1971_<index>
        %t2061_<index> = OpFunctionCall %uint %wqm %t1_<index> %uint_16 %uint_0x000f0000
        %t2081_<index> = OpBitwiseOr %uint %t1991_<index> %t2061_<index>
        %t2151_<index> = OpFunctionCall %uint %wqm %t1_<index> %uint_20 %uint_0x00f00000
        %t2171_<index> = OpBitwiseOr %uint %t2081_<index> %t2151_<index>
        %t2241_<index> = OpFunctionCall %uint %wqm %t1_<index> %uint_24 %uint_0x0f000000
        %t2261_<index> = OpBitwiseOr %uint %t2171_<index> %t2241_<index>
        %t2331_<index> = OpFunctionCall %uint %wqm %t1_<index> %uint_28 %uint_0xf0000000
        %t2351_<index> = OpBitwiseOr %uint %t2261_<index> %t2331_<index>
               OpStore %<dst0> %t235_<index>
               OpStore %<dst1> %t2351_<index>
        <execz>
        <scc>
)";

	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<execz>", (operand_is_exec(inst.dst) ? EXECZ : ""))
	                   .ReplaceStr("<scc>", get_scc_check(scc_check, 2))
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* SInstPrefetch, SWaitcnt, SSendmsg */
KYTY_RECOMPILER_FUNC(Recompile_Skip)
{
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_TBufferLoadFormatX_Vdata1VaddrSvSoffsIdxenFloat1)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value0  = operand_variable_to_str(inst.dst);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
               OpStore %temp_int_5 %int_36
        %t110_<index> = OpFunctionCall %void %tbuffer_load_format_x %<p0> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_TBufferLoadFormatXyzw_Vdata4VaddrSvSoffsIdxenFloat4)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto    dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto    src0_value  = operand_variable_to_str(inst.src[0]);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0>
        %t101_<index> = OpBitcast %int %t100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %<offset>
               OpStore %temp_int_5 %int_119
        %t110_<index> = OpFunctionCall %void %tbuffer_load_format_xyzw %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0>", src0_value.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value)
		                   .ReplaceStr("<p3>", dst_value3.value);

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_TBufferLoadFormatXyzw_Vdata4Vaddr2SvSoffsOffenIdxenFloat4)
{
	const auto& inst      = code.GetInstructions().At(index);
	const auto* bind_info = spirv->GetBindInfo();

	if (bind_info != nullptr && bind_info->storage_buffers.buffers_num > 0)
	{
		EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

		auto    dst_value0  = operand_variable_to_str(inst.dst, 0);
		auto    dst_value1  = operand_variable_to_str(inst.dst, 1);
		auto    dst_value2  = operand_variable_to_str(inst.dst, 2);
		auto    dst_value3  = operand_variable_to_str(inst.dst, 3);
		auto    src0_value0 = operand_variable_to_str(inst.src[0], 0);
		auto    src0_value1 = operand_variable_to_str(inst.src[0], 1);
		auto    src1_value0 = operand_variable_to_str(inst.src[1], 0);
		auto    src1_value1 = operand_variable_to_str(inst.src[1], 1);
		String8 offset      = GetBufferOffsetIntConstant(spirv, inst.src[2]);

		EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value0.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src0_value1.type != SpirvType::Float);
		EXIT_NOT_IMPLEMENTED(src1_value0.type != SpirvType::Uint);
		EXIT_NOT_IMPLEMENTED(src1_value1.type != SpirvType::Uint);

		// TODO() check VSKIP
		// TODO() check EXEC

		static const char* text = R"(
        %t100_<index> = OpLoad %float %<src0_value0>
        %t101_<index> = OpBitcast %int %t100_<index>
       %to100_<index> = OpLoad %float %<src0_value1>
       %to101_<index> = OpBitcast %int %to100_<index>
               OpStore %temp_int_1 %t101_<index>
        %t148_<index> = OpLoad %uint %<src1_value1>
        %t150_<index> = OpShiftRightLogical %uint %t148_<index> %int_16
        %t152_<index> = OpBitwiseAnd %uint %t150_<index> %uint_0x00003fff
        %t153_<index> = OpBitcast %int %t152_<index>
               OpStore %temp_int_3 %t153_<index>
        %t155_<index> = OpLoad %uint %<src1_value0>
        %t156_<index> = OpBitcast %int %t155_<index>
      %offset_<index> = OpIAdd %int %to101_<index> %<offset>
               OpStore %temp_int_4 %t156_<index>
               OpStore %temp_int_2 %offset_<index>
               OpStore %temp_int_5 %int_119
        %t110_<index> = OpFunctionCall %void %tbuffer_load_format_xyzw %<p0> %<p1> %<p2> %<p3> %temp_int_1 %temp_int_2 %temp_int_3 %temp_int_4 %temp_int_5
)";
		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%u", index))
		                   .ReplaceStr("<src0_value0>", src0_value0.value)
		                   .ReplaceStr("<src0_value1>", src0_value1.value)
		                   .ReplaceStr("<offset>", offset)
		                   .ReplaceStr("<src1_value0>", src1_value0.value)
		                   .ReplaceStr("<src1_value1>", src1_value1.value)
		                   .ReplaceStr("<p0>", dst_value0.value)
		                   .ReplaceStr("<p1>", dst_value1.value)
		                   .ReplaceStr("<p2>", dst_value2.value)
		                   .ReplaceStr("<p3>", dst_value3.value);

		return true;
	}

	return false;
}

/* XXX: F, Eq, Ge, Gt, Le, Lg, Lt, Neq, Nge, Ngt, Nlg, Nlt, O, Tru, U */
KYTY_RECOMPILER_FUNC(Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <param> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %<dst0> %t3_<index>
          OpStore %<dst1> %uint_0
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Eq, Ne, Gt, Ge, F, Le, T */
KYTY_RECOMPILER_FUNC(Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_int(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <param> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %<dst0> %t3_<index>
          OpStore %<dst1> %uint_0
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Le, Ge, F, Gt, Lt, T */
KYTY_RECOMPILER_FUNC(Recompile_VCmp_XXX_U32_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <param> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          OpStore %<dst0> %t3_<index>
          OpStore %<dst1> %uint_0
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst_value0.value)
	                   .ReplaceStr("<dst1>", dst_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}

static void append_cmpx_result(String8* dst_source, const String8& load0, const String8& load1, const String8& predicate,
                               const String8& dst0, const String8& dst1, const String8& index_str)
{
	static const char* text = R"(
          <load0>
          <load1>
          %t2_<index> = <predicate> %bool %t0_<index> %t1_<index>
          %t3_<index> = OpSelect %uint %t2_<index> %uint_1 %uint_0
          %texec_<index> = OpLoad %uint %exec_lo
          %tmasked_<index> = OpBitwiseAnd %uint %t3_<index> %texec_<index>
          OpStore %<dst0> %tmasked_<index>
          OpStore %<dst1> %uint_0
          OpStore %exec_lo %tmasked_<index>
          OpStore %exec_hi %uint_0
          <execz>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst0>", dst0)
	                   .ReplaceStr("<dst1>", dst1)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<predicate>", predicate)
	                   .ReplaceStr("<execz>", EXECZ)
	                   .ReplaceStr("<index>", index_str);
}

/* XXX: Eq, Ne */
KYTY_RECOMPILER_FUNC(Recompile_VCmpx_XXX_I32_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_int(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	append_cmpx_result(dst_source, load0, load1, param[0], dst_value0.value, dst_value1.value, index_str);

	return true;
}

// VOPC compare-and-update-exec helper for unsigned 32-bit predicates.
KYTY_RECOMPILER_FUNC(Recompile_VCmpx_XXX_U32_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	append_cmpx_result(dst_source, load0, load1, param[0], dst_value0.value, dst_value1.value, index_str);

	return true;
}

// Ordered and unordered float predicates share the compare-and-update-exec path.
KYTY_RECOMPILER_FUNC(Recompile_VCmpx_XXX_F32_SmaskVsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value0 = operand_variable_to_str(inst.dst, 0);
	auto dst_value1 = operand_variable_to_str(inst.dst, 1);

	EXIT_NOT_IMPLEMENTED(dst_value0.type != SpirvType::Uint);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst));

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	append_cmpx_result(dst_source, load0, load1, param[0], dst_value0.value, dst_value1.value, index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VCndmaskB32_VdstVsrc0Vsrc1Smask2)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[2]));

	auto src_bool_value0 = operand_variable_to_str(inst.src[2], 0);
	auto src_bool_value1 = operand_variable_to_str(inst.src[2], 1);

	EXIT_NOT_IMPLEMENTED(src_bool_value0.type != SpirvType::Uint);

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP

	static const char* text = R"(
    <load0>
    <load1>
    %t22_<index> = OpLoad %uint %<src0>
    %t23_<index> = OpLoad %uint %<src1> ; unused
    %tb_<index> = OpBitwiseAnd %uint %t22_<index> %uint_1
    %t2_<index> = OpINotEqual %bool %tb_<index> %uint_0
    %t3_<index> = OpSelect %float %t2_<index> %t1_<index> %t0_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t3_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<src0>", src_bool_value0.value)
	                   .ReplaceStr("<src1>", src_bool_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VCvtPkrtzF16F32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check DX10_CLAMP

	static const char* text = R"(
    <load0>
    <load1>
    ; Convert each source float to binary16 bits with round-toward-zero.
	    ; The shader ISA writes the packed value, not a float rounded by GLSL.
	    %tpk0_bits_<index> = OpBitcast %uint %t0_<index>
	    %tpk0_shr_sign_<index> = OpShiftRightLogical %uint %tpk0_bits_<index> %uint_16
	    %tpk0_sign_<index> = OpBitwiseAnd %uint %tpk0_shr_sign_<index> %uint_0x00008000
    %tpk0_exp_shift_<index> = OpShiftRightLogical %uint %tpk0_bits_<index> %uint_23
	    %tpk0_exp_<index> = OpBitwiseAnd %uint %tpk0_exp_shift_<index> %uint_255
    %tpk0_mant_<index> = OpBitwiseAnd %uint %tpk0_bits_<index> %uint_0x007fffff
    %tpk0_half_exp_<index> = OpISub %uint %tpk0_exp_<index> %uint_112
    %tpk0_normal_exp_<index> = OpShiftLeftLogical %uint %tpk0_half_exp_<index> %uint_10
    %tpk0_normal_mant_<index> = OpShiftRightLogical %uint %tpk0_mant_<index> %uint_13
    %tpk0_normal_payload_<index> = OpBitwiseOr %uint %tpk0_normal_exp_<index> %tpk0_normal_mant_<index>
    %tpk0_normal_<index> = OpBitwiseOr %uint %tpk0_sign_<index> %tpk0_normal_payload_<index>
    %tpk0_mant_hidden_<index> = OpBitwiseOr %uint %tpk0_mant_<index> %uint_0x00800000
    %tpk0_sub_raw_shift_<index> = OpISub %uint %uint_126 %tpk0_exp_<index>
    %tpk0_exp_lt_103_<index> = OpULessThan %bool %tpk0_exp_<index> %uint_103
    %tpk0_exp_gt_112_<index> = OpUGreaterThan %bool %tpk0_exp_<index> %uint_112
    %tpk0_sub_shift_low_<index> = OpSelect %uint %tpk0_exp_lt_103_<index> %uint_31 %tpk0_sub_raw_shift_<index>
    %tpk0_sub_shift_<index> = OpSelect %uint %tpk0_exp_gt_112_<index> %uint_14 %tpk0_sub_shift_low_<index>
    %tpk0_sub_mant_<index> = OpShiftRightLogical %uint %tpk0_mant_hidden_<index> %tpk0_sub_shift_<index>
    %tpk0_subnormal_<index> = OpBitwiseOr %uint %tpk0_sign_<index> %tpk0_sub_mant_<index>
	    %tpk0_mant_shift_<index> = OpShiftRightLogical %uint %tpk0_mant_<index> %uint_13
	    %tpk0_nan_payload_<index> = OpBitwiseOr %uint %tpk0_mant_shift_<index> %uint_0x00000200
    %tpk0_nan_exp_<index> = OpBitwiseOr %uint %uint_0x00007c00 %tpk0_nan_payload_<index>
    %tpk0_nan_<index> = OpBitwiseOr %uint %tpk0_sign_<index> %tpk0_nan_exp_<index>
    %tpk0_inf_<index> = OpBitwiseOr %uint %tpk0_sign_<index> %uint_0x00007c00
    %tpk0_max_finite_<index> = OpBitwiseOr %uint %tpk0_sign_<index> %uint_0x00007bff
    %tpk0_mant_zero_<index> = OpIEqual %bool %tpk0_mant_<index> %uint_0
    %tpk0_special_<index> = OpSelect %uint %tpk0_mant_zero_<index> %tpk0_inf_<index> %tpk0_nan_<index>
    %tpk0_exp_le_112_<index> = OpULessThanEqual %bool %tpk0_exp_<index> %uint_112
    %tpk0_exp_ge_143_<index> = OpUGreaterThanEqual %bool %tpk0_exp_<index> %uint_143
    %tpk0_exp_eq_255_<index> = OpIEqual %bool %tpk0_exp_<index> %uint_255
    %tpk0_finite0_<index> = OpSelect %uint %tpk0_exp_le_112_<index> %tpk0_subnormal_<index> %tpk0_normal_<index>
    %tpk0_finite1_<index> = OpSelect %uint %tpk0_exp_lt_103_<index> %tpk0_sign_<index> %tpk0_finite0_<index>
    %tpk0_finite2_<index> = OpSelect %uint %tpk0_exp_ge_143_<index> %tpk0_max_finite_<index> %tpk0_finite1_<index>
	    %tpk0_exp_select_<index> = OpSelect %uint %tpk0_exp_eq_255_<index> %tpk0_special_<index> %tpk0_finite2_<index>
	    %tpk0_result_<index> = OpBitwiseAnd %uint %tpk0_exp_select_<index> %uint_0x0000ffff

	    %tpk1_bits_<index> = OpBitcast %uint %t1_<index>
	    %tpk1_shr_sign_<index> = OpShiftRightLogical %uint %tpk1_bits_<index> %uint_16
	    %tpk1_sign_<index> = OpBitwiseAnd %uint %tpk1_shr_sign_<index> %uint_0x00008000
    %tpk1_exp_shift_<index> = OpShiftRightLogical %uint %tpk1_bits_<index> %uint_23
	    %tpk1_exp_<index> = OpBitwiseAnd %uint %tpk1_exp_shift_<index> %uint_255
    %tpk1_mant_<index> = OpBitwiseAnd %uint %tpk1_bits_<index> %uint_0x007fffff
    %tpk1_half_exp_<index> = OpISub %uint %tpk1_exp_<index> %uint_112
    %tpk1_normal_exp_<index> = OpShiftLeftLogical %uint %tpk1_half_exp_<index> %uint_10
    %tpk1_normal_mant_<index> = OpShiftRightLogical %uint %tpk1_mant_<index> %uint_13
    %tpk1_normal_payload_<index> = OpBitwiseOr %uint %tpk1_normal_exp_<index> %tpk1_normal_mant_<index>
    %tpk1_normal_<index> = OpBitwiseOr %uint %tpk1_sign_<index> %tpk1_normal_payload_<index>
    %tpk1_mant_hidden_<index> = OpBitwiseOr %uint %tpk1_mant_<index> %uint_0x00800000
    %tpk1_sub_raw_shift_<index> = OpISub %uint %uint_126 %tpk1_exp_<index>
    %tpk1_exp_lt_103_<index> = OpULessThan %bool %tpk1_exp_<index> %uint_103
    %tpk1_exp_gt_112_<index> = OpUGreaterThan %bool %tpk1_exp_<index> %uint_112
    %tpk1_sub_shift_low_<index> = OpSelect %uint %tpk1_exp_lt_103_<index> %uint_31 %tpk1_sub_raw_shift_<index>
    %tpk1_sub_shift_<index> = OpSelect %uint %tpk1_exp_gt_112_<index> %uint_14 %tpk1_sub_shift_low_<index>
    %tpk1_sub_mant_<index> = OpShiftRightLogical %uint %tpk1_mant_hidden_<index> %tpk1_sub_shift_<index>
    %tpk1_subnormal_<index> = OpBitwiseOr %uint %tpk1_sign_<index> %tpk1_sub_mant_<index>
	    %tpk1_mant_shift_<index> = OpShiftRightLogical %uint %tpk1_mant_<index> %uint_13
	    %tpk1_nan_payload_<index> = OpBitwiseOr %uint %tpk1_mant_shift_<index> %uint_0x00000200
    %tpk1_nan_exp_<index> = OpBitwiseOr %uint %uint_0x00007c00 %tpk1_nan_payload_<index>
    %tpk1_nan_<index> = OpBitwiseOr %uint %tpk1_sign_<index> %tpk1_nan_exp_<index>
    %tpk1_inf_<index> = OpBitwiseOr %uint %tpk1_sign_<index> %uint_0x00007c00
    %tpk1_max_finite_<index> = OpBitwiseOr %uint %tpk1_sign_<index> %uint_0x00007bff
    %tpk1_mant_zero_<index> = OpIEqual %bool %tpk1_mant_<index> %uint_0
    %tpk1_special_<index> = OpSelect %uint %tpk1_mant_zero_<index> %tpk1_inf_<index> %tpk1_nan_<index>
    %tpk1_exp_le_112_<index> = OpULessThanEqual %bool %tpk1_exp_<index> %uint_112
    %tpk1_exp_ge_143_<index> = OpUGreaterThanEqual %bool %tpk1_exp_<index> %uint_143
    %tpk1_exp_eq_255_<index> = OpIEqual %bool %tpk1_exp_<index> %uint_255
    %tpk1_finite0_<index> = OpSelect %uint %tpk1_exp_le_112_<index> %tpk1_subnormal_<index> %tpk1_normal_<index>
    %tpk1_finite1_<index> = OpSelect %uint %tpk1_exp_lt_103_<index> %tpk1_sign_<index> %tpk1_finite0_<index>
    %tpk1_finite2_<index> = OpSelect %uint %tpk1_exp_ge_143_<index> %tpk1_max_finite_<index> %tpk1_finite1_<index>
	    %tpk1_exp_select_<index> = OpSelect %uint %tpk1_exp_eq_255_<index> %tpk1_special_<index> %tpk1_finite2_<index>
	    %tpk1_result_<index> = OpBitwiseAnd %uint %tpk1_exp_select_<index> %uint_0x0000ffff

	    %tpk1_shifted_<index> = OpShiftLeftLogical %uint %tpk1_result_<index> %uint_16
	    %tpk_result_<index> = OpBitwiseOr %uint %tpk0_result_<index> %tpk1_shifted_<index>
    %t4_<index> = OpBitcast %float %tpk_result_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t4_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
        %tdst_packed_<index> = OpLoad %uint %<dst_packed>
        %tpacked_val_<index> = OpSelect %uint %exec_lo_b_<index> %tpk_result_<index> %tdst_packed_<index>
               OpStore %<dst_packed> %tpacked_val_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<dst_packed>", packed_half_shadow_to_str(inst.dst))
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<index>", index_str)
	                   .ReplaceStr("uint_103", spirv->GetConstantUint(103))
	                   .ReplaceStr("uint_112", spirv->GetConstantUint(112))
	                   .ReplaceStr("uint_126", spirv->GetConstantUint(126))
	                   .ReplaceStr("uint_143", spirv->GetConstantUint(143))
	                   .ReplaceStr("uint_255", spirv->GetConstantUint(255));

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VInterpP1F32_VdstVsrcAttrChan)
{
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VInterpP2F32_VdstVsrcAttrChan)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.src[0]));
	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[1]));
	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

	auto dst_value = operand_variable_to_str(inst.dst);

	String8 load0 = String8::FromPrintf("%%t0_<index> = OpAccessChain %%_ptr_Input_float %%attr%u %%uint_%u", inst.src[1].constant.u,
	                                    inst.src[2].constant.u);

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
         <load0>
         %t1_<index> = OpLoad %float %t0_<index>
                       OpStore %<dst> %t1_<index>
)";
	*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<load0>", load0).ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VInterpMovF32_VdstVsrcAttrChan)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[0]));
	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[1]));
	EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[2]));

	EXIT_NOT_IMPLEMENTED(inst.src[0].constant.u != 2);

	auto dst_value = operand_variable_to_str(inst.dst);

	String8 load0 = String8::FromPrintf("%%t0_<index> = OpAccessChain %%_ptr_Input_float %%attr%u %%uint_%u", inst.src[1].constant.u,
	                                    inst.src[2].constant.u);

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
         <load0>
         %t1_<index> = OpLoad %float %t0_<index>
                       OpStore %<dst> %t1_<index>
)";
	*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<load0>", load0).ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Mad, Madak, Madmk, Max3, Min3, Med3, Fma */
KYTY_RECOMPILER_FUNC(Recompile_V_XXX_F32_VdstVsrc0Vsrc1Vsrc2)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;
	String8 load2;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	// EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	// EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[2], "t2_<index>", index_str, &load2))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check SP_ROUND
	// TODO() check DX10_CLAMP
	// TODO() check IEEE

	static const char* text = R"(
              <load0>
              <load1>
              <load2>
              <param0>
              <param1>
              <param2>
              <param3>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %tl2_<index> None
               OpBranchConditional %exec_lo_b_<index> %tl1_<index> %tl2_<index>
         %tl1_<index> = OpLabel
               OpStore %<dst> %t_<index>
              <multiply>
              <clamp>
               OpBranch %tl2_<index>
         %tl2_<index> = OpLabel
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<multiply>", (inst.dst.multiplier != 1.0f
	                                                  ? String8(MULTIPLY).ReplaceStr("<mul>", spirv->GetConstantFloat(inst.dst.multiplier))
	                                                  : ""))
	                   .ReplaceStr("<clamp>", (inst.dst.clamp ? CLAMP : ""))
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VMbcntHiU32B32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	// if (inst.src[0].type == ShaderOperandType::ExecHi)
	//{
	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load0))
	{
		return false;
	}

	// TODO() check VSKIP

	static const char* text = R"(
	    <load0>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t1_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
	)";
	*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<load0>", load0).ReplaceStr("<index>", index_str);

	return true;
	//}

	//	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_VMbcntLoU32B32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	// if (inst.src[0].type == ShaderOperandType::ExecLo)
	//{
	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load0))
	{
		return false;
	}

	// TODO() check VSKIP

	static const char* text = R"(
	    <load0>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t1_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
	)";
	*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<load0>", load0).ReplaceStr("<index>", index_str);

	return true;
	//}

	// return false;
}

/* XXX: Bfrev, Not */
KYTY_RECOMPILER_FUNC(Recompile_V_XXX_B32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	// TODO() check VSKIP

	static const char* text = R"(
              <load0>
              <param0>
              %tf_<index> = OpBitcast %float %t_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %tf_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VNop)
{
	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VMovB32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	// TODO() check VSKIP

	static const char* text = R"(
    <load0>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t0_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text).ReplaceStr("<dst>", dst_value.value).ReplaceStr("<load0>", load0).ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Mac, Max, Min, Mul, Sub, Subrev, Add */
KYTY_RECOMPILER_FUNC(Recompile_V_XXX_F32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;
	String8 load_dst;
	String8 param0 = param[0];

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_float(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}
	if (param0.ContainsStr("tdst_<index>") && !operand_load_float(spirv, inst.dst, "tdst_<index>", index_str, &load_dst))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check SP_DENORM
	// TODO() check SP_ROUND
	// TODO() check DX10_CLAMP
	// TODO() check IEEE

	static const char* text = R"(
              <load0>
              <load1>
              <load_dst>
              <param>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %tl2_<index> None
               OpBranchConditional %exec_lo_b_<index> %tl1_<index> %tl2_<index>
         %tl1_<index> = OpLabel
               OpStore %<dst> %t_<index>
              <multiply>
              <clamp>
               OpBranch %tl2_<index>
         %tl2_<index> = OpLabel
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<multiply>", (inst.dst.multiplier != 1.0f
	                                                  ? String8(MULTIPLY).ReplaceStr("<mul>", spirv->GetConstantFloat(inst.dst.multiplier))
	                                                  : ""))
	                   .ReplaceStr("<clamp>", (inst.dst.clamp ? CLAMP : ""))
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load_dst>", load_dst)
	                   .ReplaceStr("<param>", param0)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Rcp, Rsq, Sqrt, Ceil, Floor, Fract, Rndne, Trunc, Exp, Log, Cos, Sin */
KYTY_RECOMPILER_FUNC(Recompile_V_XXX_F32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	// EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	// EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check DX10_CLAMP
	// TODO() check IEEE

	static const char* text = R"(
    <load0>
    <param0>
    <param1>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
               OpSelectionMerge %tl2_<index> None
               OpBranchConditional %exec_lo_b_<index> %tl1_<index> %tl2_<index>
         %tl1_<index> = OpLabel
               OpStore %<dst> %t_<index>
              <multiply>
              <clamp>
               OpBranch %tl2_<index>
         %tl2_<index> = OpLabel
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<multiply>", (inst.dst.multiplier != 1.0f
	                                                  ? String8(MULTIPLY).ReplaceStr("<mul>", spirv->GetConstantFloat(inst.dst.multiplier))
	                                                  : ""))
	                   .ReplaceStr("<clamp>", (inst.dst.clamp ? CLAMP : ""))
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: And, Or, Bcnt, Bfm, Lshr, Lshl, Lshlrev, Lshrrev, MinU32, MulU32U24, MulLoU32, MulHiU32 */
KYTY_RECOMPILER_FUNC(Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP

	static const char* text = R"(
              <load0>
              <load1>
              <param0>
              <param1>
              <param2>
              %tf_<index> = OpBitcast %float %t_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %tf_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Ashr, Ashrrev, MulLo */
KYTY_RECOMPILER_FUNC(Recompile_V_XXX_I32_SVdstSVsrc0SVsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_int(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_int(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP

	static const char* text = R"(
              <load0>
              <load1>
              <param0>
              <param1>
              %tf_<index> = OpBitcast %float %t_<index>
              OpStore %<dst> %tf_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %tf_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: U32 */
KYTY_RECOMPILER_FUNC(Recompile_VCvt_XXX_F32_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!operand_load_float(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check EXEC
	// TODO() check SP_DENORM_IN

	static const char* text = R"(
    <load0>
    <param0>
    <param1>
    <param2>
    %t_<index> = OpBitcast %float %t2_<index>
    OpStore %<dst> %t_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", param[1])
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: U32, I32, UbyteX, F16 */
KYTY_RECOMPILER_FUNC(Recompile_VCvtF32_XXX_SVdstSVsrc0)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	String8 load0;

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check SP_ROUND

	static const char* text = R"(
    <load0>
    <param0>
    <param1>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %t_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", (param[1] == nullptr ? "" : param[1]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Sad, Bfe, MadU32U24 */
KYTY_RECOMPILER_FUNC(Recompile_V_XXX_U32_VdstVsrc0Vsrc1Vsrc2)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;
	String8 load2;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp);
	EXIT_NOT_IMPLEMENTED(inst.dst.multiplier != 1.0f);

	auto dst_value = operand_variable_to_str(inst.dst);

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[2], "t2_<index>", index_str, &load2))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() Sad: use only lower 16 bits of Vaccum

	static const char* text = R"(
               <load0>
               <load1>
               <load2>
               <param0>
               <param1>
               <param2>
               <param3>
         %tf_<index> = OpBitcast %float %t_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %tdst_<index> = OpLoad %float %<dst>
        %tval_<index> = OpSelect %float %exec_lo_b_<index> %tf_<index> %tdst_<index>
               OpStore %<dst> %tval_<index>
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<param0>", param[0])
	                   .ReplaceStr("<param1>", param[1])
	                   .ReplaceStr("<param2>", (param[2] == nullptr ? "" : param[2]))
	                   .ReplaceStr("<param3>", (param[3] == nullptr ? "" : param[3]))
	                   .ReplaceStr("<index>", index_str);

	return true;
}

/* XXX: Add, Sub, Subrev */
KYTY_RECOMPILER_FUNC(Recompile_V_XXX_U32_VdstSdst2Vsrc0Vsrc1)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst2));

	auto dst_value   = operand_variable_to_str(inst.dst);
	auto dst2_value0 = operand_variable_to_str(inst.dst2, 0);
	auto dst2_value1 = operand_variable_to_str(inst.dst2, 1);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst2));

	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);
	EXIT_NOT_IMPLEMENTED(dst2_value0.type != SpirvType::Uint);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}

	// TODO() check VSKIP
	// TODO() check EXEC

	static const char* text = R"(
              <load0>
              <load1>
        <param>
        %t208_<index> = OpCompositeExtract %uint %t_<index> 1
        %t209_<index> = OpCompositeExtract %uint %t_<index> 0
        %t210_<index> = OpBitcast %float %t209_<index>
               OpStore %<dst> %t210_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %t213_<index> = OpSelect %uint %exec_lo_b_<index> %t208_<index> %uint_0
               OpStore %<dst2_0> %t213_<index>
               OpStore %<dst2_1> %uint_0
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<dst2_0>", dst2_value0.value)
	                   .ReplaceStr("<dst2_1>", dst2_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<param>", param[0])
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_V_XXX_U32_VdstSdst2Vsrc0Vsrc1Ssrc2)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 load0;
	String8 load1;
	String8 load2;

	String8 index_str = String8::FromPrintf("%u", index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst));
	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst2));
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp || inst.dst.multiplier != 1.0f);

	auto dst_value   = operand_variable_to_str(inst.dst);
	auto dst2_value0 = operand_variable_to_str(inst.dst2, 0);
	auto dst2_value1 = operand_variable_to_str(inst.dst2, 1);

	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst2));
	EXIT_NOT_IMPLEMENTED(dst_value.type != SpirvType::Float);
	EXIT_NOT_IMPLEMENTED(dst2_value0.type != SpirvType::Uint);

	if (!operand_load_uint(spirv, inst.src[0], "t0_<index>", index_str, &load0))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[1], "t1_<index>", index_str, &load1))
	{
		return false;
	}
	if (!operand_load_uint(spirv, inst.src[2], "t2_<index>", index_str, &load2, 0))
	{
		return false;
	}

	// V_ADD_CO_CI_U32 uses the VOP3B scalar source pair as a per-lane carry-in.
	// The shared addc helper returns the modular sum and carry-out as uvec2.
	static const char* text = R"(
              <load0>
              <load1>
              <load2>
        %t_<index> = OpFunctionCall %v2uint %addc %t0_<index> %t1_<index> %t2_<index>
        %t208_<index> = OpCompositeExtract %uint %t_<index> 1
        %t209_<index> = OpCompositeExtract %uint %t_<index> 0
        %t210_<index> = OpBitcast %float %t209_<index>
               OpStore %<dst> %t210_<index>
        %exec_lo_u_<index> = OpLoad %uint %exec_lo
        %exec_hi_u_<index> = OpLoad %uint %exec_hi ; unused
        %exec_lo_b_<index> = OpINotEqual %bool %exec_lo_u_<index> %uint_0
        %t213_<index> = OpSelect %uint %exec_lo_b_<index> %t208_<index> %uint_0
               OpStore %<dst2_0> %t213_<index>
               OpStore %<dst2_1> %uint_0
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<dst>", dst_value.value)
	                   .ReplaceStr("<dst2_0>", dst2_value0.value)
	                   .ReplaceStr("<dst2_1>", dst2_value1.value)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2>", load2)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_VMadU64U32_Vdst2Sdst2Vsrc0Vsrc1Vsrc2Pair)
{
	const auto& inst = code.GetInstructions().At(index);

	EXIT_NOT_IMPLEMENTED(!operand_is_variable(inst.dst) || !operand_is_variable(inst.dst2));
	EXIT_NOT_IMPLEMENTED(inst.dst.size != 2 || inst.dst2.size != 2 || inst.src[2].size != 2);
	EXIT_NOT_IMPLEMENTED(inst.dst.clamp || inst.dst.multiplier != 1.0f);
	EXIT_NOT_IMPLEMENTED(operand_is_exec(inst.dst2));

	const String8 index_str = String8::FromPrintf("%u", index);
	const auto    dst_lo    = operand_variable_to_str(inst.dst, 0);
	const auto    dst_hi    = operand_variable_to_str(inst.dst, 1);
	const auto    carry_lo  = operand_variable_to_str(inst.dst2, 0);
	const auto    carry_hi  = operand_variable_to_str(inst.dst2, 1);

	EXIT_NOT_IMPLEMENTED(dst_lo.type != SpirvType::Float || dst_hi.type != SpirvType::Float);
	EXIT_NOT_IMPLEMENTED(carry_lo.type != SpirvType::Uint || carry_hi.type != SpirvType::Uint);

	String8 load0;
	String8 load1;
	String8 load2_lo;
	String8 load2_hi;
	if (!operand_load_uint(spirv, inst.src[0], "tmad0_<index>", index_str, &load0) ||
	    !operand_load_uint(spirv, inst.src[1], "tmad1_<index>", index_str, &load1) ||
	    !operand_load_uint(spirv, inst.src[2], "tadd_lo_<index>", index_str, &load2_lo, 0) ||
	    !operand_load_uint(spirv, inst.src[2], "tadd_hi_<index>", index_str, &load2_hi, 1))
	{
		return false;
	}

	static const char* text = R"(
          <load0>
          <load1>
          <load2_lo>
          <load2_hi>
          %tmul_<index> = OpUMulExtended %ResTypeU %tmad0_<index> %tmad1_<index>
          %tmul_lo_<index> = OpCompositeExtract %uint %tmul_<index> 0
          %tmul_hi_<index> = OpCompositeExtract %uint %tmul_<index> 1
          %tsum_lo_pair_<index> = OpIAddCarry %ResTypeU %tmul_lo_<index> %tadd_lo_<index>
          %tsum_lo_<index> = OpCompositeExtract %uint %tsum_lo_pair_<index> 0
          %tcarry_lo_<index> = OpCompositeExtract %uint %tsum_lo_pair_<index> 1
          %tsum_hi_base_pair_<index> = OpIAddCarry %ResTypeU %tmul_hi_<index> %tadd_hi_<index>
          %tsum_hi_base_<index> = OpCompositeExtract %uint %tsum_hi_base_pair_<index> 0
          %tcarry_hi_base_<index> = OpCompositeExtract %uint %tsum_hi_base_pair_<index> 1
          %tsum_hi_pair_<index> = OpIAddCarry %ResTypeU %tsum_hi_base_<index> %tcarry_lo_<index>
          %tsum_hi_<index> = OpCompositeExtract %uint %tsum_hi_pair_<index> 0
          %tcarry_hi_extra_<index> = OpCompositeExtract %uint %tsum_hi_pair_<index> 1
          %tcarry_out_<index> = OpBitwiseOr %uint %tcarry_hi_base_<index> %tcarry_hi_extra_<index>
          %tresult_lo_<index> = OpBitcast %float %tsum_lo_<index>
          %tresult_hi_<index> = OpBitcast %float %tsum_hi_<index>
          %texec_mad_<index> = OpLoad %uint %exec_lo
          %tactive_mad_<index> = OpINotEqual %bool %texec_mad_<index> %uint_0
          %tdst_old_lo_<index> = OpLoad %float %<dst_lo>
          %tdst_old_hi_<index> = OpLoad %float %<dst_hi>
          %tdst_active_lo_<index> = OpSelect %float %tactive_mad_<index> %tresult_lo_<index> %tdst_old_lo_<index>
          %tdst_active_hi_<index> = OpSelect %float %tactive_mad_<index> %tresult_hi_<index> %tdst_old_hi_<index>
          OpStore %<dst_lo> %tdst_active_lo_<index>
          OpStore %<dst_hi> %tdst_active_hi_<index>
          %tcarry_active_<index> = OpSelect %uint %tactive_mad_<index> %tcarry_out_<index> %uint_0
          OpStore %<carry_lo> %tcarry_active_<index>
          OpStore %<carry_hi> %uint_0
)";
	*dst_source += String8(text)
	                   .ReplaceStr("<load0>", load0)
	                   .ReplaceStr("<load1>", load1)
	                   .ReplaceStr("<load2_lo>", load2_lo)
	                   .ReplaceStr("<load2_hi>", load2_hi)
	                   .ReplaceStr("<dst_lo>", dst_lo.value)
	                   .ReplaceStr("<dst_hi>", dst_hi.value)
	                   .ReplaceStr("<carry_lo>", carry_lo.value)
	                   .ReplaceStr("<carry_hi>", carry_hi.value)
	                   .ReplaceStr("<index>", index_str);

	return true;
}

KYTY_RECOMPILER_FUNC(Recompile_Fetch)
{
	const auto& inst = code.GetInstructions().At(index);

	const auto* input_info = spirv->GetVsInputInfo();

	EXIT_NOT_IMPLEMENTED(input_info == nullptr || !input_info->fetch_embedded);

	if (input_info != nullptr && input_info->fetch_embedded && inst.dst.type == ShaderOperandType::Vgpr &&
	    inst.src[2].type == ShaderOperandType::IntegerInlineConstant)
	{
		int attrib_id = inst.src[2].constant.i;

		const auto& r = input_info->resources_dst[attrib_id];

		EXIT_NOT_IMPLEMENTED(r.registers_num != inst.dst.size);

		String8 text;

		switch (r.registers_num)
		{
			case 1:
				text = R"(
				         %t1_<index> = OpLoad %float %<attr>
				                       OpStore %temp_float %t1_<index>
				         %t2_<index> = OpFunctionCall %void %fetch_f1_f1_ %<p0> %temp_float
				)";
				break;
			case 2:
				text = R"(
				         %t1_<index> = OpLoad %v2float %<attr>
				                       OpStore %temp_v2float %t1_<index>
				         %t2_<index> = OpFunctionCall %void %fetch_f1_f1_vf2_ %<p0> %<p1> %temp_v2float
				)";
				break;
			case 3:
				text = R"(
				         %t1_<index> = OpLoad %v3float %<attr>
				                       OpStore %temp_v3float %t1_<index>
				         %t2_<index> = OpFunctionCall %void %fetch_f1_f1_f1_vf3_ %<p0> %<p1> %<p2> %temp_v3float
				)";
				break;
			case 4:
				text = R"(
				         %t1_<index> = OpLoad %v4float %<attr>
				                       OpStore %temp_v4float %t1_<index>
				         %t2_<index> = OpFunctionCall %void %fetch_f1_f1_f1_f1_vf4_ %<p0> %<p1> %<p2> %<p3> %temp_v4float
				)";
				break;
			default: EXIT("invalid registers_num: %d\n", r.registers_num);
		}

		*dst_source += String8(text)
		                   .ReplaceStr("<index>", String8::FromPrintf("%d_%u", attrib_id, index))
		                   .ReplaceStr("<p0>", String8::FromPrintf("v%d", inst.dst.register_id + 0))
		                   .ReplaceStr("<p1>", String8::FromPrintf("v%d", inst.dst.register_id + 1))
		                   .ReplaceStr("<p2>", String8::FromPrintf("v%d", inst.dst.register_id + 2))
		                   .ReplaceStr("<p3>", String8::FromPrintf("v%d", inst.dst.register_id + 3))
		                   .ReplaceStr("<attr>", String8::FromPrintf("attr%d", attrib_id));

		return true;
	}

	return false;
}

KYTY_RECOMPILER_FUNC(Recompile_Inject_Debug)
{
	const auto& inst = code.GetInstructions().At(index);

	String8 index_str = String8::FromPrintf("%u", index);

	bool injected = false;
	int  str_id   = 0;
	for (const auto& c: code.GetDebugPrintfs())
	{
		if (c.pc == inst.pc)
		{
			Core::StringList8 loads;
			Core::StringList8 params;
			int               arg_id = 0;
			EXIT_IF(c.args.Size() != c.types.Size());
			for (const auto& a: c.args)
			{
				auto    type = c.types.At(arg_id);
				String8 load;
				bool    ok        = false;
				String8 result_id = String8::FromPrintf("t_%d_<index>", arg_id);
				switch (type)
				{
					case ShaderDebugPrintf::Type::Uint: ok = operand_load_uint(spirv, a, result_id, index_str, &load); break;
					case ShaderDebugPrintf::Type::Int: ok = operand_load_int(spirv, a, result_id, index_str, &load); break;
					case ShaderDebugPrintf::Type::Float: ok = operand_load_float(spirv, a, result_id, index_str, &load); break;
				}
				EXIT_NOT_IMPLEMENTED(!ok);
				loads.Add(load);
				params.Add("%" + result_id);
				arg_id++;
			}

			static const char* text = R"(
                <loads>
     %tt_<index> = OpExtInst %void %NonSemantic_DebugPrintf 1 %printf_str_<str_id> <params>
		)";
			*dst_source += String8(text)
			                   .ReplaceStr("<loads>", loads.Concat("\n"))
			                   .ReplaceStr("<str_id>", String8::FromPrintf("%d", str_id))
			                   .ReplaceStr("<params>", params.Concat(" "))
			                   .ReplaceStr("<index>", index_str);
			injected = true;
		}
		str_id++;
	}

	return injected;
}

const RecompilerFunc* RecompFunc(ShaderInstructionType type, ShaderInstructionFormat::Format format)
{
	static const RecompilerFunc g_recomp_func[] = {
	    // clang-format off
    {Recompile_BufferLoadUbyte_Vdata1VaddrSvSoffsIdxen,  ShaderInstructionType::BufferLoadUbyte,     ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen,        {""}},
    {Recompile_BufferLoadDword,                             ShaderInstructionType::BufferLoadDword,      ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen,        {""}},
    {Recompile_BufferLoadDword,                             ShaderInstructionType::BufferLoadDword,      ShaderInstructionFormat::Vdata1Vaddr2SvSoffsOffenIdxen, {""}},
    {Recompile_BufferLoadDwordx2_Vdata2VaddrSvSoffsIdxen,   ShaderInstructionType::BufferLoadDwordx2,    ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxen,        {""}},
    {Recompile_BufferLoadDwordx3_Vdata3VaddrSvSoffsIdxen,   ShaderInstructionType::BufferLoadDwordx3,    ShaderInstructionFormat::Vdata3VaddrSvSoffsIdxen,        {""}},
    {Recompile_BufferLoadDwordx4_Vdata4VaddrSvSoffsIdxen,   ShaderInstructionType::BufferLoadDwordx4,    ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen,        {""}},
    {Recompile_BufferLoadFormatX_Vdata1VaddrSvSoffsIdxen,   ShaderInstructionType::BufferLoadFormatX,    ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen,        {""}},
    {Recompile_BufferLoadFormatXyzw_Vdata4VaddrSvSoffsIdxen, ShaderInstructionType::BufferLoadFormatXyzw, ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen,        {""}},
    {Recompile_BufferStoreDword_Vdata1VaddrSvSoffsIdxen,    ShaderInstructionType::BufferStoreDword,     ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen,        {""}},
    {Recompile_BufferStoreDword_Vdata1VaddrSvSoffsIdxen,    ShaderInstructionType::BufferStoreDword,     ShaderInstructionFormat::Vdata1VaddrSvSoffsOffen,        {""}},
    {Recompile_BufferStoreDwordx2_Vdata2VaddrSvSoffsIdxen,  ShaderInstructionType::BufferStoreDwordx2,   ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxen,        {""}},
    {Recompile_BufferStoreDwordx3_Vdata3VaddrSvSoffsIdxen,  ShaderInstructionType::BufferStoreDwordx3,   ShaderInstructionFormat::Vdata3VaddrSvSoffsIdxen,        {""}},
    {Recompile_BufferStoreDwordx4_Vdata4VaddrSvSoffsIdxen,  ShaderInstructionType::BufferStoreDwordx4,   ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen,        {""}},
    {Recompile_BufferStoreFormatX_Vdata1VaddrSvSoffsIdxen,  ShaderInstructionType::BufferStoreFormatX,   ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen,        {""}},
    {Recompile_BufferStoreFormatXy_Vdata2VaddrSvSoffsIdxen, ShaderInstructionType::BufferStoreFormatXy,  ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxen,        {""}},
    {Recompile_BufferStoreFormatXyzw_Vdata4VaddrSvSoffsIdxen, ShaderInstructionType::BufferStoreFormatXyzw, ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen,      {""}},

    {Recompile_Fetch,                                       ShaderInstructionType::FetchX,               ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxen,        {""}},
    {Recompile_Fetch,                                       ShaderInstructionType::FetchXy,              ShaderInstructionFormat::Vdata2VaddrSvSoffsIdxen,        {""}},
    {Recompile_Fetch,                                       ShaderInstructionType::FetchXyz,             ShaderInstructionFormat::Vdata3VaddrSvSoffsIdxen,        {""}},
    {Recompile_Fetch,                                       ShaderInstructionType::FetchXyzw,            ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxen,        {""}},

    {Recompile_DsAppend_VdstGds,                           ShaderInstructionType::DsAppend,            ShaderInstructionFormat::VdstGds,                        {""}},
    {Recompile_DsConsume_VdstGds,                          ShaderInstructionType::DsConsume,           ShaderInstructionFormat::VdstGds,                        {""}},
    {Recompile_DsWriteB32_VaddrVdataOffset,                ShaderInstructionType::DsWriteB32,          ShaderInstructionFormat::VaddrVdataOffset,                {""}},
    {Recompile_DsRead2B32_Vdst2VaddrOffset01,              ShaderInstructionType::DsRead2B32,          ShaderInstructionFormat::Vdst2VaddrOffset01,              {""}},
    {Recompile_SBarrier_Empty,                             ShaderInstructionType::SBarrier,             ShaderInstructionFormat::Empty,                            {""}},

    {Recompile_Exp_MrtNullDone,                            ShaderInstructionType::Exp,                 ShaderInstructionFormat::Mrt0OffOffComprVmDone,          {""}},
    {Recompile_Exp_MrtNullDone,                            ShaderInstructionType::Exp,                 ShaderInstructionFormat::Mrt1OffOffComprVmDone,          {""}},
    {Recompile_Exp_MrtNullDone,                            ShaderInstructionType::Exp,                 ShaderInstructionFormat::Mrt2OffOffComprVmDone,          {""}},
    {Recompile_Exp_MrtNullDone,                            ShaderInstructionType::Exp,                 ShaderInstructionFormat::Mrt3OffOffComprVmDone,          {""}},
    {Recompile_Exp_Mrt_Compr_Vsrc0Vsrc1,                   ShaderInstructionType::Exp,                 ShaderInstructionFormat::Mrt0Vsrc0Vsrc1ComprVmDone,      {"outColor"}},
    {Recompile_Exp_Mrt_Compr_Vsrc0Vsrc1,                   ShaderInstructionType::Exp,                 ShaderInstructionFormat::Mrt1Vsrc0Vsrc1ComprVm,          {"outColor1"}},
    {Recompile_Exp_Mrt_Compr_Vsrc0Vsrc1,                   ShaderInstructionType::Exp,                 ShaderInstructionFormat::Mrt2Vsrc0Vsrc1ComprVm,          {"outColor2"}},
    {Recompile_Exp_Mrt_Compr_Vsrc0Vsrc1,                   ShaderInstructionType::Exp,                 ShaderInstructionFormat::Mrt3Vsrc0Vsrc1ComprVm,          {"outColor3"}},
    {Recompile_Exp_Mrt_Full_Vsrc0Vsrc1Vsrc2Vsrc3,          ShaderInstructionType::Exp,                 ShaderInstructionFormat::Mrt0Vsrc0Vsrc1Vsrc2Vsrc3VmDone, {"outColor"}},
    {Recompile_Exp_Mrt_Full_Vsrc0Vsrc1Vsrc2Vsrc3,          ShaderInstructionType::Exp,                 ShaderInstructionFormat::Mrt1Vsrc0Vsrc1Vsrc2Vsrc3Vm,     {"outColor1"}},
    {Recompile_Exp_Mrt_Full_Vsrc0Vsrc1Vsrc2Vsrc3,          ShaderInstructionType::Exp,                 ShaderInstructionFormat::Mrt2Vsrc0Vsrc1Vsrc2Vsrc3Vm,     {"outColor2"}},
    {Recompile_Exp_Mrt_Full_Vsrc0Vsrc1Vsrc2Vsrc3,          ShaderInstructionType::Exp,                 ShaderInstructionFormat::Mrt3Vsrc0Vsrc1Vsrc2Vsrc3Vm,     {"outColor3"}},
    {Recompile_Exp_Param_XXX_Vsrc0Vsrc1Vsrc2Vsrc3,         ShaderInstructionType::Exp,                 ShaderInstructionFormat::Param0Vsrc0Vsrc1Vsrc2Vsrc3,     {"param0"}},
    {Recompile_Exp_Param_XXX_Vsrc0Vsrc1Vsrc2Vsrc3,         ShaderInstructionType::Exp,                 ShaderInstructionFormat::Param1Vsrc0Vsrc1Vsrc2Vsrc3,     {"param1"}},
    {Recompile_Exp_Param_XXX_Vsrc0Vsrc1Vsrc2Vsrc3,         ShaderInstructionType::Exp,                 ShaderInstructionFormat::Param2Vsrc0Vsrc1Vsrc2Vsrc3,     {"param2"}},
    {Recompile_Exp_Param_XXX_Vsrc0Vsrc1Vsrc2Vsrc3,         ShaderInstructionType::Exp,                 ShaderInstructionFormat::Param3Vsrc0Vsrc1Vsrc2Vsrc3,     {"param3"}},
	{Recompile_Exp_Param_XXX_Vsrc0Vsrc1Vsrc2Vsrc3,         ShaderInstructionType::Exp,                 ShaderInstructionFormat::Param4Vsrc0Vsrc1Vsrc2Vsrc3,     {"param4"}},
	{Recompile_Exp_Param_XXX_Vsrc0Vsrc1Vsrc2Vsrc3,         ShaderInstructionType::Exp,                 ShaderInstructionFormat::Param5Vsrc0Vsrc1Vsrc2Vsrc3,     {"param5"}},
	{Recompile_Exp_Param_XXX_Vsrc0Vsrc1Vsrc2Vsrc3,         ShaderInstructionType::Exp,                 ShaderInstructionFormat::Param6Vsrc0Vsrc1Vsrc2Vsrc3,     {"param6"}},
    {Recompile_Exp_Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done,           ShaderInstructionType::Exp,                 ShaderInstructionFormat::Pos0Vsrc0Vsrc1Vsrc2Vsrc3Done,   {""}},
    {Recompile_Exp_PrimVsrc0OffOffOffDone,                 ShaderInstructionType::Exp,                 ShaderInstructionFormat::PrimVsrc0OffOffOffDone,         {""}},

    {Recompile_ImageLoad_Vdata1Vaddr3StDmask1,             ShaderInstructionType::ImageLoad,           ShaderInstructionFormat::Vdata1Vaddr3StDmask1,           {""}},
    {Recompile_ImageLoad_Vdata4Vaddr3StDmaskF,             ShaderInstructionType::ImageLoad,           ShaderInstructionFormat::Vdata4Vaddr3StDmaskF,           {""}},
    {Recompile_ImageSample_Vdata1Vaddr3StSsDmask1,         ShaderInstructionType::ImageSample,         ShaderInstructionFormat::Vdata1Vaddr3StSsDmask1,         {""}},
    {Recompile_ImageSample_Vdata1Vaddr3StSsDmask2,         ShaderInstructionType::ImageSample,         ShaderInstructionFormat::Vdata1Vaddr3StSsDmask2,         {""}},
    {Recompile_ImageSample_Vdata1Vaddr3StSsDmask4,         ShaderInstructionType::ImageSample,         ShaderInstructionFormat::Vdata1Vaddr3StSsDmask4,         {""}},
    {Recompile_ImageSample_Vdata1Vaddr3StSsDmask8,         ShaderInstructionType::ImageSample,         ShaderInstructionFormat::Vdata1Vaddr3StSsDmask8,         {""}},
    {Recompile_ImageSample_Vdata2Vaddr3StSsDmask3,         ShaderInstructionType::ImageSample,         ShaderInstructionFormat::Vdata2Vaddr3StSsDmask3,         {""}},
    {Recompile_ImageSample_Vdata2Vaddr3StSsDmask5,         ShaderInstructionType::ImageSample,         ShaderInstructionFormat::Vdata2Vaddr3StSsDmask5,         {""}},
    {Recompile_ImageSample_Vdata2Vaddr3StSsDmask9,         ShaderInstructionType::ImageSample,         ShaderInstructionFormat::Vdata2Vaddr3StSsDmask9,         {""}},
    {Recompile_ImageSample_Vdata2Vaddr3StSsDmaskA,         ShaderInstructionType::ImageSample,         ShaderInstructionFormat::Vdata2Vaddr3StSsDmaskA,         {""}},
    {Recompile_ImageSample_Vdata3Vaddr3StSsDmask7,         ShaderInstructionType::ImageSample,         ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7,         {""}},
    {Recompile_ImageSample_Vdata3Vaddr3StSsDmaskB,         ShaderInstructionType::ImageSample,         ShaderInstructionFormat::Vdata3Vaddr3StSsDmaskB,         {""}},
    {Recompile_ImageSample_Vdata4Vaddr3StSsDmaskF,         ShaderInstructionType::ImageSample,         ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF,         {""}},
	{Recompile_ImageSampleL_Vdata4Vaddr3StSsDmaskF,        ShaderInstructionType::ImageSampleL,        ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF,         {""}},
    {Recompile_ImageSampleLz_Vdata1Vaddr3StSsDmask1,       ShaderInstructionType::ImageSampleLz,       ShaderInstructionFormat::Vdata1Vaddr3StSsDmask1,         {""}},
    {Recompile_ImageSampleLz_Vdata2Vaddr3StSsDmask3,       ShaderInstructionType::ImageSampleLz,       ShaderInstructionFormat::Vdata2Vaddr3StSsDmask3,         {""}},
    {Recompile_ImageSampleLz_Vdata3Vaddr3StSsDmask7,       ShaderInstructionType::ImageSampleLz,       ShaderInstructionFormat::Vdata3Vaddr3StSsDmask7,         {""}},
    {Recompile_ImageSampleLz_Vdata4Vaddr3StSsDmaskF,       ShaderInstructionType::ImageSampleLz,       ShaderInstructionFormat::Vdata4Vaddr3StSsDmaskF,         {""}},
    {Recompile_ImageSampleLzO_Vdata3Vaddr4StSsDmask7,      ShaderInstructionType::ImageSampleLzO,      ShaderInstructionFormat::Vdata3Vaddr4StSsDmask7,         {""}},
    {Recompile_ImageStore_Vdata4Vaddr3StDmaskF,            ShaderInstructionType::ImageStore,          ShaderInstructionFormat::Vdata4Vaddr3StDmaskF,           {""}},
    {Recompile_ImageStoreMip_Vdata4Vaddr4StDmaskF,         ShaderInstructionType::ImageStoreMip,       ShaderInstructionFormat::Vdata4Vaddr4StDmaskF,           {""}},

    {Recompile_SBufferLoadDword_SdstSvSoffset,             ShaderInstructionType::SBufferLoadDword,    ShaderInstructionFormat::SdstSvSoffset,                  {""}},
    {Recompile_SBufferLoadDwordx2_Sdst2SvSoffset,          ShaderInstructionType::SBufferLoadDwordx2,  ShaderInstructionFormat::Sdst2SvSoffset,                 {""}},
    {Recompile_SBufferLoadDwordx4_Sdst4SvSoffset,          ShaderInstructionType::SBufferLoadDwordx4,  ShaderInstructionFormat::Sdst4SvSoffset,                 {""}},
    {Recompile_SBufferLoadDwordx8_Sdst8SvSoffset,          ShaderInstructionType::SBufferLoadDwordx8,  ShaderInstructionFormat::Sdst8SvSoffset,                 {""}},
    {Recompile_SBufferLoadDwordx16_Sdst16SvSoffset,        ShaderInstructionType::SBufferLoadDwordx16, ShaderInstructionFormat::Sdst16SvSoffset,                {""}},

    {Recompile_SCbranch_XXX_Label,                         ShaderInstructionType::SCbranchExecz,       ShaderInstructionFormat::Label,                          {"%cc_u_<index> = OpLoad %uint %execz",  "%cc_b_<index> = OpINotEqual %bool %cc_u_<index> %uint_0"}},
    {Recompile_SCbranch_XXX_Label,                         ShaderInstructionType::SCbranchScc0,        ShaderInstructionFormat::Label,                          {"%cc_u_<index> = OpLoad %uint %scc",    "%cc_b_<index> = OpIEqual    %bool %cc_u_<index> %uint_0"}},
    {Recompile_SCbranch_XXX_Label,                         ShaderInstructionType::SCbranchScc1,        ShaderInstructionFormat::Label,                          {"%cc_u_<index> = OpLoad %uint %scc",    "%cc_b_<index> = OpIEqual    %bool %cc_u_<index> %uint_1"}},
    {Recompile_SCbranch_XXX_Label,                         ShaderInstructionType::SCbranchVccz,        ShaderInstructionFormat::Label,                          {"%cc_u_<index> = OpLoad %uint %vcc_lo", "%cc_b_<index> = OpIEqual    %bool %cc_u_<index> %uint_0"}},
    {Recompile_SCbranch_XXX_Label,                         ShaderInstructionType::SCbranchVccnz,       ShaderInstructionFormat::Label,                          {"%cc_u_<index> = OpLoad %uint %vcc_lo", "%cc_b_<index> = OpINotEqual %bool %cc_u_<index> %uint_0"}},
    {Recompile_SBranch_Label,                              ShaderInstructionType::SBranch,             ShaderInstructionFormat::Label,                          {""}},

    {Recompile_SEndpgm_Empty,                              ShaderInstructionType::SEndpgm,             ShaderInstructionFormat::Empty,                          {""}},

	{Recompile_SLoadDword_SdstSbaseSoffset,                ShaderInstructionType::SLoadDword,          ShaderInstructionFormat::SdstSbaseSoffset,               {""}},
	{Recompile_SLoadDwordx2_Sdst2Ssrc02Ssrc1,              ShaderInstructionType::SLoadDwordx2,        ShaderInstructionFormat::Sdst2Ssrc02Ssrc1,               {""}},
    {Recompile_SLoadDwordx4_Sdst4SbaseSoffset,             ShaderInstructionType::SLoadDwordx4,        ShaderInstructionFormat::Sdst4SbaseSoffset,              {""}},
    {Recompile_SLoadDwordx8_Sdst8SbaseSoffset,             ShaderInstructionType::SLoadDwordx8,        ShaderInstructionFormat::Sdst8SbaseSoffset,              {""}},

    {Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12, ShaderInstructionType::SAndn2B64,   ShaderInstructionFormat::Sdst2Ssrc02Ssrc12, {"%ta_<index> = OpNot %uint %t2_<index>",
                                                                                                                             "%tb_<index> = OpBitwiseAnd %uint %t0_<index> %ta_<index>",
                                                                                                                             "%tc_<index> = OpNot %uint %t3_<index>",
                                                                                                                             "%td_<index> = OpBitwiseAnd %uint %t1_<index> %tc_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12, ShaderInstructionType::SOrn2B64,    ShaderInstructionFormat::Sdst2Ssrc02Ssrc12, {"%ta_<index> = OpNot %uint %t2_<index>",
                                                                                                                             "%tb_<index> = OpBitwiseOr %uint %t0_<index> %ta_<index>",
                                                                                                                             "%tc_<index> = OpNot %uint %t3_<index>",
                                                                                                                             "%td_<index> = OpBitwiseOr %uint %t1_<index> %tc_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12, ShaderInstructionType::SAndB64,     ShaderInstructionFormat::Sdst2Ssrc02Ssrc12, {"%tb_<index> = OpBitwiseAnd %uint %t0_<index> %t2_<index>",
                                                                                                                             "%td_<index> = OpBitwiseAnd %uint %t1_<index> %t3_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12, ShaderInstructionType::SNorB64,     ShaderInstructionFormat::Sdst2Ssrc02Ssrc12, {"%ta_<index> = OpBitwiseOr %uint %t0_<index> %t2_<index>",
                                                                                                                             "%tb_<index> = OpNot %uint %ta_<index>",
                                                                                                                             "%tc_<index> = OpBitwiseOr %uint %t1_<index> %t3_<index>",
                                                                                                                             "%td_<index> = OpNot %uint %tc_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12, ShaderInstructionType::SNandB64,    ShaderInstructionFormat::Sdst2Ssrc02Ssrc12, {"%ta_<index> = OpBitwiseAnd %uint %t0_<index> %t2_<index>",
                                                                                                                             "%tb_<index> = OpNot %uint %ta_<index>",
                                                                                                                             "%tc_<index> = OpBitwiseAnd %uint %t1_<index> %t3_<index>",
                                                                                                                             "%td_<index> = OpNot %uint %tc_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12, ShaderInstructionType::SXnorB64,    ShaderInstructionFormat::Sdst2Ssrc02Ssrc12, {"%ta_<index> = OpBitwiseXor %uint %t0_<index> %t2_<index>",
                                                                                                                             "%tb_<index> = OpNot %uint %ta_<index>",
                                                                                                                             "%tc_<index> = OpBitwiseXor %uint %t1_<index> %t3_<index>",
                                                                                                                             "%td_<index> = OpNot %uint %tc_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12, ShaderInstructionType::SOrB64,      ShaderInstructionFormat::Sdst2Ssrc02Ssrc12, {"%tb_<index> = OpBitwiseOr %uint %t0_<index> %t2_<index>",
                                                                                                                             "%td_<index> = OpBitwiseOr %uint %t1_<index> %t3_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12, ShaderInstructionType::SXorB64,     ShaderInstructionFormat::Sdst2Ssrc02Ssrc12, {"%tb_<index> = OpBitwiseXor %uint %t0_<index> %t2_<index>",
                                                                                                                             "%td_<index> = OpBitwiseXor %uint %t1_<index> %t3_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B64_Sdst2Ssrc02Ssrc12, ShaderInstructionType::SCselectB64, ShaderInstructionFormat::Sdst2Ssrc02Ssrc12, {"%ts_<index> = OpLoad %uint %scc",
                                                                                                                             "%tsb_<index> = OpINotEqual %bool %ts_<index> %uint_0",
                                                                                                                             "%tb_<index> = OpSelect %uint %tsb_<index> %t0_<index> %t2_<index>",
                                                                                                                             "%td_<index> = OpSelect %uint %tsb_<index> %t1_<index> %t3_<index>" }, SccCheck::None},

    {Recompile_S_Bfe_U64_Sdst2Ssrc02Ssrc1,  ShaderInstructionType::SBfeU64,     ShaderInstructionFormat::Sdst2Ssrc02Ssrc1,  {"", ""} , SccCheck::NonZero},
    {Recompile_S_Lshl_B64_Sdst2Ssrc02Ssrc1, ShaderInstructionType::SLshlB64,    ShaderInstructionFormat::Sdst2Ssrc02Ssrc1,  {"", ""} , SccCheck::NonZero},
    {Recompile_S_Lshr_B64_Sdst2Ssrc02Ssrc1, ShaderInstructionType::SLshrB64,    ShaderInstructionFormat::Sdst2Ssrc02Ssrc1,  {"", ""} , SccCheck::NonZero},

    {Recompile_S_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SAndB32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpBitwiseAnd %uint %t0_<index> %t1_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SBfmB32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%tcount_<index> = OpBitwiseAnd %uint %t0_<index> %uint_31", "%toffset_<index> = OpBitwiseAnd %uint %t1_<index> %uint_31", "%t_<index> = OpBitFieldInsert %uint %uint_0 %uint_0xffffffff %toffset_<index> %tcount_<index>"}, SccCheck::None},
    {Recompile_S_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SCselectB32,     ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t22_<index> = OpLoad %uint %scc", "%t2_<index> = OpINotEqual %bool %t22_<index> %uint_0", "%t_<index> = OpSelect %uint %t2_<index> %t0_<index> %t1_<index>"}, SccCheck::None},
    {Recompile_S_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SLshlB32,        ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%ts_<index> = OpBitwiseAnd %uint %t1_<index> %uint_31", "%t_<index> = OpShiftLeftLogical %uint %t0_<index> %ts_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SLshrB32,        ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%ts_<index> = OpBitwiseAnd %uint %t1_<index> %uint_31", "%t_<index> = OpShiftRightLogical %uint %t0_<index> %ts_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SOrB32,          ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpBitwiseOr %uint %t0_<index> %t1_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_I32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SAddI32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpIAdd %int %t0_<index> %t1_<index>"}, SccCheck::OverflowAdd},
    {Recompile_S_XXX_I32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SMulI32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpIMul %int %t0_<index> %t1_<index>"}, SccCheck::None},
    {Recompile_S_XXX_I32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SSubI32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpISub %int %t0_<index> %t1_<index>"}, SccCheck::OverflowSub},
    {Recompile_S_XXX_U32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SAddcU32,        ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%tscc_<index> = OpLoad %uint %scc", "%ts_<index> = OpFunctionCall %v2uint %addc %t0_<index> %t1_<index> %tscc_<index>", "%t_<index> = OpCompositeExtract %uint %ts_<index> 0", "%carry_<index> = OpCompositeExtract %uint %ts_<index> 1"}, SccCheck::CarryOut},
    {Recompile_S_XXX_U32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SAddU32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%ts_<index> = OpIAddCarry %ResTypeU %t0_<index> %t1_<index>", "%t_<index> = OpCompositeExtract %uint %ts_<index> 0", "%carry_<index> = OpCompositeExtract %uint %ts_<index> 1"}, SccCheck::CarryOut},
    {Recompile_S_XXX_U32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SBfeU32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%to_<index> = OpBitFieldUExtract %uint %t1_<index> %uint_0  %uint_5", "%ts_<index> = OpBitFieldUExtract %uint %t1_<index> %uint_16 %uint_7", "%t_<index> = OpBitFieldUExtract %uint %t0_<index> %to_<index> %ts_<index>"}, SccCheck::NonZero},
    {Recompile_S_XXX_U32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SLshl4AddU32,    ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%ts_<index> = OpFunctionCall %v2uint %lshl_add %t0_<index> %t1_<index> %uint_4", "%t_<index> = OpCompositeExtract %uint %ts_<index> 0", "%carry_<index> = OpCompositeExtract %uint %ts_<index> 1"}, SccCheck::CarryOut},
    {Recompile_S_XXX_U32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::SMulHiU32,       ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpFunctionCall %uint %mul_hi_uint %t0_<index> %t1_<index>"}, SccCheck::None},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VAndB32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpBitwiseAnd %uint %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VAddI32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpIAdd %uint %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VSubI32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpISub %uint %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VBcntU32B32,     ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%tb_<index> = OpBitCount %int %t0_<index>", "%tbu_<index> = OpBitcast %uint %tb_<index>", "%t_<index> = OpIAdd %uint %tbu_<index> %t1_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VBfmB32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%tcount_<index> = OpBitwiseAnd %uint %t0_<index> %uint_31", "%toffset_<index> = OpBitwiseAnd %uint %t1_<index> %uint_31", "%t_<index> = OpBitFieldInsert %uint %uint_0 %uint_0xffffffff %toffset_<index> %tcount_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VLshlB32,        ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%ts_<index> = OpBitwiseAnd %uint %t1_<index> %uint_31", "%t_<index> = OpShiftLeftLogical %uint %t0_<index> %ts_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VLshlrevB32,     ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%ts_<index> = OpBitwiseAnd %uint %t0_<index> %uint_31", "%t_<index> = OpShiftLeftLogical %uint %t1_<index> %ts_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VLshrB32,        ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%ts_<index> = OpBitwiseAnd %uint %t1_<index> %uint_31", "%t_<index> = OpShiftRightLogical %uint %t0_<index> %ts_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VLshrrevB32,     ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%ts_<index> = OpBitwiseAnd %uint %t0_<index> %uint_31", "%t_<index> = OpShiftRightLogical %uint %t1_<index> %ts_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VMulHiU32,       ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpFunctionCall %uint %mul_hi_uint %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VMulLoU32,       ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpFunctionCall %uint %mul_lo_uint %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VMulU32U24,      ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%tu0_<index> = OpBitwiseAnd %uint %t0_<index> %uint_0x00ffffff", "%tu1_<index> = OpBitwiseAnd %uint %t1_<index> %uint_0x00ffffff", "%t_<index> = OpFunctionCall %uint %mul_lo_uint %tu0_<index> %tu1_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VMinU32,          ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpExtInst %uint %GLSL_std_450 UMin %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VOrB32,          ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpBitwiseOr %uint %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VXnorB32,        ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%tx_<index> = OpBitwiseXor %uint %t0_<index> %t1_<index>", "%t_<index> = OpNot %uint %tx_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VXorB32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpBitwiseXor %uint %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VAddF32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpFAdd %float %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VMacF32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpExtInst %float %GLSL_std_450 Fma %t0_<index> %t1_<index> %tdst_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VMaxF32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpExtInst %float %GLSL_std_450 FMax %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VMinF32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpExtInst %float %GLSL_std_450 FMin %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VMulF32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpFMul %float %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VSubF32,         ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpFSub %float %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VSubrevF32,      ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpFSub %float %t1_<index> %t0_<index>"}},
    {Recompile_V_XXX_I32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VAshrI32,        ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%ts_<index> = OpBitwiseAnd %int %t1_<index> %int_31", "%t_<index> = OpShiftRightArithmetic %int %t0_<index> %ts_<index>"}},
    {Recompile_V_XXX_I32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VAshrrevI32,     ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%ts_<index> = OpBitwiseAnd %int %t0_<index> %int_31", "%t_<index> = OpShiftRightArithmetic %int %t1_<index> %ts_<index>"}},
    {Recompile_V_XXX_I32_SVdstSVsrc0SVsrc1,       ShaderInstructionType::VMulLoI32,       ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {"%t_<index> = OpFunctionCall %int %mul_lo_int %t0_<index> %t1_<index>"}},
    {Recompile_VCvtPkrtzF16F32_SVdstSVsrc0SVsrc1, ShaderInstructionType::VCvtPkrtzF16F32, ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {""}},
    {Recompile_VMbcntHiU32B32_SVdstSVsrc0SVsrc1,  ShaderInstructionType::VMbcntHiU32B32,  ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {""}},
    {Recompile_VMbcntLoU32B32_SVdstSVsrc0SVsrc1,  ShaderInstructionType::VMbcntLoU32B32,  ShaderInstructionFormat::SVdstSVsrc0SVsrc1,  {""}},


    {Recompile_SMovB32_SVdstSVsrc0,            ShaderInstructionType::SMovB32,             ShaderInstructionFormat::SVdstSVsrc0, {""}},
    {Recompile_SMovB32_SVdstSVsrc0,            ShaderInstructionType::SMovkI32,            ShaderInstructionFormat::SVdstSVsrc0, {""}},
    {Recompile_SMulkI32_SVdstSVsrc0,           ShaderInstructionType::SMulkI32,            ShaderInstructionFormat::SVdstSVsrc0, {""}},
    {Recompile_V_XXX_B32_SVdstSVsrc0,          ShaderInstructionType::VBfrevB32,           ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpBitReverse %uint %t0_<index>"}},
    {Recompile_V_XXX_B32_SVdstSVsrc0,          ShaderInstructionType::VNotB32,             ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpNot %uint %t0_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VCeilF32,            ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpExtInst %float %GLSL_std_450 Ceil %t0_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VCosF32,             ShaderInstructionFormat::SVdstSVsrc0, {"%tr_<index> = OpFMul %float %t0_<index> %float_2pi", "%t_<index> = OpExtInst %float %GLSL_std_450 Cos %tr_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VExpF32,             ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpExtInst %float %GLSL_std_450 Exp2 %t0_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VFloorF32,           ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpExtInst %float %GLSL_std_450 Floor %t0_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VFractF32,           ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpExtInst %float %GLSL_std_450 Fract %t0_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VLogF32,             ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpExtInst %float %GLSL_std_450 Log2 %t0_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VRcpF32,             ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpFDiv %float %float_1_000000 %t0_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VRndneF32,           ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpExtInst %float %GLSL_std_450 RoundEven %t0_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VRsqF32,             ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpExtInst %float %GLSL_std_450 InverseSqrt %t0_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VSinF32,             ShaderInstructionFormat::SVdstSVsrc0, {"%tr_<index> = OpFMul %float %t0_<index> %float_2pi", "%t_<index> = OpExtInst %float %GLSL_std_450 Sin %tr_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VSqrtF32,            ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpExtInst %float %GLSL_std_450 Sqrt %t0_<index>"}},
    {Recompile_V_XXX_F32_SVdstSVsrc0,          ShaderInstructionType::VTruncF32,           ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpExtInst %float %GLSL_std_450 Trunc %t0_<index>"}},
    {Recompile_VCvt_XXX_F32_SVdstSVsrc0,       ShaderInstructionType::VCvtU32F32,          ShaderInstructionFormat::SVdstSVsrc0, {"%t1_<index> = OpExtInst %float %GLSL_std_450 Trunc %t0_<index>", "%t2_<index> = OpConvertFToU %uint %t1_<index>"}},
    // v_cvt_i32_f32: trunc toward zero, convert to signed int; result bits in t2
    // (uint) so the shared template can OpBitcast %float %t2 for VGPR storage.
    {Recompile_VCvt_XXX_F32_SVdstSVsrc0,       ShaderInstructionType::VCvtI32F32,          ShaderInstructionFormat::SVdstSVsrc0, {"%t1_<index> = OpExtInst %float %GLSL_std_450 Trunc %t0_<index>", "%ti_<index> = OpConvertFToS %int %t1_<index>", "%t2_<index> = OpBitcast %uint %ti_<index>"}},
    {Recompile_VCvtF32_XXX_SVdstSVsrc0,        ShaderInstructionType::VCvtF32F16,          ShaderInstructionFormat::SVdstSVsrc0, {"%ts_<index> = OpExtInst %v2float %GLSL_std_450 UnpackHalf2x16 %t0_<index>", "%t_<index> = OpCompositeExtract %float %ts_<index> 0"}},
    {Recompile_VCvtF32_XXX_SVdstSVsrc0,        ShaderInstructionType::VCvtF32I32,          ShaderInstructionFormat::SVdstSVsrc0, {"%ti_<index> = OpBitcast %int %t0_<index>", "%t_<index> = OpConvertSToF %float %ti_<index>"}},
    {Recompile_VCvtF32_XXX_SVdstSVsrc0,        ShaderInstructionType::VCvtF32U32,          ShaderInstructionFormat::SVdstSVsrc0, {"%t_<index> = OpConvertUToF %float %t0_<index>"}},
    {Recompile_VCvtF32_XXX_SVdstSVsrc0,        ShaderInstructionType::VCvtF32Ubyte0,       ShaderInstructionFormat::SVdstSVsrc0, {"%tb_<index> = OpBitFieldUExtract %uint %t0_<index> %uint_0 %uint_8", "%t_<index> = OpConvertUToF %float %tb_<index>"}},
    {Recompile_VCvtF32_XXX_SVdstSVsrc0,        ShaderInstructionType::VCvtF32Ubyte1,       ShaderInstructionFormat::SVdstSVsrc0, {"%tb_<index> = OpBitFieldUExtract %uint %t0_<index> %uint_8 %uint_8", "%t_<index> = OpConvertUToF %float %tb_<index>"}},
    {Recompile_VCvtF32_XXX_SVdstSVsrc0,        ShaderInstructionType::VCvtF32Ubyte2,       ShaderInstructionFormat::SVdstSVsrc0, {"%tb_<index> = OpBitFieldUExtract %uint %t0_<index> %uint_16 %uint_8", "%t_<index> = OpConvertUToF %float %tb_<index>"}},
    {Recompile_VCvtF32_XXX_SVdstSVsrc0,        ShaderInstructionType::VCvtF32Ubyte3,       ShaderInstructionFormat::SVdstSVsrc0, {"%tb_<index> = OpBitFieldUExtract %uint %t0_<index> %uint_24 %uint_8", "%t_<index> = OpConvertUToF %float %tb_<index>"}},
    {Recompile_VMovB32_SVdstSVsrc0,            ShaderInstructionType::VMovB32,             ShaderInstructionFormat::SVdstSVsrc0, {""}},
    {Recompile_VNop,                           ShaderInstructionType::VNop,                ShaderInstructionFormat::Empty,       {""}},

    {Recompile_SSaveexecB64_Sdst2Ssrc02,       ShaderInstructionType::SAndSaveexecB64,     ShaderInstructionFormat::Sdst2Ssrc02, {""}, SccCheck::ExecNonZero},
    {Recompile_SSaveexecB64_Sdst2Ssrc02,       ShaderInstructionType::SAndn1SaveexecB64,   ShaderInstructionFormat::Sdst2Ssrc02, {""}, SccCheck::ExecNonZero},
    {Recompile_SMovB64_Sdst2Ssrc02,            ShaderInstructionType::SMovB64,             ShaderInstructionFormat::Sdst2Ssrc02, {""}},
    {Recompile_SNotB32_SVdstSVsrc0,            ShaderInstructionType::SNotB32,             ShaderInstructionFormat::SVdstSVsrc0, {""}, SccCheck::NonZero},
    {Recompile_SNotB64_Sdst2Ssrc02,            ShaderInstructionType::SNotB64,             ShaderInstructionFormat::Sdst2Ssrc02, {""}, SccCheck::NonZero},
    {Recompile_SSwappcB64_Sdst2Ssrc02,         ShaderInstructionType::SSwappcB64,          ShaderInstructionFormat::Sdst2Ssrc02, {""}},
    {Recompile_SWqmB64_Sdst2Ssrc02,            ShaderInstructionType::SWqmB64,             ShaderInstructionFormat::Sdst2Ssrc02, {""}, SccCheck::NonZero},

	{Recompile_Skip,                           ShaderInstructionType::SInstPrefetch,       ShaderInstructionFormat::Imm,         {""}},
	{Recompile_Skip,                           ShaderInstructionType::SSendmsg,            ShaderInstructionFormat::Imm,         {""}},
    {Recompile_Skip,                           ShaderInstructionType::SWaitcnt,            ShaderInstructionFormat::Imm,         {""}},

    {Recompile_TBufferLoadFormatX_Vdata1VaddrSvSoffsIdxenFloat1,          ShaderInstructionType::TBufferLoadFormatX,    ShaderInstructionFormat::Vdata1VaddrSvSoffsIdxenFloat1,  {""}},
    {Recompile_TBufferLoadFormatXyzw_Vdata4Vaddr2SvSoffsOffenIdxenFloat4, ShaderInstructionType::TBufferLoadFormatXyzw, ShaderInstructionFormat::Vdata4Vaddr2SvSoffsOffenIdxenFloat4,  {""}},
    {Recompile_TBufferLoadFormatXyzw_Vdata4VaddrSvSoffsIdxenFloat4,       ShaderInstructionType::TBufferLoadFormatXyzw, ShaderInstructionFormat::Vdata4VaddrSvSoffsIdxenFloat4,  {""}},

    {Recompile_V_XXX_U32_VdstSdst2Vsrc0Vsrc1,  ShaderInstructionType::VAddI32,    ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1,  {"%t_<index> = OpIAddCarry %ResTypeU %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_U32_VdstSdst2Vsrc0Vsrc1,  ShaderInstructionType::VSubI32,    ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1,  {"%t_<index> = OpISubBorrow %ResTypeU %t0_<index> %t1_<index>"}},
    {Recompile_V_XXX_U32_VdstSdst2Vsrc0Vsrc1,  ShaderInstructionType::VSubrevI32, ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1,  {"%t_<index> = OpISubBorrow %ResTypeU %t1_<index> %t0_<index>"}},
    {Recompile_V_XXX_U32_VdstSdst2Vsrc0Vsrc1Ssrc2, ShaderInstructionType::VAddCoCiU32, ShaderInstructionFormat::VdstSdst2Vsrc0Vsrc1Ssrc2A2, {""}},
    {Recompile_VMadU64U32_Vdst2Sdst2Vsrc0Vsrc1Vsrc2Pair, ShaderInstructionType::VMadU64U32, ShaderInstructionFormat::Vdst2Sdst2Vsrc0Vsrc1Vsrc2Pair, {""}},

    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpEqF32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFOrdEqual"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpFF32,     ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpIEqual %bool %uint_0 %uint_1 ; "}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpGeF32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFOrdGreaterThanEqual"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpGtF32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFOrdGreaterThan"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpLeF32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFOrdLessThanEqual"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpLgF32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFOrdNotEqual"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpLtF32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFOrdLessThan"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpNeqF32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFUnordNotEqual"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpNgeF32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFUnordLessThan"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpNgtF32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFUnordLessThanEqual"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpNleF32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFUnordGreaterThan"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpNlgF32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFUnordEqual"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpNltF32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFUnordGreaterThanEqual"}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpOF32,     ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFunctionCall %bool %ordered %t0_<index> %t1_<index> ; "}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpTruF32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpIEqual %bool %uint_0 %uint_0 ; "}},
    {Recompile_VCmp_XXX_F32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpUF32,     ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFunctionCall %bool %unordered %t0_<index> %t1_<index> ; "}},
    {Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpEqI32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpIEqual"}},
    {Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpEqU32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpIEqual"}},
    {Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpFI32,     ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpIEqual %bool %uint_0 %uint_1 ; "}},
    {Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpGeI32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpSGreaterThanEqual"}},
    {Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpGtI32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpSGreaterThan"}},
    {Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpLeI32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpSLessThanEqual"}},
    {Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpLtI32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpSLessThan"}},
    {Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpNeI32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpINotEqual"}},
    {Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpNeU32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpINotEqual"}},
    {Recompile_VCmp_XXX_I32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpTI32,     ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpIEqual %bool %uint_0 %uint_0 ; "}},
    {Recompile_VCmp_XXX_U32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpFU32,     ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpIEqual %bool %uint_0 %uint_1 ; "}},
    {Recompile_VCmp_XXX_U32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpGeU32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpUGreaterThanEqual"}},
    {Recompile_VCmp_XXX_U32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpGtU32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpUGreaterThan"}},
    {Recompile_VCmp_XXX_U32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpLeU32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpULessThanEqual"}},
    {Recompile_VCmp_XXX_U32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpLtU32,    ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpULessThan"}},
    {Recompile_VCmp_XXX_U32_SmaskVsrc0Vsrc1,  ShaderInstructionType::VCmpTU32,     ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpIEqual %bool %uint_0 %uint_0 ; "}},
    {Recompile_VCmpx_XXX_F32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxNeqF32,  ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFUnordNotEqual"}},
    {Recompile_VCmpx_XXX_F32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxGtF32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFOrdGreaterThan"}},
    {Recompile_VCmpx_XXX_F32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxLtF32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFOrdLessThan"}},
    {Recompile_VCmpx_XXX_F32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxNleF32,  ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFUnordGreaterThan"}},
    {Recompile_VCmpx_XXX_F32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxNltF32,  ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpFUnordGreaterThanEqual"}},
    {Recompile_VCmpx_XXX_I32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxEqU32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpIEqual"}},
    {Recompile_VCmpx_XXX_I32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxNeU32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpINotEqual"}},
    {Recompile_VCmpx_XXX_I32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxLtI32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpSLessThan"}},
    {Recompile_VCmpx_XXX_I32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxEqI32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpIEqual"}},
    {Recompile_VCmpx_XXX_I32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxLeI32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpSLessThanEqual"}},
    {Recompile_VCmpx_XXX_I32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxGtI32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpSGreaterThan"}},
    {Recompile_VCmpx_XXX_I32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxNeI32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpINotEqual"}},
    {Recompile_VCmpx_XXX_I32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxGeI32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpSGreaterThanEqual"}},
    {Recompile_VCmpx_XXX_U32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxGeU32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpUGreaterThanEqual"}},
    {Recompile_VCmpx_XXX_U32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxGtU32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpUGreaterThan"}},
    {Recompile_VCmpx_XXX_U32_SmaskVsrc0Vsrc1, ShaderInstructionType::VCmpxLtU32,   ShaderInstructionFormat::SmaskVsrc0Vsrc1,      {"OpULessThan"}},

    {Recompile_SCmp_XXX_I32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpEqI32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpIEqual"}},
    {Recompile_SCmp_XXX_I32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpGeI32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpSGreaterThanEqual"}},
    {Recompile_SCmp_XXX_I32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpGtI32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpSGreaterThan"}},
    {Recompile_SCmp_XXX_I32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpLgI32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpINotEqual"}},
    {Recompile_SCmp_XXX_I32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpLtI32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpSLessThan"}},
    {Recompile_SCmp_XXX_I32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpLeI32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpSLessThanEqual"}},
    {Recompile_SCmp_XXX_U32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpEqU32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpIEqual"}},
    {Recompile_SCmp_XXX_U32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpGeU32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpUGreaterThanEqual"}},
    {Recompile_SCmp_XXX_U32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpGtU32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpUGreaterThan"}},
    {Recompile_SCmp_XXX_U32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpLeU32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpULessThanEqual"}},
    {Recompile_SCmp_XXX_U32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpLtU32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpULessThan"}},
    {Recompile_SCmp_XXX_U32_Ssrc0Ssrc1,  ShaderInstructionType::SCmpLgU32,    ShaderInstructionFormat::Ssrc0Ssrc1,      {"OpINotEqual"}},

    {Recompile_VCndmaskB32_VdstVsrc0Vsrc1Smask2,   ShaderInstructionType::VCndmaskB32,  ShaderInstructionFormat::VdstVsrc0Vsrc1Smask2, {""}},

    {Recompile_VInterpMovF32_VdstVsrcAttrChan,     ShaderInstructionType::VInterpMovF32, ShaderInstructionFormat::VdstVsrcAttrChan,    {""}},
    {Recompile_VInterpP1F32_VdstVsrcAttrChan,      ShaderInstructionType::VInterpP1F32, ShaderInstructionFormat::VdstVsrcAttrChan,     {""}},
    {Recompile_VInterpP2F32_VdstVsrcAttrChan,      ShaderInstructionType::VInterpP2F32, ShaderInstructionFormat::VdstVsrcAttrChan,     {""}},

    {Recompile_V_XXX_F32_VdstVsrc0Vsrc1Vsrc2,  ShaderInstructionType::VMadF32,    ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,  {"%t_<index> = OpExtInst %float %GLSL_std_450 Fma %t0_<index> %t1_<index> %t2_<index>"}},
    {Recompile_V_XXX_F32_VdstVsrc0Vsrc1Vsrc2,  ShaderInstructionType::VFmaF32,    ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,  {"%t_<index> = OpExtInst %float %GLSL_std_450 Fma %t0_<index> %t1_<index> %t2_<index>"}},
    {Recompile_V_XXX_F32_VdstVsrc0Vsrc1Vsrc2,  ShaderInstructionType::VMadakF32,  ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,  {"%t_<index> = OpExtInst %float %GLSL_std_450 Fma %t0_<index> %t1_<index> %t2_<index>"}},
    {Recompile_V_XXX_F32_VdstVsrc0Vsrc1Vsrc2,  ShaderInstructionType::VMadmkF32,  ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,  {"%t_<index> = OpExtInst %float %GLSL_std_450 Fma %t0_<index> %t1_<index> %t2_<index>"}},
    {Recompile_V_XXX_F32_VdstVsrc0Vsrc1Vsrc2,  ShaderInstructionType::VMax3F32,   ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,  {"%tm_<index> = OpExtInst %float %GLSL_std_450 FMax %t0_<index> %t1_<index>",
                                                                                                                                  "%t_<index> = OpExtInst %float %GLSL_std_450 FMax %tm_<index> %t2_<index>"}},
    {Recompile_V_XXX_F32_VdstVsrc0Vsrc1Vsrc2,  ShaderInstructionType::VMin3F32,   ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,  {"%tm_<index> = OpExtInst %float %GLSL_std_450 FMin %t0_<index> %t1_<index>",
                                                                                                                                  "%t_<index> = OpExtInst %float %GLSL_std_450 FMin %tm_<index> %t2_<index>"}},
    {Recompile_V_XXX_F32_VdstVsrc0Vsrc1Vsrc2,  ShaderInstructionType::VMed3F32,   ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,  {"%t3_<index> = OpExtInst %float %GLSL_std_450 FMin %t0_<index> %t1_<index>",
                                                                                                                                  "%t4_<index> = OpExtInst %float %GLSL_std_450 FMax %t0_<index> %t1_<index>",
                                                                                                                                  "%t5_<index> = OpExtInst %float %GLSL_std_450 FMin %t4_<index> %t2_<index>",
                                                                                                                                  "%t_<index> = OpExtInst %float %GLSL_std_450 FMax %t3_<index> %t5_<index>"}},
    {Recompile_V_XXX_U32_VdstVsrc0Vsrc1Vsrc2,  ShaderInstructionType::VSadU32,    ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,  {"%td_<index> = OpFunctionCall %uint %abs_diff %t0_<index> %t1_<index>",
                                                                                                                                  "%t_<index> = OpIAdd %uint %td_<index> %t2_<index>"}},
    {Recompile_V_XXX_U32_VdstVsrc0Vsrc1Vsrc2,  ShaderInstructionType::VBfeU32,    ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,  {"%to_<index> = OpBitwiseAnd %uint %t1_<index> %uint_31",
                                                                                                                                  "%ts_<index> = OpBitwiseAnd %uint %t2_<index> %uint_31",
                                                                                                                                  "%t_<index> = OpBitFieldUExtract %uint %t0_<index> %to_<index> %ts_<index>"}},
	// v_bfe_i32: signed bitfield extract; offset/width masked to 5 bits like u32 form.
	// RecompilerFunc param array is fixed at 4 strings — pack intermediate ops.
	{Recompile_V_XXX_U32_VdstVsrc0Vsrc1Vsrc2, ShaderInstructionType::VBfeI32, ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,
	 {"%to_<index> = OpBitwiseAnd %uint %t1_<index> %uint_31", "%ts_<index> = OpBitwiseAnd %uint %t2_<index> %uint_31",
	  "%ti0_<index> = OpBitcast %int %t0_<index>\n          %toi_<index> = OpBitcast %int %to_<index>\n          "
	  "%tsi_<index> = OpBitcast %int %ts_<index>\n          %tr_<index> = OpBitFieldSExtract %int %ti0_<index> %toi_<index> "
	  "%tsi_<index>",
	  "%t_<index> = OpBitcast %uint %tr_<index>"}},
	// v_bfi_b32: dst = (src0 & src1) | (~src0 & src2)
	{Recompile_V_XXX_U32_VdstVsrc0Vsrc1Vsrc2, ShaderInstructionType::VBfiB32, ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,
	 {"%ta_<index> = OpBitwiseAnd %uint %t0_<index> %t1_<index>", "%tn_<index> = OpNot %uint %t0_<index>",
	  "%tb_<index> = OpBitwiseAnd %uint %tn_<index> %t2_<index>",
	  "%t_<index> = OpBitwiseOr %uint %ta_<index> %tb_<index>"}},
	{Recompile_V_XXX_U32_VdstVsrc0Vsrc1Vsrc2, ShaderInstructionType::VLshlAddU32, ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,
	 {"%ts_<index> = OpBitwiseAnd %uint %t1_<index> %uint_31", "%tm_<index> = OpShiftLeftLogical %uint %t0_<index> %ts_<index>",
	  "%t_<index> = OpIAdd %uint %tm_<index> %t2_<index>"}},
	{Recompile_V_XXX_U32_VdstVsrc0Vsrc1Vsrc2, ShaderInstructionType::VAndOrB32, ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,
	 {"%tm_<index> = OpBitwiseAnd %uint %t0_<index> %t1_<index>", "%t_<index> = OpBitwiseOr %uint %tm_<index> %t2_<index>"}},
	{Recompile_V_XXX_U32_VdstVsrc0Vsrc1Vsrc2, ShaderInstructionType::VLshlOrB32, ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,
	 {"%ts_<index> = OpBitwiseAnd %uint %t1_<index> %uint_31", "%tm_<index> = OpShiftLeftLogical %uint %t0_<index> %ts_<index>",
	  "%t_<index> = OpBitwiseOr %uint %tm_<index> %t2_<index>"}},
	// v_add3_u32: dst = src0 + src1 + src2 (wraps as uint).
	{Recompile_V_XXX_U32_VdstVsrc0Vsrc1Vsrc2, ShaderInstructionType::VAdd3U32, ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,
	 {"%ts_<index> = OpIAdd %uint %t0_<index> %t1_<index>", "%t_<index> = OpIAdd %uint %ts_<index> %t2_<index>"}},
    {Recompile_V_XXX_U32_VdstVsrc0Vsrc1Vsrc2,  ShaderInstructionType::VMadU32U24, ShaderInstructionFormat::VdstVsrc0Vsrc1Vsrc2,  {"%tu0_<index> = OpBitwiseAnd %uint %t0_<index> %uint_0x00ffffff",
                                                                                                                                  "%tu1_<index> = OpBitwiseAnd %uint %t1_<index> %uint_0x00ffffff",
                                                                                                                                  "%tm_<index> = OpFunctionCall %uint %mul_lo_uint %tu0_<index> %tu1_<index>",
                                                                                                                                  "%t_<index> = OpIAdd %uint %tm_<index> %t2_<index>"}},

	    // clang-format on
	};

	static Core::Hashmap<ShaderInstructionTypeFormat, const RecompilerFunc*>* map = nullptr;

	if (map == nullptr)
	{
		map = new Core::Hashmap<ShaderInstructionTypeFormat, const RecompilerFunc*>();

		for (const auto& func: g_recomp_func)
		{
			ShaderInstructionTypeFormat p = {func.type, func.format};
			EXIT_IF(map->Contains(p));
			map->Put(p, &func);
		}
	}

	ShaderInstructionTypeFormat p = {type, format};

	return map->Get(p, nullptr);
}

void Spirv::AddConstantUint(uint32_t u)
{
	ShaderConstant c {};
	c.u = u;
	AddConstant(SpirvType::Uint, c);
}

void Spirv::AddConstantInt(int i)
{
	ShaderConstant c {};
	c.i = i;
	AddConstant(SpirvType::Int, c);
}

void Spirv::AddConstantFloat(float f)
{
	ShaderConstant c {};
	c.f = f;
	AddConstant(SpirvType::Float, c);
}

void Spirv::AddConstant(ShaderOperand op)
{
	SpirvType type = SpirvType::Unknown;

	if (op.type == ShaderOperandType::LiteralConstant)
	{
		type = SpirvType::Uint;
	}
	if (op.type == ShaderOperandType::IntegerInlineConstant)
	{
		type = SpirvType::Int;
	}
	if (op.type == ShaderOperandType::FloatInlineConstant)
	{
		type = SpirvType::Float;
	}

	EXIT_NOT_IMPLEMENTED(type == SpirvType::Unknown);

	AddConstant(type, op.constant);
}

void Spirv::AddConstant(SpirvType type, ShaderConstant constant)
{
	for (const auto& c: m_constants)
	{
		if (c.type == type && c.constant.u == constant.u)
		{
			return;
		}
	}

	Constant c {};
	c.type     = type;
	c.constant = constant;
	c.type_str = Core::EnumName(type).ToLower().C_Str();

	if (type == SpirvType::Uint)
	{
		c.value_str = constant.u < 256 ? String8::FromPrintf("%u", constant.u) : String8::FromPrintf("0x%08" PRIx32, constant.u);
		c.literal_str = c.value_str;
	}
	if (type == SpirvType::Int)
	{
		c.value_str = String8::FromPrintf("%d", constant.i);
		c.literal_str = c.value_str;
	}
	if (type == SpirvType::Float)
	{
		c.value_str = String8::FromPrintf("%f", constant.f);
		c.literal_str = String8::FromPrintf("%.9g", static_cast<double>(constant.f));
	}

	c.id = String8::FromPrintf("%s_%s", c.type_str.c_str(), c.value_str.ReplaceChar('.', '_').ReplaceChar('-', 'm').c_str());

	m_constants.Add(c);
}

void Spirv::AddVariable(ShaderOperandType type, int register_id, int size)
{
	ShaderOperand op;
	op.type        = type;
	op.register_id = register_id;
	op.size        = size;
	AddVariable(op);
}

void Spirv::AddVariable(ShaderOperand op)
{
	if (operand_is_variable(op))
	{
		EXIT_IF(op.size == 0);

		for (int i = 0; i < op.size; i++)
		{
			Variable v;
			v.op.type        = op.type;
			v.op.register_id = op.register_id + i;
			v.op.size        = 1;

			if (op.type == ShaderOperandType::VccLo && op.size == 2 && i == 1)
			{
				v.op.type        = ShaderOperandType::VccHi;
				v.op.register_id = 0;
			}

			if (op.type == ShaderOperandType::ExecLo && op.size == 2 && i == 1)
			{
				v.op.type        = ShaderOperandType::ExecHi;
				v.op.register_id = 0;
			}

			if (!m_variables.Contains(v, [](auto v1, auto v2) { return v1.op == v2.op; }))
			{
				m_variables.Add(v);
			}
		}
	}
}

String8 Spirv::GetConstantUint(uint32_t u) const
{
	for (const auto& c: m_constants)
	{
		if (c.type == SpirvType::Uint && c.constant.u == u)
		{
			return c.id;
		}
	}

	return "unknown_uint_constant";
}

String8 Spirv::GetConstantInt(int i) const
{
	for (const auto& c: m_constants)
	{
		if (c.type == SpirvType::Int && c.constant.i == i)
		{
			return c.id;
		}
	}

	return "unknown_int_constant";
}

String8 Spirv::GetConstantFloat(float f) const
{
	for (const auto& c: m_constants)
	{
		if (c.type == SpirvType::Float && c.constant.f == f)
		{
			return c.id;
		}
	}

	return "unknown_float_constant";
}

String8 Spirv::GetConstant(ShaderOperand op) const
{
	SpirvType type = SpirvType::Unknown;

	if (op.type == ShaderOperandType::LiteralConstant)
	{
		type = SpirvType::Uint;
	}
	if (op.type == ShaderOperandType::IntegerInlineConstant)
	{
		type = SpirvType::Int;
	}
	if (op.type == ShaderOperandType::FloatInlineConstant)
	{
		type = SpirvType::Float;
	}

	for (const auto& c: m_constants)
	{
		if (c.type == type && c.constant.u == op.constant.u)
		{
			return c.id;
		}
	}

	return "unknown_operand_constant";
}

void Spirv::GenerateSource()
{
	m_source.Clear();

	switch (m_code.GetType())
	{
		case ShaderType::Pixel: m_bind = (m_ps_input_info != nullptr ? &m_ps_input_info->bind : nullptr); break;
		case ShaderType::Vertex: m_bind = (m_vs_input_info != nullptr ? &m_vs_input_info->bind : nullptr); break;
		case ShaderType::Compute: m_bind = (m_cs_input_info != nullptr ? &m_cs_input_info->bind : nullptr); break;
		default: m_bind = nullptr; break;
	}

	if (m_vs_input_info != nullptr)
	{
		if (m_vs_input_info->fetch_embedded || m_vs_input_info->fetch_inline)
		{
			DetectFetch();
		}
	}

	WriteHeader();
	WriteDebug();
	WriteAnnotations();
	WriteTypes();
	WriteConstants();
	WriteGlobalVariables();
	WriteMainProlog();
	WriteLocalVariables();
	WriteInstructions();
	WriteMainEpilog();
	WriteFunctions();
}

void Spirv::WriteHeader()
{
	static const char* header = R"(
                ; Header
                OpCapability Shader
                OpCapability ImageQuery
                <Extensions>
                <Imports>
                OpMemoryModel Logical GLSL450
                OpEntryPoint <Type> %main "main" <Variables>
                <ExecutionModes>
)";

	String8 header_str;

	Core::StringList8 vars;
	Core::StringList8 extensions;
	Core::StringList8 imports;
	Core::StringList8 execution_modes;

	imports.Add("%GLSL_std_450 = OpExtInstImport \"GLSL.std.450\"");

	if (Config::SpirvDebugPrintfEnabled())
	{
		extensions.Add("OpExtension \"SPV_KHR_non_semantic_info\"");
		imports.Add("%NonSemantic_DebugPrintf = OpExtInstImport \"NonSemantic.DebugPrintf\"");
	}

	if (m_bind != nullptr)
	{
		if (m_bind->storage_buffers.buffers_num > 0)
		{
			vars.Add("%buf");
		}
		if (m_bind->textures2D.textures2d_sampled_num > 0)
		{
			vars.Add("%textures2D_S");
		}
		if (m_bind->textures2D.textures2d_storage_num > 0)
		{
			vars.Add("%textures2D_L");
		}
		if (m_bind->samplers.samplers_num > 0)
		{
			vars.Add("%samplers");
		}
		if (m_bind->gds_pointers.pointers_num > 0)
		{
			vars.Add("%gds");
		}
		if (m_bind->push_constant_size > 0)
		{
			vars.Add("%vsharp");
		}
	}

	switch (m_code.GetType())
	{
		case ShaderType::Pixel:
			// Location 0 always uses %outColor (legacy name). Additional RTs that
			// have a non-zero target_output_mode are declared as %outColorN.
			vars.Add("%outColor");
			if (m_ps_input_info != nullptr)
			{
				for (int rt = 1; rt < 8; rt++)
				{
					if (m_ps_input_info->target_output_mode[rt] != 0)
					{
						vars.Add(String8::FromPrintf("%%outColor%d", rt));
					}
				}
				for (uint32_t i = 0; i < m_ps_input_info->input_num; i++)
				{
					vars.Add(String8::FromPrintf("%%attr%d", i));
				}
				if (m_ps_input_info->ps_pos_xy)
				{
					vars.Add("%gl_FragCoord");
				}
				// Guest EarlyZThenLateZ may reject occluded fragments early, but
				// depth is committed after shader kill. Vulkan EarlyFragmentTests
				// alone commits depth before OpKill, so use late tests for shaders
				// that can discard.
				if (m_ps_input_info->ps_early_z && !m_ps_input_info->ps_pixel_kill_enable)
				{
					execution_modes.Add("OpExecutionMode %main EarlyFragmentTests\n");
				}
			}
			header_str = String8(header).ReplaceStr("<Type>", "Fragment");
			execution_modes.Add("OpExecutionMode %main OriginUpperLeft\n");
			// TODO() do we need PixelCenterInteger mode?
			break;
		case ShaderType::Vertex:
			if (m_vs_input_info != nullptr)
			{
				for (int i = 0; i < m_vs_input_info->resources_num; i++)
				{
					vars.Add(String8::FromPrintf("%%attr%d", i));
				}
				for (int i = 0; i < m_vs_input_info->export_count; i++)
				{
					vars.Add(String8::FromPrintf("%%param%d", i));
				}
			}
			vars.Add("%gl_VertexIndex");
			vars.Add("%gl_InstanceIndex");
			vars.Add("%outPerVertex");
			// vars.Add("%param0");
			header_str = String8(header).ReplaceStr("<Type>", "Vertex");
			break;
		case ShaderType::Compute:
			if (m_cs_input_info != nullptr)
			{
				execution_modes.Add(String8::FromPrintf("OpExecutionMode %%main LocalSize %u %u %u", m_cs_input_info->threads_num[0],
				                                        m_cs_input_info->threads_num[1], m_cs_input_info->threads_num[2]));
			}
			vars.Add("%gl_LocalInvocationID");
			vars.Add("%gl_WorkGroupID");
			header_str = String8(header).ReplaceStr("<Type>", "GLCompute");
			break;
		default: EXIT("unknown type: %s\n", Core::EnumName8(m_code.GetType()).c_str()); break;
	}

	m_source += header_str.ReplaceStr("<Variables>", vars.Concat(' '))
	                .ReplaceStr("<ExecutionModes>", execution_modes.Concat("\n" + String8(' ', 15)))
	                .ReplaceStr("<Imports>", imports.Concat("\n" + String8(' ', 15)))
	                .ReplaceStr("<Extensions>", extensions.Concat("\n" + String8(' ', 15)));
}

void Spirv::WriteDebug()
{
	if (Config::SpirvDebugPrintfEnabled())
	{
		int index = 0;
		for (const auto& p: m_code.GetDebugPrintfs())
		{
			m_source += String8::FromPrintf("%%printf_str_%d = OpString \"%s\"", index, p.format.C_Str());
			index++;
		}
	}
}

void Spirv::WriteAnnotations()
{
	static const char* pixel_annotations   = R"(
               ; Annotations
               OpDecorate %outColor Location 0
               <Variables>
)";
	static const char* vertex_annotations  = R"(
               ; Annotations
               OpDecorate %gl_VertexIndex BuiltIn VertexIndex
               OpDecorate %gl_InstanceIndex BuiltIn InstanceIndex
               OpMemberDecorate %gl_PerVertex 0 BuiltIn Position
               OpMemberDecorate %gl_PerVertex 1 BuiltIn PointSize
               OpMemberDecorate %gl_PerVertex 2 BuiltIn ClipDistance
               OpMemberDecorate %gl_PerVertex 3 BuiltIn CullDistance
               OpDecorate %gl_PerVertex Block
               ; OpDecorate %param0 Location 0
               <Variables>
)";
	static const char* compute_annotations = R"(
               ; Annotations
               OpDecorate %gl_LocalInvocationID BuiltIn LocalInvocationId
               OpDecorate %gl_WorkGroupID BuiltIn WorkgroupId
               OpDecorate %gl_WorkGroupSize BuiltIn WorkgroupSize
               <Variables>
)";

	Core::StringList8 vars;

	switch (m_code.GetType())
	{
		case ShaderType::Pixel:
			if (m_ps_input_info != nullptr)
			{
				for (int rt = 1; rt < 8; rt++)
				{
					if (m_ps_input_info->target_output_mode[rt] != 0)
					{
						vars.Add(String8::FromPrintf("OpDecorate %%outColor%d Location %d", rt, rt));
					}
				}
				for (uint32_t i = 0; i < m_ps_input_info->input_num; i++)
				{
					EXIT_NOT_IMPLEMENTED((m_ps_input_info->interpolator_settings[i] & ~static_cast<uint32_t>(0x41fu)) != 0);

					bool     flat     = (m_ps_input_info->interpolator_settings[i] & 0x400u) != 0;
					uint32_t location = m_ps_input_info->interpolator_settings[i] & 0x1fu;

					if (flat)
					{
						vars.Add(String8::FromPrintf("OpDecorate %%attr%u Flat", i));
					}
					vars.Add(String8::FromPrintf("OpDecorate %%attr%u Location %u", i, location));
				}
				if (m_ps_input_info->ps_pos_xy)
				{
					vars.Add("OpDecorate %gl_FragCoord BuiltIn FragCoord");
				}
			}
			m_source += String8(pixel_annotations).ReplaceStr("<Variables>", vars.Concat("\n" + String8(' ', 15)));
			break;
		case ShaderType::Vertex:
			if (m_vs_input_info != nullptr)
			{
				for (int i = 0; i < m_vs_input_info->resources_num; i++)
				{
					vars.Add(String8::FromPrintf("OpDecorate %%attr%d Location %d", i, i));
				}
				for (int i = 0; i < m_vs_input_info->export_count; i++)
				{
					vars.Add(String8::FromPrintf("OpDecorate %%param%d Location %d", i, i));
				}
			}
			m_source += String8(vertex_annotations).ReplaceStr("<Variables>", vars.Concat("\n" + String8(' ', 15)));
			break;
		case ShaderType::Compute:
			m_source += String8(compute_annotations).ReplaceStr("<Variables>", vars.Concat("\n" + String8(' ', 15)));
			break;
		default: EXIT("unknown type: %s\n", Core::EnumName8(m_code.GetType()).c_str()); break;
	}

	static const char* storage_buffers_annotations = R"(
       OpDecorate %buffers_runtimearr_float ArrayStride 4
       OpMemberDecorate %BufferObject 0 Offset 0
       OpDecorate %BufferObject Block
       OpDecorate %buf DescriptorSet <DescriptorSet>
       OpDecorate %buf Binding <BindingIndex>
)";

	static const char* textures_annotations_s = R"(
       OpDecorate %textures2D_S DescriptorSet <DescriptorSet>
       OpDecorate %textures2D_S Binding <BindingIndex>
)";

	static const char* textures_annotations_l = R"(
       OpDecorate %textures2D_L DescriptorSet <DescriptorSet>
       OpDecorate %textures2D_L Binding <BindingIndex>
)";

	static const char* samplers_annotations = R"(
       OpDecorate %samplers DescriptorSet <DescriptorSet>
       OpDecorate %samplers Binding <BindingIndex>
)";
	static const char* gds_annotations      = R"(
               OpDecorate %gds_runtimearr_uint ArrayStride 4
               OpMemberDecorate %GDS 0 Coherent
               OpMemberDecorate %GDS 0 Offset 0
               OpDecorate %GDS Block
               OpDecorate %gds DescriptorSet <DescriptorSet>
               OpDecorate %gds Binding <BindingIndex>
)";

	static const char* vsharp_annotations = R"(
       OpDecorate %vsharp_arr_uint_uint_4 ArrayStride 4
       OpDecorate %vsharp_arr__arr_uint_uint_4_uint_<buffers_num> ArrayStride 16
	   OpMemberDecorate %BufferResource 0 Offset <Offset>
       OpDecorate %BufferResource Block
)";

	if (m_bind != nullptr)
	{
		if (m_bind->storage_buffers.buffers_num > 0)
		{
			m_source += String8(storage_buffers_annotations)
			                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
			                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->storage_buffers.binding_index));
		}
		if (m_bind->textures2D.textures2d_sampled_num > 0)
		{
			m_source += String8(textures_annotations_s)
			                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
			                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->textures2D.binding_sampled_index));
		}
		if (m_bind->textures2D.textures2d_storage_num > 0)
		{
			m_source += String8(textures_annotations_l)
			                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
			                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->textures2D.binding_storage_index));
		}
		if (m_bind->samplers.samplers_num > 0)
		{
			m_source += String8(samplers_annotations)
			                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
			                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->samplers.binding_index));
		}
		if (m_bind->gds_pointers.pointers_num > 0)
		{
			m_source += String8(gds_annotations)
			                .ReplaceStr("<DescriptorSet>", String8::FromPrintf("%u", m_bind->descriptor_set_slot))
			                .ReplaceStr("<BindingIndex>", String8::FromPrintf("%d", m_bind->gds_pointers.binding_index));
		}
		if (m_bind->push_constant_size > 0)
		{
			m_source += String8(vsharp_annotations)
			                .ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->push_constant_size / 16))
			                .ReplaceStr("<Offset>", String8::FromPrintf("%u", m_bind->push_constant_offset));
		}
	}
}

void Spirv::WriteTypes()
{
	static const char* types = R"(
                               ; Types
                         %void = OpTypeVoid
                        %float = OpTypeFloat 32
                          %int = OpTypeInt 32 1
                         %uint = OpTypeInt 32 0
                         %bool = OpTypeBool
                      %v2float = OpTypeVector %float 2
                      %v3float = OpTypeVector %float 3
                      %v4float = OpTypeVector %float 4
                       %v2uint = OpTypeVector %uint 2
                       %v3uint = OpTypeVector %uint 3
                       %v4uint = OpTypeVector %uint 4
                        %v2int = OpTypeVector %int 2
                 %undef_v2uint = OpUndef %v2uint
               %_ptr_Input_int = OpTypePointer Input %int
              %_ptr_Input_uint = OpTypePointer Input %uint
             %_ptr_Input_float = OpTypePointer Input %float
           %_ptr_Input_v2float = OpTypePointer Input %v2float
           %_ptr_Input_v3float = OpTypePointer Input %v3float
           %_ptr_Input_v4float = OpTypePointer Input %v4float
            %_ptr_Input_v3uint = OpTypePointer Input %v3uint
          %_ptr_Output_v4float = OpTypePointer Output %v4float
          %_ptr_Function_float = OpTypePointer Function %float
           %_ptr_Function_bool = OpTypePointer Function %bool
            %_ptr_Function_int = OpTypePointer Function %int
           %_ptr_Function_uint = OpTypePointer Function %uint
        %_ptr_Function_v2float = OpTypePointer Function %v2float
        %_ptr_Function_v3float = OpTypePointer Function %v3float
        %_ptr_Function_v4float = OpTypePointer Function %v4float
           %_ptr_Uniform_float = OpTypePointer Uniform %float
     %_ptr_StorageBuffer_float = OpTypePointer StorageBuffer %float
      %_ptr_StorageBuffer_uint = OpTypePointer StorageBuffer %uint
                     %ResTypeI = OpTypeStruct %int %int
                     %ResTypeU = OpTypeStruct %uint %uint
                %function_void = OpTypeFunction %void
              %function_fetch1 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float
              %function_fetch2 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_v2float
              %function_fetch3 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_v3float
              %function_fetch4 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_v4float
                 %function_u_u = OpTypeFunction %uint %uint %uint
               %function_u_u_u = OpTypeFunction %uint %uint %uint %uint
            %function_u2_u_u_u = OpTypeFunction %v2uint %uint %uint %uint
               %function_b_f_f = OpTypeFunction %bool %float %float
                 %function_i_i = OpTypeFunction %int %int %int
               %function_shift = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint
%function_tbuffer_load_store_format_xyzw = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int
    %function_buffer_load_store_float1 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int
   %function_buffer_load_store_ubyte = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int
    %function_buffer_load_store_float2 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int
     %function_buffer_load_store_float4 = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_float %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int
 %function_tbuffer_load_store_format_x = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int
%function_tbuffer_load_store_format_xy = OpTypeFunction %void %_ptr_Function_float %_ptr_Function_float %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int %_ptr_Function_int
          %function_sbuffer_load_dword = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_int %_ptr_Function_int
        %function_sbuffer_load_dword_2 = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_int %_ptr_Function_int
        %function_sbuffer_load_dword_4 = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_int %_ptr_Function_int
        %function_sbuffer_load_dword_8 = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_int %_ptr_Function_int
       %function_sbuffer_load_dword_16 = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_int %_ptr_Function_int
)";

	static const char* pixel_types = R"(
)";

	static const char* vertex_types = R"(
            %array_length = OpConstant %uint 1
        %int_per_vertex_0 = OpConstant %int 0
       %_arr_float_uint_1 = OpTypeArray %float %array_length
            %gl_PerVertex = OpTypeStruct %v4float %float %_arr_float_uint_1 %_arr_float_uint_1
%_ptr_Output_gl_PerVertex = OpTypePointer Output %gl_PerVertex
)";

static const char* compute_types = R"(
)";

	m_source += types;

	switch (m_code.GetType())
	{
		case ShaderType::Vertex: m_source += vertex_types; break;
		case ShaderType::Pixel: m_source += pixel_types; break;
		case ShaderType::Compute: m_source += compute_types; break;
		default: EXIT("unknown type: %s\n", Core::EnumName8(m_code.GetType()).c_str()); break;
	}

	if (m_code.GetType() == ShaderType::Compute && m_cs_input_info != nullptr && m_cs_input_info->lds_dwords > 0)
	{
		m_source += String8(R"(
                  %lds_length = OpConstant %uint <lds_dwords>
              %lds_array_uint = OpTypeArray %uint %lds_length
          %_ptr_Workgroup_uint = OpTypePointer Workgroup %uint
%_ptr_Workgroup_lds_array_uint = OpTypePointer Workgroup %lds_array_uint
)")
		                .ReplaceStr("<lds_dwords>", String8::FromPrintf("%u", m_cs_input_info->lds_dwords));
	}

	static const char* storage_buffers_types = R"(
                               %buffers_runtimearr_float = OpTypeRuntimeArray %float
                                           %BufferObject = OpTypeStruct %buffers_runtimearr_float
                         %buffers_num_uint_<buffers_num> = OpConstant %uint <buffers_num>
                   %_arr_BufferObject_uint_<buffers_num> = OpTypeArray %BufferObject %buffers_num_uint_<buffers_num>
%_ptr_StorageBuffer__arr_BufferObject_uint_<buffers_num> = OpTypePointer StorageBuffer %_arr_BufferObject_uint_<buffers_num>
)";

static const char* textures_sampled_types = R"(
                                             %ImageS = OpTypeImage %<image_scalar> <image_dimension> 0 <arrayed> 0 1 Unknown
                    %textures2D_S_uint_<buffers_num> = OpConstant %uint <buffers_num>
                     %_arr_ImageS_uint_<buffers_num> = OpTypeArray %ImageS %textures2D_S_uint_<buffers_num>
%_ptr_UniformConstant__arr_ImageS_uint_<buffers_num> = OpTypePointer UniformConstant %_arr_ImageS_uint_<buffers_num>
                        %_ptr_UniformConstant_ImageS = OpTypePointer UniformConstant %ImageS
                                       %SampledImage = OpTypeSampledImage %ImageS
)";

static const char* textures_loaded_types = R"(
                                             %ImageL = OpTypeImage %<image_scalar> <image_dimension> 0 <arrayed> 0 2 <image_format>
                    %textures2D_L_uint_<buffers_num> = OpConstant %uint <buffers_num>
                     %_arr_ImageL_uint_<buffers_num> = OpTypeArray %ImageL %textures2D_L_uint_<buffers_num>
%_ptr_UniformConstant__arr_ImageL_uint_<buffers_num> = OpTypePointer UniformConstant %_arr_ImageL_uint_<buffers_num>
                        %_ptr_UniformConstant_ImageL = OpTypePointer UniformConstant %ImageL
)";

	static const char* samplers_types = R"(
                                             %Sampler = OpTypeSampler
                         %samplers_uint_<buffers_num> = OpConstant %uint <buffers_num>
                     %_arr_Sampler_uint_<buffers_num> = OpTypeArray %Sampler %samplers_uint_<buffers_num>
%_ptr_UniformConstant__arr_Sampler_uint_<buffers_num> = OpTypePointer UniformConstant %_arr_Sampler_uint_<buffers_num>
                        %_ptr_UniformConstant_Sampler = OpTypePointer UniformConstant %Sampler
)";

	static const char* gds_types = R"(
            %gds_runtimearr_uint = OpTypeRuntimeArray %uint
                    %GDS = OpTypeStruct %gds_runtimearr_uint
            %_ptr_StorageBuffer_GDS = OpTypePointer StorageBuffer %GDS
)";

	static const char* vsharp_types = R"(
         %vsharp_buffers_num_uint_<buffers_num> = OpConstant %uint <buffers_num>
                             %vsharp_num_uint_4 = OpConstant %uint 4
                        %vsharp_arr_uint_uint_4 = OpTypeArray %uint %vsharp_num_uint_4
%vsharp_arr__arr_uint_uint_4_uint_<buffers_num> = OpTypeArray %vsharp_arr_uint_uint_4 %vsharp_buffers_num_uint_<buffers_num>
                                %BufferResource = OpTypeStruct %vsharp_arr__arr_uint_uint_4_uint_<buffers_num>
              %_ptr_PushConstant_BufferResource = OpTypePointer PushConstant %BufferResource
                        %_ptr_PushConstant_uint = OpTypePointer PushConstant %uint
)";

	if (m_bind != nullptr)
	{
		const char* arrayed = UsesArrayed2dImages(m_bind) ? "1" : "0";
		const bool three_dimensional = UsesThreeDimensionalImages(m_bind);
		const bool uint_images = UsesUnsignedIntegerImages(m_bind);
		const char* image_scalar = uint_images ? "uint" : "float";
		const char* image_format = uint_images ? "R32ui" : "Rgba8";
		const char* image_dimension = three_dimensional ? "3D" : "2D";
		if (m_bind->storage_buffers.buffers_num > 0)
		{
			m_source +=
			    String8(storage_buffers_types).ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->storage_buffers.buffers_num));
		}
		if (m_bind->textures2D.textures2d_sampled_num > 0)
		{
			m_source += String8(textures_sampled_types)
			                .ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->textures2D.textures2d_sampled_num))
			                .ReplaceStr("<image_scalar>", image_scalar)
			                .ReplaceStr("<image_dimension>", image_dimension)
			                .ReplaceStr("<arrayed>", arrayed);
		}
		if (m_bind->textures2D.textures2d_storage_num > 0)
		{
			m_source += String8(textures_loaded_types)
			                .ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->textures2D.textures2d_storage_num))
			                .ReplaceStr("<image_scalar>", image_scalar)
			                .ReplaceStr("<image_format>", image_format)
			                .ReplaceStr("<image_dimension>", image_dimension)
			                .ReplaceStr("<arrayed>", arrayed);
		}
		if (m_bind->samplers.samplers_num > 0)
		{
			m_source += String8(samplers_types).ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->samplers.samplers_num));
		}
		if (m_bind->gds_pointers.pointers_num > 0)
		{
			m_source += String8(gds_types);
		}
		if (m_bind->push_constant_size > 0)
		{
			m_source += String8(vsharp_types).ReplaceStr("<buffers_num>", String8::FromPrintf("%d", m_bind->push_constant_size / 16));
		}
	}

}

void Spirv::WriteConstants()
{
	FindConstants();

	static const char* comment = R"(
    ; Constants
         %true = OpConstantTrue %bool
        %false = OpConstantFalse %bool
    %float_2pi = OpConstant %float 6.283185307179586476925286766559
)";

	m_source += comment;

	for (const auto& c: m_constants)
	{
		m_source += String8::FromPrintf("%%%s = OpConstant %%%s %s\n", c.id.c_str(), c.type_str.c_str(), c.literal_str.c_str());
	}
}

void Spirv::WriteGlobalVariables()
{
	static const char* pixel_variables   = R"(
              ;Variables
   %outColor = OpVariable %_ptr_Output_v4float Output
               <Variables>
)";
	static const char* vertex_variables  = R"(
              ;Variables
    %gl_VertexIndex = OpVariable %_ptr_Input_int Input
  %gl_InstanceIndex = OpVariable %_ptr_Input_int Input
      %outPerVertex = OpVariable %_ptr_Output_gl_PerVertex Output
            ; %param0 = OpVariable %_ptr_Output_v4float Output
               <Variables>
)";
	static const char* compute_variables = R"(
              ;Variables
%gl_LocalInvocationID = OpVariable %_ptr_Input_v3uint Input
      %gl_WorkGroupID = OpVariable %_ptr_Input_v3uint Input
               <Variables>
)";

	Core::StringList8 vars;

	if (m_code.GetType() == ShaderType::Pixel && m_ps_input_info != nullptr)
	{
		for (int rt = 1; rt < 8; rt++)
		{
			if (m_ps_input_info->target_output_mode[rt] != 0)
			{
				vars.Add(String8::FromPrintf("%%outColor%d = OpVariable %%_ptr_Output_v4float Output", rt));
			}
		}
	}

	if (m_bind != nullptr)
	{
		if (m_bind->storage_buffers.buffers_num > 0)
		{
			vars.Add(String8::FromPrintf("%%buf = OpVariable %%_ptr_StorageBuffer__arr_BufferObject_uint_%d StorageBuffer",
			                             m_bind->storage_buffers.buffers_num));
		}
		if (m_bind->textures2D.textures2d_sampled_num > 0)
		{
			vars.Add(String8::FromPrintf("%%textures2D_S = OpVariable %%_ptr_UniformConstant__arr_ImageS_uint_%d UniformConstant",
			                             m_bind->textures2D.textures2d_sampled_num));
		}
		if (m_bind->textures2D.textures2d_storage_num > 0)
		{
			vars.Add(String8::FromPrintf("%%textures2D_L = OpVariable %%_ptr_UniformConstant__arr_ImageL_uint_%d UniformConstant",
			                             m_bind->textures2D.textures2d_storage_num));
		}
		if (m_bind->samplers.samplers_num > 0)
		{
			vars.Add(String8::FromPrintf("%%samplers = OpVariable %%_ptr_UniformConstant__arr_Sampler_uint_%d UniformConstant",
			                             m_bind->samplers.samplers_num));
		}
		if (m_bind->gds_pointers.pointers_num > 0)
		{
			vars.Add("%gds = OpVariable %_ptr_StorageBuffer_GDS StorageBuffer");
		}
		if (m_bind->push_constant_size > 0)
		{
			vars.Add("%vsharp = OpVariable %_ptr_PushConstant_BufferResource PushConstant");
		}
	}

	if (m_code.GetType() == ShaderType::Compute && m_cs_input_info != nullptr && m_cs_input_info->lds_dwords > 0)
	{
		vars.Add("%lds = OpVariable %_ptr_Workgroup_lds_array_uint Workgroup");
	}

	switch (m_code.GetType())
	{
		case ShaderType::Pixel:
			if (m_ps_input_info != nullptr)
			{
				for (uint32_t i = 0; i < m_ps_input_info->input_num; i++)
				{
					vars.Add(String8::FromPrintf("%%attr%d = OpVariable %%_ptr_Input_v4float Input", i));
				}
				if (m_ps_input_info->ps_pos_xy)
				{
					vars.Add("%gl_FragCoord = OpVariable %_ptr_Input_v4float Input");
				}
			}
			m_source += String8(pixel_variables).ReplaceStr("<Variables>", vars.Concat("\n" + String8(' ', 15)));
			break;
		case ShaderType::Vertex:
			if (m_vs_input_info != nullptr)
			{
				for (int i = 0; i < m_vs_input_info->resources_num; i++)
				{
					switch (m_vs_input_info->resources_dst[i].registers_num)
					{
						case 1: vars.Add(String8::FromPrintf("%%attr%d = OpVariable %%_ptr_Input_float Input", i)); break;
						case 2: vars.Add(String8::FromPrintf("%%attr%d = OpVariable %%_ptr_Input_v2float Input", i)); break;
						case 3: vars.Add(String8::FromPrintf("%%attr%d = OpVariable %%_ptr_Input_v3float Input", i)); break;
						case 4: vars.Add(String8::FromPrintf("%%attr%d = OpVariable %%_ptr_Input_v4float Input", i)); break;
						default: EXIT("invalid registers_num: %d\n", m_vs_input_info->resources_dst[i].registers_num);
					}
				}
				for (int i = 0; i < m_vs_input_info->export_count; i++)
				{
					vars.Add(String8::FromPrintf("%%param%d = OpVariable %%_ptr_Output_v4float Output", i));
				}
			}
			m_source += String8(vertex_variables).ReplaceStr("<Variables>", vars.Concat("\n" + String8(' ', 15)));
			break;
		case ShaderType::Compute:
			if (m_cs_input_info != nullptr)
			{
				vars.Add(String8::FromPrintf("%%gl_WorkGroupSize = OpConstantComposite %%v3uint %%uint_%u %%uint_%u %%uint_%u",
				                             m_cs_input_info->threads_num[0], m_cs_input_info->threads_num[1],
				                             m_cs_input_info->threads_num[2]));
			}
			m_source += String8(compute_variables).ReplaceStr("<Variables>", vars.Concat("\n" + String8(' ', 15)));
			break;
		default: EXIT("unknown type: %s\n", Core::EnumName8(m_code.GetType()).c_str()); break;
	}
}

void Spirv::WriteMainProlog()
{
	static const char* text = R"(
                   ; Function main
                   ; Prolog
       %main       = OpFunction %void None %function_void
       %main_label = OpLabel
)";

	m_source += text;
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void Spirv::WriteLocalVariables()
{
	FindVariables();

	Vector<int> packed_half_regs;
	for (const auto& inst: m_code.GetInstructions())
	{
		if (inst.type == ShaderInstructionType::VCvtPkrtzF16F32 && inst.dst.type == ShaderOperandType::Vgpr && inst.dst.size == 1)
		{
			bool exists = false;
			for (auto reg: packed_half_regs)
			{
				if (reg == inst.dst.register_id)
				{
					exists = true;
					break;
				}
			}
			if (!exists)
			{
				packed_half_regs.Add(inst.dst.register_id);
			}
		}
	}

	static const char* comment = R"(
    ; Registers
)";

	m_source += comment;

	for (const auto& c: m_variables)
	{
		auto value = operand_variable_to_str(c.op);
		m_source += String8::FromPrintf("%%%s = OpVariable %%_ptr_Function_%s Function\n", value.value.c_str(),
		                                Core::EnumName(value.type).ToLower().C_Str());
	}
	for (auto reg: packed_half_regs)
	{
		m_source += String8::FromPrintf("%%v%d_packed_half = OpVariable %%_ptr_Function_uint Function\n", reg);
	}

	static const char* common_vars = R"(
             %temp_float = OpVariable %_ptr_Function_float Function
           %temp_v2float = OpVariable %_ptr_Function_v2float Function
           %temp_v3float = OpVariable %_ptr_Function_v3float Function
	       %temp_v4float = OpVariable %_ptr_Function_v4float Function
           %temp_int_0 = OpVariable %_ptr_Function_int Function
           %temp_int_1 = OpVariable %_ptr_Function_int Function
           %temp_int_2 = OpVariable %_ptr_Function_int Function
           %temp_int_3 = OpVariable %_ptr_Function_int Function
           %temp_int_4 = OpVariable %_ptr_Function_int Function
           %temp_int_5 = OpVariable %_ptr_Function_int Function
           %temp_uint_0 = OpVariable %_ptr_Function_uint Function
           %temp_uint_1 = OpVariable %_ptr_Function_uint Function
           %temp_uint_2 = OpVariable %_ptr_Function_uint Function
           %temp_uint_3 = OpVariable %_ptr_Function_uint Function
           %temp_uint_4 = OpVariable %_ptr_Function_uint Function
           %temp_uint_5 = OpVariable %_ptr_Function_uint Function
)";

	m_source += common_vars;
	for (auto reg: packed_half_regs)
	{
		m_source += String8::FromPrintf("               OpStore %%v%d_packed_half %%uint_0\n", reg);
	}

	if (m_code.GetType() == ShaderType::Vertex)
	{
		static const char* text = R"(
       %vertex_index_int = OpLoad %int %gl_VertexIndex
           %vertex_index = OpBitcast %float %vertex_index_int
                           OpStore %<v> %vertex_index
       %instance_index_int = OpLoad %int %gl_InstanceIndex
           %instance_index = OpBitcast %float %instance_index_int
                           OpStore %<i> %instance_index
)";
		if (m_vs_input_info != nullptr && m_vs_input_info->gs_prolog)
		{
			m_source += String8(text).ReplaceStr("<v>", "v5").ReplaceStr("<i>", "v8");

			// [7:0] - num_vertices, [15:8] - num_primitives
			static const char* init_s3 = R"(
	               OpStore %s3 %uint_1
				)";

			m_source += init_s3;
		} else
		{
			m_source += String8(text).ReplaceStr("<v>", "v0").ReplaceStr("<i>", "v3");
		}
	}

	if (m_code.GetType() == ShaderType::Pixel)
	{
		if (m_ps_input_info != nullptr && m_ps_input_info->ps_pos_xy)
		{
			static const char* native_text = R"(
         %FragCoord_px = OpAccessChain %_ptr_Input_float %gl_FragCoord %uint_0
         %FragCoord_x = OpLoad %float %FragCoord_px
               OpStore %v2 %FragCoord_x
         %FragCoord_py = OpAccessChain %_ptr_Input_float %gl_FragCoord %uint_1
         %FragCoord_y = OpLoad %float %FragCoord_py
               OpStore %v3 %FragCoord_y
)";
			static const char* scaled_text = R"(
         %FragCoord_px = OpAccessChain %_ptr_Input_float %gl_FragCoord %uint_0
         %FragCoord_x = OpLoad %float %FragCoord_px
         %FragCoord_scale_x = OpFDiv %float %<x_numerator> %<x_denominator>
         %FragCoord_guest_x = OpFMul %float %FragCoord_x %FragCoord_scale_x
               OpStore %v2 %FragCoord_guest_x
         %FragCoord_py = OpAccessChain %_ptr_Input_float %gl_FragCoord %uint_1
         %FragCoord_y = OpLoad %float %FragCoord_py
         %FragCoord_scale_y = OpFDiv %float %<y_numerator> %<y_denominator>
         %FragCoord_guest_y = OpFMul %float %FragCoord_y %FragCoord_scale_y
               OpStore %v3 %FragCoord_guest_y
)";
			const auto&        scale       = m_ps_input_info->host_to_guest_scale;
			if (scale.IsIdentity())
			{
				m_source += native_text;
			} else
			{
				m_source += String8(scaled_text)
				                .ReplaceStr("<x_numerator>", GetConstantFloat(static_cast<float>(scale.x_guest_numerator)))
				                .ReplaceStr("<x_denominator>", GetConstantFloat(static_cast<float>(scale.x_host_denominator)))
				                .ReplaceStr("<y_numerator>", GetConstantFloat(static_cast<float>(scale.y_guest_numerator)))
				                .ReplaceStr("<y_denominator>", GetConstantFloat(static_cast<float>(scale.y_host_denominator)));
			}
		}
	}

	if (m_code.GetType() == ShaderType::Compute)
	{
		static const char* text_thread_id = R"(
		%LocalInvocationID_114_<i> = OpAccessChain %_ptr_Input_uint %gl_LocalInvocationID %uint_<i>
        %LocalInvocationID_115_<i> = OpLoad %uint %LocalInvocationID_114_<i>
        %LocalInvocationID_116_<i> = OpBitcast %float %LocalInvocationID_115_<i>
               OpStore %v<i> %LocalInvocationID_116_<i>
)";

		static const char* text_group_id = R"(
        %WorkGroupID_120_<i> = OpAccessChain %_ptr_Input_uint %gl_WorkGroupID %uint_<i>
        %WorkGroupID_121_<i> = OpLoad %uint %WorkGroupID_120_<i>
               OpStore %<WorkGroupReg> %WorkGroupID_121_<i>
)";
		if (m_cs_input_info != nullptr)
		{
			for (int i = 0; i < m_cs_input_info->thread_ids_num; i++)
			{
				m_source += String8(text_thread_id).ReplaceStr("<i>", String8::FromPrintf("%d", i));
			}

			int reg = 0;
			for (int i = 0; i < 3; i++)
			{
				if (m_cs_input_info->group_id[i])
				{
					m_source += String8(text_group_id)
					                .ReplaceStr("<WorkGroupReg>", String8::FromPrintf("s%u", m_cs_input_info->workgroup_register + reg))
					                .ReplaceStr("<i>", String8::FromPrintf("%d", i));
					reg++;
				}
			}
		}
	}

	if (m_bind != nullptr)
	{
		static const char* text = R"(
         %vsharp_<reg> = OpAccessChain %_ptr_PushConstant_uint %vsharp %int_0 %int_<buffer> %int_<field>
         %vsharp_value_<reg> = OpLoad %uint %vsharp_<reg>
               OpStore %<reg> %vsharp_value_<reg>
		)";

		int buffer_index = 0;

		int shift_regs = (m_vs_input_info != nullptr && m_vs_input_info->gs_prolog ? 8 : 0);

		for (auto& m: m_extended_mapping)
		{
			m[0] = m[1] = 0;
		}

		for (int i = 0; i < m_bind->storage_buffers.buffers_num; i++)
		{
			int  start_reg = m_bind->storage_buffers.start_register[i];
			bool extended  = m_bind->storage_buffers.extended[i];

			EXIT_IF(buffer_index + i >= static_cast<int>(m_bind->push_constant_size) / 16);

			String8 buffer = String8::FromPrintf("%d", buffer_index + i);
			for (int f = 0; f < 4; f++)
			{
				if (extended)
				{
					EXIT_IF(start_reg < 16);
					EXIT_NOT_IMPLEMENTED(shift_regs != 0);
					EXIT_NOT_IMPLEMENTED(start_reg - 16 + f >= m_extended_mapping.Size());
					m_extended_mapping[start_reg - 16 + f][0] = buffer_index + i;
					m_extended_mapping[start_reg - 16 + f][1] = f;
				} else
				{
					String8 reg   = String8::FromPrintf("s%d", start_reg + f + shift_regs);
					String8 field = String8::FromPrintf("%d", f);
					m_source += String8(text).ReplaceStr("<reg>", reg).ReplaceStr("<buffer>", buffer).ReplaceStr("<field>", field);
				}
			}
		}

		buffer_index += m_bind->storage_buffers.buffers_num;

		for (int i = 0; i < m_bind->textures2D.textures_num; i++)
		{
			int  start_reg = m_bind->textures2D.desc[i].start_register;
			bool extended  = m_bind->textures2D.desc[i].extended;

			for (int ti = 0; ti < 2; ti++)
			{
				EXIT_IF(buffer_index + i * 2 + ti >= static_cast<int>(m_bind->push_constant_size) / 16);

				String8 buffer = String8::FromPrintf("%d", buffer_index + i * 2 + ti);
				for (int f = 0; f < 4; f++)
				{
					if (extended)
					{
						EXIT_IF(start_reg < 16);
						EXIT_NOT_IMPLEMENTED(shift_regs != 0);
						EXIT_NOT_IMPLEMENTED(start_reg - 16 + 4 * ti + f >= m_extended_mapping.Size());
						m_extended_mapping[start_reg - 16 + 4 * ti + f][0] = buffer_index + i * 2 + ti;
						m_extended_mapping[start_reg - 16 + 4 * ti + f][1] = f;
					} else
					{
						String8 reg   = String8::FromPrintf("s%d", start_reg + 4 * ti + f + shift_regs);
						String8 field = String8::FromPrintf("%d", f);
						m_source += String8(text).ReplaceStr("<reg>", reg).ReplaceStr("<buffer>", buffer).ReplaceStr("<field>", field);
					}
				}
			}
		}

		buffer_index += m_bind->textures2D.textures_num * 2;

		for (int i = 0; i < m_bind->samplers.samplers_num; i++)
		{
			int  start_reg = m_bind->samplers.start_register[i];
			bool extended  = m_bind->samplers.extended[i];

			EXIT_IF(buffer_index + i >= static_cast<int>(m_bind->push_constant_size) / 16);

			String8 buffer = String8::FromPrintf("%d", buffer_index + i);
			for (int f = 0; f < 4; f++)
			{
				if (extended)
				{
					EXIT_IF(start_reg < 16);
					EXIT_NOT_IMPLEMENTED(shift_regs != 0);
					EXIT_NOT_IMPLEMENTED(start_reg - 16 + f >= m_extended_mapping.Size());
					m_extended_mapping[start_reg - 16 + f][0] = buffer_index + i;
					m_extended_mapping[start_reg - 16 + f][1] = f;
				} else
				{
					String8 reg   = String8::FromPrintf("s%d", start_reg + f + shift_regs);
					String8 field = String8::FromPrintf("%d", f);
					m_source += String8(text).ReplaceStr("<reg>", reg).ReplaceStr("<buffer>", buffer).ReplaceStr("<field>", field);
				}
			}
		}

		buffer_index += m_bind->samplers.samplers_num;

		for (int i = 0; i < m_bind->gds_pointers.pointers_num; i++)
		{
			int  start_reg = m_bind->gds_pointers.start_register[i];
			bool extended  = m_bind->gds_pointers.extended[i];

			EXIT_IF(buffer_index + i / 4 >= static_cast<int>(m_bind->push_constant_size) / 16);

			if (extended)
			{
				EXIT_IF(start_reg < 16);
				EXIT_NOT_IMPLEMENTED(shift_regs != 0);
				EXIT_NOT_IMPLEMENTED(start_reg - 16 >= m_extended_mapping.Size());
				m_extended_mapping[start_reg - 16][0] = buffer_index + i / 4;
				m_extended_mapping[start_reg - 16][1] = i % 4;
			} else
			{
				String8 buffer = String8::FromPrintf("%d", buffer_index + i / 4);
				String8 reg    = String8::FromPrintf("s%d", start_reg + shift_regs);
				String8 field  = String8::FromPrintf("%d", i % 4);
				m_source += String8(text).ReplaceStr("<reg>", reg).ReplaceStr("<buffer>", buffer).ReplaceStr("<field>", field);
			}
		}

		buffer_index += (m_bind->gds_pointers.pointers_num > 0 ? (m_bind->gds_pointers.pointers_num - 1) / 4 + 1 : 0);

		for (int i = 0; i < m_bind->direct_sgprs.sgprs_num; i++)
		{
			int start_reg = m_bind->direct_sgprs.start_register[i];

			EXIT_IF(buffer_index + i / 4 >= static_cast<int>(m_bind->push_constant_size) / 16);

			String8 buffer = String8::FromPrintf("%d", buffer_index + i / 4);
			String8 reg    = String8::FromPrintf("s%d", start_reg + shift_regs);
			String8 field  = String8::FromPrintf("%d", i % 4);
			m_source += String8(text).ReplaceStr("<reg>", reg).ReplaceStr("<buffer>", buffer).ReplaceStr("<field>", field);
		}

		/* buffer_index += (m_bind->direct_sgprs.sgprs_num > 0 ? (m_bind->direct_sgprs.sgprs_num - 1) / 4 + 1 : 0); */

		if (m_bind->extended.used)
		{
			// TODO() load pointer

			printf("Extended mapping: ");
			for (auto& m: m_extended_mapping)
			{
				printf("{%d, %d} ", m[0], m[1]);
			}
			printf("\n");
		}
	}

	static const char* common_init = R"(
               OpStore %exec_lo %uint_1
               OpStore %exec_hi %uint_0
               OpStore %execz %uint_0
               OpStore %scc %uint_0
	)";

	m_source += common_init;
	m_source += "\n";
}

void Spirv::WriteLabel(int index)
{
	if (index > 0)
	{
		const auto& instructions = m_code.GetInstructions();
		const auto& inst         = instructions.At(index);

		auto& labels     = m_code.GetLabels();
		int   labels_num = 0;
		for (uint32_t i = labels.Size(); i > 0; i--)
		{
			auto& label = labels[i - 1];
			if (!label.IsDisabled() && label.GetDst() == inst.pc)
			{
				static const char* text = R"(
                   <branch>
                   %<label> = OpLabel
		)";

				bool discard = m_code.ReadBlock(label.GetDst()).is_discard;

				bool skip_branch = (discard || ((instructions.At(index - 1).type == ShaderInstructionType::SEndpgm ||
				                                 instructions.At(index - 1).type == ShaderInstructionType::SBranch) &&
				                                labels_num == 0));

				m_source +=
				    String8(text).ReplaceStr("<branch>", (skip_branch ? "" : "OpBranch %<label>")).ReplaceStr("<label>", label.ToString());
				labels_num++;

				bool backward_sbranch = false;
				for (const auto& source_inst: instructions)
				{
					if (source_inst.pc == label.GetSrc())
					{
						backward_sbranch = source_inst.type == ShaderInstructionType::SBranch;
						break;
					}
				}

				if (backward_sbranch && label.GetSrc() > label.GetDst())
				{
					if (find_backward_loop_merge(m_code, label).Size() != 0)
					{
						continue;
					}

					// SPIR-V requires OpLoopMerge to be immediately followed by
					// OpBranch or OpBranchConditional. The guest body becomes a
					// dedicated successor so the header is a valid structured
					// loop construct (same pattern as the embedded mipmap helper).
					String8 continue_label = String8::FromPrintf("loop_continue_%04" PRIx32, label.GetSrc());
					String8 merge_label    = String8::FromPrintf("loop_merge_%04" PRIx32, label.GetSrc());
					String8 body_label     = String8::FromPrintf("loop_body_%04" PRIx32, label.GetSrc());
					m_source += String8::FromPrintf("               OpLoopMerge %%%s %%%s None\n"
					                                "               OpBranch %%%s\n"
					                                "       %%%s = OpLabel\n",
					                                merge_label.c_str(), continue_label.c_str(), body_label.c_str(), body_label.c_str());
				}

				if (discard)
				{
					label.Disable();
					break;
				}
			}
		}
	}
}

void Spirv::ModifyCode()
{
	struct DiscardLabel
	{
		ShaderControlFlowBlock block;
		int                    num = 0;
	};
	const auto&          labels = m_code.GetLabels();
	Vector<DiscardLabel> dls;
	for (const auto& l: labels)
	{
		if (!l.IsDisabled())
		{
			int  num = 0;
			auto pc  = l.GetDst();
			for (const auto& l2: labels)
			{
				if (l2.GetDst() == pc)
				{
					num++;
				}
			}
			EXIT_IF(num == 0);
			if (num > 1)
			{
				if (auto block = m_code.ReadBlock(pc); block.is_discard)
				{
					if (!dls.Contains(pc, [](auto d, auto pc) { return d.block.pc == pc; }))
					{
						dls.Add(DiscardLabel({block, num - 1}));
					}
				}
			}
		}
	}
	for (const auto& dl: dls)
	{
		auto block = m_code.ReadIntructions(dl.block);
		for (int i = 0; i < dl.num; i++)
		{
			// Duplicate discard block if there are different branches with the same label
			m_code.GetInstructions().Add(block);
		}
	}
}

// NOLINTNEXTLINE(readability-function-cognitive-complexity)
void Spirv::DetectFetch()
{
	EXIT_IF(m_vs_input_info == nullptr);
	EXIT_IF(!m_vs_input_info->fetch_embedded);

	EXIT_NOT_IMPLEMENTED(!m_vs_input_info->gs_prolog);
	EXIT_NOT_IMPLEMENTED(m_vs_input_info->fetch_inline);

	enum class Type
	{
		Unknown,
		Attrib,
		Buffer,
		Index
	};

	struct VgprInfo
	{
		Type type = Type::Unknown;
	};

	struct SgprInfo
	{
		Type type      = Type::Unknown;
		int  attrib_id = 0;
	};

	auto is_sgpr = [](const ShaderOperand& op)
	{ return op.type == ShaderOperandType::Sgpr || op.type == ShaderOperandType::VccLo || op.type == ShaderOperandType::VccHi; };
	auto sgpr_reg = [](const ShaderOperand& op)
	{ return (op.type == ShaderOperandType::VccLo ? 106 : (op.type == ShaderOperandType::VccHi ? 107 : op.register_id)); };
	auto is_vgpr  = [](const ShaderOperand& op) { return op.type == ShaderOperandType::Vgpr; };
	auto vgpr_reg = [](const ShaderOperand& op) { return op.register_id; };

	int shift_regs = 8;
	int attrib_reg = m_vs_input_info->fetch_attrib_reg + shift_regs;
	int buffer_reg = m_vs_input_info->fetch_buffer_reg + shift_regs;

	Vector<ShaderControlFlowBlock> blocks;

	blocks.Add(m_code.ReadBlock(0));

	for (const auto& label: m_code.GetLabels())
	{
		blocks.Add(m_code.ReadBlock(label.GetDst()));
	}

	for (const auto& label: m_code.GetIndirectLabels())
	{
		blocks.Add(m_code.ReadBlock(label.GetDst()));
	}

	Vector<std::pair<ShaderInstruction, int>> load_instructions;

	for (const auto& block: blocks)
	{
		auto code = m_code.ReadIntructions(block);

		Core::Array<SgprInfo, 108> sgprs;
		Core::Array<VgprInfo, 256> vgprs;

		for (const auto& inst: code)
		{

			switch (inst.type)
			{
				case ShaderInstructionType::SLoadDword:
				case ShaderInstructionType::SLoadDwordx2:
				case ShaderInstructionType::SLoadDwordx4:
				case ShaderInstructionType::SLoadDwordx8:
				case ShaderInstructionType::SLoadDwordx16:
					if (is_sgpr(inst.src[0]) && sgpr_reg(inst.src[0]) == attrib_reg)
					{
						EXIT_NOT_IMPLEMENTED(!operand_is_constant(inst.src[1]));
						EXIT_NOT_IMPLEMENTED(inst.src[1].constant.i < 0);
						int register_id = sgpr_reg(inst.dst);
						int index       = inst.src[1].constant.i / 4;
						for (int i = 0; i < inst.dst.size; i++)
						{
							sgprs[register_id + i].type      = Type::Attrib;
							sgprs[register_id + i].attrib_id = i + index;
						}
					}
					if (is_sgpr(inst.src[0]) && sgpr_reg(inst.src[0]) == buffer_reg)
					{
						EXIT_NOT_IMPLEMENTED(operand_is_constant(inst.src[1]));
						EXIT_NOT_IMPLEMENTED(is_sgpr(inst.src[1]) && sgprs[sgpr_reg(inst.src[1])].type != Type::Attrib);
						int register_id = sgpr_reg(inst.dst);
						for (int i = 0; i < inst.dst.size; i++)
						{
							sgprs[register_id + i].type      = Type::Buffer;
							sgprs[register_id + i].attrib_id = sgprs[sgpr_reg(inst.src[1])].attrib_id;
						}
					}
					break;

				case ShaderInstructionType::VCndmaskB32:
					if (is_vgpr(inst.src[0]) && vgpr_reg(inst.src[0]) == 8 && is_vgpr(inst.src[1]) && vgpr_reg(inst.src[1]) == 5)
					{
						vgprs[vgpr_reg(inst.dst)].type = Type::Index;
					}
					break;

				case ShaderInstructionType::SBfeU32:
				case ShaderInstructionType::SAndB32:
				case ShaderInstructionType::SLshlB32:
					if (is_sgpr(inst.src[0]) && sgprs[sgpr_reg(inst.src[0])].type == Type::Attrib && operand_is_constant(inst.src[1]))
					{
						sgprs[sgpr_reg(inst.dst)] = sgprs[sgpr_reg(inst.src[0])];
					}
					break;

				case ShaderInstructionType::BufferLoadFormatX:
				case ShaderInstructionType::BufferLoadFormatXy:
				case ShaderInstructionType::BufferLoadFormatXyz:
				case ShaderInstructionType::BufferLoadFormatXyzw:
				{
					EXIT_NOT_IMPLEMENTED(!(vgprs[vgpr_reg(inst.src[0])].type == Type::Index &&
					                       sgprs[sgpr_reg(inst.src[1])].type == Type::Buffer &&
					                       sgprs[sgpr_reg(inst.src[2])].type == Type::Attrib));

					load_instructions.Add({inst, sgprs[sgpr_reg(inst.src[1])].attrib_id});

					break;
				}
				default: break;
			}
		}
	}

	for (auto& inst: m_code.GetInstructions())
	{
		if (auto index = load_instructions.Find(inst.pc, [](auto i, auto pc) { return i.first.pc == pc; });
		    load_instructions.IndexValid(index))
		{
			const auto& p = load_instructions.At(index);

			printf("load vertex: pc = 0x%08" PRIx32 ", size = %d, attrib_id = %d\n", p.first.pc, p.first.dst.size, p.second);

			EXIT_IF(inst.type != p.first.type);
			EXIT_IF(inst.format != p.first.format);

			switch (inst.type)
			{
				case ShaderInstructionType::BufferLoadFormatX: inst.type = ShaderInstructionType::FetchX; break;
				case ShaderInstructionType::BufferLoadFormatXy: inst.type = ShaderInstructionType::FetchXy; break;
				case ShaderInstructionType::BufferLoadFormatXyz: inst.type = ShaderInstructionType::FetchXyz; break;
				case ShaderInstructionType::BufferLoadFormatXyzw: inst.type = ShaderInstructionType::FetchXyzw; break;
				default: break;
			}

			inst.src[2].type       = ShaderOperandType::IntegerInlineConstant;
			inst.src[2].size       = 0;
			inst.src[2].constant.i = p.second;
		}
	}
}

void Spirv::WriteInstructions()
{
	ModifyCode();

	const bool  uses_arrayed_2d_images = UsesArrayed2dImages(m_bind);
	const bool  uses_three_dimensional_images = UsesThreeDimensionalImages(m_bind);
	const bool  uses_uint_images       = UsesUnsignedIntegerImages(m_bind);
	int         index        = -1;
	const auto& instructions = m_code.GetInstructions();
	bool        need_debug   = (Config::SpirvDebugPrintfEnabled() && !m_code.GetDebugPrintfs().IsEmpty());
	for (const auto& inst: instructions)
	{
		index++;
		EXIT_NOT_IMPLEMENTED(uses_arrayed_2d_images && IsImageInstruction(inst) && !SupportsArrayed2dImageInstruction(inst));
		EXIT_NOT_IMPLEMENTED(uses_three_dimensional_images && IsImageInstruction(inst) && !SupportsArrayed2dImageInstruction(inst));
		EXIT_NOT_IMPLEMENTED(uses_uint_images && IsImageInstruction(inst) && !SupportsArrayed2dImageInstruction(inst));

		WriteLabel(index);

		String8 src = ShaderCode::DbgInstructionToStr(inst);
		String8 dst;
		String8 dst_debug;

		bool ok = false;

		const auto* func = RecompFunc(inst.type, inst.format);

		if (func != nullptr)
		{
			EXIT_IF(func->type != inst.type);
			EXIT_IF(func->format != inst.format);
			ok = func->func(index, m_code, &dst, this, func->param, func->scc_check);
		}

		if (!ok)
		{
			printf("%s\n", m_source.c_str());
			EXIT("Can't recompile: %s\n", src.c_str());
		}

		m_source += String8::FromPrintf("; %s\n", src.c_str());
		m_source += String8::FromPrintf("%s\n", dst.c_str());

		if (need_debug && Recompile_Inject_Debug(index, m_code, &dst_debug, this, nullptr, SccCheck::None))
		{
			m_source += String8::FromPrintf("%s\n", dst_debug.c_str());
		}
	}
}

void Spirv::WriteMainEpilog()
{
	static const char* text = R"(
                   ; Epilog
                   OpFunctionEnd
)";

	m_source += text;
}

void Spirv::WriteFunctions()
{
	if (m_code.HasAnyOf({ShaderInstructionType::BufferLoadUbyte}))
	{
		m_source += BUFFER_LOAD_UBYTE;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::VSadU32}))
	{
		m_source += FUNC_ABS_DIFF;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::SWqmB64}))
	{
		m_source += FUNC_WQM;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::SAddcU32, ShaderInstructionType::VAddCoCiU32}))
	{
		m_source += FUNC_ADDC;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::SLshl4AddU32}))
	{
		m_source += FUNC_LSHL_ADD;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::ImageStoreMip}))
	{
		m_source += FUNC_MIPMAP;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::VCmpOF32, ShaderInstructionType::VCmpUF32}))
	{
		m_source += FUNC_ORDERED;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::VMulLoI32, ShaderInstructionType::VMulLoU32, ShaderInstructionType::VMulHiU32,
	                     ShaderInstructionType::VMadU32U24, ShaderInstructionType::VMulU32U24, ShaderInstructionType::SMulHiU32}))
	{
		m_source += FUNC_MUL_EXTENDED;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::SLshrB64, ShaderInstructionType::SBfeU64}))
	{
		m_source += FUNC_SHIFT_RIGHT;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::SLshlB64, ShaderInstructionType::SBfeU64}))
	{
		m_source += FUNC_SHIFT_LEFT;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::SSwappcB64, ShaderInstructionType::FetchX, ShaderInstructionType::FetchXy,
	                     ShaderInstructionType::FetchXyz, ShaderInstructionType::FetchXyzw}))
	{
		m_source += FUNC_FETCH_1;
		m_source += FUNC_FETCH_2;
		m_source += FUNC_FETCH_3;
		m_source += FUNC_FETCH_4;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::BufferLoadDword, ShaderInstructionType::BufferLoadDwordx2,
	                     ShaderInstructionType::BufferLoadDwordx3, ShaderInstructionType::BufferLoadDwordx4,
	                     ShaderInstructionType::BufferLoadFormatX, ShaderInstructionType::BufferLoadFormatXy,
	                     ShaderInstructionType::BufferLoadFormatXyz, ShaderInstructionType::BufferLoadFormatXyzw,
	                     ShaderInstructionType::TBufferLoadFormatX, ShaderInstructionType::TBufferLoadFormatXyzw}))
	{
		m_source += BUFFER_LOAD_FLOAT1;
		m_source += BUFFER_LOAD_FLOAT4;
		m_source += TBUFFER_LOAD_FORMAT_X;
		m_source += TBUFFER_LOAD_FORMAT_XYZW;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::BufferStoreDword, ShaderInstructionType::BufferStoreDwordx2,
	                     ShaderInstructionType::BufferStoreDwordx3, ShaderInstructionType::BufferStoreDwordx4,
	                     ShaderInstructionType::BufferStoreFormatX, ShaderInstructionType::BufferStoreFormatXy,
	                     ShaderInstructionType::BufferStoreFormatXyzw}))
	{
		m_source += BUFFER_STORE_FLOAT1;
		m_source += BUFFER_STORE_FLOAT2;
		m_source += BUFFER_STORE_FLOAT4;
		m_source += TBUFFER_STORE_FORMAT_X;
		m_source += TBUFFER_STORE_FORMAT_XY;
		m_source += TBUFFER_STORE_FORMAT_XYZW;
	}

	if (m_code.HasAnyOf({ShaderInstructionType::SBufferLoadDword, ShaderInstructionType::SBufferLoadDwordx2,
	                     ShaderInstructionType::SBufferLoadDwordx4, ShaderInstructionType::SBufferLoadDwordx8,
	                     ShaderInstructionType::SBufferLoadDwordx16}))
	{
		m_source += SBUFFER_LOAD_DWORD;
		m_source += SBUFFER_LOAD_DWORD_2;
		m_source += SBUFFER_LOAD_DWORD_4;
		m_source += SBUFFER_LOAD_DWORD_8;
		m_source += SBUFFER_LOAD_DWORD_16;
	}
}

void Spirv::FindConstants()
{
	m_constants.Clear();
	AddConstantFloat(0.0f);
	AddConstantFloat(0.5f);
	AddConstantFloat(1.0f);
	AddConstantFloat(2.0f);
	AddConstantFloat(4.0f);
	for (int i = 0; i <= 32; i++)
	{
		AddConstantInt(i);
		AddConstantUint(i);
	}
	for (const auto& inst: m_code.GetInstructions())
	{
		if (inst.type == ShaderInstructionType::SBarrier)
		{
			AddConstantUint(SPIRV_WORKGROUP_MEMORY_ACQ_REL);
		}
		for (int i = 0; i < inst.src_num; i++)
		{
			if (operand_is_constant(inst.src[i]))
			{
				AddConstant(inst.src[i]);
				// MUBUF/MTBUF soffset is OpStore'd into %temp_int_* (signed).
				// LiteralConstant only registers as Uint via AddConstant; also
				// emit the Int twin so GetConstantInt succeeds at recompile.
				// Offsets such as 56/80/136 appear as folded 12-bit+imm values.
				if (inst.src[i].type == ShaderOperandType::LiteralConstant)
				{
					AddConstantInt(static_cast<int>(inst.src[i].constant.u));
				}
			}
		}
		// SMEM dual-offset path adds SGPR soffset + signed imm in SPIR-V.
		if (inst.smem_imm_offset != 0)
		{
			AddConstantUint(static_cast<uint32_t>(inst.smem_imm_offset));
		}
	}
	// Attribute-table dwords materialized by SLoad from fetch_attrib_reg.
	if (m_vs_input_info != nullptr)
	{
		for (int i = 0; i < m_vs_input_info->fetch_attrib_data_num; i++)
		{
			AddConstantUint(m_vs_input_info->fetch_attrib_data[i]);
		}
	}
	if (m_vs_input_info != nullptr || m_ps_input_info != nullptr || m_cs_input_info != nullptr)
	{
		AddConstantInt(12);
		AddConstantInt(16);
		AddConstantInt(31);
		AddConstantInt(36);
		AddConstantInt(39);
		AddConstantInt(75);
		AddConstantInt(76);
		AddConstantInt(77);
		AddConstantInt(92);
		AddConstantInt(95);
		AddConstantInt(119);
		AddConstantUint(24);
		AddConstantUint(31);
		AddConstantUint(32);
		AddConstantUint(63);
		AddConstantUint(64);
		AddConstantUint(72);
		AddConstantUint(127);
		AddConstantUint(103);
		AddConstantUint(112);
		AddConstantUint(126);
		AddConstantUint(143);
		AddConstantUint(255);
		AddConstantUint(0x00008000);
		AddConstantUint(0x000000ff);
		AddConstantUint(0x007fffff);
		AddConstantUint(0x00800000);
		AddConstantUint(0x00000200);
		AddConstantUint(0x00007c00);
		AddConstantUint(0x00007bff);
		AddConstantUint(0x0000ffff);
		AddConstantUint(0x3fff);
		AddConstantUint(0xffffff);
		AddConstantUint(0xffffffff);
		AddConstantUint(0x0000000f);
		AddConstantUint(0x000000f0);
		AddConstantUint(0x00000f00);
		AddConstantUint(0x0000f000);
		AddConstantUint(0x000f0000);
		AddConstantUint(0x00f00000);
		AddConstantUint(0x0f000000);
		AddConstantUint(0xf0000000);
	}
	if (m_ps_input_info != nullptr && m_ps_input_info->ps_pos_xy && !m_ps_input_info->host_to_guest_scale.IsIdentity())
	{
		const auto& scale = m_ps_input_info->host_to_guest_scale;
		AddConstantFloat(static_cast<float>(scale.x_guest_numerator));
		AddConstantFloat(static_cast<float>(scale.x_host_denominator));
		AddConstantFloat(static_cast<float>(scale.y_guest_numerator));
		AddConstantFloat(static_cast<float>(scale.y_host_denominator));
	}
	if (m_cs_input_info != nullptr)
	{
		AddConstantUint(m_cs_input_info->threads_num[0]);
		AddConstantUint(m_cs_input_info->threads_num[1]);
		AddConstantUint(m_cs_input_info->threads_num[2]);
	}
}

void Spirv::FindVariables()
{
	m_variables.Clear();

	AddVariable(ShaderOperandType::Vgpr, 0, 1);
	AddVariable(ShaderOperandType::ExecLo, 0, 2);
	AddVariable(ShaderOperandType::ExecZ, 0, 1);
	AddVariable(ShaderOperandType::Scc, 0, 1);

	for (const auto& inst: m_code.GetInstructions())
	{
		AddVariable(inst.dst);
		AddVariable(inst.dst2);
		for (int i = 0; i < inst.src_num; i++)
		{
			AddVariable(inst.src[i]);
		}
		for (int address = 0; address < inst.mimg_address_num; ++address)
		{
			AddVariable(inst.mimg_address[address]);
		}
	}

	if (m_vs_input_info != nullptr)
	{
		if (m_vs_input_info->gs_prolog)
		{
			AddVariable(ShaderOperandType::Vgpr, 5, 1);
			AddVariable(ShaderOperandType::Vgpr, 8, 1);
		} else
		{
			AddVariable(ShaderOperandType::Vgpr, 3, 1);
		}
		for (int i = 0; i < m_vs_input_info->resources_num; i++)
		{
			AddVariable(ShaderOperandType::Vgpr, m_vs_input_info->resources_dst[i].register_start,
			            m_vs_input_info->resources_dst[i].registers_num);
		}
	}

	if (m_ps_input_info != nullptr)
	{
		if (m_ps_input_info->ps_pos_xy)
		{
			EXIT_NOT_IMPLEMENTED(!m_ps_input_info->host_to_guest_scale.IsValid());
			AddVariable(ShaderOperandType::Vgpr, 2, 1);
			AddVariable(ShaderOperandType::Vgpr, 3, 1);
		}
	}

	if (m_cs_input_info != nullptr)
	{
		AddVariable(ShaderOperandType::Vgpr, 0, 3);
		AddVariable(ShaderOperandType::Sgpr, m_cs_input_info->workgroup_register, 3);
	}

	if (m_bind != nullptr)
	{
		int shift_regs = (m_vs_input_info != nullptr && m_vs_input_info->gs_prolog ? 8 : 0);

		for (int i = 0; i < m_bind->storage_buffers.buffers_num; i++)
		{
			int storage_start = m_bind->storage_buffers.start_register[i] + shift_regs;
			AddVariable(ShaderOperandType::Sgpr, storage_start, 4);
		}
		for (int i = 0; i < m_bind->textures2D.textures_num; i++)
		{
			int storage_start = m_bind->textures2D.desc[i].start_register + shift_regs;
			AddVariable(ShaderOperandType::Sgpr, storage_start, 8);
		}
		for (int i = 0; i < m_bind->samplers.samplers_num; i++)
		{
			int storage_start = m_bind->samplers.start_register[i] + shift_regs;
			AddVariable(ShaderOperandType::Sgpr, storage_start, 8);
		}
		for (int i = 0; i < m_bind->direct_sgprs.sgprs_num; i++)
		{
			int direct_start = m_bind->direct_sgprs.start_register[i] + shift_regs;
			AddVariable(ShaderOperandType::Sgpr, direct_start, 1);
		}
	}
}

bool Spirv::CanLoadPackedHalfForExport(int export_index, ShaderOperand op) const
{
	if (op.type != ShaderOperandType::Vgpr || op.size != 1 || export_index <= 0)
	{
		return false;
	}

	const int   reg          = op.register_id;
	const auto& insts        = m_code.GetInstructions();
	const int   insts_count  = static_cast<int>(insts.Size());
	const int   search_start = (export_index < insts_count ? export_index - 1 : insts_count - 1);

	for (int i = search_start; i >= 0; i--)
	{
		const auto& prev = insts.At(i);

		if (instruction_changes_control_flow(prev))
		{
			return false;
		}

		if (!instruction_writes_vgpr(prev, reg))
		{
			continue;
		}

		return prev.type == ShaderInstructionType::VCvtPkrtzF16F32 && operand_covers_vgpr(prev.dst, reg);
	}

	return false;
}

String8 SpirvGenerateSource(const ShaderCode& code, const ShaderVertexInputInfo* vs_input_info, const ShaderPixelInputInfo* ps_input_info,
                            const ShaderComputeInputInfo* cs_input_info)
{
	Spirv spirv;
	spirv.SetCode(code);
	spirv.SetVsInputInfo(vs_input_info);
	spirv.SetPsInputInfo(ps_input_info);
	spirv.SetCsInputInfo(cs_input_info);
	spirv.GenerateSource();

	return spirv.GetSource();
}

String8 SpirvGetEmbeddedVs(uint32_t id)
{
	EXIT_NOT_IMPLEMENTED(id != 0);

	return EMBEDDED_SHADER_VS_0;
}

String8 SpirvGetEmbeddedPs(uint32_t id)
{
	EXIT_NOT_IMPLEMENTED(id != 0);

	return EMBEDDED_SHADER_PS_0;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
