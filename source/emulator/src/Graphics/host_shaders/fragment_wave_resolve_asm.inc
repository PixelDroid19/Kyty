// Generated from the public resolve kernel. Do not edit by hand.
constexpr const char* kFragmentResolveWriterSignature = R"CORE(%write_colors_u1_ = OpFunction %void None %22
       %base = OpFunctionParameter %_ptr_Function_uint
)CORE";
constexpr const char* kFragmentResolveCore = R"CORE(; SPIR-V
; Version: 1.6
; Generator: Google Shaderc over Glslang; 11
; Bound: 426
; Schema: 0
               OpCapability Shader
               OpCapability Geometry
               OpCapability DemoteToHelperInvocation
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main" %transport <resolve_interface> %result_buffer %control_buffer %gl_HelperInvocation %record_buffer %reference_buffer %gl_FragCoord %gl_PrimitiveID
               OpExecutionMode %main OriginUpperLeft
               OpSource GLSL 460
               OpSourceExtension "GL_GOOGLE_cpp_style_line_directive"
               OpSourceExtension "GL_GOOGLE_include_directive"
               OpName %main "main"
               OpName %quad_words_ "quad_words("
               OpName %wave_table_offset_ "wave_table_offset("
               OpName %active_reference_offset_ "active_reference_offset("
               OpName %output_reference_offset_ "output_reference_offset("
               OpName %hash_key_u1_u1_ "hash_key(u1;u1;"
               OpName %primitive "primitive"
               OpName %xy "xy"
               OpName %write_colors_u1_ "write_colors(u1;"
               OpName %base "base"
               OpName %fail_u1_ "fail(u1;"
               OpName %reason "reason"
               OpName %TransportParameters "TransportParameters"
               OpMemberName %TransportParameters 0 "quad_capacity"
               OpMemberName %TransportParameters 1 "primitive_count"
               OpMemberName %TransportParameters 2 "lane_words"
               OpMemberName %TransportParameters 3 "wave_header_words"
               OpMemberName %TransportParameters 4 "quad_lookup_capacity"
               OpMemberName %TransportParameters 5 "wave_capacity"
               OpMemberName %TransportParameters 6 "wave_lookup_capacity"
               OpName %transport "transport"
               OpName %h "h"
               OpName %Results "Results"
               OpMemberName %Results 0 "results"
               OpName %result_buffer "result_buffer"
               OpName %Control "Control"
               OpMemberName %Control 0 "control"
               OpName %control_buffer "control_buffer"
               OpName %gl_HelperInvocation "gl_HelperInvocation"
               OpName %Records "Records"
               OpMemberName %Records 0 "records"
               OpName %record_buffer "record_buffer"
               OpName %References "References"
               OpMemberName %References 0 "references"
               OpName %reference_buffer "reference_buffer"
               OpName %param "param"
               OpName %pixel "pixel"
               OpName %gl_FragCoord "gl_FragCoord"
               OpName %quad "quad"
               OpName %primitive_0 "primitive"
               OpName %gl_PrimitiveID "gl_PrimitiveID"
               OpName %param_0 "param"
               OpName %xy_0 "xy"
               OpName %hash "hash"
               OpName %param_1 "param"
               OpName %param_2 "param"
               OpName %record "record"
               OpName %attempt "attempt"
               OpName %base_0 "base"
               OpName %param_3 "param"
               OpName %lane "lane"
               OpName %wave_lane "wave_lane"
               OpName %param_4 "param"
               OpName %base_1 "base"
               OpName %required_components "required_components"
               OpName %param_5 "param"
               OpName %param_6 "param"
               OpDecorate %TransportParameters Block
               OpMemberDecorate %TransportParameters 0 Offset 0
               OpMemberDecorate %TransportParameters 1 Offset 4
               OpMemberDecorate %TransportParameters 2 Offset 8
               OpMemberDecorate %TransportParameters 3 Offset 12
               OpMemberDecorate %TransportParameters 4 Offset 16
               OpMemberDecorate %TransportParameters 5 Offset 20
               OpMemberDecorate %TransportParameters 6 Offset 24
               OpDecorate %_runtimearr_uint ArrayStride 4
               OpDecorate %Results Block
               OpMemberDecorate %Results 0 NonWritable
               OpMemberDecorate %Results 0 Offset 0
               OpDecorate %result_buffer NonWritable
               OpDecorate %result_buffer Binding 4
               OpDecorate %result_buffer DescriptorSet 0
               OpDecorate %_runtimearr_uint_0 ArrayStride 4
               OpDecorate %Control Block
               OpMemberDecorate %Control 0 Offset 0
               OpDecorate %control_buffer Binding 1
               OpDecorate %control_buffer DescriptorSet 0
               OpDecorate %gl_HelperInvocation BuiltIn HelperInvocation
               OpDecorate %gl_HelperInvocation Volatile
               OpDecorate %_runtimearr_uint_1 ArrayStride 4
               OpDecorate %Records Block
               OpMemberDecorate %Records 0 NonWritable
               OpMemberDecorate %Records 0 Offset 0
               OpDecorate %record_buffer NonWritable
               OpDecorate %record_buffer Binding 0
               OpDecorate %record_buffer DescriptorSet 0
               OpDecorate %_runtimearr_uint_2 ArrayStride 4
               OpDecorate %References Block
               OpMemberDecorate %References 0 NonWritable
               OpMemberDecorate %References 0 Offset 0
               OpDecorate %reference_buffer NonWritable
               OpDecorate %reference_buffer Binding 2
               OpDecorate %reference_buffer DescriptorSet 0
               OpDecorate %gl_FragCoord BuiltIn FragCoord
               OpDecorate %gl_PrimitiveID BuiltIn PrimitiveId
               OpDecorate %gl_PrimitiveID Flat
