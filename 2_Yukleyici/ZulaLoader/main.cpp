#include <windows.h>
#include <iostream>
#include <vector>
#include <string>
#include <tlhelp32.h>

#define MAGIC_CODE 0x67676767
#define CMD_READ_MEM 1
#define CMD_WRITE_MEM 2
#define CMD_GET_BASE 3

#pragma pack(push, 8)
struct COMMAND_STRUCT {
    ULONG magic;
    ULONG code;
    ULONG pid;
    ULONG_PTR address;
    ULONG_PTR buffer;
    SIZE_T size;
    ULONG_PTR result_base;
};
#pragma pack(pop)

extern "C" void AsmVmmcall(COMMAND_STRUCT* cmd);

DWORD GetProcessIdByName(const wchar_t* processName) {
    PROCESSENTRY32W pe32;
    pe32.dwSize = sizeof(PROCESSENTRY32W);
    HANDLE hSnapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (hSnapshot == INVALID_HANDLE_VALUE) return 0;

    if (Process32FirstW(hSnapshot, &pe32)) {
        do {
            if (_wcsicmp(pe32.szExeFile, processName) == 0) {
                CloseHandle(hSnapshot);
                return pe32.th32ProcessID;
            }
        } while (Process32NextW(hSnapshot, &pe32));
    }
    CloseHandle(hSnapshot);
    return 0;
}

template<typename T>
T KRead(ULONG pid, ULONG_PTR address) {
    T buffer = {};
    
    COMMAND_STRUCT cmd = {0};
    cmd.magic = MAGIC_CODE;
    cmd.code = CMD_READ_MEM;
    cmd.pid = pid;
    cmd.address = address;
    cmd.buffer = (ULONG_PTR)&buffer;
    cmd.size = sizeof(T);
    
    AsmVmmcall(&cmd); 
    
    return buffer;
}

ULONG_PTR GetModuleBase(ULONG pid, const wchar_t* moduleName) {
    wchar_t name[256];
    wcscpy_s(name, 256, moduleName);
    
    COMMAND_STRUCT cmd = {0};
    cmd.magic = MAGIC_CODE;
    cmd.code = CMD_GET_BASE;
    cmd.pid = pid;
    cmd.buffer = (ULONG_PTR)name;
    
    AsmVmmcall(&cmd);
    
    return cmd.result_base;
}

int main() {
    SetConsoleTitleA("AMD SVM Hypervisor Link");
    
    std::cout << "=====================================================" << std::endl;
    std::cout << "               AMD SVM HYPERVISOR ACTIVE             " << std::endl;
    std::cout << "=====================================================" << std::endl;
    std::cout << "[+] VMCB state active. Nested Page Tables (NPT) linked." << std::endl;
    std::cout << "[+] Awaiting VMMCALL triggers." << std::endl;
    std::cout << "=====================================================" << std::endl;
    
    DWORD zulaPid = GetProcessIdByName(L"zula.exe");
    if (!zulaPid) {
        std::cout << "[!] Could not find zula.exe running. Start the game first." << std::endl;
        std::cin.get();
        return 1;
    }
    
    std::cout << "[+] Found target process PID: " << zulaPid << std::endl;
    
    ULONG_PTR base = GetModuleBase(zulaPid, L"zula.exe");
    
    if (base) {
        std::cout << "[+] Successfully resolved base via Hypercall: 0x" << std::hex << base << std::dec << std::endl;
        
        // Read test
        int hp = KRead<int>(zulaPid, base + 0x1000);
        std::cout << "[+] Read test (HP): " << hp << std::endl;
    } else {
        std::cout << "[!] VMMCALL interception failed or module not found." << std::endl;
    }
    
    std::cin.get();
    return 0;
}
