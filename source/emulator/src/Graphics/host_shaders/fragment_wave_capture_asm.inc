// Generated from the public capture kernel. Do not edit by hand.
constexpr const char* kFragmentCaptureReaderSignature = R"CORE(%read_lane_word_u1_ = OpFunction %uint None %17
       %word = OpFunctionParameter %_ptr_Function_uint
)CORE";
constexpr const char* kFragmentCaptureCore = R"CORE(; SPIR-V
; Version: 1.6
; Generator: Google Shaderc over Glslang; 11
; Bound: 643
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
               OpName %reserve_ordinal_b1_u1_ "reserve_ordinal(b1;u1;"
               OpName %reserved "reserved"
               OpName %primitive_1 "primitive"
               OpName %store_quad_u1_u1_u1_u1_u1_u1_ "store_quad(u1;u1;u1;u1;u1;u1;"
               OpName %slot "slot"
               OpName %record "record"
               OpName %ordinal "ordinal"
               OpName %primitive_2 "primitive"
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
               OpName %ordinal_0 "ordinal"
               OpName %pending "pending"
               OpName %leader_0 "leader"
               OpName %leader_primitive "leader_primitive"
               OpName %member "member"
               OpName %count_0 "count"
               OpName %rank "rank"
               OpName %first_ordinal "first_ordinal"
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
               OpName %member_0 "member"
               OpName %first "first"
               OpName %lane "lane"
               OpName %origin "origin"
               OpName %gl_FragCoord "gl_FragCoord"
               OpName %quad "quad"
               OpName %writer "writer"
               OpName %primitive_3 "primitive"
               OpName %gl_PrimitiveID "gl_PrimitiveID"
               OpName %xy_2 "xy"
               OpName %slot_1 "slot"
               OpName %param_3 "param"
               OpName %param_4 "param"
               OpName %record_1 "record"
               OpName %param_5 "param"
               OpName %ordinal_1 "ordinal"
               OpName %param_6 "param"
               OpName %param_7 "param"
               OpName %param_8 "param"
               OpName %param_9 "param"
               OpName %param_10 "param"
               OpName %param_11 "param"
               OpName %param_12 "param"
               OpName %param_13 "param"
               OpName %word_0 "word"
               OpName %own "own"
               OpName %param_14 "param"
               OpName %member_1 "member"
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
               OpDecorate %206 RelaxedPrecision
               OpDecorate %210 RelaxedPrecision
               OpDecorate %256 RelaxedPrecision
               OpDecorate %260 RelaxedPrecision
               OpDecorate %280 RelaxedPrecision
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
               OpDecorate %457 RelaxedPrecision
               OpDecorate %458 RelaxedPrecision
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
         %36 = OpTypeFunction %uint %_ptr_Function_bool %_ptr_Function_uint
         %41 = OpTypeFunction %void %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint %_ptr_Function_uint
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
      %false = OpConstantFalse %bool
%_runtimearr_uint_0 = OpTypeRuntimeArray %uint
    %Records = OpTypeStruct %_runtimearr_uint_0
%_ptr_StorageBuffer_Records = OpTypePointer StorageBuffer %Records
%record_buffer = OpVariable %_ptr_StorageBuffer_Records StorageBuffer
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
   %member_0 = OpVariable %_ptr_Function_uint Function
      %first = OpVariable %_ptr_Function_uint Function
        %448 = OpVariable %_ptr_Function_uint Function
       %lane = OpVariable %_ptr_Function_uint Function
     %origin = OpVariable %_ptr_Function_v2float Function
       %quad = OpVariable %_ptr_Function_v2uint Function
     %writer = OpVariable %_ptr_Function_bool Function