<resolve_annotations>
       %void = OpTypeVoid
          %3 = OpTypeFunction %void
       %uint = OpTypeInt 32 0
          %7 = OpTypeFunction %uint
%_ptr_Function_uint = OpTypePointer Function %uint
         %17 = OpTypeFunction %uint %_ptr_Function_uint %_ptr_Function_uint
         %22 = OpTypeFunction %void %_ptr_Function_uint
     %uint_4 = OpConstant %uint 4
%TransportParameters = OpTypeStruct %uint %uint %uint %uint %uint %uint %uint
%_ptr_PushConstant_TransportParameters = OpTypePointer PushConstant %TransportParameters
  %transport = OpVariable %_ptr_PushConstant_TransportParameters PushConstant
        %int = OpTypeInt 32 1
      %int_2 = OpConstant %int 2
%_ptr_PushConstant_uint = OpTypePointer PushConstant %uint
    %uint_16 = OpConstant %uint 16
     %uint_2 = OpConstant %uint 2
      %int_1 = OpConstant %int 1
      %int_4 = OpConstant %int 4
      %int_6 = OpConstant %int 6
      %int_5 = OpConstant %int 5
%uint_2654435769 = OpConstant %uint 2654435769
%uint_2246822507 = OpConstant %uint 2246822507
%uint_2146121005 = OpConstant %uint 2146121005
    %uint_15 = OpConstant %uint 15
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
%_ptr_Output_v4float = OpTypePointer Output %v4float
%_runtimearr_uint = OpTypeRuntimeArray %uint
    %Results = OpTypeStruct %_runtimearr_uint
%_ptr_StorageBuffer_Results = OpTypePointer StorageBuffer %Results
%result_buffer = OpVariable %_ptr_StorageBuffer_Results StorageBuffer
      %int_0 = OpConstant %int 0
%_ptr_StorageBuffer_uint = OpTypePointer StorageBuffer %uint
     %uint_3 = OpConstant %uint 3
     %uint_5 = OpConstant %uint 5
     %v4uint = OpTypeVector %uint 4
%_runtimearr_uint_0 = OpTypeRuntimeArray %uint
    %Control = OpTypeStruct %_runtimearr_uint_0
%_ptr_StorageBuffer_Control = OpTypePointer StorageBuffer %Control
%control_buffer = OpVariable %_ptr_StorageBuffer_Control StorageBuffer
     %uint_1 = OpConstant %uint 1
     %uint_0 = OpConstant %uint 0
       %bool = OpTypeBool
%_ptr_Input_bool = OpTypePointer Input %bool
%gl_HelperInvocation = OpVariable %_ptr_Input_bool Input
%_runtimearr_uint_1 = OpTypeRuntimeArray %uint
    %Records = OpTypeStruct %_runtimearr_uint_1
