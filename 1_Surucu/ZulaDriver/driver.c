#include <ntifs.h>
#include <ntddk.h>
#include <windef.h>
#include <intrin.h>
#include "driver.h"

PVIRTUAL_MACHINE_STATE g_GuestStates[256] = {0};

NTKERNELAPI PVOID PsGetProcessSectionBaseAddress(PEPROCESS Process);
#define CMD_GET_BASE 3

extern void __stdcall AsmGetSegmentRegisters(PVOID State);
extern void __stdcall AsmVirtualizeCore(PHYSICAL_ADDRESS VmcbPhysical, PVOID StateSaveArea);

BOOLEAN CheckSvmSupport() {
    int cpuInfo[4] = {0};
    __cpuid(cpuInfo, 0x80000001);
    
    if ((cpuInfo[2] & CPUID_FEAT_ECX_SVM) == 0) return FALSE;
    
    ULONG64 vmCr = __readmsr(MSR_VM_CR);
    if ((vmCr & (1 << 4)) != 0) return FALSE; // SVMDIS bit is set
    
    return TRUE;
}

NTSTATUS BuildNestedPageTables(PVIRTUAL_MACHINE_STATE state) {
    PHYSICAL_ADDRESS maxAddr;
    maxAddr.QuadPart = MAXULONG64;

    state->NptPml4 = (PNPT_ENTRY)MmAllocateContiguousMemory(SVM_ALIGNMENT, maxAddr);
    if (!state->NptPml4) return STATUS_INSUFFICIENT_RESOURCES;
    RtlSecureZeroMemory(state->NptPml4, SVM_ALIGNMENT);
    state->NptPhysical = MmGetPhysicalAddress(state->NptPml4);

    PNPT_ENTRY pdpt = (PNPT_ENTRY)MmAllocateContiguousMemory(SVM_ALIGNMENT, maxAddr);
    if (!pdpt) return STATUS_INSUFFICIENT_RESOURCES;
    RtlSecureZeroMemory(pdpt, SVM_ALIGNMENT);
    
    state->NptPml4[0].AsUInt64 = MmGetPhysicalAddress(pdpt).QuadPart | 0x7;

    for (int i = 0; i < 512; i++) {
        PNPT_ENTRY pd = (PNPT_ENTRY)MmAllocateContiguousMemory(SVM_ALIGNMENT, maxAddr);
        if (!pd) return STATUS_INSUFFICIENT_RESOURCES;
        RtlSecureZeroMemory(pd, SVM_ALIGNMENT);
        
        pdpt[i].AsUInt64 = MmGetPhysicalAddress(pd).QuadPart | 0x7;
        
        for (int j = 0; j < 512; j++) {
            ULONG64 physAddr = (i * 512ULL + j) << 21; 
            pd[j].AsUInt64 = physAddr | 0x87; 
        }
    }

    return STATUS_SUCCESS;
}

