/* airspyhf.dll falsa: tom complexo em +10 kHz, amplitude 0.01, em tempo real */
#include <windows.h>
#include <stdint.h>
#include <math.h>
typedef struct { float re, im; } cf;
typedef struct { void*device; void*ctx; cf*samples; int sample_count; uint64_t dropped; } tr;
typedef int (__cdecl *cb_t)(tr*);
static uint32_t rate=768000; static cb_t cb; static volatile int run; static HANDLE th; static uint32_t freq;
static DWORD WINAPI thr(LPVOID a){ static cf buf[8192]; double ph=0; int n=1024; LARGE_INTEGER f,t0,t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0); double sent=0;
 while(run){ for(int i=0;i<n;i++){ buf[i].re=0.01f*cos(ph); buf[i].im=0.01f*sin(ph); ph+=2*M_PI*10000.0/rate; if(ph>2*M_PI)ph-=2*M_PI;}
  tr x={0,0,buf,n,0}; cb(&x); sent+=n; for(;;){QueryPerformanceCounter(&t); if((double)(t.QuadPart-t0.QuadPart)/f.QuadPart>=sent/rate)break; Sleep(1);} } return 0; }
__declspec(dllexport) int airspyhf_open(void**d){*d=(void*)1;return 0;}
__declspec(dllexport) int airspyhf_open_sn(void**d,uint64_t s){*d=(void*)1;return 0;}
__declspec(dllexport) int airspyhf_close(void*d){return 0;}
__declspec(dllexport) int airspyhf_start(void*d,cb_t c,void*x){cb=c;run=1;th=CreateThread(0,0,thr,0,0,0);return 0;}
__declspec(dllexport) int airspyhf_stop(void*d){run=0;WaitForSingleObject(th,INFINITE);return 0;}
__declspec(dllexport) int airspyhf_set_freq(void*d,uint32_t f){freq=f;return 0;}
__declspec(dllexport) int airspyhf_get_samplerates(void*d,uint32_t*b,uint32_t n){ static uint32_t r[]={912000,768000,456000,384000,256000,192000}; if(n==0){*b=6;return 0;} for(uint32_t i=0;i<n&&i<6;i++)b[i]=r[i]; return 0;}
__declspec(dllexport) int airspyhf_set_samplerate(void*d,uint32_t r){rate=r;return 0;}
__declspec(dllexport) int airspyhf_set_hf_agc(void*d,uint8_t f){return 0;}
__declspec(dllexport) int airspyhf_set_hf_agc_threshold(void*d,uint8_t f){return 0;}
__declspec(dllexport) int airspyhf_set_hf_att(void*d,uint8_t f){return 0;}
__declspec(dllexport) int airspyhf_set_hf_lna(void*d,uint8_t f){return 0;}