%primitive_3 = OpVariable %_ptr_Function_uint Function
       %xy_2 = OpVariable %_ptr_Function_uint Function
     %slot_1 = OpVariable %_ptr_Function_uint Function
    %param_3 = OpVariable %_ptr_Function_uint Function
    %param_4 = OpVariable %_ptr_Function_uint Function
   %record_1 = OpVariable %_ptr_Function_uint Function
    %param_5 = OpVariable %_ptr_Function_bool Function
  %ordinal_1 = OpVariable %_ptr_Function_uint Function
    %param_6 = OpVariable %_ptr_Function_bool Function
    %param_7 = OpVariable %_ptr_Function_uint Function
    %param_8 = OpVariable %_ptr_Function_uint Function
    %param_9 = OpVariable %_ptr_Function_uint Function
   %param_10 = OpVariable %_ptr_Function_uint Function
   %param_11 = OpVariable %_ptr_Function_uint Function
   %param_12 = OpVariable %_ptr_Function_uint Function
   %param_13 = OpVariable %_ptr_Function_uint Function
     %word_0 = OpVariable %_ptr_Function_uint Function
        %own = OpVariable %_ptr_Function_uint Function
   %param_14 = OpVariable %_ptr_Function_uint Function
   %member_1 = OpVariable %_ptr_Function_uint Function
      %value = OpVariable %_ptr_Function_uint Function
        %425 = OpLoad %bool %gl_HelperInvocation
        %426 = OpSelect %uint %425 %uint_0 %uint_1
               OpStore %covered %426
               OpStore %mask_0 %uint_0
               OpStore %member_0 %uint_0
               OpBranch %429
        %429 = OpLabel
               OpLoopMerge %431 %432 None
               OpBranch %433
        %433 = OpLabel
        %434 = OpLoad %uint %member_0
        %435 = OpULessThan %bool %434 %uint_4
               OpBranchConditional %435 %430 %431
        %430 = OpLabel
        %436 = OpLoad %uint %covered
        %437 = OpLoad %uint %member_0
        %438 = OpGroupNonUniformQuadBroadcast %uint %uint_3 %436 %437
        %439 = OpLoad %uint %member_0
        %440 = OpShiftLeftLogical %uint %438 %439
        %441 = OpLoad %uint %mask_0
        %442 = OpBitwiseOr %uint %441 %440
               OpStore %mask_0 %442
               OpBranch %432
        %432 = OpLabel
        %443 = OpLoad %uint %member_0
        %444 = OpIAdd %uint %443 %int_1
               OpStore %member_0 %444
               OpBranch %429
        %431 = OpLabel
        %446 = OpLoad %uint %mask_0
        %447 = OpIEqual %bool %446 %uint_0
               OpSelectionMerge %450 None
               OpBranchConditional %447 %449 %451
        %449 = OpLabel
               OpStore %448 %uint_0
               OpBranch %450
        %451 = OpLabel
        %452 = OpLoad %uint %mask_0
        %453 = OpExtInst %int %1 FindILsb %452
        %454 = OpBitcast %uint %453
               OpStore %448 %454
               OpBranch %450
        %450 = OpLabel
        %455 = OpLoad %uint %448
               OpStore %first %455
        %457 = OpLoad %uint %gl_SubgroupInvocationID
        %458 = OpBitwiseAnd %uint %457 %uint_3
               OpStore %lane %458
        %463 = OpLoad %v4float %gl_FragCoord
        %464 = OpVectorShuffle %v2float %463 %463 0 1
        %465 = OpGroupNonUniformQuadBroadcast %v2float %uint_3 %464 %uint_0
               OpStore %origin %465
        %469 = OpLoad %v2float %origin
        %470 = OpConvertFToU %v2uint %469
        %471 = OpCompositeConstruct %v2uint %uint_2 %uint_2
        %472 = OpUDiv %v2uint %470 %471
               OpStore %quad %472
        %474 = OpLoad %uint %covered
        %475 = OpINotEqual %bool %474 %uint_0
        %476 = OpLoad %uint %lane
        %477 = OpLoad %uint %first
        %478 = OpIEqual %bool %476 %477
        %479 = OpLogicalAnd %bool %475 %478
               OpStore %writer %479
        %483 = OpLoad %int %gl_PrimitiveID
        %484 = OpBitcast %uint %483
               OpStore %primitive_3 %484
        %486 = OpAccessChain %_ptr_Function_uint %quad %uint_0
        %487 = OpLoad %uint %486
        %488 = OpAccessChain %_ptr_Function_uint %quad %uint_1
        %489 = OpLoad %uint %488
        %490 = OpShiftLeftLogical %uint %489 %uint_16
        %491 = OpBitwiseOr %uint %487 %490
               OpStore %xy_2 %491
               OpStore %slot_1 %uint_4294967295
        %493 = OpLoad %bool %writer
               OpSelectionMerge %495 None
               OpBranchConditional %493 %494 %495
        %494 = OpLabel
        %496 = OpArrayLength %uint %control_buffer 0
        %497 = OpBitcast %int %496
        %498 = OpBitcast %uint %497
        %499 = OpFunctionCall %uint %wave_table_offset_
        %500 = OpAccessChain %_ptr_PushConstant_uint %transport %int_6
        %501 = OpLoad %uint %500
        %502 = OpIAdd %uint %499 %501
        %503 = OpULessThan %bool %498 %502
        %504 = OpLogicalNot %bool %503
               OpSelectionMerge %506 None
               OpBranchConditional %504 %505 %506
        %505 = OpLabel
        %507 = OpArrayLength %uint %record_buffer 0
        %508 = OpBitcast %int %507
        %509 = OpBitcast %uint %508
        %510 = OpAccessChain %_ptr_PushConstant_uint %transport %int_0
        %511 = OpLoad %uint %510
        %512 = OpFunctionCall %uint %quad_words_
        %513 = OpIMul %uint %511 %512
        %514 = OpULessThan %bool %509 %513
               OpBranch %506
        %506 = OpLabel
        %515 = OpPhi %bool %503 %494 %514 %505
        %516 = OpLogicalNot %bool %515
               OpSelectionMerge %518 None
               OpBranchConditional %516 %517 %518
        %517 = OpLabel
        %519 = OpArrayLength %uint %reference_buffer 0
        %520 = OpBitcast %int %519
        %521 = OpBitcast %uint %520
        %522 = OpFunctionCall %uint %output_reference_offset_
        %523 = OpAccessChain %_ptr_PushConstant_uint %transport %int_0
        %524 = OpLoad %uint %523
        %525 = OpIAdd %uint %522 %524
        %526 = OpULessThan %bool %521 %525
               OpBranch %518
        %518 = OpLabel
        %527 = OpPhi %bool %515 %506 %526 %517
               OpSelectionMerge %529 None
               OpBranchConditional %527 %528 %538
        %528 = OpLabel
        %530 = OpArrayLength %uint %control_buffer 0
        %531 = OpBitcast %int %530
        %532 = OpBitcast %uint %531
        %533 = OpUGreaterThanEqual %bool %532 %uint_16
               OpSelectionMerge %535 None
               OpBranchConditional %533 %534 %535
        %534 = OpLabel
        %536 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %537 = OpAtomicOr %uint %536 %uint_1 %uint_0 %uint_128
               OpBranch %535
        %535 = OpLabel
               OpBranch %529
        %538 = OpLabel
        %539 = OpAccessChain %_ptr_Function_uint %quad %uint_0
        %540 = OpLoad %uint %539
        %542 = OpUGreaterThanEqual %bool %540 %uint_32768
        %543 = OpLogicalNot %bool %542
               OpSelectionMerge %545 None
               OpBranchConditional %543 %544 %545
        %544 = OpLabel
        %546 = OpAccessChain %_ptr_Function_uint %quad %uint_1
        %547 = OpLoad %uint %546
        %548 = OpUGreaterThanEqual %bool %547 %uint_32768
               OpBranch %545
        %545 = OpLabel
        %549 = OpPhi %bool %542 %538 %548 %544
               OpSelectionMerge %551 None
               OpBranchConditional %549 %550 %554
        %550 = OpLabel
        %552 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %553 = OpAtomicOr %uint %552 %uint_1 %uint_0 %uint_4
               OpBranch %551
        %554 = OpLabel
        %556 = OpLoad %uint %primitive_3
               OpStore %param_3 %556
        %558 = OpLoad %uint %xy_2
               OpStore %param_4 %558
        %559 = OpFunctionCall %uint %claim_slot_u1_u1_ %param_3 %param_4
               OpStore %slot_1 %559
               OpBranch %551
        %551 = OpLabel
               OpBranch %529
        %529 = OpLabel
               OpBranch %495
        %495 = OpLabel
        %561 = OpLoad %uint %slot_1
        %562 = OpINotEqual %bool %561 %uint_4294967295
               OpStore %param_5 %562
        %564 = OpFunctionCall %uint %reserve_record_b1_ %param_5
               OpStore %record_1 %564
        %566 = OpLoad %uint %record_1
        %567 = OpINotEqual %bool %566 %uint_4294967295
               OpStore %param_6 %567
        %570 = OpLoad %uint %primitive_3
               OpStore %param_7 %570
        %571 = OpFunctionCall %uint %reserve_ordinal_b1_u1_ %param_6 %param_7
               OpStore %ordinal_1 %571
        %572 = OpLoad %uint %record_1
        %573 = OpINotEqual %bool %572 %uint_4294967295
               OpSelectionMerge %575 None
               OpBranchConditional %573 %574 %575
        %574 = OpLabel
        %577 = OpLoad %uint %slot_1
               OpStore %param_8 %577
        %579 = OpLoad %uint %record_1
               OpStore %param_9 %579
        %581 = OpLoad %uint %ordinal_1
               OpStore %param_10 %581
        %583 = OpLoad %uint %primitive_3
               OpStore %param_11 %583
        %585 = OpLoad %uint %xy_2
               OpStore %param_12 %585
        %587 = OpLoad %uint %mask_0
               OpStore %param_13 %587
        %588 = OpFunctionCall %void %store_quad_u1_u1_u1_u1_u1_u1_ %param_8 %param_9 %param_10 %param_11 %param_12 %param_13
               OpBranch %575
        %575 = OpLabel
        %589 = OpLoad %uint %record_1
        %590 = OpLoad %uint %first
        %591 = OpGroupNonUniformQuadBroadcast %uint %uint_3 %589 %590
               OpStore %record_1 %591
               OpStore %word_0 %uint_0
               OpBranch %593
        %593 = OpLabel
               OpLoopMerge %595 %596 None
               OpBranch %597
        %597 = OpLabel
        %598 = OpLoad %uint %word_0
        %599 = OpAccessChain %_ptr_PushConstant_uint %transport %int_2
        %600 = OpLoad %uint %599
        %601 = OpULessThan %bool %598 %600
               OpBranchConditional %601 %594 %595
        %594 = OpLabel
        %604 = OpLoad %uint %word_0
               OpStore %param_14 %604
        %605 = OpFunctionCall %uint %read_lane_word_u1_ %param_14
               OpStore %own %605
               OpStore %member_1 %uint_0
               OpBranch %607
        %607 = OpLabel
               OpLoopMerge %609 %610 None
               OpBranch %611
        %611 = OpLabel
        %612 = OpLoad %uint %member_1
        %613 = OpULessThan %bool %612 %uint_4
               OpBranchConditional %613 %608 %609
        %608 = OpLabel
        %615 = OpLoad %uint %own
        %616 = OpLoad %uint %member_1
        %617 = OpGroupNonUniformQuadBroadcast %uint %uint_3 %615 %616
               OpStore %value %617
        %618 = OpLoad %bool %writer
        %619 = OpLoad %uint %record_1
        %620 = OpINotEqual %bool %619 %uint_4294967295
        %621 = OpLogicalAnd %bool %618 %620
               OpSelectionMerge %623 None
               OpBranchConditional %621 %622 %623
        %622 = OpLabel
        %624 = OpLoad %uint %record_1
        %625 = OpFunctionCall %uint %quad_words_
        %626 = OpIMul %uint %624 %625
        %627 = OpIAdd %uint %626 %uint_4
        %628 = OpLoad %uint %member_1
        %629 = OpAccessChain %_ptr_PushConstant_uint %transport %int_2
        %630 = OpLoad %uint %629
        %631 = OpIMul %uint %628 %630
        %632 = OpIAdd %uint %627 %631
        %633 = OpLoad %uint %word_0
        %634 = OpIAdd %uint %632 %633
        %635 = OpLoad %uint %value
        %636 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %634
               OpStore %636 %635
               OpBranch %623
        %623 = OpLabel
               OpBranch %610
        %610 = OpLabel
        %637 = OpLoad %uint %member_1
        %638 = OpIAdd %uint %637 %int_1
               OpStore %member_1 %638
               OpBranch %607
        %609 = OpLabel
               OpBranch %596
        %596 = OpLabel
        %639 = OpLoad %uint %word_0
        %640 = OpIAdd %uint %639 %int_1
               OpStore %word_0 %640
               OpBranch %593
        %595 = OpLabel
               OpReturn
               OpFunctionEnd
