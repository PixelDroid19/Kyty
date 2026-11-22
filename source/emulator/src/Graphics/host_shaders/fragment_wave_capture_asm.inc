// Generated from the public capture kernel. Do not edit by hand.
constexpr const char* kFragmentCaptureReaderSignature = R"CORE(%read_lane_word_u1_ = OpFunction %uint None %17
       %word = OpFunctionParameter %_ptr_Function_uint
)CORE";
constexpr const char* kFragmentCaptureCore = R"CORE(; SPIR-V
; Version: 1.6
; Generator: Google Shaderc over Glslang; 11
; Bound: 510
; Schema: 0
               OpCapability Shader
               OpCapability Geometry
               OpCapability GroupNonUniform
               OpCapability GroupNonUniformQuad
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main" %transport <capture_interface> %control_buffer %record_buffer %reference_buffer %gl_HelperInvocation %gl_SubgroupInvocationID %gl_FragCoord %gl_PrimitiveID
               OpExecutionMode %main OriginUpperLeft
               OpSource GLSL 460
               OpSourceExtension "GL_GOOGLE_cpp_style_line_directive"
               OpSourceExtension "GL_GOOGLE_include_directive"
               OpSourceExtension "GL_KHR_shader_subgroup_basic"
               OpSourceExtension "GL_KHR_shader_subgroup_quad"
               OpName %main "main"
               OpName %quad_words_ "quad_words("
               OpName %wave_table_offset_ "wave_table_offset("
               OpName %active_reference_offset_ "active_reference_offset("
               OpName %output_reference_offset_ "output_reference_offset("
               OpName %read_lane_word_u1_ "read_lane_word(u1;"
               OpName %word "word"
               OpName %hash_key_u1_u1_ "hash_key(u1;u1;"
               OpName %primitive "primitive"
               OpName %xy "xy"
               OpName %reserve_record_ "reserve_record("
               OpName %capture_quad_u1_u1_u1_ "capture_quad(u1;u1;u1;"
               OpName %primitive_0 "primitive"
               OpName %xy_0 "xy"
               OpName %mask "mask"
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
               OpName %record "record"
               OpName %Control "Control"
               OpMemberName %Control 0 "control"
               OpName %control_buffer "control_buffer"
               OpName %hash "hash"
               OpName %param "param"
               OpName %param_0 "param"
               OpName %probe "probe"
               OpName %slot "slot"
               OpName %base "base"
               OpName %record_0 "record"
               OpName %ordinal "ordinal"
               OpName %header "header"
               OpName %Records "Records"
               OpMemberName %Records 0 "records"
               OpName %record_buffer "record_buffer"
               OpName %key "key"
               OpName %wave_hash "wave_hash"
               OpName %param_1 "param"
               OpName %param_2 "param"
               OpName %found "found"
               OpName %attempt "attempt"
               OpName %wave_slot "wave_slot"
               OpName %previous "previous"
               OpName %References "References"
               OpMemberName %References 0 "references"
               OpName %reference_buffer "reference_buffer"
               OpName %covered "covered"
               OpName %gl_HelperInvocation "gl_HelperInvocation"
               OpName %mask_0 "mask"
               OpName %member "member"
               OpName %first "first"
               OpName %lane "lane"
               OpName %gl_SubgroupInvocationID "gl_SubgroupInvocationID"
               OpName %origin "origin"
               OpName %gl_FragCoord "gl_FragCoord"
               OpName %quad "quad"
               OpName %writer "writer"
               OpName %record_1 "record"
               OpName %gl_PrimitiveID "gl_PrimitiveID"
               OpName %param_3 "param"
               OpName %param_4 "param"
               OpName %param_5 "param"
               OpName %word_0 "word"
               OpName %own "own"
               OpName %param_6 "param"
               OpName %member_0 "member"
               OpName %value "value"
               OpDecorate %TransportParameters Block
               OpMemberDecorate %TransportParameters 0 Offset 0
               OpMemberDecorate %TransportParameters 1 Offset 4
               OpMemberDecorate %TransportParameters 2 Offset 8
               OpMemberDecorate %TransportParameters 3 Offset 12
               OpMemberDecorate %TransportParameters 4 Offset 16
               OpMemberDecorate %TransportParameters 5 Offset 20
               OpMemberDecorate %TransportParameters 6 Offset 24
               OpDecorate %_runtimearr_uint ArrayStride 4
               OpDecorate %Control Block
               OpMemberDecorate %Control 0 Offset 0
               OpDecorate %control_buffer Binding 1
               OpDecorate %control_buffer DescriptorSet 0
               OpDecorate %_runtimearr_uint_0 ArrayStride 4
               OpDecorate %Records Block
               OpMemberDecorate %Records 0 Offset 0
               OpDecorate %record_buffer Binding 0
               OpDecorate %record_buffer DescriptorSet 0
               OpDecorate %_runtimearr_uint_1 ArrayStride 4
               OpDecorate %References Block
               OpMemberDecorate %References 0 Offset 0
               OpDecorate %reference_buffer Binding 2
               OpDecorate %reference_buffer DescriptorSet 0
               OpDecorate %gl_HelperInvocation BuiltIn HelperInvocation
               OpDecorate %gl_HelperInvocation Volatile
               OpDecorate %gl_SubgroupInvocationID RelaxedPrecision
               OpDecorate %gl_SubgroupInvocationID BuiltIn SubgroupLocalInvocationId
               OpDecorate %gl_SubgroupInvocationID Flat
               OpDecorate %355 RelaxedPrecision
               OpDecorate %356 RelaxedPrecision
               OpDecorate %gl_FragCoord BuiltIn FragCoord
               OpDecorate %gl_PrimitiveID BuiltIn PrimitiveId
               OpDecorate %gl_PrimitiveID Flat
