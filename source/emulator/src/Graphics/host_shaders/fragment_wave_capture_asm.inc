// Generated from the public capture kernel. Do not edit by hand.
constexpr const char* kFragmentCaptureReaderSignature = R"CORE(%read_lane_word_u1_ = OpFunction %uint None %17
       %word = OpFunctionParameter %_ptr_Function_uint
)CORE";
constexpr const char* kFragmentCaptureCore = R"CORE(; SPIR-V
; Version: 1.6
; Generator: Google Shaderc over Glslang; 11
; Bound: 573
; Schema: 0
               OpCapability Shader
               OpCapability Geometry
               OpCapability GroupNonUniform
               OpCapability GroupNonUniformArithmetic
               OpCapability GroupNonUniformQuad
          %1 = OpExtInstImport "GLSL.std.450"
               OpMemoryModel Logical GLSL450
               OpEntryPoint Fragment %main "main" %transport <capture_interface> %control_buffer %gl_SubgroupInvocationID %record_buffer %reference_buffer %gl_HelperInvocation %gl_FragCoord %gl_PrimitiveID
               OpExecutionMode %main OriginUpperLeft
               OpSource GLSL 460
               OpSourceExtension "GL_GOOGLE_cpp_style_line_directive"
               OpSourceExtension "GL_GOOGLE_include_directive"
               OpSourceExtension "GL_KHR_shader_subgroup_arithmetic"
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
               OpName %claim_slot_u1_u1_ "claim_slot(u1;u1;"
               OpName %primitive_0 "primitive"
               OpName %xy_0 "xy"
               OpName %reserve_record_b1_ "reserve_record(b1;"
               OpName %claimed "claimed"
               OpName %store_quad_u1_u1_u1_u1_u1_ "store_quad(u1;u1;u1;u1;u1;"
               OpName %slot "slot"
               OpName %record "record"
               OpName %primitive_1 "primitive"
               OpName %xy_1 "xy"
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
               OpName %Control "Control"
               OpMemberName %Control 0 "control"
               OpName %control_buffer "control_buffer"
               OpName %hash "hash"
               OpName %param "param"
               OpName %param_0 "param"
               OpName %probe "probe"
               OpName %slot_0 "slot"
               OpName %base "base"
               OpName %claim "claim"
               OpName %count "count"
               OpName %leader "leader"
               OpName %gl_SubgroupInvocationID "gl_SubgroupInvocationID"
               OpName %first_record "first_record"
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
               OpName %base_0 "base"
               OpName %covered "covered"
               OpName %gl_HelperInvocation "gl_HelperInvocation"
               OpName %mask_0 "mask"
               OpName %member "member"
               OpName %first "first"
               OpName %lane "lane"
               OpName %origin "origin"
               OpName %gl_FragCoord "gl_FragCoord"
               OpName %quad "quad"
               OpName %writer "writer"
               OpName %primitive_2 "primitive"
               OpName %gl_PrimitiveID "gl_PrimitiveID"
               OpName %xy_2 "xy"
               OpName %slot_1 "slot"
               OpName %param_3 "param"
               OpName %param_4 "param"
               OpName %record_1 "record"
               OpName %param_5 "param"
               OpName %param_6 "param"
               OpName %param_7 "param"
               OpName %param_8 "param"
               OpName %param_9 "param"
               OpName %param_10 "param"
               OpName %word_0 "word"
               OpName %own "own"
               OpName %param_11 "param"
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
               OpDecorate %gl_SubgroupInvocationID RelaxedPrecision
               OpDecorate %gl_SubgroupInvocationID BuiltIn SubgroupLocalInvocationId
               OpDecorate %gl_SubgroupInvocationID Flat
               OpDecorate %200 RelaxedPrecision
               OpDecorate %204 RelaxedPrecision
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
               OpDecorate %396 RelaxedPrecision
               OpDecorate %397 RelaxedPrecision
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
       %bool = OpTypeBool
%_ptr_Function_bool = OpTypePointer Function %bool
         %32 = OpTypeFunction %uint %_ptr_Function_bool
         %36 = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint
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
%uint_4294967295 = OpConstant %uint 4294967295
   %uint_128 = OpConstant %uint 128
%_ptr_Input_uint = OpTypePointer Input %uint
%gl_SubgroupInvocationID = OpVariable %_ptr_Input_uint Input
    %uint_17 = OpConstant %uint 17