void CloakHypervisor() {
    PHYSICAL_ADDRESS maxAddr;
    maxAddr.QuadPart = MAXULONG64;
    PVOID dummyPage = MmAllocateContiguousMemory(4096, maxAddr);
    if (!dummyPage) return;
    RtlSecureZeroMemory(dummyPage, 4096);
    ULONG64 dummyPhys = MmGetPhysicalAddress(dummyPage).QuadPart;

    // Use Core 0's NPT as the master template
    PHYSICAL_ADDRESS pml4Target;
    pml4Target.QuadPart = g_GuestStates[0]->NptPml4[0].AsUInt64 & 0xFFFFFFFFFF000;
    PNPT_ENTRY pdpt = (PNPT_ENTRY)MmGetVirtualForPhysical(pml4Target);
    
    for (int i = 0; i < 256; i++) {
        if (!g_GuestStates[i]) continue;
        
        ULONG64 targets[2] = {
            g_GuestStates[i]->VmcbPhysical.QuadPart,
            g_GuestStates[i]->HostStatePhysical.QuadPart
        };
        
        for (int t = 0; t < 2; t++) {
            ULONG64 statePhys = targets[t];
            ULONG64 pdptIdx = (statePhys >> 30) & 0x1FF;
            ULONG64 pdIdx = (statePhys >> 21) & 0x1FF;
            ULONG64 ptIdx = (statePhys >> 12) & 0x1FF;
            
            PHYSICAL_ADDRESS pdTargetPhys;
            pdTargetPhys.QuadPart = pdpt[pdptIdx].AsUInt64 & 0xFFFFFFFFFF000;
            PNPT_ENTRY pdTarget = (PNPT_ENTRY)MmGetVirtualForPhysical(pdTargetPhys);
            
            // Split 2MB PD entry into 4KB PT pages if necessary
            if (pdTarget[pdIdx].Fields.PageSize) {
                PNPT_ENTRY pt = (PNPT_ENTRY)MmAllocateContiguousMemory(4096, maxAddr);
                if (!pt) continue;
                RtlSecureZeroMemory(pt, 4096);
                
                ULONG64 pdBasePhys = (pdptIdx << 30) | (pdIdx << 21);
                for (int p = 0; p < 512; p++) {
                    pt[p].AsUInt64 = pdBasePhys | (p << 12) | 0x87; 
                }
                
                pdTarget[pdIdx].AsUInt64 = MmGetPhysicalAddress(pt).QuadPart | 0x7; 
            }
            
            PHYSICAL_ADDRESS ptTargetPhys;
            ptTargetPhys.QuadPart = pdTarget[pdIdx].AsUInt64 & 0xFFFFFFFFFF000;
            PNPT_ENTRY ptTarget = (PNPT_ENTRY)MmGetVirtualForPhysical(ptTargetPhys);
            
            // Cloak only the exact 4KB hardware page
            ptTarget[ptIdx].AsUInt64 = dummyPhys | 0x87;
        }
    }
}

NTSTATUS AllocateSvmMemory(ULONG processorId) {
    PHYSICAL_ADDRESS maxAddr;
    maxAddr.QuadPart = MAXULONG64;

    PVIRTUAL_MACHINE_STATE state = (PVIRTUAL_MACHINE_STATE)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(VIRTUAL_MACHINE_STATE), 'MVS_');
    if (!state) return STATUS_INSUFFICIENT_RESOURCES;
    
    state->HostStateArea = MmAllocateContiguousMemory(SVM_ALIGNMENT, maxAddr);
    if (!state->HostStateArea) return STATUS_INSUFFICIENT_RESOURCES;
    RtlSecureZeroMemory(state->HostStateArea, SVM_ALIGNMENT);
    state->HostStatePhysical = MmGetPhysicalAddress(state->HostStateArea);
    
    state->Vmcb = (PVMCB)MmAllocateContiguousMemory(SVM_ALIGNMENT, maxAddr);
    if (!state->Vmcb) {
        MmFreeContiguousMemory(state->HostStateArea);
        ExFreePool(state);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlSecureZeroMemory(state->Vmcb, SVM_ALIGNMENT);
    state->VmcbPhysical = MmGetPhysicalAddress(state->Vmcb);
    
    // Memory Optimization: Share the exact same NPT across all 64 cores
    // This reduces NPT memory overhead from 256MB to 2MB total.
    if (processorId == 0) {
        if (!NT_SUCCESS(BuildNestedPageTables(state))) return STATUS_INSUFFICIENT_RESOURCES;
    } else {
        state->NptPml4 = g_GuestStates[0]->NptPml4;
        state->NptPhysical = g_GuestStates[0]->NptPhysical;
    }
    
    g_GuestStates[processorId] = state;
    return STATUS_SUCCESS;
}

void EnterSvmRootMode(PVIRTUAL_MACHINE_STATE state) {
    __writemsr(MSR_VM_HSAVE_PA, state->HostStatePhysical.QuadPart);
    ULONG64 efer = __readmsr(MSR_EFER);
    efer |= EFER_SVME;
    __writemsr(MSR_EFER, efer);
}