<capture_annotations>
       %void = OpTypeVoid
          %3 = OpTypeFunction %void
       %uint = OpTypeInt 32 0
          %7 = OpTypeFunction %uint
%_ptr_Function_uint = OpTypePointer Function %uint
         %17 = OpTypeFunction %uint %_ptr_Function_uint
         %21 = OpTypeFunction %uint %_ptr_Function_uint %_ptr_Function_uint
         %28 = OpTypeFunction %uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint
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
      %float = OpTypeFloat 32
    %v4float = OpTypeVector %float 4
%_ptr_Input_v4float = OpTypePointer Input %v4float
     %uint_3 = OpConstant %uint 3
%_ptr_Input_float = OpTypePointer Input %float
%uint_2654435769 = OpConstant %uint 2654435769
%uint_2246822507 = OpConstant %uint 2246822507
%uint_2146121005 = OpConstant %uint 2146121005
    %uint_15 = OpConstant %uint 15
%_runtimearr_uint = OpTypeRuntimeArray %uint
    %Control = OpTypeStruct %_runtimearr_uint
%_ptr_StorageBuffer_Control = OpTypePointer StorageBuffer %Control
%control_buffer = OpVariable %_ptr_StorageBuffer_Control StorageBuffer
      %int_0 = OpConstant %int 0
%_ptr_StorageBuffer_uint = OpTypePointer StorageBuffer %uint
     %uint_1 = OpConstant %uint 1
     %uint_0 = OpConstant %uint 0
       %bool = OpTypeBool
%uint_4294967295 = OpConstant %uint 4294967295
   %uint_128 = OpConstant %uint 128
    %uint_17 = OpConstant %uint 17
%_runtimearr_uint_0 = OpTypeRuntimeArray %uint
    %Records = OpTypeStruct %_runtimearr_uint_0
%_ptr_StorageBuffer_Records = OpTypePointer StorageBuffer %Records
%record_buffer = OpVariable %_ptr_StorageBuffer_Records StorageBuffer
%_ptr_Function_bool = OpTypePointer Function %bool
      %false = OpConstantFalse %bool
%_runtimearr_uint_1 = OpTypeRuntimeArray %uint
 %References = OpTypeStruct %_runtimearr_uint_1
%_ptr_StorageBuffer_References = OpTypePointer StorageBuffer %References
%reference_buffer = OpVariable %_ptr_StorageBuffer_References StorageBuffer
       %true = OpConstantTrue %bool
    %uint_64 = OpConstant %uint 64
%_ptr_Input_bool = OpTypePointer Input %bool
%gl_HelperInvocation = OpVariable %_ptr_Input_bool Input
%_ptr_Input_uint = OpTypePointer Input %uint
%gl_SubgroupInvocationID = OpVariable %_ptr_Input_uint Input
    %v2float = OpTypeVector %float 2
