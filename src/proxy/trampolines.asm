.data
extern fp:qword  ; FARPROC fp[17] from proxy.cpp

.code

proxy_GetFileVersionInfoA proc
    jmp qword ptr [fp + 0*8]
proxy_GetFileVersionInfoA endp

proxy_GetFileVersionInfoByHandle proc
    jmp qword ptr [fp + 1*8]
proxy_GetFileVersionInfoByHandle endp

proxy_GetFileVersionInfoExA proc
    jmp qword ptr [fp + 2*8]
proxy_GetFileVersionInfoExA endp

proxy_GetFileVersionInfoExW proc
    jmp qword ptr [fp + 3*8]
proxy_GetFileVersionInfoExW endp

proxy_GetFileVersionInfoSizeA proc
    jmp qword ptr [fp + 4*8]
proxy_GetFileVersionInfoSizeA endp

proxy_GetFileVersionInfoSizeExA proc
    jmp qword ptr [fp + 5*8]
proxy_GetFileVersionInfoSizeExA endp

proxy_GetFileVersionInfoSizeExW proc
    jmp qword ptr [fp + 6*8]
proxy_GetFileVersionInfoSizeExW endp

proxy_GetFileVersionInfoSizeW proc
    jmp qword ptr [fp + 7*8]
proxy_GetFileVersionInfoSizeW endp

proxy_GetFileVersionInfoW proc
    jmp qword ptr [fp + 8*8]
proxy_GetFileVersionInfoW endp

proxy_VerFindFileA proc
    jmp qword ptr [fp + 9*8]
proxy_VerFindFileA endp

proxy_VerFindFileW proc
    jmp qword ptr [fp + 10*8]
proxy_VerFindFileW endp

proxy_VerInstallFileA proc
    jmp qword ptr [fp + 11*8]
proxy_VerInstallFileA endp

proxy_VerInstallFileW proc
    jmp qword ptr [fp + 12*8]
proxy_VerInstallFileW endp

proxy_VerLanguageNameA proc
    jmp qword ptr [fp + 13*8]
proxy_VerLanguageNameA endp

proxy_VerLanguageNameW proc
    jmp qword ptr [fp + 14*8]
proxy_VerLanguageNameW endp

proxy_VerQueryValueA proc
    jmp qword ptr [fp + 15*8]
proxy_VerQueryValueA endp

proxy_VerQueryValueW proc
    jmp qword ptr [fp + 16*8]
proxy_VerQueryValueW endp

end