%_runtimearr_uint_0 = OpTypeRuntimeArray %uint
    %Records = OpTypeStruct %_runtimearr_uint_0
%_ptr_StorageBuffer_Records = OpTypePointer StorageBuffer %Records
%record_buffer = OpVariable %_ptr_StorageBuffer_Records StorageBuffer
      %false = OpConstantFalse %bool
%_runtimearr_uint_1 = OpTypeRuntimeArray %uint
 %References = OpTypeStruct %_runtimearr_uint_1
%_ptr_StorageBuffer_References = OpTypePointer StorageBuffer %References
%reference_buffer = OpVariable %_ptr_StorageBuffer_References StorageBuffer
       %true = OpConstantTrue %bool
    %uint_64 = OpConstant %uint 64
%_ptr_Input_bool = OpTypePointer Input %bool
%gl_HelperInvocation = OpVariable %_ptr_Input_bool Input
    %v2float = OpTypeVector %float 2
%_ptr_Function_v2float = OpTypePointer Function %v2float
%gl_FragCoord = OpVariable %_ptr_Input_v4float Input
     %v2uint = OpTypeVector %uint 2
%_ptr_Function_v2uint = OpTypePointer Function %v2uint
%_ptr_Input_int = OpTypePointer Input %int
%gl_PrimitiveID = OpVariable %_ptr_Input_int Input
 %uint_32768 = OpConstant %uint 32768
     %uint_8 = OpConstant %uint 8
    %uint_32 = OpConstant %uint 32
<capture_declarations>
       %main = OpFunction %void None %3
          %5 = OpLabel
    %covered = OpVariable %_ptr_Function_uint Function
     %mask_0 = OpVariable %_ptr_Function_uint Function
     %member = OpVariable %_ptr_Function_uint Function
      %first = OpVariable %_ptr_Function_uint Function
        %387 = OpVariable %_ptr_Function_uint Function
       %lane = OpVariable %_ptr_Function_uint Function
     %origin = OpVariable %_ptr_Function_v2float Function
       %quad = OpVariable %_ptr_Function_v2uint Function
     %writer = OpVariable %_ptr_Function_bool Function
