/* faz o papel do Skimmer: carrega a DLL, mede blocos/s, frequencia e nivel */
#include <windows.h>
#include <stdio.h>
#include <math.h>
#include <stdint.h>
typedef struct { float Re, Im; } C;
typedef struct { char*name; int n; float r[3]; } Info;
typedef struct { int h,rc,rate; BOOL ll; void *iq,*au,*sb,*lp,*er; } Set;
static volatile long blocks; static int blk; static double fsum,asum; static long fn; static C last; static int have; static int aligned=1, zeros_ok=1;
static void __stdcall iq(int h, C**d){ if(h!=1234)printf("bad handle\n"); if(((uintptr_t)d[0])&15)aligned=0;
 for(int k=1;k<8;k++) if(!d[k]||d[k][0].Re!=0) zeros_ok=0;
 for(int i=0;i<blk;i++){ C s=d[0][i]; if(have&&blocks>20){ double dp=atan2(s.Im*last.Re-s.Re*last.Im, s.Re*last.Re+s.Im*last.Im); fsum+=dp; asum+=sqrt(s.Re*s.Re+s.Im*s.Im); fn++; } last=s; have=1; } InterlockedIncrement(&blocks); }
static void __stdcall er(int h,char*t){ printf("ErrorProc: %s\n",t); }
int main(){ HMODULE m=LoadLibraryA("Qs1rIntf.dll"); if(!m){printf("load fail %lu\n",GetLastError());return 1;}
 void (__stdcall *gi)(Info*)=(void*)GetProcAddress(m,"GetSdrInfo"); void (__stdcall *st)(Set*)=(void*)GetProcAddress(m,"StartRx");
 void (__stdcall *sp)(void)=(void*)GetProcAddress(m,"StopRx"); void (__stdcall *sf)(int,int)=(void*)GetProcAddress(m,"SetRxFrequency");
 void *cb=GetProcAddress(m,"SetCtrlBits"),*rp=GetProcAddress(m,"ReadPort"); printf("exports %d\n",gi&&st&&sp&&sf&&cb&&rp);
 Info i; gi(&i); printf("%s rx=%d rates %.0f %.0f %.0f\n",i.name,i.n,i.r[0],i.r[1],i.r[2]);
 for(int rid=0;rid<3;rid++){ int sr=48000<<rid; blk=sr/93.75; blocks=0;fsum=asum=0;fn=0;have=0;
  Set s={1234,1,rid|0x5500,0,iq,0,0,0,er}; sf(7030000,0); st(&s); DWORD t0=GetTickCount(); Sleep(4000); long b=blocks; DWORD dt=GetTickCount()-t0; sp(); DWORD t1=GetTickCount();
  printf("rate %d: %.2f blocos/s, tom %.1f Hz, amplitude %.0f (esperado %.0f), stop %lums, align %d zeros %d\n",sr,b*1000.0/dt,fsum/fn*sr/(2*M_PI),asum/fn,0.01*2147483648.0,GetTickCount()-t1,aligned,zeros_ok); }
 return 0; }
