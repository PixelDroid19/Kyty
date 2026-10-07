// Generated from the public capture kernel. Do not edit by hand.
constexpr const char* kFragmentCaptureReaderSignature = R"CORE(%read_lane_word_u1_ = OpFunction %uint None %17
       %word = OpFunctionParameter %_ptr_Function_uint
)CORE";
constexpr const char* kFragmentCaptureCore = R"CORE(; SPIR-V
; Version: 1.6
; Generator: Google Shaderc over Glslang; 11
; Bound: 525
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
               OpName %current "current"
               OpName %Control "Control"
               OpMemberName %Control 0 "control"
               OpName %control_buffer "control_buffer"
               OpName %observed "observed"
               OpName %hash "hash"
               OpName %param "param"
               OpName %param_0 "param"
               OpName %probe "probe"
               OpName %slot "slot"
               OpName %base "base"
               OpName %record "record"
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
               OpName %record_0 "record"
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
               OpDecorate %370 RelaxedPrecision
               OpDecorate %371 RelaxedPrecision
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
     %uint_0 = OpConstant %uint 0
     %uint_1 = OpConstant %uint 1
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
        %359 = OpVariable %_ptr_Function_uint Function
       %lane = OpVariable %_ptr_Function_uint Function
     %origin = OpVariable %_ptr_Function_v2float Function
       %quad = OpVariable %_ptr_Function_v2uint Function
     %writer = OpVariable %_ptr_Function_bool Function
   %record_0 = OpVariable %_ptr_Function_uint Function
    %param_3 = OpVariable %_ptr_Function_uint Function
    %param_4 = OpVariable %_ptr_Function_uint Function
    %param_5 = OpVariable %_ptr_Function_uint Function
     %word_0 = OpVariable %_ptr_Function_uint Function
        %own = OpVariable %_ptr_Function_uint Function
    %param_6 = OpVariable %_ptr_Function_uint Function
   %member_0 = OpVariable %_ptr_Function_uint Function
      %value = OpVariable %_ptr_Function_uint Function
        %336 = OpLoad %bool %gl_HelperInvocation
        %337 = OpSelect %uint %336 %uint_0 %uint_1
               OpStore %covered %337
               OpStore %mask_0 %uint_0
               OpStore %member %uint_0
               OpBranch %340
        %340 = OpLabel
               OpLoopMerge %342 %343 None
               OpBranch %344
        %344 = OpLabel
        %345 = OpLoad %uint %member
        %346 = OpULessThan %bool %345 %uint_4
               OpBranchConditional %346 %341 %342
        %341 = OpLabel
        %347 = OpLoad %uint %covered
        %348 = OpLoad %uint %member
        %349 = OpGroupNonUniformQuadBroadcast %uint %uint_3 %347 %348
        %350 = OpLoad %uint %member
        %351 = OpShiftLeftLogical %uint %349 %350
        %352 = OpLoad %uint %mask_0
        %353 = OpBitwiseOr %uint %352 %351
               OpStore %mask_0 %353
               OpBranch %343
        %343 = OpLabel
        %354 = OpLoad %uint %member
        %355 = OpIAdd %uint %354 %int_1
               OpStore %member %355
               OpBranch %340
        %342 = OpLabel
        %357 = OpLoad %uint %mask_0
        %358 = OpIEqual %bool %357 %uint_0
               OpSelectionMerge %361 None
               OpBranchConditional %358 %360 %362
        %360 = OpLabel
               OpStore %359 %uint_0
               OpBranch %361
        %362 = OpLabel
        %363 = OpLoad %uint %mask_0
        %364 = OpExtInst %int %1 FindILsb %363
        %365 = OpBitcast %uint %364
               OpStore %359 %365
               OpBranch %361
        %361 = OpLabel
        %366 = OpLoad %uint %359
               OpStore %first %366
        %370 = OpLoad %uint %gl_SubgroupInvocationID
        %371 = OpBitwiseAnd %uint %370 %uint_3
               OpStore %lane %371
        %376 = OpLoad %v4float %gl_FragCoord
        %377 = OpVectorShuffle %v2float %376 %376 0 1
        %378 = OpGroupNonUniformQuadBroadcast %v2float %uint_3 %377 %uint_0
               OpStore %origin %378
        %382 = OpLoad %v2float %origin
        %383 = OpConvertFToU %v2uint %382
        %384 = OpCompositeConstruct %v2uint %uint_2 %uint_2
        %385 = OpUDiv %v2uint %383 %384
               OpStore %quad %385
        %387 = OpLoad %uint %covered
        %388 = OpINotEqual %bool %387 %uint_0
        %389 = OpLoad %uint %lane
        %390 = OpLoad %uint %first
        %391 = OpIEqual %bool %389 %390
        %392 = OpLogicalAnd %bool %388 %391
               OpStore %writer %392
               OpStore %record_0 %uint_4294967295
        %394 = OpLoad %bool %writer
               OpSelectionMerge %396 None
               OpBranchConditional %394 %395 %396
        %395 = OpLabel
        %397 = OpArrayLength %uint %control_buffer 0
        %398 = OpBitcast %int %397
        %399 = OpBitcast %uint %398
        %400 = OpFunctionCall %uint %wave_table_offset_
        %401 = OpAccessChain %_ptr_PushConstant_uint %transport %int_6
        %402 = OpLoad %uint %401
        %403 = OpIAdd %uint %400 %402
        %404 = OpULessThan %bool %399 %403
        %405 = OpLogicalNot %bool %404
               OpSelectionMerge %407 None
               OpBranchConditional %405 %406 %407
        %406 = OpLabel
        %408 = OpArrayLength %uint %record_buffer 0
        %409 = OpBitcast %int %408
        %410 = OpBitcast %uint %409
        %411 = OpAccessChain %_ptr_PushConstant_uint %transport %int_0
        %412 = OpLoad %uint %411
        %413 = OpFunctionCall %uint %quad_words_
        %414 = OpIMul %uint %412 %413
        %415 = OpULessThan %bool %410 %414
               OpBranch %407
        %407 = OpLabel
        %416 = OpPhi %bool %404 %395 %415 %406
        %417 = OpLogicalNot %bool %416
               OpSelectionMerge %419 None
               OpBranchConditional %417 %418 %419
        %418 = OpLabel
        %420 = OpArrayLength %uint %reference_buffer 0
        %421 = OpBitcast %int %420
        %422 = OpBitcast %uint %421
        %423 = OpFunctionCall %uint %output_reference_offset_
        %424 = OpAccessChain %_ptr_PushConstant_uint %transport %int_0
        %425 = OpLoad %uint %424
        %426 = OpIAdd %uint %423 %425
        %427 = OpULessThan %bool %422 %426
               OpBranch %419
        %419 = OpLabel
        %428 = OpPhi %bool %416 %407 %427 %418
               OpSelectionMerge %430 None
               OpBranchConditional %428 %429 %439
        %429 = OpLabel
        %431 = OpArrayLength %uint %control_buffer 0
        %432 = OpBitcast %int %431
        %433 = OpBitcast %uint %432
        %434 = OpUGreaterThanEqual %bool %433 %uint_16
               OpSelectionMerge %436 None
               OpBranchConditional %434 %435 %436
        %435 = OpLabel
        %437 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %438 = OpAtomicOr %uint %437 %uint_1 %uint_0 %uint_128
               OpBranch %436
        %436 = OpLabel
               OpBranch %430
        %439 = OpLabel
        %440 = OpAccessChain %_ptr_Function_uint %quad %uint_0
        %441 = OpLoad %uint %440
        %443 = OpUGreaterThanEqual %bool %441 %uint_32768
        %444 = OpLogicalNot %bool %443
               OpSelectionMerge %446 None
               OpBranchConditional %444 %445 %446
        %445 = OpLabel
        %447 = OpAccessChain %_ptr_Function_uint %quad %uint_1
        %448 = OpLoad %uint %447
        %449 = OpUGreaterThanEqual %bool %448 %uint_32768
               OpBranch %446
        %446 = OpLabel
        %450 = OpPhi %bool %443 %439 %449 %445
               OpSelectionMerge %452 None
               OpBranchConditional %450 %451 %455
        %451 = OpLabel
        %453 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %454 = OpAtomicOr %uint %453 %uint_1 %uint_0 %uint_4
               OpBranch %452
        %455 = OpLabel
        %458 = OpLoad %int %gl_PrimitiveID
        %459 = OpBitcast %uint %458
        %460 = OpAccessChain %_ptr_Function_uint %quad %uint_0
        %461 = OpLoad %uint %460
        %462 = OpAccessChain %_ptr_Function_uint %quad %uint_1
        %463 = OpLoad %uint %462
        %464 = OpShiftLeftLogical %uint %463 %uint_16
        %465 = OpBitwiseOr %uint %461 %464
               OpStore %param_3 %459
               OpStore %param_4 %465
        %469 = OpLoad %uint %mask_0
               OpStore %param_5 %469
        %470 = OpFunctionCall %uint %capture_quad_u1_u1_u1_ %param_3 %param_4 %param_5
               OpStore %record_0 %470
               OpBranch %452
        %452 = OpLabel
               OpBranch %430
        %430 = OpLabel
               OpBranch %396
        %396 = OpLabel
        %471 = OpLoad %uint %record_0
        %472 = OpLoad %uint %first
        %473 = OpGroupNonUniformQuadBroadcast %uint %uint_3 %471 %472
               OpStore %record_0 %473
               OpStore %word_0 %uint_0
               OpBranch %475
        %475 = OpLabel
               OpLoopMerge %477 %478 None
               OpBranch %479
        %479 = OpLabel
        %480 = OpLoad %uint %word_0
        %481 = OpAccessChain %_ptr_PushConstant_uint %transport %int_2
        %482 = OpLoad %uint %481
        %483 = OpULessThan %bool %480 %482
               OpBranchConditional %483 %476 %477
        %476 = OpLabel
        %486 = OpLoad %uint %word_0
               OpStore %param_6 %486
        %487 = OpFunctionCall %uint %read_lane_word_u1_ %param_6
               OpStore %own %487
               OpStore %member_0 %uint_0
               OpBranch %489
        %489 = OpLabel
               OpLoopMerge %491 %492 None
               OpBranch %493
        %493 = OpLabel
        %494 = OpLoad %uint %member_0
        %495 = OpULessThan %bool %494 %uint_4
               OpBranchConditional %495 %490 %491
        %490 = OpLabel
        %497 = OpLoad %uint %own
        %498 = OpLoad %uint %member_0
        %499 = OpGroupNonUniformQuadBroadcast %uint %uint_3 %497 %498
               OpStore %value %499
        %500 = OpLoad %bool %writer
        %501 = OpLoad %uint %record_0
        %502 = OpINotEqual %bool %501 %uint_4294967295
        %503 = OpLogicalAnd %bool %500 %502
               OpSelectionMerge %505 None
               OpBranchConditional %503 %504 %505
        %504 = OpLabel
        %506 = OpLoad %uint %record_0
        %507 = OpFunctionCall %uint %quad_words_
        %508 = OpIMul %uint %506 %507
        %509 = OpIAdd %uint %508 %uint_4
        %510 = OpLoad %uint %member_0
        %511 = OpAccessChain %_ptr_PushConstant_uint %transport %int_2
        %512 = OpLoad %uint %511
        %513 = OpIMul %uint %510 %512
        %514 = OpIAdd %uint %509 %513
        %515 = OpLoad %uint %word_0
        %516 = OpIAdd %uint %514 %515
        %517 = OpLoad %uint %value
        %518 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %516
               OpStore %518 %517
               OpBranch %505
        %505 = OpLabel
               OpBranch %492
        %492 = OpLabel
        %519 = OpLoad %uint %member_0
        %520 = OpIAdd %uint %519 %int_1
               OpStore %member_0 %520
               OpBranch %489
        %491 = OpLabel
               OpBranch %478
        %478 = OpLabel
        %521 = OpLoad %uint %word_0
        %522 = OpIAdd %uint %521 %int_1
               OpStore %word_0 %522
               OpBranch %475
        %477 = OpLabel
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
    %current = OpVariable %_ptr_Function_uint Function
   %observed = OpVariable %_ptr_Function_uint Function
        %116 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_0
        %119 = OpAtomicIAdd %uint %116 %uint_1 %uint_0 %uint_0
               OpStore %current %119
               OpBranch %120
        %120 = OpLabel
               OpLoopMerge %122 %123 None
               OpBranch %124
        %124 = OpLabel
        %125 = OpLoad %uint %current
        %126 = OpAccessChain %_ptr_PushConstant_uint %transport %int_0
        %127 = OpLoad %uint %126
        %129 = OpULessThan %bool %125 %127
               OpBranchConditional %129 %121 %122
        %121 = OpLabel
        %131 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_0
        %132 = OpLoad %uint %current
        %133 = OpLoad %uint %current
        %134 = OpIAdd %uint %133 %uint_1
        %135 = OpAtomicCompareExchange %uint %131 %uint_1 %uint_0 %uint_0 %134 %132
               OpStore %observed %135
        %136 = OpLoad %uint %observed
        %137 = OpLoad %uint %current
        %138 = OpIEqual %bool %136 %137
               OpSelectionMerge %140 None
               OpBranchConditional %138 %139 %140
        %139 = OpLabel
        %141 = OpLoad %uint %current
               OpReturnValue %141
        %140 = OpLabel
        %143 = OpLoad %uint %observed
               OpStore %current %143
               OpBranch %123
        %123 = OpLabel
               OpBranch %120
        %122 = OpLabel
        %144 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %145 = OpAtomicOr %uint %144 %uint_1 %uint_0 %uint_1
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
     %record = OpVariable %_ptr_Function_uint Function
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
        %149 = OpLoad %uint %primitive_0
        %150 = OpAccessChain %_ptr_PushConstant_uint %transport %int_1
        %151 = OpLoad %uint %150
        %152 = OpUGreaterThanEqual %bool %149 %151
               OpSelectionMerge %154 None
               OpBranchConditional %152 %153 %154
        %153 = OpLabel
        %155 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %156 = OpAtomicOr %uint %155 %uint_1 %uint_0 %uint_4
               OpReturnValue %uint_4294967295
        %154 = OpLabel
        %160 = OpLoad %uint %primitive_0
               OpStore %param %160
        %162 = OpLoad %uint %xy_0
               OpStore %param_0 %162
        %163 = OpFunctionCall %uint %hash_key_u1_u1_ %param %param_0
               OpStore %hash %163
               OpStore %probe %uint_0
               OpBranch %165
        %165 = OpLabel
               OpLoopMerge %167 %168 None
               OpBranch %169
        %169 = OpLabel
        %170 = OpLoad %uint %probe
        %171 = OpAccessChain %_ptr_PushConstant_uint %transport %int_4
        %172 = OpLoad %uint %171
        %174 = OpExtInst %uint %1 UMin %172 %uint_128
        %175 = OpULessThan %bool %170 %174
               OpBranchConditional %175 %166 %167
        %166 = OpLabel
        %177 = OpLoad %uint %hash
        %178 = OpLoad %uint %probe
        %179 = OpIAdd %uint %177 %178
        %180 = OpAccessChain %_ptr_PushConstant_uint %transport %int_4
        %181 = OpLoad %uint %180
        %182 = OpISub %uint %181 %uint_1
        %183 = OpBitwiseAnd %uint %179 %182
               OpStore %slot %183
        %185 = OpAccessChain %_ptr_PushConstant_uint %transport %int_1
        %186 = OpLoad %uint %185
        %187 = OpIMul %uint %186 %uint_2
        %188 = OpIAdd %uint %uint_16 %187
        %189 = OpLoad %uint %slot
        %190 = OpIMul %uint %189 %uint_4
        %191 = OpIAdd %uint %188 %190
               OpStore %base %191
        %192 = OpLoad %uint %base
        %193 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %192
        %194 = OpAtomicCompareExchange %uint %193 %uint_1 %uint_0 %uint_0 %uint_1 %uint_0
        %195 = OpINotEqual %bool %194 %uint_0
               OpSelectionMerge %197 None
               OpBranchConditional %195 %196 %197
        %196 = OpLabel
               OpBranch %168
        %197 = OpLabel
        %200 = OpFunctionCall %uint %reserve_record_
               OpStore %record %200
        %201 = OpLoad %uint %record
        %202 = OpIEqual %bool %201 %uint_4294967295
               OpSelectionMerge %204 None
               OpBranchConditional %202 %203 %204
        %203 = OpLabel
        %205 = OpLoad %uint %record
               OpReturnValue %205
        %204 = OpLabel
        %209 = OpLoad %uint %primitive_0
        %210 = OpIMul %uint %209 %uint_2
        %211 = OpIAdd %uint %uint_17 %210
        %212 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %211
        %213 = OpAtomicIAdd %uint %212 %uint_1 %uint_0 %uint_1
               OpStore %ordinal %213
        %215 = OpLoad %uint %record
        %216 = OpFunctionCall %uint %quad_words_
        %217 = OpIMul %uint %215 %216
               OpStore %header %217
        %222 = OpLoad %uint %header
        %223 = OpLoad %uint %mask
        %224 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %222
               OpStore %224 %223
        %225 = OpLoad %uint %header
        %226 = OpIAdd %uint %225 %uint_1
        %227 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %226
               OpStore %227 %uint_1
        %228 = OpLoad %uint %header
        %229 = OpIAdd %uint %228 %uint_2
        %230 = OpLoad %uint %primitive_0
        %231 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %229
               OpStore %231 %230
        %232 = OpLoad %uint %header
        %233 = OpIAdd %uint %232 %uint_3
        %234 = OpLoad %uint %ordinal
        %235 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %233
               OpStore %235 %234
        %237 = OpLoad %uint %primitive_0
        %238 = OpShiftLeftLogical %uint %237 %uint_16
        %239 = OpLoad %uint %ordinal
        %240 = OpUDiv %uint %239 %uint_16
        %241 = OpBitwiseOr %uint %238 %240
        %242 = OpIAdd %uint %241 %uint_1
               OpStore %key %242
        %244 = OpLoad %uint %ordinal
        %245 = OpUDiv %uint %244 %uint_16
        %247 = OpLoad %uint %primitive_0
               OpStore %param_1 %247
               OpStore %param_2 %245
        %249 = OpFunctionCall %uint %hash_key_u1_u1_ %param_1 %param_2
               OpStore %wave_hash %249
               OpStore %found %false
               OpStore %attempt %uint_0
               OpBranch %254
        %254 = OpLabel
               OpLoopMerge %256 %257 None
               OpBranch %258
        %258 = OpLabel
        %259 = OpLoad %uint %attempt
        %260 = OpAccessChain %_ptr_PushConstant_uint %transport %int_6
        %261 = OpLoad %uint %260
        %262 = OpExtInst %uint %1 UMin %261 %uint_128
        %263 = OpULessThan %bool %259 %262
               OpBranchConditional %263 %255 %256
        %255 = OpLabel
        %265 = OpLoad %uint %wave_hash
        %266 = OpLoad %uint %attempt
        %267 = OpIAdd %uint %265 %266
        %268 = OpAccessChain %_ptr_PushConstant_uint %transport %int_6
        %269 = OpLoad %uint %268
        %270 = OpISub %uint %269 %uint_1
        %271 = OpBitwiseAnd %uint %267 %270
               OpStore %wave_slot %271
        %273 = OpFunctionCall %uint %wave_table_offset_
        %274 = OpLoad %uint %wave_slot
        %275 = OpIAdd %uint %273 %274
        %276 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %275
        %277 = OpLoad %uint %key
        %278 = OpAtomicCompareExchange %uint %276 %uint_1 %uint_0 %uint_0 %277 %uint_0
               OpStore %previous %278
        %279 = OpLoad %uint %previous
        %280 = OpINotEqual %bool %279 %uint_0
        %281 = OpLoad %uint %previous
        %282 = OpLoad %uint %key
        %283 = OpINotEqual %bool %281 %282
        %284 = OpLogicalAnd %bool %280 %283
               OpSelectionMerge %286 None
               OpBranchConditional %284 %285 %286
        %285 = OpLabel
               OpBranch %257
        %286 = OpLabel
        %292 = OpLoad %uint %wave_slot
        %293 = OpIMul %uint %292 %uint_16
        %294 = OpLoad %uint %ordinal
        %295 = OpBitwiseAnd %uint %294 %uint_15
        %296 = OpIAdd %uint %293 %295
        %297 = OpLoad %uint %record
        %298 = OpAccessChain %_ptr_StorageBuffer_uint %reference_buffer %int_0 %296
               OpStore %298 %297
               OpStore %found %true
               OpBranch %256
        %257 = OpLabel
        %301 = OpLoad %uint %attempt
        %302 = OpIAdd %uint %301 %int_1
               OpStore %attempt %302
               OpBranch %254
        %256 = OpLabel
        %303 = OpLoad %bool %found
        %304 = OpLogicalNot %bool %303
               OpSelectionMerge %306 None
               OpBranchConditional %304 %305 %306
        %305 = OpLabel
        %307 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %309 = OpAtomicOr %uint %307 %uint_1 %uint_0 %uint_64
               OpBranch %306
        %306 = OpLabel
        %310 = OpLoad %uint %base
        %311 = OpIAdd %uint %310 %uint_1
        %312 = OpLoad %uint %primitive_0
        %313 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %311
               OpStore %313 %312
        %314 = OpLoad %uint %base
        %315 = OpIAdd %uint %314 %uint_2
        %316 = OpLoad %uint %xy_0
        %317 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %315
               OpStore %317 %316
        %318 = OpLoad %uint %base
        %319 = OpIAdd %uint %318 %uint_3
        %320 = OpLoad %uint %record
        %321 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %319
               OpStore %321 %320
        %322 = OpLoad %uint %base
        %323 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %322
        %324 = OpAtomicExchange %uint %323 %uint_1 %uint_0 %uint_2
        %325 = OpLoad %uint %record
               OpReturnValue %325
        %168 = OpLabel
        %327 = OpLoad %uint %probe
        %328 = OpIAdd %uint %327 %int_1
               OpStore %probe %328
               OpBranch %165
        %167 = OpLabel
        %329 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %330 = OpAtomicOr %uint %329 %uint_1 %uint_0 %uint_2
               OpReturnValue %uint_4294967295
               OpFunctionEnd
)CORE";
