#pragma once
#include <ntifs.h>
#include <ntddk.h>
#include <intrin.h>

// ==========================================
// Full AMD SVM (Secure Virtual Machine) Architecture
// ==========================================

#define MSR_EFER 0xC0000080
#define MSR_VM_CR 0xC0010114
#define MSR_VM_HSAVE_PA 0xC0010117

#define EFER_SVME (1ULL << 12)
#define CPUID_FEAT_ECX_SVM (1 << 2)
#define SVM_ALIGNMENT 4096

#pragma pack(push, 1)

// Segment Attribute Struct for VMCB State
typedef union _SEGMENT_ATTRIBUTES {
    USHORT AsUInt16;
    struct {
        USHORT Type : 4;
        USHORT System : 1;
        USHORT Dpl : 2;
        USHORT Present : 1;
        USHORT Available : 1;
        USHORT LongMode : 1;
        USHORT DefaultOperandSize : 1;
        USHORT Granularity : 1;
        USHORT Reserved : 4;
    } Fields;
} SEGMENT_ATTRIBUTES;

// VMCB Segment Register Format
typedef struct _VMCB_SEGMENT_REGISTER {
    USHORT Selector;
    SEGMENT_ATTRIBUTES Attributes;
    ULONG32 Limit;
    ULONG64 Base;
} VMCB_SEGMENT_REGISTER, *PVMCB_SEGMENT_REGISTER;

// VMCB Control Area (Exact 1024 bytes)
typedef struct _VMCB_CONTROL_AREA {
    ULONG16 InterceptCrRead;    // 0x00
    ULONG16 InterceptCrWrite;   // 0x02
    ULONG16 InterceptDrRead;    // 0x04
    ULONG16 InterceptDrWrite;   // 0x06
    ULONG32 InterceptException; // 0x08
    ULONG32 InterceptMisc1;     // 0x0C
    ULONG32 InterceptMisc2;     // 0x10
    ULONG32 InterceptMisc3;     // 0x14
    UCHAR Reserved1[40];        // 0x18
    ULONG64 IOPMBasePA;         // 0x40
    ULONG64 MSRPMBasePA;        // 0x48
    ULONG64 TSC_Offset;         // 0x50
    ULONG32 GuestASID;          // 0x58
    UCHAR TLB_Control;          // 0x5C
    UCHAR Reserved2[3];         // 0x5D
    ULONG32 V_TPR;              // 0x60
    ULONG32 Reserved3;          // 0x64
    ULONG64 InterruptShadow;    // 0x68
    ULONG64 ExitCode;           // 0x70
    ULONG64 ExitInfo1;          // 0x78
    ULONG64 ExitInfo2;          // 0x80
    ULONG32 ExitIntInfo;        // 0x88
    ULONG32 Reserved4;          // 0x8C
    ULONG64 NpEnable;           // 0x90
    ULONG64 AVIC_APIC_BAR;      // 0x98
    ULONG64 GuestPhysicalPageHit; // 0xA0
    ULONG64 Reserved5;          // 0xA8
    ULONG64 N_CR3;              // 0xB0
    ULONG64 Reserved6;          // 0xB8
    ULONG32 Vmcb_Clean;         // 0xC0
    ULONG32 Reserved7;          // 0xC4
    ULONG64 nRIP;               // 0xC8
    UCHAR NumberOfBytesFetched; // 0xD0
    UCHAR GuestInstructionBytes[15]; // 0xD1
    ULONG64 APIC_Backing_Page_Pointer; // 0xE0
    UCHAR Reserved8[792];       // Pad to 1024
} VMCB_CONTROL_AREA, *PVMCB_CONTROL_AREA;

