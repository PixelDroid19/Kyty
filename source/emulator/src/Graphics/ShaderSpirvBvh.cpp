#include "ShaderSpirvEmitters.h"
#include "ShaderSpirvInternal.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Libs::Graphics {

// image_bvh_intersect_ray (RDNA2 ISA 8.2.10, RTIP 1.1 node formats):
//   node address = T#.base * 256 + (node_pointer & ~7) * 8, node type = ptr & 7
//   types 0/1: triangle (vertices V0,V1,V2 / V1,V3,V2), 4: fp16 box, 5: fp32
//   box, others: user nodes returning 0xFFFFFFFF.
// Box nodes return the four child pointers, misses as 0xFFFFFFFF, sorted by
// entry distance when T# box sorting is enabled; the hit test grows the exit
// distance by T#.box_grow ULPs of 2^-24. Triangle nodes return
// {t_num, t_denom, triangle_id, hit} (T# return mode 0) or
// {t_num, t_denom, I_num, J_num} with the node's barycentric rotation (mode 1).
// Every result is computed and the node type selects one, so the lowering has
// no data-dependent control flow besides the EXEC-gated store.
KYTY_RECOMPILER_FUNC(Recompile_ImageBvhIntersectRay_Vdata4BvhAddressSrsrc4)
{
	const auto& inst = code.GetInstructions().At(index);
	if (!spirv->UsesGuestDeviceAddress() || inst.mimg_address_num != 11 || inst.dst.size != 4)
	{
		return false;
	}
	const auto i = String8::FromPrintf("bvh_%u", index);
	String8    s;
	auto       line = [&](const char* fmt, auto... args) { s += String8::FromPrintf(fmt, args...); };
	const auto* p   = i.c_str();

	// Ray operands (float) and node pointer.
	for (int k = 0; k < 11; k++)
	{
		line("%%%s_a%d = OpLoad %%float %%%s\n", p, k, operand_variable_to_str(inst.mimg_address[k]).value.c_str());
	}
	line("%%%s_ptr = OpBitcast %%uint %%%s_a0\n", p, p);
	// T# words.
	for (int w = 0; w < 4; w++)
	{
		line("%%%s_t%d = OpLoad %%uint %%%s\n", p, w, operand_variable_to_str(inst.src[1], w).value.c_str());
	}
	s += String8(R"(
%<p>_base_lo = OpShiftLeftLogical %uint %<p>_t0 %uint_8
%<p>_base_top = OpShiftRightLogical %uint %<p>_t0 %uint_24
%<p>_t1_lo8 = OpBitwiseAnd %uint %<p>_t1 %uint_255
%<p>_t1_sh = OpShiftLeftLogical %uint %<p>_t1_lo8 %uint_8
%<p>_base_hi = OpBitwiseOr %uint %<p>_base_top %<p>_t1_sh
%<p>_grow_raw = OpShiftRightLogical %uint %<p>_t1 %uint_23
%<p>_grow = OpBitwiseAnd %uint %<p>_grow_raw %uint_255
%<p>_sort_bit = OpShiftRightLogical %uint %<p>_t1 %uint_31
%<p>_sort = OpINotEqual %bool %<p>_sort_bit %uint_0
%<p>_mode_raw = OpShiftRightLogical %uint %<p>_t3 %uint_24
%<p>_mode_bit = OpBitwiseAnd %uint %<p>_mode_raw %uint_1
%<p>_mode1 = OpINotEqual %bool %<p>_mode_bit %uint_0
%<p>_type = OpBitwiseAnd %uint %<p>_ptr %uint_7
%<p>_ptr_hi = OpShiftRightLogical %uint %<p>_ptr %uint_29
%<p>_ptr_clear = OpBitwiseAnd %uint %<p>_ptr %<not7>
%<p>_off_lo = OpShiftLeftLogical %uint %<p>_ptr_clear %uint_3
%<p>_addr_lo = OpIAdd %uint %<p>_base_lo %<p>_off_lo
%<p>_carry_b = OpULessThan %bool %<p>_addr_lo %<p>_base_lo
%<p>_carry = OpSelect %uint %<p>_carry_b %uint_1 %uint_0
%<p>_hi_sum = OpIAdd %uint %<p>_base_hi %<p>_ptr_hi
%<p>_addr_hi = OpIAdd %uint %<p>_hi_sum %<p>_carry
%<p>_is16 = OpIEqual %bool %<p>_type %uint_4
)").ReplaceStr("<p>", i).ReplaceStr("<not7>", spirv->GetConstantUint(0xfffffff8u));
	if (!spirv->EmitGuestLoad(i + "_addr_lo", i + "_addr_hi", 32, i + "_n", &s))
	{
		return false;
	}
	auto f = [&](int w) { return String8::FromPrintf("%s_f%d", p, w); };
	for (int w = 0; w < 32; w++)
	{
		line("%%%s_f%d = OpBitcast %%float %%%s_n_d%d\n", p, w, p, w);
	}

	// Box children bounds: fp32 layout, and fp16 layout unpacked to fp32.
	for (int c = 0; c < 4; c++)
	{
		for (int k = 0; k < 6; k++)
		{
			line("%%%s_b32_%d_%d = OpCopyObject %%float %%%s\n", p, c, k, f(4 + 6 * c + k).c_str());
		}
		for (int k = 0; k < 3; k++)
		{
			line("%%%s_h_%d_%d = OpExtInst %%v2float %%GLSL_std_450 UnpackHalf2x16 %%%s_n_d%d\n", p, c, k, p, 4 + 3 * c + k);
		}
		const char* comps[6][2] = {{"0", "0"}, {"0", "1"}, {"1", "0"}, {"1", "1"}, {"2", "0"}, {"2", "1"}};
		for (int k = 0; k < 6; k++)
		{
			line("%%%s_b16_%d_%d = OpCompositeExtract %%float %%%s_h_%d_%s %s\n", p, c, k, p, c, comps[k][0], comps[k][1]);
			line("%%%s_bb_%d_%d = OpSelect %%float %%%s_is16 %%%s_b16_%d_%d %%%s_b32_%d_%d\n", p, c, k, p, p, c, k, p, c, k);
		}
	}
	// Slab tests: grown exit distance, entry distance used as sort key.
	s += String8(R"(
%<p>_eps = OpConvertUToF %float %<p>_grow
%<p>_ulp = OpBitcast %float %<ulp_bits>
%<p>_grow_f = OpFMul %float %<p>_eps %<p>_ulp
%<p>_grow_scale = OpFAdd %float %float_1_000000 %<p>_grow_f
%<p>_inf = OpBitcast %float %<inf_bits>
%<p>_ninf = OpBitcast %float %<ninf_bits>
)").ReplaceStr("<p>", i).ReplaceStr("<inf_bits>", spirv->GetConstantUint(0x7f800000u)).ReplaceStr("<ninf_bits>", spirv->GetConstantUint(0xff800000u)).ReplaceStr("<ulp_bits>", spirv->GetConstantUint(0x33800000u));
	const char* axis[3] = {"2", "3", "4"};
	const char* inv[3]  = {"8", "9", "10"};
	for (int c = 0; c < 4; c++)
	{
		for (int a = 0; a < 3; a++)
		{
			line("%%%s_c%d_lo%d = OpFSub %%float %%%s_bb_%d_%d %%%s_a%s\n"
			     "%%%s_c%d_hi%d = OpFSub %%float %%%s_bb_%d_%d %%%s_a%s\n"
			     "%%%s_c%d_tlo%d = OpFMul %%float %%%s_c%d_lo%d %%%s_a%s\n"
			     "%%%s_c%d_thi%d = OpFMul %%float %%%s_c%d_hi%d %%%s_a%s\n"
			     "%%%s_c%d_pos%d = OpFOrdGreaterThanEqual %%bool %%%s_a%s %%float_0_000000\n"
			     "%%%s_c%d_in%d = OpSelect %%float %%%s_c%d_pos%d %%%s_c%d_tlo%d %%%s_c%d_thi%d\n"
			     "%%%s_c%d_out%d = OpSelect %%float %%%s_c%d_pos%d %%%s_c%d_thi%d %%%s_c%d_tlo%d\n",
			     p, c, a, p, c, a, p, axis[a], p, c, a, p, c, a + 3, p, axis[a], p, c, a, p, c, a, p, inv[a], p, c, a, p, c, a, p,
			     inv[a], p, c, a, p, inv[a], p, c, a, p, c, a, p, c, a, p, c, a, p, c, a, p, c, a, p, c, a, p, c, a);
		}
		s += String8(R"(
%<p>_c<c>_in01 = OpExtInst %float %GLSL_std_450 NMax %<p>_c<c>_in0 %<p>_c<c>_in1
%<p>_c<c>_enter = OpExtInst %float %GLSL_std_450 NMax %<p>_c<c>_in01 %<p>_c<c>_in2
%<p>_c<c>_out01 = OpExtInst %float %GLSL_std_450 NMin %<p>_c<c>_out0 %<p>_c<c>_out1
%<p>_c<c>_exit = OpExtInst %float %GLSL_std_450 NMin %<p>_c<c>_out01 %<p>_c<c>_out2
%<p>_c<c>_tmin0 = OpExtInst %float %GLSL_std_450 NMax %<p>_c<c>_enter %float_0_000000
%<p>_c<c>_tmax0 = OpExtInst %float %GLSL_std_450 NMin %<p>_c<c>_exit %<p>_a1
%<p>_c<c>_nan_in = OpIsNan %bool %<p>_c<c>_enter
%<p>_c<c>_nan_out = OpIsNan %bool %<p>_c<c>_exit
%<p>_c<c>_nan = OpLogicalOr %bool %<p>_c<c>_nan_in %<p>_c<c>_nan_out
%<p>_c<c>_tmin = OpSelect %float %<p>_c<c>_nan %<inf> %<p>_c<c>_tmin0
%<p>_c<c>_tmax = OpSelect %float %<p>_c<c>_nan %<ninf> %<p>_c<c>_tmax0
%<p>_c<c>_grown = OpFMul %float %<p>_c<c>_tmax %<p>_grow_scale
%<p>_c<c>_hit = OpFOrdLessThanEqual %bool %<p>_c<c>_tmin %<p>_c<c>_grown
%<p>_c<c>_node = OpSelect %uint %<p>_c<c>_hit %<p>_n_d<c> %<invalid>
%<p>_k<c>_0 = OpCopyObject %float %<p>_c<c>_tmin
%<p>_v<c>_0 = OpCopyObject %uint %<p>_c<c>_node
)").ReplaceStr("<p>", i)
		         .ReplaceStr("<c>", String8::FromPrintf("%d", c))
		         .ReplaceStr("<inf>", i + "_inf")
		         .ReplaceStr("<ninf>", i + "_ninf")
		         .ReplaceStr("<invalid>", spirv->GetConstantUint(0xffffffffu));
	}
	// Sorting network (0,2)(1,3)(0,1)(2,3)(1,2): swap when B is valid and
	// nearer, or A is invalid, and only when T# box sorting is enabled.
	int        version[4]  = {0, 0, 0, 0};
	const int  pairs[5][2] = {{0, 2}, {1, 3}, {0, 1}, {2, 3}, {1, 2}};
	for (int n = 0; n < 5; n++)
	{
		const int  a  = pairs[n][0];
		const int  b  = pairs[n][1];
		const auto va = String8::FromPrintf("%s_v%d_%d", p, a, version[a]);
		const auto vb = String8::FromPrintf("%s_v%d_%d", p, b, version[b]);
		const auto ka = String8::FromPrintf("%s_k%d_%d", p, a, version[a]);
		const auto kb = String8::FromPrintf("%s_k%d_%d", p, b, version[b]);
		version[a]++;
		version[b]++;
		s += String8(R"(
%<sw>_bvalid = OpINotEqual %bool %<vb> %<invalid>
%<sw>_near = OpFOrdLessThan %bool %<kb> %<ka>
%<sw>_both = OpLogicalAnd %bool %<sw>_bvalid %<sw>_near
%<sw>_ainv = OpIEqual %bool %<va> %<invalid>
%<sw>_any = OpLogicalOr %bool %<sw>_both %<sw>_ainv
%<sw> = OpLogicalAnd %bool %<sw>_any %<p>_sort
%<va2> = OpSelect %uint %<sw> %<vb> %<va>
%<vb2> = OpSelect %uint %<sw> %<va> %<vb>
%<ka2> = OpSelect %float %<sw> %<kb> %<ka>
%<kb2> = OpSelect %float %<sw> %<ka> %<kb>
)").ReplaceStr("<sw>", String8::FromPrintf("%s_sw%d", p, n))
		         .ReplaceStr("<va2>", String8::FromPrintf("%s_v%d_%d", p, a, version[a]))
		         .ReplaceStr("<vb2>", String8::FromPrintf("%s_v%d_%d", p, b, version[b]))
		         .ReplaceStr("<ka2>", String8::FromPrintf("%s_k%d_%d", p, a, version[a]))
		         .ReplaceStr("<kb2>", String8::FromPrintf("%s_k%d_%d", p, b, version[b]))
		         .ReplaceStr("<va>", va)
		         .ReplaceStr("<vb>", vb)
		         .ReplaceStr("<ka>", ka)
		         .ReplaceStr("<kb>", kb)
		         .ReplaceStr("<p>", i)
		         .ReplaceStr("<invalid>", spirv->GetConstantUint(0xffffffffu));
	}

	// Triangle: pick vertices by type (0: V0,V1,V2; 1: V1,V3,V2).
	s += String8::FromPrintf("%%%s_tri1 = OpIEqual %%bool %%%s_type %%uint_1\n", p, p);
	const int first0[3] = {0, 3, 6};
	const int first1[3] = {3, 9, 6};
	for (int v = 0; v < 3; v++)
	{
		for (int k = 0; k < 3; k++)
		{
			line("%%%s_v%d%d = OpSelect %%float %%%s_tri1 %%%s %%%s\n", p, v, k, p, f(first1[v] + k).c_str(), f(first0[v] + k).c_str());
		}
		line("%%%s_vec%d = OpCompositeConstruct %%v3float %%%s_v%d0 %%%s_v%d1 %%%s_v%d2\n", p, v, p, v, p, v, p, v);
	}
	s += String8(R"(
%<p>_orig = OpCompositeConstruct %v3float %<p>_a2 %<p>_a3 %<p>_a4
%<p>_dir = OpCompositeConstruct %v3float %<p>_a5 %<p>_a6 %<p>_a7
%<p>_e1 = OpFSub %v3float %<p>_vec1 %<p>_vec0
%<p>_e2 = OpFSub %v3float %<p>_vec2 %<p>_vec0
%<p>_e3 = OpFSub %v3float %<p>_orig %<p>_vec0
%<p>_s1 = OpExtInst %v3float %GLSL_std_450 Cross %<p>_dir %<p>_e2
%<p>_s2 = OpExtInst %v3float %GLSL_std_450 Cross %<p>_e3 %<p>_e1
%<p>_rx = OpDot %float %<p>_e2 %<p>_s2
%<p>_ry = OpDot %float %<p>_s1 %<p>_e1
%<p>_rz = OpDot %float %<p>_e3 %<p>_s1
%<p>_rw = OpDot %float %<p>_dir %<p>_s2
%<p>_t = OpFDiv %float %<p>_rx %<p>_ry
%<p>_u = OpFDiv %float %<p>_rz %<p>_ry
%<p>_v = OpFDiv %float %<p>_rw %<p>_ry
%<p>_m0 = OpFOrdLessThan %bool %<p>_u %float_0_000000
%<p>_m1 = OpFOrdGreaterThan %bool %<p>_u %float_1_000000
%<p>_m2 = OpFOrdLessThan %bool %<p>_v %float_0_000000
%<p>_uv = OpFAdd %float %<p>_u %<p>_v
%<p>_m3 = OpFOrdGreaterThan %bool %<p>_uv %float_1_000000
%<p>_m4 = OpFOrdLessThan %bool %<p>_t %float_0_000000
%<p>_m01 = OpLogicalOr %bool %<p>_m0 %<p>_m1
%<p>_m23 = OpLogicalOr %bool %<p>_m2 %<p>_m3
%<p>_m0123 = OpLogicalOr %bool %<p>_m01 %<p>_m23
%<p>_miss = OpLogicalOr %bool %<p>_m0123 %<p>_m4
%<p>_tnum = OpSelect %float %<p>_miss %<inf> %<p>_rx
%<p>_tden = OpSelect %float %<p>_miss %float_1_000000 %<p>_ry
%<p>_bc0 = OpFSub %float %<p>_ry %<p>_rz
%<p>_bc0b = OpFSub %float %<p>_bc0 %<p>_rw
%<p>_id = OpCopyObject %uint %<p>_n_d15
%<p>_shift = OpIMul %uint %<p>_type %uint_8
%<p>_isel_raw = OpShiftRightLogical %uint %<p>_id %<p>_shift
%<p>_isel = OpBitwiseAnd %uint %<p>_isel_raw %uint_3
%<p>_jsh = OpIAdd %uint %<p>_shift %uint_2
%<p>_jsel_raw = OpShiftRightLogical %uint %<p>_id %<p>_jsh
%<p>_jsel = OpBitwiseAnd %uint %<p>_jsel_raw %uint_3
%<p>_i1 = OpIEqual %bool %<p>_isel %uint_1
%<p>_i2 = OpIEqual %bool %<p>_isel %uint_2
%<p>_ia = OpSelect %float %<p>_i1 %<p>_rz %<p>_bc0b
%<p>_inum = OpSelect %float %<p>_i2 %<p>_rw %<p>_ia
%<p>_j1 = OpIEqual %bool %<p>_jsel %uint_1
%<p>_j2 = OpIEqual %bool %<p>_jsel %uint_2
%<p>_ja = OpSelect %float %<p>_j1 %<p>_rz %<p>_bc0b
%<p>_jnum = OpSelect %float %<p>_j2 %<p>_rw %<p>_ja
%<p>_hit_u = OpSelect %uint %<p>_miss %uint_0 %uint_1
%<p>_tri_r0 = OpBitcast %uint %<p>_tnum
%<p>_tri_r1 = OpBitcast %uint %<p>_tden
%<p>_inum_u = OpBitcast %uint %<p>_inum
%<p>_jnum_u = OpBitcast %uint %<p>_jnum
%<p>_tri_r2 = OpSelect %uint %<p>_mode1 %<p>_inum_u %<p>_id
%<p>_tri_r3 = OpSelect %uint %<p>_mode1 %<p>_jnum_u %<p>_hit_u
%<p>_is_tri = OpULessThan %bool %<p>_type %uint_2
%<p>_is_box16 = OpIEqual %bool %<p>_type %uint_4
%<p>_is_box32 = OpIEqual %bool %<p>_type %uint_5
%<p>_is_box = OpLogicalOr %bool %<p>_is_box16 %<p>_is_box32
)").ReplaceStr("<p>", i).ReplaceStr("<inf>", i + "_inf");
	const auto invalid = spirv->GetConstantUint(0xffffffffu);
	for (int r = 0; r < 4; r++)
	{
		line("%%%s_rb%d = OpSelect %%uint %%%s_is_box %%%s_v%d_%d %%%s\n"
		     "%%%s_r%d = OpSelect %%uint %%%s_is_tri %%%s_tri_r%d %%%s_rb%d\n"
		     "%%%s_r%d_f = OpBitcast %%float %%%s_r%d\n",
		     p, r, p, p, r, version[r], invalid.c_str(), p, r, p, p, r, p, r, p, r, p, r);
	}
	// EXEC-gated store of the four results.
	line("%%%s_exec = OpLoad %%uint %%exec_lo\n%%%s_active = OpINotEqual %%bool %%%s_exec %%uint_0\n"
	     "OpSelectionMerge %%%s_merge None\nOpBranchConditional %%%s_active %%%s_store %%%s_merge\n%%%s_store = OpLabel\n",
	     p, p, p, p, p, p, p, p);
	for (int r = 0; r < 4; r++)
	{
		line("OpStore %%%s %%%s_r%d_f\n", operand_variable_to_str(inst.dst, r).value.c_str(), p, r);
	}
	line("OpBranch %%%s_merge\n%%%s_merge = OpLabel\n", p, p);
	*dst_source += s;
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