%quad_words_ = OpFunction %uint None %7
          %9 = OpLabel
         %57 = OpAccessChain %_ptr_PushConstant_uint %transport %int_2
         %58 = OpLoad %uint %57
         %59 = OpIMul %uint %uint_4 %58
         %60 = OpIAdd %uint %uint_4 %59
               OpReturnValue %60
               OpFunctionEnd
%wave_table_offset_ = OpFunction %uint None %7
         %11 = OpLabel
         %66 = OpAccessChain %_ptr_PushConstant_uint %transport %int_1
         %67 = OpLoad %uint %66
         %68 = OpIMul %uint %uint_2 %67
         %69 = OpIAdd %uint %uint_16 %68
         %71 = OpAccessChain %_ptr_PushConstant_uint %transport %int_4
         %72 = OpLoad %uint %71
         %73 = OpIMul %uint %uint_4 %72
         %74 = OpIAdd %uint %69 %73
               OpReturnValue %74
               OpFunctionEnd
%active_reference_offset_ = OpFunction %uint None %7
         %13 = OpLabel
         %78 = OpAccessChain %_ptr_PushConstant_uint %transport %int_6
         %79 = OpLoad %uint %78
         %80 = OpIMul %uint %uint_16 %79
               OpReturnValue %80
               OpFunctionEnd