void SetupVmcb(PVIRTUAL_MACHINE_STATE state) {
    PVMCB vmcb = state->Vmcb;
    
    vmcb->ControlArea.InterceptMisc1 |= (1 << 18); // Intercept CPUID
    vmcb->ControlArea.InterceptMisc2 |= (1 << 1);  // Intercept VMMCALL
    
    vmcb->ControlArea.NpEnable = 1;
    vmcb->ControlArea.N_CR3 = state->NptPhysical.QuadPart;
    vmcb->ControlArea.GuestASID = 1; 
    
    vmcb->ControlArea.TSC_Offset = 0; 
    
    vmcb->StateSaveArea.Efer = __readmsr(MSR_EFER);
    vmcb->StateSaveArea.Cr0 = __readcr0();
    vmcb->StateSaveArea.Cr3 = __readcr3();
    vmcb->StateSaveArea.Cr4 = __readcr4();
    vmcb->StateSaveArea.Rflags = __readeflags();
    
    vmcb->StateSaveArea.Efer = __readmsr(0xC0000080);
    vmcb->StateSaveArea.G_Pat = __readmsr(0x277);
    vmcb->StateSaveArea.Dr6 = __readdr(6);
    vmcb->StateSaveArea.Dr7 = __readdr(7);
    
    vmcb->StateSaveArea.Fs.Base = __readmsr(0xC0000100);
    vmcb->StateSaveArea.Gs.Base = __readmsr(0xC0000101);
    vmcb->StateSaveArea.KernelGsBase = __readmsr(0xC0000102);
    
    vmcb->StateSaveArea.Star = __readmsr(0xC0000081);
    vmcb->StateSaveArea.Lstar = __readmsr(0xC0000082);
    vmcb->StateSaveArea.Cstar = __readmsr(0xC0000083);
    vmcb->StateSaveArea.Sfmask = __readmsr(0xC0000084);
    
    AsmGetSegmentRegisters(&vmcb->StateSaveArea);
    
    // Parse GDT to get the 64-bit base addresses for TR and LDTR
    ULONG64 gdtBase = vmcb->StateSaveArea.Gdtr.Base;
    
    ULONG16 trSelector = vmcb->StateSaveArea.Tr.Selector;
    if (trSelector) {
        PUCHAR desc = (PUCHAR)(gdtBase + (trSelector & ~7));
        ULONG64 base = (ULONG64)desc[2] | ((ULONG64)desc[3] << 8) | ((ULONG64)desc[4] << 16) | ((ULONG64)desc[7] << 24);
        base |= ((ULONG64)(*(PULONG)(desc + 8)) << 32);
        vmcb->StateSaveArea.Tr.Base = base;
    }
    
    ULONG16 ldtrSelector = vmcb->StateSaveArea.Ldtr.Selector;
    if (ldtrSelector) {
        PUCHAR desc = (PUCHAR)(gdtBase + (ldtrSelector & ~7));
        ULONG64 base = (ULONG64)desc[2] | ((ULONG64)desc[3] << 8) | ((ULONG64)desc[4] << 16) | ((ULONG64)desc[7] << 24);
        base |= ((ULONG64)(*(PULONG)(desc + 8)) << 32);
        vmcb->StateSaveArea.Ldtr.Base = base;
    }
}

// ==========================================
// Bare-Metal Memory Translation Engine
// ==========================================

#define MAGIC_CODE 0x67676767
#define CMD_READ_MEM 1
#define CMD_WRITE_MEM 2
#define CMD_GET_BASE 3

#pragma pack(push, 8)
typedef struct _COMMAND_STRUCT {
    ULONG magic;
    ULONG code;
    ULONG pid;
    ULONG_PTR address;
    ULONG_PTR buffer;
    SIZE_T size;
    ULONG_PTR result_base;
} COMMAND_STRUCT, *PCOMMAND_STRUCT;
#pragma pack(pop)

