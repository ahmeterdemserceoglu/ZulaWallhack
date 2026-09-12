extern VmExitHandler : proc

.code

; void __stdcall AsmGetSegmentRegisters(PVOID State);
AsmGetSegmentRegisters PROC
    ; ES
    mov ax, es
    mov word ptr [rcx + 00h], ax  
    xor r8d, r8d
    lsl r8d, eax
    mov dword ptr [rcx + 00h + 4], r8d
    xor r8d, r8d
    lar r8d, eax
    shr r8d, 8
    mov word ptr [rcx + 00h + 2], r8w

    ; CS
    mov ax, cs
    mov word ptr [rcx + 10h], ax  
    xor r8d, r8d
    lsl r8d, eax
    mov dword ptr [rcx + 10h + 4], r8d
    xor r8d, r8d
    lar r8d, eax
    shr r8d, 8
    mov word ptr [rcx + 10h + 2], r8w

    ; SS
    mov ax, ss
    mov word ptr [rcx + 20h], ax  
    xor r8d, r8d
    lsl r8d, eax
    mov dword ptr [rcx + 20h + 4], r8d
    xor r8d, r8d
    lar r8d, eax
    shr r8d, 8
    mov word ptr [rcx + 20h + 2], r8w

    ; DS
    mov ax, ds
    mov word ptr [rcx + 30h], ax  
    xor r8d, r8d
    lsl r8d, eax
    mov dword ptr [rcx + 30h + 4], r8d
    xor r8d, r8d
    lar r8d, eax
    shr r8d, 8
    mov word ptr [rcx + 30h + 2], r8w

    ; FS
    mov ax, fs
    mov word ptr [rcx + 40h], ax  
    xor r8d, r8d
    lsl r8d, eax
    mov dword ptr [rcx + 40h + 4], r8d
    xor r8d, r8d
    lar r8d, eax
    shr r8d, 8
    mov word ptr [rcx + 40h + 2], r8w

    ; GS
    mov ax, gs
    mov word ptr [rcx + 50h], ax  
    xor r8d, r8d
    lsl r8d, eax
    mov dword ptr [rcx + 50h + 4], r8d
    xor r8d, r8d
    lar r8d, eax
    shr r8d, 8
    mov word ptr [rcx + 50h + 2], r8w

    ; TR
    str ax
    mov word ptr [rcx + 90h], ax
    xor r8d, r8d
    lsl r8d, eax
    mov dword ptr [rcx + 90h + 4], r8d
    xor r8d, r8d
    lar r8d, eax
    shr r8d, 8
    mov word ptr [rcx + 90h + 2], r8w

    ; LDTR
    sldt ax
    mov word ptr [rcx + 70h], ax
    xor r8d, r8d
    lsl r8d, eax
    mov dword ptr [rcx + 70h + 4], r8d
    xor r8d, r8d
    lar r8d, eax
    shr r8d, 8
    mov word ptr [rcx + 70h + 2], r8w

    ; GDTR and IDTR require careful 10-byte extraction (2 byte limit, 8 byte base)
    sub rsp, 10h
    
    sgdt [rsp]
    movzx eax, word ptr [rsp]
    mov dword ptr [rcx + 60h + 4], eax    ; Gdtr Limit
    mov r8, [rsp + 2]
    mov qword ptr [rcx + 60h + 8], r8     ; Gdtr Base
    
    sidt [rsp]
    movzx eax, word ptr [rsp]
    mov dword ptr [rcx + 80h + 4], eax    ; Idtr Limit
    mov r8, [rsp + 2]
    mov qword ptr [rcx + 80h + 8], r8     ; Idtr Base
    
    add rsp, 10h
    ret
AsmGetSegmentRegisters ENDP

; void __stdcall AsmVirtualizeCore(PHYSICAL_ADDRESS VmcbPhysical, PVOID StateSaveArea);
AsmVirtualizeCore PROC
    ; RCX = VmcbPhysical
    ; RDX = StateSaveArea
    
    ; 1. Save Guest Context on the stack so the Guest can pop it when it resumes
    pushfq
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rdi
    push rsi
    push rbp
    push rdx
    push rcx
    push rbx
    push rax

    ; 2. Configure Guest RIP and RSP in the VMCB StateSaveArea
    lea rax, GuestResume
    mov [rdx + 178h], rax  ; StateSaveArea.Rip
    mov [rdx + 1D8h], rsp  ; StateSaveArea.Rsp

    ; 3. Transition to Host Context
    ; Save VmcbPhysical onto the Host Stack so the Guest cannot overwrite it
    push rcx

HostLoop:
    ; Load VmcbPhysical from the Host Stack (RSP points exactly here before VMRUN)
    mov rax, [rsp]
    
    ; 4. World Switch! Guest starts executing at GuestResume.
    vmrun 
    
    ; --- VM-EXIT HAPPENS HERE ---
    
    push rbx
    push rcx
    push rdx
    push rbp
    push rsi
    push rdi
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15
    
    ; Pass GuestRegs (RSP) as RCX
    mov rcx, rsp
    
    ; RDX is the 2nd argument (VmcbPhysical).
    ; We pushed 14 registers (112 bytes). The VmcbPhysical is at RSP + 112.
    mov rdx, [rsp + 112]
    
    sub rsp, 20h
    
    call VmExitHandler
    
    add rsp, 20h
    
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rdi
    pop rsi
    pop rbp
    pop rdx
    pop rcx
    pop rbx
    
    jmp HostLoop

GuestResume:
    ; 5. The OS resumes here inside the Virtual Machine!
    ; We are now Ring-0 executing as a Guest.
    pop rax
    pop rbx
    pop rcx
    pop rdx
    pop rbp
    pop rsi
    pop rdi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15
    popfq
    ret
AsmVirtualizeCore ENDP

END