%output_reference_offset_ = OpFunction %uint None %7
         %15 = OpLabel
         %83 = OpFunctionCall %uint %active_reference_offset_
         %85 = OpAccessChain %_ptr_PushConstant_uint %transport %int_5
         %86 = OpLoad %uint %85
         %87 = OpIAdd %uint %83 %86
               OpReturnValue %87
               OpFunctionEnd
<capture_reader>
%hash_key_u1_u1_ = OpFunction %uint None %21
  %primitive = OpFunctionParameter %_ptr_Function_uint
         %xy = OpFunctionParameter %_ptr_Function_uint
         %25 = OpLabel
          %h = OpVariable %_ptr_Function_uint Function
        %104 = OpLoad %uint %xy
        %106 = OpIMul %uint %104 %uint_2654435769
        %107 = OpLoad %uint %primitive
        %109 = OpIMul %uint %107 %uint_2246822507
        %110 = OpBitwiseXor %uint %106 %109
               OpStore %h %110
        %111 = OpLoad %uint %h
        %112 = OpShiftRightLogical %uint %111 %uint_16
        %113 = OpLoad %uint %h
        %114 = OpBitwiseXor %uint %113 %112
               OpStore %h %114
        %116 = OpLoad %uint %h
        %117 = OpIMul %uint %116 %uint_2146121005
               OpStore %h %117
        %118 = OpLoad %uint %h
        %119 = OpLoad %uint %h
        %121 = OpShiftRightLogical %uint %119 %uint_15
        %122 = OpBitwiseXor %uint %118 %121
               OpReturnValue %122
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
        %125 = OpLoad %uint %primitive_0
        %126 = OpAccessChain %_ptr_PushConstant_uint %transport %int_1
        %127 = OpLoad %uint %126
        %128 = OpUGreaterThanEqual %bool %125 %127
               OpSelectionMerge %130 None
               OpBranchConditional %128 %129 %130
        %129 = OpLabel
        %137 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %140 = OpAtomicOr %uint %137 %uint_1 %uint_0 %uint_4
               OpReturnValue %uint_4294967295
        %130 = OpLabel
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
               OpStore %slot_0 %168
        %170 = OpAccessChain %_ptr_PushConstant_uint %transport %int_1
        %171 = OpLoad %uint %170
        %172 = OpIMul %uint %171 %uint_2
        %173 = OpIAdd %uint %uint_16 %172
        %174 = OpLoad %uint %slot_0
        %175 = OpIMul %uint %174 %uint_4
        %176 = OpIAdd %uint %173 %175
               OpStore %base %176
        %177 = OpLoad %uint %base
        %178 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %177
        %179 = OpAtomicCompareExchange %uint %178 %uint_1 %uint_0 %uint_0 %uint_1 %uint_0
        %180 = OpIEqual %bool %179 %uint_0
               OpSelectionMerge %182 None
               OpBranchConditional %180 %181 %182
        %181 = OpLabel
        %183 = OpLoad %uint %slot_0
               OpReturnValue %183
        %182 = OpLabel
               OpBranch %153
        %153 = OpLabel
        %185 = OpLoad %uint %probe
        %186 = OpIAdd %uint %185 %int_1
               OpStore %probe %186
               OpBranch %150
        %152 = OpLabel
        %187 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %188 = OpAtomicOr %uint %187 %uint_1 %uint_0 %uint_2
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
        %192 = OpLoad %bool %claimed
        %193 = OpSelect %uint %192 %uint_1 %uint_0
               OpStore %claim %193
        %195 = OpLoad %uint %claim
        %196 = OpGroupNonUniformIAdd %uint %uint_3 Reduce %195
               OpStore %count %196
        %197 = OpLoad %uint %count
        %198 = OpIEqual %bool %197 %uint_0
               OpSelectionMerge %200 None
               OpBranchConditional %198 %199 %200
        %199 = OpLabel
               OpReturnValue %uint_4294967295
        %200 = OpLabel
        %203 = OpLoad %bool %claimed
        %206 = OpLoad %uint %gl_SubgroupInvocationID
        %207 = OpSelect %uint %203 %206 %uint_4294967295
        %208 = OpGroupNonUniformUMin %uint %uint_3 Reduce %207
               OpStore %leader %208
               OpStore %first_record %uint_0
        %210 = OpLoad %uint %gl_SubgroupInvocationID
        %211 = OpLoad %uint %leader
        %212 = OpIEqual %bool %210 %211
               OpSelectionMerge %214 None
               OpBranchConditional %212 %213 %214
        %213 = OpLabel
        %215 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_0
        %216 = OpLoad %uint %count
        %217 = OpAtomicIAdd %uint %215 %uint_1 %uint_0 %216
               OpStore %first_record %217
               OpBranch %214
        %214 = OpLabel
        %218 = OpLoad %uint %first_record
        %219 = OpGroupNonUniformUMax %uint %uint_3 Reduce %218
               OpStore %first_record %219
        %220 = OpLoad %bool %claimed
        %221 = OpLogicalNot %bool %220
               OpSelectionMerge %223 None
               OpBranchConditional %221 %222 %223
        %222 = OpLabel
               OpReturnValue %uint_4294967295
        %223 = OpLabel
        %226 = OpLoad %uint %first_record
        %227 = OpLoad %uint %claim
        %228 = OpGroupNonUniformIAdd %uint %uint_3 ExclusiveScan %227
        %229 = OpIAdd %uint %226 %228
               OpStore %record_0 %229
        %230 = OpLoad %uint %record_0
        %231 = OpAccessChain %_ptr_PushConstant_uint %transport %int_0
        %232 = OpLoad %uint %231
        %233 = OpULessThan %bool %230 %232
               OpSelectionMerge %235 None
               OpBranchConditional %233 %234 %235
        %234 = OpLabel
        %236 = OpLoad %uint %record_0
               OpReturnValue %236
        %235 = OpLabel
        %238 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %239 = OpAtomicOr %uint %238 %uint_1 %uint_0 %uint_1
               OpReturnValue %uint_4294967295
               OpFunctionEnd