ULONG64 TranslateLinearAddress(ULONG64 directoryTableBase, ULONG64 virtualAddress) {
    ULONG64 pml4Index = (virtualAddress >> 39) & 0x1FF;
    PHYSICAL_ADDRESS pml4Addr;
    pml4Addr.QuadPart = (directoryTableBase & 0xFFFFFFFFFF000) + (pml4Index * 8);
    PULONG64 pml4Entry = (PULONG64)MmGetVirtualForPhysical(pml4Addr);
    if (!pml4Entry || !(*pml4Entry & 1)) return 0;

    ULONG64 pdptIndex = (virtualAddress >> 30) & 0x1FF;
    PHYSICAL_ADDRESS pdptAddr;
    pdptAddr.QuadPart = (*pml4Entry & 0xFFFFFFFFFF000) + (pdptIndex * 8);
    PULONG64 pdptEntry = (PULONG64)MmGetVirtualForPhysical(pdptAddr);
    if (!pdptEntry || !(*pdptEntry & 1)) return 0;

    if (*pdptEntry & 0x80) return (*pdptEntry & 0xFFFFC00000000) + (virtualAddress & 0x3FFFFFFF);

    ULONG64 pdIndex = (virtualAddress >> 21) & 0x1FF;
    PHYSICAL_ADDRESS pdAddr;
    pdAddr.QuadPart = (*pdptEntry & 0xFFFFFFFFFF000) + (pdIndex * 8);
    PULONG64 pdEntry = (PULONG64)MmGetVirtualForPhysical(pdAddr);
    if (!pdEntry || !(*pdEntry & 1)) return 0;

    if (*pdEntry & 0x80) return (*pdEntry & 0xFFFFFFE00000) + (virtualAddress & 0x1FFFFF);

    ULONG64 ptIndex = (virtualAddress >> 12) & 0x1FF;
    PHYSICAL_ADDRESS ptAddr;
    ptAddr.QuadPart = (*pdEntry & 0xFFFFFFFFFF000) + (ptIndex * 8);
    PULONG64 ptEntry = (PULONG64)MmGetVirtualForPhysical(ptAddr);
    if (!ptEntry || !(*ptEntry & 1)) return 0;

    return (*ptEntry & 0xFFFFFFFFFF000) + (virtualAddress & 0xFFF);
}

ULONG64 GetProcessCr3(ULONG targetPid) {
    PEPROCESS process = NULL;
    if (NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)targetPid, &process))) {
        KAPC_STATE apcState;
        KeStackAttachProcess(process, &apcState);
        ULONG64 cr3 = __readcr3();
        KeUnstackDetachProcess(&apcState);
        ObDereferenceObject(process);
        return cr3;
    }
    return 0;
}

void CopyPhysicalMemory(ULONG64 destCr3, ULONG64 destVa, ULONG64 srcCr3, ULONG64 srcVa, SIZE_T size) {
    SIZE_T bytesRemaining = size;
    ULONG64 currentDestVa = destVa;
    ULONG64 currentSrcVa = srcVa;

    while (bytesRemaining > 0) {
        ULONG64 srcPhys = TranslateLinearAddress(srcCr3, currentSrcVa);
        ULONG64 destPhys = TranslateLinearAddress(destCr3, currentDestVa);
        if (!srcPhys || !destPhys) break;

        PHYSICAL_ADDRESS srcPa, destPa;
        srcPa.QuadPart = srcPhys;
        destPa.QuadPart = destPhys;

        PVOID srcHost = MmGetVirtualForPhysical(srcPa);
        PVOID destHost = MmGetVirtualForPhysical(destPa);
        if (!srcHost || !destHost) break;

        SIZE_T srcOffset = currentSrcVa & 0xFFF;
        SIZE_T destOffset = currentDestVa & 0xFFF;
        SIZE_T maxSrc = 0x1000 - srcOffset;
        SIZE_T maxDest = 0x1000 - destOffset;
        
        SIZE_T copySize = bytesRemaining;
        if (copySize > maxSrc) copySize = maxSrc;
        if (copySize > maxDest) copySize = maxDest;

        memcpy(destHost, srcHost, copySize);

        bytesRemaining -= copySize;
        currentSrcVa += copySize;
        currentDestVa += copySize;
    }
}