%primitive_2 = OpVariable %_ptr_Function_uint Function
       %xy_2 = OpVariable %_ptr_Function_uint Function
     %slot_1 = OpVariable %_ptr_Function_uint Function
    %param_3 = OpVariable %_ptr_Function_uint Function
    %param_4 = OpVariable %_ptr_Function_uint Function
   %record_1 = OpVariable %_ptr_Function_uint Function
    %param_5 = OpVariable %_ptr_Function_bool Function
    %param_6 = OpVariable %_ptr_Function_uint Function
    %param_7 = OpVariable %_ptr_Function_uint Function
    %param_8 = OpVariable %_ptr_Function_uint Function
    %param_9 = OpVariable %_ptr_Function_uint Function
   %param_10 = OpVariable %_ptr_Function_uint Function
     %word_0 = OpVariable %_ptr_Function_uint Function
        %own = OpVariable %_ptr_Function_uint Function
   %param_11 = OpVariable %_ptr_Function_uint Function
   %member_0 = OpVariable %_ptr_Function_uint Function
      %value = OpVariable %_ptr_Function_uint Function
        %364 = OpLoad %bool %gl_HelperInvocation
        %365 = OpSelect %uint %364 %uint_0 %uint_1
               OpStore %covered %365
               OpStore %mask_0 %uint_0
               OpStore %member %uint_0
               OpBranch %368
        %368 = OpLabel
               OpLoopMerge %370 %371 None
               OpBranch %372
        %372 = OpLabel
        %373 = OpLoad %uint %member
        %374 = OpULessThan %bool %373 %uint_4
               OpBranchConditional %374 %369 %370
        %369 = OpLabel
        %375 = OpLoad %uint %covered
        %376 = OpLoad %uint %member
        %377 = OpGroupNonUniformQuadBroadcast %uint %uint_3 %375 %376
        %378 = OpLoad %uint %member
        %379 = OpShiftLeftLogical %uint %377 %378
        %380 = OpLoad %uint %mask_0
        %381 = OpBitwiseOr %uint %380 %379
               OpStore %mask_0 %381
               OpBranch %371
        %371 = OpLabel
        %382 = OpLoad %uint %member
        %383 = OpIAdd %uint %382 %int_1
               OpStore %member %383
               OpBranch %368
        %370 = OpLabel
        %385 = OpLoad %uint %mask_0
        %386 = OpIEqual %bool %385 %uint_0
               OpSelectionMerge %389 None
               OpBranchConditional %386 %388 %390
        %388 = OpLabel
               OpStore %387 %uint_0
               OpBranch %389
        %390 = OpLabel
        %391 = OpLoad %uint %mask_0
        %392 = OpExtInst %int %1 FindILsb %391
        %393 = OpBitcast %uint %392
               OpStore %387 %393
               OpBranch %389
        %389 = OpLabel
        %394 = OpLoad %uint %387
               OpStore %first %394
        %396 = OpLoad %uint %gl_SubgroupInvocationID
        %397 = OpBitwiseAnd %uint %396 %uint_3
               OpStore %lane %397
        %402 = OpLoad %v4float %gl_FragCoord
        %403 = OpVectorShuffle %v2float %402 %402 0 1
        %404 = OpGroupNonUniformQuadBroadcast %v2float %uint_3 %403 %uint_0
               OpStore %origin %404
        %408 = OpLoad %v2float %origin
        %409 = OpConvertFToU %v2uint %408
        %410 = OpCompositeConstruct %v2uint %uint_2 %uint_2
        %411 = OpUDiv %v2uint %409 %410
               OpStore %quad %411
        %413 = OpLoad %uint %covered
        %414 = OpINotEqual %bool %413 %uint_0
        %415 = OpLoad %uint %lane
        %416 = OpLoad %uint %first
        %417 = OpIEqual %bool %415 %416
        %418 = OpLogicalAnd %bool %414 %417
               OpStore %writer %418
        %422 = OpLoad %int %gl_PrimitiveID
        %423 = OpBitcast %uint %422
               OpStore %primitive_2 %423
        %425 = OpAccessChain %_ptr_Function_uint %quad %uint_0
        %426 = OpLoad %uint %425
        %427 = OpAccessChain %_ptr_Function_uint %quad %uint_1
        %428 = OpLoad %uint %427
        %429 = OpShiftLeftLogical %uint %428 %uint_16
        %430 = OpBitwiseOr %uint %426 %429
               OpStore %xy_2 %430
               OpStore %slot_1 %uint_4294967295
        %432 = OpLoad %bool %writer
               OpSelectionMerge %434 None
               OpBranchConditional %432 %433 %434
        %433 = OpLabel
        %435 = OpArrayLength %uint %control_buffer 0
        %436 = OpBitcast %int %435
        %437 = OpBitcast %uint %436
        %438 = OpFunctionCall %uint %wave_table_offset_
        %439 = OpAccessChain %_ptr_PushConstant_uint %transport %int_6
        %440 = OpLoad %uint %439
        %441 = OpIAdd %uint %438 %440
        %442 = OpULessThan %bool %437 %441
        %443 = OpLogicalNot %bool %442
               OpSelectionMerge %445 None
               OpBranchConditional %443 %444 %445
        %444 = OpLabel
        %446 = OpArrayLength %uint %record_buffer 0
        %447 = OpBitcast %int %446
        %448 = OpBitcast %uint %447
        %449 = OpAccessChain %_ptr_PushConstant_uint %transport %int_0
        %450 = OpLoad %uint %449
        %451 = OpFunctionCall %uint %quad_words_
        %452 = OpIMul %uint %450 %451
        %453 = OpULessThan %bool %448 %452
               OpBranch %445
        %445 = OpLabel
        %454 = OpPhi %bool %442 %433 %453 %444
        %455 = OpLogicalNot %bool %454
               OpSelectionMerge %457 None
               OpBranchConditional %455 %456 %457
        %456 = OpLabel
        %458 = OpArrayLength %uint %reference_buffer 0
        %459 = OpBitcast %int %458
        %460 = OpBitcast %uint %459
        %461 = OpFunctionCall %uint %output_reference_offset_
        %462 = OpAccessChain %_ptr_PushConstant_uint %transport %int_0
        %463 = OpLoad %uint %462
        %464 = OpIAdd %uint %461 %463
        %465 = OpULessThan %bool %460 %464
               OpBranch %457
        %457 = OpLabel
        %466 = OpPhi %bool %454 %445 %465 %456
               OpSelectionMerge %468 None
               OpBranchConditional %466 %467 %477
        %467 = OpLabel
        %469 = OpArrayLength %uint %control_buffer 0
        %470 = OpBitcast %int %469
        %471 = OpBitcast %uint %470
        %472 = OpUGreaterThanEqual %bool %471 %uint_16
               OpSelectionMerge %474 None
               OpBranchConditional %472 %473 %474
        %473 = OpLabel
        %475 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %476 = OpAtomicOr %uint %475 %uint_1 %uint_0 %uint_128
               OpBranch %474
        %474 = OpLabel
               OpBranch %468
        %477 = OpLabel
        %478 = OpAccessChain %_ptr_Function_uint %quad %uint_0
        %479 = OpLoad %uint %478
        %481 = OpUGreaterThanEqual %bool %479 %uint_32768
        %482 = OpLogicalNot %bool %481
               OpSelectionMerge %484 None
               OpBranchConditional %482 %483 %484
        %483 = OpLabel
        %485 = OpAccessChain %_ptr_Function_uint %quad %uint_1
        %486 = OpLoad %uint %485
        %487 = OpUGreaterThanEqual %bool %486 %uint_32768
               OpBranch %484
        %484 = OpLabel
        %488 = OpPhi %bool %481 %477 %487 %483
               OpSelectionMerge %490 None
               OpBranchConditional %488 %489 %493
        %489 = OpLabel
        %491 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %492 = OpAtomicOr %uint %491 %uint_1 %uint_0 %uint_4
               OpBranch %490
        %493 = OpLabel
        %495 = OpLoad %uint %primitive_2
               OpStore %param_3 %495
        %497 = OpLoad %uint %xy_2
               OpStore %param_4 %497
        %498 = OpFunctionCall %uint %claim_slot_u1_u1_ %param_3 %param_4
               OpStore %slot_1 %498
               OpBranch %490
        %490 = OpLabel
               OpBranch %468
        %468 = OpLabel
               OpBranch %434
        %434 = OpLabel
        %500 = OpLoad %uint %slot_1
        %501 = OpINotEqual %bool %500 %uint_4294967295
               OpStore %param_5 %501
        %503 = OpFunctionCall %uint %reserve_record_b1_ %param_5
               OpStore %record_1 %503
        %504 = OpLoad %uint %record_1
        %505 = OpINotEqual %bool %504 %uint_4294967295
               OpSelectionMerge %507 None
               OpBranchConditional %505 %506 %507
        %506 = OpLabel
        %509 = OpLoad %uint %slot_1
               OpStore %param_6 %509
        %511 = OpLoad %uint %record_1
               OpStore %param_7 %511
        %513 = OpLoad %uint %primitive_2
               OpStore %param_8 %513
        %515 = OpLoad %uint %xy_2
               OpStore %param_9 %515
        %517 = OpLoad %uint %mask_0
               OpStore %param_10 %517
        %518 = OpFunctionCall %void %store_quad_u1_u1_u1_u1_u1_ %param_6 %param_7 %param_8 %param_9 %param_10
               OpBranch %507
        %507 = OpLabel
        %519 = OpLoad %uint %record_1
        %520 = OpLoad %uint %first
        %521 = OpGroupNonUniformQuadBroadcast %uint %uint_3 %519 %520
               OpStore %record_1 %521
               OpStore %word_0 %uint_0
               OpBranch %523
        %523 = OpLabel
               OpLoopMerge %525 %526 None
               OpBranch %527
        %527 = OpLabel
        %528 = OpLoad %uint %word_0
        %529 = OpAccessChain %_ptr_PushConstant_uint %transport %int_2
        %530 = OpLoad %uint %529
        %531 = OpULessThan %bool %528 %530
               OpBranchConditional %531 %524 %525
        %524 = OpLabel
        %534 = OpLoad %uint %word_0
               OpStore %param_11 %534
        %535 = OpFunctionCall %uint %read_lane_word_u1_ %param_11
               OpStore %own %535
               OpStore %member_0 %uint_0
               OpBranch %537
        %537 = OpLabel
               OpLoopMerge %539 %540 None
               OpBranch %541
        %541 = OpLabel
        %542 = OpLoad %uint %member_0
        %543 = OpULessThan %bool %542 %uint_4
               OpBranchConditional %543 %538 %539
        %538 = OpLabel
        %545 = OpLoad %uint %own
        %546 = OpLoad %uint %member_0
        %547 = OpGroupNonUniformQuadBroadcast %uint %uint_3 %545 %546
               OpStore %value %547
        %548 = OpLoad %bool %writer
        %549 = OpLoad %uint %record_1
        %550 = OpINotEqual %bool %549 %uint_4294967295
        %551 = OpLogicalAnd %bool %548 %550
               OpSelectionMerge %553 None
               OpBranchConditional %551 %552 %553
        %552 = OpLabel
        %554 = OpLoad %uint %record_1
        %555 = OpFunctionCall %uint %quad_words_
        %556 = OpIMul %uint %554 %555
        %557 = OpIAdd %uint %556 %uint_4
        %558 = OpLoad %uint %member_0
        %559 = OpAccessChain %_ptr_PushConstant_uint %transport %int_2
        %560 = OpLoad %uint %559
        %561 = OpIMul %uint %558 %560
        %562 = OpIAdd %uint %557 %561
        %563 = OpLoad %uint %word_0
        %564 = OpIAdd %uint %562 %563
        %565 = OpLoad %uint %value
        %566 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %564
               OpStore %566 %565
               OpBranch %553
        %553 = OpLabel
               OpBranch %540
        %540 = OpLabel
        %567 = OpLoad %uint %member_0
        %568 = OpIAdd %uint %567 %int_1
               OpStore %member_0 %568
               OpBranch %537
        %539 = OpLabel
               OpBranch %526
        %526 = OpLabel
        %569 = OpLoad %uint %word_0
        %570 = OpIAdd %uint %569 %int_1
               OpStore %word_0 %570
               OpBranch %523
        %525 = OpLabel
               OpReturn
               OpFunctionEnd