%_ptr_StorageBuffer_Records = OpTypePointer StorageBuffer %Records
%record_buffer = OpVariable %_ptr_StorageBuffer_Records StorageBuffer
%_runtimearr_uint_2 = OpTypeRuntimeArray %uint
 %References = OpTypeStruct %_runtimearr_uint_2
%_ptr_StorageBuffer_References = OpTypePointer StorageBuffer %References
%reference_buffer = OpVariable %_ptr_StorageBuffer_References StorageBuffer
    %uint_64 = OpConstant %uint 64
    %uint_34 = OpConstant %uint 34
   %uint_128 = OpConstant %uint 128
     %v2uint = OpTypeVector %uint 2
%_ptr_Function_v2uint = OpTypePointer Function %v2uint
%_ptr_Input_v4float = OpTypePointer Input %v4float
%gl_FragCoord = OpVariable %_ptr_Input_v4float Input
    %v2float = OpTypeVector %float 2
%_ptr_Input_int = OpTypePointer Input %int
%gl_PrimitiveID = OpVariable %_ptr_Input_int Input
 %uint_32768 = OpConstant %uint 32768
%uint_4294967295 = OpConstant %uint 4294967295
    %uint_32 = OpConstant %uint 32
%required_components = OpConstant %uint <resolve_mask>
   %uint_256 = OpConstant %uint 256
     %uint_8 = OpConstant %uint 8
<resolve_declarations>
       %main = OpFunction %void None %3
          %5 = OpLabel
      %param = OpVariable %_ptr_Function_uint Function
      %pixel = OpVariable %_ptr_Function_v2uint Function
       %quad = OpVariable %_ptr_Function_v2uint Function