%reserve_ordinal_b1_u1_ = OpFunction %uint None %36
   %reserved = OpFunctionParameter %_ptr_Function_bool
%primitive_1 = OpFunctionParameter %_ptr_Function_uint
         %40 = OpLabel
  %ordinal_0 = OpVariable %_ptr_Function_uint Function
    %pending = OpVariable %_ptr_Function_bool Function
   %leader_0 = OpVariable %_ptr_Function_uint Function
%leader_primitive = OpVariable %_ptr_Function_uint Function
     %member = OpVariable %_ptr_Function_uint Function
    %count_0 = OpVariable %_ptr_Function_uint Function
       %rank = OpVariable %_ptr_Function_uint Function
%first_ordinal = OpVariable %_ptr_Function_uint Function
               OpStore %ordinal_0 %uint_0
        %244 = OpLoad %bool %reserved
               OpStore %pending %244
               OpBranch %245
        %245 = OpLabel
               OpLoopMerge %247 %248 None
               OpBranch %249
        %249 = OpLabel
        %250 = OpLoad %bool %pending
        %251 = OpSelect %uint %250 %uint_1 %uint_0
        %252 = OpGroupNonUniformIAdd %uint %uint_3 Reduce %251
        %253 = OpINotEqual %bool %252 %uint_0
               OpBranchConditional %253 %246 %247
        %246 = OpLabel
        %255 = OpLoad %bool %pending
        %256 = OpLoad %uint %gl_SubgroupInvocationID
        %257 = OpSelect %uint %255 %256 %uint_4294967295
        %258 = OpGroupNonUniformUMin %uint %uint_3 Reduce %257
               OpStore %leader_0 %258
        %260 = OpLoad %uint %gl_SubgroupInvocationID
        %261 = OpLoad %uint %leader_0
        %262 = OpIEqual %bool %260 %261
        %263 = OpLoad %uint %primitive_1
        %264 = OpSelect %uint %262 %263 %uint_0
        %265 = OpGroupNonUniformUMax %uint %uint_3 Reduce %264
               OpStore %leader_primitive %265
        %267 = OpLoad %bool %pending
        %268 = OpLoad %uint %primitive_1
        %269 = OpLoad %uint %leader_primitive
        %270 = OpIEqual %bool %268 %269
        %271 = OpLogicalAnd %bool %267 %270
        %272 = OpSelect %uint %271 %uint_1 %uint_0
               OpStore %member %272
        %274 = OpLoad %uint %member
        %275 = OpGroupNonUniformIAdd %uint %uint_3 Reduce %274
               OpStore %count_0 %275
        %277 = OpLoad %uint %member
        %278 = OpGroupNonUniformIAdd %uint %uint_3 ExclusiveScan %277
               OpStore %rank %278
               OpStore %first_ordinal %uint_0
        %280 = OpLoad %uint %gl_SubgroupInvocationID
        %281 = OpLoad %uint %leader_0
        %282 = OpIEqual %bool %280 %281
               OpSelectionMerge %284 None
               OpBranchConditional %282 %283 %284
        %283 = OpLabel
        %286 = OpLoad %uint %leader_primitive
        %287 = OpIMul %uint %286 %uint_2
        %288 = OpIAdd %uint %uint_17 %287
        %289 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %288
        %290 = OpLoad %uint %count_0
        %291 = OpAtomicIAdd %uint %289 %uint_1 %uint_0 %290
               OpStore %first_ordinal %291
               OpBranch %284
        %284 = OpLabel
        %292 = OpLoad %uint %first_ordinal
        %293 = OpGroupNonUniformUMax %uint %uint_3 Reduce %292
               OpStore %first_ordinal %293
        %294 = OpLoad %uint %member
        %295 = OpINotEqual %bool %294 %uint_0
               OpSelectionMerge %297 None
               OpBranchConditional %295 %296 %297
        %296 = OpLabel
        %298 = OpLoad %uint %first_ordinal
        %299 = OpLoad %uint %rank
        %300 = OpIAdd %uint %298 %299
               OpStore %ordinal_0 %300
               OpStore %pending %false
               OpBranch %297
        %297 = OpLabel
               OpBranch %248
        %248 = OpLabel
               OpBranch %245
        %247 = OpLabel
        %302 = OpLoad %uint %ordinal_0
               OpReturnValue %302
               OpFunctionEnd