%quad_words_ = OpFunction %uint None %7
          %9 = OpLabel
         %51 = OpAccessChain %_ptr_PushConstant_uint %transport %int_2
         %52 = OpLoad %uint %51
         %53 = OpIMul %uint %uint_4 %52
         %54 = OpIAdd %uint %uint_4 %53
               OpReturnValue %54
               OpFunctionEnd
%wave_table_offset_ = OpFunction %uint None %7
         %11 = OpLabel
         %60 = OpAccessChain %_ptr_PushConstant_uint %transport %int_1
         %61 = OpLoad %uint %60
         %62 = OpIMul %uint %uint_2 %61
         %63 = OpIAdd %uint %uint_16 %62
         %65 = OpAccessChain %_ptr_PushConstant_uint %transport %int_4
         %66 = OpLoad %uint %65
         %67 = OpIMul %uint %uint_4 %66
         %68 = OpIAdd %uint %63 %67
               OpReturnValue %68
               OpFunctionEnd
%active_reference_offset_ = OpFunction %uint None %7
         %13 = OpLabel
         %72 = OpAccessChain %_ptr_PushConstant_uint %transport %int_6
         %73 = OpLoad %uint %72
         %74 = OpIMul %uint %uint_16 %73
               OpReturnValue %74
               OpFunctionEnd