%_ptr_Function_v2float = OpTypePointer Function %v2float
%gl_FragCoord = OpVariable %_ptr_Input_v4float Input
     %v2uint = OpTypeVector %uint 2
%_ptr_Function_v2uint = OpTypePointer Function %v2uint
 %uint_32768 = OpConstant %uint 32768
%_ptr_Input_int = OpTypePointer Input %int
%gl_PrimitiveID = OpVariable %_ptr_Input_int Input
     %uint_8 = OpConstant %uint 8
    %uint_32 = OpConstant %uint 32
<capture_declarations>
       %main = OpFunction %void None %3
          %5 = OpLabel
    %covered = OpVariable %_ptr_Function_uint Function
     %mask_0 = OpVariable %_ptr_Function_uint Function
     %member = OpVariable %_ptr_Function_uint Function
      %first = OpVariable %_ptr_Function_uint Function
        %344 = OpVariable %_ptr_Function_uint Function
       %lane = OpVariable %_ptr_Function_uint Function
     %origin = OpVariable %_ptr_Function_v2float Function
       %quad = OpVariable %_ptr_Function_v2uint Function
     %writer = OpVariable %_ptr_Function_bool Function
   %record_1 = OpVariable %_ptr_Function_uint Function
    %param_3 = OpVariable %_ptr_Function_uint Function
    %param_4 = OpVariable %_ptr_Function_uint Function
    %param_5 = OpVariable %_ptr_Function_uint Function
     %word_0 = OpVariable %_ptr_Function_uint Function
        %own = OpVariable %_ptr_Function_uint Function
    %param_6 = OpVariable %_ptr_Function_uint Function
   %member_0 = OpVariable %_ptr_Function_uint Function
      %value = OpVariable %_ptr_Function_uint Function
        %321 = OpLoad %bool %gl_HelperInvocation
        %322 = OpSelect %uint %321 %uint_0 %uint_1
               OpStore %covered %322
               OpStore %mask_0 %uint_0
               OpStore %member %uint_0
               OpBranch %325
        %325 = OpLabel
               OpLoopMerge %327 %328 None
               OpBranch %329
        %329 = OpLabel
        %330 = OpLoad %uint %member
        %331 = OpULessThan %bool %330 %uint_4
               OpBranchConditional %331 %326 %327
        %326 = OpLabel
        %332 = OpLoad %uint %covered
        %333 = OpLoad %uint %member
        %334 = OpGroupNonUniformQuadBroadcast %uint %uint_3 %332 %333
        %335 = OpLoad %uint %member
        %336 = OpShiftLeftLogical %uint %334 %335
        %337 = OpLoad %uint %mask_0
        %338 = OpBitwiseOr %uint %337 %336
               OpStore %mask_0 %338
               OpBranch %328
        %328 = OpLabel
        %339 = OpLoad %uint %member
        %340 = OpIAdd %uint %339 %int_1
               OpStore %member %340
               OpBranch %325
        %327 = OpLabel
        %342 = OpLoad %uint %mask_0
        %343 = OpIEqual %bool %342 %uint_0
               OpSelectionMerge %346 None
               OpBranchConditional %343 %345 %347
        %345 = OpLabel
               OpStore %344 %uint_0
               OpBranch %346
        %347 = OpLabel
        %348 = OpLoad %uint %mask_0
        %349 = OpExtInst %int %1 FindILsb %348
        %350 = OpBitcast %uint %349
               OpStore %344 %350
               OpBranch %346
        %346 = OpLabel
        %351 = OpLoad %uint %344
               OpStore %first %351
        %355 = OpLoad %uint %gl_SubgroupInvocationID
        %356 = OpBitwiseAnd %uint %355 %uint_3
               OpStore %lane %356
        %361 = OpLoad %v4float %gl_FragCoord
        %362 = OpVectorShuffle %v2float %361 %361 0 1
        %363 = OpGroupNonUniformQuadBroadcast %v2float %uint_3 %362 %uint_0
               OpStore %origin %363
        %367 = OpLoad %v2float %origin
        %368 = OpConvertFToU %v2uint %367
        %369 = OpCompositeConstruct %v2uint %uint_2 %uint_2
        %370 = OpUDiv %v2uint %368 %369
               OpStore %quad %370
        %372 = OpLoad %uint %covered
        %373 = OpINotEqual %bool %372 %uint_0
        %374 = OpLoad %uint %lane
        %375 = OpLoad %uint %first
        %376 = OpIEqual %bool %374 %375
        %377 = OpLogicalAnd %bool %373 %376
               OpStore %writer %377
               OpStore %record_1 %uint_4294967295
        %379 = OpLoad %bool %writer
               OpSelectionMerge %381 None
               OpBranchConditional %379 %380 %381
        %380 = OpLabel
        %382 = OpArrayLength %uint %control_buffer 0
        %383 = OpBitcast %int %382
        %384 = OpBitcast %uint %383
        %385 = OpFunctionCall %uint %wave_table_offset_
        %386 = OpAccessChain %_ptr_PushConstant_uint %transport %int_6
        %387 = OpLoad %uint %386
        %388 = OpIAdd %uint %385 %387
        %389 = OpULessThan %bool %384 %388
        %390 = OpLogicalNot %bool %389
               OpSelectionMerge %392 None
               OpBranchConditional %390 %391 %392
        %391 = OpLabel
        %393 = OpArrayLength %uint %record_buffer 0
        %394 = OpBitcast %int %393
        %395 = OpBitcast %uint %394
        %396 = OpAccessChain %_ptr_PushConstant_uint %transport %int_0
        %397 = OpLoad %uint %396
        %398 = OpFunctionCall %uint %quad_words_
        %399 = OpIMul %uint %397 %398
        %400 = OpULessThan %bool %395 %399
               OpBranch %392
        %392 = OpLabel
        %401 = OpPhi %bool %389 %380 %400 %391
        %402 = OpLogicalNot %bool %401
               OpSelectionMerge %404 None
               OpBranchConditional %402 %403 %404
        %403 = OpLabel
        %405 = OpArrayLength %uint %reference_buffer 0
        %406 = OpBitcast %int %405
        %407 = OpBitcast %uint %406
        %408 = OpFunctionCall %uint %output_reference_offset_
        %409 = OpAccessChain %_ptr_PushConstant_uint %transport %int_0
        %410 = OpLoad %uint %409
        %411 = OpIAdd %uint %408 %410
        %412 = OpULessThan %bool %407 %411
               OpBranch %404
        %404 = OpLabel
        %413 = OpPhi %bool %401 %392 %412 %403
               OpSelectionMerge %415 None
               OpBranchConditional %413 %414 %424
        %414 = OpLabel
        %416 = OpArrayLength %uint %control_buffer 0
        %417 = OpBitcast %int %416
        %418 = OpBitcast %uint %417
        %419 = OpUGreaterThanEqual %bool %418 %uint_16
               OpSelectionMerge %421 None
               OpBranchConditional %419 %420 %421
        %420 = OpLabel
        %422 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %423 = OpAtomicOr %uint %422 %uint_1 %uint_0 %uint_128
               OpBranch %421
        %421 = OpLabel
               OpBranch %415
        %424 = OpLabel
        %425 = OpAccessChain %_ptr_Function_uint %quad %uint_0
        %426 = OpLoad %uint %425
        %428 = OpUGreaterThanEqual %bool %426 %uint_32768
        %429 = OpLogicalNot %bool %428
               OpSelectionMerge %431 None
               OpBranchConditional %429 %430 %431
        %430 = OpLabel
        %432 = OpAccessChain %_ptr_Function_uint %quad %uint_1
        %433 = OpLoad %uint %432
        %434 = OpUGreaterThanEqual %bool %433 %uint_32768
               OpBranch %431
        %431 = OpLabel
        %435 = OpPhi %bool %428 %424 %434 %430
               OpSelectionMerge %437 None
               OpBranchConditional %435 %436 %440
        %436 = OpLabel
        %438 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %439 = OpAtomicOr %uint %438 %uint_1 %uint_0 %uint_4
               OpBranch %437
        %440 = OpLabel
        %443 = OpLoad %int %gl_PrimitiveID
        %444 = OpBitcast %uint %443
        %445 = OpAccessChain %_ptr_Function_uint %quad %uint_0
        %446 = OpLoad %uint %445
        %447 = OpAccessChain %_ptr_Function_uint %quad %uint_1
        %448 = OpLoad %uint %447
        %449 = OpShiftLeftLogical %uint %448 %uint_16
        %450 = OpBitwiseOr %uint %446 %449
               OpStore %param_3 %444
               OpStore %param_4 %450
        %454 = OpLoad %uint %mask_0
               OpStore %param_5 %454
        %455 = OpFunctionCall %uint %capture_quad_u1_u1_u1_ %param_3 %param_4 %param_5
               OpStore %record_1 %455
               OpBranch %437
        %437 = OpLabel
               OpBranch %415
        %415 = OpLabel
               OpBranch %381
        %381 = OpLabel
        %456 = OpLoad %uint %record_1
        %457 = OpLoad %uint %first
        %458 = OpGroupNonUniformQuadBroadcast %uint %uint_3 %456 %457
               OpStore %record_1 %458
               OpStore %word_0 %uint_0
               OpBranch %460
        %460 = OpLabel
               OpLoopMerge %462 %463 None
               OpBranch %464
        %464 = OpLabel
        %465 = OpLoad %uint %word_0
        %466 = OpAccessChain %_ptr_PushConstant_uint %transport %int_2
        %467 = OpLoad %uint %466
        %468 = OpULessThan %bool %465 %467
               OpBranchConditional %468 %461 %462
        %461 = OpLabel
        %471 = OpLoad %uint %word_0
               OpStore %param_6 %471
        %472 = OpFunctionCall %uint %read_lane_word_u1_ %param_6
               OpStore %own %472
               OpStore %member_0 %uint_0
               OpBranch %474
        %474 = OpLabel
               OpLoopMerge %476 %477 None
               OpBranch %478
        %478 = OpLabel
        %479 = OpLoad %uint %member_0
        %480 = OpULessThan %bool %479 %uint_4
               OpBranchConditional %480 %475 %476
        %475 = OpLabel
        %482 = OpLoad %uint %own
        %483 = OpLoad %uint %member_0
        %484 = OpGroupNonUniformQuadBroadcast %uint %uint_3 %482 %483
               OpStore %value %484
        %485 = OpLoad %bool %writer
        %486 = OpLoad %uint %record_1
        %487 = OpINotEqual %bool %486 %uint_4294967295
        %488 = OpLogicalAnd %bool %485 %487
               OpSelectionMerge %490 None
               OpBranchConditional %488 %489 %490
        %489 = OpLabel
        %491 = OpLoad %uint %record_1
        %492 = OpFunctionCall %uint %quad_words_
        %493 = OpIMul %uint %491 %492
        %494 = OpIAdd %uint %493 %uint_4
        %495 = OpLoad %uint %member_0
        %496 = OpAccessChain %_ptr_PushConstant_uint %transport %int_2
        %497 = OpLoad %uint %496
        %498 = OpIMul %uint %495 %497
        %499 = OpIAdd %uint %494 %498
        %500 = OpLoad %uint %word_0
        %501 = OpIAdd %uint %499 %500
        %502 = OpLoad %uint %value
        %503 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %501
               OpStore %503 %502
               OpBranch %490
        %490 = OpLabel
               OpBranch %477
        %477 = OpLabel
        %504 = OpLoad %uint %member_0
        %505 = OpIAdd %uint %504 %int_1
               OpStore %member_0 %505
               OpBranch %474
        %476 = OpLabel
               OpBranch %463
        %463 = OpLabel
        %506 = OpLoad %uint %word_0
        %507 = OpIAdd %uint %506 %int_1
               OpStore %word_0 %507
               OpBranch %460
        %462 = OpLabel
               OpReturn
               OpFunctionEnd
