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
// T#.size (nodes - 1) bounds the node index (pointer >> 3): an out-of-range
// node returns four invalid children for a box or a miss for a triangle, which
// is what ends traversal of an empty (all-zero) acceleration structure.
// Lanes outside EXEC neither load nor compute, and a switch on the node type
// runs only the layout the node holds, loading only the words it reads. A
// traversal that walks one node per wave keeps the type uniform, so the other
// layouts cost no instructions.
KYTY_RECOMPILER_FUNC(Recompile_ImageBvhIntersectRay_Vdata4BvhAddressSrsrc4)
{
	const auto& inst = code.GetInstructions().At(index);
	if (!spirv->UsesGuestDeviceAddress() || inst.mimg_address_num != 11 || inst.dst.size != 4)
	{
		return false;
	}
	const auto  i    = String8::FromPrintf("bvh_%u", index);
	String8     s;
	auto        line = [&](const char* fmt, auto... args) { s += String8::FromPrintf(fmt, args...); };
	const auto* p    = i.c_str();
	const auto  invalid  = spirv->GetConstantUint(0xffffffffu);
	const auto  inf_bits = spirv->GetConstantUint(0x7f800000u);
	const auto  ninf_bits = spirv->GetConstantUint(0xff800000u);

	// Skip operand loads and address math for inactive lanes.
	s += String8(R"(
%<p>_exec = OpLoad %uint %exec_lo
%<p>_active = OpINotEqual %bool %<p>_exec %uint_0
OpSelectionMerge %<p>_merge None
OpBranchConditional %<p>_active %<p>_work %<p>_merge
%<p>_work = OpLabel
)").ReplaceStr("<p>", i);

	auto load_ray = [&](int reg)
	{
		line("%%%s_a%d = OpLoad %%float %%%s\n", p, reg, operand_variable_to_str(inst.mimg_address[reg]).value.c_str());
	};
	// Node pointer and T# descriptor words for address, bounds check, and switch dispatch.
	load_ray(0);
	line("%%%s_ptr = OpBitcast %%uint %%%s_a0\n", p, p);
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
%<p>_sort_bit = OpShiftRightLogical %uint %<p>_t1 %uint_31
%<p>_sort = OpINotEqual %bool %<p>_sort_bit %uint_0
%<p>_type = OpBitwiseAnd %uint %<p>_ptr %uint_7
%<p>_ptr_hi = OpShiftRightLogical %uint %<p>_ptr %uint_29
%<p>_ptr_clear = OpBitwiseAnd %uint %<p>_ptr %<not7>
%<p>_off_lo = OpShiftLeftLogical %uint %<p>_ptr_clear %uint_3
%<p>_addr_lo = OpIAdd %uint %<p>_base_lo %<p>_off_lo
%<p>_carry_b = OpULessThan %bool %<p>_addr_lo %<p>_base_lo
%<p>_carry = OpSelect %uint %<p>_carry_b %uint_1 %uint_0
%<p>_hi_sum = OpIAdd %uint %<p>_base_hi %<p>_ptr_hi
%<p>_addr_hi = OpIAdd %uint %<p>_hi_sum %<p>_carry
%<p>_index = OpShiftRightLogical %uint %<p>_ptr %<three>
%<p>_size_hi = OpBitwiseAnd %uint %<p>_t3 %<size_hi_mask>
%<p>_size_big = OpINotEqual %bool %<p>_size_hi %uint_0
%<p>_index_fits = OpULessThanEqual %bool %<p>_index %<p>_t2
%<p>_in_bounds = OpLogicalOr %bool %<p>_size_big %<p>_index_fits
%<p>_type_oob = OpBitwiseOr %uint %<p>_type %uint_8
%<p>_kind = OpSelect %uint %<p>_in_bounds %<p>_type %<p>_type_oob
%<p>_inf = OpBitcast %float %<inf_bits>
%<p>_ninf = OpBitcast %float %<ninf_bits>
OpSelectionMerge %<p>_kind_end None
OpSwitch %<p>_kind %<p>_user 0 %<p>_tri 1 %<p>_tri 4 %<p>_box16 5 %<p>_box32 8 %<p>_tri_oob 9 %<p>_tri_oob
)").ReplaceStr("<p>", i)
	                                                        .ReplaceStr("<not7>", spirv->GetConstantUint(0xfffffff8u))
	                                                        .ReplaceStr("<size_hi_mask>", spirv->GetConstantUint(0x3ffu))
	                                                        .ReplaceStr("<three>", spirv->GetConstantUint(3u))
	                                                        .ReplaceStr("<inf_bits>", inf_bits)
	                                                        .ReplaceStr("<ninf_bits>", ninf_bits);

	// Split fp16 (box16: 16 dwords) and fp32 (box32: 28 dwords) box nodes so
	// neither loads unused bounds or keeps unused ray operands alive.
	const int pairs[5][2] = {{0, 2}, {1, 3}, {0, 1}, {2, 3}, {1, 2}};
	auto      emit_box    = [&](bool fp16, const char* tag, int final_versions[4]) -> bool
	{
		const auto bx   = String8::FromPrintf("%s_%s", p, tag);
		const auto nd_p = bx + "_n";
		const int  dws  = fp16 ? 16 : 28;
		line("%%%s = OpLabel\n", bx.c_str());
		if (!spirv->EmitGuestLoad64Aligned(i + "_addr_lo", i + "_addr_hi", dws, nd_p, &s))
		{
			return false;
		}
		s += String8(R"(
%<bx>_grow_raw = OpShiftRightLogical %uint %<p>_t1 %uint_23
%<bx>_grow = OpBitwiseAnd %uint %<bx>_grow_raw %uint_255
%<bx>_eps = OpConvertUToF %float %<bx>_grow
%<bx>_ulp = OpBitcast %float %<ulp_bits>
%<bx>_grow_f = OpFMul %float %<bx>_eps %<bx>_ulp
%<bx>_grow_scale = OpFAdd %float %float_1_000000 %<bx>_grow_f
)").ReplaceStr("<bx>", bx)
		         .ReplaceStr("<p>", i)
		         .ReplaceStr("<ulp_bits>", spirv->GetConstantUint(0x33800000u));

		for (int k: {1, 2, 3, 4, 8, 9, 10})
		{
			line("%%%s_a%d = OpLoad %%float %%%s\n", bx.c_str(), k, operand_variable_to_str(inst.mimg_address[k]).value.c_str());
		}

		const char* axis[3] = {"2", "3", "4"};
		const char* inv[3]  = {"8", "9", "10"};
		for (int c = 0; c < 4; c++)
		{
			if (fp16)
			{
				const char* comps[6][2] = {{"0", "0"}, {"0", "1"}, {"1", "0"}, {"1", "1"}, {"2", "0"}, {"2", "1"}};
				for (int k = 0; k < 3; k++)
				{
					line("%%%s_h_%d_%d = OpExtInst %%v2float %%GLSL_std_450 UnpackHalf2x16 %%%s_d%d\n", bx.c_str(), c, k, nd_p.c_str(),
					     4 + 3 * c + k);
				}
				for (int k = 0; k < 6; k++)
				{
					line("%%%s_bb_%d_%d = OpCompositeExtract %%float %%%s_h_%d_%s %s\n", bx.c_str(), c, k, bx.c_str(), c, comps[k][0],
					     comps[k][1]);
				}
			} else
			{
				for (int k = 0; k < 6; k++)
				{
					line("%%%s_bb_%d_%d = OpBitcast %%float %%%s_d%d\n", bx.c_str(), c, k, nd_p.c_str(), 4 + 6 * c + k);
				}
			}
			for (int a = 0; a < 3; a++)
			{
				line("%%%s_c%d_lo%d = OpFSub %%float %%%s_bb_%d_%d %%%s_a%s\n"
				     "%%%s_c%d_hi%d = OpFSub %%float %%%s_bb_%d_%d %%%s_a%s\n"
				     "%%%s_c%d_tlo%d = OpFMul %%float %%%s_c%d_lo%d %%%s_a%s\n"
				     "%%%s_c%d_thi%d = OpFMul %%float %%%s_c%d_hi%d %%%s_a%s\n"
				     "%%%s_c%d_pos%d = OpFOrdGreaterThanEqual %%bool %%%s_a%s %%float_0_000000\n"
				     "%%%s_c%d_in%d = OpSelect %%float %%%s_c%d_pos%d %%%s_c%d_tlo%d %%%s_c%d_thi%d\n"
				     "%%%s_c%d_out%d = OpSelect %%float %%%s_c%d_pos%d %%%s_c%d_thi%d %%%s_c%d_tlo%d\n",
				     bx.c_str(), c, a, bx.c_str(), c, a, bx.c_str(), axis[a], bx.c_str(), c, a, bx.c_str(), c, a + 3, bx.c_str(),
				     axis[a], bx.c_str(), c, a, bx.c_str(), c, a, bx.c_str(), inv[a], bx.c_str(), c, a, bx.c_str(), c, a, bx.c_str(),
				     inv[a], bx.c_str(), c, a, bx.c_str(), inv[a], bx.c_str(), c, a, bx.c_str(), c, a, bx.c_str(), c, a, bx.c_str(), c,
				     a, bx.c_str(), c, a, bx.c_str(), c, a, bx.c_str(), c, a, bx.c_str(), c, a);
			}
			s += String8(R"(
%<bx>_c<c>_in01 = OpExtInst %float %GLSL_std_450 NMax %<bx>_c<c>_in0 %<bx>_c<c>_in1
%<bx>_c<c>_enter = OpExtInst %float %GLSL_std_450 NMax %<bx>_c<c>_in01 %<bx>_c<c>_in2
%<bx>_c<c>_out01 = OpExtInst %float %GLSL_std_450 NMin %<bx>_c<c>_out0 %<bx>_c<c>_out1
%<bx>_c<c>_exit = OpExtInst %float %GLSL_std_450 NMin %<bx>_c<c>_out01 %<bx>_c<c>_out2
%<bx>_c<c>_tmin0 = OpExtInst %float %GLSL_std_450 NMax %<bx>_c<c>_enter %float_0_000000
%<bx>_c<c>_tmax0 = OpExtInst %float %GLSL_std_450 NMin %<bx>_c<c>_exit %<bx>_a1
%<bx>_c<c>_nan_in = OpIsNan %bool %<bx>_c<c>_enter
%<bx>_c<c>_nan_out = OpIsNan %bool %<bx>_c<c>_exit
%<bx>_c<c>_nan = OpLogicalOr %bool %<bx>_c<c>_nan_in %<bx>_c<c>_nan_out
%<bx>_c<c>_tmin = OpSelect %float %<bx>_c<c>_nan %<inf> %<bx>_c<c>_tmin0
%<bx>_c<c>_tmax = OpSelect %float %<bx>_c<c>_nan %<ninf> %<bx>_c<c>_tmax0
%<bx>_c<c>_grown = OpFMul %float %<bx>_c<c>_tmax %<bx>_grow_scale
%<bx>_c<c>_hit = OpFOrdLessThanEqual %bool %<bx>_c<c>_tmin %<bx>_c<c>_grown
%<bx>_c<c>_node = OpSelect %uint %<bx>_c<c>_hit %<nd>_d<c> %<invalid>
%<bx>_k<c>_0 = OpCopyObject %float %<bx>_c<c>_tmin
%<bx>_v<c>_0 = OpCopyObject %uint %<bx>_c<c>_node
)").ReplaceStr("<bx>", bx)
			         .ReplaceStr("<nd>", nd_p)
			         .ReplaceStr("<c>", String8::FromPrintf("%d", c))
			         .ReplaceStr("<inf>", i + "_inf")
			         .ReplaceStr("<ninf>", i + "_ninf")
			         .ReplaceStr("<invalid>", invalid);
		}
		for (int c = 0; c < 4; c++)
		{
			final_versions[c] = 0;
		}
		for (int n = 0; n < 5; n++)
		{
			const int  a  = pairs[n][0];
			const int  b  = pairs[n][1];
			const auto va = String8::FromPrintf("%s_v%d_%d", bx.c_str(), a, final_versions[a]);
			const auto vb = String8::FromPrintf("%s_v%d_%d", bx.c_str(), b, final_versions[b]);
			const auto ka = String8::FromPrintf("%s_k%d_%d", bx.c_str(), a, final_versions[a]);
			const auto kb = String8::FromPrintf("%s_k%d_%d", bx.c_str(), b, final_versions[b]);
			final_versions[a]++;
			final_versions[b]++;
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
)").ReplaceStr("<sw>", String8::FromPrintf("%s_sw%d", bx.c_str(), n))
			         .ReplaceStr("<va2>", String8::FromPrintf("%s_v%d_%d", bx.c_str(), a, final_versions[a]))
			         .ReplaceStr("<vb2>", String8::FromPrintf("%s_v%d_%d", bx.c_str(), b, final_versions[b]))
			         .ReplaceStr("<ka2>", String8::FromPrintf("%s_k%d_%d", bx.c_str(), a, final_versions[a]))
			         .ReplaceStr("<kb2>", String8::FromPrintf("%s_k%d_%d", bx.c_str(), b, final_versions[b]))
			         .ReplaceStr("<va>", va)
			         .ReplaceStr("<vb>", vb)
			         .ReplaceStr("<ka>", ka)
			         .ReplaceStr("<kb>", kb)
			         .ReplaceStr("<p>", i)
			         .ReplaceStr("<invalid>", invalid);
		}
		line("OpBranch %%%s_end\n%%%s_end = OpLabel\nOpBranch %%%s_kind_end\n", bx.c_str(), bx.c_str(), p);
		return true;
	};

	int ver16[4] = {};
	int ver32[4] = {};
	if (!emit_box(true, "box16", ver16) || !emit_box(false, "box32", ver32))
	{
		return false;
	}

	line("%%%s_tri = OpLabel\n", p);
	// Triangle node: vertices in words 0-11, triangle id in word 15. Pick
	// vertices by type (0: V0,V1,V2; 1: V1,V3,V2).
	if (!spirv->EmitGuestLoad64Aligned(i + "_addr_lo", i + "_addr_hi", 16, i + "_tn", &s))
	{
		return false;
	}
	for (int k: {2, 3, 4, 5, 6, 7})
	{
		line("%%%s_tri_a%d = OpLoad %%float %%%s\n", p, k, operand_variable_to_str(inst.mimg_address[k]).value.c_str());
	}
	s += String8(R"(
%<p>_mode_raw = OpShiftRightLogical %uint %<p>_t3 %uint_24
%<p>_mode_bit = OpBitwiseAnd %uint %<p>_mode_raw %uint_1
%<p>_mode1 = OpINotEqual %bool %<p>_mode_bit %uint_0
)").ReplaceStr("<p>", i);
	auto g = [&](int w) { return String8::FromPrintf("%s_g%d", p, w); };
	for (int w = 0; w < 12; w++)
	{
		line("%%%s_g%d = OpBitcast %%float %%%s_tn_d%d\n", p, w, p, w);
	}
	s += String8::FromPrintf("%%%s_tri1 = OpIEqual %%bool %%%s_type %%uint_1\n", p, p);
	const int first0[3] = {0, 3, 6};
	const int first1[3] = {3, 9, 6};
	for (int v = 0; v < 3; v++)
	{
		for (int k = 0; k < 3; k++)
		{
			line("%%%s_v%d%d = OpSelect %%float %%%s_tri1 %%%s %%%s\n", p, v, k, p, g(first1[v] + k).c_str(), g(first0[v] + k).c_str());
		}
		line("%%%s_vec%d = OpCompositeConstruct %%v3float %%%s_v%d0 %%%s_v%d1 %%%s_v%d2\n", p, v, p, v, p, v, p, v);
	}
	s += String8(R"(
%<p>_orig = OpCompositeConstruct %v3float %<p>_tri_a2 %<p>_tri_a3 %<p>_tri_a4
%<p>_dir = OpCompositeConstruct %v3float %<p>_tri_a5 %<p>_tri_a6 %<p>_tri_a7
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
%<p>_id = OpCopyObject %uint %<p>_tn_d15
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
OpBranch %<p>_tri_end
%<p>_tri_end = OpLabel
OpBranch %<p>_kind_end
%<p>_tri_oob = OpLabel
OpBranch %<p>_kind_end
%<p>_user = OpLabel
OpBranch %<p>_kind_end
%<p>_kind_end = OpLabel
)").ReplaceStr("<p>", i).ReplaceStr("<inf>", i + "_inf");
	// Out-of-range triangles miss; boxes out of range and user nodes return
	// four invalid pointers.
	const String8 triangle_miss[4] = {inf_bits, spirv->GetConstantUint(0x3f800000u), spirv->GetConstantUint(0u),
	                                  spirv->GetConstantUint(0u)};
	for (int r = 0; r < 4; r++)
	{
		line("%%%s_r%d = OpPhi %%uint %%%s_tri_r%d %%%s_tri_end %%%s_box16_v%d_%d %%%s_box16_end %%%s_box32_v%d_%d %%%s_box32_end %%%s "
		     "%%%s_tri_oob %%%s %%%s_user\n",
		     p, r, p, r, p, p, r, ver16[r], p, p, r, ver32[r], p, triangle_miss[r].c_str(), p, invalid.c_str(), p);
	}
	for (int r = 0; r < 4; r++)
	{
		line("%%%s_r%d_f = OpBitcast %%float %%%s_r%d\nOpStore %%%s %%%s_r%d_f\n", p, r, p, r, operand_variable_to_str(inst.dst, r).value.c_str(),
		     p, r);
	}
	line("OpBranch %%%s_merge\n%%%s_merge = OpLabel\n", p, p);
	*dst_source += s;
	return true;
}

} // namespace Kyty::Libs::Graphics

#endif // KYTY_EMU_ENABLED