%output_reference_offset_ = OpFunction %uint None %7
         %15 = OpLabel
         %77 = OpFunctionCall %uint %active_reference_offset_
         %79 = OpAccessChain %_ptr_PushConstant_uint %transport %int_5
         %80 = OpLoad %uint %79
         %81 = OpIAdd %uint %77 %80
               OpReturnValue %81
               OpFunctionEnd
<capture_reader>
%hash_key_u1_u1_ = OpFunction %uint None %21
  %primitive = OpFunctionParameter %_ptr_Function_uint
         %xy = OpFunctionParameter %_ptr_Function_uint
         %25 = OpLabel
          %h = OpVariable %_ptr_Function_uint Function
         %98 = OpLoad %uint %xy
        %100 = OpIMul %uint %98 %uint_2654435769
        %101 = OpLoad %uint %primitive
        %103 = OpIMul %uint %101 %uint_2246822507
        %104 = OpBitwiseXor %uint %100 %103
               OpStore %h %104
        %105 = OpLoad %uint %h
        %106 = OpShiftRightLogical %uint %105 %uint_16
        %107 = OpLoad %uint %h
        %108 = OpBitwiseXor %uint %107 %106
               OpStore %h %108
        %110 = OpLoad %uint %h
        %111 = OpIMul %uint %110 %uint_2146121005
               OpStore %h %111
        %112 = OpLoad %uint %h
        %113 = OpLoad %uint %h
        %115 = OpShiftRightLogical %uint %113 %uint_15
        %116 = OpBitwiseXor %uint %112 %115
               OpReturnValue %116
               OpFunctionEnd