%quad_words_ = OpFunction %uint None %7
          %9 = OpLabel
         %41 = OpAccessChain %_ptr_PushConstant_uint %transport %int_2
         %42 = OpLoad %uint %41
         %43 = OpIMul %uint %uint_4 %42
         %44 = OpIAdd %uint %uint_4 %43
               OpReturnValue %44
               OpFunctionEnd
%wave_table_offset_ = OpFunction %uint None %7
         %11 = OpLabel
         %50 = OpAccessChain %_ptr_PushConstant_uint %transport %int_1
         %51 = OpLoad %uint %50
         %52 = OpIMul %uint %uint_2 %51
         %53 = OpIAdd %uint %uint_16 %52
         %55 = OpAccessChain %_ptr_PushConstant_uint %transport %int_4
         %56 = OpLoad %uint %55
         %57 = OpIMul %uint %uint_4 %56
         %58 = OpIAdd %uint %53 %57
               OpReturnValue %58
               OpFunctionEnd
%active_reference_offset_ = OpFunction %uint None %7
         %13 = OpLabel
         %62 = OpAccessChain %_ptr_PushConstant_uint %transport %int_6
         %63 = OpLoad %uint %62
         %64 = OpIMul %uint %uint_16 %63
               OpReturnValue %64
               OpFunctionEnd
