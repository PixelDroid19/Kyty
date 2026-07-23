#ifndef EMULATOR_INCLUDE_EMULATOR_LOADER_JIT_H_
#define EMULATOR_INCLUDE_EMULATOR_LOADER_JIT_H_

#include "Emulator/Common.h"

#ifdef KYTY_EMU_ENABLED

namespace Kyty::Loader::Jit {

#pragma pack(1)

struct JmpWithIndex
{
	void SetIndex(uint32_t index) { *reinterpret_cast<uint32_t*>(&code[1]) = index; }

	void SetFunc(void* handler)
	{
		auto func_addr = reinterpret_cast<int64_t>(handler);
		auto rip_addr  = reinterpret_cast<int64_t>(&code[10]);
		auto offset64  = func_addr - rip_addr;
		auto offset32  = static_cast<uint32_t>(static_cast<uint64_t>(offset64) & 0xffffffffu);

		*reinterpret_cast<uint32_t*>(&code[6]) = offset32;
	}

	static constexpr uint64_t GetSize() { return 16; }

	// 68 00 00 00 00          push     <index>
	// E9 E0 FF FF FF          jmp      <handler>
	uint8_t code[16] = {0x68, 0x00, 0x00, 0x00, 0x00, 0xE9, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90};
};

struct CallPlt
{
	using resolver_t = KYTY_SYSV_ABI uint64_t (*)(void* program, uint64_t relocation_index);

	explicit CallPlt(uint32_t table_size)
	{
		for (uint32_t index = 0; index < table_size; index++)
		{
			auto* c = new (&code[kResolverSize] + JmpWithIndex::GetSize() * index) JmpWithIndex;
			c->SetIndex(index);
			c->SetFunc(this);
		}
	}

	void SetPltGot(uint64_t vaddr) { *reinterpret_cast<uint64_t*>(&code[2]) = vaddr; }
	void SetResolver(resolver_t resolver) { *reinterpret_cast<resolver_t*>(&code[87]) = resolver; }

	uint64_t GetAddr(uint32_t index) { return reinterpret_cast<uint64_t>(&code[kResolverSize] + JmpWithIndex::GetSize() * index); }

	static constexpr uint64_t GetSize(uint32_t table_size) { return kResolverSize + JmpWithIndex::GetSize() * table_size; }

	static constexpr uint64_t kResolverSize = 192;

	// The initial PLT hit receives the relocation index on top of the guest
	// return address. Preserve every volatile integer and vector argument across
	// the host resolver, then discard only that index before tail-jumping to the
	// resolved guest export.
	//
	// 0:    movabs r11, pltgot
	// 10:   push rax, rdi, rsi, rdx, rcx, r8, r9
	// 19:   sub rsp, 0x88
	// 26:   save xmm0..xmm7
	// 73:   rdi = [r11 + 8] (Program*), rsi = [rsp + 0xc0] (relocation index)
	// 85:   call resolver
	// 98:   r11 = resolved target; restore guest argument state
	// 164:  discard relocation index and jump r11
	uint8_t code[kResolverSize] = {
	    0x49, 0xBB, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
	    0x50, 0x57, 0x56, 0x52, 0x51, 0x41, 0x50, 0x41, 0x51,
	    0x48, 0x81, 0xEC, 0x88, 0x00, 0x00, 0x00,
	    0xF3, 0x0F, 0x7F, 0x04, 0x24,
	    0xF3, 0x0F, 0x7F, 0x4C, 0x24, 0x10,
	    0xF3, 0x0F, 0x7F, 0x54, 0x24, 0x20,
	    0xF3, 0x0F, 0x7F, 0x5C, 0x24, 0x30,
	    0xF3, 0x0F, 0x7F, 0x64, 0x24, 0x40,
	    0xF3, 0x0F, 0x7F, 0x6C, 0x24, 0x50,
	    0xF3, 0x0F, 0x7F, 0x74, 0x24, 0x60,
	    0xF3, 0x0F, 0x7F, 0x7C, 0x24, 0x70,
	    0x49, 0x8B, 0x7B, 0x08,
	    0x48, 0x8B, 0xB4, 0x24, 0xC0, 0x00, 0x00, 0x00,
	    0x49, 0xBA, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
	    0x41, 0xFF, 0xD2,
	    0x49, 0x89, 0xC3,
	    0xF3, 0x0F, 0x6F, 0x04, 0x24,
	    0xF3, 0x0F, 0x6F, 0x4C, 0x24, 0x10,
	    0xF3, 0x0F, 0x6F, 0x54, 0x24, 0x20,
	    0xF3, 0x0F, 0x6F, 0x5C, 0x24, 0x30,
	    0xF3, 0x0F, 0x6F, 0x64, 0x24, 0x40,
	    0xF3, 0x0F, 0x6F, 0x6C, 0x24, 0x50,
	    0xF3, 0x0F, 0x6F, 0x74, 0x24, 0x60,
	    0xF3, 0x0F, 0x6F, 0x7C, 0x24, 0x70,
	    0x48, 0x81, 0xC4, 0x88, 0x00, 0x00, 0x00,
	    0x41, 0x59, 0x41, 0x58, 0x59, 0x5A, 0x5E, 0x5F, 0x58,
	    0x48, 0x83, 0xC4, 0x08,
	    0x41, 0xFF, 0xE3,
	};
};

struct JmpRax
{
	template <class Handler>
	void SetFunc(Handler func)
	{
		*reinterpret_cast<Handler*>(&code[2]) = func;
	}