%claim_slot_u1_u1_ = OpFunction %uint None %21
%primitive_0 = OpFunctionParameter %_ptr_Function_uint
       %xy_0 = OpFunctionParameter %_ptr_Function_uint
         %29 = OpLabel
       %hash = OpVariable %_ptr_Function_uint Function
      %param = OpVariable %_ptr_Function_uint Function
    %param_0 = OpVariable %_ptr_Function_uint Function
      %probe = OpVariable %_ptr_Function_uint Function
     %slot_0 = OpVariable %_ptr_Function_uint Function
       %base = OpVariable %_ptr_Function_uint Function
        %119 = OpLoad %uint %primitive_0
        %120 = OpAccessChain %_ptr_PushConstant_uint %transport %int_1
        %121 = OpLoad %uint %120
        %122 = OpUGreaterThanEqual %bool %119 %121
               OpSelectionMerge %124 None
               OpBranchConditional %122 %123 %124
        %123 = OpLabel
        %131 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %134 = OpAtomicOr %uint %131 %uint_1 %uint_0 %uint_4
               OpReturnValue %uint_4294967295
        %124 = OpLabel
        %139 = OpLoad %uint %primitive_0
               OpStore %param %139
        %141 = OpLoad %uint %xy_0
               OpStore %param_0 %141
        %142 = OpFunctionCall %uint %hash_key_u1_u1_ %param %param_0
               OpStore %hash %142
               OpStore %probe %uint_0
               OpBranch %144
        %144 = OpLabel
               OpLoopMerge %146 %147 None
               OpBranch %148
        %148 = OpLabel
        %149 = OpLoad %uint %probe
        %150 = OpAccessChain %_ptr_PushConstant_uint %transport %int_4
        %151 = OpLoad %uint %150
        %153 = OpExtInst %uint %1 UMin %151 %uint_128
        %154 = OpULessThan %bool %149 %153
               OpBranchConditional %154 %145 %146
        %145 = OpLabel
        %156 = OpLoad %uint %hash
        %157 = OpLoad %uint %probe
        %158 = OpIAdd %uint %156 %157
        %159 = OpAccessChain %_ptr_PushConstant_uint %transport %int_4
        %160 = OpLoad %uint %159
        %161 = OpISub %uint %160 %uint_1
        %162 = OpBitwiseAnd %uint %158 %161
               OpStore %slot_0 %162
        %164 = OpAccessChain %_ptr_PushConstant_uint %transport %int_1
        %165 = OpLoad %uint %164
        %166 = OpIMul %uint %165 %uint_2
        %167 = OpIAdd %uint %uint_16 %166
        %168 = OpLoad %uint %slot_0
        %169 = OpIMul %uint %168 %uint_4
        %170 = OpIAdd %uint %167 %169
               OpStore %base %170
        %171 = OpLoad %uint %base
        %172 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %171
        %173 = OpAtomicCompareExchange %uint %172 %uint_1 %uint_0 %uint_0 %uint_1 %uint_0
        %174 = OpIEqual %bool %173 %uint_0
               OpSelectionMerge %176 None
               OpBranchConditional %174 %175 %176
        %175 = OpLabel
        %177 = OpLoad %uint %slot_0
               OpReturnValue %177
        %176 = OpLabel
               OpBranch %147
        %147 = OpLabel
        %179 = OpLoad %uint %probe
        %180 = OpIAdd %uint %179 %int_1
               OpStore %probe %180
               OpBranch %144
        %146 = OpLabel
        %181 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %182 = OpAtomicOr %uint %181 %uint_1 %uint_0 %uint_2
               OpReturnValue %uint_4294967295
               OpFunctionEnd
%reserve_record_b1_ = OpFunction %uint None %32
    %claimed = OpFunctionParameter %_ptr_Function_bool
         %35 = OpLabel
      %claim = OpVariable %_ptr_Function_uint Function
      %count = OpVariable %_ptr_Function_uint Function
     %leader = OpVariable %_ptr_Function_uint Function