%output_reference_offset_ = OpFunction %uint None %7
         %15 = OpLabel
         %67 = OpFunctionCall %uint %active_reference_offset_
         %69 = OpAccessChain %_ptr_PushConstant_uint %transport %int_5
         %70 = OpLoad %uint %69
         %71 = OpIAdd %uint %67 %70
               OpReturnValue %71
               OpFunctionEnd
<capture_reader>
%hash_key_u1_u1_ = OpFunction %uint None %21
  %primitive = OpFunctionParameter %_ptr_Function_uint
         %xy = OpFunctionParameter %_ptr_Function_uint
         %25 = OpLabel
          %h = OpVariable %_ptr_Function_uint Function
         %88 = OpLoad %uint %xy
         %90 = OpIMul %uint %88 %uint_2654435769
         %91 = OpLoad %uint %primitive
         %93 = OpIMul %uint %91 %uint_2246822507
         %94 = OpBitwiseXor %uint %90 %93
               OpStore %h %94
         %95 = OpLoad %uint %h
         %96 = OpShiftRightLogical %uint %95 %uint_16
         %97 = OpLoad %uint %h
         %98 = OpBitwiseXor %uint %97 %96
               OpStore %h %98
        %100 = OpLoad %uint %h
        %101 = OpIMul %uint %100 %uint_2146121005
               OpStore %h %101
        %102 = OpLoad %uint %h
        %103 = OpLoad %uint %h
        %105 = OpShiftRightLogical %uint %103 %uint_15
        %106 = OpBitwiseXor %uint %102 %105
               OpReturnValue %106
               OpFunctionEnd