%store_quad_u1_u1_u1_u1_u1_u1_ = OpFunction %void None %41
       %slot = OpFunctionParameter %_ptr_Function_uint
     %record = OpFunctionParameter %_ptr_Function_uint
    %ordinal = OpFunctionParameter %_ptr_Function_uint
%primitive_2 = OpFunctionParameter %_ptr_Function_uint
       %xy_1 = OpFunctionParameter %_ptr_Function_uint
       %mask = OpFunctionParameter %_ptr_Function_uint
         %49 = OpLabel
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
        %306 = OpLoad %uint %record
        %307 = OpFunctionCall %uint %quad_words_
        %308 = OpIMul %uint %306 %307
               OpStore %header %308
        %313 = OpLoad %uint %header
        %314 = OpLoad %uint %mask
        %315 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %313
               OpStore %315 %314
        %316 = OpLoad %uint %header
        %317 = OpIAdd %uint %316 %uint_1
        %318 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %317
               OpStore %318 %uint_1
        %319 = OpLoad %uint %header
        %320 = OpIAdd %uint %319 %uint_2
        %321 = OpLoad %uint %primitive_2
        %322 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %320
               OpStore %322 %321
        %323 = OpLoad %uint %header
        %324 = OpIAdd %uint %323 %uint_3
        %325 = OpLoad %uint %ordinal
        %326 = OpAccessChain %_ptr_StorageBuffer_uint %record_buffer %int_0 %324
               OpStore %326 %325
        %328 = OpLoad %uint %primitive_2
        %329 = OpShiftLeftLogical %uint %328 %uint_16
        %330 = OpLoad %uint %ordinal
        %331 = OpUDiv %uint %330 %uint_16
        %332 = OpBitwiseOr %uint %329 %331
        %333 = OpIAdd %uint %332 %uint_1
               OpStore %key %333
        %335 = OpLoad %uint %ordinal
        %336 = OpUDiv %uint %335 %uint_16
        %338 = OpLoad %uint %primitive_2
               OpStore %param_1 %338
               OpStore %param_2 %336
        %340 = OpFunctionCall %uint %hash_key_u1_u1_ %param_1 %param_2
               OpStore %wave_hash %340
               OpStore %found %false
               OpStore %attempt %uint_0
               OpBranch %343
        %343 = OpLabel
               OpLoopMerge %345 %346 None
               OpBranch %347
        %347 = OpLabel
        %348 = OpLoad %uint %attempt
        %349 = OpAccessChain %_ptr_PushConstant_uint %transport %int_6
        %350 = OpLoad %uint %349
        %351 = OpExtInst %uint %1 UMin %350 %uint_128
        %352 = OpULessThan %bool %348 %351
               OpBranchConditional %352 %344 %345
        %344 = OpLabel
        %354 = OpLoad %uint %wave_hash
        %355 = OpLoad %uint %attempt
        %356 = OpIAdd %uint %354 %355
        %357 = OpAccessChain %_ptr_PushConstant_uint %transport %int_6
        %358 = OpLoad %uint %357
        %359 = OpISub %uint %358 %uint_1
        %360 = OpBitwiseAnd %uint %356 %359
               OpStore %wave_slot %360
        %362 = OpFunctionCall %uint %wave_table_offset_
        %363 = OpLoad %uint %wave_slot
        %364 = OpIAdd %uint %362 %363
        %365 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %364
        %366 = OpLoad %uint %key
        %367 = OpAtomicCompareExchange %uint %365 %uint_1 %uint_0 %uint_0 %366 %uint_0
               OpStore %previous %367
        %368 = OpLoad %uint %previous
        %369 = OpINotEqual %bool %368 %uint_0
        %370 = OpLoad %uint %previous
        %371 = OpLoad %uint %key
        %372 = OpINotEqual %bool %370 %371
        %373 = OpLogicalAnd %bool %369 %372
               OpSelectionMerge %375 None
               OpBranchConditional %373 %374 %375
        %374 = OpLabel
               OpBranch %346
        %375 = OpLabel
        %381 = OpLoad %uint %wave_slot
        %382 = OpIMul %uint %381 %uint_16
        %383 = OpLoad %uint %ordinal
        %384 = OpBitwiseAnd %uint %383 %uint_15
        %385 = OpIAdd %uint %382 %384
        %386 = OpLoad %uint %record
        %387 = OpAccessChain %_ptr_StorageBuffer_uint %reference_buffer %int_0 %385
               OpStore %387 %386
               OpStore %found %true
               OpBranch %345
        %346 = OpLabel
        %390 = OpLoad %uint %attempt
        %391 = OpIAdd %uint %390 %int_1
               OpStore %attempt %391
               OpBranch %343
        %345 = OpLabel
        %392 = OpLoad %bool %found
        %393 = OpLogicalNot %bool %392
               OpSelectionMerge %395 None
               OpBranchConditional %393 %394 %395
        %394 = OpLabel
        %396 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %int_1
        %398 = OpAtomicOr %uint %396 %uint_1 %uint_0 %uint_64
               OpBranch %395
        %395 = OpLabel
        %400 = OpAccessChain %_ptr_PushConstant_uint %transport %int_1
        %401 = OpLoad %uint %400
        %402 = OpIMul %uint %401 %uint_2
        %403 = OpIAdd %uint %uint_16 %402
        %404 = OpLoad %uint %slot
        %405 = OpIMul %uint %404 %uint_4
        %406 = OpIAdd %uint %403 %405
               OpStore %base_0 %406
        %407 = OpLoad %uint %base_0
        %408 = OpIAdd %uint %407 %uint_1
        %409 = OpLoad %uint %primitive_2
        %410 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %408
               OpStore %410 %409
        %411 = OpLoad %uint %base_0
        %412 = OpIAdd %uint %411 %uint_2
        %413 = OpLoad %uint %xy_1
        %414 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %412
               OpStore %414 %413
        %415 = OpLoad %uint %base_0
        %416 = OpIAdd %uint %415 %uint_3
        %417 = OpLoad %uint %record
        %418 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %416
               OpStore %418 %417
        %419 = OpLoad %uint %base_0
        %420 = OpAccessChain %_ptr_StorageBuffer_uint %control_buffer %int_0 %419
        %421 = OpAtomicExchange %uint %420 %uint_1 %uint_0 %uint_2
               OpReturn
               OpFunctionEnd
)CORE";
