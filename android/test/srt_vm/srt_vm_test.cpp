#define XBYAK64 1
#include <xbyak/xbyak.h>
#include <signal.h>
#include <cstdio>
#include <cstring>
#include "core/signals.h"
#include "shader_recompiler/ir/passes/srt.h"
using namespace Xbyak::util;
static void seg(int, siginfo_t* si, void* uc){ if(!Core::Signals::Instance()->h(uc, si->si_addr)) _exit(99); }
int main(){
  struct sigaction sa{}; sa.sa_sigaction=seg; sa.sa_flags=SA_SIGINFO; sigaction(SIGSEGV,&sa,nullptr);
  static u32 table[64]; for(int i=0;i<64;i++) table[i]=1000+i;
  u32 ud[16]={}; u64 tp=(u64)table; memcpy(&ud[2],&tp,8); ud[5]=3; u64 bad=0x10; memcpy(&ud[8],&bad,8);
  Xbyak::CodeGenerator c(4096, Xbyak::DontSetProtectRWE);
  // PushPtr(imm 2): rdi = [rdi+8] & 48 bits
  c.push(rdi); c.mov(rdi, ptr[rdi+8]); c.mov(r10,0xFFFFFFFFFFFFULL); c.and_(rdi,r10);
  c.mov(r10d, ptr[rdi+4*4]); c.mov(ptr[rsi+16*4], r10d);           // dst16 = table[4]
  // dynamic: r10d = ud[5]*2+1 (via flat dst copy) then umin with 6, shl 2
  c.mov(r10d, ptr[rsi+5*4]); c.imul(r10d,r10d,2); c.push(r10); c.mov(r10d,1); c.add(r10d,dword[rsp]); c.add(rsp,8);
  c.mov(ecx,6); c.cmp(r10d,ecx); c.cmova(r10d,ecx);                 // min(7,6)=6
  c.shl(r10d,2); c.mov(r10d,r10d); c.mov(r10d,dword[rdi+r10]); c.mov(ptr[rsi+17*4], r10d); // table[6]
  // shr by cl, bitfield
  c.mov(r10d,0xF0); c.mov(ecx,4); c.shr(r10d,cl); c.not_(r10d); c.mov(edx,1); c.shl(edx,cl); c.dec(edx); c.and_(r10d,edx); c.mov(ptr[rsi+18*4], r10d); // (~0xF)&0xF=0
  c.mov(r10d,9); c.neg(r10d); c.add(r10d,20); c.xor_(r10d,1); c.or_(r10d,0x100); c.mov(ptr[rsi+19*4], r10d); // (11^1)|0x100=0x10a
  c.pop(rdi);
  // faulting pointer
  c.push(rdi); c.mov(rdi, ptr[rdi+32]); c.mov(r10,0xFFFFFFFFFFFFULL); c.and_(rdi,r10); c.mov(r10d, ptr[rdi]); c.mov(ptr[rsi+20*4], r10d); c.pop(rdi);
  c.ret();
  u32 dst[24]; memset(dst,0xAA,sizeof dst); memcpy(dst,ud,64);
  Shader::BbSrtVmRun((Shader::PFN_SrtWalker)c.getCode(), ud, dst);
  printf("%u %u %u %#x %u\n", dst[16],dst[17],dst[18],dst[19],dst[20]);
  return !(dst[16]==1004 && dst[17]==1006 && dst[18]==0 && dst[19]==0x10a && dst[20]==0);
}