%reserve_record_ = OpFunction %uint None %7
         %27 = OpLabel
     %record = OpVariable %_ptr_Function_uint Function
        %116 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_0
        %119 = OpAtomicIAdd %uint %116 %uint_1 %uint_0 %uint_1
               OpStore %record %119
        %120 = OpLoad %uint %record
        %121 = OpAccessChain %_ptr_PushConstant_uint %transport %int_0
        %122 = OpLoad %uint %121
        %124 = OpULessThan %bool %120 %122
               OpSelectionMerge %126 None
               OpBranchConditional %124 %125 %126
        %125 = OpLabel
        %127 = OpLoad %uint %record
               OpReturnValue %127
        %126 = OpLabel
        %129 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %130 = OpAtomicOr %uint %129 %uint_1 %uint_0 %uint_1
               OpReturnValue %uint_4294967295
               OpFunctionEnd
%capture_quad_u1_u1_u1_ = OpFunction %uint None %28
%primitive_0 = OpFunctionParameter %_ptr_Function_uint
       %xy_0 = OpFunctionParameter %_ptr_Function_uint
       %mask = OpFunctionParameter %_ptr_Function_uint
         %33 = OpLabel
       %hash = OpVariable %_ptr_Function_uint Function
      %param = OpVariable %_ptr_Function_uint Function
    %param_0 = OpVariable %_ptr_Function_uint Function
      %probe = OpVariable %_ptr_Function_uint Function
       %slot = OpVariable %_ptr_Function_uint Function
       %base = OpVariable %_ptr_Function_uint Function
   %record_0 = OpVariable %_ptr_Function_uint Function
    %ordinal = OpVariable %_ptr_Function_uint Function
     %header = OpVariable %_ptr_Function_uint Function
        %key = OpVariable %_ptr_Function_uint Function
  %wave_hash = OpVariable %_ptr_Function_uint Function
    %param_1 = OpVariable %_ptr_Function_uint Function
    %param_2 = OpVariable %_ptr_Function_uint Function
      %found = OpVariable %_ptr_Function_bool Function
    %attempt = OpVariable %_ptr_Function_uint Function
  %wave_slot = OpVariable %_ptr_Function_uint Function
   %previous = OpVariable %_ptr_Function_uint Function
        %134 = OpLoad %uint %primitive_0
        %135 = OpAccessChain %_ptr_PushConstant_uint %transport %int_1
        %136 = OpLoad %uint %135
        %137 = OpUGreaterThanEqual %bool %134 %136
               OpSelectionMerge %139 None
               OpBranchConditional %137 %138 %139
        %138 = OpLabel
        %140 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %141 = OpAtomicOr %uint %140 %uint_1 %uint_0 %uint_4
               OpReturnValue %uint_4294967295
        %139 = OpLabel
        %145 = OpLoad %uint %primitive_0
               OpStore %param %145
        %147 = OpLoad %uint %xy_0
               OpStore %param_0 %147
        %148 = OpFunctionCall %uint %hash_key_u1_u1_ %param %param_0
               OpStore %hash %148
               OpStore %probe %uint_0
               OpBranch %150
        %150 = OpLabel
               OpLoopMerge %152 %153 None
               OpBranch %154
        %154 = OpLabel
        %155 = OpLoad %uint %probe
        %156 = OpAccessChain %_ptr_PushConstant_uint %transport %int_4
        %157 = OpLoad %uint %156
        %159 = OpExtInst %uint %1 UMin %157 %uint_128
        %160 = OpULessThan %bool %155 %159
               OpBranchConditional %160 %151 %152
        %151 = OpLabel
        %162 = OpLoad %uint %hash
        %163 = OpLoad %uint %probe
        %164 = OpIAdd %uint %162 %163
        %165 = OpAccessChain %_ptr_PushConstant_uint %transport %int_4
        %166 = OpLoad %uint %165
        %167 = OpISub %uint %166 %uint_1
        %168 = OpBitwiseAnd %uint %164 %167
               OpStore %slot %168
        %170 = OpAccessChain %_ptr_PushConstant_uint %transport %int_1
        %171 = OpLoad %uint %170
        %172 = OpIMul %uint %171 %uint_2
        %173 = OpIAdd %uint %uint_16 %172
        %174 = OpLoad %uint %slot
        %175 = OpIMul %uint %174 %uint_4
        %176 = OpIAdd %uint %173 %175
               OpStore %base %176
        %177 = OpLoad %uint %base
        %178 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %177
        %179 = OpAtomicCompareExchange %uint %178 %uint_1 %uint_0 %uint_0 %uint_1 %uint_0
        %180 = OpINotEqual %bool %179 %uint_0
               OpSelectionMerge %182 None
               OpBranchConditional %180 %181 %182
        %181 = OpLabel
               OpBranch %153
        %182 = OpLabel
        %185 = OpFunctionCall %uint %reserve_record_
               OpStore %record_0 %185
        %186 = OpLoad %uint %record_0
        %187 = OpIEqual %bool %186 %uint_4294967295
               OpSelectionMerge %189 None
               OpBranchConditional %187 %188 %189
        %188 = OpLabel
        %190 = OpLoad %uint %record_0
               OpReturnValue %190
        %189 = OpLabel
        %194 = OpLoad %uint %primitive_0
        %195 = OpIMul %uint %194 %uint_2
        %196 = OpIAdd %uint %uint_17 %195
        %197 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %196
        %198 = OpAtomicIAdd %uint %197 %uint_1 %uint_0 %uint_1
               OpStore %ordinal %198
        %200 = OpLoad %uint %record_0
        %201 = OpFunctionCall %uint %quad_words_
        %202 = OpIMul %uint %200 %201
               OpStore %header %202
        %207 = OpLoad %uint %header
        %208 = OpLoad %uint %mask
        %209 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %207
               OpStore %209 %208
        %210 = OpLoad %uint %header
        %211 = OpIAdd %uint %210 %uint_1
        %212 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %211
               OpStore %212 %uint_1
        %213 = OpLoad %uint %header
        %214 = OpIAdd %uint %213 %uint_2
        %215 = OpLoad %uint %primitive_0
        %216 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %214
               OpStore %216 %215
        %217 = OpLoad %uint %header
        %218 = OpIAdd %uint %217 %uint_3
        %219 = OpLoad %uint %ordinal
        %220 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %218
               OpStore %220 %219
        %222 = OpLoad %uint %primitive_0
        %223 = OpShiftLeftLogical %uint %222 %uint_16
        %224 = OpLoad %uint %ordinal
        %225 = OpUDiv %uint %224 %uint_16
        %226 = OpBitwiseOr %uint %223 %225
        %227 = OpIAdd %uint %226 %uint_1
               OpStore %key %227
        %229 = OpLoad %uint %ordinal
        %230 = OpUDiv %uint %229 %uint_16
        %232 = OpLoad %uint %primitive_0
               OpStore %param_1 %232
               OpStore %param_2 %230
        %234 = OpFunctionCall %uint %hash_key_u1_u1_ %param_1 %param_2
               OpStore %wave_hash %234
               OpStore %found %false
               OpStore %attempt %uint_0
               OpBranch %239
        %239 = OpLabel
               OpLoopMerge %241 %242 None
               OpBranch %243
        %243 = OpLabel
        %244 = OpLoad %uint %attempt
        %245 = OpAccessChain %_ptr_PushConstant_uint %transport %int_6
        %246 = OpLoad %uint %245
        %247 = OpExtInst %uint %1 UMin %246 %uint_128
        %248 = OpULessThan %bool %244 %247
               OpBranchConditional %248 %240 %241
        %240 = OpLabel
        %250 = OpLoad %uint %wave_hash
        %251 = OpLoad %uint %attempt
        %252 = OpIAdd %uint %250 %251
        %253 = OpAccessChain %_ptr_PushConstant_uint %transport %int_6
        %254 = OpLoad %uint %253
        %255 = OpISub %uint %254 %uint_1
        %256 = OpBitwiseAnd %uint %252 %255
               OpStore %wave_slot %256
        %258 = OpFunctionCall %uint %wave_table_offset_
        %259 = OpLoad %uint %wave_slot
        %260 = OpIAdd %uint %258 %259
        %261 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %260
        %262 = OpLoad %uint %key
        %263 = OpAtomicCompareExchange %uint %261 %uint_1 %uint_0 %uint_0 %262 %uint_0
               OpStore %previous %263
        %264 = OpLoad %uint %previous
        %265 = OpINotEqual %bool %264 %uint_0
        %266 = OpLoad %uint %previous
        %267 = OpLoad %uint %key
        %268 = OpINotEqual %bool %266 %267
        %269 = OpLogicalAnd %bool %265 %268
               OpSelectionMerge %271 None
               OpBranchConditional %269 %270 %271
        %270 = OpLabel
               OpBranch %242
        %271 = OpLabel
        %277 = OpLoad %uint %wave_slot
        %278 = OpIMul %uint %277 %uint_16
        %279 = OpLoad %uint %ordinal
        %280 = OpBitwiseAnd %uint %279 %uint_15
        %281 = OpIAdd %uint %278 %280
        %282 = OpLoad %uint %record_0
        %283 = OpAccessChain %_ptr_StorageBuffer_uint %reference_buffer %int_0 %281
               OpStore %283 %282
               OpStore %found %true
               OpBranch %241
        %242 = OpLabel
        %286 = OpLoad %uint %attempt
        %287 = OpIAdd %uint %286 %int_1
               OpStore %attempt %287
               OpBranch %239
        %241 = OpLabel
        %288 = OpLoad %bool %found
        %289 = OpLogicalNot %bool %288
               OpSelectionMerge %291 None
               OpBranchConditional %289 %290 %291
        %290 = OpLabel
        %292 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %294 = OpAtomicOr %uint %292 %uint_1 %uint_0 %uint_64
               OpBranch %291
        %291 = OpLabel
        %295 = OpLoad %uint %base
        %296 = OpIAdd %uint %295 %uint_1
        %297 = OpLoad %uint %primitive_0
        %298 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %296
               OpStore %298 %297
        %299 = OpLoad %uint %base
        %300 = OpIAdd %uint %299 %uint_2
        %301 = OpLoad %uint %xy_0
        %302 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %300
               OpStore %302 %301
        %303 = OpLoad %uint %base
        %304 = OpIAdd %uint %303 %uint_3
        %305 = OpLoad %uint %record_0
        %306 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %304
               OpStore %306 %305
        %307 = OpLoad %uint %base
        %308 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %307
        %309 = OpAtomicExchange %uint %308 %uint_1 %uint_0 %uint_2
        %310 = OpLoad %uint %record_0
               OpReturnValue %310
        %153 = OpLabel
        %312 = OpLoad %uint %probe
        %313 = OpIAdd %uint %312 %int_1
               OpStore %probe %313
               OpBranch %150
        %152 = OpLabel
        %314 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %315 = OpAtomicOr %uint %314 %uint_1 %uint_0 %uint_2
               OpReturnValue %uint_4294967295
               OpFunctionEnd
)CORE";