%primitive_0 = OpVariable %_ptr_Function_uint Function
    %param_0 = OpVariable %_ptr_Function_uint Function
       %xy_0 = OpVariable %_ptr_Function_uint Function
       %hash = OpVariable %_ptr_Function_uint Function
    %param_1 = OpVariable %_ptr_Function_uint Function
    %param_2 = OpVariable %_ptr_Function_uint Function
     %record = OpVariable %_ptr_Function_uint Function
    %attempt = OpVariable %_ptr_Function_uint Function
     %base_0 = OpVariable %_ptr_Function_uint Function
    %param_3 = OpVariable %_ptr_Function_uint Function
       %lane = OpVariable %_ptr_Function_uint Function
  %wave_lane = OpVariable %_ptr_Function_uint Function
    %param_4 = OpVariable %_ptr_Function_uint Function
     %base_1 = OpVariable %_ptr_Function_uint Function
    %param_5 = OpVariable %_ptr_Function_uint Function
    %param_6 = OpVariable %_ptr_Function_uint Function
        %134 = OpLoad %bool %gl_HelperInvocation
               OpSelectionMerge %136 None
               OpBranchConditional %134 %135 %136
        %135 = OpLabel
               OpDemoteToHelperInvocation
               OpBranch %136
        %136 = OpLabel
        %137 = OpArrayLength %uint %control_buffer 0
        %138 = OpBitcast %int %137
        %139 = OpBitcast %uint %138
        %140 = OpULessThan %bool %139 %uint_16
               OpSelectionMerge %142 None
               OpBranchConditional %140 %141 %142
        %141 = OpLabel
               OpDemoteToHelperInvocation
               OpBranch %142
        %142 = OpLabel
        %143 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %144 = OpAtomicIAdd %uint %143 %uint_1 %uint_0 %uint_0
        %145 = OpINotEqual %bool %144 %uint_0
               OpSelectionMerge %147 None
               OpBranchConditional %145 %146 %147
        %146 = OpLabel
               OpDemoteToHelperInvocation
               OpBranch %147
        %147 = OpLabel
        %148 = OpArrayLength %uint %control_buffer 0
        %149 = OpBitcast %int %148
        %150 = OpBitcast %uint %149
        %151 = OpFunctionCall %uint %wave_table_offset_
        %152 = OpAccessChain %_ptr_PushConstant_uint %transport %int_6
        %153 = OpLoad %uint %152
        %154 = OpIAdd %uint %151 %153
        %155 = OpULessThan %bool %150 %154
        %156 = OpLogicalNot %bool %155
               OpSelectionMerge %158 None
               OpBranchConditional %156 %157 %158
        %157 = OpLabel
        %163 = OpArrayLength %uint %record_buffer 0
        %164 = OpBitcast %int %163
        %165 = OpBitcast %uint %164
        %166 = OpAccessChain %_ptr_PushConstant_uint %transport %int_0
        %167 = OpLoad %uint %166
        %168 = OpFunctionCall %uint %quad_words_
        %169 = OpIMul %uint %167 %168
        %170 = OpULessThan %bool %165 %169
               OpBranch %158
        %158 = OpLabel
        %171 = OpPhi %bool %155 %147 %170 %157
        %172 = OpLogicalNot %bool %171
               OpSelectionMerge %174 None
               OpBranchConditional %172 %173 %174
        %173 = OpLabel
        %179 = OpArrayLength %uint %reference_buffer 0
        %180 = OpBitcast %int %179
        %181 = OpBitcast %uint %180
        %182 = OpFunctionCall %uint %output_reference_offset_
        %183 = OpAccessChain %_ptr_PushConstant_uint %transport %int_0
        %184 = OpLoad %uint %183
        %185 = OpIAdd %uint %182 %184
        %186 = OpULessThan %bool %181 %185
               OpBranch %174
        %174 = OpLabel
        %187 = OpPhi %bool %171 %158 %186 %173
        %188 = OpLogicalNot %bool %187
               OpSelectionMerge %190 None
               OpBranchConditional %188 %189 %190
        %189 = OpLabel
        %191 = OpArrayLength %uint %result_buffer 0
        %192 = OpBitcast %int %191
        %193 = OpBitcast %uint %192
        %194 = OpAccessChain %_ptr_PushConstant_uint %transport %int_5
        %195 = OpLoad %uint %194
        %197 = OpIMul %uint %195 %uint_64
        %199 = OpIMul %uint %197 %uint_34
        %200 = OpULessThan %bool %193 %199
               OpBranch %190
        %190 = OpLabel
        %201 = OpPhi %bool %187 %174 %200 %189
               OpSelectionMerge %203 None
               OpBranchConditional %201 %202 %203
        %202 = OpLabel
               OpStore %param %uint_128
        %206 = OpFunctionCall %void %fail_u1_ %param
               OpDemoteToHelperInvocation
               OpBranch %203
        %203 = OpLabel
        %213 = OpLoad %v4float %gl_FragCoord
        %214 = OpVectorShuffle %v2float %213 %213 0 1
        %215 = OpConvertFToU %v2uint %214
               OpStore %pixel %215
        %217 = OpLoad %v2uint %pixel
        %218 = OpCompositeConstruct %v2uint %uint_2 %uint_2
        %219 = OpUDiv %v2uint %217 %218
               OpStore %quad %219
        %223 = OpLoad %int %gl_PrimitiveID
        %224 = OpBitcast %uint %223
               OpStore %primitive_0 %224
        %225 = OpLoad %uint %primitive_0
        %226 = OpAccessChain %_ptr_PushConstant_uint %transport %int_1
        %227 = OpLoad %uint %226
        %228 = OpUGreaterThanEqual %bool %225 %227
        %229 = OpLogicalNot %bool %228
               OpSelectionMerge %231 None
               OpBranchConditional %229 %230 %231
        %230 = OpLabel
        %232 = OpAccessChain %_ptr_Function_uint %quad %uint_0
        %233 = OpLoad %uint %232
        %235 = OpUGreaterThanEqual %bool %233 %uint_32768
               OpBranch %231
        %231 = OpLabel
        %236 = OpPhi %bool %228 %203 %235 %230
        %237 = OpLogicalNot %bool %236
               OpSelectionMerge %239 None
               OpBranchConditional %237 %238 %239
        %238 = OpLabel
        %240 = OpAccessChain %_ptr_Function_uint %quad %uint_1
        %241 = OpLoad %uint %240
        %242 = OpUGreaterThanEqual %bool %241 %uint_32768
               OpBranch %239
        %239 = OpLabel
        %243 = OpPhi %bool %236 %231 %242 %238
               OpSelectionMerge %245 None
               OpBranchConditional %243 %244 %245
        %244 = OpLabel
               OpStore %param_0 %uint_4
        %247 = OpFunctionCall %void %fail_u1_ %param_0
               OpDemoteToHelperInvocation
               OpBranch %245
        %245 = OpLabel
        %249 = OpAccessChain %_ptr_Function_uint %quad %uint_0
        %250 = OpLoad %uint %249
        %251 = OpAccessChain %_ptr_Function_uint %quad %uint_1
        %252 = OpLoad %uint %251
        %253 = OpShiftLeftLogical %uint %252 %uint_16
        %254 = OpBitwiseOr %uint %250 %253
               OpStore %xy_0 %254
        %257 = OpLoad %uint %primitive_0
               OpStore %param_1 %257
        %259 = OpLoad %uint %xy_0
               OpStore %param_2 %259
        %260 = OpFunctionCall %uint %hash_key_u1_u1_ %param_1 %param_2
               OpStore %hash %260
               OpStore %record %uint_4294967295
               OpStore %attempt %uint_0
               OpBranch %264
        %264 = OpLabel
               OpLoopMerge %266 %267 None
               OpBranch %268
        %268 = OpLabel
        %269 = OpLoad %uint %attempt
        %270 = OpAccessChain %_ptr_PushConstant_uint %transport %int_4
        %271 = OpLoad %uint %270
        %272 = OpExtInst %uint %1 UMin %271 %uint_128
        %273 = OpULessThan %bool %269 %272
               OpBranchConditional %273 %265 %266
        %265 = OpLabel
        %275 = OpAccessChain %_ptr_PushConstant_uint %transport %int_1
        %276 = OpLoad %uint %275
        %277 = OpIMul %uint %276 %uint_2
        %278 = OpIAdd %uint %uint_16 %277
        %279 = OpLoad %uint %hash
        %280 = OpLoad %uint %attempt
        %281 = OpIAdd %uint %279 %280
        %282 = OpAccessChain %_ptr_PushConstant_uint %transport %int_4
        %283 = OpLoad %uint %282
        %284 = OpISub %uint %283 %uint_1
        %285 = OpBitwiseAnd %uint %281 %284
        %286 = OpIMul %uint %285 %uint_4
        %287 = OpIAdd %uint %278 %286
               OpStore %base_0 %287
        %288 = OpLoad %uint %base_0
        %289 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %288
        %290 = OpLoad %uint %289
        %291 = OpIEqual %bool %290 %uint_0
               OpSelectionMerge %293 None
               OpBranchConditional %291 %292 %293
        %292 = OpLabel
               OpBranch %266
        %293 = OpLabel
        %295 = OpLoad %uint %base_0
        %296 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %295
        %297 = OpLoad %uint %296
        %298 = OpIEqual %bool %297 %uint_2
               OpSelectionMerge %300 None
               OpBranchConditional %298 %299 %300
        %299 = OpLabel
        %301 = OpLoad %uint %base_0
        %302 = OpIAdd %uint %301 %uint_1
        %303 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %302
        %304 = OpLoad %uint %303
        %305 = OpLoad %uint %primitive_0
        %306 = OpIEqual %bool %304 %305
               OpBranch %300
        %300 = OpLabel
        %307 = OpPhi %bool %298 %293 %306 %299
               OpSelectionMerge %309 None
               OpBranchConditional %307 %308 %309
        %308 = OpLabel
        %310 = OpLoad %uint %base_0
        %311 = OpIAdd %uint %310 %uint_2
        %312 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %311
        %313 = OpLoad %uint %312
        %314 = OpLoad %uint %xy_0
        %315 = OpIEqual %bool %313 %314
               OpBranch %309
        %309 = OpLabel
        %316 = OpPhi %bool %307 %300 %315 %308
               OpSelectionMerge %318 None
               OpBranchConditional %316 %317 %318
        %317 = OpLabel
        %319 = OpLoad %uint %base_0
        %320 = OpIAdd %uint %319 %uint_3
        %321 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %320
        %322 = OpLoad %uint %321
               OpStore %record %322
               OpBranch %266
        %318 = OpLabel
               OpBranch %267
        %267 = OpLabel
        %324 = OpLoad %uint %attempt
        %325 = OpIAdd %uint %324 %int_1
               OpStore %attempt %325
               OpBranch %264
        %266 = OpLabel
        %326 = OpLoad %uint %record
        %327 = OpAccessChain %_ptr_PushConstant_uint %transport %int_0
        %328 = OpLoad %uint %327
        %329 = OpUGreaterThanEqual %bool %326 %328
        %330 = OpLogicalNot %bool %329
               OpSelectionMerge %332 None
               OpBranchConditional %330 %331 %332
        %331 = OpLabel
        %333 = OpLoad %uint %record
        %334 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_0
        %335 = OpLoad %uint %334
        %336 = OpUGreaterThanEqual %bool %333 %335
               OpBranch %332
        %332 = OpLabel
        %337 = OpPhi %bool %329 %266 %336 %331
               OpSelectionMerge %339 None
               OpBranchConditional %337 %338 %339
        %338 = OpLabel
               OpStore %param_3 %uint_16
        %341 = OpFunctionCall %void %fail_u1_ %param_3
               OpDemoteToHelperInvocation
               OpBranch %339
        %339 = OpLabel
        %343 = OpAccessChain %_ptr_Function_uint %pixel %uint_1
        %344 = OpLoad %uint %343
        %345 = OpBitwiseAnd %uint %344 %uint_1
        %346 = OpIMul %uint %345 %uint_2
        %347 = OpAccessChain %_ptr_Function_uint %pixel %uint_0
        %348 = OpLoad %uint %347
        %349 = OpBitwiseAnd %uint %348 %uint_1
        %350 = OpIAdd %uint %346 %349
               OpStore %lane %350
        %352 = OpFunctionCall %uint %output_reference_offset_
        %353 = OpLoad %uint %record
        %354 = OpIAdd %uint %352 %353
        %355 = OpAccessChain %_ptr_StorageBuffer_uint %reference_buffer %int_0 %354
        %356 = OpLoad %uint %355
               OpStore %wave_lane %356
        %357 = OpLoad %uint %wave_lane
        %358 = OpAccessChain %_ptr_PushConstant_uint %transport %int_5
        %359 = OpLoad %uint %358
        %360 = OpIMul %uint %359 %uint_64
        %361 = OpUGreaterThanEqual %bool %357 %360
        %362 = OpLogicalNot %bool %361
               OpSelectionMerge %364 None
               OpBranchConditional %362 %363 %364
        %363 = OpLabel
        %365 = OpLoad %uint %wave_lane
        %366 = OpBitwiseAnd %uint %365 %uint_3
        %367 = OpINotEqual %bool %366 %uint_0
               OpBranch %364
        %364 = OpLabel
        %368 = OpPhi %bool %361 %339 %367 %363
        %369 = OpLogicalNot %bool %368
               OpSelectionMerge %371 None
               OpBranchConditional %369 %370 %371
        %370 = OpLabel
        %372 = OpLoad %uint %record
        %373 = OpFunctionCall %uint %quad_words_
        %374 = OpIMul %uint %372 %373
        %375 = OpIAdd %uint %374 %uint_2
        %376 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %375
        %377 = OpLoad %uint %376
        %378 = OpLoad %uint %primitive_0
        %379 = OpINotEqual %bool %377 %378
               OpBranch %371
        %371 = OpLabel
        %380 = OpPhi %bool %368 %364 %379 %370
        %381 = OpLogicalNot %bool %380
               OpSelectionMerge %383 None
               OpBranchConditional %381 %382 %383
        %382 = OpLabel
        %384 = OpLoad %uint %record
        %385 = OpFunctionCall %uint %quad_words_
        %386 = OpIMul %uint %384 %385
        %387 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %386
        %388 = OpLoad %uint %387
        %389 = OpLoad %uint %lane
        %390 = OpShiftLeftLogical %uint %uint_1 %389
        %391 = OpBitwiseAnd %uint %388 %390
        %392 = OpIEqual %bool %391 %uint_0
               OpBranch %383
        %383 = OpLabel
        %393 = OpPhi %bool %380 %371 %392 %382
               OpSelectionMerge %395 None
               OpBranchConditional %393 %394 %395
        %394 = OpLabel
               OpStore %param_4 %uint_32
        %398 = OpFunctionCall %void %fail_u1_ %param_4
               OpDemoteToHelperInvocation
               OpBranch %395
        %395 = OpLabel
        %400 = OpLoad %uint %wave_lane
        %401 = OpLoad %uint %lane
        %402 = OpIAdd %uint %400 %401
        %403 = OpIMul %uint %402 %uint_34
               OpStore %base_1 %403
        %404 = OpLoad %uint %base_1
        %405 = OpAccessChain %_ptr_StorageBuffer_uint %result_buffer %int_0 %404
        %406 = OpLoad %uint %405
        %407 = OpIEqual %bool %406 %uint_0
               OpSelectionMerge %409 None
               OpBranchConditional %407 %408 %409
        %408 = OpLabel
               OpDemoteToHelperInvocation
               OpBranch %409
        %409 = OpLabel
        %410 = OpLoad %uint %base_1
        %411 = OpIAdd %uint %410 %uint_1
        %412 = OpAccessChain %_ptr_StorageBuffer_uint %result_buffer %int_0 %411
        %413 = OpLoad %uint %412
        %415 = OpBitwiseAnd %uint %413 %required_components
        %416 = OpINotEqual %bool %415 %required_components
               OpSelectionMerge %418 None
               OpBranchConditional %416 %417 %418
        %417 = OpLabel
               OpStore %param_5 %uint_256
        %421 = OpFunctionCall %void %fail_u1_ %param_5
               OpDemoteToHelperInvocation
               OpBranch %418
        %418 = OpLabel
        %423 = OpLoad %uint %base_1
               OpStore %param_6 %423
        %424 = OpFunctionCall %void %write_colors_u1_ %param_6
               OpReturn
               OpFunctionEnd