%first_record = OpVariable %_ptr_Function_uint Function
   %record_0 = OpVariable %_ptr_Function_uint Function
        %186 = OpLoad %bool %claimed
        %187 = OpSelect %uint %186 %uint_1 %uint_0
               OpStore %claim %187
        %189 = OpLoad %uint %claim
        %190 = OpGroupNonUniformIAdd %uint %uint_3 Reduce %189
               OpStore %count %190
        %191 = OpLoad %uint %count
        %192 = OpIEqual %bool %191 %uint_0
               OpSelectionMerge %194 None
               OpBranchConditional %192 %193 %194
        %193 = OpLabel
               OpReturnValue %uint_4294967295
        %194 = OpLabel
        %197 = OpLoad %bool %claimed
        %200 = OpLoad %uint %gl_SubgroupInvocationID
        %201 = OpSelect %uint %197 %200 %uint_4294967295
        %202 = OpGroupNonUniformUMin %uint %uint_3 Reduce %201
               OpStore %leader %202
               OpStore %first_record %uint_0
        %204 = OpLoad %uint %gl_SubgroupInvocationID
        %205 = OpLoad %uint %leader
        %206 = OpIEqual %bool %204 %205
               OpSelectionMerge %208 None
               OpBranchConditional %206 %207 %208
        %207 = OpLabel
        %209 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_0
        %210 = OpLoad %uint %count
        %211 = OpAtomicIAdd %uint %209 %uint_1 %uint_0 %210
               OpStore %first_record %211
               OpBranch %208
        %208 = OpLabel
        %212 = OpLoad %uint %first_record
        %213 = OpGroupNonUniformUMax %uint %uint_3 Reduce %212
               OpStore %first_record %213
        %214 = OpLoad %bool %claimed
        %215 = OpLogicalNot %bool %214
               OpSelectionMerge %217 None
               OpBranchConditional %215 %216 %217
        %216 = OpLabel
               OpReturnValue %uint_4294967295
        %217 = OpLabel
        %220 = OpLoad %uint %first_record
        %221 = OpLoad %uint %claim
        %222 = OpGroupNonUniformIAdd %uint %uint_3 ExclusiveScan %221
        %223 = OpIAdd %uint %220 %222
               OpStore %record_0 %223
        %224 = OpLoad %uint %record_0
        %225 = OpAccessChain %_ptr_PushConstant_uint %transport %int_0
        %226 = OpLoad %uint %225
        %227 = OpULessThan %bool %224 %226
               OpSelectionMerge %229 None
               OpBranchConditional %227 %228 %229
        %228 = OpLabel
        %230 = OpLoad %uint %record_0
               OpReturnValue %230
        %229 = OpLabel
        %232 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %233 = OpAtomicOr %uint %232 %uint_1 %uint_0 %uint_1
               OpReturnValue %uint_4294967295
               OpFunctionEnd
%store_quad_u1_u1_u1_u1_u1_ = OpFunction %void None %36
       %slot = OpFunctionParameter %_ptr_Function_uint
     %record = OpFunctionParameter %_ptr_Function_uint