// VMCB State Save Area (Offsets strictly bound to AMD documentation)
typedef struct _VMCB_STATE_SAVE_AREA {
    VMCB_SEGMENT_REGISTER Es; 
    VMCB_SEGMENT_REGISTER Cs; 
    VMCB_SEGMENT_REGISTER Ss; 
    VMCB_SEGMENT_REGISTER Ds; 
    VMCB_SEGMENT_REGISTER Fs; 
    VMCB_SEGMENT_REGISTER Gs; 
    VMCB_SEGMENT_REGISTER Gdtr; 
    VMCB_SEGMENT_REGISTER Ldtr; 
    VMCB_SEGMENT_REGISTER Idtr; 
    VMCB_SEGMENT_REGISTER Tr;   
    UCHAR Reserved1[43];        
    UCHAR Cpl;                  
    ULONG32 Reserved2;          
    ULONG64 Efer;               
    ULONG64 Reserved3[14];      
    ULONG64 Cr4;                
    ULONG64 Cr3;                
    ULONG64 Cr0;                
    ULONG64 Dr7;                
    ULONG64 Dr6;                
    ULONG64 Rflags;             
    ULONG64 Rip;                
    ULONG64 Reserved4[11];      
    ULONG64 Rsp;                
    ULONG64 Reserved5[3];       // 0x1E0 to 0x1F8 (24 bytes)
    ULONG64 Rax;                // 0x1F8
    ULONG64 Star;               
    ULONG64 Lstar;              
    ULONG64 Cstar;              
    ULONG64 Sfmask;             
    ULONG64 KernelGsBase;       
    ULONG64 SysenterCs;         
    ULONG64 SysenterEsp;        
    ULONG64 SysenterEip;        
    ULONG64 Cr2;                
    UCHAR Reserved6[32];        
    ULONG64 G_Pat;              
    ULONG64 DbgCtl;             
    ULONG64 Br_From;            
    ULONG64 Br_To;              
    ULONG64 LastExcpFrom;       
    ULONG64 LastExcpTo;         
    UCHAR Pad[2408];            // Total structure padding to 4096 (adjusted for Reserved5[3])
} VMCB_STATE_SAVE_AREA, *PVMCB_STATE_SAVE_AREA;

typedef struct _VMCB {
    VMCB_CONTROL_AREA ControlArea;
    VMCB_STATE_SAVE_AREA StateSaveArea;
} VMCB, *PVMCB;

#pragma pack(pop)

// Nested Page Table (NPT) Structures for Memory Cloaking
typedef union _NPT_ENTRY {
    ULONG64 AsUInt64;
    struct {
        ULONG64 Valid : 1;
        ULONG64 Write : 1;
        ULONG64 User : 1;
        ULONG64 Pwt : 1;
        ULONG64 Pcd : 1;
        ULONG64 Accessed : 1;
        ULONG64 Dirty : 1;
        ULONG64 PageSize : 1; // 1 for 2MB pages, 0 for 4KB
        ULONG64 Global : 1;
        ULONG64 Avl : 3;
        ULONG64 PageFrameNumber : 40;
        ULONG64 Reserved : 11;
        ULONG64 NoExecute : 1;
    } Fields;
} NPT_ENTRY, *PNPT_ENTRY;

// Core Hypervisor State
typedef struct _VIRTUAL_MACHINE_STATE {
    PVOID HostStateArea;
    PHYSICAL_ADDRESS HostStatePhysical;
    PVMCB Vmcb;
    PHYSICAL_ADDRESS VmcbPhysical;
    PNPT_ENTRY NptPml4;
    PHYSICAL_ADDRESS NptPhysical;
} VIRTUAL_MACHINE_STATE, *PVIRTUAL_MACHINE_STATE;

// VM-Exit Codes
#define VMEXIT_CPUID 0x72
#define VMEXIT_VMMCALL 0x81

// Guest General Purpose Registers (Matches assembly stack frame)
typedef struct _GUEST_REGISTERS {
    ULONG64 R15;
    ULONG64 R14;
    ULONG64 R13;
    ULONG64 R12;
    ULONG64 R11;
    ULONG64 R10;
    ULONG64 R9;
    ULONG64 R8;
    ULONG64 Rdi;
    ULONG64 Rsi;
    ULONG64 Rbp;
    ULONG64 Rdx;
    ULONG64 Rcx;
    ULONG64 Rbx;
    // RAX, RIP, RSP, RFLAGS are saved natively in the VMCB
} GUEST_REGISTERS, *PGUEST_REGISTERS;

// Global array for Multicore state
extern PVIRTUAL_MACHINE_STATE g_GuestStates[256];

// Core initialization and exit handler
void VmExitHandler(PGUEST_REGISTERS GuestRegs, PHYSICAL_ADDRESS VmcbPhysical);
BOOLEAN CheckSvmSupport();
NTSTATUS AllocateSvmMemory(ULONG processorId);
void EnterSvmRootMode(PVIRTUAL_MACHINE_STATE state);
void SetupVmcb(PVIRTUAL_MACHINE_STATE state);
void AsmVirtualizeCore(PHYSICAL_ADDRESS VmcbPhysical, PVOID StateSaveArea);