%quad_words_ = OpFunction %uint None %7
          %9 = OpLabel
         %36 = OpAccessChain %_ptr_PushConstant_uint %transport %int_2
         %37 = OpLoad %uint %36
         %38 = OpIMul %uint %uint_4 %37
         %39 = OpIAdd %uint %uint_4 %38
               OpReturnValue %39
               OpFunctionEnd
%wave_table_offset_ = OpFunction %uint None %7
         %11 = OpLabel
         %45 = OpAccessChain %_ptr_PushConstant_uint %transport %int_1
         %46 = OpLoad %uint %45
         %47 = OpIMul %uint %uint_2 %46
         %48 = OpIAdd %uint %uint_16 %47
         %50 = OpAccessChain %_ptr_PushConstant_uint %transport %int_4
         %51 = OpLoad %uint %50
         %52 = OpIMul %uint %uint_4 %51
         %53 = OpIAdd %uint %48 %52
               OpReturnValue %53
               OpFunctionEnd
%active_reference_offset_ = OpFunction %uint None %7
         %13 = OpLabel
         %57 = OpAccessChain %_ptr_PushConstant_uint %transport %int_6
         %58 = OpLoad %uint %57
         %59 = OpIMul %uint %uint_16 %58
               OpReturnValue %59
               OpFunctionEnd