	// mov rax, 0x1122334455667788
	// jmp rax
	uint8_t code[16] = {0x48, 0xB8, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0xFF, 0xE0};
};

struct Call9
{
	template <class Handler>
	void SetFunc(Handler func)
	{
		auto func_addr = reinterpret_cast<int64_t>(reinterpret_cast<void*>(func));
		auto rip_addr  = reinterpret_cast<int64_t>(&code[5]);
		auto offset64  = func_addr - rip_addr;
		auto offset32  = static_cast<uint32_t>(static_cast<uint64_t>(offset64) & 0xffffffffu);

		*reinterpret_cast<uint32_t*>(&code[1]) = offset32;
	}

	static uint64_t GetSize() { return 9; }

	// call func
	// mov rax,rax
	// nop
	uint8_t code[9] = {0xE8, 0x00, 0x00, 0x00, 0x00, 0x48, 0x89, 0xC0, 0x90};
};

struct SafeCall
{
	using func_t = KYTY_MS_ABI uint8_t* (*)();

	void SetFunc(func_t func) { *reinterpret_cast<func_t*>(&code[0x23]) = func; }
	void SetRegSaveArea(uint8_t* area) { *reinterpret_cast<uint8_t**>(&code[0x19]) = area; }
	void SetLockVar(uint8_t* lock_var) { *reinterpret_cast<uint8_t**>(&code[0x0f]) = lock_var; }

	static uint64_t GetSize() { return 0x1000; }

	uint8_t code[0x78] = {
	    /*00*/ 0x51,                                                       // push   rcx  /* Save general purpose registers */
	    /*01*/ 0x52,                                                       // push   rdx
	    /*02*/ 0x41, 0x50,                                                 // push   r8
	    /*04*/ 0x41, 0x51,                                                 // push   r9
	    /*06*/ 0x41, 0x52,                                                 // push   r10
	    /*08*/ 0x41, 0x53,                                                 // push   r11
	    /*0a*/ 0x57,                                                       // push   rdi
	    /*0b*/ 0x56,                                                       // push   rsi
	    /*0c*/ 0x53,                                                       // push   rbx
	    /*0d*/ 0x48, 0xbf, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, // movabs rdi,0x1122334455667788
	    /*17*/ 0x48, 0xbe, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, // movabs rsi,0x1122334455667788
	    /*21*/ 0x48, 0xb9, 0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, // movabs rcx,0x1122334455667788
	    /*2b*/ 0xb0, 0x01,                                                 // mov    al,0x1                  /* Lock */
	    /*2d*/ 0x86, 0x07,                                                 // xchg   BYTE PTR [rdi],al
	    /*2f*/ 0x84, 0xc0,                                                 // test   al,al
	    /*31*/ 0x75, 0xf8,                                                 // jne    2b <LOCK>
	    /*33*/ 0xb8, 0xff, 0xff, 0xff, 0xff,                               // mov    eax,0xffffffff
	    /*38*/ 0xba, 0xff, 0xff, 0xff, 0xff,                               // mov    edx,0xffffffff
	    /*3d*/ 0x0f, 0xae, 0x26,                                           // xsave  [rsi]      /* Save float registers */
	    /*40*/ 0x48, 0x89, 0xe3,                                           // mov    rbx,rsp
	    /*43*/ 0x48, 0x83, 0xe4, 0xf0,                                     // and    rsp,0xfffffffffffffff0
	    /*47*/ 0x48, 0x83, 0xec, 0x20,                                     // sub    rsp,0x20   /* MS ABI shadow space */
	    /*4b*/ 0xff, 0xd1,                                                 // call   rcx
	    /*4d*/ 0x48, 0x83, 0xc4, 0x20,                                     // add    rsp,0x20
	    /*51*/ 0x48, 0x89, 0xdc,                                           // mov    rsp,rbx
	    /*54*/ 0x48, 0x89, 0xc1,                                           // mov    rcx,rax
	    /*57*/ 0xb8, 0xff, 0xff, 0xff, 0xff,                               // mov    eax,0xffffffff
	    /*5c*/ 0xba, 0xff, 0xff, 0xff, 0xff,                               // mov    edx,0xffffffff
	    /*61*/ 0x0f, 0xae, 0x2e,                                           // xrstor [rsi]      /* Restore float registers */
	    /*64*/ 0x48, 0x89, 0xc8,                                           // mov    rax,rcx
	    /*67*/ 0xc6, 0x07, 0x00,                                           // mov    BYTE PTR [rdi],0x0       /* Unlock */
	    /*6a*/ 0x5b,                                                       // pop    rbx      /* Restore general purpose registers */
	    /*6b*/ 0x5e,                                                       // pop    rsi
	    /*6c*/ 0x5f,                                                       // pop    rdi
	    /*6d*/ 0x41, 0x5b,                                                 // pop    r11
	    /*6f*/ 0x41, 0x5a,                                                 // pop    r10
	    /*71*/ 0x41, 0x59,                                                 // pop    r9
	    /*73*/ 0x41, 0x58,                                                 // pop    r8
	    /*75*/ 0x5a,                                                       // pop    rdx
	    /*76*/ 0x59,                                                       // pop    rcx
	    /*77*/ 0xc3,                                                       // ret

	};
};

#pragma pack()

} // namespace Kyty::Loader::Jit

#endif // KYTY_EMU_ENABLED

#endif /* EMULATOR_INCLUDE_EMULATOR_LOADER_JIT_H_ */