// ==========================================
// The VM-Exit Dispatcher
// ==========================================
void VmExitHandler(PGUEST_REGISTERS GuestRegs, PHYSICAL_ADDRESS VmcbPhysical) {
    ULONG64 tscStart = __rdtsc();

    PVIRTUAL_MACHINE_STATE state = NULL;
    for (int i=0; i<256; i++) {
        if (g_GuestStates[i] && g_GuestStates[i]->VmcbPhysical.QuadPart == VmcbPhysical.QuadPart) {
            state = g_GuestStates[i];
            break;
        }
    }
    if (!state) return;

    PVMCB vmcb = state->Vmcb;
    ULONG64 exitCode = vmcb->ControlArea.ExitCode;
    
    if (vmcb->ControlArea.nRIP != 0) {
        vmcb->StateSaveArea.Rip = vmcb->ControlArea.nRIP;
    } else {
        vmcb->StateSaveArea.Rip += 3;
    }
    
    if (exitCode == VMEXIT_VMMCALL) {
        ULONG64 guestCr3 = vmcb->StateSaveArea.Cr3;
        ULONG64 cmdVirtualAddr = GuestRegs->Rcx;
        ULONG64 cmdPhysicalAddr = TranslateLinearAddress(guestCr3, cmdVirtualAddr);
        
        if (cmdPhysicalAddr != 0) {
            PHYSICAL_ADDRESS physAddr;
            physAddr.QuadPart = cmdPhysicalAddr;
            PCOMMAND_STRUCT cmd = (PCOMMAND_STRUCT)MmGetVirtualForPhysical(physAddr);
            
            if (cmd && cmd->magic == MAGIC_CODE) {
                if (cmd->code == CMD_READ_MEM) {
                    ULONG64 targetCr3 = GetProcessCr3(cmd->pid);
                    if (targetCr3 != 0) {
                        CopyPhysicalMemory(guestCr3, cmd->buffer, targetCr3, cmd->address, cmd->size);
                    }
                } else if (cmd->code == CMD_GET_BASE) {
                    PEPROCESS process = NULL;
                    if (NT_SUCCESS(PsLookupProcessByProcessId((HANDLE)(ULONG_PTR)cmd->pid, &process))) {
                        cmd->result_base = (ULONG_PTR)PsGetProcessSectionBaseAddress(process);
                        ObDereferenceObject(process);
                    } else {
                        cmd->result_base = 0;
                    }
                }
            }
        }
    }
    else if (exitCode == VMEXIT_CPUID) {
        int cpuInfo[4];
        __cpuidex(cpuInfo, (int)vmcb->StateSaveArea.Rax, (int)GuestRegs->Rcx);
        
        if (vmcb->StateSaveArea.Rax == 1) {
            cpuInfo[2] &= ~(1 << 31); 
        }
        
        vmcb->StateSaveArea.Rax = cpuInfo[0];
        GuestRegs->Rbx = cpuInfo[1];
        GuestRegs->Rcx = cpuInfo[2];
        GuestRegs->Rdx = cpuInfo[3];
    }
    
    vmcb->ControlArea.Vmcb_Clean = 0; 
    ULONG64 tscEnd = __rdtsc();
    vmcb->ControlArea.TSC_Offset -= (tscEnd - tscStart + 1500); 
}

NTSTATUS DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath) {
    UNREFERENCED_PARAMETER(DriverObject);
    UNREFERENCED_PARAMETER(RegistryPath);
    
    if (!CheckSvmSupport()) return STATUS_NOT_SUPPORTED;
    
    ULONG processorCount = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    if (processorCount > 256) processorCount = 256; 
    
    for (ULONG i = 0; i < processorCount; i++) {
        if (!NT_SUCCESS(AllocateSvmMemory(i))) return STATUS_INSUFFICIENT_RESOURCES;
    }
    
    // Globally cloak all cores in the shared NPT with 4KB precision
    CloakHypervisor();
    
    // Virtualize all CPUs synchronously using explicit Group Affinity
    for (ULONG i = 0; i < processorCount; i++) {
        PROCESSOR_NUMBER procNumber;
        KeGetProcessorNumberFromIndex(i, &procNumber);
        
        GROUP_AFFINITY groupAffinity = {0};
        groupAffinity.Group = procNumber.Group;
        groupAffinity.Mask = (KAFFINITY)(1ULL << procNumber.Number);
        
        GROUP_AFFINITY previousAffinity;
        KeSetSystemGroupAffinityThread(&groupAffinity, &previousAffinity);
        
        PVIRTUAL_MACHINE_STATE state = g_GuestStates[i];
        if (state) {
            EnterSvmRootMode(state);
            SetupVmcb(state);
            
            // IGNITION: Enter the matrix.
            AsmVirtualizeCore(state->VmcbPhysical, &state->Vmcb->StateSaveArea);
        }
        
        KeRevertToUserGroupAffinityThread(&previousAffinity);
    }
    
    return STATUS_SUCCESS; 
}