%output_reference_offset_ = OpFunction %uint None %7
         %15 = OpLabel
         %62 = OpFunctionCall %uint %active_reference_offset_
         %64 = OpAccessChain %_ptr_PushConstant_uint %transport %int_5
         %65 = OpLoad %uint %64
         %66 = OpIAdd %uint %62 %65
               OpReturnValue %66
               OpFunctionEnd
%hash_key_u1_u1_ = OpFunction %uint None %17
  %primitive = OpFunctionParameter %_ptr_Function_uint
         %xy = OpFunctionParameter %_ptr_Function_uint
         %21 = OpLabel
          %h = OpVariable %_ptr_Function_uint Function
         %70 = OpLoad %uint %xy
         %72 = OpIMul %uint %70 %uint_2654435769
         %73 = OpLoad %uint %primitive
         %75 = OpIMul %uint %73 %uint_2246822507
         %76 = OpBitwiseXor %uint %72 %75
               OpStore %h %76
         %77 = OpLoad %uint %h
         %78 = OpShiftRightLogical %uint %77 %uint_16
         %79 = OpLoad %uint %h
         %80 = OpBitwiseXor %uint %79 %78
               OpStore %h %80
         %82 = OpLoad %uint %h
         %83 = OpIMul %uint %82 %uint_2146121005
               OpStore %h %83
         %84 = OpLoad %uint %h
         %85 = OpLoad %uint %h
         %87 = OpShiftRightLogical %uint %85 %uint_15
         %88 = OpBitwiseXor %uint %84 %87
               OpReturnValue %88
               OpFunctionEnd
<resolve_writer>
   %fail_u1_ = OpFunction %void None %22
     %reason = OpFunctionParameter %_ptr_Function_uint
         %28 = OpLabel
        %126 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %127 = OpLoad %uint %reason
        %130 = OpAtomicOr %uint %126 %uint_1 %uint_0 %127
               OpReturn
               OpFunctionEnd
)CORE";