%primitive_1 = OpFunctionParameter %_ptr_Function_uint
       %xy_1 = OpFunctionParameter %_ptr_Function_uint
       %mask = OpFunctionParameter %_ptr_Function_uint
         %43 = OpLabel
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
     %base_0 = OpVariable %_ptr_Function_uint Function
        %238 = OpLoad %uint %primitive_1
        %239 = OpIMul %uint %238 %uint_2
        %240 = OpIAdd %uint %uint_17 %239
        %241 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %240
        %242 = OpAtomicIAdd %uint %241 %uint_1 %uint_0 %uint_1
               OpStore %ordinal %242
        %244 = OpLoad %uint %record
        %245 = OpFunctionCall %uint %quad_words_
        %246 = OpIMul %uint %244 %245
               OpStore %header %246
        %251 = OpLoad %uint %header
        %252 = OpLoad %uint %mask
        %253 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %251
               OpStore %253 %252
        %254 = OpLoad %uint %header
        %255 = OpIAdd %uint %254 %uint_1
        %256 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %255
               OpStore %256 %uint_1
        %257 = OpLoad %uint %header
        %258 = OpIAdd %uint %257 %uint_2
        %259 = OpLoad %uint %primitive_1
        %260 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %258
               OpStore %260 %259
        %261 = OpLoad %uint %header
        %262 = OpIAdd %uint %261 %uint_3
        %263 = OpLoad %uint %ordinal
        %264 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %262
               OpStore %264 %263
        %266 = OpLoad %uint %primitive_1
        %267 = OpShiftLeftLogical %uint %266 %uint_16
        %268 = OpLoad %uint %ordinal
        %269 = OpUDiv %uint %268 %uint_16
        %270 = OpBitwiseOr %uint %267 %269
        %271 = OpIAdd %uint %270 %uint_1
               OpStore %key %271
        %273 = OpLoad %uint %ordinal
        %274 = OpUDiv %uint %273 %uint_16
        %276 = OpLoad %uint %primitive_1
               OpStore %param_1 %276
               OpStore %param_2 %274
        %278 = OpFunctionCall %uint %hash_key_u1_u1_ %param_1 %param_2
               OpStore %wave_hash %278
               OpStore %found %false
               OpStore %attempt %uint_0
               OpBranch %282
        %282 = OpLabel
               OpLoopMerge %284 %285 None
               OpBranch %286
        %286 = OpLabel
        %287 = OpLoad %uint %attempt
        %288 = OpAccessChain %_ptr_PushConstant_uint %transport %int_6
        %289 = OpLoad %uint %288
        %290 = OpExtInst %uint %1 UMin %289 %uint_128
        %291 = OpULessThan %bool %287 %290
               OpBranchConditional %291 %283 %284
        %283 = OpLabel
        %293 = OpLoad %uint %wave_hash
        %294 = OpLoad %uint %attempt
        %295 = OpIAdd %uint %293 %294
        %296 = OpAccessChain %_ptr_PushConstant_uint %transport %int_6
        %297 = OpLoad %uint %296
        %298 = OpISub %uint %297 %uint_1
        %299 = OpBitwiseAnd %uint %295 %298
               OpStore %wave_slot %299
        %301 = OpFunctionCall %uint %wave_table_offset_
        %302 = OpLoad %uint %wave_slot
        %303 = OpIAdd %uint %301 %302
        %304 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %303
        %305 = OpLoad %uint %key
        %306 = OpAtomicCompareExchange %uint %304 %uint_1 %uint_0 %uint_0 %305 %uint_0
               OpStore %previous %306
        %307 = OpLoad %uint %previous
        %308 = OpINotEqual %bool %307 %uint_0
        %309 = OpLoad %uint %previous
        %310 = OpLoad %uint %key
        %311 = OpINotEqual %bool %309 %310
        %312 = OpLogicalAnd %bool %308 %311
               OpSelectionMerge %314 None
               OpBranchConditional %312 %313 %314
        %313 = OpLabel
               OpBranch %285
        %314 = OpLabel
        %320 = OpLoad %uint %wave_slot
        %321 = OpIMul %uint %320 %uint_16
        %322 = OpLoad %uint %ordinal
        %323 = OpBitwiseAnd %uint %322 %uint_15
        %324 = OpIAdd %uint %321 %323
        %325 = OpLoad %uint %record
        %326 = OpAccessChain %_ptr_StorageBuffer_uint %reference_buffer %int_0 %324
               OpStore %326 %325
               OpStore %found %true
               OpBranch %284
        %285 = OpLabel
        %329 = OpLoad %uint %attempt
        %330 = OpIAdd %uint %329 %int_1
               OpStore %attempt %330
               OpBranch %282
        %284 = OpLabel
        %331 = OpLoad %bool %found
        %332 = OpLogicalNot %bool %331
               OpSelectionMerge %334 None
               OpBranchConditional %332 %333 %334
        %333 = OpLabel
        %335 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %337 = OpAtomicOr %uint %335 %uint_1 %uint_0 %uint_64
               OpBranch %334
        %334 = OpLabel
        %339 = OpAccessChain %_ptr_PushConstant_uint %transport %int_1
        %340 = OpLoad %uint %339
        %341 = OpIMul %uint %340 %uint_2
        %342 = OpIAdd %uint %uint_16 %341
        %343 = OpLoad %uint %slot
        %344 = OpIMul %uint %343 %uint_4
        %345 = OpIAdd %uint %342 %344
               OpStore %base_0 %345
        %346 = OpLoad %uint %base_0
        %347 = OpIAdd %uint %346 %uint_1
        %348 = OpLoad %uint %primitive_1
        %349 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %347
               OpStore %349 %348
        %350 = OpLoad %uint %base_0
        %351 = OpIAdd %uint %350 %uint_2
        %352 = OpLoad %uint %xy_1
        %353 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %351
               OpStore %353 %352
        %354 = OpLoad %uint %base_0
        %355 = OpIAdd %uint %354 %uint_3
        %356 = OpLoad %uint %record
        %357 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %355
               OpStore %357 %356
        %358 = OpLoad %uint %base_0
        %359 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %358
        %360 = OpAtomicExchange %uint %359 %uint_1 %uint_0 %uint_2
               OpReturn
               OpFunctionEnd
)CORE";
